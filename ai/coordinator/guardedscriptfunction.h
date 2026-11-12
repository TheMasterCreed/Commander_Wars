#pragma once

#include <QByteArray>
#include <QJSValue>
#include <QString>

#include <optional>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ai/coordinator/refinementledger.h"

class QJSEngine;
class QObject;
struct QMetaObject;

namespace Coordinator
{
    enum class GuardedScriptFunctionKind : std::int8_t
    {
        Unavailable,
        Script,
        Builtin,
    };

    enum class GuardedScriptFunctionStatus : std::int8_t
    {
        Verified,
        Unsupported,
        BudgetExhausted,
    };

    struct GuardedScriptFunctionImage
    {
        GuardedScriptFunctionStatus status{GuardedScriptFunctionStatus::Unsupported};
        GuardedScriptFunctionKind kind{GuardedScriptFunctionKind::Unavailable};
        QByteArray implementation;
        QString compilationName;
        std::uint32_t functionOffset{0};
        std::int64_t chargedWork{0};

        bool sameImplementation(const GuardedScriptFunctionImage &) const;
    };

    struct GuardedScriptImageCacheData;

    // One synchronous collection owns immutable compilation units, never live bindings.
    class GuardedScriptImageCache final
    {
    public:
        GuardedScriptImageCache(QJSEngine &, RefinementLedger &);
        ~GuardedScriptImageCache();
        GuardedScriptImageCache(const GuardedScriptImageCache &) = delete;
        GuardedScriptImageCache & operator=(const GuardedScriptImageCache &) = delete;
        GuardedScriptFunctionStatus status() const;

    private:
        friend struct GuardedScriptImageCacheAccess;
        std::unique_ptr<GuardedScriptImageCacheData> m_data;
        GuardedScriptFunctionStatus m_status{GuardedScriptFunctionStatus::BudgetExhausted};
    };

    enum class GuardedScriptValueKind : std::int8_t
    {
        Unavailable,
        Undefined,
        Null,
        Boolean,
        Number,
        String,
        Managed,
    };

    // Borrows the source's rooted value; no script, collection or rebinding may intervene.
    class GuardedScriptValueView final
    {
    public:
        GuardedScriptValueKind kind() const;
        std::optional<double> number() const;
        bool isNull() const;

    private:
        friend struct GuardedScriptObservationAccess;
        friend struct GuardedCommandFunctionAccess;
        std::uint64_t m_encoded{0};
        GuardedScriptValueKind m_kind{GuardedScriptValueKind::Unavailable};
    };

    enum class GuardedScriptPropertyStatus : std::int8_t
    {
        Found,
        Missing,
        Unsupported,
        BudgetExhausted,
    };

    struct GuardedScriptProperty
    {
        GuardedScriptPropertyStatus status{GuardedScriptPropertyStatus::Unsupported};
        GuardedScriptValueView value;
        std::int64_t chargedWork{0};
        std::uint32_t prototypeDepth{0};
    };

    struct GuardedScriptOwnIndexData;

    // Complete own-shape indexes never cache values or whole-chain absence.
    class GuardedScriptOwnIndexCache final
    {
    public:
        GuardedScriptOwnIndexCache(QJSEngine &, RefinementLedger &);
        ~GuardedScriptOwnIndexCache();
        GuardedScriptOwnIndexCache(const GuardedScriptOwnIndexCache &) = delete;
        GuardedScriptOwnIndexCache & operator=(const GuardedScriptOwnIndexCache &) = delete;
        GuardedScriptPropertyStatus status() const;

    private:
        friend struct GuardedScriptOwnIndexAccess;
        std::unique_ptr<GuardedScriptOwnIndexData> m_data;
        GuardedScriptPropertyStatus m_status{GuardedScriptPropertyStatus::BudgetExhausted};
    };

    inline constexpr std::size_t GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES = 64;

    // Index lifetime is one synchronous observation; values are read again at lookup.
    class GuardedScriptGlobalIndex final
    {
    public:
        GuardedScriptGlobalIndex() = default;
        static GuardedScriptGlobalIndex prepare(QJSEngine &, std::span<const QString>, RefinementLedger &);
        GuardedScriptPropertyStatus status() const;
        GuardedScriptProperty find(QJSEngine &, const QString &, RefinementLedger &) const;

    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        GuardedScriptPropertyStatus m_status{GuardedScriptPropertyStatus::Unsupported};
    };

    constexpr std::uint32_t GUARDED_SCRIPT_MAX_ARRAY_LENGTH = 256;

    struct GuardedScriptArrayInspection
    {
        GuardedScriptPropertyStatus status{GuardedScriptPropertyStatus::Unsupported};
        std::uint32_t length{0};
        std::int64_t chargedWork{0};
    };

    struct GuardedScriptAliasInspection
    {
        GuardedScriptPropertyStatus status{GuardedScriptPropertyStatus::Unsupported};
        std::int64_t chargedWork{0};
    };

    enum class GuardedScriptIntrinsicPrototype : std::int8_t
    {
        Array,
        String,
        Number,
    };

    enum class GuardedScriptScalarStatus : std::int8_t
    {
        Verified,
        Unsupported,
        BudgetExhausted,
    };

    struct GuardedScriptScalar
    {
        GuardedScriptScalarStatus status{GuardedScriptScalarStatus::Unsupported};
        GuardedScriptValueKind kind{GuardedScriptValueKind::Unavailable};
        std::variant<std::monostate, bool, double, QString> value;
        std::int64_t chargedWork{0};
    };

    enum class GuardedScriptContextOperands : std::int8_t
    {
        GlobalScopeOnly,
        ScopedOrDynamicOperands,
        Unsupported,
        BudgetExhausted,
    };

    struct GuardedScriptContextInspection
    {
        GuardedScriptContextOperands status{GuardedScriptContextOperands::Unsupported};
        std::int64_t chargedWork{0};
        std::uint32_t instructions{0};
        std::uint32_t scopes{0};
        std::uint32_t bytecodeBytes{0};
    };

    struct GuardedNativeMethodRequirement
    {
        std::string_view name;
        std::int32_t arguments{0};
    };

    template<class Left, class Right>
    constexpr bool guardedNativeMethodPrecedes(const Left & left, const Right & right)
    {
        if (left.name.size() != right.name.size()) { return left.name.size() < right.name.size(); }
        if (left.arguments != right.arguments) { return left.arguments < right.arguments; }
        return left.name < right.name;
    }

    struct GuardedNativeMethodUnion
    {
        const QMetaObject* metadata{nullptr};
        std::span<const GuardedNativeMethodRequirement> methods;
    };

    enum class GuardedNativeReceiverStatus : std::int8_t
    {
        Bound,
        Unsupported,
        BudgetExhausted,
    };

    // The receiver remains borrowed from the rooted, synchronous engine observation.
    struct GuardedNativeReceiverObservation
    {
        GuardedNativeReceiverStatus status{GuardedNativeReceiverStatus::Unsupported};
        const QObject* receiver{nullptr};
        std::int64_t chargedWork{0};
    };

    // This checks compile-owned native metadata, separately from JavaScript receiver properties.
    GuardedNativeReceiverStatus guardedNativeNameAbsent(const QMetaObject &, std::string_view, RefinementLedger &);

    class GuardedNativeMethodContract final
    {
    public:
        GuardedNativeMethodContract() = default;
        GuardedNativeReceiverStatus status() const;
        std::size_t size() const;
        // Compile-owned metadata binds native dispatch, not the method's gameplay effects.
        static GuardedNativeMethodContract prepare(
            const QMetaObject &, std::span<const GuardedNativeMethodRequirement>, RefinementLedger &);
        GuardedNativeReceiverStatus authenticate(
            QJSEngine &, const QJSValue &, const QObject &, RefinementLedger &) const;
        GuardedNativeReceiverStatus authenticate(
            QJSEngine &, const GuardedScriptValueView &, const QObject &, RefinementLedger &) const;
        // The caller must separately bind the native getter and its exact returned QObject.
        GuardedNativeReceiverStatus authenticateReturned(
            QJSEngine &, const QObject &, RefinementLedger &) const;
        std::optional<std::int64_t> returnedWork() const;

    private:
        friend class GuardedNativeMethodCache;
        GuardedNativeReceiverStatus canonicalOrder(RefinementLedger &) const;
        GuardedNativeMethodContract project(std::span<const GuardedNativeMethodRequirement>, RefinementLedger &) const;
        struct Data;
        std::shared_ptr<const Data> m_data;
        GuardedNativeReceiverStatus m_status{GuardedNativeReceiverStatus::Unsupported};
    };

    // One source owns paid descriptions of compile-owned static metadata, never receiver observations.
    class GuardedNativeMethodCache final
    {
    public:
        // Union declarations must be immutable and outlive the source-owned cache.
        explicit GuardedNativeMethodCache(
            RefinementLedger & ledger, std::span<const GuardedNativeMethodUnion> unions = {})
            : m_ledger(ledger), m_unions(unions) {}
        GuardedNativeMethodCache(const GuardedNativeMethodCache &) = delete;
        GuardedNativeMethodCache & operator=(const GuardedNativeMethodCache &) = delete;
        GuardedNativeMethodContract prepare(
            const QMetaObject &, std::span<const GuardedNativeMethodRequirement>, RefinementLedger &);

    private:
        RefinementLedger & m_ledger;
        std::vector<GuardedNativeMethodContract> m_contracts;
        std::span<const GuardedNativeMethodUnion> m_unions;
        std::vector<GuardedNativeMethodContract> m_descriptions;
    };

    struct GuardedNativeValueConstructorAgreement
    {
        GuardedNativeReceiverStatus status{GuardedNativeReceiverStatus::Unsupported};
        GuardedNativeMethodContract methods;
        std::int64_t chargedWork{0};
    };

    struct GuardedScriptPointValue
    {
        GuardedNativeReceiverStatus status{GuardedNativeReceiverStatus::Unsupported};
        double x{0};
        double y{0};
        std::int64_t chargedWork{0};
    };

    GuardedScriptFunctionImage guardedScriptFunctionImage(
        const QJSValue &, RefinementLedger &, GuardedScriptImageCache* cache = nullptr);
    GuardedScriptFunctionImage guardedScriptFunctionImage(
        const GuardedScriptValueView &, RefinementLedger &, GuardedScriptImageCache* cache = nullptr);
    GuardedScriptProperty guardedScriptProperty(
        const QJSValue &, const QString &, RefinementLedger &, GuardedScriptOwnIndexCache* cache = nullptr);
    GuardedScriptProperty guardedScriptProperty(
        const GuardedScriptValueView &, const QString &, RefinementLedger &, GuardedScriptOwnIndexCache* cache = nullptr);
    GuardedScriptProperty guardedScriptGlobalProperty(QJSEngine &, const QString &, RefinementLedger &);
    GuardedScriptAliasInspection guardedScriptGlobalAlias(
        QJSEngine &, const GuardedScriptValueView &, RefinementLedger &);
    GuardedNativeReceiverObservation guardedScriptNativeReceiver(
        QJSEngine &, const GuardedScriptValueView &, const QMetaObject &, RefinementLedger &);
    GuardedNativeValueConstructorAgreement guardedScriptQtValueConstructors(QJSEngine &, RefinementLedger &);
    GuardedScriptPointValue guardedScriptPointValue(
        QJSEngine &, const GuardedScriptValueView &, RefinementLedger &);
    GuardedScriptProperty guardedScriptIntrinsicPrototype(
        QJSEngine &, GuardedScriptIntrinsicPrototype, RefinementLedger &);
    GuardedScriptProperty guardedScriptBuiltinProperty(
        const GuardedScriptValueView &, const QString &, RefinementLedger &);
    GuardedScriptArrayInspection guardedScriptArray(const GuardedScriptValueView &, RefinementLedger &);
    GuardedScriptAliasInspection guardedScriptIndexedArrayData(
        const GuardedScriptValueView &, RefinementLedger &);
    GuardedScriptProperty guardedScriptArrayElement(
        const GuardedScriptValueView &, std::uint32_t index, RefinementLedger &);
    // The caller supplies a live native receiver and its compile-owned static metadata.
    GuardedScriptProperty guardedScriptQObjectProperty(
        const GuardedScriptValueView &, const QObject &, const QMetaObject &,
        const QString &, RefinementLedger &, GuardedScriptOwnIndexCache* cache = nullptr);
    GuardedScriptScalar guardedScriptScalar(const GuardedScriptValueView &, RefinementLedger &);
    GuardedScriptContextInspection guardedScriptContextOperands(const QJSValue &, RefinementLedger &);
    GuardedScriptContextInspection guardedScriptContextOperands(const GuardedScriptValueView &, RefinementLedger &);
}
