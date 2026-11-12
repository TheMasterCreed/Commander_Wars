#pragma once

#include <algorithm>
#include <array>

#include "ai/coordinator/guardedscriptfunction.h"

namespace Coordinator::NativeMethodRequirements
{
    namespace Stock
    {
        inline constexpr std::array MAP_METHODS{
            GuardedNativeMethodRequirement{"getMapWidth", 0},
            GuardedNativeMethodRequirement{"getMapHeight", 0},
            GuardedNativeMethodRequirement{"getTerrain", 2},
            GuardedNativeMethodRequirement{"getTerrain", 1},
            GuardedNativeMethodRequirement{"getPlayer", 1},
            GuardedNativeMethodRequirement{"getPlayerCount", 0},
            GuardedNativeMethodRequirement{"getCurrentPlayer", 0},
            GuardedNativeMethodRequirement{"getCurrentDay", 0},
            GuardedNativeMethodRequirement{"getGameRules", 0},
            GuardedNativeMethodRequirement{"getUnit", 1},
            GuardedNativeMethodRequirement{"getCurrentViewPlayer", 0},
            GuardedNativeMethodRequirement{"getImageSize", 0},
            GuardedNativeMethodRequirement{"getGameScript", 0},
        };
        inline constexpr std::array UNIT_METHODS{
            GuardedNativeMethodRequirement{"getHp", 0},
            GuardedNativeMethodRequirement{"setHp", 1},
            GuardedNativeMethodRequirement{"getOwner", 0},
            GuardedNativeMethodRequirement{"getUnitID", 0},
            GuardedNativeMethodRequirement{"getTerrain", 0},
            GuardedNativeMethodRequirement{"getFuel", 0},
            GuardedNativeMethodRequirement{"getAmmo1", 0},
            GuardedNativeMethodRequirement{"getAmmo2", 0},
            GuardedNativeMethodRequirement{"getHasMoved", 0},
            GuardedNativeMethodRequirement{"getCapturePoints", 0},
            GuardedNativeMethodRequirement{"setCapturePoints", 1},
            GuardedNativeMethodRequirement{"getCaptureRate", 1},
            GuardedNativeMethodRequirement{"getX", 0},
            GuardedNativeMethodRequirement{"getY", 0},
            GuardedNativeMethodRequirement{"getPosition", 0},
            GuardedNativeMethodRequirement{"getHpRounded", 0},
            GuardedNativeMethodRequirement{"getUnitType", 0},
            GuardedNativeMethodRequirement{"getUnitCosts", 0},
            GuardedNativeMethodRequirement{"getCosts", 0},
            GuardedNativeMethodRequirement{"getBaseCosts", 0},
            GuardedNativeMethodRequirement{"getRepairCostModifier", 0},
            GuardedNativeMethodRequirement{"getRepairBonus", 1},
            GuardedNativeMethodRequirement{"canBeRepaired", 1},
            GuardedNativeMethodRequirement{"refill", 4},
            GuardedNativeMethodRequirement{"isStealthed", 1},
            GuardedNativeMethodRequirement{"isStealthed", 2},
            GuardedNativeMethodRequirement{"isStealthed", 4},
            GuardedNativeMethodRequirement{"getMovementpoints", 1},
            GuardedNativeMethodRequirement{"getMovementCosts", 5},
            GuardedNativeMethodRequirement{"getMovementFuelCostModifier", 1},
            GuardedNativeMethodRequirement{"moveUnitAction", 1},
            GuardedNativeMethodRequirement{"moveUnitToField", 2},
            GuardedNativeMethodRequirement{"setHasMoved", 1},
            GuardedNativeMethodRequirement{"setFuel", 1},
            GuardedNativeMethodRequirement{"getMaxFuel", 0},
            GuardedNativeMethodRequirement{"getMaxAmmo1", 0},
            GuardedNativeMethodRequirement{"getMaxAmmo2", 0},
            GuardedNativeMethodRequirement{"reduceAmmo1", 1},
            GuardedNativeMethodRequirement{"reduceAmmo2", 1},
            GuardedNativeMethodRequirement{"getTypeOfWeapon1", 0},
            GuardedNativeMethodRequirement{"getTypeOfWeapon2", 0},
            GuardedNativeMethodRequirement{"getWeapon1ID", 0},
            GuardedNativeMethodRequirement{"getWeapon2ID", 0},
            GuardedNativeMethodRequirement{"hasAmmo1", 0},
            GuardedNativeMethodRequirement{"hasAmmo2", 0},
            GuardedNativeMethodRequirement{"getUnitDamageID", 0},
            GuardedNativeMethodRequirement{"getUnitDamage", 1},
            GuardedNativeMethodRequirement{"getMinRange", 1},
            GuardedNativeMethodRequirement{"getMaxRangeAtPosition", 1},
            GuardedNativeMethodRequirement{"getBonusMinRange", 1},
            GuardedNativeMethodRequirement{"getBonusMaxRange", 1},
            GuardedNativeMethodRequirement{"isAttackable", 4},
            GuardedNativeMethodRequirement{"canAttackWithWeapon", 6},
            GuardedNativeMethodRequirement{"canCounterAttack", 5},
            GuardedNativeMethodRequirement{"getFirstStrike", 4},
            GuardedNativeMethodRequirement{"getBonusOffensive", 6},
            GuardedNativeMethodRequirement{"getUnitBonusOffensive", 6},
            GuardedNativeMethodRequirement{"getBonusDefensive", 6},
            GuardedNativeMethodRequirement{"getUnitBonusDefensive", 6},
            GuardedNativeMethodRequirement{"getAttackHpBonus", 1},
            GuardedNativeMethodRequirement{"getBonusLuck", 1},
            GuardedNativeMethodRequirement{"getBonusMisfortune", 1},
            GuardedNativeMethodRequirement{"getTrueDamage", 8},
            GuardedNativeMethodRequirement{"getDamageReduction", 8},
            GuardedNativeMethodRequirement{"getTerrainDefense", 0},
            GuardedNativeMethodRequirement{"getTerrainDefense", 2},
            GuardedNativeMethodRequirement{"getTerrainDefenseModifier", 1},
            GuardedNativeMethodRequirement{"useTerrainDefense", 0},
            GuardedNativeMethodRequirement{"getUnitRank", 0},
            GuardedNativeMethodRequirement{"getVirtualHp", 0},
            GuardedNativeMethodRequirement{"getVirtualHpValue", 0},
            GuardedNativeMethodRequirement{"setVirtualHpValue", 1},
            GuardedNativeMethodRequirement{"postBattleActions", 5},
            GuardedNativeMethodRequirement{"postAction", 1},
            GuardedNativeMethodRequirement{"increaseCapturePoints", 1},
            GuardedNativeMethodRequirement{"loadSpriteV2", 2},
            GuardedNativeMethodRequirement{"loadSpriteV2", 3},
            GuardedNativeMethodRequirement{"loadSpriteV2", 4},
            GuardedNativeMethodRequirement{"unloadIcon", 1},
        };
        inline constexpr std::array PLAYER_METHODS{
            GuardedNativeMethodRequirement{"getMap", 0},
            GuardedNativeMethodRequirement{"getPlayerID", 0},
            GuardedNativeMethodRequirement{"getArmy", 0},
            GuardedNativeMethodRequirement{"getFunds", 0},
            GuardedNativeMethodRequirement{"addFunds", 1},
            GuardedNativeMethodRequirement{"getFundsModifier", 0},
            GuardedNativeMethodRequirement{"getBuildList", 0},
            GuardedNativeMethodRequirement{"getCO", 1},
            GuardedNativeMethodRequirement{"getIsDefeated", 0},
            GuardedNativeMethodRequirement{"getTeam", 0},
            GuardedNativeMethodRequirement{"isEnemy", 1},
            GuardedNativeMethodRequirement{"getBuildingCount", 1},
            GuardedNativeMethodRequirement{"getUnitCount", 1},
            GuardedNativeMethodRequirement{"getCostModifier", 3},
            GuardedNativeMethodRequirement{"getIncomeReduction", 2},
            GuardedNativeMethodRequirement{"getRepairCostModifier", 1},
            GuardedNativeMethodRequirement{"gainPowerstar", 5},
            GuardedNativeMethodRequirement{"postBattleActions", 6},
            GuardedNativeMethodRequirement{"postAction", 1},
            GuardedNativeMethodRequirement{"getBaseGameInput", 0},
        };
        inline constexpr std::array PROPERTY_METHODS{
            GuardedNativeMethodRequirement{"getOwner", 0},
            GuardedNativeMethodRequirement{"getOwnerID", 0},
            GuardedNativeMethodRequirement{"getBuildingID", 0},
            GuardedNativeMethodRequirement{"getTerrain", 0},
            GuardedNativeMethodRequirement{"getBaseIncome", 0},
            GuardedNativeMethodRequirement{"getIncome", 0},
            GuardedNativeMethodRequirement{"getConstructionList", 0},
            GuardedNativeMethodRequirement{"getRepairTypes", 0},
            GuardedNativeMethodRequirement{"canRepair", 2},
            GuardedNativeMethodRequirement{"getCostModifier", 3},
            GuardedNativeMethodRequirement{"getOffensiveBonus", 0},
            GuardedNativeMethodRequirement{"getDefensiveBonus", 0},
            GuardedNativeMethodRequirement{"setUnitOwner", 1},
            GuardedNativeMethodRequirement{"loadSpriteV2", 2},
            GuardedNativeMethodRequirement{"loadSpriteV2", 3},
            GuardedNativeMethodRequirement{"loadSpriteV2", 4},
            GuardedNativeMethodRequirement{"loadSpriteV2", 5},
            GuardedNativeMethodRequirement{"loadSpriteV2", 6},
        };
        inline constexpr std::array RULE_METHODS{
            GuardedNativeMethodRequirement{"getParallelCos", 0},
            GuardedNativeMethodRequirement{"getEnableDayToDayCoAbilities", 0},
            GuardedNativeMethodRequirement{"getCoGlobalD2D", 0},
            GuardedNativeMethodRequirement{"getCoUnits", 0},
            GuardedNativeMethodRequirement{"getNoPower", 0},
            GuardedNativeMethodRequirement{"getPowerGainMode", 0},
            GuardedNativeMethodRequirement{"getPowerGainZone", 0},
            GuardedNativeMethodRequirement{"getPowerGainSpeed", 0},
            GuardedNativeMethodRequirement{"getPowerUsageReduction", 0},
            GuardedNativeMethodRequirement{"getDamageFormula", 0},
            GuardedNativeMethodRequirement{"getTerrainDefense", 0},
            GuardedNativeMethodRequirement{"getHpDefenseReduction", 0},
            GuardedNativeMethodRequirement{"getCurrentWeather", 0},
            GuardedNativeMethodRequirement{"getRankingSystem", 0},
            GuardedNativeMethodRequirement{"getFogMode", 0},
            GuardedNativeMethodRequirement{"getGameRule", 1},
        };
    }

    namespace Command
    {
        inline constexpr std::array MAP_METHODS{
            GuardedNativeMethodRequirement{"getPlayerCount", 0}, GuardedNativeMethodRequirement{"getPlayer", 1},
            GuardedNativeMethodRequirement{"getCurrentPlayer", 0}, GuardedNativeMethodRequirement{"getGameRules", 0}};
        inline constexpr std::array PLAYER_METHODS{
            GuardedNativeMethodRequirement{"getCO", 1}, GuardedNativeMethodRequirement{"getUnitCount", 1},
            GuardedNativeMethodRequirement{"getPlayerID", 0}};
        inline constexpr std::array RULES_METHODS{
            GuardedNativeMethodRequirement{"hasVictoryRule", 1}, GuardedNativeMethodRequirement{"getVictoryRule", 1}};
        inline constexpr std::array UNIT_GETTER{GuardedNativeMethodRequirement{"getMap", 0}};
        inline constexpr std::array UNIT_INITIALIZER{
            UNIT_GETTER[0], GuardedNativeMethodRequirement{"setAmmo1", 1}, GuardedNativeMethodRequirement{"setMaxAmmo1", 1},
            GuardedNativeMethodRequirement{"setWeapon1ID", 1}, GuardedNativeMethodRequirement{"setAmmo2", 1},
            GuardedNativeMethodRequirement{"setMaxAmmo2", 1}, GuardedNativeMethodRequirement{"setWeapon2ID", 1},
            GuardedNativeMethodRequirement{"setFuel", 1}, GuardedNativeMethodRequirement{"setMaxFuel", 1},
            GuardedNativeMethodRequirement{"setBaseMovementPoints", 1}, GuardedNativeMethodRequirement{"setMinRange", 1},
            GuardedNativeMethodRequirement{"setMaxRange", 1}, GuardedNativeMethodRequirement{"setVision", 1}};
        inline constexpr std::array PLAYER_MOVEMENT{
            GuardedNativeMethodRequirement{"isEnemyUnit", 1}, GuardedNativeMethodRequirement{"getVariables", 0}};
        inline constexpr std::array UNIT_ADDITIONS{
            GuardedNativeMethodRequirement{"getOwner", 0},
            GuardedNativeMethodRequirement{"getVariables", 0},
            GuardedNativeMethodRequirement{"getHp", 0},
            GuardedNativeMethodRequirement{"isStealthed", 1},
            GuardedNativeMethodRequirement{"getIgnoreUnitCollision", 0}};
    }

    namespace Actions
    {
        inline constexpr std::array MAP_METHODS{
            GuardedNativeMethodRequirement{"getPlayerCount", 0}, GuardedNativeMethodRequirement{"getPlayer", 1},
            GuardedNativeMethodRequirement{"getCurrentPlayer", 0}, GuardedNativeMethodRequirement{"getGameRules", 0},
            GuardedNativeMethodRequirement{"onMap", 2}};
        inline constexpr std::array RULES_METHODS{
            GuardedNativeMethodRequirement{"getCoGlobalD2D", 0}, GuardedNativeMethodRequirement{"hasVictoryRule", 1},
            GuardedNativeMethodRequirement{"getVictoryRule", 1}};
        inline constexpr std::array PLAYER_METHODS{
            GuardedNativeMethodRequirement{"getMap", 0}, GuardedNativeMethodRequirement{"getPlayerID", 0},
            GuardedNativeMethodRequirement{"getCO", 1}, GuardedNativeMethodRequirement{"getVariables", 0},
            GuardedNativeMethodRequirement{"isEnemy", 1}};
        inline constexpr std::array UNIT_METHODS{
            GuardedNativeMethodRequirement{"getMap", 0}, GuardedNativeMethodRequirement{"getOwner", 0},
            GuardedNativeMethodRequirement{"getUnitID", 0}, GuardedNativeMethodRequirement{"getUniqueID", 0},
            GuardedNativeMethodRequirement{"getVariables", 0}};
        inline constexpr std::array BUILDING_METHODS{
            GuardedNativeMethodRequirement{"getOwner", 0}, GuardedNativeMethodRequirement{"getVariables", 0}};
        inline constexpr std::array BUILDING_SPRITE_METHODS{
            BUILDING_METHODS[0], BUILDING_METHODS[1], GuardedNativeMethodRequirement{"getNeutralLoaded", 0}};
    }

    template<std::size_t Capacity>
    struct MethodUnion
    {
        std::array<GuardedNativeMethodRequirement, Capacity> values{};
        std::size_t count{0};

        constexpr std::span<const GuardedNativeMethodRequirement> methods() const
        {
            return {values.data(), count};
        }
    };

    template<std::size_t... Sizes>
    consteval auto join(const std::array<GuardedNativeMethodRequirement, Sizes> &... groups)
    {
        MethodUnion<(Sizes + ...)> result;
        const auto append = [&](const auto & group) {
            for (const auto & method : group)
            {
                bool present = false;
                for (std::size_t index = 0; index < result.count; ++index)
                {
                    present = present || (result.values[index].name == method.name &&
                        result.values[index].arguments == method.arguments);
                }
                if (!present) { result.values[result.count++] = method; }
            }
        };
        (append(groups), ...);
        std::sort(result.values.begin(), result.values.begin() + result.count,
            [](const auto & left, const auto & right) { return guardedNativeMethodPrecedes(left, right); });
        return result;
    }

    inline constexpr auto MAP = join(Stock::MAP_METHODS, Command::MAP_METHODS, Actions::MAP_METHODS);
    inline constexpr auto UNIT = join(Stock::UNIT_METHODS, Command::UNIT_GETTER, Command::UNIT_INITIALIZER,
        Command::UNIT_ADDITIONS, Actions::UNIT_METHODS);
    inline constexpr auto PLAYER = join(Stock::PLAYER_METHODS, Command::PLAYER_METHODS,
        Command::PLAYER_MOVEMENT, Actions::PLAYER_METHODS);
    inline constexpr auto BUILDING = join(Stock::PROPERTY_METHODS, Actions::BUILDING_METHODS, Actions::BUILDING_SPRITE_METHODS);
    inline constexpr auto RULES = join(Stock::RULE_METHODS, Command::RULES_METHODS, Actions::RULES_METHODS);
}
