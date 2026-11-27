#pragma once
#include "ai/coordinator/singleactivationbindings.h"
#include "ai/coordinator/singleactivationpath.h"
#include "ai/coordinator/openinglanebindings.h"
#include "ai/coordinator/replypassevaluation.h"
#include "coreengine/memorymanagement.h"
#include "gameinput/basegameinputif.h"
#include "game/gameaction.h"
#include "game/victoryrule.h"
#include "game/unitpathfindingsystem.h"
#include "coreengine/scriptvariables.h"
#include "coreengine/scriptvariable.h"

namespace Coordinator
{
    struct SingleActivationHeadquarters
    {
        QJsonObject survives,loss;
        QString open;
    };
    inline SingleActivationHeadquarters assessSingleActivationHeadquarters(GameMap &map,const BattlefieldKnowledge &knowledge,
        const PlannedAction &order,const QJsonObject &buyer,int threshold,std::int64_t &budget)
    {
        SingleActivationHeadquarters out;
        const auto refuse=[&](QString why){out.open=std::move(why);return out;};
        if(threshold!=20 || order.path.empty() || (order.actionId!="ACTION_WAIT" && order.actionId!="ACTION_BUILD_TEMP_AIRPORT" &&
           order.actionId!="ACTION_BUILD_TEMP_HARBOUR") || !buyer["modeledOpening"].isArray() ||
           buyer["openingSource"]!="NO_FUNDED_SLOT" || !buyer["modeledOpening"].toArray().isEmpty() ||
           buyer["affordableBuildSlots"].toInt(-1)!=0 || !buyer["unpricedExposedOffers"].toArray().isEmpty())return out;
        auto *owner=map.getCurrentPlayer();if(!owner || map.getPlayerCount()!=2)return out;
        auto *enemy=map.getPlayer(1-owner->getPlayerID());auto *guard=map.getUnit(order.unitId);
        if(!enemy || !guard || !owner->isEnemy(enemy) || enemy->getUnitCount()!=1 || guard->getOwner()!=owner ||
           guard->getHasMoved() || buyer["spendableFunds"].toInteger(-1)!=toMilliFunds(owner->getFunds()))return out;
        auto *co=enemy->getCO(0);
        if(owner->getIsDefeated() || enemy->getIsDefeated() || !co || enemy->getCO(1) || owner->getCO(0) || owner->getCO(1))return out;
        auto *rules=map.getGameRules();
        if(!rules || rules->getVictory() || rules->getVictoryRuleSize()!=1 || rules->getGameRuleSize() || rules->getDrawVotingRunning() ||
           rules->getVictoryRuleAtIndex(0)->getRuleID()!="VICTORYRULE_NOHQ" || rules->getRoundTimeMs() ||
           rules->getRandomWeather() || rules->getFogMode()!=GameEnums::Fog_Off || !rules->getCurrentWeather() ||
           rules->getCurrentWeather()->getWeatherId()!="WEATHER_1SUN" || map.getCampaign() || map.getIsHumanMatch() ||
           !map.getGameScript() || map.getGameScript()->hasLoadedScript() || !map.getGameScript()->getScriptFile().isEmpty() ||
           !owner->getBaseGameInput() || !enemy->getBaseGameInput() || owner->getBaseGameInput()->getAiType()==GameEnums::AiTypes_Human ||
           enemy->getBaseGameInput()->getAiType()==GameEnums::AiTypes_Human)return refuse("UNSUPPORTED_VICTORY_OR_TURN_DOMAIN");
        auto *active=rules->getVictoryRuleAtIndex(0)->getVariables()->getVariable("Active"+QString::number(owner->getPlayerID()));
        if(!active || !active->readDataBool())return refuse("HQ_RULE_INACTIVE");
        const auto xy=[](QPoint p){return QJsonArray{p.x(),p.y()};};
        const QPoint from{order.path.front().x,order.path.front().y},to{order.destination.x,order.destination.y};
        if(from!=guard->getPosition() || order.path.back()!=order.destination || !map.onMap(to.x(),to.y()) ||
           (to!=from && map.getTerrain(to.x(),to.y())->getUnit()))return refuse("UNBOUND_FRIENDLY_ORDER");
        const int width=map.getMapWidth(),height=map.getMapHeight();
        const auto charge=512+32LL*width*height+32LL*order.path.size();
        if(budget<charge)return refuse("WORK_LIMIT");
        budget-=charge;
        std::set<std::pair<int,int>> occupants;QPoint home{-1,-1};Unit *raider=nullptr;int homes=0,enemyHomes=0;
        HeadquartersTurnUnits turn;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            auto *t=map.getTerrain(x,y);auto *u=t->getUnit();auto *b=t->getBuilding();
            if(t->getHasStartOfTurn() || t->getHp()>0 || (b && b->getHp()>0))return refuse("DESTRUCTIBLE_OR_ACTIVE_TERRAIN");
            if(b && b->getBuildingID()=="SILO_ROCKET")return refuse("HOSTILE_CAN_LAUNCH_AREA_ACTION");
            if(u){
                if(u->getLoadedUnitCount() || u->hasTemporaryBonuses() || u->isStatusStealthed() || u->getHp()<=0)
                    return refuse("UNSUPPORTED_UNIT_STATE");
                if(u->getOwner()==owner){
                    if(u!=guard && !u->getHasMoved())return refuse("INCOMPLETE_FRIENDLY_TEAM");
                    const auto end=u==guard?to:QPoint{x,y};occupants.emplace(end.x(),end.y());
                    turn.outgoing.insert(u->getUnitID());
                }else if(u->getOwner()==enemy && !raider)raider=u;
                else return refuse("ADDITIONAL_ACTOR");
            }
            if(b && b->getOwner()==owner && (b->getBuildingID()=="FORTHQ" || b->getBuildingID()=="FIELD_BASE"))
                return refuse("ADDITIONAL_HEADQUARTERS_TYPE");
            if(b && b->getBuildingID()=="HQ" && b->getOwner()==owner){home={x,y};++homes;}
            if(b && b->getOwner()==enemy && b->getBuildingID()=="HQ")++enemyHomes;
            if(b && b->getOwner()==enemy && b->getBuildingID()!="HQ" && b->getBuildingID()!="TOWN")
                return refuse("OPPONENT_CAN_PRODUCE");
        }
        if(homes!=1 || enemyHomes==0 || !raider || (raider->getMaxFuel()>0 && raider->getFuel()<0) || raider->getHp()!=10 || raider->getHasMoved() ||
           raider->getIgnoreUnitCollision() || !raider->getActionList().contains("ACTION_CAPTURE") ||
           raider->getMovementType()!="MOVE_FEET" || guard->getMovementType()!="MOVE_TANK")
            return refuse("UNSUPPORTED_SINGLE_MOVER");
        // With no other hostile body there is no join or load partner; require
        // the enabling unit to be on a bare tile, so a CO-unit action cannot
        // move it to a different post and then leave a capture available.
        if(map.getTerrain(raider->getX(),raider->getY())->getBuilding())return refuse("HOSTILE_ORIGIN_CAN_SERVICE_OR_ATTACH");
        turn.incomingMinimumFuel.emplace(raider->getUnitID(),raider->getFuel());
        auto &engine=*Interpreter::getInstance();QString reason;
        if(!ordinaryOpeningCaptureBindings(engine,raider->getUnitID(),{},budget,turn))return refuse("UNVERIFIED_TITLE_OR_MOVEMENT_HOOKS");
        thread_local HqBindingDetail::StockMethods service(QCoreApplication::applicationDirPath()+"/resources/scripts",true);
        if(!ordinaryOpeningServiceBindings(engine,map,knowledge,{},service,budget,reason))return refuse(reason);
        RefinementLedger ledger;ledger.open(std::max<std::int64_t>(0,budget));
        const auto debit=qScopeGuard([&]{budget-=ledger.stats().spent();});
        if(!singleActivationPowerBindings(engine,map,*co,*enemy,raider->getUnitID(),guard->getUnitID(),order.actionId,ledger,reason))return refuse(reason);
        if(order.actionId!="ACTION_WAIT" && std::int64_t(guard->getCapturePoints())+guard->getCaptureRate(to)>=20)
            return refuse("COMPLETED_CONSTRUCTION_REQUIRES_NEW_BUILDING_HOOKS");
        UnitPathFindingSystem ownPaths(&map,guard,owner);ownPaths.explore();
        if(!ownPaths.isReachable(to.x(),to.y()))return refuse("FRIENDLY_PATH_UNREACHABLE");
        int ownCost=0;
        for(std::size_t i=1;i<order.path.size();++i){
            const auto a=order.path[i-1],b=order.path[i];
            if(!map.onMap(b.x,b.y)||std::abs(a.x-b.x)+std::abs(a.y-b.y)!=1)return refuse("BAD_FRIENDLY_STEP");
            const int cost=ownPaths.getCosts(ownPaths.getIndex(b.x,b.y),b.x,b.y,a.x,a.y,ownCost);
            if(cost<=0)return refuse("IMPASSABLE_FRIENDLY_STEP");ownCost+=cost;
        }
        if(ownCost>guard->getMovementpoints(from) || (guard->getFuel()>=0 &&
           ownCost+guard->getMovementFuelCostModifier(ownCost)>guard->getFuel()))return refuse("UNAFFORDABLE_FRIENDLY_STEP");
        auto own=MemoryManagement::create<GameAction>(order.actionId,&map,0);
        own->setTarget(from);std::vector<QPoint> ownPath;
        for(auto i=order.path.rbegin();i!=order.path.rend();++i)ownPath.emplace_back(i->x,i->y);
        own->setMovepath(ownPath,ownCost);
        if(!own->canBePerformed() || !own->isFinalStep())return refuse("NATIVE_FRIENDLY_ACTION_REFUSED");
        const auto start=raider->getPosition();
        const auto relaxed=singleActivationPath(width,height,start,home,occupants,[&](QPoint a,QPoint b){
            // If F0 constructed anything, give the hostile mover a free crossing
            // of that changed tile, regardless of its future movement table.
            if(order.actionId!="ACTION_WAIT" && (a==to || b==to))return 0;
            return raider->getBaseMovementCosts(b.x(),b.y(),a.x(),a.y())<0?-1:0;
        });
        QJsonArray actions{QString("F0 %1 %2 (%3,%4)->(%5,%6)").arg(order.actionId).arg(guard->getUniqueID())
            .arg(from.x()).arg(from.y()).arg(to.x()).arg(to.y())};
        QJsonObject evidence{{"scope","OBSERVED_F0_TEAM_AND_BUYER"},{"purchaseBoundary",buyer},{"actions",actions},
            {"hostile",raider->getUniqueID()},{"home",xy(home)},{"hostileActivationCapacity",1},{"unboundedMovementPathExists",bool(relaxed)},
            {"commanderSurface","CHECKED_STAT_OR_PRESENTATION_ONLY"}};
        if(!relaxed){
            evidence["status"]="ALL_OPPONENT_OPENINGS_SURVIVE";
            evidence["basis"]="NO_CAPTURE_PATH_EVEN_WITH_UNLIMITED_MOVEMENT_AND_CAPTURE_STRENGTH";
            out.survives=evidence;return out;
        }
        const auto prior=co->getPowerMode();const bool enabled=co->getCoRangeEnabled();
        const auto restore=qScopeGuard([&]{co->setPowerMode(prior);co->setCoRangeEnabled(enabled);});
        co->setPowerMode(GameEnums::PowerMode_Off);co->setCoRangeEnabled(true);
        std::vector<std::pair<GameEnums::PowerMode,QString>> modes{{GameEnums::PowerMode_Off,{}}};
        if(co->canUsePower())modes.emplace_back(GameEnums::PowerMode_Power,"ACTION_ACTIVATE_POWER_CO_0");
        if(co->canUseSuperpower())modes.emplace_back(GameEnums::PowerMode_Superpower,"ACTION_ACTIVATE_SUPERPOWER_CO_0");
        auto *hq=map.getTerrain(home.x(),home.y())->getBuilding();
        for(const auto &[mode,activation]:modes){
            if(!activation.isEmpty() && !rules->getAllowedActions().contains(activation))continue;
            co->setPowerMode(mode);co->setCoRangeEnabled(mode==GameEnums::PowerMode_Off);
            const int rate=raider->getCaptureRate(home);const int carry=start==home?raider->getCapturePoints():0;
            if(carry+rate<threshold)continue;
            auto path=singleActivationPath(width,height,start,home,occupants,[&](QPoint a,QPoint b){
                if(order.actionId!="ACTION_WAIT" && (a==to || b==to))return -1;
                const int cost=raider->getMovementCosts(b.x(),b.y(),a.x(),a.y());return cost>0?cost:-1;
            });
            if(!path || path->cost>raider->getMovementpoints(start) || path->cost>std::numeric_limits<int>::max() ||
               (raider->getFuel()>=0 && path->cost+raider->getMovementFuelCostModifier(int(path->cost))>raider->getFuel()))continue;
            if(!hq || hq->getOwner()!=owner || !rules->getAllowedActions().contains("ACTION_CAPTURE") ||
               raider->getBaseMovementCosts(home.x(),home.y())<=0)continue;
            QJsonArray route;for(const auto &p:path->cells)route.append(xy(p));
            if(!activation.isEmpty())actions.append("E0 "+activation);
            actions.append(QString("E0 ACTION_CAPTURE %1").arg(raider->getUniqueID()));
            evidence["actions"]=actions;evidence["status"]="FEASIBLE_OPPONENT_OPENING_WINS";
            evidence["basis"]="LEGAL_GLOBAL_MODE_AND_MOBILE_LAST_HEADQUARTERS_CAPTURE";evidence["path"]=route;
            evidence["pathCost"]=static_cast<qint64>(path->cost);evidence["movement"]=raider->getMovementpoints(start);
            evidence["captureRate"]=rate;evidence["carry"]=carry;evidence["threshold"]=threshold;
            out.loss=evidence;return out;
        }
        return refuse("RELAXED_PATH_EXISTS_WITHOUT_LETHAL_NATIVE_WITNESS");
    }
}
