#pragma once

#include "ai/coordinator/factoryprogramnative.h"

namespace Coordinator::FactoryPrograms
{
    struct Proposal {std::vector<int> choices;int unit{},site{},slot{},option{};bool immediate{},due{};};
    inline std::vector<Proposal> programmeTeams(const MasterAssignmentProblem &problem,const LiveBoard &live,
        const std::vector<int> &incumbent,const std::vector<std::vector<int>> &seedTeams,
        const std::vector<std::vector<int>> &observedTeams,const QJsonArray &due,
        std::int64_t &work,bool &truncated)
    {
        const auto &b=live.board;std::vector<Proposal> out;
        std::set<std::tuple<int,int,std::vector<int>>> distinct;
        std::map<std::pair<const MobilityCostGrid*,int>,DistanceField> lower;
        for(int j=0;j<int(b.sites.size());++j){
            if(!b.sites[j].capturable || b.sites[j].headquarters || !b.economics.sites[j].production ||
               live.initial.owners[j]==0 || b.sites[j].menu[0].empty())continue;
            for(const auto &actor:problem.actors()){
                const int slot=actor.canonicalSlot;
                if(!problem.canChangeActor(slot))continue;
                const auto u=std::find_if(live.initial.bodies.begin(),live.initial.bodies.end(),[&](const Body &a){return a.id==actor.engineUnitId;});
                if(u==live.initial.bodies.end())continue;
                const auto &type=b.types[u->profile];
                if(type.owner || !type.canCapture || type.rate[j]<=0 || !type.grid)continue;
                const auto id=std::pair{type.grid,j};auto reach=lower.find(id);
                if(reach==lower.end()){
                    if(!debit(work,12LL*b.width*b.height)){truncated=true;return out;}
                    reach=lower.emplace(id,DistanceField::build(*type.grid,{b.sites[j].at},FieldExpansion::ToTargets)).first;
                }
                const int here=reach->second.distance(u->at.x,u->at.y);
                const bool promised=std::any_of(due.constBegin(),due.constEnd(),[&](const QJsonValue &r){
                    return r.toObject()["unit"].toInt(-1)==u->id && r.toObject()["factory"].toArray()==point(b.sites[j].at);});
                for(int c=0;c<int(actor.candidates.size());++c){
                    if(!debit(work,1)){truncated=true;return out;}
                    const auto &action=actor.candidates[c];
                    if(!action.plannable || action.origin!=u->at || action.source.movementCost<0 || action.source.movementCost>u->fuel)continue;
                    const bool direct=action.capture && action.destination==b.sites[j].at;
                    const int rest=reach->second.distance(action.destination.x,action.destination.y);
                    if(!direct && (action.actionKind!=PlanBundleKind::Move || rest<0 || here<=rest ||
                       rest>std::min(type.movement,u->fuel-action.source.movementCost)))continue;
                    for(const auto &seed:seedTeams){
                        if(!debit(work,1+seed.size())){truncated=true;return out;}
                        if(seed.size()!=incumbent.size())continue;
                        auto choices=seed;choices[slot]=c;
                        if(!distinct.emplace(u->id,j,choices).second)continue;
                        out.push_back({std::move(choices),u->id,j,slot,c,direct,promised});
                    }
                    for(const auto &team:observedTeams){
                        if(!debit(work,1)){truncated=true;return out;}
                        if(team.size()!=incumbent.size() || team[slot]!=c)continue;
                        if(!debit(work,team.size())){truncated=true;return out;}
                        if(distinct.emplace(u->id,j,team).second)out.push_back({team,u->id,j,slot,c,direct,promised});
                    }
                }
            }
        }
        std::stable_sort(out.begin(),out.end(),[&](const auto &a,const auto &c){
            const auto cost=[&](const Proposal &p){int n=0;for(int i=0;i<int(incumbent.size());++i)n+=incumbent[i]!=p.choices[i];
                return std::tuple{!p.due,!p.immediate,n,p.site,p.unit,p.option};};
            return cost(a)<cost(c);
        });
        return out;
    }
    inline std::vector<Strategy> competingStrategies(const LiveBoard &terrain,const Opening &opening,int site)
    {
        std::vector<Strategy> options{{-1,-1,-1}};
        for(const auto &body:opening.afterOrders.bodies){
            const auto &kind=terrain.board.types[body.profile];
            if(!kind.owner && kind.canCapture && kind.rate[site]>0)options.push_back({body.id,-1,site});
        }
        // The live F0 buyer can supply the founder on the other programme.
        // This route remains eligible even if none of the original actors
        // is committed to the factory on that team's first activation.
        for(const auto &v:opening.firstBuyer["orders"].toArray()){
            const auto purchase=v.toObject();auto found=terrain.board.siteIds.find({purchase["x"].toInt(-1),purchase["y"].toInt(-1)});
            if(found==terrain.board.siteIds.end())continue;
            const auto &menu=terrain.board.sites[found->second].menu[0];auto type=menu.find(purchase["type"].toString());
            if(type!=menu.end() && terrain.board.types[type->second.first].canCapture)
                options.push_back({-1,found->second,site});
        }
        return options;
    }
    inline FactoryEconomy::Arm armIdentity(const Board &b,const Opening &start)
    {
        FactoryEconomy::Arm a;a.root=b.economics.root;a.procedure=b.economics.procedure;
        a.opening=start.identity.toStdString();return a;
    }
    inline QJsonArray economicEvents(const QJsonArray &phases)
    {
        QJsonArray events;
        for(const auto &v:phases){
            const auto p=v.toObject();
            if(!p["purchases"].toArray().isEmpty() || !p["completedTitles"].toArray().isEmpty())
                events.append(QJsonObject{{"tick",p["tick"]},{"purchases",p["purchases"]},{"completedTitles",p["completedTitles"]},
                    {"openingWallet",p["startWallet"]},{"closingWallet",p["endWallet"]}});
        }
        return events;
    }
    struct Hearing
    {
        std::vector<int> choices;
        QJsonObject receipt;
        QJsonArray next;
        bool selected{};
        bool retained{};
        std::vector<int> referenceChoices;
    };
    template<class Open,class Buyer,class Safety>
    Hearing hearFactoryPrograms(const MasterAssignmentProblem &problem,const LiveBoard &live,
        const std::vector<int> &incumbent,const std::vector<std::vector<int>> &seeds,
        const std::vector<std::vector<int>> &observedTeams,const QJsonArray &due,
        Open &&open,Buyer &&buyer,Safety &&safety,std::int64_t &work,const Proposal *single=nullptr)
    {
        Hearing result{incumbent,{{"status","OPEN"}},{},false};
        result.referenceChoices=incumbent;
        if(!live.reason.isEmpty()){result.receipt["reason"]=live.reason;return result;}
        const auto &b=live.board;
        std::map<std::vector<int>,std::pair<std::optional<Opening>,QString>> openings;
        const auto start=[&](const std::vector<int> &v)->const auto &{
            auto [it,fresh]=openings.try_emplace(v);
            if(fresh)it->second.first=open(v,it->second.second);
            return it->second;
        };
        bool interrupted=false;
        auto offers=single?std::vector<Proposal>{*single}:programmeTeams(problem,live,incumbent,seeds,observedTeams,due,work,interrupted);
        result.receipt["teamOffers"]=int(offers.size());result.receipt["generationInterrupted"]=interrupted;
        if(offers.empty()){result.receipt["reason"]="NO_LEGAL_FACTORY_ACQUISITION_OR_APPROACH";return result;}
        const auto &anchor=start(incumbent);
        if(!anchor.first){result.receipt["reason"]=anchor.second;return result;}
        const QString policy="matched-ordinary-economic-programme-v1";
        std::map<int,FactoryEconomy::Arm> alternatives;
        std::map<int,std::vector<QJsonArray>> alternativePhases;
        QJsonArray referenceEvidence;
        QJsonArray observations;MilliFunds best=0;
        std::vector<Proposal> retained;
        for(const auto &offer:offers){
            if(offer.choices==incumbent){retained.push_back(offer);continue;}
            if(work<=0){result.receipt["hearingInterrupted"]=true;break;}
            const auto &other=start(offer.choices);
            QJsonObject heard{{"unit",offer.unit},{"factory",point(b.sites[offer.site].at)},
                {"candidate",offer.option},{"immediate",offer.immediate},{"matured",offer.due}};
            if(!other.first){heard["reason"]=other.second;observations.append(heard);continue;}
            auto [existing,fresh]=alternatives.try_emplace(offer.site,armIdentity(b,*anchor.first));
            if(fresh){
                for(const auto &s:competingStrategies(live,*anchor.first,offer.site)){
                    auto run=simulate(b,*anchor.first,s,policy,buyer,work);
                    referenceEvidence.append(QJsonObject{{"factory",point(b.sites[offer.site].at)},{"founder",s.actor},
                        {"birthSite",s.birthSite},{"strategy",QString::fromStdString(run.program.strategy)},
                        {"completedPhases",int(run.program.phases.size())},{"firstGapTick",run.program.gap.empty()?-1:int(run.program.phases.size())},
                        {"gap",QString::fromStdString(run.program.gap)},{"economicEvents",economicEvents(run.evidence)},
                        {"lastCompletedPhase",run.evidence.isEmpty()?QJsonValue{}:run.evidence.last()}});
                    alternativePhases[offer.site].push_back(run.evidence);
                    existing->second.policies[policy.toStdString()].push_back(std::move(run.program));
                }
            }
            auto candidate=armIdentity(b,*other.first);
            auto run=simulate(b,*other.first,Strategy{offer.unit,-1,offer.site},policy,buyer,work);
            const auto &observed=*other.first;
            heard["candidateStrategy"]=QString::fromStdString(run.program.strategy);
            heard["candidateCompletedPhases"]=int(run.program.phases.size());
            heard["candidateEconomicEvents"]=economicEvents(run.evidence);
            if(!run.program.gap.empty()){
                heard["candidateFirstGapTick"]=int(run.program.phases.size());
                heard["candidateGap"]=QString::fromStdString(run.program.gap);
            }
            candidate.policies[policy.toStdString()].push_back(std::move(run.program));
            auto decision=FactoryEconomy::compareFactoryPrograms(b.economics,existing->second,candidate,offer.site,work);
            for(const auto &rival:alternativePhases[offer.site]){
                for(int t=1;t<std::min(rival.size(),run.evidence.size());++t){
                    const auto old=rival[t].toObject(),now=run.evidence[t].toObject();
                    const bool newFire=old["unpricedFire"]!=now["unpricedFire"];
                    const bool newBuyer=b.opposingSearchLimited && t%2 && old["opposingPolicyInputs"]!=now["opposingPolicyInputs"];
                    if(!newFire && !newBuyer)continue;
                    const auto key=newFire?"unpricedFire":"opposingPolicyInputs";
                    heard["firstDifferential"]=QJsonObject{{"tick",t},{"component",key},{"reference",old[key]},{"candidate",now[key]}};
                    decision.status=FactoryEconomy::Status::Open;
                    decision.reason=newFire?"UNPRICED_CHANGED_FIRE_OR_INTERRUPTION":"OPPOSING_PURCHASE_OPPORTUNITIES_DIFFER";
                    break;
                }
                if(heard.contains("firstDifferential"))break;
            }
            heard["status"]=decision.status==FactoryEconomy::Status::StrictEconomicGain?"STRICT_ECONOMIC_GAIN":
                decision.status==FactoryEconomy::Status::NoNetGain?"NO_NET_GAIN":"OPEN";
            heard["reason"]=QString::fromStdString(decision.reason);heard["referenceValue"]=qint64(decision.referenceValue);
            heard["candidateValue"]=qint64(decision.challengerValue);heard["margin"]=qint64(decision.marginal);
            heard["firstPurchaseTick"]=decision.buyTick;heard["productiveUseTick"]=decision.useTick;
            if(decision.status==FactoryEconomy::Status::StrictEconomicGain && decision.marginal>best){
                auto safe=safety(incumbent,offer,*anchor.first,observed);
                heard["safety"]=safe;
                if(safe["status"]=="NO_IMMEDIATE_DANGER"){
                    best=decision.marginal;result.choices=offer.choices;result.selected=true;
                    result.receipt["preference"]=heard;result.receipt["datedProgram"]=run.evidence;
                    result.receipt["referenceOpening"]=anchor.first->physicalTeam;
                    result.receipt["candidateOpening"]=observed.physicalTeam;
                    result.receipt["sourceBuyer"]=anchor.first->firstBuyer;
                    result.receipt["selectedBuyer"]=observed.firstBuyer;
                    result.next={QJsonObject{{"unit",offer.unit},{"factory",point(b.sites[offer.site].at)},
                        {"day",b.day+1},{"from",point(problem.actors()[offer.slot].candidates[offer.option].destination)}}};
                }
            }
            observations.append(heard);
        }
        result.receipt["referencePrograms"]=referenceEvidence;
        result.receipt["outcomes"]=observations;result.receipt["status"]=result.selected?"ECONOMIC_PREFERENCE":"OPEN";
        // The ordinary assignment may already make the first movement of the
        // earlier factory programme. Compare that real team against the legal
        // rival teams that begin a different paying capture now. Never call an
        // identical staging tile evidence of a strict current-order change.
        if(!single && !result.selected){
            QJsonArray retainedAttempts;
            for(const auto &existing:retained){
                const auto &actor=problem.actors()[existing.slot];
                std::set<std::vector<int>> rivals;
                for(int k=0;k<int(actor.candidates.size());++k){
                    const auto &other=actor.candidates[k];
                    if(!other.plannable || !other.capture || !within(b,other.destination))continue;
                    const int j=b.cellSite[cell(b,other.destination)];
                    if(j<0 || j==existing.site || b.economics.sites[j].production || b.sites[j].headquarters ||
                       b.economics.sites[j].income[0]<=0)continue;
                    for(const auto &seed:seeds)if(seed.size()==incumbent.size()){
                        auto team=seed;team[existing.slot]=k;if(team!=incumbent)rivals.insert(std::move(team));
                    }
                    for(const auto &team:observedTeams)if(team.size()==incumbent.size() &&
                        team[existing.slot]==k && team!=incumbent)rivals.insert(team);
                }
                if(rivals.empty())continue;
                bool all=true;std::optional<Hearing> weakest;QJsonArray peers;
                for(const auto &rival:rivals){
                    auto heard=hearFactoryPrograms(problem,live,rival,seeds,observedTeams,due,open,buyer,safety,work,&existing);
                    peers.append(heard.receipt);
                    if(!heard.selected){all=false;continue;}
                    if(!weakest || heard.receipt["preference"].toObject()["margin"].toInteger()<
                        weakest->receipt["preference"].toObject()["margin"].toInteger())weakest=std::move(heard);
                }
                retainedAttempts.append(QJsonObject{{"unit",existing.unit},{"factory",point(b.sites[existing.site].at)},{"competingTeams",peers}});
                if(all && weakest){
                    const auto prior=result.receipt;result=std::move(*weakest);
                    result.retained=true;result.receipt["status"]="RETAINED_ECONOMIC_PREFERENCE";
                    result.receipt["currentOrderChanged"]=false;result.receipt["otherFactoryChallenges"]=prior;
                    break;
                }
            }
            if(!retainedAttempts.isEmpty())result.receipt["retainedDirections"]=retainedAttempts;
        }
        return result;
    }
}
