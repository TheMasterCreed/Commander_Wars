#pragma once

#include "ai/coordinator/factoryprogramsearch.h"
#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/replypassevaluation.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/building.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace Coordinator::FactoryPrograms
{
    inline bool hasFactoryProgrammeProposal(const MasterAssignmentProblem &problem,const CombatTerrain &terrain,
        const BattlefieldKnowledge &knowledge,std::span<const PropertyFacts> properties,std::int64_t &work)
    {
        if(!terrain.production || terrain.production->availableUnits[0]<=0)return false;
        std::map<std::tuple<int,int,int>,DistanceField> bounds;
        for(const auto &site:properties){
            if(!site.canProduce || !site.capturable || site.ownerId==knowledge.observerId() ||
               site.objective==ObjectiveKind::Headquarters)continue;
            const TilePoint target{site.x,site.y};
            const auto shop=std::find_if(terrain.production->sites.begin(),terrain.production->sites.end(),
                [&](const auto &s){return s.tile==target;});
            if(shop==terrain.production->sites.end() || shop->menu[0].empty())continue;
            for(const auto &member:problem.actors()){
                if(!problem.canChangeActor(member.canonicalSlot) || member.knowledgeUnitIndex<0)continue;
                const auto &known=knowledge.units()[member.knowledgeUnitIndex];const auto &pawn=terrain.units[member.knowledgeUnitIndex];
                if(!known.canCapture || !pawn.present || pawn.enemy)continue;
                const int grid=pawn.travelGrid>=0?pawn.travelGrid:pawn.grid;
                if(grid<0 || grid>=int(terrain.grids.size()) || pawn.movementBeforeFuel<0)continue;
                const auto key=std::tuple{grid,site.x,site.y};
                for(const auto &c:member.candidates){
                    if(!debit(work,1))return false;
                    if(!c.plannable || c.origin!=pawn.tile || c.source.movementCost<0 || c.source.movementCost>known.fuel)continue;
                    if(c.capture && c.destination==target)return true;
                    if(c.actionKind!=PlanBundleKind::Move || c.destination==c.origin)continue;
                    auto found=bounds.find(key);
                    if(found==bounds.end()){
                        if(!debit(work,12LL*terrain.width*terrain.height))return false;
                        found=bounds.emplace(key,DistanceField::build(terrain.grids[grid],{target},FieldExpansion::ToTargets)).first;
                    }
                    const auto &dist=found->second;const int remaining=dist.distance(c.destination.x,c.destination.y);
                    if(remaining>=0 && remaining<dist.distance(c.origin.x,c.origin.y) &&
                       remaining<=std::min(pawn.movementBeforeFuel,known.fuel-c.source.movementCost))return true;
                }
            }
        }
        return false;
    }
    struct LiveBoard { Board board; World initial; QString reason; };
    inline LiveBoard nativeProgramBoard(GameMap &map,const BattlefieldKnowledge &knowledge,const CombatTerrain &terrain,
        std::span<const PropertyFacts> properties,const PropertyStockField &field,const QJsonObject &incumbent,std::int64_t &work)
    {
        LiveBoard out;auto &b=out.board;auto &w=out.initial;
        const auto fail=[&](QString reason){out.reason=std::move(reason);return std::move(out);};
        if(!terrain.production || properties.empty() || map.getPlayerCount()!=2 || field.horizonTurns()<3)
            return fail("COMPLETE_PRODUCTION_BOARD_MISSING");
        b.width=terrain.width;b.height=terrain.height;b.day=map.getCurrentDay();b.player=knowledge.observerId();
        b.threshold=field.capturePointsToCapture();b.economics.horizon=field.horizonTurns();
        if(b.width!=map.getMapWidth() || b.height!=map.getMapHeight() || b.width<=0 || b.height<=0 ||
           b.threshold<=0 || !debit(work,8LL*b.width*b.height+12LL*properties.size()))return fail("INVALID_OR_UNFUNDED_PROGRAM_GEOMETRY");
        auto *rules=map.getGameRules();
        if(!rules || rules->getRandomWeather() || !rules->getCurrentWeather())return fail("UNKNOWN_DATED_WEATHER");
        for(int t=1;t<2*b.economics.horizon;++t){
            auto *weather=rules->getWeatherAtDay((t+b.player)/2,(t+b.player)%2);
            if(!weather)weather=rules->getWeather(rules->getStartWeather());
            if(weather!=rules->getCurrentWeather())return fail(QString("MOVEMENT_RULE_CHANGE_T%1").arg(t));
        }
        auto owner=[&](int live){return live<0?-1:live==b.player?0:1;};
        b.cellSite.assign(b.width*b.height,-1);b.economics.openingFunds=terrain.production->funds;
        w.funds=b.economics.openingFunds;w.remainingSlots=terrain.production->availableUnits;
        for(const auto &p:properties){
            const TilePoint at{p.x,p.y};
            auto *building=within(b,at)?map.getTerrain(at.x,at.y)->getBuilding():nullptr;
            if(!p.visible || !building || p.coveredTileCount!=1 || b.cellSite[cell(b,at)]!=-1 || p.baseIncome<0)
                return fail(QString("PROPERTY_FOOTPRINT_OR_INCOME_%1_%2").arg(p.x).arg(p.y));
            Site s{at,p.capturable,p.objective==ObjectiveKind::Headquarters,{}};
            FactoryEconomy::Site site;site.owner=owner(p.ownerId);site.production=p.canProduce;
            for(int side=0;side<2;++side){
                auto *player=map.getPlayer(side?1-b.player:b.player);
                if(!player || player->getCO(0) || player->getCO(1))return fail("UNBOUND_COMMANDER_INCOME_OR_MOVEMENT");
                const auto rate=double(p.baseIncome)*player->getFundsModifier();
                if(!std::isfinite(rate) || rate<0 || rate>std::numeric_limits<int>::max())return fail("INVALID_NATIVE_PROPERTY_RATE");
                site.income[side]=toMilliFunds(static_cast<int>(rate));
                if(site.owner==side && site.income[side]!=toMilliFunds(building->getIncome()))return fail("NATIVE_PROPERTY_RATE_DISAGREES");
            }
            b.cellSite[cell(b,at)]=int(b.sites.size());b.sites.push_back(std::move(s));
            b.siteIds.emplace(std::pair{at.x,at.y},int(b.sites.size())-1);
            b.economics.sites.push_back(site);w.owners.push_back(site.owner);
        }
        for(int side=0;side<2;++side){
            MilliFunds income{};auto *player=map.getPlayer(side?1-b.player:b.player);
            if(!projectedIncome(b,w,side,income) || income!=toMilliFunds(player->calcIncome()))
                return fail("UNBOUND_OTHER_INCOME_SOURCE");
        }
        std::map<std::pair<int,QString>,int> unitType;
        for(int i=0;i<int(knowledge.units().size());++i){
            const auto &known=knowledge.units()[i];const auto &pawn=terrain.units[i];
            const int side=owner(known.ownerId),grid=pawn.travelGrid>=0?pawn.travelGrid:pawn.grid;
            auto *u=within(b,pawn.tile)?map.getTerrain(pawn.tile.x,pawn.tile.y)->getUnit():nullptr;
            if(!u || !pawn.present || pawn.engineUnitId!=u->getUniqueID() || side<0 || u->getLoadedUnitCount() ||
               u->isStatusStealthed() || u->hasTemporaryBonuses() || u->getHp()!=10.0 ||
               grid<0 || grid>=int(terrain.grids.size()) || known.fuel<0)
                return fail(QString("UNBOUND_SERVICE_MOVEMENT_OR_ROSTER_UNIT_%1").arg(pawn.engineUnitId));
            if(!debit(work,16+8LL*properties.size()))return fail("PROGRAM_ROSTER_WORK_LIMIT");
            Profile p{known.unitId,side,pawn.movementBeforeFuel,u->getMaxFuel(),&terrain.grids[grid],
                known.canCapture,known.ignoresUnitCollision};
            p.armed=known.hasWeapons;p.minRange=known.minRange;p.maxRange=known.maxRange;p.mobileFire=known.canMoveAndFire;
            const int klass=u->getUnitType();unitType[{side,known.unitId}]=klass;
            for(const auto &site:properties){
                p.rate.push_back(known.canCapture && site.capturable?u->getCaptureRate(QPoint(site.x,site.y)):0);
                p.refills.push_back(repairsUnitType(site.repairTypeMask,klass));
            }
            const int profile=int(b.types.size());b.types.push_back(std::move(p));
            w.bodies.push_back({pawn.engineUnitId,profile,known.fuel,known.capturePoints,0,-1,-1,-1,pawn.tile});
            if(!b.economics.originalActors.emplace(pawn.engineUnitId,side).second ||
               pawn.engineUnitId>=std::numeric_limits<int>::max()-1)return fail("NATIVE_ACTOR_ID_COLLISION");
            w.nextId=std::max(w.nextId,pawn.engineUnitId+1);
        }
        std::map<std::tuple<int,QString,const MobilityCostGrid*,int,int>,int> paidProfiles;
        for(const auto &factory:terrain.production->sites){
            const int j=b.cellSite[cell(b,factory.tile)];
            if(j<0 || !b.economics.sites[j].production)return fail("PRODUCTION_PROPERTY_NOT_IN_NATIVE_ROSTER");
            for(int side=0;side<2;++side)for(const auto &option:factory.menu[side]){
                if(!debit(work,4))return fail("PAID_PROFILES_WORK_LIMIT");
                if(!option.movementGrid || option.movement<=0 || option.initialFuel<0 || option.transportCapacity ||
                   option.supportsUnits || option.startingHp!=10.0)continue;
                const auto key=std::tuple{side,option.type,option.movementGrid.get(),option.movement,option.initialFuel};
                auto [it,fresh]=paidProfiles.emplace(key,int(b.types.size()));
                if(fresh){
                    if(!debit(work,24+8LL*properties.size()))return fail("PAID_PROFILES_WORK_LIMIT");
                    Profile p{option.type,side,option.movement,option.initialFuel,option.movementGrid.get(),
                        !option.captureRate.empty(),option.ignoresUnitCollision};
                    p.armed=!option.screeningWeapons[0].isEmpty() || !option.screeningWeapons[1].isEmpty();
                    p.minRange=option.minRange;p.maxRange=option.maxRange;p.mobileFire=option.mobileFire;
                    auto found=unitType.find({side,option.type});
                    if(found==unitType.end()){
                        Unit preview(option.type,map.getPlayer(side?1-b.player:b.player),false,&map);
                        found=unitType.emplace(std::pair{side,option.type},preview.getUnitType()).first;
                    }
                    for(const auto &site:properties){
                        const auto c=option.captureRate.find(site.y*b.width+site.x);
                        p.rate.push_back(c==option.captureRate.end()?0:c->second);
                        p.refills.push_back(repairsUnitType(site.repairTypeMask,found->second));
                    }
                    b.types.push_back(std::move(p));
                }
                b.sites[j].menu[side].emplace(option.type,std::pair{it->second,option.cost});
            }
        }
        bool opposingProduction=false;
        for(int j=0;j<int(b.sites.size());++j)opposingProduction|=w.owners[j]==1 && b.economics.sites[j].production;
        if(opposingProduction && !incumbent["productionOrders"].isArray())return fail("OPPONENT_DATED_PORTFOLIO_MISSING");
        b.opposingSearchLimited=opposingProduction && incumbent["productionWorkLimited"].toBool(true);
        for(const auto &v:incumbent["productionOrders"].toArray()){
            const auto o=v.toObject();if(!o["enemy"].toBool())continue;
            const auto f=o["factory"].toArray();const auto price=o["price"].toInteger(-1);const int day=o["buildDay"].toInt(-1);
            if(f.size()!=2 || day<0 || price<=0 || price%MILLI_FUNDS_PER_FUND)return fail("UNBOUND_OPPOSING_DATED_ORDER");
            b.opposingPurchases[2*day+1].append(QJsonObject{{"x",f[0]},{"y",f[1]},{"type",o["type"]},{"price",qint64(price/MILLI_FUNDS_PER_FUND)}});
        }
        b.economics.procedure="dated-stock-capture-programme-v1";
        b.economics.root=digest(QJsonObject{{"map",QString::fromLatin1(map.getMapHash().toHex())},
            {"day",b.day},{"player",b.player},{"board",snapshot(b,w)}}).toStdString();
        return out;
    }

    inline std::optional<Opening> nativeProgramOpening(const LiveBoard &live,const MasterAssignmentProblem &problem,
        const ReplyPassEvaluation &team,const QJsonObject &physical,const QJsonObject &actualBuyer,
        QJsonObject projection,const QJsonObject &quote,QString &why,std::int64_t &work)
    {
        const auto refuse=[&](QString s)->std::optional<Opening>{why=std::move(s);return {};};
        if(!team.valued() || !team.replay.complete() || !team.shots.empty())return refuse("OPENING_COMBAT_OR_ORDERS_UNPRICED");
        for(const auto &key:{"orders","seed","drawCounter","remainingFunds"})if(!quote.contains(key) || quote[key]!=actualBuyer[key])
            return refuse("ACTUAL_F0_BUYER_MISMATCH");
        const auto &b=live.board;Opening out;
        out.physicalTeam=physical;out.identity=digest(physical);out.firstBuyer=quote;out.firstProjection=std::move(projection);
        out.afterOrders=live.initial;auto &w=out.afterOrders;
        for(const auto &a:team.actors){
            if(!debit(work,12+w.bodies.size()))return refuse("OPENING_TEAM_WORK_LIMIT");
            const auto &m=problem.actors()[a.actorSlot];const auto &c=m.candidates[a.candidateIndex];
            const auto &native=team.replay.plan.action(a.actionIndex);
            const auto actor=std::find_if(w.bodies.begin(),w.bodies.end(),[&](const Body &u){return u.id==m.engineUnitId;});
            if(actor==w.bodies.end() || actor->at!=c.origin || native.destination!=c.destination ||
               (native.actionId!="ACTION_WAIT" && native.actionId!="ACTION_CAPTURE") ||
               c.source.movementCost<0 || actor->fuel<c.source.movementCost)return refuse("UNSUPPORTED_OR_UNBOUND_F0_ORDER");
            actor->fuel-=c.source.movementCost;if(actor->at!=c.destination)actor->carry=0;
            actor->at=c.destination;
            if(native.actionId=="ACTION_CAPTURE"){
                const int site=b.cellSite[cell(b,actor->at)];const auto &profile=b.types[actor->profile];
                if(site<0 || w.owners[site]==0 || b.sites[site].headquarters || profile.rate[site]<=0)
                    return refuse("F0_CAPTURE_OWNER_OR_TERMINAL_UNBOUND");
                actor->carry+=profile.rate[site];
                if(actor->carry>=b.threshold){
                    out.completed.push_back({actor->id,site,w.owners[site]});
                    w.owners[site]=0;actor->carry=0;
                }
            }
        }
        const auto trueUnits=out.firstProjection["units"].toArray();
        if(trueUnits.size()!=int(w.bodies.size()))return refuse("POST_ORDER_ROSTER_SIZE_CHANGED");
        std::set<int> checked;
        for(const auto &v:trueUnits){
            const auto u=v.toObject();const int id=u["id"].toInt(-1);
            const auto it=std::find_if(w.bodies.begin(),w.bodies.end(),[&](const Body &a){return a.id==id;});
            if(it==w.bodies.end() || !checked.insert(id).second || it->at!=TilePoint{u["x"].toInt(-1),u["y"].toInt(-1)} ||
               u["hp"].toDouble()!=10 || u["capPoints"].toInt(-1)!=it->carry)return refuse("F0_UNIT_REPLAY_MISMATCH");
        }
        std::set<int> sites;
        for(const auto &v:out.firstProjection["properties"].toArray()){
            const auto s=v.toObject();const auto f=b.siteIds.find({s["x"].toInt(-1),s["y"].toInt(-1)});
            if(f==b.siteIds.end() || !sites.insert(f->second).second)return refuse("F0_PROPERTY_IDENTITY_MISMATCH");
            const int owner=w.owners[f->second];if(s["owner"].toInt(-8)!=(owner<0?-1:owner?1-b.player:b.player))return refuse("F0_PROPERTY_OWNER_MISMATCH");
        }
        if(sites.size()!=b.sites.size())return refuse("F0_PROPERTY_ROSTER_INCOMPLETE");
        return out;
    }
}
