#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardedcommandfunctions_p.h"
#include "ai/coordinator/guardedmovementdeclarations.h"

#include <QJSEngine>
#include <QPoint>
#include <QThread>
#include <cstring>

#ifdef SCRIPTSOURCESUPPORT
#include <private/qv4context_p.h>
#include <private/qv4arraydata_p.h>
#include <private/qv4qobjectwrapper_p.h>
#include <private/qv4string_p.h>
#include <private/qqmlmetatype_p.h>
#include <private/qqmlvaluetype_p.h>
#endif

namespace Coordinator
{
#ifdef SCRIPTSOURCESUPPORT
    using namespace GuardedCommandDetail;
#endif
    namespace
    {
        constexpr std::int64_t MOVEMENT_BINDING_WORK = 131072;
#ifdef SCRIPTSOURCESUPPORT

        bool movementDelegate(QJSEngine & engine, const QV4::Value & value, const MovementDeclaration::Closure & definition,
                              const LinkedUnit*& linked, QV4::Value & delegate, RefinementLedger & ledger)
        {
            const auto* script = value.as<QV4::JavaScriptFunctionObject>();
            if (!matchRecord(engine, script, definition.wrapper, linked, ledger) ||
                !functionAt(*linked, script->function(), definition.wrapperIndex))
            {
                return false;
            }
            const auto* call = closureContext(engine, *script, {&definition.creator, 1},
                                              definition.creatorIndex, definition.locals, 0, linked, ledger);
            if (!call || definition.slot >= definition.locals)
            {
                return false;
            }
            delegate = call->locals.values[definition.slot];
            return true;
        }

        bool movementIntrinsics(QJSEngine & engine, QJSEngine & stock, const QV4::Value & called, RefinementLedger & ledger)
        {
            auto* vm = engine.handle();
            auto* expected = stock.handle();
            const auto* fn = called.as<QV4::JavaScriptFunctionObject>();
            if (!fn || fn->d()->prototype() != vm->functionPrototype()->d() ||
                guardedScriptBuiltinProperty(GuardedCommandFunctionAccess::managed(called), QStringLiteral("call"), ledger).status !=
                    GuardedScriptPropertyStatus::Missing ||
                vm->qmlEngine() || vm->sequencePrototype()->d()->prototype() != vm->arrayPrototype()->d() ||
                vm->arrayPrototype()->d()->prototype() != vm->objectPrototype()->d() || vm->objectPrototype()->d()->prototype())
            {
                return false;
            }
            // A push into a fresh array can call an inherited numeric setter.
            for (const auto* proto : {vm->arrayPrototype(), vm->objectPrototype()})
            {
                const auto* indexed = proto->arrayData();
                if (indexed && (indexed->type != QV4::Heap::ArrayData::Simple || indexed->values.size != 0))
                {
                    return false;
                }
            }
            const auto equal = [&](const QV4::Object* actual, const QV4::Object* approved, QLatin1StringView name, bool getter) {
                const auto a = builtinMember(engine, *actual, name, getter, ledger);
                const auto b = builtinMember(stock, *approved, name, getter, ledger);
                return a.kind == GuardedScriptFunctionKind::Builtin && b.kind == GuardedScriptFunctionKind::Builtin && a.sameImplementation(b);
            };
            return equal(vm->functionPrototype(), expected->functionPrototype(), QLatin1StringView("call"), false) &&
                   equal(vm->arrayPrototype(), expected->arrayPrototype(), QLatin1StringView("push"), false) &&
                   equal(vm->sequencePrototype(), expected->sequencePrototype(), QLatin1StringView("length"), true);
        }

        bool movementSequence()
        {
            const auto list = QMetaType::fromType<QVector<QPoint>>();
            if (QQmlMetaType::metaObjectForValueType(list) ||
                QQmlMetaType::metaObjectForValueType(QMetaType::fromType<QPoint>()) != &QQmlPointValueType::staticMetaObject)
            {
                return false;
            }
            const auto expected = QMetaSequence::fromContainer<QVector<QPoint>>();
            const auto registered = QQmlMetaType::qmlListType(list);
            return registered.isSequentialContainer() && registered.listMetaSequence() == expected;
        }

        bool movementMarker(const QV4::Value & prior, QJSEngine & engine)
        {
            if (prior.isNull()) { return true; }
            const auto* wrapper = prior.as<QV4::QObjectWrapper>();
            if (!wrapper || wrapper->vtable() != QV4::QObjectWrapper::staticVTable() || wrapper->engine() != engine.handle() ||
                wrapper->internalClass()->size || wrapper->arrayData() || wrapper->d()->prototype() != engine.handle()->objectPrototype()->d())
            {
                return false;
            }
            auto* object = wrapper->object();
            return !object || QJSEngine::objectOwnership(object) == QJSEngine::CppOwnership;
        }

        bool literalText(const GuardedScriptValueView & source, QLatin1StringView expected, RefinementLedger & ledger)
        {
            const auto observed = guardedScriptScalar(source, ledger);
            const auto* text = std::get_if<QString>(&observed.value);
            return observed.status == GuardedScriptScalarStatus::Verified && text && *text == expected;
        }

        constexpr auto MOVEMENT_FOUND = GuardedScriptPropertyStatus::Found;
        std::array<QString, 6 + MovementDeclaration::MODES.size()> movementGlobals()
        {
            using namespace MovementDeclaration;
            std::array<QString, 6 + MODES.size()> names{
                QStringLiteral("UNIT"), QStringLiteral("undefined"), QStringLiteral("WARDEN_BARRAGE_VARIABLE"),
                QString(latin(WARDEN.global)), QString(latin(FRONTIER.global)), QString(latin(CHESS.global))};
            for (std::size_t i = 0; i < MODES.size(); ++i)
            {
                names[6 + i] = QString(latin(MODES[i].module.global));
            }
            return names;
        }

        const auto MOVEMENT_GLOBALS = movementGlobals();

        class MovementBindings final
        {
        public:
            MovementBindings(QJSEngine & realm, RefinementLedger & ledger)
                : m_engine(realm), m_ledger(ledger), m_globals(GuardedScriptGlobalIndex::prepare(realm, MOVEMENT_GLOBALS, ledger)), m_members(realm, ledger) {}

            GuardedScriptProperty global(const QString & name)
            {
                const auto p = m_globals.find(m_engine, name, m_ledger);
                return p.prototypeDepth == 0 ? p : GuardedScriptProperty{};
            }

            GuardedScriptProperty member(const GuardedScriptValueView & object, const QString & name)
            {
                return own(object, name, m_members, m_ledger);
            }

            bool module(const MovementDeclaration::Module & d, const LinkedUnit*& linked, GuardedScriptValueView & object)
            {
                const auto binding = global(QString(latin(d.global)));
                if (binding.status != MOVEMENT_FOUND) { return false; }
                object = binding.value;
                for (const auto & f : d.helpers)
                {
                    const auto p = member(object, QString(latin(f.name)));
                    if (p.status != MOVEMENT_FOUND || !matchFunction(m_engine, p.value, f, linked, m_ledger)) { return false; }
                }
                if (!moduleStrings(linked, d.strings, latin(d.path), m_ledger)) { return false; }
                for (const auto & n : d.integers)
                {
                    if (!storedInteger(*linked, n.index, n.value)) { return false; }
                }
                return true;
            }

            bool modes(const GuardedScriptValueView & chess)
            {
                using namespace MovementDeclaration;
                const auto all = member(chess, QStringLiteral("_modes"));
                if (all.status != MOVEMENT_FOUND || !array(all.value, MODES.size())) { return false; }
                for (std::size_t i = 0; i < MODES.size(); ++i)
                {
                    const LinkedUnit* linked = nullptr;
                    GuardedScriptValueView mode;
                    if (!module(MODES[i].module, linked, mode)) { return false; }
                    const auto entry = guardedScriptArrayElement(all.value, i, m_ledger);
                    const auto id = member(mode, QStringLiteral("id"));
                    const auto list = member(mode, QStringLiteral("peers"));
                    if (entry.status != MOVEMENT_FOUND || entry.prototypeDepth ||
                        GuardedCommandFunctionAccess::read(entry.value).asReturnedValue() != GuardedCommandFunctionAccess::read(mode).asReturnedValue() ||
                        id.status != MOVEMENT_FOUND || !literalText(id.value, latin(MODES[i].module.global), m_ledger) ||
                        list.status != MOVEMENT_FOUND || !array(list.value, MODES[i].peers.size()))
                    {
                        return false;
                    }
                    for (std::size_t j = 0; j < MODES[i].peers.size(); ++j)
                    {
                        const auto peer = guardedScriptArrayElement(list.value, j, m_ledger);
                        if (peer.status != MOVEMENT_FOUND || peer.prototypeDepth || !literalText(peer.value, latin(MODES[MODES[i].peers[j]].module.global), m_ledger))
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

        private:
            bool array(const GuardedScriptValueView & view, std::size_t length)
            {
                const auto observed = guardedScriptArray(view, m_ledger);
                return observed.status == MOVEMENT_FOUND && observed.length == length;
            }
            QJSEngine & m_engine;
            RefinementLedger & m_ledger;
            GuardedScriptGlobalIndex m_globals;
            GuardedScriptOwnIndexCache m_members;
        };

        bool movementPostAction(QJSEngine & engine, QJSEngine & stock, const GuardedScriptValueView & value, RefinementLedger & ledger)
        {
            using namespace MovementDeclaration;
            const auto actual = GuardedCommandFunctionAccess::read(value);
            MovementBindings surface(engine, ledger);
            const auto unit = surface.global(MOVEMENT_GLOBALS[0]);
            const auto undefined = surface.global(MOVEMENT_GLOBALS[1]);
            const auto key = surface.global(MOVEMENT_GLOBALS[2]);
            if (unit.status != MOVEMENT_FOUND || undefined.status != MOVEMENT_FOUND || undefined.value.kind() != GuardedScriptValueKind::Undefined ||
                key.status != MOVEMENT_FOUND || !literalText(key.value, GUARDED_WARDEN_BARRAGES, ledger))
            {
                return false;
            }
            const auto base = surface.member(unit.value, QStringLiteral("postAction"));
            if (base.status != MOVEMENT_FOUND || GuardedCommandFunctionAccess::read(base.value).asReturnedValue() != actual.asReturnedValue())
            {
                return false;
            }
            const LinkedUnit *frontier = nullptr, *warden = nullptr, *chess = nullptr;
            QV4::Value wardenFn, chessFn, last;
            GuardedScriptValueView frontierApi, wardenApi, chessApi;
            if (!movementDelegate(engine, actual, FRONTIER_POST, frontier, wardenFn, ledger) || !surface.module(FRONTIER, frontier, frontierApi) ||
                !movementDelegate(engine, wardenFn, WARDEN_POST, warden, chessFn, ledger) || !surface.module(WARDEN, warden, wardenApi) ||
                !movementDelegate(engine, chessFn, CHESS_POST, chess, last, ledger) || !last.isNull() || !surface.module(CHESS, chess, chessApi))
            {
                return false;
            }
            const auto rule = surface.member(frontierApi, QStringLiteral("RULE_ID"));
            const auto w = GuardedCommandFunctionAccess::read(wardenApi);
            const auto* holder = w.as<QV4::Object>();
            QV4::Value prior;
            QV4::PropertyAttributes attributes;
            return rule.status == MOVEMENT_FOUND && literalText(rule.value, QLatin1StringView("CMODE_PROCEDURAL_FRONTIER"), ledger) &&
                holder && std::strcmp(holder->vtable()->className, "Object") == 0 &&
                descriptorSlot(*holder, QLatin1StringView("resolvedAction"), attributes, prior, ledger) && attributes.isData() && attributes.isWritable() &&
                movementMarker(prior, engine) &&
                surface.modes(chessApi) && movementIntrinsics(engine, stock, wardenFn, ledger) && movementSequence();
        }
#endif
    }

    GuardedNativeBindingStatus authenticateGuardedChessFunctions(QJSEngine & engine, RefinementLedger & ledger)
    {
        if (!ledger.consume(MOVEMENT_BINDING_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto before = ledger.stats().refusals;
        if (engine.thread() == QThread::currentThread() && supportedRuntime())
        {
            MovementBindings bindings(engine, ledger);
            const LinkedUnit* linked = nullptr;
            GuardedScriptValueView chess;
            if (bindings.module(MovementDeclaration::CHESS, linked, chess) && bindings.modes(chess))
            {
                return GuardedNativeBindingStatus::Bound;
            }
        }
        if (ledger.stats().refusals != before) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedNativeBindingStatus authenticateMovementPostAction(
        QJSEngine & engine, QJSEngine & stock, const GuardedScriptValueView & value, RefinementLedger & ledger)
    {
        if (!ledger.consume(MOVEMENT_BINDING_WORK, RefinementWork::MasterPreparation)) { return GuardedNativeBindingStatus::BudgetExhausted; }
#ifdef SCRIPTSOURCESUPPORT
        const auto before = ledger.stats().refusals;
        if (&engine != &stock && engine.thread() == QThread::currentThread() && stock.thread() == engine.thread() &&
            supportedRuntime() && movementPostAction(engine, stock, value, ledger)) { return GuardedNativeBindingStatus::Bound; }
        if (ledger.stats().refusals != before) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(stock)
        Q_UNUSED(value)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedCommandOperand guardedMovementTrapOperand(QJSEngine & engine, const GuardedScriptValueView & value, RefinementLedger & ledger)
    {
        if (!ledger.consume(MOVEMENT_BINDING_WORK, RefinementWork::MasterPreparation)) { return {GuardedNativeBindingStatus::BudgetExhausted, {}, false}; }
#ifdef SCRIPTSOURCESUPPORT
        using namespace MovementDeclaration;
        const auto before = ledger.stats().refusals;
        const LinkedUnit* linked = nullptr;
        QV4::Value saved;
        if (engine.thread() == QThread::currentThread() && supportedRuntime() &&
            movementDelegate(engine, GuardedCommandFunctionAccess::read(value), WARDEN_TRAP, linked, saved, ledger) &&
            moduleStrings(linked, WARDEN.strings, latin(WARDEN.path), ledger))
        {
            const auto* captured = saved.as<QV4::JavaScriptFunctionObject>();
            if (captured && captured->engine() == engine.handle())
            {
                return {GuardedNativeBindingStatus::Bound, GuardedCommandFunctionAccess::managed(saved), true};
            }
        }
        if (ledger.stats().refusals != before) { return {GuardedNativeBindingStatus::BudgetExhausted, {}, false}; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(value)
#endif
        return {};
    }

}
