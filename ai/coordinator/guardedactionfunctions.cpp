#include "ai/coordinator/guardedactiondeclarations.h"
#include "ai/coordinator/guardedcommandfunctions_p.h"
#include "ai/coordinator/guardednativecode.h"

#include <QJSEngine>
#include <QPoint>
#include <QThread>
#include <algorithm>
#include <cstring>
#include <limits>

#ifdef SCRIPTSOURCESUPPORT
#include <private/qqmlmetatype_p.h>
#include <private/qqmlvaluetype_p.h>
#include <private/qv4string_p.h>
#include <private/qv4stringobject_p.h>
#endif

namespace Coordinator
{
    namespace
    {
        enum class CallbackKind { Action, Context, BuildingSprites, BuildingWeather, Ranking };

        struct Callback
        {
            GuardedNativeCodeKind module;
            std::string_view global, member;
            GuardedNativeForwarder forwarder;
            CallbackKind kind;
        };
        constexpr std::array CALLBACKS{
            Callback{GuardedNativeCodeKind::Wait, "ACTION_WAIT", "perform", GuardedNativeForwarder::CommandWait, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::Capture, "ACTION_CAPTURE", "canBePerformed", GuardedNativeForwarder::CommandCaptureCan, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::Capture, "ACTION_CAPTURE", "perform", GuardedNativeForwarder::CommandCapture, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::Capture, "ACTION_CAPTURE", "performPostAnimation", GuardedNativeForwarder::CommandCapturePost, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::NextPlayer, "ACTION_NEXT_PLAYER", "canBePerformed", GuardedNativeForwarder::CommandNextCan, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::NextPlayer, "ACTION_NEXT_PLAYER", "perform", GuardedNativeForwarder::CommandNext, CallbackKind::Action},
            Callback{GuardedNativeCodeKind::Player, "PLAYER", "startOfTurn", GuardedNativeForwarder::CommandPlayerStart, CallbackKind::Context},
            Callback{GuardedNativeCodeKind::Player, "PLAYER", "endOfTurn", GuardedNativeForwarder::CommandPlayerEnd, CallbackKind::Context},
            Callback{GuardedNativeCodeKind::Global, "Global", "getArmyNameFromPlayerTable", GuardedNativeForwarder::CommandArmy, CallbackKind::Context},
            Callback{GuardedNativeCodeKind::Town, "TOWN", "loadSprites", GuardedNativeForwarder::CommandTownSprites, CallbackKind::BuildingSprites},
            Callback{GuardedNativeCodeKind::Town, "TOWN", "onWeatherChanged", GuardedNativeForwarder::CommandTownWeather, CallbackKind::BuildingWeather},
            Callback{GuardedNativeCodeKind::Factory, "FACTORY", "loadSprites", GuardedNativeForwarder::CommandFactorySprites, CallbackKind::BuildingSprites},
            Callback{GuardedNativeCodeKind::Factory, "FACTORY", "onWeatherChanged", GuardedNativeForwarder::CommandFactoryWeather, CallbackKind::BuildingWeather},
            Callback{GuardedNativeCodeKind::Headquarters, "HQ", "loadSprites", GuardedNativeForwarder::CommandHeadquartersSprites, CallbackKind::BuildingSprites},
            Callback{GuardedNativeCodeKind::Headquarters, "HQ", "onWeatherChanged", GuardedNativeForwarder::CommandHeadquartersWeather, CallbackKind::BuildingWeather},
            Callback{GuardedNativeCodeKind::Ranking, "UNITRANKINGSYSTEM", "unloadIcons", GuardedNativeForwarder::CommandRanking, CallbackKind::Ranking}};
        static_assert(CALLBACKS.size() <= std::numeric_limits<std::uint16_t>::digits);
        struct RankingBonusDefinition
        {
            GuardedCommandRankingBonus kind;
            const ActionDeclaration::Function & delegate;
            const ActionDeclaration::Function & helper;
        };
        constexpr std::array RANKING_BONUSES{
            RankingBonusDefinition{GuardedCommandRankingBonus::Offensive,
                ActionDeclaration::LEGACY_RANK_OFFENSIVE_DELEGATE, ActionDeclaration::LEGACY_RANK_GET_OFFENSIVE_BONUS},
            RankingBonusDefinition{GuardedCommandRankingBonus::Defensive,
                ActionDeclaration::LEGACY_RANK_DEFENSIVE_DELEGATE, ActionDeclaration::LEGACY_RANK_GET_DEFENSIVE_BONUS}};
        constexpr std::int64_t CALLBACK_BINDING_WORK = 131072;

#ifdef SCRIPTSOURCESUPPORT
        using namespace GuardedCommandDetail;
        using ActionFunction = ActionDeclaration::Function;
        constexpr auto FOUND = GuardedScriptPropertyStatus::Found;
        constexpr std::int64_t FORMAL_NAMES_CONTROL_WORK = 256;
        const std::array GLOBALS{
            QStringLiteral("CO_COMMAND"), QStringLiteral("Global"), QStringLiteral("undefined"),
            QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_CAPTURE"), QStringLiteral("ACTION_NEXT_PLAYER"),
            QStringLiteral("PLAYER"), QStringLiteral("CMODE_DECKBUILDER"), QStringLiteral("CMODE_PROCGEN"),
            QStringLiteral("Array"), QStringLiteral("getArmyNameFromPlayerTable"), QStringLiteral("CMODE_CO_COMMAND"),
            QStringLiteral("TOWN"), QStringLiteral("FACTORY"), QStringLiteral("HQ"),
            QStringLiteral("UNITRANKINGSYSTEM"), QStringLiteral("String"), QStringLiteral("LEGACY_COMMAND_UNIT_RANKS")};

        bool literalText(const QV4::Value & value, QLatin1StringView expected)
        {
            const auto* text = value.as<QV4::String>();
            return text && text->d()->subtype < QV4::Heap::String::StringType_Complex &&
                text->d()->text().size == expected.size() && text->toQString() == expected;
        }

        bool functionNames(const QV4::JavaScriptFunctionObject & script, const ActionFunction & expected,
                           const LinkedUnit & linked, RefinementLedger & ledger)
        {
            const auto* function = script.function()->compiledFunction;
            if (function->nFormals != expected.formals.size() ||
                !ledger.consume(FORMAL_NAMES_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                return false;
            }
            for (std::size_t i = 0; i < expected.formals.size(); ++i)
            {
                if (!storedString(linked, function->formalsTable()[i].nameIndex, expected.formals[i], ledger))
                {
                    return false;
                }
            }
            return true;
        }

        bool functionRecord(QJSEngine & engine, const QV4::Value & value, const ActionFunction & expected,
                            const LinkedUnit*& linked, RefinementLedger & ledger)
        {
            const auto* script = value.as<QV4::JavaScriptFunctionObject>();
            return script && std::ranges::any_of(expected.records, [&](const auto & record) {
                return matchRecord(engine, script, record, linked, ledger, expected.locals);
            }) &&
                functionAt(*linked, script->function(), expected.index) &&
                functionNames(*script, expected, *linked, ledger);
        }

        bool moduleOperands(QJSEngine & engine, const LinkedUnit* linked, const ActionDeclaration::Module & module,
                            RefinementLedger & ledger)
        {
            if (!moduleStrings(linked, module.strings, latin(module.path), ledger) ||
                !ledger.consume(128 * (module.constants.size() + 1), RefinementWork::MasterPreparation))
            {
                return false;
            }
            for (const auto & constant : module.constants)
            {
                if (!storedConstant(*linked, constant.index, constant.encoded)) { return false; }
            }
            for (const auto & shape : module.shapes)
            {
                if (!literalShape(*linked, *engine.handle(), shape.index, shape.members, ledger)) { return false; }
            }
            for (const auto & block : module.catches)
            {
                if (!catchBlock(*linked, *engine.handle(), block.index, block.name, ledger)) { return false; }
            }
            return true;
        }

        bool ordinaryMethod(QJSEngine & engine, const QV4::Value & value, QLatin1StringView method,
                            RefinementLedger & ledger)
        {
            const auto* script = value.as<QV4::JavaScriptFunctionObject>();
            return script && script->engine() == engine.handle() &&
                script->d()->prototype() == engine.handle()->functionPrototype()->d() &&
                guardedScriptBuiltinProperty(GuardedCommandFunctionAccess::managed(value), QString(method), ledger).status ==
                    GuardedScriptPropertyStatus::Missing;
        }

        class ActionBindings final
        {
        public:
            ActionBindings(QJSEngine & engine, RefinementLedger & ledger,
                           const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members)
                : m_engine(engine), m_ledger(ledger), m_globals(globals), m_members(members)
            {
                if (!m_globals)
                {
                    m_ownedGlobals.emplace(prepareGuardedCommandCallbackGlobals(engine, ledger));
                    m_globals = &*m_ownedGlobals;
                }
                if (!m_members)
                {
                    m_ownedMembers.emplace(engine, ledger);
                    m_members = &*m_ownedMembers;
                }
            }

            GuardedScriptProperty global(std::string_view name)
            {
                const auto result = m_globals->find(m_engine, QString(latin(name)), m_ledger);
                return result.prototypeDepth == 0 ? result : GuardedScriptProperty{};
            }

            GuardedScriptProperty member(const GuardedScriptValueView & object, std::string_view name)
            {
                const auto raw = GuardedCommandFunctionAccess::read(object);
                const auto* holder = raw.as<QV4::Object>();
                if (holder && holder->d() == m_engine.handle()->globalObject->d())
                {
                    return global(name);
                }
                return own(object, QString(latin(name)), *m_members, m_ledger);
            }

            bool helper(const GuardedScriptValueView & object, const ActionFunction & function, const LinkedUnit*& linked)
            {
                const auto actual = member(object, function.records.front().name);
                if (actual.status != FOUND ||
                    guardedScriptContextOperands(actual.value, m_ledger).status != GuardedScriptContextOperands::GlobalScopeOnly)
                {
                    return false;
                }
                return functionRecord(m_engine, GuardedCommandFunctionAccess::read(actual.value), function, linked, m_ledger);
            }

            bool text(const GuardedScriptValueView & object, std::string_view name, QLatin1StringView expected)
            {
                const auto actual = member(object, name);
                const auto scalar = guardedScriptScalar(actual.value, m_ledger);
                const auto* value = std::get_if<QString>(&scalar.value);
                return actual.status == FOUND && scalar.status == GuardedScriptScalarStatus::Verified && value && *value == expected;
            }

            bool writableNull(const GuardedScriptValueView & object, QLatin1StringView name)
            {
                const auto raw = GuardedCommandFunctionAccess::read(object);
                const auto* holder = raw.as<QV4::Object>();
                QV4::PropertyAttributes attributes;
                QV4::Value value;
                return holder && holder->engine() == m_engine.handle() && std::strcmp(holder->vtable()->className, "Object") == 0 &&
                    descriptorSlot(*holder, name, attributes, value, m_ledger) &&
                    attributes.isData() && attributes.isWritable() && value.isNull();
            }

            bool originMarker(const GuardedScriptValueView & object)
            {
                const auto raw = GuardedCommandFunctionAccess::read(object);
                const auto* holder = raw.as<QV4::Object>();
                QV4::PropertyAttributes attributes;
                QV4::Value value;
                if (!holder || holder->engine() != m_engine.handle() || std::strcmp(holder->vtable()->className, "Object") != 0 ||
                    !descriptorSlot(*holder, QLatin1StringView("__cmode_co_command_origin"), attributes, value, m_ledger) ||
                    !attributes.isData() || !attributes.isWritable())
                {
                    return false;
                }
                if (value.isNull()) { return true; }
                const auto* origin = value.as<QV4::Object>();
                if (!origin || origin->engine() != m_engine.handle() || std::strcmp(origin->vtable()->className, "Object") != 0 ||
                    origin->internalClass()->size != 2 || origin->arrayData() ||
                    origin->d()->prototype() != m_engine.handle()->objectPrototype()->d())
                {
                    return false;
                }
                const auto view = GuardedCommandFunctionAccess::managed(value);
                const auto owner = member(view, "ownerId");
                const auto lineage = member(view, "lineage");
                return owner.status == FOUND && (owner.value.number() == 0 || owner.value.number() == 1) &&
                    lineage.status == FOUND && lineage.value.number() == 0;
            }

        private:
            QJSEngine & m_engine;
            RefinementLedger & m_ledger;
            std::optional<GuardedScriptGlobalIndex> m_ownedGlobals;
            std::optional<GuardedScriptOwnIndexCache> m_ownedMembers;
            const GuardedScriptGlobalIndex* m_globals;
            GuardedScriptOwnIndexCache* m_members;
        };

        const QV4::Heap::CallContext* frame(QJSEngine & engine, const QV4::Value & value,
                                          const ActionFunction & wrapper, const ActionFunction & creator,
                                          const LinkedUnit*& linked, RefinementLedger & ledger)
        {
            if (!functionRecord(engine, value, wrapper, linked, ledger)) { return nullptr; }
            const auto* script = value.as<QV4::JavaScriptFunctionObject>();
            const auto* context = closureContext(engine, *script, creator.records, creator.index,
                                                 creator.locals, creator.formals.size(), linked, ledger);
            if (!context) { return nullptr; }
            const auto making = QV4::Value::fromHeapObject(context->function.get());
            const auto* function = making.as<QV4::JavaScriptFunctionObject>();
            return functionNames(*function, creator, *linked, ledger) ? context : nullptr;
        }

        bool savedDelegate(const QV4::Heap::CallContext & context, std::uint32_t local, QV4::Value & result)
        {
            if (local >= context.locals.size) { return false; }
            result = context.locals.values[local];
            return true;
        }

        bool objectArgument(const QV4::Heap::CallContext & context, std::uint32_t position, const GuardedScriptValueView & expected)
        {
            return position < context.nArgs &&
                context.locals.values[context.locals.size + position].asReturnedValue() ==
                    GuardedCommandFunctionAccess::read(expected).asReturnedValue();
        }

        bool textArgument(const QV4::Heap::CallContext & context, std::uint32_t position, std::string_view expected)
        {
            return position < context.nArgs &&
                literalText(context.locals.values[context.locals.size + position], latin(expected));
        }

        bool buildingDelegate(QJSEngine & engine, const Callback & callback, const GuardedScriptValueView & object,
                              const QV4::Value & actual, QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const bool weather = callback.kind == CallbackKind::BuildingWeather;
            const auto & wrapper = weather ? COMMAND_BUILDING_WEATHER_WRAPPER : COMMAND_BUILDING_LOAD_WRAPPER;
            const LinkedUnit* linked = nullptr;
            const auto* context = frame(engine, actual, wrapper, COMMAND_BUILDING_CREATOR, linked, ledger);
            return context && objectArgument(*context, 0, object) && textArgument(*context, 1, callback.global) &&
                savedDelegate(*context, weather ? 1u : 0u, result) &&
                ordinaryMethod(engine, result, QLatin1StringView("apply"), ledger) &&
                moduleOperands(engine, linked, COMMAND, ledger) &&
                moduleOperands(engine, linked, COMMAND_BUILDING_EXTRA, ledger);
        }

        bool rankingBonusDefinition(
            QJSEngine & engine, ActionBindings & bindings, const GuardedScriptValueView & value,
            const RankingBonusDefinition & definition, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const LinkedUnit *legacy = nullptr, *command = nullptr;
            if (guardedScriptContextOperands(value, ledger).status != GuardedScriptContextOperands::GlobalScopeOnly ||
                !functionRecord(engine, GuardedCommandFunctionAccess::read(value), definition.delegate, legacy, ledger))
            {
                return false;
            }
            const auto object = bindings.global("LEGACY_COMMAND_UNIT_RANKS");
            if (object.status != FOUND || !bindings.helper(object.value, definition.helper, legacy)) { return false; }
            const auto eligible = bindings.member(object.value, "isEligibleUnit");
            if (eligible.status != FOUND) { return false; }
            const auto* context = frame(engine, GuardedCommandFunctionAccess::read(eligible.value),
                COMMAND_RANK_ELIGIBILITY_WRAPPER, COMMAND_RANK_ELIGIBILITY_CREATOR, command, ledger);
            if (!context) { return false; }
            QV4::Value receiver, original;
            if (!savedDelegate(*context, 0, receiver) ||
                receiver.asReturnedValue() != GuardedCommandFunctionAccess::read(object.value).asReturnedValue() ||
                !savedDelegate(*context, 1, original) ||
                !ordinaryMethod(engine, original, QLatin1StringView("apply"), ledger) ||
                guardedScriptContextOperands(GuardedCommandFunctionAccess::managed(original), ledger).status !=
                    GuardedScriptContextOperands::GlobalScopeOnly ||
                !functionRecord(engine, original, LEGACY_RANK_IS_ELIGIBLE_UNIT, legacy, ledger))
            {
                return false;
            }
            const auto maker = QV4::Value::fromHeapObject(context->function.get());
            const auto* creator = maker.as<QV4::JavaScriptFunctionObject>()->function()->compiledFunction;
            for (std::size_t index = 0; index < COMMAND_RANK_ELIGIBILITY_CREATOR_LOCALS.size(); ++index)
            {
                if (!storedString(*command, creator->localsTable()[index],
                                  COMMAND_RANK_ELIGIBILITY_CREATOR_LOCALS[index], ledger))
                {
                    return false;
                }
            }
            return moduleOperands(engine, legacy, LEGACY_RANK, ledger) &&
                moduleOperands(engine, command, COMMAND, ledger) &&
                moduleOperands(engine, command, COMMAND_RANK_BONUS, ledger);
        }

        bool rankingDelegate(QJSEngine & engine, const QV4::Value & actual, QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const LinkedUnit* linked = nullptr;
            const auto* context = frame(engine, actual, COMMAND_RANKING_WRAPPER, COMMAND_RANKING_CREATOR, linked, ledger);
            return context && savedDelegate(*context, 0, result) &&
                ordinaryMethod(engine, result, QLatin1StringView("apply"), ledger) &&
                moduleOperands(engine, linked, COMMAND, ledger) &&
                moduleOperands(engine, linked, COMMAND_RANKING_EXTRA, ledger);
        }

        bool spriteDelegate(QJSEngine & engine, ActionBindings & bindings, const Callback & callback,
                            const GuardedScriptValueView & object, const QV4::Value & value,
                            const LinkedUnit*& linked, QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const auto* context = frame(engine, value, COMMAND_SPRITE_WRAPPER, COMMAND_SPRITE_CREATOR, linked, ledger);
            const auto command = bindings.global("CO_COMMAND");
            const auto resolver = bindings.member(command.value,
                callback.forwarder == GuardedNativeForwarder::CommandCapturePost ? "postAnimationUnitContext" : "actionTargetContext");
            return context && command.status == FOUND && resolver.status == FOUND &&
                objectArgument(*context, 0, object) && textArgument(*context, 1, callback.global) &&
                textArgument(*context, 2, callback.member) && objectArgument(*context, 3, resolver.value) &&
                savedDelegate(*context, 0, result) && ordinaryMethod(engine, result, QLatin1StringView("apply"), ledger);
        }

        bool commandDelegate(QJSEngine & engine, const Callback & callback, const GuardedScriptValueView & object,
                             const QV4::Value & value, const LinkedUnit*& linked, QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const bool post = callback.forwarder == GuardedNativeForwarder::CommandCapturePost;
            const bool nextCan = callback.forwarder == GuardedNativeForwarder::CommandNextCan;
            const auto & wrapper = post ? COMMAND_CALLBACK_WRAPPER : nextCan ? COMMAND_NEXT_CAN_WRAPPER : COMMAND_ACTION_WRAPPER;
            const auto & creator = post ? COMMAND_CALLBACK_CREATOR : nextCan ? COMMAND_NEXT_CAN_CREATOR : COMMAND_ACTION_CREATOR;
            const auto* context = frame(engine, value, wrapper, creator, linked, ledger);
            return context && objectArgument(*context, 0, object) && textArgument(*context, 1, callback.global) &&
                (!post || textArgument(*context, 2, callback.member)) &&
                savedDelegate(*context, 0, result) && ordinaryMethod(engine, result, QLatin1StringView("apply"), ledger);
        }

        bool deckDelegate(QJSEngine & engine, const QV4::Value & value, const ActionFunction & wrapper,
                          const ActionFunction & creator, std::uint32_t local, const LinkedUnit*& linked,
                          QV4::Value & result, RefinementLedger & ledger)
        {
            const auto* context = frame(engine, value, wrapper, creator, linked, ledger);
            return context && savedDelegate(*context, local, result) &&
                (result.isNull() || ordinaryMethod(engine, result, QLatin1StringView("call"), ledger));
        }

        bool actionDelegate(QJSEngine & engine, ActionBindings & bindings, const Callback & callback,
                            const GuardedScriptValueView & object, const QV4::Value & actual,
                            QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const LinkedUnit *command = nullptr, *deck = nullptr, *chess = nullptr, *prism = nullptr;
            QV4::Value inner, next;
            if (callback.forwarder == GuardedNativeForwarder::CommandCaptureCan)
            {
                if (!deckDelegate(engine, actual, DECK_CAPTURE_WRAPPER, DECK_CAPTURE_CREATOR, 0, deck, inner, ledger) ||
                    !moduleOperands(engine, deck, DECK, ledger) ||
                    !functionRecord(engine, inner, PRISM_CAPTURE_WRAPPER, prism, ledger) ||
                    guardedScriptContextOperands(GuardedCommandFunctionAccess::managed(inner), ledger).status !=
                        GuardedScriptContextOperands::GlobalScopeOnly ||
                    !moduleOperands(engine, prism, PRISM, ledger))
                {
                    return false;
                }
                const auto leaf = bindings.member(object, "newCanBePerformed");
                if (leaf.status != FOUND) { return false; }
                result = GuardedCommandFunctionAccess::read(leaf.value);
                return result.as<QV4::JavaScriptFunctionObject>();
            }
            if (callback.forwarder == GuardedNativeForwarder::CommandNextCan)
            {
                if (!commandDelegate(engine, callback, object, actual, command, inner, ledger) ||
                    !deckDelegate(engine, inner, DECK_NEXT_CAN_WRAPPER, DECK_NEXT_CREATOR, 0, deck, result, ledger) ||
                    !moduleOperands(engine, deck, DECK, ledger))
                {
                    return false;
                }
            }
            else
            {
                if (!spriteDelegate(engine, bindings, callback, object, actual, command, inner, ledger) ||
                    !commandDelegate(engine, callback, object, inner, command, result, ledger))
                {
                    return false;
                }
                if (callback.forwarder == GuardedNativeForwarder::CommandNext)
                {
                    if (!deckDelegate(engine, result, DECK_NEXT_PERFORM_WRAPPER, DECK_NEXT_CREATOR, 1, deck, inner, ledger) ||
                        !moduleOperands(engine, deck, DECK, ledger))
                    {
                        return false;
                    }
                    const auto* context = frame(engine, inner, CHESS_NEXT_WRAPPER, CHESS_NEXT_CREATOR, chess, ledger);
                    if (!context || !savedDelegate(*context, 1, result) ||
                        !ordinaryMethod(engine, result, QLatin1StringView("call"), ledger) ||
                        !moduleOperands(engine, chess, CHESS, ledger))
                    {
                        return false;
                    }
                }
            }
            return moduleOperands(engine, command, COMMAND, ledger);
        }

        bool contextDelegate(QJSEngine & engine, const Callback & callback, const QV4::Value & actual,
                             QV4::Value & result, RefinementLedger & ledger)
        {
            using namespace ActionDeclaration;
            const LinkedUnit *command = nullptr, *frontier = nullptr, *deck = nullptr;
            if (callback.forwarder == GuardedNativeForwarder::CommandArmy)
            {
                const auto* context = frame(engine, actual, COMMAND_ARMY_WRAPPER, COMMAND_ARMY_CREATOR, command, ledger);
                return context && savedDelegate(*context, 0, result) &&
                    ordinaryMethod(engine, result, QLatin1StringView("apply"), ledger) &&
                    moduleOperands(engine, command, COMMAND, ledger);
            }
            QV4::Value inner = actual;
            if (callback.forwarder == GuardedNativeForwarder::CommandPlayerStart)
            {
                const auto* context = frame(engine, actual, FRONTIER_START_WRAPPER, FRONTIER_START_CREATOR, frontier, ledger);
                if (!context || !savedDelegate(*context, 0, inner) ||
                    !ordinaryMethod(engine, inner, QLatin1StringView("call"), ledger) ||
                    !moduleOperands(engine, frontier, FRONTIER, ledger))
                {
                    return false;
                }
                return deckDelegate(engine, inner, DECK_START_WRAPPER, DECK_START_CREATOR, 0, deck, result, ledger) &&
                    result.isNull() && moduleOperands(engine, deck, DECK, ledger);
            }
            return callback.forwarder == GuardedNativeForwarder::CommandPlayerEnd &&
                deckDelegate(engine, inner, DECK_END_WRAPPER, DECK_END_CREATOR, 0, deck, result, ledger) &&
                result.isNull() && moduleOperands(engine, deck, DECK, ledger);
        }

        bool sameBuiltin(QJSEngine & engine, QJSEngine & stock, const QV4::Object & actual, const QV4::Object & expected,
                         QLatin1StringView name, RefinementLedger & ledger)
        {
            const auto observed = builtinMember(engine, actual, name, false, ledger);
            const auto approved = builtinMember(stock, expected, name, false, ledger);
            return observed.kind == GuardedScriptFunctionKind::Builtin && approved.kind == GuardedScriptFunctionKind::Builtin &&
                observed.sameImplementation(approved);
        }

        bool actionIntrinsics(QJSEngine & engine, QJSEngine & stock, ActionBindings & bindings, RefinementLedger & ledger)
        {
            auto* actual = engine.handle();
            auto* expected = stock.handle();
            const auto array = bindings.global("Array");
            const auto raw = GuardedCommandFunctionAccess::read(array.value);
            const auto* constructor = raw.as<QV4::Object>();
            const auto prototype = GuardedCommandFunctionAccess::managed(QV4::Value::fromHeapObject(actual->objectPrototype()->d()));
            if (actual->qmlEngine() || array.status != FOUND || !constructor ||
                constructor->d() != actual->arrayCtor()->d() || actual->objectPrototype()->d()->prototype() ||
                actual->internalClasses(QV4::EngineBase::Class_Object)->prototype != actual->objectPrototype()->d() ||
                QQmlMetaType::metaObjectForValueType(QMetaType::fromType<QPoint>()) != &QQmlPointValueType::staticMetaObject ||
                guardedScriptProperty(prototype, QStringLiteral("getGameRules"), ledger).status != GuardedScriptPropertyStatus::Missing)
            {
                return false;
            }
            return sameBuiltin(engine, stock, *actual->functionPrototype(), *expected->functionPrototype(), QLatin1StringView("call"), ledger) &&
                sameBuiltin(engine, stock, *actual->functionPrototype(), *expected->functionPrototype(), QLatin1StringView("apply"), ledger) &&
                sameBuiltin(engine, stock, *actual->stringPrototype(), *expected->stringPrototype(), QLatin1StringView("toUpperCase"), ledger) &&
                sameBuiltin(engine, stock, *constructor, *expected->arrayCtor(), QLatin1StringView("isArray"), ledger);
        }

        bool nonproductionAction(ActionBindings & bindings, const GuardedScriptValueView & command,
                                 std::string_view name, RefinementLedger & ledger)
        {
            const auto lookup = bindings.member(command, "productionActionLookup");
            const auto module = bindings.global(name);
            if (lookup.status != FOUND || module.status != FOUND) { return false; }
            const auto production = guardedScriptProperty(lookup.value, QString(latin(name)), ledger);
            if (production.status != GuardedScriptPropertyStatus::Missing)
            {
                const auto scalar = guardedScriptScalar(production.value, ledger);
                const auto* value = std::get_if<bool>(&scalar.value);
                if (production.status != FOUND || scalar.status != GuardedScriptScalarStatus::Verified || !value || *value)
                {
                    return false;
                }
            }
            for (const auto field : {QStringLiteral("getIsBaseProductionAction"), QStringLiteral("canBuildUnits")})
            {
                const auto property = guardedScriptProperty(module.value, field, ledger);
                if (property.status != GuardedScriptPropertyStatus::Missing &&
                    (property.status != FOUND || (!property.value.isNull() && property.value.kind() != GuardedScriptValueKind::Undefined)))
                {
                    return false;
                }
            }
            return bindings.originMarker(module.value) &&
                bindings.writableNull(module.value, QLatin1StringView("__cmode_co_command_building"));
        }

        bool commandActionHelpers(QJSEngine & engine, ActionBindings & bindings,
                                  const GuardedScriptValueView & command, RefinementLedger & ledger, bool buildingCallbacks)
        {
            const LinkedUnit* linked = nullptr;
            for (const auto & helper : ActionDeclaration::COMMAND_HELPERS)
            {
                if (!bindings.helper(command, helper, linked)) { return false; }
            }
            if (buildingCallbacks)
            {
                for (const auto & helper : ActionDeclaration::COMMAND_BUILDING_HELPERS)
                {
                    if (!bindings.helper(command, helper, linked)) { return false; }
                }
                if (!moduleOperands(engine, linked, ActionDeclaration::COMMAND_BUILDING_EXTRA, ledger) ||
                    !bindings.text(command, "PRODUCTION_BADGE_KEY", GUARDED_COMMAND_BUILDING_BADGE_SETTING))
                {
                    return false;
                }
            }
            for (const auto & helper : CommandDeclaration::WEATHER_HELPERS)
            {
                if (helper.name == "probeWeatherImmune") { continue; }
                const auto actual = bindings.member(command, helper.name);
                if (actual.status != FOUND || !matchFunction(engine, actual.value, helper, linked, ledger)) { return false; }
            }
            if (!moduleOperands(engine, linked, ActionDeclaration::COMMAND, ledger) ||
                !moduleStrings(linked, CommandDeclaration::WEATHER_STRINGS,
                               QLatin1StringView("/mods/custom_gamemodes/scripts/general/cmode_co_command.js"), ledger) ||
                !storedInteger(*linked, 2, 1) || !storedInteger(*linked, 7, 0) ||
                !literalShape(*linked, *engine.handle(), 21, CommandDeclaration::SHAPE_35, ledger) ||
                !literalShape(*linked, *engine.handle(), 22, CommandDeclaration::SHAPE_35, ledger))
            {
                return false;
            }
            const std::array texts{
                std::pair{"NEXT_PLAYER_ACTION_ID", QLatin1StringView("ACTION_NEXT_PLAYER")},
                std::pair{"ACTION_ORIGIN_KEY", QLatin1StringView("__cmode_co_command_origin")},
                std::pair{"ACTION_BUILDING_KEY", QLatin1StringView("__cmode_co_command_building")},
                std::pair{"UNIT_OWNER_KEY", GUARDED_COMMAND_UNIT_OWNER},
                std::pair{"UNIT_LINEAGE_KEY", GUARDED_COMMAND_UNIT_LINEAGE},
                std::pair{"BUILDING_OWNER_KEY", GUARDED_COMMAND_BUILDING_OWNER},
                std::pair{"BUILDING_LINEAGE_KEY", GUARDED_COMMAND_BUILDING_LINEAGE},
                std::pair{"ARMY_SPRITE_KEY", GUARDED_COMMAND_SPRITE_SETTING}};
            for (const auto & [name, value] : texts)
            {
                if (!bindings.text(command, name, value)) { return false; }
            }
            const auto lineage = bindings.member(command, "LINEAGE_1");
            const auto alternating = bindings.member(command, "BUILDER_MODE_ALTERNATING");
            const auto active = bindings.member(command, "activeHookCo");
            return lineage.status == FOUND && lineage.value.number() == 0 &&
                alternating.status == FOUND && alternating.value.number() == 3 &&
                active.status == FOUND && active.value.isNull() &&
                bindings.writableNull(command, QLatin1StringView("pendingBuild")) &&
                bindings.writableNull(command, QLatin1StringView("spriteContext")) &&
                nonproductionAction(bindings, command, "ACTION_WAIT", ledger) &&
                nonproductionAction(bindings, command, "ACTION_CAPTURE", ledger);
        }

        bool inactiveActionHelpers(QJSEngine & engine, ActionBindings & bindings, RefinementLedger & ledger)
        {
            const auto deck = bindings.global("CMODE_DECKBUILDER");
            const auto frontier = bindings.global("CMODE_PROCGEN");
            const LinkedUnit *deckUnit = nullptr, *frontierUnit = nullptr;
            if (deck.status != FOUND || frontier.status != FOUND) { return false; }
            for (const auto & helper : ActionDeclaration::DECK_HELPERS)
            {
                if (!bindings.helper(deck.value, helper, deckUnit)) { return false; }
            }
            for (const auto & helper : {ActionDeclaration::FRONTIER_GET_RULE, ActionDeclaration::FRONTIER_IS_ENABLED,
                                       ActionDeclaration::FRONTIER_ON_PLAYER_START})
            {
                if (!bindings.helper(frontier.value, helper, frontierUnit)) { return false; }
            }
            return moduleOperands(engine, deckUnit, ActionDeclaration::DECK, ledger) &&
                moduleOperands(engine, frontierUnit, ActionDeclaration::FRONTIER, ledger) &&
                bindings.text(frontier.value, "RULE_ID", QLatin1StringView("CMODE_PROCEDURAL_FRONTIER")) &&
                authenticateGuardedChessFunctions(engine, ledger) == GuardedNativeBindingStatus::Bound;
        }

        bool rankingCache(ActionBindings & bindings, const GuardedScriptValueView & command,
                          const GuardedNativeInventory & inventory, RefinementLedger & ledger)
        {
            if (inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS) { return false; }
            const auto cache = bindings.member(command, "unitBadgeState");
            if (cache.status != FOUND) { return false; }
            constexpr std::int64_t RANKING_CACHE_KEY_WORK = 1024;
            for (const auto & actor : inventory.actors)
            {
                if (!ledger.consume(RANKING_CACHE_KEY_WORK, RefinementWork::MasterPreparation) || actor.id <= 0)
                {
                    return false;
                }
                const auto state = guardedScriptProperty(cache.value, QString::number(actor.id), ledger);
                if (state.status != FOUND || state.prototypeDepth != 0) { return false; }
                const auto iconProperty = bindings.member(state.value, "icon");
                const auto insetProperty = bindings.member(state.value, "inset");
                if (iconProperty.status != FOUND || insetProperty.status != FOUND) { return false; }
                const auto icon = guardedScriptScalar(iconProperty.value, ledger);
                const auto inset = guardedScriptScalar(insetProperty.value, ledger);
                const auto* name = std::get_if<QString>(&icon.value);
                const auto* offset = std::get_if<double>(&inset.value);
                if (icon.status != GuardedScriptScalarStatus::Verified || !name || !name->isEmpty() ||
                    inset.status != GuardedScriptScalarStatus::Verified || !offset || *offset != 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool rankingEnvironment(QJSEngine & engine, const GuardedNativeInventory & inventory, RefinementLedger & ledger,
                                const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members)
        {
            ActionBindings bindings(engine, ledger, globals, members);
            const auto command = bindings.global("CO_COMMAND");
            const auto string = bindings.global("String");
            const auto undefined = bindings.global("undefined");
            const auto raw = GuardedCommandFunctionAccess::read(string.value);
            const auto* constructor = raw.as<QV4::Object>();
            if (engine.handle()->qmlEngine() || command.status != FOUND || string.status != FOUND || !constructor ||
                constructor->d() != engine.handle()->stringCtor()->d() ||
                undefined.status != FOUND || undefined.value.kind() != GuardedScriptValueKind::Undefined)
            {
                return false;
            }
            const LinkedUnit* linked = nullptr;
            for (const auto & helper : ActionDeclaration::COMMAND_RANKING_HELPERS)
            {
                if (!bindings.helper(command.value, helper, linked)) { return false; }
            }
            return moduleOperands(engine, linked, ActionDeclaration::COMMAND, ledger) &&
                moduleOperands(engine, linked, ActionDeclaration::COMMAND_RANKING_EXTRA, ledger) &&
                bindings.text(command.value, "UNIT_BADGE_KEY", GUARDED_COMMAND_UNIT_BADGE_SETTING) &&
                rankingCache(bindings, command.value, inventory, ledger);
        }

        bool actionEnvironment(QJSEngine & engine, QJSEngine & stock, RefinementLedger & ledger,
                               const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members, bool buildingCallbacks)
        {
            ActionBindings bindings(engine, ledger, globals, members);
            const auto command = bindings.global("CO_COMMAND");
            const auto global = bindings.global("Global");
            const auto undefined = bindings.global("undefined");
            const auto capture = bindings.global("ACTION_CAPTURE");
            if (command.status != FOUND || global.status != FOUND || undefined.status != FOUND || capture.status != FOUND ||
                undefined.value.kind() != GuardedScriptValueKind::Undefined ||
                guardedScriptGlobalAlias(engine, global.value, ledger).status != FOUND)
            {
                return false;
            }
            for (const auto name : {QLatin1StringView("postAnimationUnit"), QLatin1StringView("postAnimationBuilding"),
                                   QLatin1StringView("postAnimationTarget")})
            {
                if (!bindings.writableNull(capture.value, name)) { return false; }
            }
            return actionIntrinsics(engine, stock, bindings, ledger) &&
                commandActionHelpers(engine, bindings, command.value, ledger, buildingCallbacks) &&
                inactiveActionHelpers(engine, bindings, ledger);
        }
#endif
    }

    GuardedScriptGlobalIndex prepareGuardedCommandCallbackGlobals(QJSEngine & engine, RefinementLedger & ledger)
    {
#ifdef SCRIPTSOURCESUPPORT
        return GuardedScriptGlobalIndex::prepare(engine, GLOBALS, ledger);
#else
        Q_UNUSED(engine)
        Q_UNUSED(ledger)
        return {};
#endif
    }

    GuardedNativeForwarder guardedCommandCallbackForwarder(GuardedNativeCodeKind kind, const QString & member)
    {
        const auto found = std::ranges::find_if(CALLBACKS, [&](const auto & callback) {
            return callback.module == kind && member == QLatin1StringView(callback.member.data(), callback.member.size());
        });
        return found == CALLBACKS.end() ? GuardedNativeForwarder::None : found->forwarder;
    }

    std::uint16_t guardedCommandCallbackBit(GuardedNativeForwarder forwarder)
    {
        const auto found = std::ranges::find(CALLBACKS, forwarder, &Callback::forwarder);
        if (found == CALLBACKS.end()) { return 0; }
        return static_cast<std::uint16_t>(1u << std::distance(CALLBACKS.begin(), found));
    }

    bool guardedCommandHasRankingCallbacks(std::uint16_t mask)
    {
        return (mask & guardedCommandCallbackBit(GuardedNativeForwarder::CommandRanking)) != 0;
    }

    bool guardedCommandHasBuildingCallbacks(std::uint16_t mask)
    {
        for (std::size_t index = 0; index < CALLBACKS.size(); ++index)
        {
            const auto kind = CALLBACKS[index].kind;
            if ((mask & (1u << index)) &&
                (kind == CallbackKind::BuildingSprites || kind == CallbackKind::BuildingWeather))
            {
                return true;
            }
        }
        return false;
    }

    GuardedCommandOperand guardedCommandCallbackOperand(
        QJSEngine & engine, QJSEngine & stock, const GuardedScriptValueView & value,
        GuardedNativeForwarder forwarder, RefinementLedger & ledger,
        const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members)
    {
        if (!ledger.consume(CALLBACK_BINDING_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}, false};
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto refusals = ledger.stats().refusals;
        const auto callback = std::ranges::find(CALLBACKS, forwarder, &Callback::forwarder);
        if (callback == CALLBACKS.end() || &engine == &stock || engine.thread() != QThread::currentThread() ||
            stock.thread() != engine.thread() || !supportedRuntime())
        {
            return {};
        }
        ActionBindings bindings(engine, ledger, globals, members);
        const auto object = bindings.global(callback->global);
        const auto member = bindings.member(object.value, callback->member);
        const auto actual = GuardedCommandFunctionAccess::read(value);
        QV4::Value leaf;
        const bool same = object.status == FOUND && member.status == FOUND &&
            GuardedCommandFunctionAccess::read(member.value).asReturnedValue() == actual.asReturnedValue();
        bool bound = false;
        if (same)
        {
            switch (callback->kind)
            {
                case CallbackKind::Action:
                    bound = actionDelegate(engine, bindings, *callback, object.value, actual, leaf, ledger);
                    break;
                case CallbackKind::Context:
                    bound = contextDelegate(engine, *callback, actual, leaf, ledger);
                    break;
                case CallbackKind::BuildingSprites:
                case CallbackKind::BuildingWeather:
                    bound = buildingDelegate(engine, *callback, object.value, actual, leaf, ledger);
                    break;
                case CallbackKind::Ranking:
                    bound = rankingDelegate(engine, actual, leaf, ledger);
                    break;
            }
        }
        if (bound)
        {
            const auto operand = leaf.isNull() ? GuardedCommandFunctionAccess::nullValue() : GuardedCommandFunctionAccess::managed(leaf);
            return {GuardedNativeBindingStatus::Bound, operand, true};
        }
        if (ledger.stats().refusals != refusals)
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}, false};
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(stock)
        Q_UNUSED(globals)
        Q_UNUSED(members)
        Q_UNUSED(value)
        Q_UNUSED(forwarder)
#endif
        return {};
    }

    std::optional<GuardedCommandRankingBonus> guardedCommandRankingBonus(const QString & member)
    {
        for (const auto & definition : RANKING_BONUSES)
        {
            const auto name = definition.helper.records.front().name;
            if (member == QLatin1StringView(name.data(), name.size())) { return definition.kind; }
        }
        return {};
    }

    GuardedNativeBindingStatus authenticateCommandRankingBonus(
        QJSEngine & engine, const GuardedScriptValueView & value, GuardedCommandRankingBonus kind,
        RefinementLedger & ledger, const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members)
    {
        if (!ledger.consume(CALLBACK_BINDING_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto found = std::ranges::find(RANKING_BONUSES, kind, &RankingBonusDefinition::kind);
        const auto refusals = ledger.stats().refusals;
        if (found != RANKING_BONUSES.end() && engine.thread() == QThread::currentThread() && supportedRuntime())
        {
            ActionBindings bindings(engine, ledger, globals, members);
            if (rankingBonusDefinition(engine, bindings, value, *found, ledger)) { return GuardedNativeBindingStatus::Bound; }
        }
        if (ledger.stats().refusals != refusals) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(value)
        Q_UNUSED(kind)
        Q_UNUSED(globals)
        Q_UNUSED(members)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedNativeBindingStatus authenticateGuardedRankingFunctions(
        QJSEngine & engine, const GuardedNativeInventory & inventory, RefinementLedger & ledger,
        const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members)
    {
        if (!ledger.consume(CALLBACK_BINDING_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto refusals = ledger.stats().refusals;
        if (engine.thread() == QThread::currentThread() && supportedRuntime() &&
            rankingEnvironment(engine, inventory, ledger, globals, members))
        {
            return GuardedNativeBindingStatus::Bound;
        }
        if (ledger.stats().refusals != refusals) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(inventory)
        Q_UNUSED(globals)
        Q_UNUSED(members)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedNativeBindingStatus authenticateGuardedActionFunctions(
        QJSEngine & engine, QJSEngine & stock, RefinementLedger & ledger,
        const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* members, bool buildingCallbacks)
    {
        if (!ledger.consume(CALLBACK_BINDING_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto refusals = ledger.stats().refusals;
        if (&engine != &stock && engine.thread() == QThread::currentThread() && stock.thread() == engine.thread() &&
            supportedRuntime() && actionEnvironment(engine, stock, ledger, globals, members, buildingCallbacks))
        {
            return GuardedNativeBindingStatus::Bound;
        }
        if (ledger.stats().refusals != refusals) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(stock)
        Q_UNUSED(globals)
        Q_UNUSED(members)
        Q_UNUSED(buildingCallbacks)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }
}
