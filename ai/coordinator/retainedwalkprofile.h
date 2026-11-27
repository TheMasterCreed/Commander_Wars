#pragma once

#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/guardednativecode.h"
#include "coreengine/interpreter.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/gamescript.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/weather.h"

namespace Coordinator
{
    struct RetainedWalkProfiles
    {
        std::vector<int> through;
        QString reason;
        int checkedCells{},admitted{};
        bool ordinaryCaptureInstallments{};

        bool contains(int actor,int tick,std::int64_t &budget) const
        {
            if(budget<1)return false;
            --budget;
            return actor>=0 && actor<static_cast<int>(through.size()) && tick>=0 && tick<=through[actor];
        }
    };

    // A negative stock movement-table entry cannot be made passable by an
    // additive weather/CO modifier in Unit::getMovementCosts. A positive
    // base pays at least one. The unchanged, typed grid used here additionally
    // needs a lower bound for more expensive terrain such as a mountain.
    inline bool retainedWalkCostLowerBound(int gridCost,int stableBase)
    {
        if(stableBase==0)return false;
        return stableBase<0 || (gridCost>=0 && gridCost<=stableBase);
    }

    inline RetainedWalkProfiles prepareRetainedWalkProfiles(GameMap &map,const BattlefieldKnowledge &knowledge,
        const CombatTerrain &terrain,int horizon,std::int64_t &budget)
    {
        RetainedWalkProfiles result;result.through.assign(terrain.units.size(),-1);
        const auto refuse=[&](const char *reason){result.reason=QString::fromLatin1(reason);return result;};
        if(budget<=0 || horizon<=0 || horizon>64 || map.getPlayerCount()!=2 || !map.getCurrentPlayer())
            return refuse("MISSING_TWO_PLAYER_MOVEMENT_CLOCK");
        auto *rules=map.getGameRules();
        const auto *gameScript=map.getGameScript();
        if(!rules || rules->getFogMode()!=GameEnums::Fog_Off || !rules->getWeatherPrediction() ||
           (gameScript && (gameScript->hasLoadedScript() || !gameScript->getScriptFile().isEmpty())))
            return refuse("UNVERIFIED_MOVEMENT_CALENDAR");
        for(int side=0;side<2;++side)
        {
            auto *p=map.getPlayer(side);
            if(!p || p->getCO(0) || p->getCO(1) || p->getIsDefeated())return refuse("NONORDINARY_MOVEMENT_MODIFIERS");
        }
        RefinementLedger ledger;ledger.open(budget);
        struct Debit{std::int64_t &budget;RefinementLedger &ledger;~Debit(){budget-=ledger.stats().spent();}} debit{budget,ledger};
        const auto pay=[&](std::int64_t amount){return ledger.consume(amount,RefinementWork::MasterPreparation);};
        using Kind=GuardedNativeCodeKind;
        std::map<Kind,std::vector<int>> candidates;
        std::set<QString> names,terrainTypes,unitTypes,weatherTypes,properties;
        for(int i=0;i<static_cast<int>(terrain.units.size());++i)
        {
            if(!pay(12))return refuse("MOVEMENT_PROFILE_WORK_LIMIT");
            const auto &known=knowledge.units()[i];const auto &pawn=terrain.units[i];
            unitTypes.insert(known.unitId);
            if(!pawn.present || !known.canCapture || pawn.grid<0 || pawn.grid>=static_cast<int>(terrain.grids.size()))continue;
            auto *unit=map.getTerrain(known.x,known.y)->getUnit();
            if(!unit || unit->hasTemporaryBonuses() || unit->getBaseMovementPoints()<=0 ||
               pawn.movementBeforeFuel!=unit->getBaseMovementPoints())continue;
            const auto type=unit->getMovementType();
            if(type==QStringLiteral("MOVE_FEET"))candidates[Kind::MoveFeet].push_back(i);
            else if(type==QStringLiteral("MOVE_MECH"))candidates[Kind::MoveMech].push_back(i);
        }
        if(candidates.empty())return refuse("NO_STABLE_STOCK_FOOT_CAPPER");
        const int cells=terrain.width*terrain.height,cutoff=2*horizon-1;
        if(cells<=0 || !pay(12LL*cells+8LL*(cutoff+1)))return refuse("MOVEMENT_PROFILE_WORK_LIMIT");
        std::vector<QString> cellIds;cellIds.reserve(cells);
        std::set<QString> cellTypes;
        for(int y=0;y<terrain.height;++y)for(int x=0;x<terrain.width;++x)
        {
            auto *at=map.getTerrain(x,y);
            if(!at || at->getHasStartOfTurn() || at->getHp()>0 || (at->getBuilding() && at->getBuilding()->getHp()>0))
                return refuse("CHANGEABLE_MOVEMENT_TERRAIN");
            const auto id=at->getID();
            if(at->getBuilding())properties.insert(id);
            // These are the ownership/water switches in the authenticated
            // native foot and mech table dispatch; a frozen table alone is insufficient.
            if(id=="ZGATE_E_W" || id=="ZGATE_N_S" || id=="FORTHQ" || id=="OILRIG")return refuse("CONDITIONAL_MOVEMENT_TERRAIN");
            cellIds.push_back(id);cellTypes.insert(id);terrainTypes.insert(at->getTerrainID());
        }
        auto *currentWeather=rules->getCurrentWeather();
        if(!currentWeather)return refuse("UNKNOWN_CURRENT_WEATHER");
        weatherTypes.insert(currentWeather->getWeatherId());
        const int current=map.getCurrentPlayer()->getPlayerID();
        std::vector<QString> weatherIds(cutoff+1);
        for(int tick=0;tick<=cutoff;++tick)
        {
            const int turn=current+tick;
            auto *weather=tick==0?currentWeather:rules->getWeatherAtDay(turn/2,turn%2);
            if(!weather && !rules->getRandomWeather())weather=rules->getWeather(rules->getStartWeather());
            if(!weather)break;
            weatherIds[tick]=weather->getWeatherId();weatherTypes.insert(weatherIds[tick]);
        }
        names=terrainTypes;names.insert(unitTypes.begin(),unitTypes.end());names.insert(weatherTypes.begin(),weatherTypes.end());
        names.insert(properties.begin(),properties.end());
        std::vector<QString> extra(names.begin(),names.end());
        auto &engine=*Interpreter::getInstance();
        GuardedNativeCodeCatalog catalog(ledger);
        auto observation=catalog.observe(engine);
        if(observation.status()!=GuardedNativeBindingStatus::Bound)return refuse("MOVEMENT_REALM_UNVERIFIED");
        // Catalog observations already contain the canonical movement and
        // property names. Inspect the live roster and weather in their own
        // index: many of those identifiers overlap the built-in catalog.
        const auto liveTypes=GuardedScriptGlobalIndex::prepare(engine,extra,ledger);
        if(liveTypes.status()!=GuardedScriptPropertyStatus::Found)return refuse("MOVEMENT_RUNTIME_TYPES_UNVERIFIED");
        const auto helper=catalog.authenticateStockMovementCallbacks(engine,GuardedNativeCodeKind::MovementTable);
        if(helper.status!=GuardedNativeBindingStatus::Bound)return refuse("MOVEMENT_TABLE_UNVERIFIED");
        if(!properties.empty())
        {
            if(catalog.authenticateStockPropertyCallbacks(engine,GuardedNativeCodeKind::Building,false).status!=GuardedNativeBindingStatus::Bound)
                return refuse("UNVERIFIED_PROPERTY_TURN_HELPERS");
            for(const auto &type:properties)
            {
                const auto which=catalog.kindForGlobal(type);
                if(which.status!=GuardedNativeBindingStatus::Bound ||
                   (which.kind!=GuardedNativeCodeKind::Town && which.kind!=GuardedNativeCodeKind::Factory &&
                    which.kind!=GuardedNativeCodeKind::Headquarters) ||
                   catalog.authenticateStockPropertyCallbacks(engine,which.kind,false).status!=GuardedNativeBindingStatus::Bound)
                    return refuse("UNVERIFIED_PROPERTY_TURN");
            }
        }
        GuardedScriptOwnIndexCache own(engine,ledger);
        const auto quiet=[&](const QString &object,const char *member){
            auto obj=liveTypes.find(engine,object,ledger);
            if(obj.status!=GuardedScriptPropertyStatus::Found)return false;
            const auto h=guardedScriptProperty(obj.value,QString::fromLatin1(member),ledger,&own);
            return h.status==GuardedScriptPropertyStatus::Missing || (h.status==GuardedScriptPropertyStatus::Found &&
                (h.value.isNull() || h.value.kind()==GuardedScriptValueKind::Undefined));
        };
        for(const auto &type:terrainTypes)if(!quiet(type,"getMovementcostModifier"))return refuse("DIRECTIONAL_TERRAIN_HOOK");
        // The retained roster may not acquire movement or change terrain on
        // its own between two activations. Other unit actions and transports
        // remain outside a named, unassisted walk.
        for(const auto &type:unitTypes)if(!quiet(type,"startOfTurn") || !quiet(type,"endOfTurn"))
            return refuse("ROSTER_TURN_MOVEMENT_UNVERIFIED");
        std::map<QString,bool> quietWeather;
        for(const auto &id:weatherTypes)quietWeather[id]=quiet(id,"getMovementCostModifier") && quiet(id,"getMovementpointModifier");
        if(!quietWeather[currentWeather->getWeatherId()])return refuse("CURRENT_MOVEMENT_WEATHER_MODIFIER");
        int through=-1;
        for(int tick=0;tick<=cutoff && !weatherIds[tick].isEmpty() && quietWeather[weatherIds[tick]];++tick)through=tick;
        for(const auto &[kind,actors]:candidates)
        {
            // The live callback and every directional cell must agree with
            // this actor's movement table over the same quiet-weather window.
            if(catalog.authenticateStockMovementCallbacks(engine,kind).status!=GuardedNativeBindingStatus::Bound)continue;
            const auto tables=catalog.authenticateStockMovementTables(engine,kind);
            if(tables.status!=GuardedNativeBindingStatus::Bound || tables.tables.size()!=1)continue;
            std::map<QString,int> lower;
            for(const auto &id:cellTypes)
            {
                const auto found=guardedNativeIntegerTableValue(tables.tables.front().evidence,id,ledger);
                if(found.status!=GuardedNativeBindingStatus::Bound)break;
                lower[id]=found.value.value_or(-1);
                if(lower[id]==0){lower.erase(id);break;}
            }
            if(lower.size()!=cellTypes.size())continue;
            std::map<int,bool> gridChecked;
            for(int i:actors)
            {
                const int which=terrain.units[i].grid;
                auto [key,fresh]=gridChecked.try_emplace(which,true);
                if(fresh)
                {
                    if(!pay(8LL*cells))return refuse("MOVEMENT_PROFILE_WORK_LIMIT");
                    const auto &grid=terrain.grids[which];
                    if(grid.width()!=terrain.width || grid.height()!=terrain.height)key->second=false;
                    for(int sq=0;key->second && sq<cells;++sq)for(auto dir:ENTRY_DIRECTIONS)
                    {
                        const int x=sq%terrain.width,y=sq/terrain.width;
                        const auto delta=entryStep(dir);
                        if(!grid.onGrid(x-delta.dx,y-delta.dy))continue;
                        if(!retainedWalkCostLowerBound(grid.cost(x,y,dir),lower.at(cellIds[sq]))){key->second=false;break;}
                    }
                    result.checkedCells+=cells;
                }
                if(key->second && through>=0){result.through[i]=through;++result.admitted;}
            }
        }
        // Unit::getCaptureRate equals rounded HP on this no-CO map. Authenticate
        // the action that actually applies one installment and checks 20 points.
        if(result.admitted)
        {
            const auto action=catalog.globalProperty(engine,QStringLiteral("ACTION_CAPTURE"));
            const auto same=[&](const QString &name){
                if(action.status!=GuardedScriptPropertyStatus::Found)return false;
                const auto method=guardedScriptProperty(action.value,name,ledger,&own);
                return method.status==GuardedScriptPropertyStatus::Found &&
                    catalog.authenticateFunction(method.value,Kind::Capture,name).status==GuardedNativeBindingStatus::Bound;
            };
            result.ordinaryCaptureInstallments=same(QStringLiteral("perform")) && same(QStringLiteral("performPostAnimation"));
        }
        result.reason=result.admitted?"CERTIFIED_RETAINED_FOOT_PROFILE":"NO_CERTIFIED_DIRECTIONAL_PROFILE";
        return result;
    }
}
