#pragma once

#include "ai/coordinator/openinglaneservice.h"
#include "ai/coordinator/openinglanebound.h"
#include "ai/coordinator/orderedopeningcomparison.h"
#include "ai/coordinator/openinglanebindings.h"
#include "ai/coordinator/openingrecruitadmission.h"

namespace Coordinator
{
    struct OpeningLaneRulesMemo
    {
        struct Row{bool supported{};QString reason;std::int64_t credits{};};
        std::map<std::vector<QString>,Row> prior;
        struct Work
        {
            qint64 cold{},lookup{},savings{},refusals{},validated{};
            QJsonObject since(const Work &before) const {
                return {{"cold",cold-before.cold},{"lookup",lookup-before.lookup},
                        {"previouslyVerifiedWork",savings-before.savings},
                        {"workRefusals",refusals-before.refusals},{"validated",validated-before.validated}};
            }
        };
        Work openingWork,friendlyWork;
        std::optional<HqBindingDetail::StockMethods> stockModules;
        const QJSEngine *observedEngine{};
        const GameMap *observedMap{};
        const BattlefieldKnowledge *observedKnowledge{};
        // Matches the portfolio inputs read by ordinaryOpeningServiceBindings.
        // This memo belongs to one synchronous comparison on one observed map.
        // It validates script methods only. Each arm still applies its actual
        // units, squares, wallet, purchases and day-start transitions independently.
        static std::vector<QString> methodKey(const QJsonArray &portfolio,bool friendlyF1=false){
            return openingServicePortfolio(portfolio,friendlyF1).methodKey();
        }
        bool verify(GameMap &map,const BattlefieldKnowledge &knowledge,const QJsonArray &portfolio,std::int64_t &work,QString &why,bool friendlyF1=false)
        {
            auto &engine=*Interpreter::getInstance();
            if(observedEngine!=&engine || observedMap!=&map || observedKnowledge!=&knowledge){
                prior.clear();
                observedEngine=&engine;observedMap=&map;observedKnowledge=&knowledge;
                stockModules.emplace(QCoreApplication::applicationDirPath()+"/resources/scripts",true);
            }
            auto &meter=friendlyF1?friendlyWork:openingWork;
            const auto key=methodKey(portfolio,friendlyF1);
            if(auto p=prior.find(key);p!=prior.end()){
                const auto charge=static_cast<std::int64_t>(portfolio.size()+key.size()+1);
                if(work<charge){++meter.refusals;why="SERVICE_METHOD_WORK_LIMIT/CACHED_CONTRACT";return false;}
                work-=charge;meter.lookup+=charge;
                meter.savings+=std::max<qint64>(0,p->second.credits-charge);
                why=p->second.reason;++meter.validated;return p->second.supported;
            }
            // Only completed modules in the detached stock reference survive a
            // refused check. Each new portfolio still resolves every required
            // live hook and compares its guarded image with the reference.
            const auto started=work;
            bool workRefused=false;
            const bool ok=ordinaryOpeningServiceBindings(engine,map,knowledge,portfolio,*stockModules,
                                                        work,why,friendlyF1,&workRefused);
            meter.cold+=started-work;
            if(ok){
                prior.emplace(key,Row{true,why,started-work});
                ++meter.validated;
            }else if(workRefused){
                ++meter.refusals;
                why=QStringLiteral("SERVICE_METHOD_WORK_LIMIT/")+why;
            }
            return ok;
        }
    };
    inline QJsonArray openingLaneSteps(std::span<const OpeningLaneAction> actions,const CombatTerrain &t)
    {
        const auto xy=[](TilePoint p){return QJsonArray{p.x,p.y};};QJsonArray result;
        for(const auto &a:actions)result.append(QJsonObject{{"unit",t.units[a.shot.shooter].engineUnitId},{"target",t.units[a.shot.target].engineUnitId},
            {"origin",xy(a.origin)},{"firing",xy(a.shot.from)},{"targetTile",xy(a.targetTile)},{"fuel",a.moveCost},{"hit",a.native.hit},{"counter",a.native.counter},
            {"weapon",a.native.weapon},{"counterWeapon",a.native.counterWeapon},{"actorHpBefore",a.shooterBefore/10.},{"actorHpAfter",a.shooterAfter/10.},
            {"targetHpBefore",a.targetBefore/10.},{"targetHpAfter",a.targetAfter/10.}});return result;
    }
    template<class Evaluate,class Price,class Rules>
    QJsonObject priceFoundOpeningLane(GameMap &map,const BattlefieldKnowledge &knowledge,const MasterAssignmentProblem &problem,
        const PropertyStockField &field,CombatTerrain &terrain,const ReplyPassEvaluation &team,const QJsonArray &portfolio,
        const OpeningRecruitTarget &target,OpeningLaneRulesMemo &rules,std::int64_t &work,Evaluate evaluate,Price price,Rules verifiedRules)
    {
        const auto originalPolicy=terrain.orderedOpening;const auto originalTarget=terrain.openingRecruitTarget;
        struct Restore{CombatTerrain &t;const OrderedOpeningPolicy *p;const OpeningRecruitTarget *g;
            ~Restore(){t.orderedOpening=p;t.openingRecruitTarget=g;}}restore{terrain,originalPolicy,originalTarget};
        QJsonObject receipt{{"status","UNRESOLVED"},{"enemyShooter",terrain.units[target.shooter].engineUnitId},{"factory",QJsonArray{target.factory.x,target.factory.y}},
            {"residualReplyMenuComplete",false}};const auto before=work;QString reason,bindingReason;
        const auto snapshot=openingBuyerProjection(map,knowledge,terrain,problem,field,team,0,0);
        const auto current=openingLaneAfterTeam(map,knowledge,problem,terrain,team,snapshot,portfolio,
            [&](GameMap &m,const BattlefieldKnowledge &k,std::int64_t &b){return rules.verify(m,k,portfolio,b,bindingReason);},work,&reason);
        if(!current){receipt["reason"]=reason;if(!bindingReason.isEmpty())receipt["bindingFailure"]=bindingReason;receipt["work"]=before-work;return receipt;}
        QJsonArray paid;for(const auto &r:current->bill.rows)if(r.debit||r.before!=r.after)paid.append(QJsonObject{{"unit",terrain.units[r.actor].engineUnitId},
            {"before",r.before/10.},{"after",r.after/10.},{"cost",static_cast<qint64>(r.debit)}});
        receipt["sharedOpeningRepairs"]=QJsonObject{{"before",static_cast<qint64>(current->bill.before)},{"after",static_cast<qint64>(current->bill.after)},
            {"rows",paid},{"materialLessCash",static_cast<qint64>(current->materialLessCash)}};
        auto candidate=proposeOpeningLane(terrain,knowledge.units(),current->after,target,work);
        if(!candidate){
            const auto rules=verifiedRules(work);
            const auto bounds=boundOpeningRecruitLane(terrain,knowledge.units(),current->after,target,rules.get(),work);
            receipt["clearanceBound"]=bounds;
            if(rules)receipt["clearanceRuleProfile"]=rules->reason;
            receipt["namedFiringLaneImpossible"]=bounds["complete"].toBool();
            if(!bounds["complete"].toBool() && work>0)
                candidate=proposeOpeningLane(terrain,knowledge.units(),current->after,target,work,true);
            if(!candidate){
                receipt["reason"]=bounds["complete"].toBool()?bounds["reason"]:QJsonValue("NO_CERTIFIED_ORDERED_PREFIX_WITHIN_SEARCH");
                receipt["work"]=before-work;return receipt;
            }
            receipt["nestedTransitClearance"]=true;
        }
        receipt["prefix"]=openingLaneSteps(candidate->search.witness,terrain);receipt["guard"]=terrain.units[candidate->guard].engineUnitId;
        receipt["firing"]=QJsonArray{candidate->firing.x,candidate->firing.y};receipt["states"]=candidate->search.visited;receipt["offers"]=candidate->search.proposals;
        receipt["boundedSearch"]=candidate->search.truncated;
        OrderedOpeningPolicy common{current->after.afterService,target.shooter,current->bill,{},openingLaneNativeExchange(*terrain.production),current->materialLessCash};
        auto charged=common;charged.prefix=candidate->search.witness;
        auto shoot=target;shoot.afterStockFires=static_cast<int>(charged.prefix.size());shoot.from=candidate->firing;shoot.beforeBlockOrders=false;
        std::array<std::optional<ReplyPassEvaluation>,3> measured;
        QJsonArray arms;
        for(int arm=0;arm<3;++arm){terrain.orderedOpening=arm?&charged:&common;terrain.openingRecruitTarget=arm==2?&shoot:nullptr;
            auto v=evaluate();QJsonObject row{{"policy",arm==0?"ORDINARY":arm==1?"CLEAR_AND_DECLINE":"CLEAR_AND_SHOOT"}};
            if(!v.valued() || (arm==2&&!v.openingRecruitFight)){row["status"]="PHYSICAL_REPLAY_UNRESOLVED";arms.append(row);continue;}
            row["status"]="PHYSICAL_REPLAY_VERIFIED";row["stockShotCount"]=static_cast<qint64>(v.shots.size());
            if(v.openingRecruitFight)row["newbornShot"]=openingRecruitRecord(*v.openingRecruitFight,terrain.units[target.shooter].engineUnitId);
            const auto reserve=v.combatLocations[target.shooter];row["reserveAfter"]=QJsonArray{reserve.x,reserve.y};measured[arm]=std::move(v);arms.append(row);
        }
        using Priced=std::invoke_result_t<Price,int,const ReplyPassEvaluation&,const OrderedOpeningPolicy&,QJsonObject&>;
        using Rank=typename Priced::value_type;
        const auto compared=compareOrderedOpening<Rank>(current->bill,charged.prefix,{false,{}},
            [&](OrderedOpeningChoice mode,std::span<const OpeningLaneAction>,const OpeningRepairBill &)->Priced{
                const int arm=static_cast<int>(mode);if(!measured[arm])return {};
                terrain.orderedOpening=arm?&charged:&common;terrain.openingRecruitTarget=nullptr;
                auto row=arms[arm].toObject();Priced rank;
                rank=price(arm,*measured[arm],charged,row);
                arms[arm]=row;return rank;
            });
        if(compared.lowestKnown){const auto &v=*compared.lowestKnown;receipt["lowestKnownConditional"]=QJsonArray{int(std::get<0>(v)),std::get<1>(v),std::get<2>(v),static_cast<qint64>(std::get<3>(v))};}
        receipt["alternatives"]=arms;receipt["normalContactVerified"]=measured[2].has_value();
        receipt["reason"]=measured[2].has_value()?"ORDERED_CONTACT_VERIFIED_RESIDUAL_RESPONSES_OPEN":"ORDERED_PREFIX_NOT_REPLAYED";receipt["work"]=before-work;return receipt;
    }
}
