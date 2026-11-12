#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardednativeinventory.h"
#include "ai/coordinator/guardedscriptfunction.h"

namespace Coordinator
{
    inline constexpr std::string_view GUARDED_NATIVE_CONSTRUCTION_LIST_MEMBER{"constructionList"};

    enum class GuardedNativeCodeKind : std::int8_t
    {
        None,
        Unit,
        Infantry,
        Sniper,
        Recon,
        LightTank,
        Action,
        Fire,
        Wait,
        Capture,
        Weapon,
        InfantryMachineGun,
        SniperWeapon,
        Co,
        Andy,
        Jess,
        TagPower,
        Trap,
        NextPlayer,
        Building,
        Town,
        Factory,
        Headquarters,
        MovementTable,
        MoveFeet,
        MoveTireA,
        MoveTank,
        Terrain,
        Player,
        BaseForest,
        BaseMountain,
        BaseStreet,
        Plains,
        Forest,
        Mountain,
        Street,
        Sea,
        Reaf,
        Weather,
        Sun,
        VictoryRule,
        NoHeadquarters,
        NoUnits,
        Ranking,
        Global,
        RepairTank,
        TankHunter,
        BattleAnimation,
        InfantryBattleAnimation,
        SniperBattleAnimation,
        Count,
    };

    struct GuardedNativeCodeAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedScriptFunctionImage actual;
        GuardedScriptFunctionImage approved;
        GuardedNativeCodeKind ownerKind{GuardedNativeCodeKind::None};
    };

    struct GuardedNativeCodeLookup
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedNativeCodeKind kind{GuardedNativeCodeKind::None};
    };

    struct GuardedNativeEnumAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::vector<std::int32_t> values;
    };
    using GuardedNativeUnitTypeAgreement = GuardedNativeEnumAgreement;

    struct GuardedNativeFunctionAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedNativeCodeAgreement code;
        GuardedScriptContextInspection context;
    };

    struct GuardedNativeScalarAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedScriptScalar actual;
        GuardedScriptScalar approved;
    };

    enum class GuardedNativeDataKind : std::int8_t
    {
        Scalar,
        Array,
        Point,
    };

    struct GuardedNativeDataNode
    {
        GuardedNativeDataKind kind{GuardedNativeDataKind::Scalar};
        std::uint32_t elements{0};
        GuardedScriptScalar scalar;
        std::array<double, 2> point{};
    };

    struct GuardedNativeArrayAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::vector<GuardedNativeDataNode> actual;
        std::vector<GuardedNativeDataNode> approved;
        std::int64_t chargedWork{0};
    };

    struct GuardedNativeIntegerTableResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::optional<std::int32_t> value;
    };

    struct GuardedNativeMovementField
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::optional<std::array<std::int32_t, GUARDED_STOCK_MAX_TILES>> costs;
    };

    struct GuardedNativeNamedArray
    {
        QString name;
        GuardedNativeArrayAgreement evidence;
    };

    struct GuardedNativeTableAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedNativeCodeKind kind{GuardedNativeCodeKind::None};
        std::vector<GuardedNativeNamedArray> tables;
        std::int64_t chargedWork{0};
    };

    // Covers canonical weather keys and canonical fixed row indices only.
    struct GuardedNativeWeatherPrefixAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::int64_t chargedWork{0};
    };

    struct GuardedNativeArmyTableLookup
    {
        QString name;
        std::array<QString, 2> results;
    };

    struct GuardedNativeArmyTableAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::array<QString, 2> armies;
        std::vector<GuardedNativeArmyTableLookup> tables;
        std::int64_t chargedWork{0};
    };

    enum class GuardedNativeForwarder : std::int8_t
    {
        None,
        CommandRepair,
        WardenTrap,
        CommandWait,
        CommandCaptureCan,
        CommandCapture,
        CommandCapturePost,
        CommandNextCan,
        CommandNext,
        CommandPlayerStart,
        CommandPlayerEnd,
        CommandArmy,
        CommandTownSprites,
        CommandTownWeather,
        CommandFactorySprites,
        CommandFactoryWeather,
        CommandHeadquartersSprites,
        CommandHeadquartersWeather,
        CommandRanking,
    };

    struct GuardedNativeMemberAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        QString name;
        std::variant<std::monostate, GuardedNativeFunctionAgreement, GuardedNativeScalarAgreement, GuardedCommandUnitDefinition, GuardedCommandTerrainDefinition, GuardedCommandRankingBonus> evidence;
        GuardedNativeForwarder forwarder{GuardedNativeForwarder::None};
    };

    struct GuardedNativeModuleAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedNativeCodeKind kind{GuardedNativeCodeKind::None};
        std::vector<GuardedNativeMemberAgreement> members;
        std::int64_t chargedWork{0};
    };

    struct GuardedNativeConstructionAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        QString type;
        GuardedNativeMemberAgreement callback;
        bool coSpecific{false};
        std::int64_t chargedWork{0};
    };

    struct GuardedNativeConstructionType
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        QString type;
    };

    struct GuardedNativeAbsentHook
    {
        QString name;
        bool explicitUndefined{false};
    };

    struct GuardedNativeAbsentHooks
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedNativeCodeKind kind{GuardedNativeCodeKind::None};
        std::vector<GuardedNativeAbsentHook> members;
        std::int64_t chargedWork{0};
    };

    enum class GuardedNativeConstantGetter : std::int8_t
    {
        UnitCost,
        PropertyIncome,
    };

    struct GuardedNativeConstantLaw
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::int32_t value{0};
        GuardedNativeMemberAgreement callback;
        std::int64_t chargedWork{0};
    };

    enum class GuardedNativeIntrinsic : std::int8_t
    {
        MathAbs,
        MathCeil,
        MathFloor,
        MathMax,
        MathMin,
        MathRound,
        ArrayIndexOf,
        StringToLowerCase,
        ArrayIncludes,
        NumberToString,
    };

    struct GuardedNativeIntrinsicEntry
    {
        GuardedNativeIntrinsic kind{GuardedNativeIntrinsic::MathAbs};
        GuardedScriptFunctionImage actual;
        GuardedScriptFunctionImage approved;
    };

    struct GuardedNativeIntrinsicAgreement
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::vector<GuardedNativeIntrinsicEntry> entries;
        std::int64_t chargedWork{0};
    };

    using GuardedNativeUnitCodeAgreement = GuardedNativeModuleAgreement;

    // Code agreement alone does not authenticate scope, dependency data or domain semantics.
    class GuardedNativeCodeCatalog final
    {
    public:
        class Observation final
        {
        public:
            ~Observation();
            Observation(const Observation &) = delete;
            Observation & operator=(const Observation &) = delete;
            GuardedNativeBindingStatus status() const;

        private:
            friend class GuardedNativeCodeCatalog;
            Observation(GuardedNativeCodeCatalog &, QJSEngine &, std::span<const QString>);
            GuardedNativeCodeCatalog* m_catalog{nullptr};
            GuardedNativeBindingStatus m_status{GuardedNativeBindingStatus::Unsupported};
        };

        explicit GuardedNativeCodeCatalog(RefinementLedger &);
        ~GuardedNativeCodeCatalog();
        GuardedNativeCodeCatalog(const GuardedNativeCodeCatalog &) = delete;
        GuardedNativeCodeCatalog & operator=(const GuardedNativeCodeCatalog &) = delete;

        Observation observe(QJSEngine &, std::span<const QString> additionalGlobals = {});
        GuardedScriptProperty globalProperty(QJSEngine &, const QString &);
        // Only paid private-realm operations, never native input work, are eligible for exclusion.
        std::optional<std::int64_t> expectedPreparationWork() const;
        std::size_t expectedModuleCompilations() const;
        GuardedNativeBindingStatus prepareConstructionQueries(QJSEngine &, std::span<const QString>);
        GuardedNativeConstructionType constructionType(GuardedNativeCodeKind);
        GuardedNativeCodeLookup kindForGlobal(const QString &);
        GuardedNativeCodeAgreement match(
            const GuardedScriptValueView &, GuardedNativeCodeKind, const QString & member);
        GuardedNativeFunctionAgreement authenticateFunction(
            const GuardedScriptValueView &, GuardedNativeCodeKind, const QString & member);
        GuardedNativeScalarAgreement matchScalar(
            const GuardedScriptValueView &, GuardedNativeCodeKind, std::span<const QString> path);
        GuardedNativeArrayAgreement matchArray(
            const GuardedScriptValueView &, GuardedNativeCodeKind, std::span<const QString> path);
        GuardedNativeArrayAgreement matchArray(
            QJSEngine &, const GuardedScriptValueView &, GuardedNativeCodeKind, std::span<const QString> path);
        GuardedNativeMemberAgreement authenticateMember(
            const GuardedScriptValueView &, GuardedNativeCodeKind, const QString & member);
        GuardedNativeUnitCodeAgreement authenticateUnitCallbacks(QJSEngine & engine, GuardedNativeCodeKind kind)
        {
            return authenticateUnitCallbacks(engine, kind, false);
        }
        GuardedNativeUnitCodeAgreement authenticateUnitCallbacks(QJSEngine &, GuardedNativeCodeKind, bool commandPolicy);
        GuardedNativeModuleAgreement authenticateStockUnitHelpers(QJSEngine &, bool commandPolicy = false);
        GuardedNativeArrayAgreement authenticateUnitActions(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeTableAgreement authenticateUnitPresentationTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockCoCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockCoConsequences(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockCoNominal(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockActionCallbacks(QJSEngine & engine, GuardedNativeCodeKind kind)
        {
            return authenticateStockActionCallbacks(engine, kind, false);
        }
        GuardedNativeModuleAgreement authenticateStockActionCallbacks(QJSEngine &, GuardedNativeCodeKind, bool commandPolicy);
        GuardedNativeModuleAgreement authenticateMovementTrapCallbacks(QJSEngine &);
        GuardedNativeArrayAgreement authenticateCaptureBuildings(QJSEngine &);
        GuardedNativeArrayAgreement authenticateCaptureArmy(QJSEngine &);
        GuardedNativeModuleAgreement authenticateStockPropertyCallbacks(QJSEngine & engine, GuardedNativeCodeKind kind)
        {
            return authenticateStockPropertyCallbacks(engine, kind, false);
        }
        GuardedNativeModuleAgreement authenticateStockPropertyCallbacks(QJSEngine &, GuardedNativeCodeKind, bool commandPolicy);
        GuardedNativeTableAgreement authenticateStockPropertyTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeTableAgreement authenticateHeadquartersGameplayTables(QJSEngine &);
        // The caller separately authenticates the player armies and stock lookup helper.
        GuardedNativeArmyTableAgreement authenticateHeadquartersArmyTables(
            QJSEngine &, const std::array<GuardedNativePlayerMaterial, 2> &);
        GuardedNativeBindingStatus recheckHeadquartersArmyTables(
            QJSEngine &, const GuardedNativeArmyTableAgreement &, const std::array<GuardedNativePlayerMaterial, 2> &);
        GuardedNativeModuleAgreement authenticateStockMovementCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeTableAgreement authenticateStockMovementTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateDefaultArmies(QJSEngine &, std::array<GuardedNativePlayerMaterial, 2> &);
        GuardedNativeModuleAgreement authenticateStockContextCallbacks(QJSEngine & engine, GuardedNativeCodeKind kind)
        {
            return authenticateStockContextCallbacks(engine, kind, false);
        }
        GuardedNativeModuleAgreement authenticateStockContextCallbacks(QJSEngine &, GuardedNativeCodeKind, bool commandPolicy);
        GuardedNativeWeatherPrefixAgreement authenticateStockWeatherPrefix(QJSEngine &);
        GuardedNativeBindingStatus recheckStockWeatherPrefix(QJSEngine &, const GuardedNativeWeatherPrefixAgreement &);
        GuardedNativeTableAgreement authenticateStockContextTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockTerrainCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateCommandTerrainCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateStockPolicyCallbacks(QJSEngine & engine, GuardedNativeCodeKind kind)
        {
            return authenticateStockPolicyCallbacks(engine, kind, false);
        }
        GuardedNativeModuleAgreement authenticateStockPolicyCallbacks(QJSEngine &, GuardedNativeCodeKind, bool commandPolicy);
        GuardedNativeTableAgreement authenticateStockPolicyTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeConstructionAgreement authenticateConstructionQuery(QJSEngine &, const QString & type);
        GuardedNativeConstructionAgreement authenticateConstructionQuery(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeBindingStatus authenticateCommandCallbackEnvironment(QJSEngine &, bool buildingCallbacks = false);
        GuardedNativeBindingStatus authenticateCommandRankingEnvironment(QJSEngine &, const GuardedNativeInventory &);
        GuardedNativeBindingStatus authenticateCommandPolicyEnvironment(QJSEngine &);
        GuardedNativeIntrinsicAgreement authenticateIntrinsics(QJSEngine &);
        GuardedNativeUnitTypeAgreement authenticateUnitTypes(
            QJSEngine &, const QObject & receiver, const QMetaObject & metadata);
        GuardedNativeEnumAgreement authenticateTerminalEnums(
            QJSEngine &, const QObject & receiver, const QMetaObject & metadata);
        GuardedNativeEnumAgreement authenticateStockContextEnums(
            QJSEngine &, const QObject & receiver, const QMetaObject & metadata);
        GuardedNativeModuleAgreement authenticateCurrentFireCallbacks(QJSEngine &);
        GuardedNativeModuleAgreement authenticateCurrentWeaponCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeTableAgreement authenticateCurrentWeaponTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeAbsentHooks authenticateCurrentAbsentHooks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeModuleAgreement authenticateDetailNoneCallbacks(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeTableAgreement authenticateDetailNoneTables(QJSEngine &, GuardedNativeCodeKind);
        GuardedNativeConstantLaw authenticateConstantLaw(
            QJSEngine &, GuardedNativeCodeKind, GuardedNativeConstantGetter, bool commandPolicy = false);
        GuardedNativeBindingStatus recheckDefaultArmies(QJSEngine &, const GuardedNativeModuleAgreement &,
            const std::array<GuardedNativePlayerMaterial, 2> &, std::array<QString, 2> &);
        GuardedNativeBindingStatus recheckModule(QJSEngine &, const GuardedNativeModuleAgreement &);
        GuardedNativeBindingStatus recheckArray(
            QJSEngine &, GuardedNativeCodeKind, const QString &, const GuardedNativeArrayAgreement &);
        GuardedNativeBindingStatus recheckTables(QJSEngine &, const GuardedNativeTableAgreement &);
        GuardedNativeBindingStatus recheckIntrinsics(QJSEngine &, const GuardedNativeIntrinsicAgreement &);
        GuardedNativeBindingStatus recheckAbsentHooks(QJSEngine &, const GuardedNativeAbsentHooks &);
        GuardedNativeBindingStatus recheckConstructionQuery(
            QJSEngine &, const GuardedNativeConstructionAgreement &);
        GuardedNativeBindingStatus recheckMember(
            QJSEngine &, GuardedNativeCodeKind, const GuardedNativeMemberAgreement &);

    private:
        GuardedNativeBindingStatus prepareCallbackObservation(QJSEngine &);
        GuardedNativeBindingStatus rankingBonus(QJSEngine &, const GuardedScriptValueView &, GuardedCommandRankingBonus);
        GuardedCommandOperand callbackOperand(
            QJSEngine &, const GuardedScriptValueView &, GuardedNativeForwarder);
        GuardedNativeBindingStatus readDefaultArmies(QJSEngine &, const std::array<GuardedNativePlayerMaterial, 2> &,
                                                    std::array<QString, 2> &);
        GuardedNativeBindingStatus prepareModuleCompilation(
            QJSEngine &, GuardedNativeCodeKind, const GuardedScriptValueView &,
            std::span<const std::string_view>, bool commandPolicy = false);
        GuardedScriptProperty moduleProperty(
            QJSEngine &, GuardedNativeCodeKind, const GuardedScriptValueView &, const QString &);
        GuardedScriptProperty constructionProperty(QJSEngine &, const QString &);
        GuardedNativeAbsentHooks authenticateAbsentMembers(
            QJSEngine &, GuardedNativeCodeKind, std::span<const std::string_view>);
        GuardedNativeBindingStatus inspectConstructionQuery(
            QJSEngine &, const QString &, GuardedNativeConstructionAgreement &);
        GuardedNativeModuleAgreement authenticateMembers(
            QJSEngine &, GuardedNativeCodeKind, std::span<const std::string_view>, bool commandPolicy = false);
        GuardedNativeBindingStatus recheckObservedMember(
            QJSEngine &, const GuardedScriptValueView &, const GuardedNativeMemberAgreement &);
        GuardedNativeArrayAgreement authenticateArrayMember(
            QJSEngine &, GuardedNativeCodeKind, const QString &);
        GuardedNativeTableAgreement authenticateTables(
            QJSEngine &, GuardedNativeCodeKind, std::span<const std::string_view>);
        GuardedNativeBindingStatus observeArrayPair(
            const GuardedScriptValueView &, GuardedNativeCodeKind, std::span<const QString>,
            std::vector<GuardedNativeDataNode> &, std::vector<GuardedNativeDataNode> &, QJSEngine*);
        GuardedNativeBindingStatus authenticateArray(
            const GuardedScriptValueView &, GuardedNativeCodeKind, std::span<const QString>,
            GuardedNativeArrayAgreement &, QJSEngine* actualEngine = nullptr);
        struct Data;
        std::unique_ptr<Data> m_data;
        RefinementLedger & m_ledger;
        GuardedNativeBindingStatus m_initializationStatus{GuardedNativeBindingStatus::BudgetExhausted};
        QJSEngine* m_observedEngine{nullptr};
        GuardedScriptGlobalIndex m_globals;
        GuardedScriptGlobalIndex m_constructionGlobals;
        std::optional<GuardedScriptGlobalIndex> m_callbackGlobals;
        std::unique_ptr<GuardedScriptImageCache> m_imageCache;
        std::optional<GuardedScriptOwnIndexCache> m_ownIndex;
    };

    std::size_t guardedNativeDeclarationCount();
    std::int64_t guardedNativeRealmConstructionWork();
    std::int64_t guardedNativeDeclarationLookupWork();
    std::optional<std::int32_t> guardedNativeIntegerData(const GuardedNativeDataNode &);
    GuardedNativeIntegerTableResult guardedNativeIntegerTableValue(
        const GuardedNativeArrayAgreement &, const QString &, RefinementLedger &);
    GuardedNativeMovementField prepareGuardedNativeMovementField(
        const GuardedNativeArrayAgreement &, const GuardedNativeInventory &, RefinementLedger &);
}
