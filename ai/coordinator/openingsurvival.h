#pragma once

#include "ai/coordinator/openinglanebindings.h"
#include "ai/coordinator/screenfirebindings.h"
#include "ai/coordinator/openingsurvivalbindings.h"
#include "ai/coordinator/replypassevaluation.h"
#include "ai/coreai.h"
#include "game/building.h"
#include "game/gameenums.h"
#include "game/gamerules.h"
#include "game/unitpathfindingsystem.h"
#include "game/victoryrule.h"
#include <QScopeGuard>

namespace Coordinator
{
    // One complete friendly volley with no purchase. The opponent has exactly
    // one unit and owns no building with a native production/action menu.
    // Nothing here uses the actor's location as a valuation input.
    inline QJsonObject checkedLastHostileVolley(GameMap &map,const BattlefieldKnowledge &knowledge,
        const PlannedAction &order,const QJsonObject &buyer,std::int64_t &work,QString &why)
    {
        const auto open=[&](QString cause){why=std::move(cause);return QJsonObject{};};
        if(buyer["openingSource"]!="NO_FUNDED_SLOT" || !buyer.contains("spendableFunds") ||
           !buyer.contains("affordableBuildSlots") || buyer["affordableBuildSlots"].toInt(-1)!=0 ||
           !buyer["modeledOpening"].isArray() || !buyer["modeledOpening"].toArray().isEmpty() ||
           !buyer["unpricedExposedOffers"].toArray().isEmpty())return {};
        if(order.actionId!="ACTION_FIRE" || order.path.empty())return {};
        auto *gun=map.getUnit(order.unitId),*victim=map.getUnit(order.targetUnitId);
        auto *player=map.getCurrentPlayer();auto *rules=map.getGameRules();
        if(!gun || !victim || !player || !rules || map.getPlayerCount()!=2 ||
           gun->getOwner()!=player || victim->getOwner()==player || !player->isEnemy(victim->getOwner()) ||
           gun->getHasMoved() || gun->hasTemporaryBonuses() || victim->hasTemporaryBonuses() ||
           victim->getOwner()->getUnitCount()!=1 || gun->getLoadedUnitCount() || victim->getLoadedUnitCount())return {};
        if(buyer["spendableFunds"].toInteger(-1)!=toMilliFunds(player->getFunds()))
            return open("BUYER_WALLET_MISMATCH");
        const std::int64_t shapeCost=256+8LL*map.getMapWidth()*map.getMapHeight()+16LL*order.path.size();
        if(work<shapeCost)return open("WORK_LIMIT");
        work-=shapeCost;
        if(player->getIsDefeated() || victim->getOwner()->getIsDefeated() || rules->getVictory() ||
           rules->getDrawVotingRunning() || rules->getRoundTimeMs()!=0 ||
           rules->getGameRuleSize()!=0 || rules->getFogMode()!=GameEnums::Fog_Off || rules->getRandomWeather() ||
           !rules->getCurrentWeather() || rules->getCurrentWeather()->getWeatherId()!="WEATHER_1SUN" ||
           map.getIsHumanMatch() || map.getCampaign() || !map.getGameScript() || map.getGameScript()->hasLoadedScript() ||
           !map.getGameScript()->getScriptFile().isEmpty())return open("UNSUPPORTED_VICTORY_OR_PHASE_RULES");
        if(!player->getBaseGameInput() || player->getBaseGameInput()->getAiType()==GameEnums::AiTypes_Human)
            return open("UNVERIFIED_KILL_ACHIEVEMENT");
        for(int i=0;i<2;++i)if(map.getPlayer(i)->getCO(0) || map.getPlayer(i)->getCO(1))
            return open("UNPRICED_COMMANDER_POWER_OR_TURN_CALLBACK");
        if(rules->getVictoryRuleSize()==0)return open("UNBOUND_VICTORY_RULES");
        for(int i=0;i<rules->getVictoryRuleSize();++i){
            const auto id=rules->getVictoryRuleAtIndex(i)->getRuleID();
            if(id!="VICTORYRULE_NOHQ" && id!="VICTORYRULE_NOUNITS")return open("OTHER_VICTORY_RULE_"+id);
        }
        const auto from=QPoint(order.path.front().x,order.path.front().y);
        const auto to=QPoint(order.destination.x,order.destination.y),target=QPoint(order.target.x,order.target.y);
        if(from!=gun->getPosition() || target!=victim->getPosition() || order.path.back()!=order.destination)
            return open("UNBOUND_FIRE_LOCATION");
        std::set<QString> terrainKinds,buildingKinds,unitKinds;
        std::vector<QString> ids;HeadquartersTurnUnits turn;
        bool ownHQ=false;
        for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x){
            auto *t=map.getTerrain(x,y);
            if(t->getHasStartOfTurn())return open("DATED_TERRAIN_CALLBACK");
            ids.push_back(t->getID());terrainKinds.insert(t->getTerrainID());
            if(auto *u=t->getUnit()){
                if(u->getLoadedUnitCount() || u->hasTemporaryBonuses() || u->getHp()<=0)return open("UNSUPPORTED_ACTOR_STATE");
                unitKinds.insert(u->getUnitID());
                if(u->getOwner()==player){
                    if(u!=gun && !u->getHasMoved())return open("INCOMPLETE_FRIENDLY_TEAM");
                    turn.outgoing.insert(u->getUnitID());
                }else if(u!=victim)return open("ADDITIONAL_OPPONENT_ACTOR");
            }
            if(auto *b=t->getBuilding()){
                const auto type=b->getBuildingID();buildingKinds.insert(type);
                ownHQ|=b->getOwner()==player && type=="HQ";
                if(b->getOwner()==victim->getOwner() && type!="HQ" && type!="TOWN")
                    return open("OPPONENT_BUILDING_ACTION_"+type);
            }
        }
        if(!ownHQ)return open("NO_RETAINED_FRIENDLY_HEADQUARTERS");
        auto *enemy=victim->getOwner();const auto &methodRoot=QCoreApplication::applicationDirPath()+"/resources/scripts";
        thread_local HqBindingDetail::StockMethods screen(methodRoot,QStringList{"perform","performPostAnimation",
            "canBePerformed","getMissileBuildings","init","getMovementType","getBaseDamage"});
        thread_local HqBindingDetail::StockMethods service(methodRoot,true);
        const auto *hq=HqBindingDetail::asset(victim->getUnitID());
        if(!hq || !hq->recruit || !ordinaryOpeningCaptureBindings(*Interpreter::getInstance(),victim->getUnitID(),{},work,turn))
            return open("UNVERIFIED_STOCK_HQ_RULES_OR_BOUNDARY");
        QString reason;
        if(!ordinaryOpeningServiceBindings(*Interpreter::getInstance(),map,knowledge,{},service,work,reason))
            return open(reason);
        RefinementLedger ledger;ledger.open(std::max<std::int64_t>(0,work));
        const auto debit=qScopeGuard([&]{work-=ledger.stats().spent();});
        auto &engine=*Interpreter::getInstance();using Kind=GuardedNativeCodeKind;
        constexpr auto bound=GuardedNativeBindingStatus::Bound;
        const auto law=readScreenBoundRules(engine,ledger,screen,terrainKinds,buildingKinds,{"WEATHER_1SUN"},unitKinds,unitKinds,ids);
        if(!law.reason.isEmpty())return open(law.reason);
        for(const auto &weapon:{gun->getWeapon1ID(),gun->getWeapon2ID(),victim->getWeapon1ID(),victim->getWeapon2ID()})
            if(!weapon.isEmpty() && (!law.rows.contains(weapon) || !law.rows.at(weapon)))return open("UNBOUND_WEAPON_"+weapon);
        GuardedNativeCodeCatalog catalog(ledger);auto observation=catalog.observe(engine);
        if(observation.status()!=bound || catalog.authenticateCurrentFireCallbacks(engine).status!=bound ||
           catalog.authenticateCurrentAbsentHooks(engine,Kind::Fire).status!=bound ||
           catalog.authenticateStockActionCallbacks(engine,Kind::Trap).status!=bound ||
           catalog.authenticateStockPolicyCallbacks(engine,Kind::Ranking).status!=bound ||
           catalog.authenticateStockPolicyTables(engine,Kind::Ranking).status!=bound ||
           catalog.authenticateIntrinsics(engine).status!=bound ||
           catalog.authenticateTerminalEnums(engine,GameEnums::nativeInstance(),GameEnums::staticMetaObject).status!=bound ||
           catalog.authenticateStockContextEnums(engine,GameEnums::nativeInstance(),GameEnums::staticMetaObject).status!=bound)
            return open("UNBOUND_NATIVE_FIRE_OR_RULE_ENVIRONMENT");
        for(const auto &type:unitKinds){
            const auto k=catalog.kindForGlobal(type);
            if(k.status!=bound || catalog.authenticateUnitCallbacks(engine,k.kind).status!=bound ||
               catalog.authenticateCurrentAbsentHooks(engine,k.kind).status!=bound)return open("UNBOUND_ACTOR_"+type);
        }
        if(!openingSurvivalGlobalBindings(engine,map,*enemy,catalog,ledger,why))return {};
        if(!ledger.consume(256+32*order.path.size(),RefinementWork::MasterPreparation))return open("WORK_LIMIT");
        UnitPathFindingSystem paths(&map,gun,player);paths.explore();
        std::vector<QPoint> path;
        for(const auto p:order.path)path.insert(path.begin(),QPoint(p.x,p.y));
        const auto length=paths.getCosts(path);
        if(length<0 || !paths.isReachable(to.x(),to.y()) || (to!=from && map.getTerrain(to.x(),to.y())->getUnit()) ||
           !gun->isAttackableFromPosition(victim,to))return open("UNAVAILABLE_FIRST_VOLLEY");
        // A cheapest route proves reachability but cannot validate an arbitrary
        // supplied route. Every step, its blocking occupancy and its cost bind.
        int exactCost=0;
        for(std::size_t i=1;i<order.path.size();++i){
            const auto p=order.path[i-1],q=order.path[i];
            if(!map.onMap(q.x,q.y) || std::abs(p.x-q.x)+std::abs(p.y-q.y)!=1)return open("INVALID_NATIVE_STEP");
            const int cost=paths.getCosts(paths.getIndex(q.x,q.y),q.x,q.y,p.x,p.y,exactCost);
            if(cost<=0 || (map.getTerrain(q.x,q.y)->getUnit() && map.getTerrain(q.x,q.y)->getUnit()->getOwner()!=player))
                return open("INTERRUPTED_NATIVE_PATH");
            exactCost+=cost;
        }
        if(length!=exactCost || exactCost>gun->getMovementpoints(from) ||
           (gun->getFuel()>=0 && exactCost+gun->getMovementFuelCostModifier(exactCost)>gun->getFuel()))
            return open("UNAVAILABLE_MOVEMENT_OR_FUEL");
        const auto minHit=CoreAI::calcVirtuelUnitDamage(&map,gun,0.,to,GameEnums::LuckDamageMode_Min,
            victim,0.,target,GameEnums::LuckDamageMode_Max,false,false);
        const auto lethalAt=std::ceil(victim->getHp()*10.-1e-9);
        if(!std::isfinite(minHit.x()) || minHit.y()<0 || std::floor(minHit.x())<lethalAt ||
           minHit.width()>0)return open("OPPONENT_SURVIVES_OR_COUNTERSTRIKES");
        return {{"status","ALL_OPPONENT_OPENINGS_SURVIVE"},{"scope","OBSERVED_F0_TEAM_AND_BUYER"},
            {"basis","LAST_HOSTILE_UNIT_REMOVED_AT_MINIMUM_NATIVE_DAMAGE_NO_HOSTILE_BUILD_OR_GLOBAL_ATTACK"},
            {"actions",QJsonArray{QString("F0 ACTION_FIRE %1 -> %2").arg(gun->getUniqueID()).arg(victim->getUniqueID())}},
            {"purchaseBoundary",buyer},{"hostileUnitsBefore",1},{"hostileUnitsAfterEveryDraw",0},
            {"minimumNativeFire",minHit.x()},{"requiredDamageSteps",lethalAt},{"actor",gun->getUniqueID()},
            {"target",victim->getUniqueID()},{"enemyPlayer",enemy->getPlayerID()},{"phase","THROUGH_E0"}};
    }
    inline QJsonObject oneVolleyOpeningSurvival(GameMap &map,const BattlefieldKnowledge &knowledge,
        const ReplyPassEvaluation &team,const QJsonObject &buyer,std::int64_t &work,QString &why)
    {
        if(team.actors.size()!=1)return {};
        const auto &a=team.actors.front();
        if(a.actionIndex<0 || a.ownShot.target==NO_UNIT)return {};
        const auto &ordered=team.replay.plan.action(a.actionIndex);
        if(ordered.unitId!=a.engineUnitId)return {};
        return checkedLastHostileVolley(map,knowledge,ordered,buyer,work,why);
    }
}
