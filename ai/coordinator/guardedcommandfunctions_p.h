#pragma once

#ifdef SCRIPTSOURCESUPPORT
#include "ai/coordinator/guardedcommanddeclarations.h"
#include "ai/coordinator/guardedscriptfunction.h"
#include <private/qv4context_p.h>
#include <private/qv4executablecompilationunit_p.h>
#include <private/qv4functionobject_p.h>

namespace Coordinator
{
    struct GuardedCommandFunctionAccess
    {
        static QV4::Value read(const GuardedScriptValueView & view)
        {
            if (view.m_kind != GuardedScriptValueKind::Managed)
            {
                return QV4::Value::undefinedValue();
            }
            return QV4::Value::fromReturnedValue(view.m_encoded);
        }

        static GuardedScriptValueView nullValue()
        {
            GuardedScriptValueView view;
            view.m_kind = GuardedScriptValueKind::Null;
            view.m_encoded = QV4::Value::nullValue().asReturnedValue();
            return view;
        }

        static GuardedScriptValueView managed(const QV4::Value & raw)
        {
            GuardedScriptValueView view;
            if (raw.as<QV4::Object>())
            {
                view.m_kind = GuardedScriptValueKind::Managed;
                view.m_encoded = raw.asReturnedValue();
            }
            return view;
        }
    };

    namespace GuardedCommandDetail
    {
        using LinkedUnit = QV4::ExecutableCompilationUnit;
        bool supportedRuntime();
        QLatin1StringView latin(std::string_view text);
        bool matchRecord(QJSEngine & engine, const QV4::JavaScriptFunctionObject* script,
                         const CommandDeclaration::Function & declaration, const LinkedUnit*& linked,
                         RefinementLedger & ledger, std::uint32_t expectedLocals = 0);
        bool matchFunction(QJSEngine & engine, const GuardedScriptValueView & value,
                           const CommandDeclaration::Function & declaration,
                           const LinkedUnit*& linked, RefinementLedger & ledger);
        GuardedScriptProperty own(const GuardedScriptValueView & object, const QString & name,
                                  GuardedScriptOwnIndexCache & cache, RefinementLedger & ledger);
        bool moduleStrings(const LinkedUnit* linked, std::span<const CommandDeclaration::String> strings,
                           QLatin1StringView path, RefinementLedger & ledger);
        bool functionAt(const LinkedUnit & linked, const QV4::Function* function, std::uint32_t index);
        const QV4::Heap::CallContext* closureContext(
            QJSEngine &, const QV4::JavaScriptFunctionObject &,
            std::span<const CommandDeclaration::Function>, std::uint32_t creatorIndex,
            std::uint32_t locals, std::uint32_t arguments, const LinkedUnit*&, RefinementLedger &);
        bool descriptorSlot(const QV4::Object &, QLatin1StringView, QV4::PropertyAttributes &, QV4::Value &, RefinementLedger &);
        GuardedScriptFunctionImage builtinMember(
            QJSEngine &, const QV4::Object &, QLatin1StringView, bool getter, RefinementLedger &);
        bool literalShape(const LinkedUnit &, QV4::ExecutionEngine &, std::uint32_t,
                          std::span<const std::uint32_t>, RefinementLedger &);
        bool storedString(const LinkedUnit &, std::uint32_t, std::string_view);
        bool storedString(const LinkedUnit &, std::uint32_t, std::string_view, RefinementLedger &);
        bool storedConstant(const LinkedUnit &, std::uint32_t, std::uint64_t);
        bool catchBlock(const LinkedUnit &, QV4::ExecutionEngine &, std::uint32_t, std::uint32_t, RefinementLedger &);
        bool storedInteger(const LinkedUnit & linked, std::uint32_t index, int expected);
    }
}
#endif
