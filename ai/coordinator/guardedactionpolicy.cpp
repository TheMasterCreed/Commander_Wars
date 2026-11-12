#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardedmethodrequirements.h"
#include "ai/coordinator/guardednativecode.h"
#include "coreengine/jsthis.h"
#include "coreengine/scriptvariable.h"
#include "coreengine/scriptvariables.h"
#include "game/building.h"
#include "game/gameaction.h"
#include "game/gameanimation/gameanimationwalk.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/victoryrule.h"

#include <QJSEngine>
#include <QThread>
#include <algorithm>
#include <type_traits>

namespace Coordinator
{
    namespace
    {
        constexpr auto BOUND = GuardedNativeReceiverStatus::Bound;
        constexpr QLatin1StringView EMBARGO_KEY{"DB_CURSE_EMBARGO_UNTIL"};
        constexpr auto & MAP_METHODS = NativeMethodRequirements::Actions::MAP_METHODS;
        constexpr auto & RULES_METHODS = NativeMethodRequirements::Actions::RULES_METHODS;
        constexpr auto & PLAYER_METHODS = NativeMethodRequirements::Actions::PLAYER_METHODS;
        constexpr auto & UNIT_METHODS = NativeMethodRequirements::Actions::UNIT_METHODS;
        constexpr auto & BUILDING_METHODS = NativeMethodRequirements::Actions::BUILDING_METHODS;
        constexpr auto & BUILDING_SPRITE_METHODS = NativeMethodRequirements::Actions::BUILDING_SPRITE_METHODS;
        constexpr std::array ACTION_METHODS{
            GuardedNativeMethodRequirement{"getTargetUnit", 0}, GuardedNativeMethodRequirement{"getTargetBuilding", 0},
            GuardedNativeMethodRequirement{"getMovementBuilding", 0}, GuardedNativeMethodRequirement{"getActionTarget", 0}};
        constexpr std::array VARIABLE_LOOKUP{GuardedNativeMethodRequirement{"getVariable", 1}};
        constexpr std::array VARIABLE_READ{GuardedNativeMethodRequirement{"readDataInt32", 0}};

        bool variables(QJSEngine & engine, ScriptVariables* values, const GuardedNativeMethodContract & contract,
                       RefinementLedger & ledger, qsizetype maximum = GUARDED_NATIVE_MAX_RULE_VARIABLES)
        {
            return values && values->getStoredVariables().size() <= maximum &&
                contract.authenticateReturned(engine, *values, ledger) == BOUND &&
                ledger.consume(256 * (values->getStoredVariables().size() + 1), RefinementWork::MasterPreparation) &&
                std::ranges::all_of(values->getStoredVariables(), [](const auto & value) { return bool(value); });
        }

        bool noStoredKey(const ScriptVariables & variables, QLatin1StringView name)
        {
            return std::ranges::none_of(variables.getStoredVariables(), [&](const auto & value) {
                return !value || value->getStoredId() == name;
            });
        }

        bool optionalInteger(QJSEngine & engine, ScriptVariables & variables, const QString & name,
                             const GuardedNativeMethodContract & read, RefinementLedger & ledger)
        {
            const auto* value = variables.getVariable(name);
            return !value || (value->hasUsableStoredDataStream() && value->getStoredData().size() == 4 &&
                             read.authenticateReturned(engine, *value, ledger) == BOUND);
        }

        bool callbackPlayers(GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
                             const GuardedNativeMethodContract & contract, const GuardedNativeMethodContract & lookup,
                             RefinementLedger & ledger)
        {
            if (map.getPlayerCount() != 2 || !inventory.playerMaterial) { return false; }
            for (int i = 0; i < 2; ++i)
            {
                auto* player = map.getPlayer(i);
                const auto & stored = inventory.playerMaterial->at(i).cos;
                if (!player || player->getMap() != &map || stored[0] || stored[1] || player->getCO(0) || player->getCO(1) ||
                    contract.authenticate(engine, JsThis::getJsThis(player), *player, ledger) != BOUND ||
                    contract.authenticateReturned(engine, *player, ledger) != BOUND ||
                    !variables(engine, player->getVariables(), lookup, ledger) ||
                    !noStoredKey(*player->getVariables(), EMBARGO_KEY))
                {
                    return false;
                }
            }
            return true;
        }

        bool callbackUnits(GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
                           const GuardedNativeMethodContract & contract, const GuardedNativeMethodContract & lookup,
                           RefinementLedger & ledger)
        {
            for (const auto & actor : inventory.actors)
            {
                if (actor.tile < 0 || actor.tile >= inventory.width * inventory.height) { return false; }
                auto* tile = map.getTerrain(actor.tile % inventory.width, actor.tile / inventory.width);
                auto* unit = tile ? tile->getUnit() : nullptr;
                if (!unit || actor.id <= 0 || unit->getUniqueID() != actor.id || unit->getMap() != &map ||
                    actor.owner < 0 || actor.owner >= 2 || unit->getOwner() != map.getPlayer(actor.owner) ||
                    contract.authenticate(engine, JsThis::getJsThis(unit), *unit, ledger) != BOUND ||
                    contract.authenticateReturned(engine, *unit, ledger) != BOUND ||
                    !variables(engine, unit->getVariables(), lookup, ledger) ||
                    !noStoredKey(*unit->getVariables(), GUARDED_COMMAND_UNIT_OWNER) ||
                    !noStoredKey(*unit->getVariables(), GUARDED_COMMAND_UNIT_LINEAGE))
                {
                    return false;
                }
            }
            return true;
        }

        bool callbackBuildings(GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
                               const GuardedNativeMethodContract & contract, const GuardedNativeMethodContract & lookup,
                               const GuardedNativeMethodContract & read, RefinementLedger & ledger)
        {
            for (const auto & property : inventory.properties)
            {
                if (property.tile < 0 || property.tile >= inventory.width * inventory.height) { return false; }
                auto* tile = map.getTerrain(property.tile % inventory.width, property.tile / inventory.width);
                auto* building = tile ? tile->getBuilding() : nullptr;
                if (!building || building->getMap() != &map ||
                    contract.authenticate(engine, JsThis::getJsThis(building), *building, ledger) != BOUND ||
                    contract.authenticateReturned(engine, *building, ledger) != BOUND ||
                    !variables(engine, building->getVariables(), lookup, ledger) ||
                    !optionalInteger(engine, *building->getVariables(), QString(GUARDED_COMMAND_BUILDING_OWNER), read, ledger) ||
                    !optionalInteger(engine, *building->getVariables(), QString(GUARDED_COMMAND_BUILDING_LINEAGE), read, ledger))
                {
                    return false;
                }
            }
            return true;
        }

        bool callbackRule(GameMap & map, QJSEngine & engine, const GuardedNativeCalendar & calendar,
                          const GuardedNativeMethodContract & lookup, const GuardedNativeMethodContract & read,
                          RefinementLedger & ledger, GuardedNativeMethodCache & contracts, bool buildingCallbacks, bool rankingCallbacks)
        {
            auto* rules = map.getGameRules();
            if (!rules || rules->getMap() != &map || rules->getVictoryRuleSize() != 3 ||
                calendar.basis.victoryRules.size() != 3 || !calendar.commandRuleIndex ||
                *calendar.commandRuleIndex >= calendar.basis.victoryRules.size())
            {
                return false;
            }
            for (const auto name : {"CMODE_DECKBUILDER", "CMODE_PROCEDURAL_FRONTIER", "CMODE_CHESS_LITE", "CMODE_CHESS_STD", "CMODE_CHESS_TRUE"})
            {
                if (rules->hasVictoryRule(QString::fromLatin1(name))) { return false; }
            }
            const auto nativeRules = contracts.prepare(GameRules::staticMetaObject, RULES_METHODS, ledger);
            constexpr std::array ruleMethods{GuardedNativeMethodRequirement{"getVariables", 0}};
            const auto nativeRule = contracts.prepare(VictoryRule::staticMetaObject, ruleMethods, ledger);
            auto* rule = rules->getVictoryRule(QString(GUARDED_NATIVE_COMMAND_RULE));
            if (nativeRules.status() != BOUND || nativeRules.authenticateReturned(engine, *rules, ledger) != BOUND ||
                !rule || rule->getMap() != &map || nativeRule.status() != BOUND ||
                nativeRule.authenticateReturned(engine, *rule, ledger) != BOUND ||
                !variables(engine, rule->getVariables(), lookup, ledger, GUARDED_NATIVE_COMMAND_VARIABLES) ||
                rule->getVariables()->getStoredVariables().size() != GUARDED_NATIVE_COMMAND_VARIABLES)
            {
                return false;
            }
            const auto & recorded = calendar.basis.victoryRules[*calendar.commandRuleIndex];
            if (recorded.id != GUARDED_NATIVE_COMMAND_RULE || rule != rules->getVictoryRuleAtIndex(*calendar.commandRuleIndex))
            {
                return false;
            }
            const std::array names{QStringLiteral("SpinboxValue1"), QString(GUARDED_COMMAND_SPRITE_SETTING),
                                   QString(GUARDED_COMMAND_BUILDING_BADGE_SETTING), QString(GUARDED_COMMAND_UNIT_BADGE_SETTING)};
            for (const auto & name : names)
            {
                if ((!buildingCallbacks && name == GUARDED_COMMAND_BUILDING_BADGE_SETTING) ||
                    (!rankingCallbacks && name == GUARDED_COMMAND_UNIT_BADGE_SETTING)) { continue; }
                auto* actual = rule->getVariables()->getVariable(name);
                const auto prior = std::ranges::find(recorded.variables, name, &GuardedNativeVariable::id);
                if (!actual || prior == recorded.variables.end() || !actual->hasUsableStoredDataStream() ||
                    actual->getStoredData().size() != 4 || actual->getStoredData() != prior->data ||
                    read.authenticateReturned(engine, *actual, ledger) != BOUND ||
                    (name == QStringLiteral("SpinboxValue1") && actual->getStoredData() != QByteArrayView("\0\0\0\2", 4)))
                {
                    return false;
                }
            }
            return true;
        }

        bool callbackReceivers(GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
                               const GuardedNativeCalendar & calendar, RefinementLedger & ledger, GuardedNativeMethodCache & contracts, bool buildingCallbacks, bool rankingCallbacks)
        {
            static_assert(std::is_final_v<GameAnimationWalk>);
            if (inventory.width <= 0 || inventory.height <= 0 || inventory.width > GUARDED_STOCK_MAX_TILES ||
                inventory.height > GUARDED_STOCK_MAX_TILES || inventory.width != map.getMapWidth() || inventory.height != map.getMapHeight() ||
                std::int64_t(inventory.width) * inventory.height > GUARDED_STOCK_MAX_TILES ||
                inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS ||
                inventory.properties.size() > GUARDED_STOCK_MAX_TILES ||
                map.thread() != QThread::currentThread() || engine.thread() != map.thread())
            {
                return false;
            }
            if (!ledger.consume(131072 + 1024 * (inventory.actors.size() + inventory.properties.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                return false;
            }
            const auto nativeMap = contracts.prepare(GameMap::staticMetaObject, MAP_METHODS, ledger);
            const auto players = contracts.prepare(Player::staticMetaObject, PLAYER_METHODS, ledger);
            const auto units = contracts.prepare(Unit::staticMetaObject, UNIT_METHODS, ledger);
            const auto buildingMethods = buildingCallbacks
                ? std::span<const GuardedNativeMethodRequirement>(BUILDING_SPRITE_METHODS)
                : std::span<const GuardedNativeMethodRequirement>(BUILDING_METHODS);
            const auto buildings = contracts.prepare(Building::staticMetaObject, buildingMethods, ledger);
            const auto actions = contracts.prepare(GameAction::staticMetaObject, ACTION_METHODS, ledger);
            const auto lookup = contracts.prepare(ScriptVariables::staticMetaObject, VARIABLE_LOOKUP, ledger);
            const auto read = contracts.prepare(ScriptVariable::staticMetaObject, VARIABLE_READ, ledger);
            if (nativeMap.status() != BOUND || players.status() != BOUND || units.status() != BOUND ||
                buildings.status() != BOUND || actions.status() != BOUND || lookup.status() != BOUND || read.status() != BOUND ||
                nativeMap.authenticate(engine, JsThis::getJsThis(&map), map, ledger) != BOUND ||
                nativeMap.authenticateReturned(engine, map, ledger) != BOUND ||
                guardedNativeNameAbsent(GameAnimationWalk::staticMetaObject, "getGameRules", ledger) != BOUND ||
                GameAnimationWalk::staticMetaObject.classInfoCount() != 0)
            {
                return false;
            }
            return callbackRule(map, engine, calendar, lookup, read, ledger, contracts, buildingCallbacks, rankingCallbacks) &&
                callbackPlayers(map, engine, inventory, players, lookup, ledger) &&
                callbackUnits(map, engine, inventory, units, lookup, ledger) &&
                callbackBuildings(map, engine, inventory, buildings, lookup, read, ledger);
        }
    }

    GuardedNativeBindingStatus authenticateCommandCallbacks(
        GameMap & map, QJSEngine & engine, GuardedNativeCodeCatalog & catalog, const GuardedNativeInventory & inventory,
        const GuardedNativeCalendar & calendar, RefinementLedger & ledger, GuardedNativeMethodCache & contracts, bool buildingCallbacks, bool rankingCallbacks)
    {
        const auto refusals = ledger.stats().refusals;
        if (!callbackReceivers(map, engine, inventory, calendar, ledger, contracts, buildingCallbacks, rankingCallbacks))
        {
            return ledger.stats().refusals != refusals ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }
        const auto environment = catalog.authenticateCommandCallbackEnvironment(engine, buildingCallbacks);
        if (environment != GuardedNativeBindingStatus::Bound || !rankingCallbacks) { return environment; }
        return catalog.authenticateCommandRankingEnvironment(engine, inventory);
    }
}
