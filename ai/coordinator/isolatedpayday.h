#pragma once

#include "ai/coordinator/datedformulaledger.h"
#include "ai/coordinator/openingsurvival.h"
#include "ai/coordinator/singleactivationpath.h"
#include "ai/coordinator/singleactivationassets.h"
#include "gameinput/basegameinputif.h"
#include "coreengine/settings.h"
#include <deque>

namespace Coordinator
{
    struct IsolatedPaydayResult
    {
        QJsonObject proof;
        QString open;
        bool candidate{},interrupted{};
    };

    // A private neutral objective can pay at most once per payday. For this
    // class the only capper is a full-health purchase, which spends its build
    // activation. The movement, capture rules, fixed buyer and isolation must
    // all be established on the observed game before using those dates.
    inline IsolatedPaydayResult isolatedPayday(
        GameMap &map,const BattlefieldKnowledge &knowledge,const PlannedAction &order,const QJsonObject &buyer,
        int horizon,int threshold,MilliFunds opening,std::int64_t &work)
    {
        IsolatedPaydayResult out;
        const RefinementLedger *charging=nullptr;bool upstreamRefused=false;
        const auto gap=[&](QString reason){
            out.interrupted=work<=0 || upstreamRefused || (charging && charging->stats().refusals) || reason.contains("WORK_LIMIT");
            out.open=std::move(reason);return out;
        };
        if(horizon<4 || horizon>64 || threshold!=20 || order.actionId!="ACTION_WAIT" || order.path.empty() ||
           map.getPlayerCount()!=2 || !map.getCurrentPlayer())return out;
        auto *self=map.getCurrentPlayer(),*foe=map.getPlayer(1-self->getPlayerID());
        const std::array<Player*,2> players{self,foe};
        if(!foe || !self->isEnemy(foe) || self->getUnitCount()!=1 || foe->getUnitCount()!=1)
            return gap("SHARED_ARMY_OR_MULTIPLE_EXISTING_ACTIVATIONS");
        auto *rules=map.getGameRules();
        if(!rules || rules->getVictory() || rules->getVictoryRuleSize()!=1 ||
           rules->getVictoryRuleAtIndex(0)->getRuleID()!="VICTORYRULE_NOHQ" || rules->getGameRuleSize() ||
           rules->getUnitLimit() || rules->getRoundTimeMs() || rules->getDrawVotingRunning() ||
           rules->getResellValue()>1. || rules->getResellValue()<0. || rules->getFogMode()!=GameEnums::Fog_Off ||
           rules->getRandomWeather() || !rules->getCurrentWeather() || rules->getCurrentWeather()->getWeatherId()!="WEATHER_1SUN" ||
           rules->getWeather(rules->getStartWeather())!=rules->getCurrentWeather() ||
           map.getIsHumanMatch() || map.getCampaign() || !map.getGameScript() || map.getGameScript()->hasLoadedScript() ||
           !map.getGameScript()->getScriptFile().isEmpty())return gap("UNSUPPORTED_ECONOMIC_GAME_RULES");
        for(const auto &day:rules->getWeatherForecast())for(const auto index:day)
            if(rules->getWeather(index)!=rules->getCurrentWeather())return gap("CHANGING_ECONOMIC_CALENDAR");
        if(!rules->getAllowedActions().contains("ACTION_CAPTURE") || !rules->getAllowedActions().contains("ACTION_BUILD_UNITS") ||
           !rules->getAllowedActions().contains("ACTION_WAIT") || !rules->getAllowedActions().contains("ACTION_NEXT_PLAYER"))
            return gap("MISSING_ECONOMIC_ACTION");
        std::array<QString,2> recruits;
        for(int who=0;who<2;++who){
            auto *p=players[who];
            if(p->getIsDefeated() || p->getCO(0) || p->getCO(1) || p->getFundsModifier()!=1. ||
               !p->getBaseGameInput() || p->getBaseGameInput()->getAiType()==GameEnums::AiTypes_Human ||
               p->getBuildList().size()!=1)return gap("NOT_A_SINGLE_PRIVATE_RECRUIT_MARKET");
            recruits[who]=p->getBuildList().front();
        }
        out.candidate=true;
        if(opening!=0)return gap("UNBOUND_NONPROPERTY_OPENING_TERM");
        const int w=map.getMapWidth(),h=map.getMapHeight();
        if(w<1 || h<1 || std::int64_t(w)*h>10000 || work<128LL*w*h)return gap("MARKET_GEOMETRY_WORK_LIMIT");
        work-=128LL*w*h;
        const auto key=[&](QPoint p){return p.x()+w*p.y();};
        std::array<QPoint,2> ports{QPoint{-1,-1},QPoint{-1,-1}},hq=ports,town=ports;
        std::array<Unit*,2> guards{};
        std::set<QString> terrainTypes;
        std::vector<QPoint> neutral;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            auto *t=map.getTerrain(x,y);terrainTypes.insert(t->getTerrainID());
            if(t->getHp()>0 || t->getHasStartOfTurn())return gap("ACTIVE_OR_DESTRUCTIBLE_TERRAIN");
            if(auto *b=t->getBuilding()){
                const auto type=b->getBuildingID();
                if(b->getHp()>0 || (type!="HQ" && type!="FACTORY" && type!="TOWN"))return gap("ADDITIONAL_ECONOMIC_PROPERTY_RULES");
                auto *p=b->getOwner();
                if(!p){
                    if(type!="TOWN")return gap("TRANSFERABLE_PRODUCTION_OR_HEADQUARTERS");
                    neutral.emplace_back(x,y);
                }else{
                    const int side=p==self?0:p==foe?1:-1;
                    if(side<0)return gap("UNBOUND_PROPERTY_OWNER");
                    if(type=="HQ" || type=="FACTORY"){
                        auto &spot=type=="HQ"?hq[side]:ports[side];
                        if(spot.x()>=0)return gap("SHARED_PRODUCTION_QUEUE");
                        spot={x,y};
                    }
                }
            }
            if(auto *u=t->getUnit()){
                int who=u->getOwner()==self?0:u->getOwner()==foe?1:-1;
                if(who<0 || guards[who] || u->getLoadedUnitCount() || u->hasTemporaryBonuses() || u->isStatusStealthed() ||
                   u->getIgnoreUnitCollision() || u->getHp()!=10. || u->getCapturePoints()!=0 || u->canCapture() ||
                   u->getBaseMaxRange()!=1 || u->getFuel()<0)return gap("EXISTING_OR_NONLOCAL_ECONOMIC_CAPABILITY");
                guards[who]=u;
            }
        }
        if(neutral.size()!=2 || !guards[0] || !guards[1] || hq[0].x()<0 || hq[1].x()<0 || ports[0].x()<0 || ports[1].x()<0 ||
           guards[0]->getUniqueID()!=order.unitId || guards[0]->getHasMoved() || guards[0]->getPosition()!=ports[0] ||
           guards[1]->getPosition()==ports[1])return gap("MISSING_PRIVATE_SECTORS_OR_INITIAL_QUEUE");
        auto &engine=*Interpreter::getInstance();
        HeadquartersTurnUnits turn;
        for(auto *g:guards){turn.outgoing.insert(g->getUnitID());turn.incomingMinimumFuel.emplace(g->getUnitID(),g->getFuel());}
        QJsonArray possible;for(int k=0;k<2;++k)possible.append(QJsonObject{{"type",recruits[k]},{"enemy",bool(k)}});
        QString reason;
        for(const auto &type:recruits)if(!ordinaryOpeningCaptureBindings(engine,type,{},work,turn,&upstreamRefused))
            return gap("UNPRICED_NATIVE_CAPTURE_CAPABILITY");
        thread_local HqBindingDetail::StockMethods service(QCoreApplication::applicationDirPath()+"/resources/scripts",true);
        if(!ordinaryOpeningServiceBindings(engine,map,knowledge,possible,service,work,reason,false,&upstreamRefused) ||
           !ordinaryOpeningServiceBindings(engine,map,knowledge,possible,service,work,reason,true,&upstreamRefused))return gap(reason);
        {
            RefinementLedger ledger;ledger.open(work);charging=&ledger;
            const auto debit=qScopeGuard([&]{work-=ledger.stats().spent();});
            using K=GuardedNativeCodeKind;
            constexpr auto ok=GuardedNativeBindingStatus::Bound;
            GuardedNativeCodeCatalog c(ledger);
            if(c.observe(engine).status()!=ok || c.authenticateIntrinsics(engine).status!=ok ||
               c.authenticateStockWeatherPrefix(engine).status!=ok ||
               c.authenticateStockPolicyCallbacks(engine,K::Sun).status!=ok ||
               c.authenticateTerminalEnums(engine,GameEnums::nativeInstance(),GameEnums::staticMetaObject).status!=ok)
                return gap("UNVERIFIED_ECONOMIC_REALM");
            if(!openingSurvivalGlobalBindings(engine,map,*foe,c,ledger,reason,true))return gap(reason);
            if(c.authenticateCurrentFireCallbacks(engine).status!=ok ||
               c.authenticateStockActionCallbacks(engine,K::Wait).status!=ok ||
               c.authenticateStockActionCallbacks(engine,K::Capture).status!=ok)return gap("UNVERIFIED_ECONOMIC_ACTION");
            for(const auto k:{K::Headquarters,K::Town,K::Factory})
                if(c.authenticateStockPropertyCallbacks(engine,k).status!=ok)return gap("UNVERIFIED_OWNERSHIP_CALLBACKS");
            using namespace PowerBindingDetail;
            const std::vector<QString> dependencies{"ACTION_JOIN","ACTION_LOAD","ACTION_UNLOAD","ACTION_MISSILE",
                "ACTION_CO_UNIT_0","ACTION_CO_UNIT_1","ACTION_BUILD_UNITS","TOWN","BUILDING"};
            auto names=GuardedScriptGlobalIndex::prepare(engine,dependencies,ledger);
            GuardedScriptOwnIndexCache members(engine,ledger);GuardedScriptImageCache images(engine,ledger);
            static const QStringList keys{"canBePerformed","getMissileBuildings","onCaptured","addCaptureAnimationBuilding","perform","getStepData","isFinalStep"};
            thread_local HqBindingDetail::StockMethods methods(QCoreApplication::applicationDirPath()+"/resources/scripts",keys);
            const auto method=[&](const QString &id,const char *key,const HqBindingDetail::Asset &asset){
                const auto obj=names.find(engine,id,ledger);if(obj.status!=GuardedScriptPropertyStatus::Found)return false;
                const auto fn=guardedScriptProperty(obj.value,key,ledger,&members);
                return fn.status==GuardedScriptPropertyStatus::Found &&
                    guardedScriptContextOperands(fn.value,ledger).status==GuardedScriptContextOperands::GlobalScopeOnly &&
                    methods.matches(asset,key,guardedScriptFunctionImage(fn.value,ledger,&images),ledger);
            };
            for(const auto &a:{join,load,unload,missile,coUnit0,coUnit1})
                if(!method(a.id,"canBePerformed",a))return gap("UNVERIFIED_UNIT_OPTION_"+QString::fromLatin1(a.id));
            if(!method("ACTION_MISSILE","getMissileBuildings",missile))return gap("UNVERIFIED_AREA_ACTION");
            if(!method("TOWN","onCaptured",building))return gap("UNVERIFIED_TOWN_CAPTURE_HANDLER");
            if(!method("TOWN","addCaptureAnimationBuilding",building))return gap("UNVERIFIED_TOWN_CAPTURE_PRESENTATION");
            for(const auto *key:{"canBePerformed","isFinalStep","perform","getStepData"})
                if(!method("ACTION_BUILD_UNITS",key,*HqBindingDetail::asset("ACTION_BUILD_UNITS")))return gap("UNVERIFIED_BUILD_ACTION_"+QString::fromLatin1(key));
            const auto verifyType=[&](const QString &id){
                const auto k=c.kindForGlobal(id);
                return k.status==ok && (k.kind==K::Infantry || k.kind==K::Recon) &&
                    c.authenticateUnitCallbacks(engine,k.kind).status==ok &&
                    c.authenticateUnitActions(engine,k.kind).status==ok;
            };
            for(const auto &id:{guards[0]->getUnitID(),guards[1]->getUnitID(),recruits[0],recruits[1]})
                if(!verifyType(id))return gap("UNVERIFIED_ECONOMIC_UNIT_"+id);
            for(const auto &id:terrainTypes){
                const auto k=c.kindForGlobal(id);
                if(k.status!=ok || (k.kind!=K::Sea && k.kind!=K::Plains && k.kind!=K::Mountain && k.kind!=K::Forest && k.kind!=K::Street) ||
                   c.authenticateStockTerrainCallbacks(engine,k.kind).status!=ok)return gap("UNVERIFIED_ECONOMIC_TERRAIN_"+id);
            }
            for(const auto k:{K::MoveFeet,K::MoveTireA})
                if(c.authenticateStockMovementCallbacks(engine,k).status!=ok || c.authenticateStockMovementTables(engine,k).status!=ok)
                    return gap("UNVERIFIED_UNIT_MOVEMENT_TABLE");
            charging=nullptr;
        }
        std::array<std::unique_ptr<Unit>,2> infantry;
        std::array<std::vector<bool>,2> reachable{std::vector<bool>(w*h),std::vector<bool>(w*h)};
        for(int who=0;who<2;++who){
            auto *p=players[who];
            infantry[who]=std::make_unique<Unit>(recruits[who],p,false,&map);
            auto &u=*infantry[who];
            auto *port=map.getTerrain(ports[who].x(),ports[who].y())->getBuilding();
            if(u.getMovementType()!="MOVE_FEET" || guards[who]->getMovementType()!="MOVE_TIRE_A" ||
               u.getHp()!=10. || !u.canCapture() || u.getBaseMaxRange()!=1 || u.getLoadingPlace()!=0 ||
               u.getIgnoreUnitCollision() || !port->getConstructionList().contains(recruits[who]))return gap("UNVERIFIED_SINGLE_CAPTURER");
            std::deque<QPoint> pending;
            for(auto p0:{ports[who],guards[who]->getPosition()})if(!reachable[who][key(p0)]){
                reachable[who][key(p0)]=true;pending.push_back(p0);
            }
            while(!pending.empty()){
                const QPoint at=pending.front();pending.pop_front();
                for(auto delta:{QPoint{1,0},QPoint{-1,0},QPoint{0,1},QPoint{0,-1}}){
                    const auto next=at+delta;if(!map.onMap(next.x(),next.y()) || reachable[who][key(next)])continue;
                    if(u.getBaseMovementCosts(next.x(),next.y(),at.x(),at.y())<0 &&
                       guards[who]->getBaseMovementCosts(next.x(),next.y(),at.x(),at.y())<0)continue;
                    reachable[who][key(next)]=true;pending.push_back(next);
                }
            }
            for(int y=0;y<h;++y)for(int x=0;x<w;++x){
                const QPoint firing{x,y};
                if(reachable[who][key(firing)] && (guards[who]->getMaxRange(firing)>1 || u.getMaxRange(firing)>1))
                    return gap("RANGED_FIRE_ACROSS_PRIVATE_SECTORS");
            }
            for(auto t:neutral)if(reachable[who][key(t)]){
                if(town[who].x()>=0)return gap("COMPETING_PRIVATE_CAPTURE_FRONTIERS");town[who]=t;
            }
            if(town[who].x()<0 || (town[who]-ports[who]).manhattanLength()!=1 || u.getCaptureRate(town[who])!=10 ||
               u.getMovementCosts(town[who].x(),town[who].y(),ports[who].x(),ports[who].y())!=1 ||
               u.getBaseMovementPoints()<1 || u.getFuel()<1)return gap("EARLIEST_CAPTURE_NOT_WITNESSED");
        }
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            const QPoint at{x,y};
            if(auto *b=map.getTerrain(x,y)->getBuilding();b && b->getOwner()){
                const int other=b->getOwner()==self?1:0;
                if(reachable[other][key(at)])return gap("OPPOSING_OWNED_TITLE_REACHABLE");
            }
            if(!reachable[0][key(at)])continue;
            if(reachable[1][key(at)])return gap("SHARED_MOVEMENT_COMPONENT");
            for(auto d:{QPoint{1,0},QPoint{-1,0},QPoint{0,1},QPoint{0,-1}}){
                const auto next=at+d;
                if(map.onMap(next.x(),next.y()) && reachable[1][key(next)])return gap("CROSS_COMPONENT_FIRE");
            }
        }
        const auto xy=[](QPoint p){return QJsonArray{p.x(),p.y()};};
        const QPoint end(order.destination.x,order.destination.y);
        const QPoint start(order.path.front().x,order.path.front().y);
        if(start!=ports[0] || order.path.back()!=order.destination || !map.onMap(end.x(),end.y()) ||
           order.path.size()>static_cast<std::size_t>(std::max(0,guards[0]->getBaseMovementPoints()))+1)
            return gap("UNBOUND_OPENING_PATH");
        if(work<static_cast<std::int64_t>(8*order.path.size()))return gap("WORK_LIMIT");
        work-=static_cast<std::int64_t>(8*order.path.size());
        UnitPathFindingSystem f0(&map,guards[0],self);f0.explore();
        if(!f0.isReachable(end.x(),end.y()))return gap("UNREACHABLE_OPENING_ORDER");
        int spent=0;
        for(std::size_t i=1;i<order.path.size();++i){
            const auto a=order.path[i-1],b=order.path[i];
            if(!map.onMap(b.x,b.y) || std::abs(a.x-b.x)+std::abs(a.y-b.y)!=1)return gap("UNBOUND_OPENING_STEP");
            const int cost=f0.getCosts(f0.getIndex(b.x,b.y),b.x,b.y,a.x,a.y,spent);
            if(cost<=0 || cost>guards[0]->getFuel()-spent)return gap("UNAFFORDABLE_OPENING_STEP");
            spent+=cost;
        }
        if(spent>guards[0]->getMovementpoints(start) || spent+guards[0]->getMovementFuelCostModifier(spent)>guards[0]->getFuel())
            return gap("UNAFFORDABLE_OPENING_ORDER");
        auto wait=MemoryManagement::create<GameAction>(order.actionId,&map,0);wait->setTarget(start);std::vector<QPoint> walk;
        for(auto it=order.path.rbegin();it!=order.path.rend();++it)walk.emplace_back(it->x,it->y);
        wait->setMovepath(walk,spent);
        if(!wait->canBePerformed() || !wait->isFinalStep())return gap("NATIVE_OPENING_ORDER_REFUSED");
        const bool held=end==ports[0];
        const auto builds=buyer["modeledOpening"].toArray();
        const auto preview=buyer["buyerPreview"].toObject();const auto purchases=preview["orders"].toArray();
        const int price=self->getCosts(recruits[0],ports[0]);
        if(buyer["spendableFunds"].toInteger(-1)!=toMilliFunds(self->getFunds()) ||
           !buyer["modeledOpening"].isArray() || !buyer["unpricedExposedOffers"].isArray() ||
           !buyer["unpricedExposedOffers"].toArray().empty() || price<=0 ||
           (held && (buyer["openingSource"]!="NO_FUNDED_SLOT" || buyer["affordableBuildSlots"].toInt(-1)!=0 || !builds.empty())) ||
           (!held && (buyer["openingSource"]!="COUNTERPOINT_PREVIEW" || !preview["supported"].toBool() ||
                       purchases.size()!=1 || purchases[0].toObject()["type"]!=recruits[0] ||
                       purchases[0].toObject()["x"].toInt(-1)!=ports[0].x() || purchases[0].toObject()["y"].toInt(-1)!=ports[0].y() ||
                       purchases[0].toObject()["price"].toInteger(-1)!=price || preview["remainingFunds"].toInteger(-1)!=self->getFunds()-price ||
                       builds.size()!=1 || builds[0].toObject()["type"]!=recruits[0] ||
                       builds[0].toObject()["factory"].toArray()!=xy(ports[0]) ||
                       builds[0].toObject()["price"].toInteger()!=toMilliFunds(price))))
            return gap("UNBOUND_CONTROLLED_OPENING_PURCHASE");
        if(foe->getFunds()+std::int64_t(foe->calcIncome())<foe->getCosts(recruits[1],ports[1]) ||
           self->calcIncome()!=foe->calcIncome())return gap("UNBOUND_PRIVATE_BUYER_OR_BASE_INCOME");
        if(guards[1]->getPosition()==town[1])return gap("OPPONENT_EARLIEST_CAPTURE_NEEDS_VACATER");
        QJsonArray actions;actions.append(QStringLiteral("E0 buy the sole legal capper, E1/E2 capture the private town"));
        if(!held && end==town[0]){
            bool clear=false;
            for(auto d:{QPoint{1,0},QPoint{-1,0},QPoint{0,1},QPoint{0,-1}}){
                auto at=end+d;if(at==ports[0] || !map.onMap(at.x(),at.y()))continue;
                const int cost=guards[0]->getMovementCosts(at.x(),at.y(),end.x(),end.y());
                if(cost<=0 || cost>guards[0]->getBaseMovementPoints() || cost>guards[0]->getFuel()-spent)continue;
                actions.append(QString("F1 vacate (%1,%2) to (%3,%4) before the paid capture").arg(end.x()).arg(end.y()).arg(at.x()).arg(at.y()));
                clear=true;break;
            }
            if(!clear)return gap("NO_WITNESSED_GUARD_DEPARTURE");
        }
        actions.append(held?QString("Grant earliest F1 purchase and F2/F3 captures to bound all friendly departures"):
                             QString("F0 paid capper takes the protected town on F1/F2"));
        std::vector<NativePropertyPayday> paydays;
        for(int tick=1;tick<=2*horizon;++tick){
            const int who=tick%2;
            const auto *p=players[who];const auto t=town[who];
            const auto increment=tick>=(who?7:held?8:6)?map.getTerrain(t.x(),t.y())->getBuilding()->getBaseIncome():0;
            paydays.push_back({tick,bool(who),toMilliFunds(p->calcIncome()+increment)});
        }
        const std::vector<MilliFunds> zero(horizon);
        const auto value=nativeTrajectoryRank(horizon,paydays,TrajectoryHeadquarters{TerminalClass::Unresolved,0,0,-1},
            NonPropertyContinuation{opening,{},zero});
        if(!value)return gap("FORMULA_OVERFLOW");
        out.proof={{"status","PRIVATE_CAPTURES_NO_CROSS_COMPONENT_EFFECT"},{"direction",held?"UPPER":"LOWER"},
            {"money",static_cast<qint64>(std::get<3>(*value))},{"purchaseBoundary",buyer},{"actions",actions},
            {"ownTown",xy(town[0])},{"enemyTown",xy(town[1])},{"ownFirstPayday",held?8:6},{"enemyFirstPayday",7}};
        return out;
    }
}
