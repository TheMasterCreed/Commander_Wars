#pragma once

#include "ai/coordinator/guardednativecalendar.h"
#include "ai/coordinator/guardedscriptfunction.h"

class QJSEngine;

namespace Coordinator
{
    class GuardedNativeCodeCatalog;
    enum class GuardedNativeCodeKind : std::int8_t;
    enum class GuardedNativeForwarder : std::int8_t;

    enum class GuardedCommandUnitDefinition : std::int8_t
    {
        InfantryInit, InfantryCost, CommandWeather, MovementPostAction, LightTankInit, LightTankCost
    };
    enum class GuardedCommandTerrainDefinition : std::int8_t
    {
        Aw2ForestDefense
    };
    GuardedNativeBindingStatus authenticateCommandTerrainDefinition(
        QJSEngine &, const GuardedScriptValueView &, GuardedCommandTerrainDefinition, RefinementLedger &);
    enum class GuardedCommandRankingBonus : std::int8_t
    {
        Offensive, Defensive
    };
    std::optional<GuardedCommandRankingBonus> guardedCommandRankingBonus(const QString &);
    // The caller also binds the active empty-CO, unranked domain.
    GuardedNativeBindingStatus authenticateCommandRankingBonus(
        QJSEngine &, const GuardedScriptValueView &, GuardedCommandRankingBonus, RefinementLedger &,
        const GuardedScriptGlobalIndex* = nullptr, GuardedScriptOwnIndexCache* = nullptr);
    inline constexpr qint32 GUARDED_AWDS_INFANTRY_COST = 1000;
    inline constexpr qint32 GUARDED_AWDS_LIGHT_TANK_COST = 7000;
    inline constexpr QLatin1StringView GUARDED_COMMAND_UNIT_OWNER{"CMODE_CO_COMMAND_UNIT_OWNER"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_UNIT_LINEAGE{"CMODE_CO_COMMAND_UNIT_LINEAGE"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_BUILDING_OWNER{"CMODE_CO_COMMAND_BUILDING_OWNER"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_BUILDING_LINEAGE{"CMODE_CO_COMMAND_BUILDING_LINEAGE"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_SPRITE_SETTING{"SpinboxValue15"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_BUILDING_BADGE_SETTING{"SpinboxValue18"};
    inline constexpr QLatin1StringView GUARDED_COMMAND_UNIT_BADGE_SETTING{"SpinboxValue17"};
    inline constexpr QLatin1StringView GUARDED_WARDEN_BARRAGES{"WARDEN_BARRAGES"};
    GuardedNativeBindingStatus authenticateCommandUnitDefinition(
        QJSEngine &, const GuardedScriptValueView &, GuardedCommandUnitDefinition, RefinementLedger &,
        GuardedScriptOwnIndexCache* = nullptr, const GuardedScriptGlobalIndex* = nullptr);

    // The settings, script, enum and native receivers must all pass for this calendar.
    GuardedNativeBindingStatus authenticateGuardedCommandFunctions(
        QJSEngine &, RefinementLedger &, GuardedScriptOwnIndexCache* = nullptr, const GuardedScriptGlobalIndex* = nullptr);
    GuardedNativeBindingStatus authenticateGuardedCommandPolicy(
        GameMap &, QJSEngine &, GuardedNativeCodeCatalog &, const GuardedNativeInventory &,
        const GuardedNativeCalendar &, RefinementLedger &, GuardedNativeMethodCache* = nullptr);

    struct GuardedCommandOperand
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        GuardedScriptValueView value;
        bool wrapped{false};
    };
    using GuardedCommandRepairOperand = GuardedCommandOperand;
    GuardedNativeForwarder guardedCommandCallbackForwarder(GuardedNativeCodeKind, const QString &);
    std::uint16_t guardedCommandCallbackBit(GuardedNativeForwarder);
    bool guardedCommandHasBuildingCallbacks(std::uint16_t);
    bool guardedCommandHasRankingCallbacks(std::uint16_t);
    GuardedScriptGlobalIndex prepareGuardedCommandCallbackGlobals(QJSEngine &, RefinementLedger &);
    GuardedCommandOperand guardedCommandCallbackOperand(
        QJSEngine &, QJSEngine &, const GuardedScriptValueView &, GuardedNativeForwarder, RefinementLedger &,
        const GuardedScriptGlobalIndex* = nullptr, GuardedScriptOwnIndexCache* = nullptr);
    GuardedNativeBindingStatus authenticateGuardedActionFunctions(
        QJSEngine &, QJSEngine &, RefinementLedger &,
        const GuardedScriptGlobalIndex* = nullptr, GuardedScriptOwnIndexCache* = nullptr, bool buildingCallbacks = false);
    GuardedNativeBindingStatus authenticateGuardedRankingFunctions(
        QJSEngine &, const GuardedNativeInventory &, RefinementLedger &,
        const GuardedScriptGlobalIndex* = nullptr, GuardedScriptOwnIndexCache* = nullptr);
    GuardedNativeBindingStatus authenticateGuardedChessFunctions(QJSEngine &, RefinementLedger &);
    GuardedNativeBindingStatus authenticateCommandCallbacks(
        GameMap &, QJSEngine &, GuardedNativeCodeCatalog &, const GuardedNativeInventory &,
        const GuardedNativeCalendar &, RefinementLedger &, GuardedNativeMethodCache &,
        bool buildingCallbacks = false, bool rankingCallbacks = false);

    // Both the returned operand and a direct input still require stock catalog authentication.
    GuardedCommandOperand guardedCommandRepairOperand(
        QJSEngine & engine, QJSEngine & stockRealm, const GuardedScriptValueView &, RefinementLedger &,
        GuardedScriptOwnIndexCache* = nullptr, const GuardedScriptGlobalIndex* = nullptr);
    GuardedNativeBindingStatus authenticateMovementPostAction(
        QJSEngine &, QJSEngine & stockRealm, const GuardedScriptValueView &, RefinementLedger &);
    GuardedCommandOperand guardedMovementTrapOperand(
        QJSEngine &, const GuardedScriptValueView &, RefinementLedger &);
    struct GuardedCommandUnitHooks
    {
        bool repair, infantryInit, lightTankInit, weather, movement;
    };
    GuardedNativeBindingStatus authenticateCommandUnits(
        GameMap &, QJSEngine &, const GuardedNativeInventory &, GuardedCommandUnitHooks, RefinementLedger &,
        GuardedNativeMethodCache* = nullptr);
}
