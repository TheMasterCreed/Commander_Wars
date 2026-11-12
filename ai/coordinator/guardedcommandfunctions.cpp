#include "ai/coordinator/guardedcommandpolicy.h"

#include "ai/coordinator/guardedcommanddeclarations.h"
#include "ai/coordinator/guardedcommandfunctions_p.h"
#include "ai/coordinator/guardedscriptfunction.h"

#include <algorithm>
#include <span>
#include <cstring>
#include <limits>

#include <QCryptographicHash>
#include <QJSEngine>
#include <QThread>
#include <QtEndian>

#ifdef SCRIPTSOURCESUPPORT
#include <private/qjsvalue_p.h>
#include <private/qv4compileddata_p.h>
#include <private/qv4context_p.h>
#include <private/qv4executablecompilationunit_p.h>
#include <private/qv4functionobject_p.h>
#include <private/qv4string_p.h>
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
#include <qt_windows.h>
#include <winver.h>
#endif
#endif

namespace Coordinator
{
#ifdef SCRIPTSOURCESUPPORT

    namespace GuardedCommandDetail
    {
        using Unit = QV4::CompiledData::Unit;
        using Shape = QV4::Heap::InternalClass;
        constexpr std::uint32_t MAX_UNIT_BYTES = 1024 * 1024;
        constexpr qsizetype MAX_NAME = 128;
        constexpr std::int64_t MEMBER_WORK = 1024;
        constexpr std::int64_t NAME_BASE_WORK = 256;
        constexpr std::int64_t NAME_CHARACTER_WORK = 16;
        constexpr std::int64_t NAME_WORK = NAME_BASE_WORK + NAME_CHARACTER_WORK * MAX_NAME;
        constexpr int MAX_DESCRIPTOR_MEMBERS = 4096;
        constexpr std::int64_t DESCRIPTOR_LOOKUP_WORK = 1024;
        constexpr std::int64_t DESCRIPTOR_MEMBER_WORK = 48;
        constexpr std::int64_t DESCRIPTOR_CHARACTER_WORK = 4;
        constexpr QLatin1StringView COMMAND_PATH{"/custom_gamemodes/scripts/general/cmode_co_command.js"};
        constexpr QLatin1StringView RULE_PATH{"/custom_gamemodes/scripts/gamerules/victory/cmode_co_command.js"};

        bool supportedRuntime()
        {
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64) && QT_VERSION == QT_VERSION_CHECK(6, 8, 3)
            if (std::strcmp(qVersion(), "6.8.3") != 0)
            {
                return false;
            }
            // Inspect the loaded QtQml module's fixed VERSIONINFO in memory.
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&QJSEngine::staticMetaObject), &module))
            {
                return false;
            }
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
            const auto size = resource ? SizeofResource(module, resource) : 0;
            const auto data = resource ? LoadResource(module, resource) : nullptr;
            const auto* bytes = data ? static_cast<const char*>(LockResource(data)) : nullptr;
            constexpr wchar_t key[] = L"VS_VERSION_INFO";
            constexpr std::size_t fixed = (6 + sizeof(key) + 3) & ~std::size_t{3};
            if (!bytes || size < fixed + sizeof(VS_FIXEDFILEINFO) ||
                qFromLittleEndian<quint16>(bytes) < fixed + sizeof(VS_FIXEDFILEINFO) ||
                qFromLittleEndian<quint16>(bytes + 2) != sizeof(VS_FIXEDFILEINFO) ||
                std::memcmp(bytes + 6, key, sizeof(key)) != 0)
            {
                return false;
            }
            VS_FIXEDFILEINFO version{};
            std::memcpy(&version, bytes + fixed, sizeof(version));
            return version.dwSignature == VS_FFI_SIGNATURE && version.dwFileVersionMS == MAKELONG(8, 6) &&
                   version.dwFileVersionLS == MAKELONG(0, 3);
#else
            return false;
#endif
        }

        QLatin1StringView latin(std::string_view text)
        {
            return {text.data(), static_cast<qsizetype>(text.size())};
        }

        bool simpleName(const QV4::Heap::String* text, QString & result)
        {
            if (text == nullptr || text->subtype >= QV4::Heap::String::StringType_Complex ||
                text->text().size < 0 || text->text().size > MAX_NAME)
            {
                return false;
            }
            result = text->toQString();
            return true;
        }

        bool storedString(const LinkedUnit & linked, std::uint32_t index, std::string_view expected)
        {
            const auto* unit = linked.unitData();
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            const auto count = static_cast<std::uint32_t>(unit->stringTableSize);
            const auto table = static_cast<std::uint32_t>(unit->offsetToStringTable);
            if (expected.size() > static_cast<std::size_t>(MAX_NAME) ||
                index >= count || table > size || count > (size - table) / 4 || !linked.runtimeStrings)
            {
                return false;
            }
            const auto* bytes = reinterpret_cast<const char*>(unit);
            const auto offset = qFromLittleEndian<quint32>(bytes + table + 4 * index);
            if (offset > size - 4 || expected.size() > (size - offset - 4) / 2 ||
                qFromLittleEndian<quint32>(bytes + offset) != expected.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                if (qFromLittleEndian<quint16>(bytes + offset + 4 + 2 * i) != static_cast<unsigned char>(expected[i]))
                {
                    return false;
                }
            }
            const auto* runtimeName = linked.runtimeStrings[index];
            if (!runtimeName || runtimeName->subtype >= QV4::Heap::String::StringType_Complex ||
                runtimeName->text().size != static_cast<qsizetype>(expected.size()))
            {
                return false;
            }
            QString runtime;
            return simpleName(runtimeName, runtime) && runtime == latin(expected);
        }

        bool storedString(const LinkedUnit & linked, std::uint32_t index, std::string_view expected,
                          RefinementLedger & ledger)
        {
            return expected.size() <= static_cast<std::size_t>(MAX_NAME) &&
                ledger.consume(NAME_BASE_WORK + NAME_CHARACTER_WORK * static_cast<std::int64_t>(expected.size()),
                               RefinementWork::MasterPreparation) &&
                storedString(linked, index, expected);
        }

        bool extendsShape(const Shape* shape, const Shape* base, std::uint32_t members)
        {
            for (auto remaining = members; remaining > 0; --remaining)
            {
                if (!shape || shape->engine != base->engine || shape->vtable != base->vtable ||
                    shape->prototype != base->prototype || shape->size != base->size + remaining)
                {
                    return false;
                }
                shape = shape->parent;
            }
            return shape == base;
        }

        bool memberName(const Shape & shape, std::uint32_t slot, QV4::PropertyAttributes attributes,
                        const QV4::Heap::String* expected, RefinementLedger & ledger)
        {
            const auto* table = shape.propertyTable.d;
            if (slot >= shape.size || shape.propertyData.at(slot) != attributes ||
                !table || table->alloc < 0 || table->alloc > 256 ||
                !ledger.consume(32 * (table->alloc + 1), RefinementWork::MasterPreparation))
            {
                return false;
            }
            bool found = false;
            for (int entry = 0; entry < table->alloc; ++entry)
            {
                const auto & member = table->entries[entry];
                if (!member.identifier.isValid() || member.index != slot)
                {
                    continue;
                }
                QString actualName, expectedName;
                if (found || !member.identifier.isString() ||
                    !simpleName(member.identifier.asStringOrSymbol<QV4::Heap::String>(), actualName) ||
                    !simpleName(expected, expectedName) || actualName != expectedName)
                {
                    return false;
                }
                found = true;
            }
            return found;
        }

        bool ordinaryActivation(const QV4::JavaScriptFunctionObject & script, RefinementLedger & ledger,
                                std::uint32_t expectedLocals = 0)
        {
            const auto* code = script.function();
            const auto* fn = code->compiledFunction;
            const auto* base = script.engine()->internalClasses(QV4::EngineBase::Class_CallContext);
            const auto* shape = code->internalClass.get();
            if (!base || !code->executableCompilationUnit()->runtimeStrings || fn->nLocals != expectedLocals ||
                fn->nFormals > 9 || code->nFormals != fn->nFormals ||
                !extendsShape(shape, base, fn->nFormals + expectedLocals))
            {
                return false;
            }
            for (std::uint32_t i = 0; i < fn->nFormals; ++i)
            {
                const auto index = static_cast<std::uint32_t>(fn->formalsTable()[i].nameIndex);
                if (index >= code->executableCompilationUnit()->unitData()->stringTableSize ||
                    !memberName(*shape, base->size + expectedLocals + i, QV4::Attr_NotConfigurable, code->runtimeString(index), ledger))
                {
                    return false;
                }
            }
            for (std::uint32_t i = 0; i < expectedLocals; ++i)
            {
                const auto index = static_cast<std::uint32_t>(fn->localsTable()[i]);
                if (index >= code->executableCompilationUnit()->unitData()->stringTableSize ||
                    !memberName(*shape, base->size + i, QV4::Attr_NotConfigurable, code->runtimeString(index), ledger))
                {
                    return false;
                }
            }
            return true;
        }

        bool matchRecord(QJSEngine & engine, const QV4::JavaScriptFunctionObject* script,
                         const CommandDeclaration::Function & declaration, const LinkedUnit*& linked,
                         RefinementLedger & ledger, std::uint32_t expectedLocals)
        {
            if (!ledger.consume(MEMBER_WORK + 8 * declaration.recordBytes + 3 * NAME_WORK, RefinementWork::MasterPreparation))
            {
                return false;
            }
            if (!script || script->engine() != engine.handle() || !script->function() ||
                std::strcmp(script->vtable()->className, "ScriptFunction") != 0 ||
                !script->function()->compiledFunction || script->function()->kind != QV4::Function::Kind::JsUntyped ||
                script->function()->detectedInjectedParameters ||
                !script->function()->executableCompilationUnit() ||
                script->function()->executableCompilationUnit()->engine != engine.handle())
            {
                return false;
            }
            const auto* code = script->function();
            const auto* current = code->executableCompilationUnit();
            const auto* unit = current->unitData();
            const auto* fn = code->compiledFunction;
            const auto start = reinterpret_cast<std::uintptr_t>(unit);
            const auto position = reinterpret_cast<std::uintptr_t>(fn);
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            if ((linked && linked != current) || unit->version != 66 || unit->qtVersion != 0x060803 ||
                unit->flags != Unit::IsJavascript || size > MAX_UNIT_BYTES || size < declaration.recordBytes ||
                position < start || position - start > size - declaration.recordBytes ||
                fn->codeOffset > declaration.recordBytes || fn->codeSize != declaration.recordBytes - fn->codeOffset ||
                code->codeData != fn->code())
            {
                return false;
            }
            const QByteArrayView record(reinterpret_cast<const char*>(fn), declaration.recordBytes);
            if (QCryptographicHash::hash(record, QCryptographicHash::Sha256).toHex() !=
                    QByteArrayView(declaration.sha256.data(), declaration.sha256.size()) ||
                !ordinaryActivation(*script, ledger, expectedLocals))
            {
                return false;
            }
            linked = current;
            return true;
        }

        bool matchFunction(QJSEngine & engine, const GuardedScriptValueView & value, const CommandDeclaration::Function & declaration,
                           const LinkedUnit*& linked, RefinementLedger & ledger)
        {
            const auto raw = GuardedCommandFunctionAccess::read(value);
            return guardedScriptContextOperands(value, ledger).status == GuardedScriptContextOperands::GlobalScopeOnly &&
                matchRecord(engine, raw.as<QV4::JavaScriptFunctionObject>(), declaration, linked, ledger);
        }

        const QV4::Heap::CallContext* closureContext(
            QJSEngine & engine, const QV4::JavaScriptFunctionObject & script,
            std::span<const CommandDeclaration::Function> creators, std::uint32_t creatorIndex,
            std::uint32_t locals, std::uint32_t arguments, const LinkedUnit*& linked, RefinementLedger & ledger)
        {
            auto* native = engine.handle();
            const auto* root = native->rootContext()->d();
            const auto* scope = script.scope();
            if (!root || root->outer || root->activation != native->globalObject->d() || !scope ||
                scope->type != QV4::Heap::ExecutionContext::Type_CallContext || scope->outer != root || scope->activation ||
                scope->nArgs != arguments || scope->vtable() != native->internalClasses(QV4::EngineBase::Class_CallContext)->vtable)
            {
                return nullptr;
            }
            const auto* call = static_cast<const QV4::Heap::CallContext*>(scope);
            if (!call->function || locals > 24 || arguments > 9 ||
                call->locals.size != locals || call->locals.alloc != locals + arguments)
            {
                return nullptr;
            }
            const auto creating = QV4::Value::fromHeapObject(call->function.get());
            const auto* creator = creating.as<QV4::JavaScriptFunctionObject>();
            if (!creator || creator->scope() != root ||
                !std::ranges::any_of(creators, [&](const auto & record) {
                    return matchRecord(engine, creator, record, linked, ledger, locals);
                }) ||
                !functionAt(*linked, creator->function(), creatorIndex) ||
                call->internalClass.get() != creator->function()->internalClass.get())
            {
                return nullptr;
            }
            return call;
        }

        bool descriptorSlot(const QV4::Object & holder, QLatin1StringView name, QV4::PropertyAttributes & attributes,
                          QV4::Value & value, RefinementLedger & ledger)
        {
            const auto* shape = holder.internalClass();
            const auto* table = shape->propertyTable.d;
            if (!table || table->alloc < 0 || table->alloc > MAX_DESCRIPTOR_MEMBERS ||
                !ledger.consume(DESCRIPTOR_LOOKUP_WORK + (DESCRIPTOR_MEMBER_WORK + DESCRIPTOR_CHARACTER_WORK * name.size()) * table->alloc, RefinementWork::MasterPreparation))
            {
                return false;
            }
            std::optional<std::uint32_t> found;
            for (int i = 0; i < table->alloc; ++i)
            {
                const auto & e = table->entries[i];
                if (!e.identifier.isValid() || !e.identifier.isString() || e.index >= shape->size || shape->propertyData.at(e.index).isEmpty())
                {
                    continue;
                }
                const auto* str = e.identifier.asStringOrSymbol<QV4::Heap::String>();
                if (!str || str->subtype >= QV4::Heap::String::StringType_Complex)
                {
                    return false;
                }
                if (str->text().size == name.size() && str->toQString() == name)
                {
                    if (found && *found != e.index) { return false; }
                    attributes = shape->propertyData.at(e.index);
                    value = *holder.propertyData(e.index);
                    found = e.index;
                }
            }
            return found.has_value();
        }

        GuardedScriptFunctionImage builtinMember(QJSEngine & realm, const QV4::Object & holder, QLatin1StringView name,
                                                   bool getter, RefinementLedger & ledger)
        {
            QV4::PropertyAttributes attr;
            QV4::Value value;
            if (!descriptorSlot(holder, name, attr, value, ledger) || (getter ? !attr.isAccessor() : !attr.isData()))
            {
                return {};
            }
            const auto* builtin = value.as<QV4::DynamicFunctionObject>();
            if (!builtin || builtin->engine() != realm.handle()) { return {}; }
            return guardedScriptFunctionImage(GuardedCommandFunctionAccess::managed(value), ledger);
        }

        bool literalShape(const LinkedUnit & linked, QV4::ExecutionEngine & engine, std::uint32_t index,
                          std::span<const std::uint32_t> names, RefinementLedger & ledger)
        {
            const auto* unit = linked.unitData();
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            const auto table = static_cast<std::uint32_t>(unit->offsetToJSClassTable);
            const auto count = static_cast<std::uint32_t>(unit->jsClassTableSize);
            if (!linked.runtimeClasses || index >= count || table > size || count > (size - table) / 4)
            {
                return false;
            }
            const auto* bytes = reinterpret_cast<const char*>(unit);
            const auto offset = qFromLittleEndian<quint32>(bytes + table + 4 * index);
            const auto* base = engine.internalClasses(QV4::EngineBase::Class_Object);
            const auto* shape = linked.runtimeClasses[index];
            if (offset > size - 4 || names.size() > (size - offset - 4) / 4 ||
                qFromLittleEndian<quint32>(bytes + offset) != names.size() ||
                !base || !extendsShape(shape, base, static_cast<std::uint32_t>(names.size())))
            {
                return false;
            }
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                // The high bit is the accessor flag. Every admitted member is an ordinary data field.
                if (qFromLittleEndian<quint32>(bytes + offset + 4 + 4 * i) != names[i] ||
                    names[i] >= unit->stringTableSize ||
                    !memberName(*shape, base->size + i, QV4::Attr_Data, linked.runtimeStrings[names[i]], ledger))
                {
                    return false;
                }
            }
            return true;
        }

        GuardedScriptProperty own(const GuardedScriptValueView & object, const QString & name,
                                  GuardedScriptOwnIndexCache & cache, RefinementLedger & ledger)
        {
            const auto property = guardedScriptProperty(object, name, ledger, &cache);
            return property.prototypeDepth == 0 ? property : GuardedScriptProperty{};
        }

        bool moduleStrings(const LinkedUnit* linked, std::span<const CommandDeclaration::String> strings,
                           QLatin1StringView path, RefinementLedger & ledger)
        {
            constexpr std::int64_t moduleWork = 16384 + 2 * NAME_WORK;
            constexpr auto maximumEntries = (std::numeric_limits<std::int64_t>::max() - moduleWork) / NAME_BASE_WORK;
            if (!linked || strings.size() > static_cast<std::size_t>(maximumEntries) ||
                !ledger.consume(moduleWork, RefinementWork::MasterPreparation))
            {
                return false;
            }
            const auto* unit = linked->unitData();
            if (unit->sourceFileIndex >= unit->stringTableSize || !linked->runtimeStrings)
            {
                return false;
            }
            const auto* name = linked->runtimeStrings[unit->sourceFileIndex];
            if (!name || name->subtype >= QV4::Heap::String::StringType_Complex ||
                name->text().size < 0 || name->text().size > 1024 || !name->toQString().endsWith(path))
            {
                return false;
            }
            for (const auto & s : strings)
            {
                if (!storedString(*linked, s.index, s.value, ledger))
                {
                    return false;
                }
            }
            return true;
        }

        bool verifyModule(QJSEngine & engine, const GuardedScriptValueView & object, GuardedScriptOwnIndexCache & cache,
                          std::span<const CommandDeclaration::Function> functions,
                          std::span<const CommandDeclaration::String> strings, QLatin1StringView path,
                          bool command, RefinementLedger & ledger)
        {
            const LinkedUnit* linked = nullptr;
            for (const auto & fn : functions)
            {
                const auto p = own(object, QString(latin(fn.name)), cache, ledger);
                if (p.status != GuardedScriptPropertyStatus::Found || !matchFunction(engine, p.value, fn, linked, ledger))
                {
                    return false;
                }
            }
            if (!moduleStrings(linked, strings, path, ledger))
            {
                return false;
            }
            if (command)
            {
                for (const auto & shape : CommandDeclaration::COMMAND_SHAPES)
                {
                    if (!literalShape(*linked, *engine.handle(), shape.index, shape.members, ledger))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool commandConstants(const GuardedScriptValueView & object, const GuardedScriptValueView & rule,
                              GuardedScriptOwnIndexCache & cache, RefinementLedger & ledger)
        {
            const auto read = [&](const QString & key) {
                const auto p = own(object, key, cache, ledger);
                return p.status == GuardedScriptPropertyStatus::Found ? guardedScriptScalar(p.value, ledger) : GuardedScriptScalar{};
            };
            const auto text = [&](const QString & key, QLatin1StringView expected) {
                const auto s = read(key);
                const auto* actual = std::get_if<QString>(&s.value);
                return s.status == GuardedScriptScalarStatus::Verified && actual && *actual == expected;
            };
            const auto integer = [&](const QString & key, int expected) {
                const auto s = read(key);
                const auto* actual = std::get_if<double>(&s.value);
                return s.status == GuardedScriptScalarStatus::Verified && actual && *actual == expected;
            };
            const auto infinity = own(rule, QStringLiteral("SPINBOX_INFINITY"), cache, ledger);
            return infinity.status == GuardedScriptPropertyStatus::Found && infinity.value.number() == -1000 &&
                   text(QStringLiteral("RULE_ID"), GUARDED_NATIVE_COMMAND_RULE) &&
                   text(QStringLiteral("RULE_VALUE_KEY"), QLatin1StringView("SpinboxValue0")) &&
                   text(QStringLiteral("BUILDER_MODE_KEY"), QLatin1StringView("SpinboxValue1")) &&
                   text(QStringLiteral("PENDING_POWER_VALID_KEY"), QLatin1StringView("CMODE_CO_COMMAND_POWER_VALID")) &&
                   text(QStringLiteral("POWER_CHARGE_MODE_KEY"), QLatin1StringView("SpinboxValue2")) &&
                   integer(QStringLiteral("BUILDER_MODE_FREE"), 1) && integer(QStringLiteral("BUILDER_MODE_ACTIVE"), 2) &&
                   integer(QStringLiteral("BUILDER_MODE_ASSIGNED"), 4) &&
                   integer(QStringLiteral("LEGACY_BUILDER_MODE_ACTIVE_HALF_CHARGE"), 4) &&
                   integer(QStringLiteral("POWER_CHARGE_MODE_FIRST_SLOT"), 1) &&
                   integer(QStringLiteral("POWER_CHARGE_MODE_WEIGHTED_SPLIT"), 5) &&
                   integer(QStringLiteral("JOIN_MODE_DISABLED"), 1) && integer(QStringLiteral("JOIN_MODE_ACTIVE"), 3);
        }

        bool functionAt(const LinkedUnit & linked, const QV4::Function* function, std::uint32_t index)
        {
            const auto* unit = linked.unitData();
            const auto length = static_cast<std::uint32_t>(unit->unitSize);
            const auto table = static_cast<std::uint32_t>(unit->offsetToFunctionTable);
            const auto count = static_cast<std::uint32_t>(unit->functionTableSize);
            if (index >= count || table > length || count > (length - table) / 4 ||
                index >= linked.runtimeFunctions.size() || linked.runtimeFunctions[index] != function)
            {
                return false;
            }
            const auto* bytes = reinterpret_cast<const char*>(unit);
            const auto offset = qFromLittleEndian<quint32>(bytes + table + 4 * index);
            return offset < length && bytes + offset == reinterpret_cast<const char*>(function->compiledFunction);
        }

        bool storedConstant(const LinkedUnit & linked, std::uint32_t index, std::uint64_t expected)
        {
            const auto* unit = linked.unitData();
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            const auto offset = static_cast<std::uint32_t>(unit->offsetToConstantTable);
            if (!linked.constants || index >= unit->constantTableSize ||
                offset > size || unit->constantTableSize > (size - offset) / sizeof(quint64))
            {
                return false;
            }
            const auto* table = reinterpret_cast<const char*>(unit) + offset;
            return qFromLittleEndian<quint64>(table + sizeof(quint64) * index) == expected &&
                linked.constants[index].asReturnedValue() == expected;
        }

        bool catchBlock(const LinkedUnit & linked, QV4::ExecutionEngine & engine, std::uint32_t index,
                        std::uint32_t name, RefinementLedger & ledger)
        {
            if (!ledger.consume(4096 + NAME_WORK, RefinementWork::MasterPreparation)) { return false; }
            const auto* unit = linked.unitData();
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            const auto table = static_cast<std::uint32_t>(unit->offsetToBlockTable);
            const auto count = static_cast<std::uint32_t>(unit->blockTableSize);
            if (index >= count || index >= linked.runtimeBlocks.size() ||
                table > size || count > (size - table) / 4)
            {
                return false;
            }
            const auto* bytes = reinterpret_cast<const char*>(unit);
            const auto start = qFromLittleEndian<quint32>(bytes + table + 4 * index);
            constexpr std::uint32_t blockBytes = 20;
            if (size < blockBytes || start > size - blockBytes ||
                qFromLittleEndian<quint32>(bytes + start) != 1 ||
                qFromLittleEndian<quint32>(bytes + start + 4) != 16 ||
                qFromLittleEndian<quint16>(bytes + start + 8) != 1 ||
                qFromLittleEndian<quint16>(bytes + start + 10) != 0 ||
                qFromLittleEndian<quint32>(bytes + start + 16) != name ||
                !storedString(linked, name, "e"))
            {
                return false;
            }
            const auto* base = engine.internalClasses(QV4::EngineBase::Class_CallContext);
            const auto* shape = linked.runtimeBlocks[index];
            return base && extendsShape(shape, base, 1) &&
                memberName(*shape, base->size, QV4::Attr_NotConfigurable, linked.runtimeStrings[name], ledger);
        }

        bool storedInteger(const LinkedUnit & linked, std::uint32_t index, int expected)
        {
            const auto* unit = linked.unitData();
            const auto size = static_cast<std::uint32_t>(unit->unitSize);
            const auto offset = static_cast<std::uint32_t>(unit->offsetToConstantTable);
            if (!linked.constants || index >= unit->constantTableSize ||
                offset > size || unit->constantTableSize > (size - offset) / sizeof(quint64))
            {
                return false;
            }
            const auto* table = reinterpret_cast<const char*>(unit) + offset;
            const auto stored = QV4::Value::fromReturnedValue(qFromLittleEndian<quint64>(table + sizeof(quint64) * index));
            const auto & live = linked.constants[index];
            return stored.isInteger() && live.isInteger() && stored.int_32() == expected && live.int_32() == expected;
        }

        bool initializerIntegers(const LinkedUnit & linked, std::span<const std::int32_t> expected)
        {
            if (linked.unitData()->constantTableSize != expected.size())
            {
                return false;
            }
            for (std::uint32_t index = 0; index < expected.size(); ++index)
            {
                if (!storedInteger(linked, index, expected[index]))
                {
                    return false;
                }
            }
            return true;
        }

        bool clearRepairCo(QJSEngine & engine, RefinementLedger & ledger, GuardedScriptOwnIndexCache* members, const GuardedScriptGlobalIndex* sharedGlobals)
        {
            const std::array<QString, 2> names{QStringLiteral("CO_COMMAND"), QStringLiteral("undefined")};
            std::optional<GuardedScriptGlobalIndex> localGlobals;
            if (!sharedGlobals) { localGlobals.emplace(GuardedScriptGlobalIndex::prepare(engine, names, ledger)); }
            const auto & globals = sharedGlobals ? *sharedGlobals : *localGlobals;
            const auto command = globals.find(engine, names[0], ledger);
            const auto undefined = globals.find(engine, names[1], ledger);
            if (command.status != GuardedScriptPropertyStatus::Found || command.prototypeDepth != 0 ||
                undefined.status != GuardedScriptPropertyStatus::Found || undefined.prototypeDepth != 0 ||
                undefined.value.kind() != GuardedScriptValueKind::Undefined)
            {
                return false;
            }
            std::optional<GuardedScriptOwnIndexCache> localMembers;
            if (!members) { localMembers.emplace(engine, ledger); }
            auto & cache = members ? *members : *localMembers;
            const auto active = own(command.value, QStringLiteral("activeHookCo"), cache, ledger);
            return active.status == GuardedScriptPropertyStatus::Found &&
                (active.value.isNull() || active.value.kind() == GuardedScriptValueKind::Undefined);
        }

        bool canonicalApply(QJSEngine & engine, QJSEngine & stockRealm, const QV4::JavaScriptFunctionObject & captured,
                            const GuardedScriptValueView & value, RefinementLedger & ledger)
        {
            auto* proto = engine.handle()->functionPrototype();
            if (captured.engine() != engine.handle() || captured.d()->prototype() != proto->d() ||
                guardedScriptBuiltinProperty(value, QStringLiteral("apply"), ledger).status != GuardedScriptPropertyStatus::Missing)
            {
                return false;
            }
            const auto method = [&](QJSEngine & realm) {
                const auto object = GuardedCommandFunctionAccess::managed(
                    QV4::Value::fromHeapObject(realm.handle()->functionPrototype()->d()));
                const auto slot = guardedScriptBuiltinProperty(object, QStringLiteral("apply"), ledger);
                const auto raw = GuardedCommandFunctionAccess::read(slot.value);
                const auto* builtin = raw.as<QV4::DynamicFunctionObject>();
                if (slot.status != GuardedScriptPropertyStatus::Found || slot.prototypeDepth != 0 ||
                    !builtin || builtin->engine() != realm.handle())
                {
                    return GuardedScriptFunctionImage{};
                }
                return guardedScriptFunctionImage(slot.value, ledger);
            };
            const auto actual = method(engine);
            const auto approved = method(stockRealm);
            return actual.kind == GuardedScriptFunctionKind::Builtin && approved.kind == GuardedScriptFunctionKind::Builtin &&
                actual.sameImplementation(approved);
        }

        bool wrappedRepair(QJSEngine & engine, QJSEngine & stockRealm, const QV4::JavaScriptFunctionObject & script,
                           GuardedScriptValueView & operand, RefinementLedger & ledger, GuardedScriptOwnIndexCache* members, const GuardedScriptGlobalIndex* sharedGlobals)
        {
            if (!supportedRuntime())
            {
                return false;
            }
            auto* native = engine.handle();
            const auto* root = native->rootContext()->d();
            const auto* scope = script.scope();
            if (!root || root->outer || root->activation != native->globalObject->d() || !scope ||
                scope->type != QV4::Heap::ExecutionContext::Type_CallContext || scope->activation ||
                scope->outer != root || scope->nArgs != 0 ||
                scope->vtable() != native->internalClasses(QV4::EngineBase::Class_CallContext)->vtable)
            {
                return false;
            }
            const auto* call = static_cast<const QV4::Heap::CallContext*>(scope);
            if (call->locals.size != 1 || call->locals.alloc != 1 || !call->function)
            {
                return false;
            }
            const auto creating = QV4::Value::fromHeapObject(call->function.get());
            const auto* creator = creating.as<QV4::JavaScriptFunctionObject>();
            const auto* captured = call->locals.values[0].as<QV4::JavaScriptFunctionObject>();
            const LinkedUnit* linked = nullptr;
            // These linked declarations write the single local once and create this one closure.
            if (!creator || creator->scope() != root || !captured || captured->scope() != root ||
                std::strcmp(captured->vtable()->className, "ScriptFunction") != 0 ||
                !matchRecord(engine, &script, CommandDeclaration::REPAIR_WRAPPER, linked, ledger) ||
                !matchRecord(engine, creator, CommandDeclaration::REPAIR_CREATOR, linked, ledger, 1) ||
                call->internalClass.get() != creator->function()->internalClass.get() ||
                !functionAt(*linked, creator->function(), 396) || !functionAt(*linked, script.function(), 397) ||
                !moduleStrings(linked, CommandDeclaration::REPAIR_STRINGS, COMMAND_PATH, ledger) ||
                !clearRepairCo(engine, ledger, members, sharedGlobals))
            {
                return false;
            }
            operand = GuardedCommandFunctionAccess::managed(call->locals.values[0]);
            return canonicalApply(engine, stockRealm, *captured, operand, ledger);
        }

        bool commandWeather(QJSEngine & engine, const QV4::JavaScriptFunctionObject & script,
                            RefinementLedger & ledger, GuardedScriptOwnIndexCache* members, const GuardedScriptGlobalIndex* sharedGlobals)
        {
            auto* native = engine.handle();
            const auto* root = native->rootContext()->d();
            const auto* scope = script.scope();
            const LinkedUnit* linked = nullptr;
            if (!root || root->outer || root->activation != native->globalObject->d() || !scope ||
                scope->type != QV4::Heap::ExecutionContext::Type_CallContext || scope->activation || scope->outer != root ||
                scope->nArgs != 3 || scope->vtable() != native->internalClasses(QV4::EngineBase::Class_CallContext)->vtable ||
                !matchRecord(engine, &script, CommandDeclaration::WEATHER_WRAPPER, linked, ledger) ||
                !functionAt(*linked, script.function(), 375))
            {
                return false;
            }
            const auto* call = static_cast<const QV4::Heap::CallContext*>(scope);
            if (call->locals.size != 1 || call->locals.alloc != 4 || !call->function || !call->locals.values[0].isNull())
            {
                return false;
            }
            const auto creating = QV4::Value::fromHeapObject(call->function.get());
            const auto* creator = creating.as<QV4::JavaScriptFunctionObject>();
            if (!creator || creator->scope() != root ||
                !std::ranges::any_of(CommandDeclaration::WEATHER_CREATORS, [&](const auto & record) {
                    return matchRecord(engine, creator, record, linked, ledger, 1);
                }) ||
                call->internalClass.get() != creator->function()->internalClass.get() || !functionAt(*linked, creator->function(), 374) ||
                !moduleStrings(linked, CommandDeclaration::WEATHER_STRINGS, COMMAND_PATH, ledger) ||
                !storedString(*linked, creator->function()->compiledFunction->formalsTable()[2].nameIndex, "allowMissing") ||
                !storedInteger(*linked, 2, 1) || !storedInteger(*linked, 7, 0) ||
                !literalShape(*linked, *native, 21, CommandDeclaration::SHAPE_35, ledger) ||
                !literalShape(*linked, *native, 22, CommandDeclaration::SHAPE_35, ledger))
            {
                return false;
            }
            const std::array names{QStringLiteral("CO_COMMAND"), QStringLiteral("undefined")};
            std::optional<GuardedScriptGlobalIndex> localGlobals;
            if (!sharedGlobals) { localGlobals.emplace(GuardedScriptGlobalIndex::prepare(engine, names, ledger)); }
            const auto & globals = sharedGlobals ? *sharedGlobals : *localGlobals;
            const auto command = globals.find(engine, names[0], ledger);
            const auto undefined = globals.find(engine, names[1], ledger);
            if (command.status != GuardedScriptPropertyStatus::Found || command.prototypeDepth != 0 ||
                undefined.status != GuardedScriptPropertyStatus::Found || undefined.prototypeDepth != 0 ||
                undefined.value.kind() != GuardedScriptValueKind::Undefined)
            {
                return false;
            }
            std::optional<GuardedScriptOwnIndexCache> localMembers;
            if (!members) { localMembers.emplace(engine, ledger); }
            auto & cache = members ? *members : *localMembers;
            for (const auto & fn : CommandDeclaration::WEATHER_HELPERS)
            {
                const auto member = own(command.value, QString(latin(fn.name)), cache, ledger);
                if (member.status != GuardedScriptPropertyStatus::Found || !matchFunction(engine, member.value, fn, linked, ledger))
                {
                    return false;
                }
            }
            const auto text = [&](const QString & key, QLatin1StringView expected) {
                const auto p = own(command.value, key, cache, ledger);
                const auto s = guardedScriptScalar(p.value, ledger);
                const auto* value = std::get_if<QString>(&s.value);
                return p.status == GuardedScriptPropertyStatus::Found &&
                    s.status == GuardedScriptScalarStatus::Verified && value && *value == expected;
            };
            const auto pending = own(command.value, QStringLiteral("pendingBuild"), cache, ledger);
            const auto lineage = own(command.value, QStringLiteral("LINEAGE_1"), cache, ledger);
            return pending.status == GuardedScriptPropertyStatus::Found && pending.value.isNull() &&
                lineage.status == GuardedScriptPropertyStatus::Found && lineage.value.number() == 0 &&
                text(QStringLiteral("UNIT_OWNER_KEY"), GUARDED_COMMAND_UNIT_OWNER) &&
                text(QStringLiteral("UNIT_LINEAGE_KEY"), GUARDED_COMMAND_UNIT_LINEAGE);
        }


    }
    using namespace GuardedCommandDetail;
#endif

    GuardedNativeBindingStatus authenticateCommandTerrainDefinition(
        QJSEngine & engine, const GuardedScriptValueView & value, GuardedCommandTerrainDefinition definition,
        RefinementLedger & ledger)
    {
        constexpr std::int64_t TERRAIN_DEFINITION_CONTROL_WORK = 131072;
        if (!ledger.consume(TERRAIN_DEFINITION_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        if (definition != GuardedCommandTerrainDefinition::Aw2ForestDefense ||
            engine.thread() != QThread::currentThread() || !supportedRuntime())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        // Pinned AW2 terrain source 196aaf4180d730c72bad2a07dc4ff16a0efdf39dd3462c24c11e4779ce64a475.
        constexpr CommandDeclaration::Function record{
            "getDefense", 79, "f23369546fce95de32f644170652bea57a70d98c4fa03560dfc80e195ded74a3"};
        constexpr std::array strings{
            CommandDeclaration::String{0, ""}, CommandDeclaration::String{1, "__BASEFOREST"},
            CommandDeclaration::String{2, "getDefense"}, CommandDeclaration::String{4, "%entry"},
            CommandDeclaration::String{5, "terrain"}};
        const auto raw = GuardedCommandFunctionAccess::read(value);
        const auto* script = raw.as<QV4::JavaScriptFunctionObject>();
        const LinkedUnit* linked = nullptr;
        const auto before = ledger.stats().refusals;
        if (script && matchFunction(engine, value, record, linked, ledger) &&
            functionAt(*linked, script->function(), 1) &&
            moduleStrings(linked, strings, QLatin1StringView("/mods/AW2 terrain & weather/scripts/terrain/__BASEFOREST.js"), ledger))
        {
            return GuardedNativeBindingStatus::Bound;
        }
        if (ledger.stats().refusals != before) { return GuardedNativeBindingStatus::BudgetExhausted; }
#else
        Q_UNUSED(engine)
        Q_UNUSED(value)
        Q_UNUSED(definition)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedNativeBindingStatus authenticateCommandUnitDefinition(
        QJSEngine & engine, const GuardedScriptValueView & value, GuardedCommandUnitDefinition definition,
        RefinementLedger & ledger, GuardedScriptOwnIndexCache* members, const GuardedScriptGlobalIndex* sharedGlobals)
    {
        // Covers runtime/reader setup, the bounded function-index lookup and six native/stored integer pairs.
        if (!ledger.consume(131072, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        if (engine.thread() != QThread::currentThread() || !supportedRuntime())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto raw = GuardedCommandFunctionAccess::read(value);
        const auto* script = raw.as<QV4::JavaScriptFunctionObject>();
        if (!script || script->engine() != engine.handle())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto before = ledger.stats().refusals;
        if (definition == GuardedCommandUnitDefinition::MovementPostAction) { return GuardedNativeBindingStatus::Unsupported; }
        if (definition == GuardedCommandUnitDefinition::CommandWeather)
        {
            if (commandWeather(engine, *script, ledger, members, sharedGlobals)) { return GuardedNativeBindingStatus::Bound; }
            return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }
        if (script->scope() != engine.handle()->rootContext()->d())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        struct Definition
        {
            GuardedCommandUnitDefinition kind;
            const CommandDeclaration::Function & record;
            std::span<const CommandDeclaration::String> strings;
            std::span<const std::int32_t> integers;
            QLatin1StringView path;
            std::uint32_t index;
        };
        using namespace CommandDeclaration;
        const std::array definitions{
            Definition{GuardedCommandUnitDefinition::InfantryInit, INFANTRY_INIT, INFANTRY_INIT_STRINGS, INFANTRY_INIT_INTEGERS,
                       QLatin1StringView("/mods/awds_unit/scripts/units/infantry.js"), 1},
            Definition{GuardedCommandUnitDefinition::InfantryCost, INFANTRY_COST, INFANTRY_COST_STRINGS, {},
                       QLatin1StringView("/mods/awds_unit/scripts/units/infantry.js"), 2},
            Definition{GuardedCommandUnitDefinition::LightTankInit, LIGHT_TANK_INIT, LIGHT_TANK_INIT_STRINGS, LIGHT_TANK_INIT_INTEGERS,
                       QLatin1StringView("/mods/awds_unit/scripts/units/light_tank.js"), 1},
            Definition{GuardedCommandUnitDefinition::LightTankCost, LIGHT_TANK_COST, LIGHT_TANK_COST_STRINGS, {},
                       QLatin1StringView("/mods/awds_unit/scripts/units/light_tank.js"), 2}};
        const auto expected = std::ranges::find(definitions, definition, &Definition::kind);
        const LinkedUnit* linked = nullptr;
        if (expected != definitions.end() &&
            matchFunction(engine, value, expected->record, linked, ledger) &&
            functionAt(*linked, script->function(), expected->index) &&
            moduleStrings(linked, expected->strings, expected->path, ledger) &&
            (expected->integers.empty() || initializerIntegers(*linked, expected->integers)))
        {
            return GuardedNativeBindingStatus::Bound;
        }
        if (ledger.stats().refusals != before)
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(value)
        Q_UNUSED(definition)
        Q_UNUSED(members)
        Q_UNUSED(sharedGlobals)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }

    GuardedCommandOperand guardedCommandRepairOperand(QJSEngine & engine, QJSEngine & stockRealm,
        const GuardedScriptValueView & value, RefinementLedger & ledger, GuardedScriptOwnIndexCache* members, const GuardedScriptGlobalIndex* sharedGlobals)
    {
        if (!ledger.consume(1024, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}, false};
        }
#ifdef SCRIPTSOURCESUPPORT
        if (&engine == &stockRealm || engine.thread() != QThread::currentThread() || stockRealm.thread() != engine.thread())
        {
            return {};
        }
        const auto raw = GuardedCommandFunctionAccess::read(value);
        const auto* object = raw.as<QV4::Object>();
        if (object && object->engine() != engine.handle())
        {
            return {};
        }
        const auto* script = raw.as<QV4::JavaScriptFunctionObject>();
        if (!script || script->scope() == engine.handle()->rootContext()->d())
        {
            return {GuardedNativeBindingStatus::Bound, value, false};
        }
        const auto before = ledger.stats().refusals;
        GuardedScriptValueView delegate;
        if (ledger.consume(131072, RefinementWork::MasterPreparation) &&
            wrappedRepair(engine, stockRealm, *script, delegate, ledger, members, sharedGlobals))
        {
            return {GuardedNativeBindingStatus::Bound, delegate, true};
        }
        if (ledger.stats().refusals != before)
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}, false};
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(stockRealm)
        Q_UNUSED(value)
        Q_UNUSED(members)
        Q_UNUSED(sharedGlobals)
#endif
        return {};
    }

    GuardedNativeBindingStatus authenticateGuardedCommandFunctions(QJSEngine & engine, RefinementLedger & ledger, GuardedScriptOwnIndexCache* sharedMembers, const GuardedScriptGlobalIndex* sharedGlobals)
    {
        if (!ledger.consume(4096, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#ifdef SCRIPTSOURCESUPPORT
        if (!supportedRuntime())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto refusals = ledger.stats().refusals;
        const std::array names{QStringLiteral("CMODE_CO_COMMAND"), QStringLiteral("CO_COMMAND"), QStringLiteral("undefined")};
        std::optional<GuardedScriptGlobalIndex> localGlobals;
        if (!sharedGlobals) { localGlobals.emplace(GuardedScriptGlobalIndex::prepare(engine, names, ledger)); }
        const auto & globals = sharedGlobals ? *sharedGlobals : *localGlobals;
        std::optional<GuardedScriptOwnIndexCache> localMembers;
        if (!sharedMembers) { localMembers.emplace(engine, ledger); }
        auto & members = sharedMembers ? *sharedMembers : *localMembers;
        const auto rule = globals.find(engine, names[0], ledger);
        const auto command = globals.find(engine, names[1], ledger);
        const auto undefined = globals.find(engine, names[2], ledger);
        if (rule.status == GuardedScriptPropertyStatus::Found && rule.prototypeDepth == 0 &&
            command.status == GuardedScriptPropertyStatus::Found && command.prototypeDepth == 0 &&
            undefined.status == GuardedScriptPropertyStatus::Found && undefined.prototypeDepth == 0 &&
            undefined.value.kind() == GuardedScriptValueKind::Undefined &&
            verifyModule(engine, rule.value, members, CommandDeclaration::RULE_FUNCTIONS, CommandDeclaration::RULE_STRINGS, RULE_PATH, false, ledger) &&
            verifyModule(engine, command.value, members, CommandDeclaration::COMMAND_FUNCTIONS, CommandDeclaration::COMMAND_STRINGS, COMMAND_PATH, true, ledger) &&
            commandConstants(command.value, rule.value, members, ledger))
        {
            return GuardedNativeBindingStatus::Bound;
        }
        if (ledger.stats().refusals != refusals)
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(sharedMembers)
        Q_UNUSED(sharedGlobals)
#endif
        return GuardedNativeBindingStatus::Unsupported;
    }
}
