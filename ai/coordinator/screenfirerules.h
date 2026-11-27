#pragma once

#include "ai/coordinator/screenfirebindings.h"
#include "ai/coordinator/openingsurvivalbindings.h"
#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/retainedwalkprofile.h"

namespace Coordinator
{
    inline std::shared_ptr<ScreenFireProfile> prepareScreenFireProfile(GameMap &map,const BattlefieldKnowledge &knowledge,
        const CombatTerrain &terrain,const RetainedWalkProfiles &walk,std::int64_t &budget,bool opening=false)
    {
        const int n=static_cast<int>(terrain.units.size());
        auto out=std::make_shared<ScreenFireProfile>();out->ordinary.assign(n,false);out->original.resize(n);
        const auto fail=[&](const QString &s){out->reason=s;return out;};
        if(walk.admitted<=0 || walk.through.empty() || !terrain.production || !map.getCurrentPlayer() ||
           map.getGameRules()->getRankingSystem())return fail("UNVERIFIED_RULES_OR_PROMOTION");
        const int last=*std::max_element(walk.through.begin(),walk.through.end());
        if(last<0 || last>64)return fail("UNVERIFIED_MOVEMENT_PERIOD");
        RefinementLedger ledger;ledger.open(std::max<std::int64_t>(0,budget));
        struct Debit{std::int64_t &remaining;RefinementLedger &ledger;~Debit(){remaining-=ledger.stats().spent();}} debit{budget,ledger};
        const auto pay=[&](std::int64_t cost){return ledger.consume(cost,RefinementWork::MasterPreparation);};
        const int cells=terrain.width*terrain.height;
        if(!pay(cells*12LL+n*16LL+last*8LL+64))return fail("SCREEN_RULE_WORK_LIMIT");
        auto *rules=map.getGameRules();std::set<QString> terrainTypes,properties,weather,stockTypes,allTypes;
        std::vector<QString> tileIds;
        std::set<QString> openingBuys;
        for(int y=0;y<terrain.height;++y)for(int x=0;x<terrain.width;++x)
        {
            auto *at=map.getTerrain(x,y);tileIds.push_back(at->getID());terrainTypes.insert(at->getTerrainID());
            if(auto *building=at->getBuilding()){
                properties.insert(at->getID());
                if(opening){const auto menu=building->getConstructionList();openingBuys.insert(menu.begin(),menu.end());}
            }
        }
        const int current=map.getCurrentPlayer()->getPlayerID();
        for(int t=0;t<=last;++t)
        {
            auto *w=t?rules->getWeatherAtDay((current+t)/2,(current+t)%2):rules->getCurrentWeather();
            if(!w && !rules->getRandomWeather())w=rules->getWeather(rules->getStartWeather());
            if(!w)return fail("UNKNOWN_SCREEN_WEATHER");
            weather.insert(w->getWeatherId());
        }
        for(const auto &u:knowledge.units())stockTypes.insert(u.unitId);
        allTypes=stockTypes;
        const auto simpleMovement=[](const QString &id){return id=="MOVE_FEET" || id=="MOVE_MECH" || id=="MOVE_TANK" || id=="MOVE_TIRE_A";};
        for(const auto &site:terrain.production->sites)for(const auto &side:site.menu)for(const auto &u:side)
        {
            if(!pay(1))return fail("SCREEN_RULE_WORK_LIMIT");
            if(simpleMovement(u.screeningMovementType))allTypes.insert(u.type);
        }
        // setCurrentWeather invokes its activate/deactivate callbacks only when
        // the native weather index changes.
        const auto *active=rules->getCurrentWeather();
        for(int t=1;t<=last;++t)
        {
            auto *future=rules->getWeatherAtDay((current+t)/2,(current+t)%2);
            if(!future && !rules->getRandomWeather())future=rules->getWeather(rules->getStartWeather());
            if(!active || active!=future)return fail("CHANGING_SCREEN_WEATHER");
        }
        thread_local HqBindingDetail::StockMethods methods(QCoreApplication::applicationDirPath()+"/resources/scripts",
            {"getBaseDamage","getMissileBuildings","perform"});
        auto law=readScreenBoundRules(*Interpreter::getInstance(),ledger,methods,terrainTypes,properties,weather,stockTypes,allTypes,tileIds,opening?&openingBuys:nullptr);
        if(!law.reason.isEmpty())return fail(law.reason);
        out->ordinaryOpeningPurchases=law.ordinaryOpeningPurchases;
        if(opening && out->ordinaryOpeningPurchases){
            GuardedNativeCodeCatalog catalog(ledger);auto *enemy=map.getPlayer(1-knowledge.observerId());QString why;
            if(!enemy || !openingSurvivalGlobalBindings(*Interpreter::getInstance(),map,*enemy,catalog,ledger,why,true))
                return fail(why.isEmpty()?QStringLiteral("OPENING_GLOBAL_COMMANDS_UNVERIFIED"):why);
        }
        const auto &costs=law.costs;const auto &rows=law.rows;const auto &admitted=law.admitted;
        const auto damage=[&](const std::array<QString,2> &ids,ScreenFireType &s){
            for(const auto &target:stockTypes)
            {
                int strongest=0;bool known=true;
                for(const auto &id:ids)if(!id.isEmpty())
                {
                    const auto it=rows.find(id);
                    if(it==rows.end() || !it->second){known=false;break;}
                    if(auto base=it->second->find(target);base!=it->second->end())
                        strongest=std::max(strongest,base->second);
                }
                if(known){s.weaponBase[target]=strongest;s.maxDamage[target]=strongest>0?std::min(100,strongest+10):0;}
            }
        };
        std::map<std::pair<int,QString>,bool> checked;
        for(int i=0;i<n;++i)
        {
            auto &u=knowledge.units()[i];const auto &pawn=terrain.units[i];
            if(!pawn.present || u.carrier!=KnownUnit::NO_CARRIER)continue;
            auto *live=map.getTerrain(u.x,u.y)->getUnit();if(!live)return fail("MISSING_NATIVE_SCREEN_UNIT");
            if((opening && live->getUnitRank()!=GameEnums::UnitRank_None) || live->hasTemporaryBonuses() || live->getLoadedUnitCount() || pawn.needsNativeTownExchange)
                return fail("ACTIVE_UNIT_MODIFIER_OR_CARGO");
            const auto id=live->getMovementType();const auto field=costs.find(id);
            if(field==costs.end())continue;
            auto &s=out->original[i];s.movement=live->getBaseMovementPoints();s.range=std::max(1,live->getBaseMaxRange());
            damage({live->getWeapon1ID(),live->getWeapon2ID()},s);
            const int idx=pawn.grid>=0?pawn.grid:pawn.travelGrid;
            if(idx>=0 && idx<static_cast<int>(terrain.grids.size()))
            {
                auto [it,added]=checked.try_emplace({idx,id},true);
                if(added)
                {
                    if(!pay(8LL*cells))return fail("SCREEN_RULE_WORK_LIMIT");
                    const auto &g=terrain.grids[idx];
                    for(int sq=0;it->second && sq<cells;++sq)for(auto dir:ENTRY_DIRECTIONS)
                    {
                        const int x=sq%terrain.width,y=sq/terrain.width;const auto d=entryStep(dir);
                        if(g.onGrid(x-d.dx,y-d.dy) && !retainedWalkCostLowerBound(g.cost(x,y,dir),field->second.at(tileIds[sq])))
                        {it->second=false;break;}
                    }
                }
                if(it->second)s.grid=idx;
            }
            out->ordinary[i]=true;
        }
        for(const auto &site:terrain.production->sites)for(const auto &side:site.menu)for(const auto &u:side)
        {
            if(!pay(1))return fail("SCREEN_RULE_WORK_LIMIT");
            if(!admitted.contains(u.type) || !costs.contains(u.screeningMovementType))continue;
            auto [it,fresh]=out->types.try_emplace(u.type);
            if(!fresh)continue;
            auto &s=it->second;s.movement=u.screeningMovement;s.range=u.screeningRange;
            damage(u.screeningWeapons,s);
        }
        out->quietBirths=std::move(law.admitted);out->through=last;out->reason="VERIFIED_ORDINARY_SCREEN_FIRE";
        return out;
    }
}
