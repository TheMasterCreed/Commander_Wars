#include "ai/coordinator/guardednativeinventory.h"
#include "ai/coordinator/guardednativecalendar.h"

#include <algorithm>

#include <QThread>

#include "game/building.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/gamerule.h"
#include "game/gamescript.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace Coordinator
{
    static_assert(GUARDED_NATIVE_UNRANKED == GameEnums::UnitRank_None);

    namespace
    {
        std::optional<std::int32_t> ownerIndex(Player* owner, const std::array<Player*, 2> & players)
        {
            if (owner == nullptr)
            {
                return NO_OWNER;
            }
            for (std::int32_t index = 0; index < 2; ++index)
            {
                if (players[index] == owner)
                {
                    return index;
                }
            }
            return std::nullopt;
        }

        bool captureActor(GameMap & map, Unit & unit, Terrain & terrain, std::int32_t tile,
                          const std::array<Player*, 2> & players, GuardedNativeInventory & inventory)
        {
            const auto owner = ownerIndex(unit.getOwner(), players);
            const auto type = unit.getUnitID();
            const auto movementType = unit.getMovementType();
            const auto weapon1 = unit.getWeapon1ID();
            const auto weapon2 = unit.getWeapon2ID();
            if (!owner || *owner == NO_OWNER || unit.getMap() != &map || unit.getTerrain() != &terrain ||
                unit.getLoadedUnitCount() != 0 || type.isEmpty() || type.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                movementType.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                weapon1.size() > GUARDED_NATIVE_MAX_ID_LENGTH || weapon2.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                inventory.actors.size() >= GUARDED_STOCK_MAX_ACTORS)
            {
                return false;
            }
            inventory.actors.push_back({
                unit.getUniqueID(), *owner, tile, type, unit.getHp(), unit.getCapturePoints(),
                unit.getFuel(), unit.getAmmo1(), unit.getAmmo2(), unit.getHasMoved(),
                unit.getHidden(), unit.getCloaked()});
            inventory.actors.back().material = GuardedNativeActorMaterial{
                unit.getMaxFuel(), unit.getMaxAmmo1(), unit.getMaxAmmo2(), unit.getBaseMovementPoints(),
                unit.getBaseMinRange(), unit.getBaseMaxRange(), unit.getUnitRank(), movementType, weapon1, weapon2,
                static_cast<std::uint8_t>(unit.getModdingFlags()), unit.getIgnoreUnitCollision(),
                unit.hasTemporaryBonuses(), unit.hasMultiTurnPath(), unit.hasVirtualHp()};
            return true;
        }

        bool captureCo(GameMap & map, Player & player, CO & co, GuardedNativeCO & record)
        {
            const auto id = co.getCoID();
            const auto & perks = co.getEffectivePerkList();
            if (co.getMap() != &map || co.getOwner() != &player ||
                id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                static_cast<std::size_t>(perks.size()) > GUARDED_NATIVE_MAX_EFFECTIVE_PERKS)
            {
                return false;
            }
            record.id = id;
            record.powerMode = static_cast<std::int32_t>(co.getPowerMode());
            if (auto* unit = co.getCOUnit())
            {
                if (unit->getMap() != &map || unit->getOwner() != &player ||
                    unit->getTerrain() == nullptr || unit->getTerrain()->getUnit() != unit)
                {
                    return false;
                }
                const auto x = unit->getX();
                const auto y = unit->getY();
                if (x < 0 || x >= map.getMapWidth() || y < 0 || y >= map.getMapHeight() ||
                    map.getTerrain(x, y) != unit->getTerrain())
                {
                    return false;
                }
                record.unitId = unit->getUniqueID();
            }
            record.rangeEnabled = co.getCoRangeEnabled();
            record.globalZone = co.getGlobalCoZone();
            record.powerStars = co.getPowerStars();
            record.superpowerStars = co.getSuperpowerStars();
            record.powerUsed = co.getPowerUsed();
            record.powerFilled = co.getPowerFilled();
            record.isCo0 = co.getIsCO0();
            record.effectivePerks.reserve(perks.size());
            for (const auto & perk : perks)
            {
                if (perk.isEmpty() || perk.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
                record.effectivePerks.push_back(perk);
            }
            return true;
        }

        bool capturePlayerMaterials(
            GameMap & map, const std::array<Player*, 2> & players, GuardedNativeInventory & inventory)
        {
            std::array<GuardedNativePlayerMaterial, 2> records;
            for (std::size_t side = 0; side < players.size(); ++side)
            {
                auto & record = records[side];
                if (players[side]->getMap() != &map)
                {
                    return false;
                }
                record.fundsModifier = players[side]->getFundsModifier();
                record.army = players[side]->getPlayerArmy();
                const auto buildList = players[side]->getBuildList();
                if (record.army.isEmpty())
                {
                    const auto id = players[side]->getCachedPlayerID();
                    if (id >= 0 && id != static_cast<std::int32_t>(side))
                    {
                        return false;
                    }
                    // An uncached ID resolves by looking up this exact player in the map.
                    record.defaultArmyPlayerId = static_cast<std::int32_t>(side);
                }
                if (record.army.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    static_cast<std::size_t>(buildList.size()) > GUARDED_NATIVE_MAX_BUILD_LIST)
                {
                    return false;
                }
                record.buildList.reserve(buildList.size());
                for (const auto & id : buildList)
                {
                    if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                    {
                        return false;
                    }
                    record.buildList.push_back(id);
                }
                for (std::size_t slot = 0; slot < record.cos.size(); ++slot)
                {
                    if (auto* co = players[side]->getCO(static_cast<quint8>(slot)))
                    {
                        record.cos[slot].emplace();
                        if (!captureCo(map, *players[side], *co, *record.cos[slot]))
                        {
                            return false;
                        }
                    }
                }
            }
            inventory.playerMaterial = std::move(records);
            return true;
        }

        bool captureCoRules(GameMap & map, GuardedNativeInventory & inventory)
        {
            auto* rules = map.getGameRules();
            if (rules == nullptr || rules->getMap() != &map || rules->thread() != QThread::currentThread())
            {
                return false;
            }
            inventory.coRules = GuardedNativeCoRules{
                rules->getParallelCos(), rules->getEnableDayToDayCoAbilities(),
                rules->getCoGlobalD2D(), rules->getCoUnits(), rules->getNoPower(),
                rules->getPowerGainSpeed(), rules->getPowerUsageReduction(),
                static_cast<GuardedNativePowerGainMode>(rules->getPowerGainMode()),
                static_cast<GuardedNativePowerGainZone>(rules->getPowerGainZone())};
            return true;
        }

        bool captureCombatRules(GameMap & map, GuardedNativeInventory & inventory)
        {
            auto* rules = map.getGameRules();
            if (rules == nullptr || rules->getMap() != &map || rules->thread() != QThread::currentThread())
            {
                return false;
            }
            inventory.combatRules = GuardedNativeCombatRules{
                static_cast<GuardedNativeDamageFormula>(rules->getDamageFormula()),
                rules->getTerrainDefense(), rules->getHpDefenseReduction()};
            return true;
        }

        bool captureGameRuleIds(GameMap & map, GuardedNativeInventory & inventory)
        {
            auto* rules = map.getGameRules();
            if (rules == nullptr || rules->getMap() != &map || rules->thread() != QThread::currentThread())
            {
                return false;
            }
            const auto count = rules->getGameRuleSize();
            if (count < 0 || static_cast<std::size_t>(count) > GUARDED_NATIVE_MAX_GAME_RULES)
            {
                return false;
            }
            inventory.gameRuleIds.emplace();
            inventory.gameRuleIds->reserve(count);
            for (std::int32_t index = 0; index < count; ++index)
            {
                auto* rule = rules->getGameRuleAtIndex(index);
                if (rule == nullptr || rule->thread() != QThread::currentThread())
                {
                    return false;
                }
                const auto id = rule->getRuleID();
                if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
                inventory.gameRuleIds->push_back(id);
            }
            return true;
        }

        bool captureProperty(GameMap & map, Building & building, Terrain & terrain, std::int32_t tile,
                             const std::array<Player*, 2> & players, GuardedNativeInventory & inventory)
        {
            const auto owner = ownerIndex(building.getOwner(), players);
            const auto type = building.getBuildingID();
            if (!owner || building.getMap() != &map || building.getTerrain() != &terrain ||
                type.isEmpty() || type.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
            {
                return false;
            }
            inventory.properties.push_back({tile, *owner, type});
            return true;
        }

        bool captureTerrain(GameMap & map, Terrain & terrain, std::int32_t tile,
                            GuardedNativeInventory & inventory)
        {
            GuardedNativeTerrain record;
            record.tile = tile;
            record.layers.reserve(GUARDED_NATIVE_MAX_TERRAIN_DEPTH);
            std::array<const Terrain*, GUARDED_NATIVE_MAX_TERRAIN_DEPTH> visited{};
            auto* layer = &terrain;
            while (layer != nullptr)
            {
                const auto depth = record.layers.size();
                if (depth >= visited.size())
                {
                    return false;
                }
                for (std::size_t previous = 0; previous < depth; ++previous)
                {
                    if (visited[previous] == layer)
                    {
                        return false;
                    }
                }
                visited[depth] = layer;
                if (layer->thread() != QThread::currentThread() || layer->getMap() != &map ||
                    layer->getX() != tile % inventory.width || layer->getY() != tile / inventory.width ||
                    (depth > 0 && (layer->getUnit() != nullptr || layer->getBuilding() != nullptr)))
                {
                    return false;
                }
                const auto id = layer->getTerrainID();
                if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
                record.layers.push_back({
                    id, layer->getHp(), layer->getHasStartOfTurn(), layer->getHasFlowDirection()});
                layer = layer->getNextBaseTerrain();
            }
            inventory.terrains->push_back(std::move(record));
            return true;
        }

        bool captureRule(GameMap & map, VictoryRule & rule, std::size_t ruleCount, GuardedNativeVictoryRule & record)
        {
            if (rule.getMap() != &map || rule.thread() != QThread::currentThread())
            {
                return false;
            }
            record.id = rule.getRuleID();
            auto* variables = rule.getVariables();
            if (record.id.isEmpty() || record.id.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                variables == nullptr || variables->thread() != QThread::currentThread())
            {
                return false;
            }
            const auto & entries = variables->getStoredVariables();
            if (static_cast<std::size_t>(entries.size()) > guardedNativeRuleVariableLimit(record.id, ruleCount))
            {
                return false;
            }
            record.variables.reserve(entries.size());
            for (const auto & variable : entries)
            {
                if (!variable || variable->thread() != QThread::currentThread())
                {
                    return false;
                }
                const auto & id = variable->getStoredId();
                const auto & data = variable->getStoredData();
                if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    data.size() > GUARDED_NATIVE_MAX_RULE_BYTES)
                {
                    return false;
                }
                record.variables.push_back({id, data, variable->getModified()});
            }
            return true;
        }

        bool captureCalendarWeather(GameRules & rules, GuardedNativeCalendarBasis & basis)
        {
            const auto weatherCount = rules.getWeatherCount();
            const auto & forecast = rules.getWeatherForecast();
            if (weatherCount <= 0 || static_cast<std::size_t>(weatherCount) > GUARDED_NATIVE_MAX_WEATHERS ||
                forecast.isEmpty() || static_cast<std::size_t>(forecast.size()) > GUARDED_NATIVE_MAX_FORECAST_DAYS)
            {
                return false;
            }
            basis.weatherIds.reserve(weatherCount);
            for (std::int32_t index = 0; index < weatherCount; ++index)
            {
                auto* weather = rules.getWeather(index);
                if (weather == nullptr || weather->thread() != QThread::currentThread())
                {
                    return false;
                }
                const auto id = weather->getWeatherId();
                if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
                basis.weatherIds.push_back(id);
            }
            basis.forecast.reserve(forecast.size());
            for (const auto & day : forecast)
            {
                if (day.size() != 2)
                {
                    return false;
                }
                basis.forecast.push_back({day[0], day[1]});
            }
            return true;
        }
    }

    GuardedNativeInventoryResult captureGuardedNativeInventory(GameMap & map, RefinementLedger & ledger)
    {
        if (!ledger.consume(GUARDED_NATIVE_HEADER_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        if (map.thread() != QThread::currentThread())
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        const auto width = map.getMapWidth();
        const auto height = map.getMapHeight();
        const auto work = guardedNativeInventoryWork(width, height);
        if (!work || map.getPlayerCount() != 2)
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        if (!ledger.consume(*work - GUARDED_NATIVE_HEADER_WORK, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        const std::array<Player*, 2> players{map.getPlayer(0), map.getPlayer(1)};
        if (players[0] == nullptr || players[1] == nullptr || players[0] == players[1])
        {
            return {};
        }
        const auto player = ownerIndex(map.getCurrentPlayer(), players);
        if (!player || *player == NO_OWNER)
        {
            return {};
        }
        GuardedNativeInventory inventory;
        inventory.width = width;
        inventory.height = height;
        inventory.day = map.getCurrentDay();
        inventory.player = *player;
        auto* mapScript = map.getGameScript();
        if (mapScript == nullptr || mapScript->getMap() != &map)
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        inventory.mapScriptLoaded = mapScript->hasLoadedScript();
        for (std::size_t side = 0; side < players.size(); ++side)
        {
            inventory.funds[side] = players[side]->getFunds();
            inventory.teams[side] = players[side]->getTeam();
            inventory.defeated[side] = players[side]->getIsDefeated();
        }
        if (!captureCoRules(map, inventory) || !captureCombatRules(map, inventory) ||
            !capturePlayerMaterials(map, players, inventory) ||
            !captureGameRuleIds(map, inventory))
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        inventory.actors.reserve(GUARDED_STOCK_MAX_ACTORS);
        inventory.properties.reserve(width * height);
        inventory.terrains.emplace();
        inventory.terrains->reserve(width * height);
        for (std::int32_t y = 0; y < height; ++y)
        {
            for (std::int32_t x = 0; x < width; ++x)
            {
                auto* terrain = map.getTerrain(x, y);
                if (terrain == nullptr || terrain->getMap() != &map)
                {
                    return {};
                }
                const auto tile = y * width + x;
                if (!captureTerrain(map, *terrain, tile, inventory))
                {
                    return {GuardedNativeBindingStatus::Unsupported, {}};
                }
                if (auto* unit = terrain->getUnit())
                {
                    if (!captureActor(map, *unit, *terrain, tile, players, inventory))
                    {
                        return {GuardedNativeBindingStatus::Unsupported, {}};
                    }
                }
                if (auto* building = terrain->getBuilding())
                {
                    if (!captureProperty(map, *building, *terrain, tile, players, inventory))
                    {
                        return {GuardedNativeBindingStatus::Unsupported, {}};
                    }
                }
            }
        }
        std::sort(inventory.actors.begin(), inventory.actors.end(),
                  [](const auto & left, const auto & right) { return left.id < right.id; });
        for (std::size_t index = 0; index < inventory.actors.size(); ++index)
        {
            if (inventory.actors[index].id < 0 ||
                (index > 0 && inventory.actors[index - 1].id == inventory.actors[index].id))
            {
                return {};
            }
        }
        return {GuardedNativeBindingStatus::Bound, std::move(inventory)};
    }

    GuardedNativeCalendarBasisResult captureGuardedNativeCalendar(GameMap & map, RefinementLedger & ledger)
    {
        if (!ledger.consume(guardedNativeCalendarCaptureWork(), RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        if (map.thread() != QThread::currentThread() || map.getPlayerCount() != 2)
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        auto* rules = map.getGameRules();
        const std::array<Player*, 2> players{map.getPlayer(0), map.getPlayer(1)};
        if (rules == nullptr || rules->thread() != QThread::currentThread() || rules->getMap() != &map ||
            players[0] == nullptr || players[1] == nullptr || players[0] == players[1] ||
            players[0]->getIsDefeated() || players[1]->getIsDefeated())
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        const auto player = ownerIndex(map.getCurrentPlayer(), players);
        if (!player || *player == NO_OWNER)
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        GuardedNativeCalendarBasis basis;
        basis.day = map.getCurrentDay();
        basis.player = *player;
        basis.terminal = rules->getVictory();
        basis.humanMatch = map.getIsHumanMatch();
        basis.randomWeather = rules->getRandomWeather();
        basis.fog = static_cast<std::int32_t>(rules->getFogMode());
        basis.currentWeather = rules->getCurrentWeatherId();
        basis.startWeather = rules->getStartWeather();
        basis.dayWeather = rules->getDayWeather();
        const auto ruleCount = rules->getVictoryRuleSize();
        if (ruleCount < 0 || static_cast<std::size_t>(ruleCount) > GUARDED_NATIVE_MAX_VICTORY_RULES ||
            !captureCalendarWeather(*rules, basis))
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        basis.victoryRules.reserve(ruleCount);
        for (std::int32_t index = 0; index < ruleCount; ++index)
        {
            auto* rule = rules->getVictoryRuleAtIndex(index);
            GuardedNativeVictoryRule record;
            if (rule == nullptr || !captureRule(map, *rule, static_cast<std::size_t>(ruleCount), record))
            {
                return {GuardedNativeBindingStatus::Unsupported, {}};
            }
            basis.victoryRules.push_back(std::move(record));
        }
        return {GuardedNativeBindingStatus::Bound, std::move(basis)};
    }
}
