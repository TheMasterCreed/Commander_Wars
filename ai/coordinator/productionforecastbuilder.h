#pragma once

#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/stockcontactnative.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace Coordinator
{
    inline std::shared_ptr<ProductionForecastBoard> buildProductionForecastBoard(
        GameMap &map, const BattlefieldKnowledge &knowledge, MobilityFieldCache &mobility, CombatTerrain &terrain,
        int horizon,bool *workLimited=nullptr)
    {
        if (workLimited) *workLimited=false;
        if (map.getPlayerCount() != 2 || map.getGameRules()->getFogMode() != GameEnums::Fog_Off)
            return {};
        auto result = std::make_shared<ProductionForecastBoard>();
        result->contactDamage=nativeStockContactOracle(map,knowledge,terrain);
        const int cells=terrain.width*terrain.height;
        std::vector<int> stars(cells);
        for(int i=0;i<cells;++i)
            stars[i]=map.getTerrain(i%terrain.width,i/terrain.width)->getBaseDefense();
        const int limit = map.getGameRules()->getUnitLimit();
        DamageOracle oracle(map);
        struct StockMatch {TilePoint tile;int id,lo,hi;};
        std::vector<StockMatch> stock;stock.reserve(terrain.units.size());
        for(int i=0;i<static_cast<int>(terrain.units.size());++i)
        {
            const auto &u=knowledge.units()[i];
            stock.push_back({{u.x,u.y},terrain.units[i].engineUnitId,u.minRange,u.maxRange});
        }
        result->paidStockSlots=TownPaidStock{{},[&map,observer=knowledge.observerId(),stock=std::move(stock),oracle=DamageOracle(map)](TownPaidStock::Map &cache,const FundedCaptureGun &g,int i,int distance) mutable -> std::optional<ProductionForecastBoard::StockPairPowers>{
            if(!g.unit || i<0 || i>=static_cast<int>(stock.size()) || stock[i].id==NO_UNIT)return {};
            const auto &r=stock[i];const auto &body=*g.unit;
            const auto key=std::tuple{g.side,body.type,i,distance,body.minRange,body.maxRange};
            if(auto at=cache.find(key);at!=cache.end())return at->second;
            auto *tile=map.getTerrain(r.tile.x,r.tile.y);auto *live=tile?tile->getUnit():nullptr;
            if(!live || live->getUniqueID()!=r.id)return {};
            Unit piece(body.type,map.getPlayer(g.side?1-observer:observer),false,&map);
            ProductionForecastBoard::StockPairPowers powers;
            for(int slot=0;slot<2;++slot)
            {
                powers[0][slot]=oracle.bestSlotDamage(*live,piece,distance,r.lo,r.hi,slot==0,slot==1);
                powers[1][slot]=oracle.bestSlotDamage(piece,*live,distance,body.minRange,body.maxRange,slot==0,slot==1);
            }
            cache.emplace(key,powers);return powers;
        }};
        result->paidWeaponSlots=TownPaidPair{{},[&map,observer=knowledge.observerId(),oracle=DamageOracle(map)](TownPaidPair::Map &cache,const FundedCaptureGun &shooter,const FundedCaptureGun &victim,int distance) mutable {
            const auto &a=*shooter.unit;const auto &b=*victim.unit;
            const auto key=std::tuple{shooter.side,a.type,b.type,distance,a.minRange,a.maxRange};
            if(auto match=cache.find(key);match!=cache.end())return match->second;
            Unit attack(a.type,map.getPlayer(shooter.side ? 1-observer:observer),false,&map);
            Unit defend(b.type,map.getPlayer(victim.side ? 1-observer:observer),false,&map);
            std::array<double,2> result{
                oracle.bestSlotDamage(attack,defend,distance,a.minRange,a.maxRange,attack.hasAmmo1(),false),
                oracle.bestSlotDamage(attack,defend,distance,a.minRange,a.maxRange,false,attack.hasAmmo2())};
            cache.emplace(key,result);return result;
        }};
        for (int side = 0; side < 2; ++side)
        {
            Player *p = map.getPlayer(side ? 1 - knowledge.observerId() : knowledge.observerId());
            result->funds[side] = toMilliFunds(p->getFunds());
            result->availableUnits[side] = limit > 0 ? std::max(0, limit - p->getUnitCount()) : std::numeric_limits<int>::max();
        }
        struct Profile {std::shared_ptr<const MobilityCostGrid> grid;int minimum;};
        std::map<std::pair<int,QString>,Profile> recruitGrids;
        for (const auto &b : knowledge.buildings())
        {
            const QPoint at(b.x,b.y);
            auto *ground = map.getTerrain(b.x,b.y);
            auto *live = ground->getBuilding();
            if (!live || !live->getActionList().contains(QStringLiteral("ACTION_BUILD_UNITS")))
                continue;
            ProductionSite site{{b.x,b.y},{}};
            for (int side = 0; side < 2; ++side)
            {
                Player *p = map.getPlayer(side ? 1 - knowledge.observerId() : knowledge.observerId());
                // A private building queries the same location-specific script menu for a
                // prospective owner. Never transfer a live map building to price a capture.
                QStringList menu;
                if (live->getOwner() == p)
                    menu = live->getConstructionList();
                else
                {
                    Building preview(b.buildingId, &map);
                    preview.setTerrain(ground);
                    preview.setOwner(p);
                    menu = preview.getConstructionList();
                }
                for (const auto &id : menu)
                {
                    const int cost = p->getCosts(id,at);
                    // Later paydays can fund offers which the current treasury cannot.
                    // Affordability is checked at the actual projected purchase date.
                    if (cost <= 0) continue;
                    auto &minimum=site.minimumBuildCost[side];
                    if(minimum==0 || toMilliFunds(cost)<minimum) minimum=toMilliFunds(cost);
                    if(!result->availableUnits[side]) continue;
                    if (!terrain.pay(4LL * terrain.width * terrain.height +
                                     4LL * knowledge.units().size() + knowledge.buildings().size()))
                    {
                        if (workLimited) *workLimited=true;
                        return {};
                    }
                    Unit preview(id,p,false,&map);
                    if (!preview.canCapture() && !preview.getActionList().contains(QStringLiteral("ACTION_FIRE")))
                        continue;
                    ProductionChoice option;
                    option.type = id;
                    option.screeningMovementType=preview.getMovementType();
                    option.screeningMovement=preview.getBaseMovementPoints();
                    option.screeningRange=preview.getBaseMaxRange();
                    option.screeningWeapons={preview.getWeapon1ID(),preview.getWeapon2ID()};
                    option.cost = toMilliFunds(cost);
                    option.minRange = preview.getMinRange(at);
                    option.maxRange = preview.getMaxRange(at);
                    option.mobileFire = preview.canMoveAndFire(at);
                    option.ignoresUnitCollision=preview.getIgnoreUnitCollision();
                    option.transportCapacity=preview.getLoadingPlace()>0;
                    const auto actions=preview.getActionList();
                    option.supportsUnits=std::any_of(actions.begin(),actions.end(),[](const QString &action){return action.startsWith(QStringLiteral("ACTION_SUPPORT"));});
                    // Any compatible loaded slot can still supply this many shots.
                    // Reserve the first round for the buyer's already scheduled order.
                    option.guaranteedShots = std::numeric_limits<int>::max();
                    if (!preview.getWeapon1ID().isEmpty() && preview.getMaxAmmo1()>0 && preview.hasAmmo1())
                        option.guaranteedShots=preview.getAmmo1();
                    if (!preview.getWeapon2ID().isEmpty() && preview.getMaxAmmo2()>0 && preview.hasAmmo2())
                        option.guaranteedShots=std::min(option.guaranteedShots,preview.getAmmo2());
                    auto [profile,freshGrid]=recruitGrids.try_emplace(std::pair{side,id});
                    if(freshGrid)
                    {
                        auto grid=std::make_shared<MobilityCostGrid>(mobility.grid(map,*p,id));
                        int minimum=std::numeric_limits<int>::max();
                        for(int y=0;y<grid->height();++y) for(int x=0;x<grid->width();++x)
                            for(auto dir:ENTRY_DIRECTIONS) if(const auto cost=grid->cost(x,y,dir);cost>=0) minimum=std::min(minimum,cost);
                        profile->second={std::move(grid),minimum};
                    }
                    option.movementGrid=profile->second.grid;
                    option.minimumMoveCost=profile->second.minimum;
                    option.movement=preview.getMovementpoints(at);option.initialFuel=preview.getFuel();
                    option.routes=reinforcementRoutes(*option.movementGrid,site.tile,option.movement,horizon,option.initialFuel);
                    const auto local=projectedTerrainRule(map,preview,site.tile);
                    const bool useTerrain=preview.useTerrainDefense();
                    option.routeAt.assign(cells,-1);
                    for (int r=0;r<static_cast<int>(option.routes.size());++r)
                    {
                        const auto t=option.routes[r].destination;
                        const int sq=t.y*terrain.width+t.x;
                        auto def=local;
                        def.stars=useTerrain ? stars[sq] : 0;
                        option.routeAt[sq]=r;
                        option.defense.push_back(def);
                    }
                    option.defense.push_back(local);
                    if (preview.canCapture())
                        for (const auto &property : knowledge.buildings())
                            option.captureRate.emplace(property.y * terrain.width + property.x,
                                                       preview.getCaptureRate({property.x,property.y}));
                    option.captureLoad={preview.getMaxAmmo1()<0?-1:preview.getAmmo1(),preview.getMaxAmmo2()<0?-1:preview.getAmmo2()};
                    for (const auto &u : knowledge.units())
                    {
                        std::array<double,2> counter{-1.,-1.},incoming{-1.,-1.};
                        double shot = 0, back = 0;
                        int counterSlot=-1;
                        if (u.carrier == KnownUnit::NO_CARRIER && u.hpRounded > 0 && p->getPlayerID() != u.ownerId)
                        {
                            auto *target = map.getTerrain(u.x,u.y)->getUnit();
                            if (target)
                            {
                                shot = oracle.baseDamageAgainst(preview,*target,option.minRange,option.minRange,option.maxRange);
                                back = oracle.baseDamageAgainst(*target,preview,u.minRange,u.minRange,u.maxRange);
                                if(!option.captureRate.empty())
                                {
                                    // Use the actual paid unit as defense target. Slot-specific
                                    // quotes preserve ammunition and its own real counter.
                                    for(int slot=0;slot<2;++slot)
                                    {
                                        const bool own=slot==0 ? preview.hasAmmo1() && !preview.getWeapon1ID().isEmpty() : preview.hasAmmo2() && !preview.getWeapon2ID().isEmpty();
                                        const bool other=slot==0 ? target->hasAmmo1() && !target->getWeapon1ID().isEmpty() : target->hasAmmo2() && !target->getWeapon2ID().isEmpty();
                                        if(own && shot>0) counter[slot]=oracle.bestSlotDamage(preview,*target,option.minRange,
                                            option.minRange,option.maxRange,slot==0,slot==1);
                                        if(other && back>0) incoming[slot]=oracle.bestSlotDamage(*target,preview,u.minRange,
                                            u.minRange,u.maxRange,slot==0,slot==1);
                                    }
                                }
                                if (back>0 && u.minRange<=1 && u.maxRange>=1)
                                {
                                    const double primary=oracle.bestSlotDamage(*target,preview,u.minRange,u.minRange,
                                        u.maxRange,target->hasAmmo1(),false);
                                    counterSlot=primary==back ? 0 : 1;
                                }
                            }
                        }
                        option.attack.push_back(shot);
                        option.incoming.push_back(back);
                        option.incomingSlot.push_back(counterSlot);
                        if(!option.captureRate.empty())
                        {option.captureCounter.push_back(counter);option.againstCapper.push_back(incoming);}
                    }
                    site.menu[side].push_back(std::move(option));
                }
            }
            result->sites.push_back(std::move(site));
        }
        return result;
    }
}
