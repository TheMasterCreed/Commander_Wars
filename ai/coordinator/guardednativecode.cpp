#include "ai/coordinator/guardednativecode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <string_view>
#include <tuple>

#include <QCryptographicHash>
#include <QJSEngine>
#include <QUrl>

#include "game/GameEnums.h"

#ifdef SCRIPTSOURCESUPPORT
#include <private/qv4engine_p.h>
#endif

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t REALM_CONSTRUCTION_WORK = 131072;
        constexpr std::int64_t VALUE_HELPER_CONSTRUCTION_WORK = 4096;
        constexpr std::int64_t ASSET_METADATA_WORK = 256;
        constexpr std::int64_t APPROVED_SOURCE_BYTE_WORK = 32;
        constexpr std::int64_t CACHE_CONTROL_WORK = 64;
        constexpr std::int64_t ASSET_INSPECTION_WORK = 16;
        constexpr std::int64_t CACHE_COMPARE_PASSES = 4;
        constexpr std::size_t MAX_CODE_ENTRIES = 512;
        constexpr std::size_t MAX_EXPECTED_PROPERTIES = 256;
        constexpr std::int64_t EXPECTED_PROPERTY_RETENTION_WORK = 512;
        constexpr std::size_t MAX_DECLARED_PARENT_DEPTH = 8;
        constexpr qsizetype MAX_CODE_MEMBER = 128;
        constexpr std::int64_t COMPILER_URI_CHARACTER_WORK = 16;
        constexpr std::int64_t ENUM_NAME_METADATA_WORK = 32;
        constexpr std::int64_t ENUM_NAME_CHARACTER_WORK = 4;
        constexpr std::int64_t ENUM_RECORD_WORK = 64;
        constexpr std::int64_t ABSENT_HOOK_RETENTION_WORK = 128;
        constexpr std::int64_t CONSTANT_LAW_CONTROL_WORK = 256;
        constexpr std::int64_t CONSTANT_LAW_SOURCE_BYTE_WORK = 8;
        constexpr std::size_t MAX_DATA_PATH = 4;
        constexpr std::int64_t SCALAR_PATH_ENTRY_WORK = 32;
        constexpr std::int64_t SCALAR_PATH_CHARACTER_WORK = 8;
        constexpr std::int64_t SCALAR_COMPARISON_WORK = 32;
        constexpr std::int64_t SCALAR_COMPARE_CHARACTER_WORK = 2;
        constexpr std::int64_t FUNCTION_COMPARISON_WORK = 32;
        constexpr std::int64_t FUNCTION_COMPARE_ELEMENT_WORK = 4;
        constexpr std::int64_t MODULE_CALLBACK_CONTROL_WORK = 512;
        constexpr std::int64_t MEMBER_NAME_WORK = 32;
        constexpr std::int64_t MEMBER_NAME_CHARACTER_WORK = 8;
        constexpr std::size_t MAX_PRIMITIVE_DATA_NODES = 1024;
        constexpr std::uint32_t MAX_PRIMITIVE_ARRAY_DEPTH = 8;
        constexpr std::int64_t DATA_NODE_WORK = 64;
        constexpr std::int64_t DATA_RETENTION_WORK = 64;
        constexpr std::size_t INITIAL_DATA_CAPACITY = 8;
        constexpr std::size_t DATA_CAPACITY_GROWTH = 2;
        constexpr std::int64_t DATA_COMPARISON_WORK = 32;
        constexpr std::int64_t INTRINSIC_CONTROL_WORK = 64;
        constexpr std::int64_t INTRINSIC_RECORD_WORK = 256;
        constexpr std::int64_t INTRINSIC_COMPARE_WORK = 32;
        constexpr std::int64_t TABLE_LOOKUP_CONTROL_WORK = 64;
        constexpr std::int64_t TABLE_LOOKUP_NODE_WORK = 64;
        constexpr std::int64_t TABLE_LOOKUP_CHARACTER_WORK = 4;
        constexpr std::int64_t ARMY_TABLE_RETENTION_WORK = 512;
        constexpr std::int64_t ARMY_TEXT_WORK = 2 * TABLE_LOOKUP_CHARACTER_WORK;
        const QString STOCK_ARMY_FALLBACK = QStringLiteral("os");
        constexpr std::int64_t MOVEMENT_FIELD_CONTROL_WORK = 256;
        constexpr std::int64_t MOVEMENT_FIELD_ENTRY_WORK = 64;
        constexpr std::int64_t MOVEMENT_FIELD_STORAGE_PASSES = 4;
        constexpr std::int64_t MOVEMENT_CACHE_CHARACTER_WORK = 2;
        const QString MATH_GLOBAL = QStringLiteral("Math");
        const QString UNIT_ACTION_LIST = QStringLiteral("actionList");
        const QString CAPTURABLE_BUILDINGS = QStringLiteral("capturableBuildings");
        const QString CAPTURE_ARMY_DATA = QStringLiteral("armyData");
        const QString FILE_SCHEME = QStringLiteral("file");
        const QString RESOURCE_SCHEME = QStringLiteral("qrc");
        const QString ENUMS_GLOBAL = QStringLiteral("GameEnums");
        const QString EXPECTED_TRANSLATION_BOOTSTRAP =
            QStringLiteral("function qsTr(text) { return text; }");
        const QString EXPECTED_BOOTSTRAP_NAME = QStringLiteral("guarded-expected-bootstrap.js");
        constexpr std::string_view CO_SPECIFIC_UNIT_MEMBER{"getCOSpecificUnit"};

        struct CachedMovementValue
        {
            const QString* name{nullptr};
            std::int32_t cost{-1};
        };

        struct NativeEnumField
        {
            std::string_view name;
            std::int32_t value;
        };

        constexpr std::array UNIT_TYPE_FIELDS{
            NativeEnumField{"UnitType_Infantry", GameEnums::UnitType_Infantry},
            NativeEnumField{"UnitType_Ground", GameEnums::UnitType_Ground},
            NativeEnumField{"UnitType_Hovercraft", GameEnums::UnitType_Hovercraft},
            NativeEnumField{"UnitType_Air", GameEnums::UnitType_Air},
            NativeEnumField{"UnitType_Naval", GameEnums::UnitType_Naval},
        };
        constexpr std::array TERMINAL_ENUM_FIELDS{
            NativeEnumField{"DefeatType_Alive", GameEnums::DefeatType_Alive},
            NativeEnumField{"DefeatType_Defeated", GameEnums::DefeatType_Defeated},
            NativeEnumField{"DefeatType_ByCurrentPlayer", GameEnums::DefeatType_ByCurrentPlayer},
        };
        constexpr std::array STOCK_CONTEXT_ENUM_FIELDS{
            NativeEnumField{"PowerMode_Off", GameEnums::PowerMode_Off},
            NativeEnumField{"PowerMode_Power", GameEnums::PowerMode_Power},
            NativeEnumField{"PowerMode_Superpower", GameEnums::PowerMode_Superpower},
            NativeEnumField{"PowerMode_Tagpower", GameEnums::PowerMode_Tagpower},
            NativeEnumField{"PowerGainMode_Hp", GameEnums::PowerGainMode_Hp},
            NativeEnumField{"PowerGainMode_Hp_OnlyAttacker", GameEnums::PowerGainMode_Hp_OnlyAttacker},
            NativeEnumField{"PowerGainMode_Money", GameEnums::PowerGainMode_Money},
            NativeEnumField{"PowerGainMode_Money_OnlyAttacker", GameEnums::PowerGainMode_Money_OnlyAttacker},
            NativeEnumField{"PowerGainZone_Global", GameEnums::PowerGainZone_Global},
            NativeEnumField{"PowerGainZone_GlobalCoZoneBonus", GameEnums::PowerGainZone_GlobalCoZoneBonus},
            NativeEnumField{"PowerGainZone_OnlyCoZone", GameEnums::PowerGainZone_OnlyCoZone},
            NativeEnumField{"LuckDamageMode_Off", GameEnums::LuckDamageMode_Off},
            NativeEnumField{"LuckDamageMode_On", GameEnums::LuckDamageMode_On},
            NativeEnumField{"LuckDamageMode_Average", GameEnums::LuckDamageMode_Average},
            NativeEnumField{"LuckDamageMode_Min", GameEnums::LuckDamageMode_Min},
            NativeEnumField{"LuckDamageMode_Max", GameEnums::LuckDamageMode_Max},
            NativeEnumField{"DamageFormula_AdvanceWars4", GameEnums::DamageFormula_AdvanceWars4},
            NativeEnumField{"WeaponType_Both", GameEnums::WeaponType_Both},
            NativeEnumField{"WeaponType_Indirect", GameEnums::WeaponType_Indirect},
            NativeEnumField{"CounterAttackMode_Undefined", GameEnums::CounterAttackMode_Undefined},
            NativeEnumField{"VisionType_Shrouded", GameEnums::VisionType_Shrouded},
            NativeEnumField{"Alliance_Enemy", GameEnums::Alliance_Enemy},
            NativeEnumField{"BuildingTarget_Own", GameEnums::BuildingTarget_Own},
            NativeEnumField{"Directions_Direct", GameEnums::Directions_Direct},
            NativeEnumField{"Directions_Diagnonal", GameEnums::Directions_Diagnonal},
            NativeEnumField{"Directions_North", GameEnums::Directions_North},
            NativeEnumField{"Directions_East", GameEnums::Directions_East},
            NativeEnumField{"Directions_West", GameEnums::Directions_West},
            NativeEnumField{"Directions_NorthEast", GameEnums::Directions_NorthEast},
            NativeEnumField{"Directions_NorthWest", GameEnums::Directions_NorthWest},
            NativeEnumField{"Directions_SouthEast", GameEnums::Directions_SouthEast},
            NativeEnumField{"Directions_SouthWest", GameEnums::Directions_SouthWest},
            NativeEnumField{"AiTurnMode_EndOfDay", GameEnums::AiTurnMode_EndOfDay},
            NativeEnumField{"AiTurnMode_StartOfDay", GameEnums::AiTurnMode_StartOfDay},
            NativeEnumField{"AiTypes_Human", GameEnums::AiTypes_Human},
            NativeEnumField{"AiTypes_MovePlanner", GameEnums::AiTypes_MovePlanner},
            NativeEnumField{"GameRecord_SpecialEvents_HQCaptured", GameEnums::GameRecord_SpecialEvents_HQCaptured},
            NativeEnumField{"GameRecord_SpecialEvents_HQLost", GameEnums::GameRecord_SpecialEvents_HQLost},
            NativeEnumField{"Recoloring_Matrix", GameEnums::Recoloring_Matrix},
            NativeEnumField{"Recoloring_None", GameEnums::Recoloring_None},
        };

        constexpr std::array UNIT_CALLBACK_MEMBERS{
            std::string_view{"init"},
            std::string_view{"getMovementType"},
            std::string_view{"getBaseCost"},
            std::string_view{"getUnitType"},
            std::string_view{"getActions"},
            std::string_view{"moveUnit"},
            std::string_view{"doWalkingAnimation"},
            std::string_view{"canMoveAndFire"},
            std::string_view{"canCounterOnRangeAttacks"},
            std::string_view{"getLoadingPlace"},
            std::string_view{"useTerrainDefense"},
            std::string_view{"repairUnit"},
            std::string_view{"startOfTurn"},
            std::string_view{"endOfTurn"},
            std::string_view{"postBattleActions"},
            std::string_view{"postAction"},
            std::string_view{"getBonusOffensive"},
            std::string_view{"getBonusDefensive"},
            std::string_view{"getCaptureBonus"},
            std::string_view{"getWeatherImmune"},
            std::string_view{"getFirstStrike"},
            std::string_view{"getUnitDamageID"},
            std::string_view{"getUnitDamage"},
            std::string_view{"getTypeOfWeapon1"},
            std::string_view{"getTypeOfWeapon2"},
            std::string_view{"canUseWeapon"},
            CO_SPECIFIC_UNIT_MEMBER,
            std::string_view{"onKilled"},
            std::string_view{"onDeath"},
            std::string_view{"createExplosionAnimation"},
        };
        constexpr auto COMMAND_UNIT_MEMBERS = [] {
            std::array<std::string_view, UNIT_CALLBACK_MEMBERS.size() + 1> members{};
            std::ranges::copy(UNIT_CALLBACK_MEMBERS, members.begin());
            members.back() = "initForMods";
            return members;
        }();
        constexpr std::array STOCK_UNIT_HELPERS{std::string_view{"repairUnit"}};

        constexpr std::array STOCK_CO_CALLBACK_MEMBERS{
            std::string_view{"isActive"},
            std::string_view{"getMovementpointModifier"},
            std::string_view{"getOtherMovementpointModifier"},
            std::string_view{"getMovementcostModifier"},
            std::string_view{"getMovementFuelCostModifier"},
            std::string_view{"getFuelCostModifier"},
            std::string_view{"getCaptureBonus"},
            std::string_view{"consumeCaptureBonus"},
            std::string_view{"getRepairBonus"},
            std::string_view{"getOtherRepairBonus"},
            std::string_view{"canBeRepaired"},
            std::string_view{"getRepairCostModifier"},
            std::string_view{"getEnemyRepairCostModifier"},
            std::string_view{"getCostModifier"},
            std::string_view{"getEnemyCostModifier"},
            std::string_view{"getBonusIncome"},
            std::string_view{"getIncomeReduction"},
            std::string_view{"getActionModifierList"},
            std::string_view{"getEnemyActionModifierList"},
            std::string_view{"getAdditionalBuildingActions"},
            std::string_view{"getCOUnits"},
            std::string_view{"getEnemyCOUnits"},
            std::string_view{"getCOUnitRange"},
            std::string_view{"getHpHidden"},
            std::string_view{"getRankInfoHidden"},
            std::string_view{"getPerfectHpView"},
            std::string_view{"getVisionrangeModifier"},
            std::string_view{"getEnemyVisionBonus"},
            std::string_view{"getPerfectVision"},
            std::string_view{"getWeatherImmune"},
            std::string_view{"startOfTurn"},
            std::string_view{"endOfTurn"},
            std::string_view{"postAction"},
            std::string_view{"onUnitDeath"},
            std::string_view{"onCOUnitLost"},
        };

        constexpr std::array CO_BATTLE_MEMBERS{
            std::string_view{"getFirerangeModifier"},
            std::string_view{"getMinFirerangeModifier"},
            std::string_view{"getEnemyFirerangeModifier"},
            std::string_view{"getAttackHpBonus"},
            std::string_view{"getFirstStrike"},
            std::string_view{"canCounterAttack"},
            std::string_view{"getTerrainDefenseModifier"},
            std::string_view{"getEnemyTerrainDefenseModifier"},
            std::string_view{"getBonusLuck"},
            std::string_view{"getEnemyBonusLuck"},
            std::string_view{"getBonusMisfortune"},
            std::string_view{"getEnemyBonusMisfortune"},
            std::string_view{"getOffensiveBonus"},
            std::string_view{"getOffensiveReduction"},
            std::string_view{"getDeffensiveBonus"},
            std::string_view{"getDeffensiveReduction"},
            std::string_view{"getTrueDamage"},
            std::string_view{"getDamageReduction"},
            std::string_view{"getPowerChargeBonus"},
            std::string_view{"postBattleActions"},
            std::string_view{"gainPowerstar"},
        };

        constexpr std::string_view CO_STAR_COST = "getStarCost";
        constexpr std::array CO_NOMINAL_MEMBERS{CO_STAR_COST};
        constexpr std::array CO_CHARGE_DEPENDENCIES{
            std::string_view{"getStarGain"},
            CO_STAR_COST,
            std::string_view{"starFundsCost"},
            std::string_view{"starHpCost"},
        };
        constexpr std::array ANDY_NORMAL_BATTLE_DATA{
            std::string_view{"d2dCoZoneOffBonus"},
            std::string_view{"d2dCoZoneDefBonus"},
        };
        constexpr std::array JESS_NORMAL_BATTLE_DATA{
            std::string_view{"d2dCoZoneDefBonus"},
            std::string_view{"d2dCoZoneOffBonus"},
            std::string_view{"d2dCoZoneGroundBonus"},
            std::string_view{"d2dCoZoneNavalAirBonus"},
            std::string_view{"d2dCoZoneInfBonus"},
            std::string_view{"d2dGroundBonus"},
            std::string_view{"d2dNavalAirBonus"},
            std::string_view{"d2dInfBonus"},
        };

        template<std::size_t First, std::size_t Second>
        consteval auto joinedMembers(const std::array<std::string_view, First> & first,
                                     const std::array<std::string_view, Second> & second)
        {
            std::array<std::string_view, First + Second> result;
            for (std::size_t index = 0; index < first.size(); ++index)
            {
                result[index] = first[index];
            }
            for (std::size_t index = 0; index < second.size(); ++index)
            {
                result[first.size() + index] = second[index];
            }
            return result;
        }

        constexpr auto CO_CONSEQUENCE_MEMBERS = joinedMembers(CO_BATTLE_MEMBERS, CO_CHARGE_DEPENDENCIES);
        constexpr auto ANDY_CONSEQUENCE_MEMBERS = joinedMembers(CO_BATTLE_MEMBERS, ANDY_NORMAL_BATTLE_DATA);
        constexpr auto JESS_CONSEQUENCE_MEMBERS = joinedMembers(CO_BATTLE_MEMBERS, JESS_NORMAL_BATTLE_DATA);
        constexpr std::array ACTION_LEGALITY_MEMBERS{
            std::string_view{"isEmptyFieldAndHasNotMoved"},
            std::string_view{"getRequiresEmptyField"},
        };
        constexpr std::array STOCK_ACTION_MEMBERS{
            std::string_view{"canBePerformed"},
            std::string_view{"isFinalStep"},
            std::string_view{"perform"},
        };
        constexpr std::array CAPTURE_CONSEQUENCE_MEMBERS{
            std::string_view{"getCapturableBuildings"},
            std::string_view{"performPostAnimation"},
            std::string_view{"postAnimationUnit"},
            std::string_view{"postAnimationBuilding"},
            std::string_view{"postAnimationTarget"},
        };
        constexpr auto CAPTURE_MEMBERS = joinedMembers(STOCK_ACTION_MEMBERS, CAPTURE_CONSEQUENCE_MEMBERS);
        constexpr std::array CURRENT_FIRE_MEMBERS{
            std::string_view{"canBePerformed"},
            std::string_view{"isFinalStep"},
            std::string_view{"calcBattleDamage"},
            std::string_view{"calcBattleDamage2"},
            std::string_view{"calcBattleDamage3"},
            std::string_view{"calcBattleDamage4"},
            std::string_view{"calcAttackerWeaponDamage"},
            std::string_view{"calcAttackerDamage"},
            std::string_view{"calcDefenderWeaponDamage"},
            std::string_view{"calcDefenderDamage"},
            std::string_view{"calcEnviromentDamage"},
            std::string_view{"calcDamage"},
            std::string_view{"getDefaultLuck"},
            std::string_view{"predictSupportDamageReduction"},
            std::string_view{"doSupportDamageReduction"},
            std::string_view{"perform"},
            std::string_view{"applyPostDamageReduction"},
            std::string_view{"performPostAnimation"},
            std::string_view{"battle"},
            std::string_view{"performPostUnitAnimation"},
            std::string_view{"performPostBuildingAnimation"},
            std::string_view{"createOverworldBattleAnimation"},
        };
        constexpr std::array CURRENT_WEAPON_MEMBERS{
            std::string_view{"getBaseDamage"},
            std::string_view{"getDamageFromTable"},
            std::string_view{"getEnviromentDamage"},
            std::string_view{"calculateDamage"},
        };
        constexpr std::array CURRENT_WEAPON_TABLES{std::string_view{"damageTable"}};
        constexpr std::array CURRENT_FIRE_ABSENT_HOOKS{std::string_view{"extendedCanAttackCheck"}};
        constexpr std::array CURRENT_UNIT_ABSENT_HOOKS{
            std::string_view{"predictSupportDamageReduction"},
            std::string_view{"doSupportDamageReduction"},
        };
        constexpr std::array UNIT_COST_MEMBER{std::string_view{"getBaseCost"}};
        constexpr std::array UNIT_PRESENTATION_TABLES{std::string_view{"armyData"}};
        constexpr std::array INFANTRY_PRESENTATION_TABLES{
            std::string_view{"armyData"}, std::string_view{"animationData"}};
        constexpr std::array DETAIL_UNIT_MEMBERS{
            std::string_view{"getTerrainAnimationBase"},
            std::string_view{"getTerrainAnimationForeground"},
            std::string_view{"getTerrainAnimationBackground"},
            std::string_view{"getTerrainAnimationMoveSpeed"},
        };
        constexpr std::array DETAIL_TERRAIN_MEMBERS{
            std::string_view{"getFactoryForeground"},
            std::string_view{"getTerrainAnimationId"},
            std::string_view{"getTerrainBackgroundId"},
        };
        constexpr std::array DETAIL_WEATHER_MEMBERS{
            std::string_view{"getWeatherOverlayId"}, std::string_view{"getWeatherOverlaySpeed"}};
        constexpr auto DETAIL_TERRAIN_DISPATCH = joinedMembers(DETAIL_UNIT_MEMBERS, DETAIL_WEATHER_MEMBERS);
        constexpr auto DETAIL_TERRAIN_HELPERS = joinedMembers(DETAIL_TERRAIN_DISPATCH, DETAIL_TERRAIN_MEMBERS);
        constexpr std::array DETAIL_STREET_MEMBERS{std::string_view{"getStreetAnimationBackground"}};
        constexpr auto DETAIL_STREET_HELPERS = joinedMembers(DETAIL_TERRAIN_DISPATCH, DETAIL_STREET_MEMBERS);
        constexpr std::array DETAIL_BASE_MEMBERS{
            std::string_view{"loadSpotterOrCoMini"},
            std::string_view{"loadSpotter"},
            std::string_view{"defaultFrameDelay"},
        };
        constexpr std::array DETAIL_INFANTRY_MEMBERS{
            std::string_view{"getMaxUnitCount"},
            std::string_view{"getPositionOffset"},
            std::string_view{"loadStandingAnimation"},
            std::string_view{"loadSprite"},
            std::string_view{"getRiverString"},
            std::string_view{"isMountain"},
            std::string_view{"hasMoveInAnimation"},
            std::string_view{"loadMoveInAnimation"},
            std::string_view{"loadStopAnimation"},
            std::string_view{"getMoveInDurationMS"},
        };
        constexpr std::array DETAIL_SNIPER_MEMBERS{
            std::string_view{"getMaxUnitCount"},
            std::string_view{"getPositionOffset"},
            std::string_view{"loadStandingAnimation"},
        };
        constexpr std::array DETAIL_SPOTTER_TABLE{std::string_view{"spotterArmyData"}};
        constexpr std::array PROPERTY_INCOME_MEMBER{std::string_view{"getBaseIncome"}};
        constexpr std::string_view UNIT_COST_DECLARATION{"this.getBaseCost = function()"};
        constexpr std::string_view PROPERTY_INCOME_DECLARATION{"getBaseIncome : function(building, map)"};
        constexpr std::array TRAP_PREDICATE_MEMBERS{std::string_view{"isTrap"}};
        constexpr std::array MOVEMENT_TRAP_MEMBERS{std::string_view{"isTrap"}, std::string_view{"isStillATrap"}};
        constexpr std::string_view BUILDING_IS_HQ{"isHq"};
        constexpr std::array PROPERTY_CALLBACK_MEMBERS{
            BUILDING_IS_HQ,
            std::string_view{"getBaseIncome"},
            std::string_view{"getRepairTypes"},
            std::string_view{"getConstructionList"},
            std::string_view{"canRepair"},
            std::string_view{"startOfTurn"},
            std::string_view{"endOfTurn"},
            std::string_view{"onCaptured"},
            std::string_view{"getCostModifier"},
            std::string_view{"getCostReduction"},
            std::string_view{"getVision"},
            std::string_view{"getVisionHide"},
            std::string_view{"onWeatherChanged"},
            std::string_view{"getDefense"},
            std::string_view{"getOffensiveFieldBonus"},
            std::string_view{"getDeffensiveFieldBonus"},
            std::string_view{"getOffensiveBonus"},
            std::string_view{"getDefensiveBonus"},
            std::string_view{"getPowerChargeBonus"},
            std::string_view{"getDamage"},
            std::string_view{"addCaptureAnimationBuilding"},
            std::string_view{"loadSprites"},
            std::string_view{"getBuildingWidth"},
            std::string_view{"getBuildingHeigth"},
            std::string_view{"getBuildingBaseTerrain"},
            std::string_view{"getBaseTerrain"},
        };
        constexpr std::array BUILDING_HELPER_MEMBERS{
            std::string_view{"replenishUnit"},
            std::string_view{"repairUnit"},
            BUILDING_IS_HQ,
        };
        constexpr std::array BUILDING_TABLE_MEMBERS{
            std::string_view{"hqIds"},
            std::string_view{"armyData"},
            std::string_view{"baseTerrains"},
        };
        constexpr std::array PROPERTY_TABLE_MEMBERS{
            GUARDED_NATIVE_CONSTRUCTION_LIST_MEMBER,
            std::string_view{"baseTerrains"},
        };
        constexpr std::array HEADQUARTERS_EXTRA_TABLE_MEMBERS{
            std::string_view{"armyData"},
            std::string_view{"buildingData"},
        };
        constexpr auto HEADQUARTERS_TABLE_MEMBERS =
            joinedMembers(PROPERTY_TABLE_MEMBERS, HEADQUARTERS_EXTRA_TABLE_MEMBERS);
        constexpr std::array MOVEMENT_CALLBACK_MEMBERS{
            std::string_view{"getMovementpoints"},
            std::string_view{"getSupportsFastPfs"},
        };
        constexpr std::array MOVEMENT_HELPER_MEMBERS{
            std::string_view{"getMovementpointsFromTable"},
        };
        constexpr std::array MOVEMENT_TABLE_MEMBERS{
            std::string_view{"movementpointsTable"},
        };
        constexpr std::array TERRAIN_CONTEXT_MEMBERS{
            std::string_view{"getBuildingTerrainPalette"},
            std::string_view{"getWeatherModifier"},
            std::string_view{"getTerrainWeatherModifier"},
            std::string_view{"getWeatherOverlayId"},
            std::string_view{"getWeatherOverlaySpeed"},
        };
        constexpr std::array TERRAIN_CALLBACK_MEMBERS{
            std::string_view{"getMovementcostModifier"},
            std::string_view{"getDefense"},
            std::string_view{"getVision"},
            std::string_view{"getBonusVision"},
            std::string_view{"getVisionHide"},
            std::string_view{"getFirerangeModifier"},
            std::string_view{"getMinFirerangeModifier"},
            std::string_view{"getOffensiveFieldBonus"},
            std::string_view{"getDeffensiveFieldBonus"},
            std::string_view{"startOfTurn"},
            std::string_view{"onDestroyed"},
            std::string_view{"getDefaultPalette"},
        };
        constexpr std::array TERRAIN_BASE_ID_MEMBERS{std::string_view{"baseTerrainId"}};
        constexpr auto BASE_TERRAIN_CONTEXT_MEMBERS =
            joinedMembers(TERRAIN_CONTEXT_MEMBERS, TERRAIN_CALLBACK_MEMBERS);
        constexpr auto TERRAIN_BASE_MEMBERS =
            joinedMembers(TERRAIN_CALLBACK_MEMBERS, TERRAIN_BASE_ID_MEMBERS);
        constexpr std::array DEFAULT_ARMY_MEMBERS{std::string_view{"getDefaultArmy"}, std::string_view{"getArmies"}};
        const QString PLAYER_ARMIES = QStringLiteral("armies");
        constexpr std::array PLAYER_PHASE_MEMBERS{
            std::string_view{"startOfTurn"},
            std::string_view{"endOfTurn"},
            std::string_view{"onUnitDeath"},
        };
        constexpr std::array TERRAIN_CONTEXT_TABLES{
            std::string_view{"weatherData"},
        };
        constexpr std::array PLAYER_CONTEXT_TABLES{
            std::string_view{"defaultColors"},
            std::string_view{"colorShades"},
        };
        constexpr std::array WEATHER_CALLBACK_MEMBERS{
            std::string_view{"getMovementCostModifier"},
            std::string_view{"getMovementpointModifier"},
            std::string_view{"getMovementFuelCostModifier"},
            std::string_view{"getOffensiveModifier"},
            std::string_view{"getDefensiveModifier"},
            std::string_view{"getFirerangeModifier"},
            std::string_view{"getMinFirerangeModifier"},
            std::string_view{"getVisionrangeModifier"},
            std::string_view{"getWeatherTerrainSprite"},
        };
        constexpr std::array VICTORY_CALLBACK_MEMBERS{
            std::string_view{"checkDefeat"},
            std::string_view{"getRuleValue"},
        };
        constexpr std::array HQ_COUNT_MEMBER{std::string_view{"checkHQCount"}};
        constexpr std::array UNIT_COUNT_MEMBER{std::string_view{"checkUnitCount"}};
        constexpr auto HQ_VICTORY_MEMBERS = joinedMembers(VICTORY_CALLBACK_MEMBERS, HQ_COUNT_MEMBER);
        constexpr auto UNIT_VICTORY_MEMBERS = joinedMembers(VICTORY_CALLBACK_MEMBERS, UNIT_COUNT_MEMBER);
        constexpr std::array RANKING_CALLBACK_MEMBERS{
            std::string_view{"unloadIcons"},
            std::string_view{"getIcon"},
            std::string_view{"getOffensiveBonus"},
            std::string_view{"getDefensiveBonus"},
            std::string_view{"co1"},
            std::string_view{"co2"},
        };
        constexpr std::array RANKING_TABLE_MEMBERS{
            std::string_view{"icons"},
            std::string_view{"offensiveBonus"},
            std::string_view{"defensiveBonus"},
        };
        constexpr std::array GLOBAL_CALLBACK_MEMBERS{
            std::string_view{"getArmyNameFromPlayerTable"},
            std::string_view{"getArmyNameFromTable"},
            std::string_view{"getDataFromTable"},
            std::string_view{"getFogOfWarColor"},
            std::string_view{"getFogOfMistColor"},
        };

        enum class IntrinsicNamespace : std::int8_t
        {
            Math,
            Array,
            String,
            Number,
        };

        struct IntrinsicDescriptor
        {
            GuardedNativeIntrinsic kind;
            IntrinsicNamespace owner;
            std::string_view member;
        };

        constexpr std::array INTRINSICS{
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathAbs, IntrinsicNamespace::Math, "abs"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathCeil, IntrinsicNamespace::Math, "ceil"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathFloor, IntrinsicNamespace::Math, "floor"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathMax, IntrinsicNamespace::Math, "max"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathMin, IntrinsicNamespace::Math, "min"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::MathRound, IntrinsicNamespace::Math, "round"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::ArrayIndexOf, IntrinsicNamespace::Array, "indexOf"},
            IntrinsicDescriptor{
                GuardedNativeIntrinsic::StringToLowerCase, IntrinsicNamespace::String, "toLowerCase"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::ArrayIncludes, IntrinsicNamespace::Array, "includes"},
            IntrinsicDescriptor{GuardedNativeIntrinsic::NumberToString, IntrinsicNamespace::Number, "toString"},
        };

        GuardedNativeBindingStatus propertyStatus(GuardedScriptPropertyStatus status)
        {
            if (status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            return GuardedNativeBindingStatus::Unsupported;
        }

        GuardedNativeEnumAgreement authenticateEnumFields(
            QJSEngine & engine, const QObject & receiver, const QMetaObject & metadata,
            std::span<const NativeEnumField> fields, RefinementLedger & ledger,
            const GuardedScriptGlobalIndex* globals, GuardedScriptOwnIndexCache* ownIndex)
        {
            GuardedNativeEnumAgreement result;
            if (!ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                return result;
            }
            const auto global = globals ? globals->find(engine, ENUMS_GLOBAL, ledger)
                                        : guardedScriptGlobalProperty(engine, ENUMS_GLOBAL, ledger);
            if (global.status != GuardedScriptPropertyStatus::Found)
            {
                result.status = propertyStatus(global.status);
                return result;
            }
            if (!ledger.consume(ENUM_RECORD_WORK * fields.size(), RefinementWork::MasterPreparation))
            {
                return {GuardedNativeBindingStatus::BudgetExhausted, {}};
            }
            result.values.reserve(fields.size());
            for (const auto & field : fields)
            {
                if (!ledger.consume(ENUM_NAME_METADATA_WORK + ENUM_NAME_CHARACTER_WORK * field.name.size(),
                                    RefinementWork::MasterPreparation))
                {
                    return {GuardedNativeBindingStatus::BudgetExhausted, {}};
                }
                const auto name = QString::fromLatin1(field.name.data(), field.name.size());
                const auto property = guardedScriptQObjectProperty(
                    global.value, receiver, metadata, name, ledger, ownIndex);
                if (property.status != GuardedScriptPropertyStatus::Found)
                {
                    return {propertyStatus(property.status), {}};
                }
                const auto scalar = guardedScriptScalar(property.value, ledger);
                if (scalar.status == GuardedScriptScalarStatus::BudgetExhausted)
                {
                    return {GuardedNativeBindingStatus::BudgetExhausted, {}};
                }
                if (scalar.status != GuardedScriptScalarStatus::Verified ||
                    !std::holds_alternative<double>(scalar.value) ||
                    std::get<double>(scalar.value) != static_cast<double>(field.value) ||
                    (field.value == 0 && std::signbit(std::get<double>(scalar.value))))
                {
                    return {};
                }
                result.values.push_back(field.value);
            }
            result.status = GuardedNativeBindingStatus::Bound;
            return result;
        }

        GuardedScriptProperty intrinsicNamespace(
            QJSEngine & engine, IntrinsicNamespace owner, RefinementLedger & ledger,
            const GuardedScriptGlobalIndex* globals)
        {
            switch (owner)
            {
                case IntrinsicNamespace::Math:
                    return globals ? globals->find(engine, MATH_GLOBAL, ledger)
                                   : guardedScriptGlobalProperty(engine, MATH_GLOBAL, ledger);
                case IntrinsicNamespace::Array:
                    return guardedScriptIntrinsicPrototype(engine, GuardedScriptIntrinsicPrototype::Array, ledger);
                case IntrinsicNamespace::String:
                    return guardedScriptIntrinsicPrototype(engine, GuardedScriptIntrinsicPrototype::String, ledger);
                case IntrinsicNamespace::Number:
                    return guardedScriptIntrinsicPrototype(engine, GuardedScriptIntrinsicPrototype::Number, ledger);
            }
            return {};
        }

        bool isNativeUnitKind(GuardedNativeCodeKind kind)
        {
            switch (kind)
            {
                case GuardedNativeCodeKind::Infantry:
                case GuardedNativeCodeKind::Sniper:
                case GuardedNativeCodeKind::Recon:
                case GuardedNativeCodeKind::LightTank:
                    return true;
                default:
                    return false;
            }
        }

        bool hasTableFreeUnitPresentation(GuardedNativeCodeKind kind)
        {
            // The pinned walking callbacks for these kinds do not read presentation tables.
            return kind == GuardedNativeCodeKind::Recon || kind == GuardedNativeCodeKind::LightTank;
        }

        std::optional<QString> compilerInputName(const QString & compiled)
        {
            if (compiled.isEmpty())
            {
                return QString{};
            }
            const QUrl source(compiled);
            if (source.scheme() == FILE_SCHEME)
            {
                const auto input = source.toLocalFile();
                if (QUrl::fromLocalFile(input).toString() == compiled)
                {
                    return input;
                }
            }
            else if (source.scheme() == RESOURCE_SCHEME)
            {
                QUrl canonical;
                canonical.setPath(source.path());
                canonical.setScheme(RESOURCE_SCHEME);
                if (canonical.toString() == compiled)
                {
                    return QStringLiteral(":") + source.path();
                }
            }
            return std::nullopt;
        }

        struct CodeAsset
        {
            GuardedNativeCodeKind kind;
            GuardedNativeCodeKind parent;
            std::string_view compilationName;
            const char* global;
            const char* sha256;
            std::string_view source;
        };

        #include "guardednativeassets.h"
        static_assert(CODE_ASSETS.size() + 1 == static_cast<std::size_t>(GuardedNativeCodeKind::Count));
        constexpr std::size_t MAX_MODULE_COMPILATIONS = 2 * CODE_ASSETS.size();

        constexpr bool validAssetOrdinals()
        {
            for (std::size_t index = 0; index < CODE_ASSETS.size(); ++index)
            {
                if (static_cast<std::size_t>(CODE_ASSETS[index].kind) != index + 1)
                {
                    return false;
                }
            }
            return true;
        }

        static_assert(validAssetOrdinals());

        constexpr auto assetNameOrder()
        {
            std::array<std::size_t, CODE_ASSETS.size()> order{};
            for (std::size_t index = 0; index < order.size(); ++index)
            {
                auto destination = index;
                const std::string_view name{CODE_ASSETS[index].global};
                while (destination > 0 &&
                       name < std::string_view{CODE_ASSETS[order[destination - 1]].global})
                {
                    order[destination] = order[destination - 1];
                    --destination;
                }
                order[destination] = index;
            }
            return order;
        }

        constexpr auto ASSET_NAME_ORDER = assetNameOrder();

        constexpr bool validAssetNames()
        {
            std::string_view previous;
            for (const auto index : ASSET_NAME_ORDER)
            {
                const std::string_view name{CODE_ASSETS[index].global};
                if (name.empty() || name.size() > static_cast<std::size_t>(MAX_CODE_MEMBER) || name <= previous)
                {
                    return false;
                }
                for (const auto character : name)
                {
                    if (character == 0 || static_cast<unsigned char>(character) > 127)
                    {
                        return false;
                    }
                }
                previous = name;
            }
            return true;
        }

        static_assert(validAssetNames());

        constexpr std::int64_t maximumAssetNameLength()
        {
            std::int64_t maximum = 0;
            for (const auto & asset : CODE_ASSETS)
            {
                const auto length = static_cast<std::int64_t>(std::string_view{asset.global}.size());
                maximum = std::max(maximum, length);
            }
            return maximum;
        }

        constexpr std::int64_t assetNameSearchDepth()
        {
            std::int64_t depth = 0;
            for (auto remaining = CODE_ASSETS.size(); remaining > 0; remaining >>= 1)
            {
                ++depth;
            }
            return depth;
        }

        constexpr auto MAX_ASSET_NAME_LENGTH = maximumAssetNameLength();
        constexpr auto ASSET_NAME_SEARCH_DEPTH = assetNameSearchDepth();

        void skipSourceWhitespace(std::string_view source, std::size_t & cursor)
        {
            while (cursor < source.size() &&
                   (source[cursor] == ' ' || source[cursor] == '\t' ||
                    source[cursor] == '\n' || source[cursor] == '\r'))
            {
                ++cursor;
            }
        }

        std::optional<std::int32_t> literalGetter(
            std::string_view source, std::string_view declaration)
        {
            const auto found = source.find(declaration);
            if (found == std::string_view::npos ||
                source.find(declaration, found + declaration.size()) != std::string_view::npos)
            {
                return std::nullopt;
            }
            auto cursor = found + declaration.size();
            skipSourceWhitespace(source, cursor);
            if (cursor == source.size() || source[cursor++] != '{')
            {
                return std::nullopt;
            }
            skipSourceWhitespace(source, cursor);
            constexpr std::string_view returnKeyword{"return"};
            if (source.substr(cursor, returnKeyword.size()) != returnKeyword)
            {
                return std::nullopt;
            }
            cursor += returnKeyword.size();
            const auto separator = cursor;
            skipSourceWhitespace(source, cursor);
            if (cursor == separator)
            {
                return std::nullopt;
            }
            std::int32_t value = 0;
            const auto firstDigit = cursor;
            while (cursor < source.size() && source[cursor] >= '0' && source[cursor] <= '9')
            {
                const auto digit = source[cursor++] - '0';
                if (value > (std::numeric_limits<std::int32_t>::max() - digit) / 10)
                {
                    return std::nullopt;
                }
                value = value * 10 + digit;
            }
            if (cursor == firstDigit || value <= 0)
            {
                return std::nullopt;
            }
            skipSourceWhitespace(source, cursor);
            if (cursor == source.size() || source[cursor++] != ';')
            {
                return std::nullopt;
            }
            skipSourceWhitespace(source, cursor);
            if (cursor == source.size() || source[cursor] != '}')
            {
                return std::nullopt;
            }
            return value;
        }

        constexpr std::int64_t ASSET_LOOKUP_WORK = CACHE_CONTROL_WORK + ASSET_INSPECTION_WORK;

        const CodeAsset* assetFor(GuardedNativeCodeKind kind)
        {
            const auto ordinal = static_cast<std::int32_t>(kind);
            if (ordinal <= 0 || ordinal >= static_cast<std::int32_t>(GuardedNativeCodeKind::Count))
            {
                return nullptr;
            }
            return &CODE_ASSETS[static_cast<std::size_t>(ordinal - 1)];
        }

        std::int64_t cacheWork(std::size_t entries, qsizetype name, qsizetype member)
        {
            std::int64_t depth = 1;
            for (auto remaining = entries + 1; remaining > 1; remaining >>= 1)
            {
                ++depth;
            }
            return CACHE_CONTROL_WORK + CACHE_COMPARE_PASSES * (2 * depth + 1) * (name + member + 1);
        }

        std::int64_t functionComparisonWork(const GuardedScriptFunctionImage & image)
        {
            return FUNCTION_COMPARISON_WORK + FUNCTION_COMPARE_ELEMENT_WORK *
                (image.implementation.size() + image.compilationName.size());
        }

        GuardedNativeBindingStatus imageStatus(GuardedScriptFunctionStatus status)
        {
            if (status == GuardedScriptFunctionStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            return GuardedNativeBindingStatus::Unsupported;
        }

        bool sameScalar(const GuardedScriptScalar & actual, const GuardedScriptScalar & approved)
        {
            if (actual.kind != approved.kind || actual.value != approved.value)
            {
                return false;
            }
            if (actual.kind == GuardedScriptValueKind::Number)
            {
                return std::signbit(std::get<double>(actual.value)) ==
                    std::signbit(std::get<double>(approved.value));
            }
            return true;
        }

        std::int64_t scalarComparisonWork(const GuardedScriptScalar & actual, const GuardedScriptScalar & approved)
        {
            std::int64_t work = SCALAR_COMPARISON_WORK;
            if (actual.kind == GuardedScriptValueKind::String)
            {
                work += SCALAR_COMPARE_CHARACTER_WORK * std::get<QString>(actual.value).size();
            }
            if (approved.kind == GuardedScriptValueKind::String)
            {
                work += SCALAR_COMPARE_CHARACTER_WORK * std::get<QString>(approved.value).size();
            }
            return work;
        }

        GuardedNativeBindingStatus appendPrimitiveData(
            GuardedNativeDataNode node, std::vector<GuardedNativeDataNode> & nodes, RefinementLedger & ledger)
        {
            if (nodes.size() >= MAX_PRIMITIVE_DATA_NODES || nodes.capacity() > MAX_PRIMITIVE_DATA_NODES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (nodes.size() == nodes.capacity())
            {
                auto capacity = INITIAL_DATA_CAPACITY;
                if (nodes.capacity() != 0)
                {
                    capacity = std::min(MAX_PRIMITIVE_DATA_NODES, DATA_CAPACITY_GROWTH * nodes.capacity());
                }
                const auto work = DATA_RETENTION_WORK * (capacity + nodes.size());
                if (!ledger.consume(work, RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                nodes.reserve(capacity);
            }
            nodes.push_back(std::move(node));
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus collectPrimitiveData(
            const GuardedScriptValueView & value, std::uint32_t depth,
            std::vector<GuardedNativeDataNode> & nodes, RefinementLedger & ledger,
            QJSEngine* pointEngine)
        {
            if (!ledger.consume(DATA_NODE_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (nodes.size() >= MAX_PRIMITIVE_DATA_NODES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (value.kind() != GuardedScriptValueKind::Managed)
            {
                auto scalar = guardedScriptScalar(value, ledger);
                if (scalar.status != GuardedScriptScalarStatus::Verified)
                {
                    if (scalar.status == GuardedScriptScalarStatus::BudgetExhausted)
                    {
                        return GuardedNativeBindingStatus::BudgetExhausted;
                    }
                    return GuardedNativeBindingStatus::Unsupported;
                }
                return appendPrimitiveData(
                    {GuardedNativeDataKind::Scalar, 0, std::move(scalar)}, nodes, ledger);
            }
            if (depth >= MAX_PRIMITIVE_ARRAY_DEPTH)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto array = guardedScriptArray(value, ledger);
            if (array.status != GuardedScriptPropertyStatus::Found)
            {
                if (array.status == GuardedScriptPropertyStatus::BudgetExhausted)
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (pointEngine == nullptr)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                const auto point = guardedScriptPointValue(*pointEngine, value, ledger);
                if (point.status == GuardedNativeReceiverStatus::BudgetExhausted)
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (point.status != GuardedNativeReceiverStatus::Bound)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                return appendPrimitiveData(
                    {GuardedNativeDataKind::Point, 0, {}, {point.x, point.y}}, nodes, ledger);
            }
            const auto indexed = guardedScriptIndexedArrayData(value, ledger);
            if (indexed.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(indexed.status);
            }
            const auto appended = appendPrimitiveData(
                {GuardedNativeDataKind::Array, array.length, {}}, nodes, ledger);
            if (appended != GuardedNativeBindingStatus::Bound)
            {
                return appended;
            }
            for (std::uint32_t index = 0; index < array.length; ++index)
            {
                const auto element = guardedScriptArrayElement(value, index, ledger);
                if (element.status != GuardedScriptPropertyStatus::Found || element.prototypeDepth != 0)
                {
                    if (element.status == GuardedScriptPropertyStatus::BudgetExhausted)
                    {
                        return GuardedNativeBindingStatus::BudgetExhausted;
                    }
                    return GuardedNativeBindingStatus::Unsupported;
                }
                const auto status = collectPrimitiveData(element.value, depth + 1, nodes, ledger, pointEngine);
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    return status;
                }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus comparePrimitiveData(
            std::span<const GuardedNativeDataNode> actual,
            std::span<const GuardedNativeDataNode> approved, RefinementLedger & ledger)
        {
            if (actual.size() != approved.size())
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            for (std::size_t index = 0; index < actual.size(); ++index)
            {
                const auto & left = actual[index];
                const auto & right = approved[index];
                auto work = DATA_COMPARISON_WORK;
                if (left.kind == GuardedNativeDataKind::Scalar && right.kind == GuardedNativeDataKind::Scalar)
                {
                    work += scalarComparisonWork(left.scalar, right.scalar);
                }
                else if (left.kind == GuardedNativeDataKind::Point && right.kind == GuardedNativeDataKind::Point)
                {
                    work += 2 * SCALAR_COMPARISON_WORK;
                }
                if (!ledger.consume(work, RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (left.kind != right.kind || left.elements != right.elements ||
                    (left.kind == GuardedNativeDataKind::Scalar && !sameScalar(left.scalar, right.scalar)))
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                if (left.kind == GuardedNativeDataKind::Point)
                {
                    for (std::size_t coordinate = 0; coordinate < left.point.size(); ++coordinate)
                    {
                        if (left.point[coordinate] != right.point[coordinate] ||
                            std::signbit(left.point[coordinate]) != std::signbit(right.point[coordinate]))
                        {
                            return GuardedNativeBindingStatus::Unsupported;
                        }
                    }
                }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus recheckMemberValue(
            const GuardedScriptValueView & value, const GuardedNativeMemberAgreement & owned,
            RefinementLedger & ledger, GuardedScriptImageCache* imageCache)
        {
            if (owned.status != GuardedNativeBindingStatus::Bound)
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            if (std::holds_alternative<GuardedNativeFunctionAgreement>(owned.evidence))
            {
                const auto & function = std::get<GuardedNativeFunctionAgreement>(owned.evidence);
                const auto context = guardedScriptContextOperands(value, ledger);
                if (context.status != GuardedScriptContextOperands::GlobalScopeOnly)
                {
                    return context.status == GuardedScriptContextOperands::BudgetExhausted
                        ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
                }
                const auto image = guardedScriptFunctionImage(value, ledger, imageCache);
                if (image.status != GuardedScriptFunctionStatus::Verified)
                {
                    return imageStatus(image.status);
                }
                if (!ledger.consume(functionComparisonWork(image), RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                return function.status == GuardedNativeBindingStatus::Bound &&
                    function.code.status == GuardedNativeBindingStatus::Bound &&
                    function.context.status == GuardedScriptContextOperands::GlobalScopeOnly &&
                    image.sameImplementation(function.code.approved)
                    ? GuardedNativeBindingStatus::Bound : GuardedNativeBindingStatus::Unsupported;
            }
            if (std::holds_alternative<GuardedNativeScalarAgreement>(owned.evidence))
            {
                const auto & scalar = std::get<GuardedNativeScalarAgreement>(owned.evidence);
                const auto current = guardedScriptScalar(value, ledger);
                if (current.status != GuardedScriptScalarStatus::Verified)
                {
                    return current.status == GuardedScriptScalarStatus::BudgetExhausted
                        ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
                }
                if (!ledger.consume(scalarComparisonWork(current, scalar.approved),
                                    RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                return scalar.status == GuardedNativeBindingStatus::Bound &&
                    sameScalar(current, scalar.approved)
                    ? GuardedNativeBindingStatus::Bound : GuardedNativeBindingStatus::Unsupported;
            }
            return GuardedNativeBindingStatus::Invalid;
        }

        GuardedNativeBindingStatus recheckArrayValue(
            QJSEngine & engine, const GuardedScriptValueView & value,
            const GuardedNativeArrayAgreement & owned, RefinementLedger & ledger)
        {
            if (owned.status != GuardedNativeBindingStatus::Bound || owned.approved.empty())
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            std::vector<GuardedNativeDataNode> current;
            const auto status = collectPrimitiveData(value, 0, current, ledger, &engine);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
            return comparePrimitiveData(current, owned.approved, ledger);
        }
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckModule(
        QJSEngine & engine, const GuardedNativeModuleAgreement & owned)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(owned.kind);
        if (asset == nullptr || owned.status != GuardedNativeBindingStatus::Bound || owned.members.empty())
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK +
                MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset->global),
                RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        for (const auto & member : owned.members)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.name.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (member.name.isEmpty() || member.name.size() > MAX_CODE_MEMBER)
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            const auto property = moduleProperty(engine, owned.kind, global.value, member.name);
            if (property.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(property.status);
            }
            const auto status = recheckObservedMember(engine, property.value, member);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckMember(
        QJSEngine & engine, GuardedNativeCodeKind kind, const GuardedNativeMemberAgreement & member)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK +
                MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.name.size(),
                RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr || member.name.isEmpty() || member.name.size() > MAX_CODE_MEMBER)
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        const auto property = moduleProperty(engine, kind, global.value, member.name);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(property.status);
        }
        return recheckObservedMember(engine, property.value, member);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckArray(
        QJSEngine & engine, GuardedNativeCodeKind kind, const QString & name,
        const GuardedNativeArrayAgreement & owned)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK +
                MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * name.size(),
                RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr || name.isEmpty() || name.size() > MAX_CODE_MEMBER ||
            owned.status != GuardedNativeBindingStatus::Bound || owned.approved.empty())
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        const auto property = moduleProperty(engine, kind, global.value, name);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(property.status);
        }
        return recheckArrayValue(engine, property.value, owned, m_ledger);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckTables(
        QJSEngine & engine, const GuardedNativeTableAgreement & owned)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(owned.kind);
        if (asset == nullptr || owned.status != GuardedNativeBindingStatus::Bound ||
            (owned.tables.empty() && !hasTableFreeUnitPresentation(owned.kind)))
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK +
                MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset->global),
                RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        for (const auto & table : owned.tables)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * table.name.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto property = moduleProperty(engine, owned.kind, global.value, table.name);
            if (property.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(property.status);
            }
            const auto status = recheckArrayValue(engine, property.value, table.evidence, m_ledger);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                return status;
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckIntrinsics(
        QJSEngine & engine, const GuardedNativeIntrinsicAgreement & owned)
    {
        if (!m_ledger.consume(INTRINSIC_CONTROL_WORK + INTRINSIC_RECORD_WORK * INTRINSICS.size(),
                              RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (owned.status != GuardedNativeBindingStatus::Bound || owned.entries.size() != INTRINSICS.size())
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        for (std::size_t index = 0; index < INTRINSICS.size(); ++index)
        {
            const auto & descriptor = INTRINSICS[index];
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * descriptor.member.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto space = intrinsicNamespace(engine, descriptor.owner, m_ledger,
                m_observedEngine == &engine ? &m_globals : nullptr);
            if (space.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(space.status);
            }
            const auto property = guardedScriptBuiltinProperty(space.value,
                QString::fromLatin1(descriptor.member.data(), descriptor.member.size()), m_ledger);
            if (property.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(property.status);
            }
            const auto image = guardedScriptFunctionImage(property.value, m_ledger, m_imageCache.get());
            if (image.status != GuardedScriptFunctionStatus::Verified)
            {
                return imageStatus(image.status);
            }
            if (!m_ledger.consume(INTRINSIC_COMPARE_WORK + 4 * image.implementation.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (owned.entries[index].kind != descriptor.kind ||
                image.kind != GuardedScriptFunctionKind::Builtin ||
                !image.sameImplementation(owned.entries[index].approved))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckAbsentHooks(
        QJSEngine & engine, const GuardedNativeAbsentHooks & owned)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(owned.kind);
        if (asset == nullptr || owned.status != GuardedNativeBindingStatus::Bound)
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        for (const auto & member : owned.members)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.name.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto property = moduleProperty(engine, owned.kind, global.value, member.name);
            if (property.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if ((member.explicitUndefined && (property.status != GuardedScriptPropertyStatus::Found ||
                    property.value.kind() != GuardedScriptValueKind::Undefined)) ||
                (!member.explicitUndefined && property.status != GuardedScriptPropertyStatus::Missing))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckConstructionQuery(
        QJSEngine & engine, const GuardedNativeConstructionAgreement & owned)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + MEMBER_NAME_WORK +
                MEMBER_NAME_CHARACTER_WORK * owned.type.size(), RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (owned.status != GuardedNativeBindingStatus::Bound || owned.type.isEmpty() ||
            owned.type.size() > MAX_CODE_MEMBER)
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        const auto global = constructionProperty(engine, owned.type);
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        const auto property = guardedScriptProperty(
            global.value, owned.callback.name, m_ledger,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(property.status);
        }
        return recheckMemberValue(property.value, owned.callback, m_ledger, m_imageCache.get());
    }

    struct GuardedNativeCodeCatalog::Data
    {
        using Key = std::tuple<GuardedNativeCodeKind, QByteArray, QString>;
        using PropertyKey = std::pair<GuardedNativeCodeKind, QString>;
        QJSEngine engine;
        std::unique_ptr<GuardedScriptImageCache> imageCache;
        std::optional<GuardedScriptOwnIndexCache> ownIndex;
        std::map<Key, GuardedScriptFunctionImage> images;
        std::map<PropertyKey, GuardedScriptProperty> properties;
        std::array<GuardedScriptProperty, CODE_ASSETS.size()> globals;
        std::size_t moduleCompilations{0};
        std::array<std::optional<QString>, CODE_ASSETS.size()> loadedNames;
        std::int64_t expectedWork{0};
        std::uint32_t expectedDepth{0};
        bool expectedWorkValid{true};
        bool initialized{false};

        class ExpectedOperation final
        {
        public:
            ExpectedOperation(Data & data, RefinementLedger & ledger)
                : m_data(data), m_ledger(ledger), m_before(ledger.stats().consumed)
            {
                m_funded = ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation);
                if (m_funded)
                {
                    m_outer = data.expectedDepth++ == 0;
                }
            }

            ~ExpectedOperation()
            {
                if (!m_funded)
                {
                    return;
                }
                --m_data.expectedDepth;
                if (m_outer)
                {
                    const auto work = m_ledger.stats().consumed - m_before;
                    if (work < 0 || work > std::numeric_limits<std::int64_t>::max() - m_data.expectedWork)
                    {
                        m_data.expectedWorkValid = false;
                    }
                    else
                    {
                        m_data.expectedWork += work;
                    }
                }
            }

            bool funded() const
            {
                return m_funded;
            }

            ExpectedOperation(const ExpectedOperation &) = delete;
            ExpectedOperation & operator=(const ExpectedOperation &) = delete;

        private:
            Data & m_data;
            RefinementLedger & m_ledger;
            std::int64_t m_before{0};
            bool m_funded{false};
            bool m_outer{false};
        };

        Data()
        {
#ifdef SCRIPTSOURCESUPPORT
            engine.handle()->createQtObject();
#endif
            initialized = !engine.evaluate(EXPECTED_TRANSLATION_BOOTSTRAP, EXPECTED_BOOTSTRAP_NAME).isError();
            auto enums = engine.newObject();
            for (const auto & field : UNIT_TYPE_FIELDS)
            {
                enums.setProperty(QString::fromLatin1(field.name.data(), field.name.size()), field.value);
            }
            for (const auto & field : TERMINAL_ENUM_FIELDS)
            {
                enums.setProperty(QString::fromLatin1(field.name.data(), field.name.size()), field.value);
            }
            for (const auto & field : STOCK_CONTEXT_ENUM_FIELDS)
            {
                enums.setProperty(QString::fromLatin1(field.name.data(), field.name.size()), field.value);
            }
            engine.globalObject().setProperty(ENUMS_GLOBAL, enums);
        }

        GuardedNativeBindingStatus load(
            const CodeAsset & asset, const QString & compilationName, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (!ledger.consume(ASSET_METADATA_WORK + COMPILER_URI_CHARACTER_WORK * compilationName.size(),
                                RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (asset.source.empty())
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto index = static_cast<std::size_t>(&asset - CODE_ASSETS.data());
            if (loadedNames[index] && *loadedNames[index] == compilationName)
            {
                return GuardedNativeBindingStatus::Bound;
            }
            const auto input = compilerInputName(compilationName);
            if (!input)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (moduleCompilations >= MAX_MODULE_COMPILATIONS)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (!ledger.consume(APPROVED_SOURCE_BYTE_WORK * asset.source.size(), RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const QByteArrayView bytes(asset.source.data(), asset.source.size());
            if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != QByteArray::fromHex(asset.sha256))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (imageCache)
            {
                if (!ledger.consume(CACHE_CONTROL_WORK * (globals.size() + properties.size() + 1),
                                    RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                // Private evaluation is the only collection/mutation boundary for these borrowed projections.
                properties.clear();
                globals.fill({});
                ownIndex.reset();
            }
            ++moduleCompilations;
            const auto evaluated = engine.evaluate(QString::fromUtf8(bytes), *input);
            if (evaluated.isError())
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            loadedNames[index] = compilationName;
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus loadParent(const CodeAsset & asset, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            std::array<const CodeAsset*, MAX_DECLARED_PARENT_DEPTH> parents{};
            std::size_t count = 0;
            const auto* current = &asset;
            while (true)
            {
                if (!ledger.consume(ASSET_LOOKUP_WORK + CACHE_CONTROL_WORK * MAX_DECLARED_PARENT_DEPTH,
                                    RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (current->parent == GuardedNativeCodeKind::None)
                {
                    break;
                }
                if (count == parents.size())
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                const auto* parent = assetFor(current->parent);
                if (parent == nullptr || parent == &asset)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (parents[index] == parent)
                    {
                        return GuardedNativeBindingStatus::Unsupported;
                    }
                }
                parents[count++] = parent;
                current = parent;
            }
            while (count > 0)
            {
                const auto* parent = parents[--count];
                const auto index = static_cast<std::size_t>(parent - CODE_ASSETS.data());
                if (loadedNames[index])
                {
                    continue;
                }
                if (!ledger.consume(COMPILER_URI_CHARACTER_WORK * parent->compilationName.size(),
                                    RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                const auto name = QString::fromLatin1(parent->compilationName.data(), parent->compilationName.size());
                const auto status = load(*parent, name, ledger);
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    return status;
                }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus loadCanonical(const CodeAsset & asset, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto index = static_cast<std::size_t>(&asset - CODE_ASSETS.data());
            if (asset.source.empty())
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (loadedNames[index])
            {
                return GuardedNativeBindingStatus::Bound;
            }
            const auto parent = loadParent(asset, ledger);
            if (parent != GuardedNativeBindingStatus::Bound)
            {
                return parent;
            }
            if (!ledger.consume(COMPILER_URI_CHARACTER_WORK * asset.compilationName.size(),
                                RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto name = QString::fromLatin1(asset.compilationName.data(), asset.compilationName.size());
            return load(asset, name, ledger);
        }

        GuardedNativeBindingStatus loadForMember(
            const CodeAsset & asset, const GuardedScriptValueView & actual,
            RefinementLedger & ledger, GuardedScriptImageCache* actualImages)
        {
            const auto index = static_cast<std::size_t>(&asset - CODE_ASSETS.data());
            if (actualImages == nullptr || loadedNames[index] || actual.kind() != GuardedScriptValueKind::Managed)
            {
                return loadCanonical(asset, ledger);
            }
            const auto hint = guardedScriptFunctionImage(actual, ledger, actualImages);
            if (hint.status == GuardedScriptFunctionStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (hint.status != GuardedScriptFunctionStatus::Verified || hint.kind != GuardedScriptFunctionKind::Script)
            {
                return loadCanonical(asset, ledger);
            }
            const auto parent = loadParent(asset, ledger);
            if (parent != GuardedNativeBindingStatus::Bound)
            {
                return parent;
            }
            // The filename is only a compile hint; owner, scope, data and full images still require authentication.
            return load(asset, hint.compilationName, ledger);
        }

        GuardedScriptProperty globalProperty(const CodeAsset & asset, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return {GuardedScriptPropertyStatus::BudgetExhausted};
            }
            if (!imageCache)
            {
                return guardedScriptGlobalProperty(engine, QString::fromLatin1(asset.global), ledger);
            }
            GuardedScriptProperty result;
            if (!ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            auto & cached = globals[static_cast<std::size_t>(&asset - CODE_ASSETS.data())];
            if (cached.status == GuardedScriptPropertyStatus::Found)
            {
                return cached;
            }
            if (!ledger.consume(MEMBER_NAME_WORK +
                    MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset.global),
                                RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            result = guardedScriptGlobalProperty(engine, QString::fromLatin1(asset.global), ledger);
            if (result.status == GuardedScriptPropertyStatus::Found)
            {
                cached = result;
            }
            return result;
        }

        GuardedScriptProperty memberProperty(
            const CodeAsset & asset, const QString & name, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return {GuardedScriptPropertyStatus::BudgetExhausted};
            }
            if (!imageCache)
            {
                const auto global = globalProperty(asset, ledger);
                if (global.status != GuardedScriptPropertyStatus::Found)
                {
                    return global;
                }
                return guardedScriptProperty(global.value, name, ledger);
            }
            GuardedScriptProperty result;
            if (!ledger.consume(cacheWork(properties.size(), 0, name.size()),
                                RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            const PropertyKey key{asset.kind, name};
            const auto found = properties.find(key);
            if (found != properties.end())
            {
                return found->second;
            }
            const auto global = globalProperty(asset, ledger);
            if (global.status != GuardedScriptPropertyStatus::Found)
            {
                return global;
            }
            if (!ownIndex || ownIndex->status() != GuardedScriptPropertyStatus::Found)
            {
                ownIndex.reset();
                ownIndex.emplace(engine, ledger);
            }
            if (ownIndex->status() != GuardedScriptPropertyStatus::Found)
            {
                result.status = ownIndex->status();
                return result;
            }
            result = guardedScriptProperty(global.value, name, ledger, &*ownIndex);
            if (result.status == GuardedScriptPropertyStatus::Found && properties.size() < MAX_EXPECTED_PROPERTIES)
            {
                if (!ledger.consume(EXPECTED_PROPERTY_RETENTION_WORK + MEMBER_NAME_CHARACTER_WORK * name.size() +
                        cacheWork(properties.size(), 0, name.size()), RefinementWork::MasterPreparation))
                {
                    result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                    return result;
                }
                properties.emplace(key, result);
            }
            return result;
        }

        GuardedScriptProperty scalarProperty(
            const CodeAsset & asset, std::span<const QString> path, RefinementLedger & ledger)
        {
            ExpectedOperation operation(*this, ledger);
            if (!operation.funded())
            {
                return {GuardedScriptPropertyStatus::BudgetExhausted};
            }
            const auto before = ledger.stats().spent();
            GuardedScriptProperty property;
            if (path.empty())
            {
                property = globalProperty(asset, ledger);
            }
            else
            {
                property = memberProperty(asset, path.front(), ledger);
            }
            for (std::size_t index = 1; index < path.size(); ++index)
            {
                if (property.status != GuardedScriptPropertyStatus::Found)
                {
                    break;
                }
                const auto & name = path[index];
                property = guardedScriptProperty(property.value, name, ledger);
            }
            property.chargedWork = ledger.stats().spent() - before;
            return property;
        }
    };

    GuardedNativeCodeCatalog::GuardedNativeCodeCatalog(RefinementLedger & ledger)
        : m_ledger(ledger)
    {
        if (m_ledger.consume(guardedNativeRealmConstructionWork(), RefinementWork::MasterPreparation))
        {
            m_initializationStatus = GuardedNativeBindingStatus::Unsupported;
            m_data = std::make_unique<Data>();
            if (!m_data->initialized)
            {
                m_data.reset();
            }
            else
            {
                m_initializationStatus = GuardedNativeBindingStatus::Bound;
            }
        }
    }

    GuardedNativeCodeCatalog::~GuardedNativeCodeCatalog() = default;

    std::optional<std::int64_t> GuardedNativeCodeCatalog::expectedPreparationWork() const
    {
        if (!m_data || !m_data->expectedWorkValid || m_data->expectedDepth != 0)
        {
            return std::nullopt;
        }
        return m_data->expectedWork;
    }

    std::size_t GuardedNativeCodeCatalog::expectedModuleCompilations() const
    {
        return m_data ? m_data->moduleCompilations : 0;
    }

    GuardedNativeCodeCatalog::Observation::Observation(
        GuardedNativeCodeCatalog & catalog, QJSEngine & engine, std::span<const QString> additionalGlobals)
    {
        if (!catalog.m_data)
        {
            m_status = catalog.m_initializationStatus;
            return;
        }
        if (catalog.m_observedEngine != nullptr)
        {
            return;
        }
        constexpr auto requiredNames = CODE_ASSETS.size() + GLOBAL_CALLBACK_MEMBERS.size() + 2;
        static_assert(requiredNames <= GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES);
        if (additionalGlobals.size() > GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES - requiredNames)
        {
            return;
        }
        if (!catalog.m_ledger.consume(ASSET_LOOKUP_WORK *
                (CODE_ASSETS.size() + GLOBAL_CALLBACK_MEMBERS.size() + additionalGlobals.size()),
                                      RefinementWork::MasterPreparation))
        {
            m_status = GuardedNativeBindingStatus::BudgetExhausted;
            return;
        }
        std::vector<QString> names;
        names.reserve(requiredNames + additionalGlobals.size());
        for (const auto & asset : CODE_ASSETS)
        {
            const auto length = std::char_traits<char>::length(asset.global);
            if (!catalog.m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * length,
                                          RefinementWork::MasterPreparation))
            {
                m_status = GuardedNativeBindingStatus::BudgetExhausted;
                return;
            }
            names.push_back(QString::fromLatin1(asset.global, length));
        }
        names.push_back(ENUMS_GLOBAL);
        names.push_back(MATH_GLOBAL);
        for (const auto member : GLOBAL_CALLBACK_MEMBERS)
        {
            if (!catalog.m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.size(),
                                          RefinementWork::MasterPreparation))
            {
                m_status = GuardedNativeBindingStatus::BudgetExhausted;
                return;
            }
            names.push_back(QString::fromLatin1(member.data(), member.size()));
        }
        for (const auto & name : additionalGlobals)
        {
            if (name.isEmpty() || name.size() > MAX_CODE_MEMBER)
            {
                return;
            }
            if (!catalog.m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * name.size(),
                                          RefinementWork::MasterPreparation))
            {
                m_status = GuardedNativeBindingStatus::BudgetExhausted;
                return;
            }
            names.push_back(name);
        }
        catalog.m_globals = GuardedScriptGlobalIndex::prepare(engine, names, catalog.m_ledger);
        m_status = propertyStatus(catalog.m_globals.status());
        if (catalog.m_globals.status() == GuardedScriptPropertyStatus::Found)
        {
            catalog.m_imageCache = std::make_unique<GuardedScriptImageCache>(engine, catalog.m_ledger);
            catalog.m_data->imageCache =
                std::make_unique<GuardedScriptImageCache>(catalog.m_data->engine, catalog.m_ledger);
            catalog.m_ownIndex.emplace(engine, catalog.m_ledger);
            if (catalog.m_imageCache->status() != GuardedScriptFunctionStatus::Verified ||
                catalog.m_data->imageCache->status() != GuardedScriptFunctionStatus::Verified ||
                catalog.m_ownIndex->status() != GuardedScriptPropertyStatus::Found)
            {
                catalog.m_imageCache.reset();
                catalog.m_data->imageCache.reset();
                catalog.m_ownIndex.reset();
                catalog.m_globals = {};
                m_status = GuardedNativeBindingStatus::BudgetExhausted;
                return;
            }
            catalog.m_observedEngine = &engine;
            m_catalog = &catalog;
            m_status = GuardedNativeBindingStatus::Bound;
        }
    }

    GuardedNativeCodeCatalog::Observation::~Observation()
    {
        if (m_catalog != nullptr)
        {
            m_catalog->m_globals = {};
            m_catalog->m_constructionGlobals = {};
            m_catalog->m_imageCache.reset();
            m_catalog->m_ownIndex.reset();
            m_catalog->m_callbackGlobals.reset();
            m_catalog->m_data->properties.clear();
            m_catalog->m_data->globals.fill({});
            m_catalog->m_data->ownIndex.reset();
            m_catalog->m_data->imageCache.reset();
            m_catalog->m_observedEngine = nullptr;
        }
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::Observation::status() const
    {
        return m_status;
    }

    GuardedNativeCodeCatalog::Observation GuardedNativeCodeCatalog::observe(
        QJSEngine & engine, std::span<const QString> additionalGlobals)
    {
        return Observation(*this, engine, additionalGlobals);
    }

    GuardedScriptProperty GuardedNativeCodeCatalog::globalProperty(QJSEngine & engine, const QString & name)
    {
        if (m_observedEngine == &engine)
        {
            return m_globals.find(engine, name, m_ledger);
        }
        return guardedScriptGlobalProperty(engine, name, m_ledger);
    }

    GuardedScriptProperty GuardedNativeCodeCatalog::moduleProperty(
        QJSEngine & engine, GuardedNativeCodeKind kind, const GuardedScriptValueView & object, const QString & name)
    {
        if (kind == GuardedNativeCodeKind::Global && m_observedEngine == &engine)
        {
            const auto alias = guardedScriptGlobalAlias(engine, object, m_ledger);
            if (alias.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return {GuardedScriptPropertyStatus::BudgetExhausted};
            }
            if (alias.status == GuardedScriptPropertyStatus::Found)
            {
                return globalProperty(engine, name);
            }
        }
        return guardedScriptProperty(object, name, m_ledger,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::prepareConstructionQueries(
        QJSEngine & engine, std::span<const QString> names)
    {
        m_constructionGlobals = {};
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (m_observedEngine != &engine)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        m_constructionGlobals = GuardedScriptGlobalIndex::prepare(engine, names, m_ledger);
        if (m_constructionGlobals.status() == GuardedScriptPropertyStatus::Found)
        {
            return GuardedNativeBindingStatus::Bound;
        }
        return propertyStatus(m_constructionGlobals.status());
    }

    GuardedScriptProperty GuardedNativeCodeCatalog::constructionProperty(
        QJSEngine & engine, const QString & name)
    {
        if (m_observedEngine == &engine &&
            m_constructionGlobals.status() == GuardedScriptPropertyStatus::Found)
        {
            return m_constructionGlobals.find(engine, name, m_ledger);
        }
        return globalProperty(engine, name);
    }

    std::optional<std::int32_t> guardedNativeIntegerData(const GuardedNativeDataNode & node)
    {
        if (node.kind != GuardedNativeDataKind::Scalar ||
            node.scalar.status != GuardedScriptScalarStatus::Verified ||
            !std::holds_alternative<double>(node.scalar.value))
        {
            return std::nullopt;
        }
        const auto value = std::get<double>(node.scalar.value);
        if (!std::isfinite(value) || value != std::floor(value) ||
            value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
        {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(value);
    }

    GuardedNativeIntegerTableResult guardedNativeIntegerTableValue(
        const GuardedNativeArrayAgreement & table, const QString & name, RefinementLedger & ledger)
    {
        if (!ledger.consume(TABLE_LOOKUP_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        const auto & nodes = table.actual;
        if (table.status != GuardedNativeBindingStatus::Bound || nodes.empty() ||
            nodes.size() > MAX_PRIMITIVE_DATA_NODES || name.isEmpty() || name.size() > MAX_CODE_MEMBER)
        {
            return {};
        }
        if (!ledger.consume(TABLE_LOOKUP_NODE_WORK * nodes.size(), RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        std::int64_t characters = 0;
        for (const auto & node : nodes)
        {
            if (std::holds_alternative<QString>(node.scalar.value))
            {
                const auto length = std::get<QString>(node.scalar.value).size();
                if (length > MAX_CODE_MEMBER)
                {
                    return {};
                }
                characters += length;
            }
        }
        if (!ledger.consume(TABLE_LOOKUP_NODE_WORK * nodes.size() +
                TABLE_LOOKUP_CHARACTER_WORK * characters, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        if (nodes[0].kind != GuardedNativeDataKind::Array ||
            nodes[0].elements > (nodes.size() - 1) / 3 ||
            nodes.size() != 1 + 3 * static_cast<std::size_t>(nodes[0].elements))
        {
            return {};
        }
        for (std::size_t row = 0; row < nodes[0].elements; ++row)
        {
            const auto start = 1 + 3 * row;
            if (nodes[start].kind != GuardedNativeDataKind::Array || nodes[start].elements != 2 ||
                nodes[start + 1].kind != GuardedNativeDataKind::Scalar ||
                !std::holds_alternative<QString>(nodes[start + 1].scalar.value))
            {
                return {};
            }
            if (std::get<QString>(nodes[start + 1].scalar.value) == name)
            {
                return {GuardedNativeBindingStatus::Bound, guardedNativeIntegerData(nodes[start + 2])};
            }
        }
        return {GuardedNativeBindingStatus::Bound};
    }

    GuardedNativeMovementField prepareGuardedNativeMovementField(
        const GuardedNativeArrayAgreement & table, const GuardedNativeInventory & inventory,
        RefinementLedger & ledger)
    {
        if (!ledger.consume(MOVEMENT_FIELD_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        if (inventory.width <= 0 || inventory.width > GUARDED_NATIVE_MAX_WIDTH ||
            inventory.height <= 0 || inventory.height > GUARDED_NATIVE_MAX_HEIGHT ||
            !inventory.terrains)
        {
            return {};
        }
        const auto tiles = static_cast<std::size_t>(inventory.width * inventory.height);
        if (inventory.terrains->size() != tiles || inventory.properties.size() > tiles)
        {
            return {};
        }
        const auto storage = MOVEMENT_FIELD_STORAGE_PASSES * GUARDED_STOCK_MAX_TILES *
            (sizeof(const QString*) + sizeof(bool) + sizeof(CachedMovementValue) + sizeof(std::int32_t));
        if (!ledger.consume(storage + MOVEMENT_FIELD_ENTRY_WORK *
                (tiles + inventory.properties.size() + 1), RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        std::array<const QString*, GUARDED_STOCK_MAX_TILES> names{};
        std::array<bool, GUARDED_STOCK_MAX_TILES> properties{};
        std::array<CachedMovementValue, GUARDED_STOCK_MAX_TILES> cache{};
        std::array<std::int32_t, GUARDED_STOCK_MAX_TILES> field;
        field.fill(-1);
        for (std::size_t tile = 0; tile < tiles; ++tile)
        {
            const auto & terrain = inventory.terrains->at(tile);
            if (terrain.tile != static_cast<std::int32_t>(tile) || terrain.layers.empty() ||
                terrain.layers.front().type.isEmpty() ||
                terrain.layers.front().type.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
            {
                return {};
            }
            names[tile] = &terrain.layers.front().type;
        }
        for (const auto & property : inventory.properties)
        {
            if (property.tile < 0 || static_cast<std::size_t>(property.tile) >= tiles ||
                properties[property.tile] || property.type.isEmpty() ||
                property.type.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
            {
                return {};
            }
            properties[property.tile] = true;
            names[property.tile] = &property.type;
        }
        std::size_t cached = 0;
        std::int64_t cachedCharacters = 0;
        for (std::size_t tile = 0; tile < tiles; ++tile)
        {
            const auto & name = *names[tile];
            const auto lookup = MOVEMENT_FIELD_ENTRY_WORK * (cached + 1) +
                MOVEMENT_CACHE_CHARACTER_WORK * (name.size() * cached + cachedCharacters);
            if (!ledger.consume(lookup, RefinementWork::MasterPreparation))
            {
                return {GuardedNativeBindingStatus::BudgetExhausted};
            }
            std::size_t index = 0;
            while (index < cached && *cache[index].name != name)
            {
                ++index;
            }
            if (index == cached)
            {
                const auto value = guardedNativeIntegerTableValue(table, name, ledger);
                if (value.status != GuardedNativeBindingStatus::Bound)
                {
                    return {value.status};
                }
                const auto cost = value.value.value_or(-1);
                if (cost == 0 || cost < -1 || cost > GUARDED_NATIVE_MAX_MOVEMENT)
                {
                    return {};
                }
                cache[cached++] = {&name, cost};
                cachedCharacters += name.size();
            }
            field[tile] = cache[index].cost;
        }
        return {GuardedNativeBindingStatus::Bound, std::move(field)};
    }

    std::size_t guardedNativeDeclarationCount()
    {
        return CODE_ASSETS.size();
    }

    std::int64_t guardedNativeRealmConstructionWork()
    {
        return REALM_CONSTRUCTION_WORK + VALUE_HELPER_CONSTRUCTION_WORK +
            CACHE_CONTROL_WORK * CODE_ASSETS.size();
    }

    std::int64_t guardedNativeDeclarationLookupWork()
    {
        return ASSET_LOOKUP_WORK;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::prepareCallbackObservation(QJSEngine & engine)
    {
        if (!m_data) { return m_initializationStatus; }
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (m_observedEngine != &engine) { return GuardedNativeBindingStatus::Bound; }
        if (!m_callbackGlobals)
        {
            m_callbackGlobals.emplace(prepareGuardedCommandCallbackGlobals(engine, m_ledger));
        }
        if (m_callbackGlobals->status() == GuardedScriptPropertyStatus::Found)
        {
            return GuardedNativeBindingStatus::Bound;
        }
        return propertyStatus(m_callbackGlobals->status());
    }

    GuardedCommandOperand GuardedNativeCodeCatalog::callbackOperand(
        QJSEngine & engine, const GuardedScriptValueView & value, GuardedNativeForwarder forwarder)
    {
        const auto status = prepareCallbackObservation(engine);
        if (status != GuardedNativeBindingStatus::Bound) { return {status, {}, false}; }
        return guardedCommandCallbackOperand(engine, m_data->engine, value, forwarder, m_ledger,
            m_observedEngine == &engine ? &*m_callbackGlobals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::rankingBonus(
        QJSEngine & engine, const GuardedScriptValueView & value, GuardedCommandRankingBonus kind)
    {
        const auto status = prepareCallbackObservation(engine);
        if (status != GuardedNativeBindingStatus::Bound) { return status; }
        return authenticateCommandRankingBonus(engine, value, kind, m_ledger,
            m_observedEngine == &engine ? &*m_callbackGlobals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::authenticateCommandPolicyEnvironment(QJSEngine & engine)
    {
        const auto status = prepareCallbackObservation(engine);
        if (status != GuardedNativeBindingStatus::Bound) { return status; }
        return authenticateGuardedCommandFunctions(engine, m_ledger,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr,
            m_observedEngine == &engine ? &*m_callbackGlobals : nullptr);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::authenticateCommandCallbackEnvironment(QJSEngine & engine, bool buildingCallbacks)
    {
        const auto status = prepareCallbackObservation(engine);
        if (status != GuardedNativeBindingStatus::Bound) { return status; }
        return authenticateGuardedActionFunctions(engine, m_data->engine, m_ledger,
            m_observedEngine == &engine ? &*m_callbackGlobals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr, buildingCallbacks);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::authenticateCommandRankingEnvironment(
        QJSEngine & engine, const GuardedNativeInventory & inventory)
    {
        const auto status = prepareCallbackObservation(engine);
        if (status != GuardedNativeBindingStatus::Bound) { return status; }
        return authenticateGuardedRankingFunctions(engine, inventory, m_ledger,
            m_observedEngine == &engine ? &*m_callbackGlobals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeIntrinsicAgreement GuardedNativeCodeCatalog::authenticateIntrinsics(QJSEngine & engine)
    {
        GuardedNativeIntrinsicAgreement result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        const auto work = INTRINSIC_CONTROL_WORK + INTRINSIC_RECORD_WORK * INTRINSICS.size();
        if (!m_ledger.consume(work, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.entries.reserve(INTRINSICS.size());
        result.status = GuardedNativeBindingStatus::Bound;
        for (const auto & descriptor : INTRINSICS)
        {
            const auto nameWork = MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * descriptor.member.size();
            if (!m_ledger.consume(nameWork, RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                break;
            }
            const auto name = QString::fromLatin1(descriptor.member.data(), descriptor.member.size());
            const auto actualNamespace = intrinsicNamespace(engine, descriptor.owner, m_ledger,
                m_observedEngine == &engine ? &m_globals : nullptr);
            if (actualNamespace.status != GuardedScriptPropertyStatus::Found)
            {
                result.status = propertyStatus(actualNamespace.status);
                break;
            }
            const auto approvedNamespace = intrinsicNamespace(m_data->engine, descriptor.owner, m_ledger, nullptr);
            if (approvedNamespace.status != GuardedScriptPropertyStatus::Found)
            {
                result.status = propertyStatus(approvedNamespace.status);
                break;
            }
            const auto actual = guardedScriptBuiltinProperty(actualNamespace.value, name, m_ledger);
            if (actual.status != GuardedScriptPropertyStatus::Found)
            {
                result.status = propertyStatus(actual.status);
                break;
            }
            const auto approved = guardedScriptBuiltinProperty(approvedNamespace.value, name, m_ledger);
            if (approved.status != GuardedScriptPropertyStatus::Found)
            {
                result.status = propertyStatus(approved.status);
                break;
            }
            auto actualImage = guardedScriptFunctionImage(actual.value, m_ledger, m_imageCache.get());
            if (actualImage.status != GuardedScriptFunctionStatus::Verified)
            {
                result.status = imageStatus(actualImage.status);
                break;
            }
            auto approvedImage =
                guardedScriptFunctionImage(approved.value, m_ledger, m_data->imageCache.get());
            if (approvedImage.status != GuardedScriptFunctionStatus::Verified)
            {
                result.status = imageStatus(approvedImage.status);
                break;
            }
            if (!m_ledger.consume(INTRINSIC_COMPARE_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                break;
            }
            if (actualImage.kind != GuardedScriptFunctionKind::Builtin ||
                approvedImage.kind != GuardedScriptFunctionKind::Builtin ||
                !actualImage.sameImplementation(approvedImage))
            {
                result.status = GuardedNativeBindingStatus::Unsupported;
                break;
            }
            result.entries.push_back({descriptor.kind, std::move(actualImage), std::move(approvedImage)});
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeCodeLookup GuardedNativeCodeCatalog::kindForGlobal(const QString & global)
    {
        if (!m_data)
        {
            return {m_initializationStatus};
        }
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        if (global.isEmpty() || global.size() > MAX_CODE_MEMBER)
        {
            return {};
        }
        const auto work = ASSET_NAME_SEARCH_DEPTH *
            (ASSET_INSPECTION_WORK + 2 * (global.size() + MAX_ASSET_NAME_LENGTH + 1));
        if (!m_ledger.consume(work, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted};
        }
        std::size_t first = 0;
        auto last = ASSET_NAME_ORDER.size();
        while (first < last)
        {
            const auto middle = first + (last - first) / 2;
            const auto & asset = CODE_ASSETS[ASSET_NAME_ORDER[middle]];
            const auto comparison = global.compare(QLatin1StringView(asset.global), Qt::CaseSensitive);
            if (comparison == 0)
            {
                return {GuardedNativeBindingStatus::Bound, asset.kind};
            }
            if (comparison < 0)
            {
                last = middle;
            }
            else
            {
                first = middle + 1;
            }
        }
        return {};
    }

    GuardedNativeFunctionAgreement GuardedNativeCodeCatalog::authenticateFunction(
        const GuardedScriptValueView & function, GuardedNativeCodeKind kind, const QString & member)
    {
        GuardedNativeFunctionAgreement result;
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        result.context = guardedScriptContextOperands(function, m_ledger);
        if (result.context.status != GuardedScriptContextOperands::GlobalScopeOnly)
        {
            if (result.context.status == GuardedScriptContextOperands::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        result.code = match(function, kind, member);
        result.status = result.code.status;
        return result;
    }

    GuardedNativeScalarAgreement GuardedNativeCodeCatalog::matchScalar(
        const GuardedScriptValueView & value, GuardedNativeCodeKind kind, std::span<const QString> path)
    {
        GuardedNativeScalarAgreement result;
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (path.empty() || path.size() > MAX_DATA_PATH)
        {
            return result;
        }
        std::int64_t characters = 0;
        for (const auto & name : path)
        {
            if (name.isEmpty() || name.size() > MAX_CODE_MEMBER)
            {
                return result;
            }
            characters += name.size();
        }
        const auto pathWork = SCALAR_PATH_ENTRY_WORK * path.size() +
            SCALAR_PATH_CHARACTER_WORK * characters;
        if (!m_ledger.consume(pathWork + ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return result;
        }
        result.actual = guardedScriptScalar(value, m_ledger);
        if (result.actual.status != GuardedScriptScalarStatus::Verified)
        {
            if (result.actual.status == GuardedScriptScalarStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        const auto loaded = m_data->loadCanonical(*asset, m_ledger);
        if (loaded != GuardedNativeBindingStatus::Bound)
        {
            result.status = loaded;
            return result;
        }
        const auto property = m_data->scalarProperty(*asset, path, m_ledger);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            if (property.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        result.approved = guardedScriptScalar(property.value, m_ledger);
        if (result.approved.status != GuardedScriptScalarStatus::Verified)
        {
            if (result.approved.status == GuardedScriptScalarStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        if (!m_ledger.consume(scalarComparisonWork(result.actual, result.approved),
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (sameScalar(result.actual, result.approved))
        {
            result.status = GuardedNativeBindingStatus::Bound;
        }
        return result;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::observeArrayPair(
        const GuardedScriptValueView & value, GuardedNativeCodeKind kind, std::span<const QString> path,
        std::vector<GuardedNativeDataNode> & actual, std::vector<GuardedNativeDataNode> & approved, QJSEngine* actualEngine)
    {
        if (!m_data)
        {
            return m_initializationStatus;
        }
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (path.empty() || path.size() > MAX_DATA_PATH)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        std::int64_t characters = 0;
        for (const auto & name : path)
        {
            if (name.isEmpty() || name.size() > MAX_CODE_MEMBER)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            characters += name.size();
        }
        const auto pathWork = SCALAR_PATH_ENTRY_WORK * path.size() + SCALAR_PATH_CHARACTER_WORK * characters;
        if (!m_ledger.consume(pathWork + ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto loaded = m_data->loadCanonical(*asset, m_ledger);
        if (loaded != GuardedNativeBindingStatus::Bound)
        {
            return loaded;
        }
        const auto property = m_data->scalarProperty(*asset, path, m_ledger);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            if (property.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            return GuardedNativeBindingStatus::Unsupported;
        }
        if (value.kind() != GuardedScriptValueKind::Managed ||
            property.value.kind() != GuardedScriptValueKind::Managed)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        auto status = collectPrimitiveData(value, 0, actual, m_ledger, actualEngine);
        if (status != GuardedNativeBindingStatus::Bound)
        {
            return status;
        }
        QJSEngine* approvedEngine = nullptr;
        if (actualEngine != nullptr)
        {
            approvedEngine = &m_data->engine;
        }
        {
            Data::ExpectedOperation operation(*m_data, m_ledger);
            if (!operation.funded())
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            status = collectPrimitiveData(property.value, 0, approved, m_ledger, approvedEngine);
        }
        if (status != GuardedNativeBindingStatus::Bound)
        {
            return status;
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::authenticateArray(
        const GuardedScriptValueView & value, GuardedNativeCodeKind kind, std::span<const QString> path,
        GuardedNativeArrayAgreement & result, QJSEngine* actualEngine)
    {
        const auto status = observeArrayPair(value, kind, path, result.actual, result.approved, actualEngine);
        if (status != GuardedNativeBindingStatus::Bound) { return status; }
        return comparePrimitiveData(result.actual, result.approved, m_ledger);
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::matchArray(
        const GuardedScriptValueView & value, GuardedNativeCodeKind kind, std::span<const QString> path)
    {
        GuardedNativeArrayAgreement result;
        const auto before = m_ledger.stats().spent();
        result.status = authenticateArray(value, kind, path, result);
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::matchArray(
        QJSEngine & engine, const GuardedScriptValueView & value,
        GuardedNativeCodeKind kind, std::span<const QString> path)
    {
        GuardedNativeArrayAgreement result;
        const auto before = m_ledger.stats().spent();
        result.status = authenticateArray(value, kind, path, result, &engine);
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeMemberAgreement GuardedNativeCodeCatalog::authenticateMember(
        const GuardedScriptValueView & value, GuardedNativeCodeKind kind, const QString & member)
    {
        GuardedNativeMemberAgreement result;
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(CACHE_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (member.isEmpty() || member.size() > MAX_CODE_MEMBER)
        {
            return result;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.size() + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.name = member;
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return result;
        }
        const auto loaded = m_data->loadForMember(*asset, value, m_ledger, m_imageCache.get());
        if (loaded != GuardedNativeBindingStatus::Bound)
        {
            result.status = loaded;
            return result;
        }
        const std::array path{member};
        const auto approved = m_data->scalarProperty(*asset, path, m_ledger);
        if (approved.status != GuardedScriptPropertyStatus::Found)
        {
            if (approved.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        if (approved.value.kind() != GuardedScriptValueKind::Managed)
        {
            auto scalar = matchScalar(value, kind, path);
            result.status = scalar.status;
            result.evidence = std::move(scalar);
            return result;
        }
        if (approved.prototypeDepth > MAX_DECLARED_PARENT_DEPTH)
        {
            return result;
        }
        const auto* owner = asset;
        for (std::uint32_t depth = 0; depth < approved.prototypeDepth; ++depth)
        {
            if (!m_ledger.consume(ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                return result;
            }
            owner = assetFor(owner->parent);
            if (owner == nullptr)
            {
                return result;
            }
        }
        auto function = authenticateFunction(value, owner->kind, member);
        result.status = function.status;
        result.evidence = std::move(function);
        return result;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckObservedMember(
        QJSEngine & engine, const GuardedScriptValueView & value, const GuardedNativeMemberAgreement & member)
    {
        if (const auto* definition = std::get_if<GuardedCommandRankingBonus>(&member.evidence))
        {
            const auto expected = guardedCommandRankingBonus(member.name);
            if (member.forwarder != GuardedNativeForwarder::None || !expected || *expected != *definition)
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            return rankingBonus(engine, value, *definition);
        }
        if (const auto* definition = std::get_if<GuardedCommandTerrainDefinition>(&member.evidence))
        {
            if (member.forwarder != GuardedNativeForwarder::None || member.name != QStringLiteral("getDefense"))
            {
                return GuardedNativeBindingStatus::Invalid;
            }
            return authenticateCommandTerrainDefinition(engine, value, *definition, m_ledger);
        }
        if (const auto* definition = std::get_if<GuardedCommandUnitDefinition>(&member.evidence))
        {
            if (member.forwarder != GuardedNativeForwarder::None) { return GuardedNativeBindingStatus::Invalid; }
            if (*definition == GuardedCommandUnitDefinition::MovementPostAction)
            {
                return m_data && member.name == QStringLiteral("postAction")
                    ? authenticateMovementPostAction(engine, m_data->engine, value, m_ledger) : GuardedNativeBindingStatus::Invalid;
            }
            const bool initializer = *definition == GuardedCommandUnitDefinition::InfantryInit ||
                *definition == GuardedCommandUnitDefinition::LightTankInit;
            const bool cost = *definition == GuardedCommandUnitDefinition::InfantryCost ||
                *definition == GuardedCommandUnitDefinition::LightTankCost;
            const auto name = initializer ? QStringLiteral("init") : cost ? QStringLiteral("getBaseCost") : QStringLiteral("getWeatherImmune");
            return member.name == name
                ? authenticateCommandUnitDefinition(engine, value, *definition, m_ledger,
                    m_observedEngine == &engine ? &*m_ownIndex : nullptr,
                    m_observedEngine == &engine && m_callbackGlobals ? &*m_callbackGlobals : nullptr) : GuardedNativeBindingStatus::Invalid;
        }
        if (guardedCommandCallbackBit(member.forwarder))
        {
            if (!m_data) { return GuardedNativeBindingStatus::Invalid; }
            const auto operand = callbackOperand(engine, value, member.forwarder);
            if (operand.status != GuardedNativeBindingStatus::Bound) { return operand.status; }
            return recheckMemberValue(operand.value, member, m_ledger, m_imageCache.get());
        }
        if (member.forwarder == GuardedNativeForwarder::None)
        {
            return recheckMemberValue(value, member, m_ledger, m_imageCache.get());
        }
        const bool wardenTrap = member.forwarder == GuardedNativeForwarder::WardenTrap;
        if (!m_data || (!wardenTrap && member.forwarder != GuardedNativeForwarder::CommandRepair) ||
            member.name != (wardenTrap ? QStringLiteral("isTrap") : QStringLiteral("repairUnit")))
        {
            return GuardedNativeBindingStatus::Invalid;
        }
        const auto operand = wardenTrap ? guardedMovementTrapOperand(engine, value, m_ledger)
            : guardedCommandRepairOperand(engine, m_data->engine, value, m_ledger,
                    m_observedEngine == &engine ? &*m_ownIndex : nullptr,
                    m_observedEngine == &engine && m_callbackGlobals ? &*m_callbackGlobals : nullptr);
        if (operand.status != GuardedNativeBindingStatus::Bound) { return operand.status; }
        return operand.wrapped ? recheckMemberValue(operand.value, member, m_ledger, m_imageCache.get())
                               : GuardedNativeBindingStatus::Unsupported;
    }

    GuardedNativeUnitCodeAgreement GuardedNativeCodeCatalog::authenticateUnitCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind, bool commandPolicy)
    {
        if (!isNativeUnitKind(kind))
        {
            return authenticateMembers(engine, kind, {});
        }
        if (commandPolicy && (kind == GuardedNativeCodeKind::Infantry || kind == GuardedNativeCodeKind::LightTank))
        {
            return authenticateMembers(engine, kind, COMMAND_UNIT_MEMBERS, true);
        }
        return authenticateMembers(engine, kind, UNIT_CALLBACK_MEMBERS, commandPolicy);
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockUnitHelpers(QJSEngine & engine, bool commandPolicy)
    {
        return authenticateMembers(engine, GuardedNativeCodeKind::Unit, STOCK_UNIT_HELPERS, commandPolicy);
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateDetailNoneCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::BattleAnimation:
                return authenticateMembers(engine, kind, DETAIL_BASE_MEMBERS);
            case GuardedNativeCodeKind::InfantryBattleAnimation:
                return authenticateMembers(engine, kind, DETAIL_INFANTRY_MEMBERS);
            case GuardedNativeCodeKind::SniperBattleAnimation:
                return authenticateMembers(engine, kind, DETAIL_SNIPER_MEMBERS);
            case GuardedNativeCodeKind::Terrain:
                return authenticateMembers(engine, kind, DETAIL_TERRAIN_HELPERS);
            case GuardedNativeCodeKind::Street:
                return authenticateMembers(engine, kind, DETAIL_STREET_HELPERS);
            case GuardedNativeCodeKind::BaseStreet:
                return authenticateMembers(engine, kind, DETAIL_STREET_MEMBERS);
            case GuardedNativeCodeKind::Infantry:
            case GuardedNativeCodeKind::Sniper:
                return authenticateMembers(engine, kind, DETAIL_UNIT_MEMBERS);
            case GuardedNativeCodeKind::Plains:
            case GuardedNativeCodeKind::Forest:
            case GuardedNativeCodeKind::Mountain:
                return authenticateMembers(engine, kind, DETAIL_TERRAIN_DISPATCH);
            case GuardedNativeCodeKind::Town:
            case GuardedNativeCodeKind::Factory:
            case GuardedNativeCodeKind::Headquarters:
                return authenticateMembers(engine, kind, DETAIL_UNIT_MEMBERS);
            default:
                return authenticateMembers(engine, GuardedNativeCodeKind::None, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateDetailNoneTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::BattleAnimation:
                return authenticateTables(engine, kind, DETAIL_SPOTTER_TABLE);
            case GuardedNativeCodeKind::InfantryBattleAnimation:
            case GuardedNativeCodeKind::SniperBattleAnimation:
                return authenticateTables(engine, kind, INFANTRY_PRESENTATION_TABLES);
            default:
                return authenticateTables(engine, GuardedNativeCodeKind::None, {});
        }
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::authenticateUnitActions(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        if (!isNativeUnitKind(kind))
        {
            return authenticateArrayMember(engine, GuardedNativeCodeKind::None, UNIT_ACTION_LIST);
        }
        return authenticateArrayMember(engine, kind, UNIT_ACTION_LIST);
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::authenticateCaptureBuildings(QJSEngine & engine)
    {
        return authenticateArrayMember(engine, GuardedNativeCodeKind::Capture, CAPTURABLE_BUILDINGS);
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::authenticateCaptureArmy(QJSEngine & engine)
    {
        return authenticateArrayMember(engine, GuardedNativeCodeKind::Capture, CAPTURE_ARMY_DATA);
    }

    GuardedNativeArrayAgreement GuardedNativeCodeCatalog::authenticateArrayMember(
        QJSEngine & engine, GuardedNativeCodeKind kind, const QString & member)
    {
        GuardedNativeArrayAgreement result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return result;
        }
        if (!m_ledger.consume(
                MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset->global),
                RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            if (global.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        const auto actual = moduleProperty(engine, kind, global.value, member);
        if (actual.status != GuardedScriptPropertyStatus::Found)
        {
            if (actual.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        const std::array path{member};
        result = matchArray(engine, actual.value, kind, path);
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockCoCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Co:
            case GuardedNativeCodeKind::Andy:
            case GuardedNativeCodeKind::Jess:
            case GuardedNativeCodeKind::TagPower:
                return authenticateMembers(engine, kind, STOCK_CO_CALLBACK_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateUnitPresentationTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Infantry:
                return authenticateTables(engine, kind, INFANTRY_PRESENTATION_TABLES);
            case GuardedNativeCodeKind::Sniper:
                return authenticateTables(engine, kind, UNIT_PRESENTATION_TABLES);
            case GuardedNativeCodeKind::Recon:
            case GuardedNativeCodeKind::LightTank:
            {
                GuardedNativeTableAgreement result;
                if (!m_data)
                {
                    result.status = m_initializationStatus;
                    return result;
                }
                const auto before = m_ledger.stats().spent();
                result.kind = kind;
                result.status = GuardedNativeBindingStatus::Bound;
                result.status = recheckTables(engine, result);
                result.chargedWork = m_ledger.stats().spent() - before;
                return result;
            }
            default:
                return authenticateTables(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockCoConsequences(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Co:
                return authenticateMembers(engine, kind, CO_CONSEQUENCE_MEMBERS);
            case GuardedNativeCodeKind::Andy:
                return authenticateMembers(engine, kind, ANDY_CONSEQUENCE_MEMBERS);
            case GuardedNativeCodeKind::Jess:
                return authenticateMembers(engine, kind, JESS_CONSEQUENCE_MEMBERS);
            case GuardedNativeCodeKind::TagPower:
                return authenticateMembers(engine, kind, CO_BATTLE_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockCoNominal(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Andy:
            case GuardedNativeCodeKind::Jess:
                return authenticateMembers(engine, kind, CO_NOMINAL_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockActionCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind, bool commandPolicy)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Action:
                return authenticateMembers(engine, kind, ACTION_LEGALITY_MEMBERS);
            case GuardedNativeCodeKind::Wait:
            case GuardedNativeCodeKind::NextPlayer:
                return authenticateMembers(engine, kind, STOCK_ACTION_MEMBERS, commandPolicy);
            case GuardedNativeCodeKind::Capture:
                return authenticateMembers(engine, kind, CAPTURE_MEMBERS, commandPolicy);
            case GuardedNativeCodeKind::Trap:
                return authenticateMembers(engine, kind, TRAP_PREDICATE_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateMovementTrapCallbacks(QJSEngine & engine)
    {
        return authenticateMembers(engine, GuardedNativeCodeKind::Trap, MOVEMENT_TRAP_MEMBERS, true);
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateCurrentFireCallbacks(QJSEngine & engine)
    {
        return authenticateMembers(engine, GuardedNativeCodeKind::Fire, CURRENT_FIRE_MEMBERS);
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateCurrentWeaponCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Weapon:
            case GuardedNativeCodeKind::InfantryMachineGun:
            case GuardedNativeCodeKind::SniperWeapon:
                return authenticateMembers(engine, kind, CURRENT_WEAPON_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateCurrentWeaponTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Weapon:
            case GuardedNativeCodeKind::InfantryMachineGun:
            case GuardedNativeCodeKind::SniperWeapon:
                return authenticateTables(engine, kind, CURRENT_WEAPON_TABLES);
            default:
                return authenticateTables(engine, kind, {});
        }
    }

    GuardedNativeAbsentHooks GuardedNativeCodeCatalog::authenticateCurrentAbsentHooks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Fire:
                return authenticateAbsentMembers(engine, kind, CURRENT_FIRE_ABSENT_HOOKS);
            case GuardedNativeCodeKind::Infantry:
            case GuardedNativeCodeKind::Sniper:
            case GuardedNativeCodeKind::Recon:
            case GuardedNativeCodeKind::LightTank:
                return authenticateAbsentMembers(engine, kind, CURRENT_UNIT_ABSENT_HOOKS);
            default:
                return authenticateAbsentMembers(engine, kind, {});
        }
    }

    GuardedNativeConstantLaw GuardedNativeCodeCatalog::authenticateConstantLaw(
        QJSEngine & engine, GuardedNativeCodeKind kind, GuardedNativeConstantGetter getter, bool commandPolicy)
    {
        GuardedNativeConstantLaw result;
        const auto before = m_ledger.stats().spent();
        if (!m_ledger.consume(CONSTANT_LAW_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        std::span<const std::string_view> members;
        std::string_view declaration;
        if (getter == GuardedNativeConstantGetter::UnitCost &&
            (kind == GuardedNativeCodeKind::Infantry || kind == GuardedNativeCodeKind::Sniper ||
             kind == GuardedNativeCodeKind::Recon || kind == GuardedNativeCodeKind::LightTank))
        {
            members = UNIT_COST_MEMBER;
            declaration = UNIT_COST_DECLARATION;
        }
        else if (getter == GuardedNativeConstantGetter::PropertyIncome &&
                 (kind == GuardedNativeCodeKind::Building || kind == GuardedNativeCodeKind::Town ||
                  kind == GuardedNativeCodeKind::Factory || kind == GuardedNativeCodeKind::Headquarters))
        {
            members = PROPERTY_INCOME_MEMBER;
            declaration = PROPERTY_INCOME_DECLARATION;
        }
        else
        {
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        auto agreement = authenticateMembers(engine, kind, members, commandPolicy);
        result.status = agreement.status;
        if (agreement.status == GuardedNativeBindingStatus::Bound && agreement.members.size() == 1 &&
            std::holds_alternative<GuardedCommandUnitDefinition>(agreement.members[0].evidence) &&
            (std::get<GuardedCommandUnitDefinition>(agreement.members[0].evidence) == GuardedCommandUnitDefinition::InfantryCost ||
             std::get<GuardedCommandUnitDefinition>(agreement.members[0].evidence) == GuardedCommandUnitDefinition::LightTankCost))
        {
            // Both verified records contain only LoadInt followed by Ret.
            result.value = std::get<GuardedCommandUnitDefinition>(agreement.members[0].evidence) == GuardedCommandUnitDefinition::InfantryCost
                ? GUARDED_AWDS_INFANTRY_COST : GUARDED_AWDS_LIGHT_TANK_COST;
            result.callback = std::move(agreement.members.front());
        }
        else if (agreement.status == GuardedNativeBindingStatus::Bound && agreement.members.size() == 1 &&
            std::holds_alternative<GuardedNativeFunctionAgreement>(agreement.members[0].evidence))
        {
            const auto & function = std::get<GuardedNativeFunctionAgreement>(agreement.members[0].evidence);
            if (!m_ledger.consume(ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            else
            {
                const auto* owner = assetFor(function.code.ownerKind);
                result.status = GuardedNativeBindingStatus::Unsupported;
                if (owner && !owner->source.empty())
                {
                    if (!m_ledger.consume(CONSTANT_LAW_SOURCE_BYTE_WORK * owner->source.size(),
                                          RefinementWork::MasterPreparation))
                    {
                        result.status = GuardedNativeBindingStatus::BudgetExhausted;
                    }
                    else if (const auto value = literalGetter(owner->source, declaration))
                    {
                        result.value = *value;
                        result.callback = std::move(agreement.members[0]);
                        result.status = GuardedNativeBindingStatus::Bound;
                    }
                }
            }
        }
        else if (result.status == GuardedNativeBindingStatus::Bound)
        {
            result.status = GuardedNativeBindingStatus::Unsupported;
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeAbsentHooks GuardedNativeCodeCatalog::authenticateAbsentMembers(
        QJSEngine & engine, GuardedNativeCodeKind kind, std::span<const std::string_view> members)
    {
        GuardedNativeAbsentHooks result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.kind = kind;
        if (members.empty())
        {
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK +
                              MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset->global) +
                              ABSENT_HOOK_RETENTION_WORK * members.size(),
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            result.status = propertyStatus(global.status);
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        result.members.reserve(members.size());
        for (const auto name : members)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * name.size(),
                                  RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                break;
            }
            auto member = QString::fromLatin1(name.data(), name.size());
            const auto actual = moduleProperty(engine, kind, global.value, member);
            if (actual.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                break;
            }
            if (actual.status != GuardedScriptPropertyStatus::Missing &&
                (actual.status != GuardedScriptPropertyStatus::Found ||
                 actual.value.kind() != GuardedScriptValueKind::Undefined))
            {
                break;
            }
            result.members.push_back({std::move(member), actual.status == GuardedScriptPropertyStatus::Found});
        }
        if (result.members.size() == members.size())
        {
            result.status = GuardedNativeBindingStatus::Bound;
        }
        else
        {
            result.members.clear();
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockPropertyCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind, bool commandPolicy)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Building:
                return authenticateMembers(engine, kind, BUILDING_HELPER_MEMBERS);
            case GuardedNativeCodeKind::Town:
            case GuardedNativeCodeKind::Factory:
            case GuardedNativeCodeKind::Headquarters:
                return authenticateMembers(engine, kind, PROPERTY_CALLBACK_MEMBERS, commandPolicy);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateStockPropertyTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Building:
                return authenticateTables(engine, kind, BUILDING_TABLE_MEMBERS);
            case GuardedNativeCodeKind::Town:
            case GuardedNativeCodeKind::Factory:
                return authenticateTables(engine, kind, PROPERTY_TABLE_MEMBERS);
            case GuardedNativeCodeKind::Headquarters:
                return authenticateTables(engine, kind, HEADQUARTERS_TABLE_MEMBERS);
            default:
                return authenticateTables(engine, kind, {});
        }
    }

    namespace
    {
        GuardedNativeBindingStatus asciiArmyText(const QString & value, RefinementLedger & ledger)
        {
            if (value.size() > MAX_CODE_MEMBER) { return GuardedNativeBindingStatus::Unsupported; }
            if (!ledger.consume(TABLE_LOOKUP_CONTROL_WORK + ARMY_TEXT_WORK * (value.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            for (const auto character : value)
            {
                if (character.unicode() > 0x7f) { return GuardedNativeBindingStatus::Unsupported; }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus armyTableShape(
            const std::vector<GuardedNativeDataNode> & nodes, RefinementLedger & ledger)
        {
            if (nodes.empty() || nodes.size() > MAX_PRIMITIVE_DATA_NODES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (!ledger.consume(TABLE_LOOKUP_CONTROL_WORK + TABLE_LOOKUP_NODE_WORK * nodes.size(),
                                RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (nodes[0].kind != GuardedNativeDataKind::Array || nodes[0].elements > (nodes.size() - 1) / 3 ||
                nodes.size() != 1 + 3 * static_cast<std::size_t>(nodes[0].elements))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            for (std::size_t row = 0; row < nodes[0].elements; ++row)
            {
                const auto start = 1 + 3 * row;
                if (nodes[start].kind != GuardedNativeDataKind::Array || nodes[start].elements != 2)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                for (const auto column : {1u, 2u})
                {
                    const auto & entry = nodes[start + column];
                    const auto* value = std::get_if<QString>(&entry.scalar.value);
                    if (entry.kind != GuardedNativeDataKind::Scalar ||
                        entry.scalar.status != GuardedScriptScalarStatus::Verified ||
                        entry.scalar.kind != GuardedScriptValueKind::String || !value)
                    {
                        return GuardedNativeBindingStatus::Unsupported;
                    }
                    const auto status = asciiArmyText(*value, ledger);
                    if (status != GuardedNativeBindingStatus::Bound) { return status; }
                }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus armyTableResult(
            const std::vector<GuardedNativeDataNode> & nodes, const QString & lowerArmy,
            QString & result, RefinementLedger & ledger)
        {
            if (!ledger.consume(TABLE_LOOKUP_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            result = STOCK_ARMY_FALLBACK;
            for (std::size_t row = 0; row < nodes[0].elements; ++row)
            {
                const auto start = 1 + 3 * row;
                const auto & key = std::get<QString>(nodes[start + 1].scalar.value);
                if (!ledger.consume(TABLE_LOOKUP_NODE_WORK +
                        TABLE_LOOKUP_CHARACTER_WORK * (key.size() + lowerArmy.size() + 1),
                                    RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (key == lowerArmy)
                {
                    const auto & value = std::get<QString>(nodes[start + 2].scalar.value);
                    const auto status = asciiArmyText(value, ledger);
                    if (status != GuardedNativeBindingStatus::Bound) { return status; }
                    result = value.toLower();
                    break;
                }
            }
            return GuardedNativeBindingStatus::Bound;
        }

        GuardedNativeBindingStatus sameArmyText(
            const QString & actual, const QString & expected, RefinementLedger & ledger)
        {
            if (actual.size() > MAX_CODE_MEMBER || expected.size() > MAX_CODE_MEMBER)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            if (!ledger.consume(TABLE_LOOKUP_CONTROL_WORK +
                    TABLE_LOOKUP_CHARACTER_WORK * (actual.size() + expected.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            return actual == expected ? GuardedNativeBindingStatus::Bound : GuardedNativeBindingStatus::Unsupported;
        }

        GuardedNativeBindingStatus compareArmyTableResults(
            const std::vector<GuardedNativeDataNode> & actual, const std::vector<GuardedNativeDataNode> & approved,
            const std::array<QString, 2> & lowerArmies, std::array<QString, 2> & results, RefinementLedger & ledger)
        {
            for (const auto* nodes : {&actual, &approved})
            {
                const auto status = armyTableShape(*nodes, ledger);
                if (status != GuardedNativeBindingStatus::Bound) { return status; }
            }
            for (std::size_t player = 0; player < lowerArmies.size(); ++player)
            {
                QString expected;
                auto status = armyTableResult(actual, lowerArmies[player], results[player], ledger);
                if (status == GuardedNativeBindingStatus::Bound)
                {
                    status = armyTableResult(approved, lowerArmies[player], expected, ledger);
                }
                if (status == GuardedNativeBindingStatus::Bound)
                {
                    status = sameArmyText(results[player], expected, ledger);
                }
                if (status != GuardedNativeBindingStatus::Bound) { return status; }
            }
            return GuardedNativeBindingStatus::Bound;
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateHeadquartersGameplayTables(QJSEngine & engine)
    {
        return authenticateTables(engine, GuardedNativeCodeKind::Headquarters, PROPERTY_TABLE_MEMBERS);
    }

    GuardedNativeArmyTableAgreement GuardedNativeCodeCatalog::authenticateHeadquartersArmyTables(
        QJSEngine & engine, const std::array<GuardedNativePlayerMaterial, 2> & players)
    {
        GuardedNativeArmyTableAgreement result;
        const auto before = m_ledger.stats().spent();
        const auto finish = [&](GuardedNativeBindingStatus status) {
            result.status = status;
            result.chargedWork = m_ledger.stats().spent() - before;
            return std::move(result);
        };
        if (!m_data) { return finish(m_initializationStatus); }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK +
                ARMY_TABLE_RETENTION_WORK * HEADQUARTERS_EXTRA_TABLE_MEMBERS.size(),
                             RefinementWork::MasterPreparation))
        {
            return finish(GuardedNativeBindingStatus::BudgetExhausted);
        }
        std::array<QString, 2> lowerArmies;
        for (std::size_t player = 0; player < players.size(); ++player)
        {
            if (players[player].army.isEmpty()) { return finish(GuardedNativeBindingStatus::Unsupported); }
            const auto status = asciiArmyText(players[player].army, m_ledger);
            if (status != GuardedNativeBindingStatus::Bound) { return finish(status); }
            result.armies[player] = players[player].army;
            lowerArmies[player] = players[player].army.toLower();
        }
        const auto global = globalProperty(engine, QStringLiteral("HQ"));
        if (global.status != GuardedScriptPropertyStatus::Found) { return finish(propertyStatus(global.status)); }
        result.tables.reserve(HEADQUARTERS_EXTRA_TABLE_MEMBERS.size());
        for (const auto member : HEADQUARTERS_EXTRA_TABLE_MEMBERS)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.size(),
                                 RefinementWork::MasterPreparation))
            {
                return finish(GuardedNativeBindingStatus::BudgetExhausted);
            }
            const auto name = QString::fromLatin1(member.data(), member.size());
            const auto property = moduleProperty(engine, GuardedNativeCodeKind::Headquarters, global.value, name);
            if (property.status != GuardedScriptPropertyStatus::Found) { return finish(propertyStatus(property.status)); }
            const std::array path{name};
            std::vector<GuardedNativeDataNode> actual, approved;
            auto status = observeArrayPair(property.value, GuardedNativeCodeKind::Headquarters, path, actual, approved, &engine);
            if (status != GuardedNativeBindingStatus::Bound) { return finish(status); }
            auto & table = result.tables.emplace_back();
            table.name = name;
            status = compareArmyTableResults(actual, approved, lowerArmies, table.results, m_ledger);
            if (status != GuardedNativeBindingStatus::Bound) { return finish(status); }
        }
        return finish(GuardedNativeBindingStatus::Bound);
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckHeadquartersArmyTables(
        QJSEngine & engine, const GuardedNativeArmyTableAgreement & prior,
        const std::array<GuardedNativePlayerMaterial, 2> & players)
    {
        if (!m_ledger.consume(TABLE_LOOKUP_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (prior.status != GuardedNativeBindingStatus::Bound || prior.tables.size() != HEADQUARTERS_EXTRA_TABLE_MEMBERS.size())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        for (std::size_t player = 0; player < players.size(); ++player)
        {
            const auto status = sameArmyText(players[player].army, prior.armies[player], m_ledger);
            if (status != GuardedNativeBindingStatus::Bound) { return status; }
        }
        const auto fresh = authenticateHeadquartersArmyTables(engine, players);
        if (fresh.status != GuardedNativeBindingStatus::Bound) { return fresh.status; }
        for (std::size_t index = 0; index < fresh.tables.size(); ++index)
        {
            auto status = sameArmyText(fresh.tables[index].name, prior.tables[index].name, m_ledger);
            if (status != GuardedNativeBindingStatus::Bound) { return status; }
            for (std::size_t player = 0; player < players.size(); ++player)
            {
                status = sameArmyText(fresh.tables[index].results[player], prior.tables[index].results[player], m_ledger);
                if (status != GuardedNativeBindingStatus::Bound) { return status; }
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockMovementCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::MovementTable:
                return authenticateMembers(engine, kind, MOVEMENT_HELPER_MEMBERS);
            case GuardedNativeCodeKind::MoveFeet:
            case GuardedNativeCodeKind::MoveTireA:
            case GuardedNativeCodeKind::MoveTank:
                return authenticateMembers(engine, kind, MOVEMENT_CALLBACK_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateStockMovementTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::MoveFeet:
            case GuardedNativeCodeKind::MoveTireA:
            case GuardedNativeCodeKind::MoveTank:
                return authenticateTables(engine, kind, MOVEMENT_TABLE_MEMBERS);
            default:
                return authenticateTables(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateDefaultArmies(
        QJSEngine & engine, std::array<GuardedNativePlayerMaterial, 2> & players)
    {
        const auto before = m_ledger.stats().spent();
        auto result = authenticateMembers(engine, GuardedNativeCodeKind::Player, DEFAULT_ARMY_MEMBERS);
        std::array<QString, 2> resolved;
        if (result.status == GuardedNativeBindingStatus::Bound)
        {
            result.status = readDefaultArmies(engine, players, resolved);
        }
        if (result.status == GuardedNativeBindingStatus::Bound)
        {
            for (std::size_t side = 0; side < players.size(); ++side)
            {
                if (players[side].defaultArmyPlayerId)
                {
                    players[side].army = std::move(resolved[side]);
                }
            }
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckDefaultArmies(
        QJSEngine & engine, const GuardedNativeModuleAgreement & callbacks,
        const std::array<GuardedNativePlayerMaterial, 2> & players, std::array<QString, 2> & resolved)
    {
        const auto status = recheckModule(engine, callbacks);
        return status == GuardedNativeBindingStatus::Bound ? readDefaultArmies(engine, players, resolved) : status;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::readDefaultArmies(
        QJSEngine & engine, const std::array<GuardedNativePlayerMaterial, 2> & players, std::array<QString, 2> & resolved)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + 2 * GUARDED_NATIVE_BUILD_ENTRY_WORK,
                              RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto global = globalProperty(engine, QStringLiteral("PLAYER"));
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        const auto property = moduleProperty(engine, GuardedNativeCodeKind::Player, global.value, PLAYER_ARMIES);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(property.status);
        }
        const auto array = guardedScriptArray(property.value, m_ledger);
        if (array.status != GuardedScriptPropertyStatus::Found || array.length == 0)
        {
            return propertyStatus(array.status);
        }
        for (std::size_t side = 0; side < players.size(); ++side)
        {
            const auto & player = players[side];
            if (!player.defaultArmyPlayerId)
            {
                if (player.army.isEmpty())
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                continue;
            }
            if (*player.defaultArmyPlayerId != static_cast<std::int32_t>(side))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto element = guardedScriptArrayElement(
                property.value, static_cast<std::uint32_t>(side) % array.length, m_ledger);
            if (element.status != GuardedScriptPropertyStatus::Found)
            {
                return propertyStatus(element.status);
            }
            const auto scalar = guardedScriptScalar(element.value, m_ledger);
            const auto* value = std::get_if<QString>(&scalar.value);
            if (scalar.status != GuardedScriptScalarStatus::Verified || value == nullptr ||
                value->isEmpty() || value->size() > GUARDED_NATIVE_MAX_ID_LENGTH)
            {
                return scalar.status == GuardedScriptScalarStatus::BudgetExhausted
                    ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
            }
            resolved[side] = *value;
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockContextCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind, bool commandPolicy)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Terrain:
                return authenticateMembers(engine, kind, BASE_TERRAIN_CONTEXT_MEMBERS);
            case GuardedNativeCodeKind::Player:
                return authenticateMembers(engine, kind, PLAYER_PHASE_MEMBERS, commandPolicy);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateCommandTerrainCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        if (kind == GuardedNativeCodeKind::Forest || kind == GuardedNativeCodeKind::BaseForest)
        {
            return authenticateMembers(engine, kind, TERRAIN_BASE_MEMBERS, true);
        }
        return authenticateStockTerrainCallbacks(engine, kind);
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockTerrainCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Terrain:
                return authenticateMembers(engine, kind, BASE_TERRAIN_CONTEXT_MEMBERS);
            case GuardedNativeCodeKind::BaseForest:
            case GuardedNativeCodeKind::BaseMountain:
            case GuardedNativeCodeKind::BaseStreet:
            case GuardedNativeCodeKind::Forest:
            case GuardedNativeCodeKind::Mountain:
            case GuardedNativeCodeKind::Street:
                return authenticateMembers(engine, kind, TERRAIN_BASE_MEMBERS);
            case GuardedNativeCodeKind::Plains:
            case GuardedNativeCodeKind::Sea:
            case GuardedNativeCodeKind::Reaf:
                return authenticateMembers(engine, kind, TERRAIN_CALLBACK_MEMBERS);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeWeatherPrefixAgreement GuardedNativeCodeCatalog::authenticateStockWeatherPrefix(QJSEngine & engine)
    {
        GuardedNativeWeatherPrefixAgreement result;
        const auto before = m_ledger.stats().spent();
        const auto finish = [&](GuardedNativeBindingStatus status) {
            result.status = status;
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        };
        if (!m_data) { return finish(m_initializationStatus); }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK +
                MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * TERRAIN_CONTEXT_TABLES.front().size(),
                             RefinementWork::MasterPreparation))
        {
            return finish(GuardedNativeBindingStatus::BudgetExhausted);
        }
        const auto global = globalProperty(engine, QStringLiteral("TERRAIN"));
        if (global.status != GuardedScriptPropertyStatus::Found) { return finish(propertyStatus(global.status)); }
        const auto member = TERRAIN_CONTEXT_TABLES.front();
        const auto name = QString::fromLatin1(member.data(), member.size());
        const auto property = moduleProperty(engine, GuardedNativeCodeKind::Terrain, global.value, name);
        if (property.status != GuardedScriptPropertyStatus::Found) { return finish(propertyStatus(property.status)); }
        const std::array path{name};
        std::vector<GuardedNativeDataNode> actual, approved;
        const auto observed = observeArrayPair(property.value, GuardedNativeCodeKind::Terrain, path, actual, approved, &engine);
        if (observed != GuardedNativeBindingStatus::Bound) { return finish(observed); }
        if (!m_ledger.consume(DATA_COMPARISON_WORK, RefinementWork::MasterPreparation))
        {
            return finish(GuardedNativeBindingStatus::BudgetExhausted);
        }
        if (approved.empty() || actual.size() < approved.size() ||
            actual.front().kind != GuardedNativeDataKind::Array || approved.front().kind != GuardedNativeDataKind::Array ||
            actual.front().elements < approved.front().elements)
        {
            return finish(GuardedNativeBindingStatus::Unsupported);
        }
        constexpr std::size_t WEATHER_ROW_NODES = 6;
        if (actual.size() != 1 + WEATHER_ROW_NODES * actual.front().elements)
        {
            return finish(GuardedNativeBindingStatus::Unsupported);
        }
        for (std::size_t row = 1; row < actual.size(); row += WEATHER_ROW_NODES)
        {
            if (!m_ledger.consume(DATA_COMPARISON_WORK * WEATHER_ROW_NODES, RefinementWork::MasterPreparation))
            {
                return finish(GuardedNativeBindingStatus::BudgetExhausted);
            }
            if (actual[row].kind != GuardedNativeDataKind::Array || actual[row].elements != 2 ||
                actual[row + 2].kind != GuardedNativeDataKind::Array || actual[row + 2].elements != 3 ||
                actual[row + 3].kind != GuardedNativeDataKind::Point)
            {
                return finish(GuardedNativeBindingStatus::Unsupported);
            }
            for (const auto offset : {1u, 4u, 5u})
            {
                const auto & cell = actual[row + offset];
                if (cell.kind != GuardedNativeDataKind::Scalar ||
                    cell.scalar.status != GuardedScriptScalarStatus::Verified ||
                    !std::holds_alternative<QString>(cell.scalar.value))
                {
                    return finish(GuardedNativeBindingStatus::Unsupported);
                }
            }
        }
        return finish(comparePrimitiveData(
            std::span<const GuardedNativeDataNode>(actual).subspan(1, approved.size() - 1),
            std::span<const GuardedNativeDataNode>(approved).subspan(1), m_ledger));
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::recheckStockWeatherPrefix(
        QJSEngine & engine, const GuardedNativeWeatherPrefixAgreement & prior)
    {
        if (!m_ledger.consume(TABLE_LOOKUP_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (prior.status != GuardedNativeBindingStatus::Bound) { return GuardedNativeBindingStatus::Invalid; }
        return authenticateStockWeatherPrefix(engine).status;
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateStockContextTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Terrain:
                return authenticateTables(engine, kind, TERRAIN_CONTEXT_TABLES);
            case GuardedNativeCodeKind::Player:
                return authenticateTables(engine, kind, PLAYER_CONTEXT_TABLES);
            default:
                return authenticateTables(engine, kind, {});
        }
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateStockPolicyCallbacks(
        QJSEngine & engine, GuardedNativeCodeKind kind, bool commandPolicy)
    {
        switch (kind)
        {
            case GuardedNativeCodeKind::Weather:
            case GuardedNativeCodeKind::Sun:
                return authenticateMembers(engine, kind, WEATHER_CALLBACK_MEMBERS);
            case GuardedNativeCodeKind::VictoryRule:
                return authenticateMembers(engine, kind, VICTORY_CALLBACK_MEMBERS);
            case GuardedNativeCodeKind::NoHeadquarters:
                return authenticateMembers(engine, kind, HQ_VICTORY_MEMBERS);
            case GuardedNativeCodeKind::NoUnits:
                return authenticateMembers(engine, kind, UNIT_VICTORY_MEMBERS);
            case GuardedNativeCodeKind::Ranking:
                return authenticateMembers(engine, kind, RANKING_CALLBACK_MEMBERS, commandPolicy);
            case GuardedNativeCodeKind::Global:
                return authenticateMembers(engine, kind, GLOBAL_CALLBACK_MEMBERS, commandPolicy);
            default:
                return authenticateMembers(engine, kind, {});
        }
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateStockPolicyTables(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        if (kind == GuardedNativeCodeKind::Ranking)
        {
            return authenticateTables(engine, kind, RANKING_TABLE_MEMBERS);
        }
        return authenticateTables(engine, kind, {});
    }

    GuardedNativeConstructionAgreement GuardedNativeCodeCatalog::authenticateConstructionQuery(
        QJSEngine & engine, const QString & type)
    {
        GuardedNativeConstructionAgreement result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        result.status = inspectConstructionQuery(engine, type, result);
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeConstructionType GuardedNativeCodeCatalog::constructionType(GuardedNativeCodeKind kind)
    {
        GuardedNativeConstructionType result;
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (kind != GuardedNativeCodeKind::RepairTank && kind != GuardedNativeCodeKind::TankHunter)
        {
            return result;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return result;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * MAX_CODE_MEMBER,
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
        }
        else
        {
            result.type = QString::fromLatin1(asset->global);
            result.status = GuardedNativeBindingStatus::Bound;
        }
        return result;
    }

    GuardedNativeConstructionAgreement GuardedNativeCodeCatalog::authenticateConstructionQuery(
        QJSEngine & engine, GuardedNativeCodeKind kind)
    {
        GuardedNativeConstructionAgreement result;
        const auto before = m_ledger.stats().spent();
        const auto target = constructionType(kind);
        result.status = target.status;
        if (target.status == GuardedNativeBindingStatus::Bound)
        {
            result.status = inspectConstructionQuery(engine, target.type, result);
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::inspectConstructionQuery(
        QJSEngine & engine, const QString & type, GuardedNativeConstructionAgreement & result)
    {
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (type.isEmpty() || type.size() > MAX_CODE_MEMBER)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK *
                (type.size() + CO_SPECIFIC_UNIT_MEMBER.size()), RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        result.type = type;
        const auto global = constructionProperty(engine, type);
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(global.status);
        }
        const auto member = QString::fromLatin1(CO_SPECIFIC_UNIT_MEMBER.data(), CO_SPECIFIC_UNIT_MEMBER.size());
        const auto property = guardedScriptProperty(global.value, member, m_ledger,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
        if (property.status != GuardedScriptPropertyStatus::Found)
        {
            return propertyStatus(property.status);
        }
        auto kind = GuardedNativeCodeKind::Unit;
        if (property.value.kind() != GuardedScriptValueKind::Null)
        {
            const auto lookup = kindForGlobal(type);
            if (lookup.status != GuardedNativeBindingStatus::Bound)
            {
                return lookup.status;
            }
            kind = lookup.kind;
            if (kind != GuardedNativeCodeKind::RepairTank && kind != GuardedNativeCodeKind::TankHunter)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
        }
        result.callback = authenticateMember(property.value, kind, member);
        if (result.callback.status == GuardedNativeBindingStatus::Bound)
        {
            result.coSpecific = kind != GuardedNativeCodeKind::Unit;
        }
        return result.callback.status;
    }

    GuardedNativeTableAgreement GuardedNativeCodeCatalog::authenticateTables(
        QJSEngine & engine, GuardedNativeCodeKind kind, std::span<const std::string_view> members)
    {
        GuardedNativeTableAgreement result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (!members.empty())
        {
            result.kind = kind;
            result.tables.reserve(members.size());
            for (const auto member : members)
            {
                if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * member.size(),
                                      RefinementWork::MasterPreparation))
                {
                    result.status = GuardedNativeBindingStatus::BudgetExhausted;
                    break;
                }
                auto name = QString::fromLatin1(member.data(), member.size());
                auto evidence = authenticateArrayMember(engine, kind, name);
                const auto status = evidence.status;
                result.tables.push_back({std::move(name), std::move(evidence)});
                result.status = status;
                if (status != GuardedNativeBindingStatus::Bound)
                {
                    break;
                }
            }
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeBindingStatus GuardedNativeCodeCatalog::prepareModuleCompilation(
        QJSEngine & engine, GuardedNativeCodeKind kind, const GuardedScriptValueView & global,
        std::span<const std::string_view> members, bool commandPolicy)
    {
        if (!m_ledger.consume(CACHE_CONTROL_WORK + ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        if (m_observedEngine != &engine || m_data->loadedNames[static_cast<std::size_t>(kind) - 1])
        {
            return GuardedNativeBindingStatus::Bound;
        }
        // The initializer and price can belong to a separate program. Choose an independently
        // checked own stock member as compiler URI hint, including for a price-only query.
        constexpr std::array unitHint{std::string_view{"getMovementType"}};
        if (commandPolicy && (kind == GuardedNativeCodeKind::Infantry || kind == GuardedNativeCodeKind::LightTank))
        {
            members = unitHint;
        }
        constexpr std::array actionHint{std::string_view{"isFinalStep"}};
        if (commandPolicy && (kind == GuardedNativeCodeKind::Wait || kind == GuardedNativeCodeKind::Capture ||
                              kind == GuardedNativeCodeKind::NextPlayer))
        {
            members = actionHint;
        }
        constexpr std::array playerHint{std::string_view{"getDefaultArmy"}};
        if (commandPolicy && kind == GuardedNativeCodeKind::Player) { members = playerHint; }
        constexpr std::array globalHint{std::string_view{"getArmyNameFromTable"}};
        if (commandPolicy && kind == GuardedNativeCodeKind::Global) { members = globalHint; }
        constexpr std::array trapHint{std::string_view{"isStillATrap"}};
        if (commandPolicy && kind == GuardedNativeCodeKind::Trap) { members = trapHint; }
        for (const auto name : members)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * name.size(),
                                  RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            const auto member = QString::fromLatin1(name.data(), name.size());
            // The wrapped function's compilation URI belongs to the command script.
            // The normal member loader must use its checked stock delegate for this entry.
            if (commandPolicy && name == "repairUnit")
            {
                continue;
            }
            const auto value = moduleProperty(engine, kind, global, member);
            if (value.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (value.status != GuardedScriptPropertyStatus::Found || value.prototypeDepth != 0 ||
                value.value.kind() != GuardedScriptValueKind::Managed)
            {
                continue;
            }
            const auto image = guardedScriptFunctionImage(value.value, m_ledger, m_imageCache.get());
            if (image.status == GuardedScriptFunctionStatus::BudgetExhausted)
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (image.status != GuardedScriptFunctionStatus::Verified ||
                image.kind != GuardedScriptFunctionKind::Script)
            {
                continue;
            }
            const auto parent = m_data->loadParent(*asset, m_ledger);
            if (parent != GuardedNativeBindingStatus::Bound)
            {
                return parent;
            }
            return m_data->load(*asset, image.compilationName, m_ledger);
        }
        return GuardedNativeBindingStatus::Bound;
    }

    GuardedNativeModuleAgreement GuardedNativeCodeCatalog::authenticateMembers(
        QJSEngine & engine, GuardedNativeCodeKind kind, std::span<const std::string_view> members, bool commandPolicy)
    {
        GuardedNativeModuleAgreement result;
        const auto before = m_ledger.stats().spent();
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(MODULE_CALLBACK_CONTROL_WORK + ASSET_LOOKUP_WORK,
                                      RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        if (members.empty())
        {
            return result;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr)
        {
            return result;
        }
        if (!m_ledger.consume(
                MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * std::char_traits<char>::length(asset->global),
                RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        result.kind = kind;
        const auto global = globalProperty(engine, QString::fromLatin1(asset->global));
        result.chargedWork = m_ledger.stats().spent() - before;
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            if (global.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
            }
            return result;
        }
        const auto prepared = prepareModuleCompilation(engine, kind, global.value, members, commandPolicy);
        if (prepared != GuardedNativeBindingStatus::Bound)
        {
            result.status = prepared;
            result.chargedWork = m_ledger.stats().spent() - before;
            return result;
        }
        result.members.reserve(members.size());
        for (const auto name : members)
        {
            if (!m_ledger.consume(MEMBER_NAME_WORK + MEMBER_NAME_CHARACTER_WORK * name.size(),
                                  RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                break;
            }
            const auto member = QString::fromLatin1(name.data(), name.size());
            const auto actual = moduleProperty(engine, kind, global.value, member);
            if (actual.status != GuardedScriptPropertyStatus::Found)
            {
                if (actual.status == GuardedScriptPropertyStatus::BudgetExhausted)
                {
                    result.status = GuardedNativeBindingStatus::BudgetExhausted;
                }
                break;
            }
            GuardedCommandOperand operand{GuardedNativeBindingStatus::Bound, actual.value, false};
            const bool wardenTrap = commandPolicy && kind == GuardedNativeCodeKind::Trap && name == "isTrap";
            if (wardenTrap || (commandPolicy && name == "repairUnit"))
            {
                operand = wardenTrap ? guardedMovementTrapOperand(engine, actual.value, m_ledger)
                    : guardedCommandRepairOperand(engine, m_data->engine, actual.value, m_ledger,
                    m_observedEngine == &engine ? &*m_ownIndex : nullptr,
                    m_observedEngine == &engine && m_callbackGlobals ? &*m_callbackGlobals : nullptr);
                if (operand.status != GuardedNativeBindingStatus::Bound)
                {
                    result.status = operand.status;
                    break;
                }
            }
            auto agreement = authenticateMember(operand.value, kind, member);
            if (operand.wrapped)
            {
                agreement.forwarder = wardenTrap ? GuardedNativeForwarder::WardenTrap : GuardedNativeForwarder::CommandRepair;
            }
            if (agreement.status == GuardedNativeBindingStatus::Unsupported && commandPolicy &&
                (kind == GuardedNativeCodeKind::Infantry || kind == GuardedNativeCodeKind::LightTank) && name == "postAction")
            {
                agreement.status = authenticateMovementPostAction(engine, m_data->engine, actual.value, m_ledger);
                if (agreement.status == GuardedNativeBindingStatus::Bound)
                {
                    agreement.name = member;
                    agreement.evidence = GuardedCommandUnitDefinition::MovementPostAction;
                }
            }
            if (agreement.status == GuardedNativeBindingStatus::Unsupported && commandPolicy &&
                (kind == GuardedNativeCodeKind::Infantry || kind == GuardedNativeCodeKind::LightTank) &&
                (name == "init" || name == "getBaseCost" || name == "getWeatherImmune"))
            {
                auto definition = GuardedCommandUnitDefinition::CommandWeather;
                if (name == "init")
                {
                    definition = kind == GuardedNativeCodeKind::Infantry
                        ? GuardedCommandUnitDefinition::InfantryInit : GuardedCommandUnitDefinition::LightTankInit;
                }
                else if (name == "getBaseCost")
                {
                    definition = kind == GuardedNativeCodeKind::Infantry
                        ? GuardedCommandUnitDefinition::InfantryCost : GuardedCommandUnitDefinition::LightTankCost;
                }
                agreement.status = authenticateCommandUnitDefinition(engine, actual.value, definition, m_ledger,
                    m_observedEngine == &engine ? &*m_ownIndex : nullptr,
                    m_observedEngine == &engine && m_callbackGlobals ? &*m_callbackGlobals : nullptr);
                if (agreement.status == GuardedNativeBindingStatus::Bound)
                {
                    agreement.name = member;
                    agreement.evidence = definition;
                }
            }
            if (agreement.status == GuardedNativeBindingStatus::Unsupported && commandPolicy &&
                (kind == GuardedNativeCodeKind::Forest || kind == GuardedNativeCodeKind::BaseForest) && name == "getDefense")
            {
                const auto definition = GuardedCommandTerrainDefinition::Aw2ForestDefense;
                agreement.status = authenticateCommandTerrainDefinition(engine, actual.value, definition, m_ledger);
                if (agreement.status == GuardedNativeBindingStatus::Bound)
                {
                    agreement.name = member;
                    agreement.evidence = definition;
                }
            }
            if (agreement.status == GuardedNativeBindingStatus::Unsupported && commandPolicy &&
                kind == GuardedNativeCodeKind::Ranking)
            {
                if (const auto bonus = guardedCommandRankingBonus(member))
                {
                    agreement.status = rankingBonus(engine, actual.value, *bonus);
                    if (agreement.status == GuardedNativeBindingStatus::Bound)
                    {
                        agreement.name = member;
                        agreement.evidence = *bonus;
                    }
                }
            }
            if (agreement.status == GuardedNativeBindingStatus::Unsupported && commandPolicy)
            {
                const auto forwarder = guardedCommandCallbackForwarder(kind, member);
                if (forwarder != GuardedNativeForwarder::None)
                {
                    const auto callback = callbackOperand(engine, actual.value, forwarder);
                    if (callback.status == GuardedNativeBindingStatus::Bound)
                    {
                        agreement = authenticateMember(callback.value, kind, member);
                        agreement.forwarder = forwarder;
                    }
                    else
                    {
                        agreement.status = callback.status;
                    }
                }
            }
            const auto status = agreement.status;
            result.members.push_back(std::move(agreement));
            if (status != GuardedNativeBindingStatus::Bound)
            {
                result.status = status;
                break;
            }
        }
        if (result.members.size() == members.size() &&
            result.members.back().status == GuardedNativeBindingStatus::Bound)
        {
            result.status = GuardedNativeBindingStatus::Bound;
        }
        result.chargedWork = m_ledger.stats().spent() - before;
        return result;
    }

    GuardedNativeUnitTypeAgreement GuardedNativeCodeCatalog::authenticateUnitTypes(
        QJSEngine & engine, const QObject & receiver, const QMetaObject & metadata)
    {
        if (!m_data)
        {
            return {m_initializationStatus, {}};
        }
        return authenticateEnumFields(engine, receiver, metadata, UNIT_TYPE_FIELDS, m_ledger,
            m_observedEngine == &engine ? &m_globals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeEnumAgreement GuardedNativeCodeCatalog::authenticateTerminalEnums(
        QJSEngine & engine, const QObject & receiver, const QMetaObject & metadata)
    {
        if (!m_data)
        {
            return {m_initializationStatus, {}};
        }
        return authenticateEnumFields(engine, receiver, metadata, TERMINAL_ENUM_FIELDS, m_ledger,
            m_observedEngine == &engine ? &m_globals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeEnumAgreement GuardedNativeCodeCatalog::authenticateStockContextEnums(
        QJSEngine & engine, const QObject & receiver, const QMetaObject & metadata)
    {
        if (m_initializationStatus != GuardedNativeBindingStatus::Bound)
        {
            return {m_initializationStatus, {}};
        }
        return authenticateEnumFields(engine, receiver, metadata, STOCK_CONTEXT_ENUM_FIELDS, m_ledger,
            m_observedEngine == &engine ? &m_globals : nullptr,
            m_observedEngine == &engine ? &*m_ownIndex : nullptr);
    }

    GuardedNativeCodeAgreement GuardedNativeCodeCatalog::match(
        const GuardedScriptValueView & function, GuardedNativeCodeKind kind, const QString & member)
    {
        GuardedNativeCodeAgreement result;
        if (!m_data)
        {
            result.status = m_initializationStatus;
            return result;
        }
        if (!m_ledger.consume(ASSET_LOOKUP_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        const auto* asset = assetFor(kind);
        if (asset == nullptr || member.isEmpty() || member.size() > MAX_CODE_MEMBER)
        {
            return result;
        }
        result.actual = guardedScriptFunctionImage(function, m_ledger, m_imageCache.get());
        if (result.actual.status != GuardedScriptFunctionStatus::Verified ||
            result.actual.kind != GuardedScriptFunctionKind::Script)
        {
            result.status = imageStatus(result.actual.status);
            return result;
        }
        if (!m_ledger.consume(cacheWork(m_data->images.size(), result.actual.implementation.size(), member.size()),
                              RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        const Data::Key key{kind, result.actual.implementation, member};
        const auto cached = m_data->images.find(key);
        if (cached != m_data->images.end())
        {
            result.approved = cached->second;
        }
        else
        {
            if (m_data->images.size() >= MAX_CODE_ENTRIES)
            {
                return result;
            }
            const auto parentStatus = m_data->loadParent(*asset, m_ledger);
            if (parentStatus != GuardedNativeBindingStatus::Bound)
            {
                result.status = parentStatus;
                return result;
            }
            const auto status = m_data->load(*asset, result.actual.compilationName, m_ledger);
            if (status != GuardedNativeBindingStatus::Bound)
            {
                result.status = status;
                return result;
            }
            const auto approved = m_data->memberProperty(*asset, member, m_ledger);
            if (approved.status != GuardedScriptPropertyStatus::Found || approved.prototypeDepth != 0)
            {
                if (approved.status == GuardedScriptPropertyStatus::BudgetExhausted)
                {
                    result.status = GuardedNativeBindingStatus::BudgetExhausted;
                }
                return result;
            }
            result.approved =
                guardedScriptFunctionImage(approved.value, m_ledger, m_data->imageCache.get());
            if (result.approved.status != GuardedScriptFunctionStatus::Verified)
            {
                result.status = imageStatus(result.approved.status);
                return result;
            }
            if (!m_ledger.consume(
                    cacheWork(m_data->images.size(), result.actual.implementation.size(), member.size()),
                    RefinementWork::MasterPreparation))
            {
                result.status = GuardedNativeBindingStatus::BudgetExhausted;
                return result;
            }
            m_data->images.emplace(key, result.approved);
        }
        if (!m_ledger.consume(functionComparisonWork(result.actual), RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeBindingStatus::BudgetExhausted;
            return result;
        }
        if (result.actual.sameImplementation(result.approved))
        {
            result.status = GuardedNativeBindingStatus::Bound;
            result.ownerKind = kind;
        }
        return result;
    }
}
