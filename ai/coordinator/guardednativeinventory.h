#pragma once

#include <array>
#include <limits>
#include <optional>
#include <vector>

#include <QString>

#include "ai/coordinator/guardedstockrelation.h"
#include "ai/coordinator/masterassignmentstate.h"
#include "game/GameEnums.h"

class GameMap;

namespace Coordinator
{
    constexpr std::int32_t GUARDED_NATIVE_MAX_WIDTH = 16;
    constexpr std::int32_t GUARDED_NATIVE_MAX_HEIGHT = 15;
    constexpr qsizetype GUARDED_NATIVE_MAX_ID_LENGTH = 128;
    constexpr std::size_t GUARDED_NATIVE_MAX_PATH_POINTS = GUARDED_STOCK_MAX_TILES;
    constexpr std::int64_t GUARDED_NATIVE_HEADER_WORK = 256;
    constexpr std::size_t GUARDED_NATIVE_MAX_EFFECTIVE_PERKS = 16;
    constexpr std::int32_t GUARDED_NATIVE_MAX_RESOURCE = 1000;
    constexpr std::int32_t GUARDED_NATIVE_MAX_MOVEMENT = GUARDED_STOCK_MAX_MOVEMENT;
    constexpr std::int32_t GUARDED_NATIVE_UNRANKED = 0;
    constexpr std::int32_t GUARDED_NATIVE_MAX_POWER_STARS = 32;
    constexpr std::int32_t GUARDED_NATIVE_MAX_POWER_USES = 1000;
    constexpr double GUARDED_NATIVE_MAX_POWER_FACTOR = 1000;
    constexpr std::int64_t GUARDED_NATIVE_CO_STATE_WORK = 256;
    constexpr std::int64_t GUARDED_NATIVE_CO_RULE_WORK = 1024;
    constexpr std::size_t GUARDED_NATIVE_MAX_BUILD_LIST = 512;
    constexpr std::size_t GUARDED_NATIVE_MAX_GAME_RULES = 64;
    constexpr std::int64_t GUARDED_NATIVE_BUILD_ENTRY_WORK = 64 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH;
    constexpr std::int64_t GUARDED_NATIVE_ECONOMY_FACT_WORK =
        2 * (256 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH +
             GUARDED_NATIVE_BUILD_ENTRY_WORK * GUARDED_NATIVE_MAX_BUILD_LIST) +
        GUARDED_NATIVE_BUILD_ENTRY_WORK * GUARDED_NATIVE_MAX_GAME_RULES;
    constexpr std::size_t GUARDED_NATIVE_MAX_TERRAIN_DEPTH = 8;
    constexpr std::int32_t GUARDED_NATIVE_MAX_TERRAIN_HP = 1000;
    constexpr std::int64_t GUARDED_NATIVE_TERRAIN_LAYER_WORK = 256 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH;
    constexpr std::int64_t GUARDED_NATIVE_TERRAIN_TILE_WORK =
        64 + 16 * GUARDED_NATIVE_MAX_TERRAIN_DEPTH * GUARDED_NATIVE_MAX_TERRAIN_DEPTH +
        GUARDED_NATIVE_MAX_TERRAIN_DEPTH * GUARDED_NATIVE_TERRAIN_LAYER_WORK;
    constexpr std::int64_t GUARDED_NATIVE_COMBAT_RULE_WORK = 256;
    constexpr std::int32_t GUARDED_NATIVE_MAX_TERRAIN_DEFENSE =
        std::numeric_limits<std::uint8_t>::max();

    enum class GuardedNativeBindingStatus : std::int8_t
    {
        Bound,
        Unsupported,
        Invalid,
        BudgetExhausted,
    };

    struct GuardedNativeActorMaterial
    {
        std::int32_t maxFuel{0};
        std::int32_t maxAmmo1{0};
        std::int32_t maxAmmo2{0};
        std::int32_t baseMovement{0};
        std::int32_t minRange{0};
        std::int32_t maxRange{0};
        std::int32_t rank{0};
        QString movementType;
        QString weapon1;
        QString weapon2;
        std::uint8_t moddingFlags{0};
        bool collisionOverride{false};
        bool temporaryBonuses{false};
        bool multiTurnPath{false};
        bool virtualHp{false};

        bool operator==(const GuardedNativeActorMaterial &) const = default;
    };

    struct GuardedNativeActor
    {
        std::int32_t id{NO_UNIT};
        std::int32_t owner{NO_OWNER};
        std::int32_t tile{-1};
        QString type;
        double actualHp{0};
        std::int32_t progress{0};
        std::int32_t fuel{0};
        std::int32_t ammo1{0};
        std::int32_t ammo2{0};
        bool moved{false};
        bool hidden{false};
        bool cloaked{false};
        std::optional<GuardedNativeActorMaterial> material;

        bool operator==(const GuardedNativeActor &) const = default;
    };

    struct GuardedNativeCO
    {
        QString id;
        std::int32_t powerMode{0};
        std::int32_t unitId{NO_UNIT};
        bool rangeEnabled{false};
        std::vector<QString> effectivePerks;
        bool globalZone{false};
        std::int32_t powerStars{0};
        std::int32_t superpowerStars{0};
        std::int32_t powerUsed{0};
        double powerFilled{0};
        bool isCo0{false};

        bool operator==(const GuardedNativeCO &) const = default;
    };

    enum class GuardedNativePowerGainMode : std::int32_t
    {
        Money = GameEnums::PowerGainMode_Money,
        MoneyOnlyAttacker = GameEnums::PowerGainMode_Money_OnlyAttacker,
        HitPoints = GameEnums::PowerGainMode_Hp,
        HitPointsOnlyAttacker = GameEnums::PowerGainMode_Hp_OnlyAttacker,
    };

    enum class GuardedNativePowerGainZone : std::int32_t
    {
        Global = GameEnums::PowerGainZone_Global,
        GlobalCoZoneBonus = GameEnums::PowerGainZone_GlobalCoZoneBonus,
        OnlyCoZone = GameEnums::PowerGainZone_OnlyCoZone,
    };

    struct GuardedNativeCoRules
    {
        bool parallel{false};
        bool dayToDay{false};
        bool globalDayToDay{false};
        bool coUnits{false};
        bool noPower{false};
        double gainSpeed{0};
        double usageReduction{0};
        GuardedNativePowerGainMode gainMode{GuardedNativePowerGainMode::Money};
        GuardedNativePowerGainZone gainZone{GuardedNativePowerGainZone::Global};

        bool operator==(const GuardedNativeCoRules &) const = default;
    };

    struct GuardedNativePlayerMaterial
    {
        double fundsModifier{0};
        std::array<std::optional<GuardedNativeCO>, 2> cos;
        QString army;
        std::vector<QString> buildList;
        std::optional<std::int32_t> defaultArmyPlayerId;

        bool operator==(const GuardedNativePlayerMaterial &) const = default;
    };

    enum class GuardedNativeDamageFormula : std::int32_t
    {
        AdvanceWars4 = GameEnums::DamageFormula_AdvanceWars4,
        AdvanceWars1To3 = GameEnums::DamageFormula_AdvanceWars1_3,
    };

    struct GuardedNativeCombatRules
    {
        GuardedNativeDamageFormula formula{GuardedNativeDamageFormula::AdvanceWars4};
        std::int32_t terrainDefense{0};
        bool hpDefenseReduction{false};

        bool operator==(const GuardedNativeCombatRules &) const = default;
    };

    struct GuardedNativeProperty
    {
        std::int32_t tile{-1};
        std::int32_t owner{NO_OWNER};
        QString type;

        bool operator==(const GuardedNativeProperty &) const = default;
    };

    struct GuardedNativeTerrainLayer
    {
        QString type;
        std::int32_t hp{-1};
        bool startOfTurn{false};
        bool flowDirection{false};

        bool operator==(const GuardedNativeTerrainLayer &) const = default;
    };

    struct GuardedNativeTerrain
    {
        std::int32_t tile{-1};
        std::vector<GuardedNativeTerrainLayer> layers;

        bool operator==(const GuardedNativeTerrain &) const = default;
    };

    // Native inventory only, not script-domain or post-action proof authority.
    struct GuardedNativeInventory
    {
        std::int32_t width{0};
        std::int32_t height{0};
        std::int32_t day{0};
        std::int32_t player{NO_OWNER};
        std::array<std::int64_t, 2> funds{};
        std::array<std::int32_t, 2> teams{};
        std::array<bool, 2> defeated{};
        std::vector<GuardedNativeActor> actors;
        std::vector<GuardedNativeProperty> properties;
        std::optional<std::array<GuardedNativePlayerMaterial, 2>> playerMaterial;
        std::optional<bool> mapScriptLoaded;
        std::optional<GuardedNativeCoRules> coRules;
        std::optional<std::vector<QString>> gameRuleIds;
        std::optional<std::vector<GuardedNativeTerrain>> terrains;
        std::optional<GuardedNativeCombatRules> combatRules;

        bool operator==(const GuardedNativeInventory &) const = default;
    };

    struct GuardedNativeInventoryResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Invalid};
        std::optional<GuardedNativeInventory> inventory;
    };

    struct GuardedNativeAction
    {
        std::size_t actor{0};
        std::int32_t actionIndex{NO_ACTION};
        PlanBundleKind kind{PlanBundleKind::Wait};
        QString actionId;
        std::vector<TilePoint> path;
        TilePoint target{INVALID_TILE};
        std::int32_t targetUnitId{NO_UNIT};

        bool operator==(const GuardedNativeAction &) const = default;
    };

    struct GuardedNativePlanBinding
    {
        MasterAssignmentIdentity identity;
        std::vector<GuardedNativeAction> actions;
    };

    struct GuardedNativePlanResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Invalid};
        std::optional<GuardedNativePlanBinding> plan;
    };

    struct GuardedNativePairBinding
    {
        GuardedNativePlanBinding incoming;
        GuardedNativePlanBinding challenger;
        std::size_t changedAction{0};
        std::size_t property{0};
        std::optional<std::size_t> commonFire;
    };

    struct GuardedNativePairResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Invalid};
        std::optional<GuardedNativePairBinding> pair;
    };

    std::optional<std::int64_t> guardedNativeInventoryWork(std::int32_t width, std::int32_t height);
    // Worst-case ceiling; admission prepays its actual owned-shape inspection.
    std::optional<std::int64_t> guardedNativeMaterialWork(std::size_t actors);
    std::optional<std::int64_t> guardedNativePairWork(std::size_t actors);

    // Every bound action reuses a candidate path on this inventory, so these cap one plan's walk.
    struct GuardedNativePlanBounds
    {
        std::size_t actors{0};
        std::size_t tiles{0};
        std::size_t properties{0};
        std::size_t pathPoints{0};
    };
    std::optional<GuardedNativePlanBounds> guardedNativePlanBounds(
        const MasterAssignmentProblem &, const GuardedNativeInventory &, RefinementLedger &);
    std::optional<std::int64_t> guardedNativePlanWork(const GuardedNativePlanBounds &);
    GuardedNativeInventoryResult captureGuardedNativeInventory(GameMap &, RefinementLedger &);
    GuardedNativeBindingStatus admitGuardedNativeMaterials(
        const GuardedNativeInventory &, RefinementLedger &);
    GuardedNativePlanResult bindGuardedNativePlan(
        const MasterAssignmentProblem &, const GuardedNativeInventory &, const GuardedNativePlanBounds &,
        const MasterAssignmentState &, const MasterAssignmentReplayResult &, RefinementLedger &);
    GuardedNativePairResult bindGuardedNativePair(
        const MasterAssignmentProblem &, const GuardedNativeInventory &,
        const MasterAssignmentState & incoming, const MasterAssignmentReplayResult & incomingReplay,
        const MasterAssignmentState & challenger, const MasterAssignmentReplayResult & challengerReplay,
        RefinementLedger &);
}
