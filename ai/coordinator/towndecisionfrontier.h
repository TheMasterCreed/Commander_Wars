#pragma once

#include "ai/coordinator/towncomparisonevidence.h"
#include "ai/coordinator/ownershipschedule.h"
#include <cmath>
#include <limits>
#include <tuple>

namespace Coordinator
{
    // This is a work-ordering measurement. Removing one applied change is
    // deliberately not represented as a physically available alternative.
    struct TownDecisionFrontier
    {
        QString policy;
        TownIntention intention;
        qint64 appliedChange{},sensitivity{};
        int sourceArm{};
        std::set<QString> policies;
        qint64 sourceSubtotal{},peerSubtotal{};
        QJsonObject peerInput;
        QString stage{"APPLIED_MARGINAL"};qint64 titleOnly{};QJsonObject route;
        bool quotedPoliciesOnly{};
        bool property() const {return stage=="PROPERTY_ADMISSION";}
        QJsonObject record() const
        {
            auto row=describeTownIntention(intention);
            row["replyPolicy"]=policy;row["sourceArm"]=sourceArm;row["appliedChange"]=appliedChange;
            row["provisionalMinimumSensitivity"]=sensitivity;row["use"]="SEARCH_PRIORITY_ONLY";
            row["stage"]=stage;
            if(quotedPoliciesOnly)
            {
                row["sensitivityScope"]="QUOTED_OPPONENT_POLICIES";
                row["otherOpeningReplies"]="UNRESOLVED";
                if(stage=="APPLIED_MARGINAL")row["quotedPolicyMinimumSensitivity"]=sensitivity;
                row.remove("provisionalMinimumSensitivity");
            }
            if(stage!="APPLIED_MARGINAL")
            {
                row.remove("appliedChange");row.remove("provisionalMinimumSensitivity");
                row["titleOnlyExposure"]=titleOnly;row["otherEffects"]="UNRESOLVED";
                if(property())row["provisionalTitleOnlySwing"]=sensitivity;
                if(!route.isEmpty())row["routeWitness"]=route;
            }
            row["sourceProvisionalScore"]=sourceSubtotal;row["peerProvisionalScore"]=peerSubtotal;row["peerInput"]=peerInput;
            QJsonArray aliases;for(const auto &p:policies)aliases.append(p);row["matchingE0Policies"]=aliases;return row;
        }
    };

    inline QJsonObject townDecisionFacts(const QJsonArray &encounters,const QJsonArray &coverage)
    {
        QJsonArray applied,named,broad;bool townChanged=false;
        for(const auto &v:encounters)
        {
            const auto row=v.toObject();
            if(!row["appliedToSchedule"].toBool())continue;
            townChanged=true;
            if(!row.contains("appliedMarginal"))continue;
            const auto line=row["appliedIntention"].toObject();
            if(!readTownIntention(line))continue;
            auto item=line;item["change"]=row["appliedMarginal"];applied.append(item);
        }
        for(const auto &v:coverage)
        {
            const auto row=v.toObject();
            if(row["capperUnit"].toInt(-1)<0 || row["requiredEntryTick"].toInt(-1)<0)
            {
                if(row.contains("decisionTitleSchedule"))
                {
                    QJsonObject slot{{"tile",row["tile"]},{"attackingSide",row["attackingSide"]},{"complete",row["complete"]},
                        {"status",row["status"]},{"reason",row["reason"]},{"firstPossible",row["firstPossible"]},
                        {"schedule",row["decisionTitleSchedule"]},{"result",row["signedChange"]}};
                    if(townChanged)slot["retainedCalendarChangedByTown"]=true;
                    if(row.contains("decisionPriorityRequested"))
                    {
                        slot["requestedWork"]=row["decisionPriorityRequested"];slot["initialOffer"]=row["decisionPriorityOffer"];
                        slot["usedWork"]=row["usedWork"];slot["hearing"]=row["decisionPriorityHearing"];slot["admission"]=row["decisionAdmission"];
                    }
                    broad.append(slot);
                }
                continue;
            }
            QJsonObject key{{"tile",row["tile"]},{"attackingSide",row["attackingSide"]},{"capper",row["capperUnit"]},
                {"entryTick",row["requiredEntryTick"]},{"status",row["status"]},{"reason",row["reason"]}};
            const auto status=row["status"].toString();
            const bool exhausted=row["exhaustedActorIds"].toArray().contains(row["capperUnit"]);
            key["answered"]=row["required"].toBool() && row["complete"].toBool() && row["reason"]!="AFTER_CUTOFF" &&
                (status=="BASELINE" || status=="UNAVAILABLE" || (status=="PRICED" && exhausted));
            if(row.contains("decisionPriorityRequested"))
            {
                key["requestedWork"]=row["decisionPriorityRequested"];key["initialOffer"]=row["decisionPriorityOffer"];
                key["usedWork"]=row["usedWork"];key["hearing"]=row["decisionPriorityHearing"];
            }
            if(row.contains("conditionalStockCasualties"))key["conditionalStockCasualties"]=row.value("conditionalStockCasualties");
            if(row.contains("stockTownInteractions"))key["stockTownInteractions"]=row.value("stockTownInteractions");
            named.append(key);
        }
        return {{"applied",applied},{"named",named},{"broad",broad}};
    }

    // Upper magnitude for the ownership component only, granting instant
    // completion at this menu's earliest admitted date. Integer timing in the
    // normal ledger can differ by one milli-fund near zero. Round upward.
    inline qint64 frontierTitleExposure(const QJsonObject &row)
    {
        const auto schedule=row["schedule"].toObject();const int side=row["attackingSide"].toInt(-1);
        const int n=schedule["rounds"].toInt(),end=schedule["halfTurnCutoff"].toInt(),first=row["firstPossible"].toInt(-1);
        if(side<0 || side>1 || n<1 || n>64 || end<1 || end>2*n || first<0 || first>end || schedule.isEmpty())return 0;
        const int owner=schedule["owner"].toInt();
        const auto neutral=static_cast<int>(OwnerSign::Neutral),ours=static_cast<int>(OwnerSign::Ours),enemy=static_cast<int>(OwnerSign::Enemy);
        if((owner!=neutral && owner!=ours && owner!=enemy) || owner==(side?enemy:ours))return 0;
        qint64 numerator=0;
        for(int t=first+1;t<=end;++t)
        {
            const auto payer=t%2;
            if(payer!=side && owner==neutral)continue;
            const auto amount=schedule[payer?"enemy":"ours"].toInteger(-1);const int weight=n-(t-1)/2;
            if(amount<0 || amount>(std::numeric_limits<qint64>::max()-numerator)/weight)return 0;
            numerator+=amount*weight;
        }
        // The ceiling is used only to order questions. Any nonzero remainder
        // in the pre-existing signed cash flow can alter truncation by 1.
        return numerator/n+int(numerator>0 && numerator/n<std::numeric_limits<qint64>::max());
    }

    enum class TownPriorityScope { CompleteMenu, ObservedPolicies };

    inline std::optional<TownDecisionFrontier> nominateTownFrontier(const QJsonObject &left,const QJsonObject &right,std::int64_t *work=nullptr,
        TownPriorityScope scope=TownPriorityScope::CompleteMenu)
    {
        const auto charge=[&](std::int64_t cost){if(!work)return true;if(*work<cost)return false;*work-=cost;return true;};
        struct Policy {qint64 score;QString physical;std::map<TownIntention,qint64> applied;std::map<TownIntention,QJsonObject> named;std::set<TownIntention> stock;std::map<TownSlot,QJsonObject> broad;
            bool answered(const TownIntention &i) const {const auto n=named.find(i);return stock.contains(i) || (n!=named.end() && n->second["answered"].toBool());}};
        std::array<std::map<QString,Policy>,2> plans;
        std::optional<std::pair<int,int>> commonHeadquartersRank;
        int arm=0;
        for(const auto *team:{&left,&right})
        {
            if(team->contains("boundedByReply") ||
               (team->contains("unpricedReplyPolicy") && scope==TownPriorityScope::CompleteMenu))return {};
            for(const auto &v:team->value("replyAlternatives").toArray())
            {
                if(!charge(1))return {};
                const auto row=v.toObject();const auto name=row["replyPolicy"].toString();
                const auto value=row.contains("policyTotal")?row["policyTotal"]:row["partialPolicyTotal"];
                if(name.isEmpty() || !value.isDouble() || !row.contains("terminalClass") || row["terminalClass"].toInt()!=0 || !row.contains("townDecisionFacts"))return {};
                if(scope==TownPriorityScope::ObservedPolicies)
                {
                    const auto security=row.value("hqSecurity"),threat=row.value("conditionalThreatRank");
                    if(!security.isDouble() || !threat.isDouble() ||
                       security.toInt(0)!=security.toInt(1) || threat.toInt(0)!=threat.toInt(1))return {};
                    const std::pair rank{security.toInt(),threat.toInt()};
                    if(commonHeadquartersRank && rank!=*commonHeadquartersRank)return {};
                    commonHeadquartersRank=rank;
                }
                auto [entry,added]=plans[arm].try_emplace(name,Policy{value.toInteger(),row["physicalE0Class"].toString(name),{},{},stockWitnesses(row),{}});
                if(!added)return {};
                const auto facts=row["townDecisionFacts"].toObject();
                if(!charge(facts["applied"].toArray().size()+facts["named"].toArray().size()+facts["broad"].toArray().size()))return {};
                for(const auto &v:facts["broad"].toArray())
                {
                    const auto o=v.toObject();const auto xy=o["tile"].toArray();
                    if(xy.size()==2)entry->second.broad[{xy[0].toInt(),xy[1].toInt(),o["attackingSide"].toInt()}]=o;
                }
                for(const auto &a:facts["applied"].toArray())
                {
                    const auto o=a.toObject();const auto i=readTownIntention(o);
                    if(i && o["change"].isDouble())entry->second.applied.emplace(*i,o["change"].toInteger());
                }
                for(const auto &a:facts["named"].toArray())
                    if(const auto o=a.toObject();auto i=readTownIntention(o))entry->second.named[*i]=o;
            }
            ++arm;
        }
        if(plans[0].empty() || plans[1].empty())return {};
        // Priority hearings compare common actions. Every other legal reply
        // stays in its own team's minimum when measuring their sensitivity.
        // The signed gap can move when an applied amount disappears on the
        // source arm or appears on its peer. Either arm's minimum may instead
        // switch to a different opponent response.
        const auto shift=[](const auto &plan,const std::set<QString> &equivalent,qint64 change){
            std::optional<qint64> affected,unaffected;
            for(const auto &[name,p]:plan)
            {
                auto &minimum=equivalent.contains(name)?affected:unaffected;
                if(!minimum || p.score<*minimum)minimum=p.score;
            }
            if(!affected)return qint64{0};
            const auto amount=std::abs(change);
            if(!unaffected)return amount;
            // An ordered difference of signed scores can span the entire unsigned
            // range. Only the portion within the proposed change affects the minimum.
            if(change>0 && *affected>=*unaffected)return qint64{0};
            if(change<0 && *affected<=*unaffected)return amount;
            const auto gap=quint64(std::max(*affected,*unaffected))-quint64(std::min(*affected,*unaffected));
            const auto overlap=static_cast<qint64>(std::min(quint64(amount),gap));
            return change>0?overlap:amount-overlap;
        };
        std::optional<TownDecisionFrontier> best;
        for(int source=0;source<2;++source)for(const auto &[name,p]:plans[source])
            for(const auto &[i,delta]:p.applied)
            {
                if(!plans[1-source].contains(name))continue;
                const auto &peer=plans[1-source].at(name);
                if(!delta || delta==std::numeric_limits<qint64>::min() || peer.applied.contains(i) || peer.answered(i))continue;
                if(!charge(3*static_cast<std::int64_t>(plans[source].size())))return {};
                std::set<QString> equivalent;
                for(const auto &[alias,record]:plans[source])
                {
                    if(!plans[1-source].contains(alias))continue;
                    const auto &other=plans[1-source].at(alias);const auto a=record.applied.find(i);
                    if(record.physical==p.physical && other.physical==peer.physical && a!=record.applied.end() &&
                       a->second==delta && !other.applied.contains(i) && !other.answered(i))equivalent.insert(alias);
                }
                const auto gain=std::max(shift(plans[source],equivalent,-delta),shift(plans[1-source],equivalent,delta));
                if(!gain)continue;
                const auto order=std::tuple{gain,std::abs(delta)};
                if(!best || order>std::tuple{best->sensitivity,std::abs(best->appliedChange)} ||
                   (order==std::tuple{best->sensitivity,std::abs(best->appliedChange)} && std::pair{name,i}<std::pair{best->policy,best->intention}))
                    best=TownDecisionFrontier{name,i,delta,gain,source,std::move(equivalent),p.score,peer.score,
                        peer.named.contains(i)?peer.named.at(i):QJsonObject{{"status","MISSING_NAMED_ROW"}}};
            }
        const auto finish=[&]{
            if(best)best->quotedPoliciesOnly=left.contains("unpricedReplyPolicy") || right.contains("unpricedReplyPolicy");
            return best;
        };
        if(best)return finish();
        std::array<qint64,2> worst{std::numeric_limits<qint64>::max(),std::numeric_limits<qint64>::max()};
        for(int k=0;k<2;++k)for(const auto &[name,p]:plans[k])worst[k]=std::min(worst[k],p.score);
        for(int source=0;source<2;++source)for(const auto &[name,p]:plans[source])
        {
            if(!plans[1-source].contains(name))continue;
            const auto &peer=plans[1-source].at(name);
            for(const auto &[slot,row]:p.broad)
            {
                if(!charge(1))return {};
                if(row["complete"].toBool() || !peer.broad.contains(slot))continue;
                const auto &other=peer.broad.at(slot);
                if(row["retainedCalendarChangedByTown"].toBool() || other["retainedCalendarChangedByTown"].toBool() ||
                   !other["complete"].toBool() || other["reason"]=="AFTER_CUTOFF" || !other["result"].isDouble() ||
                   other["result"].toInteger()!=0 || row["schedule"]!=other["schedule"])continue;
                if(!charge(row["schedule"].toObject()["halfTurnCutoff"].toInt()))return {};
                const auto ceiling=frontierTitleExposure(row);
                if(ceiling<=0)continue;
                if(!charge(2*static_cast<std::int64_t>(plans[source].size())))return {};
                TownIntention key{slot[0],slot[1],slot[2],-1,-1};std::set<QString> aliases;
                for(const auto &[alias,physical]:plans[source])
                {
                    if(!plans[1-source].contains(alias))continue;
                    const auto &otherPolicy=plans[1-source].at(alias);
                    if(physical.physical!=p.physical || otherPolicy.physical!=peer.physical ||
                       !physical.broad.contains(slot) || !otherPolicy.broad.contains(slot))continue;
                    const auto &x=physical.broad.at(slot),&y=otherPolicy.broad.at(slot);
                    if(!x["retainedCalendarChangedByTown"].toBool() && !y["retainedCalendarChangedByTown"].toBool() &&
                       !x["complete"].toBool() && y["complete"].toBool() && y["reason"]!="AFTER_CUTOFF" &&
                       y["result"].isDouble() && !y["result"].toInteger() && x["schedule"]==row["schedule"] &&
                       y["schedule"]==row["schedule"] && x["firstPossible"]==row["firstPossible"])aliases.insert(alias);
                }
                // Recompute the opponent minimum under this title-only scenario. A raid
                // under a different reply can overtake the current worst response.
                const auto swing=shift(plans[source],aliases,slot[2]==1?-ceiling:ceiling);
                if(!swing && p.score!=worst[source])continue;
                if(best && std::pair{swing,ceiling}<=std::pair{best->sensitivity,best->titleOnly})continue;
                best=TownDecisionFrontier{name,key,0,swing,source,std::move(aliases),p.score,peer.score,other,"PROPERTY_ADMISSION",ceiling,{}};
            }
        }
        return finish();
    }

    inline QJsonObject townFrontierHearing(const TownDecisionFrontier &request,const QString &policy,const QJsonObject &team)
    {
        for(const auto &v:team["replyAlternatives"].toArray())
        {
            const auto reply=v.toObject();if(reply["replyPolicy"]!=policy)continue;
            if(request.property())for(const auto &v:reply["townDecisionFacts"].toObject()["broad"].toArray())
            {
                const auto b=v.toObject();const auto at=b["tile"].toArray();
                if(at.size()==2 && at[0].toInt()==request.intention[0] && at[1].toInt()==request.intention[1] &&
                   b["attackingSide"].toInt(-1)==request.intention[2])return b;
            }
            for(const auto &w:reply["townDecisionFacts"].toObject()["named"].toArray())
            {
                const auto item=w.toObject();
                if(readTownIntention(item)==request.intention && item.contains("requestedWork"))return item;
            }
            return {{"hearing","NO_PRIORITY_HEARING_RECORDED"}};
        }
        return {{"hearing","OPPONENT_REPLY_UNPRICED"}};
    }

    inline std::optional<TownDecisionFrontier> bindTownAdmission(const TownDecisionFrontier &q,const QJsonObject &left,const QJsonObject &right)
    {
        if(!q.property())return {};
        std::optional<TownDecisionFrontier> best;
        for(const auto &policy:q.policies)
        {
            const auto a=townFrontierHearing(q,policy,left),b=townFrontierHearing(q,policy,right);
            if(a["retainedCalendarChangedByTown"].toBool() || b["retainedCalendarChangedByTown"].toBool() ||
               a["initialOffer"].toInteger()<=0 || a["initialOffer"]!=b["initialOffer"] ||
               a["admission"].toObject()["offer"].toInteger()<=0 ||
               a["admission"].toObject()["offer"]!=b["admission"].toObject()["offer"] ||
               a["admission"].toObject()["status"]=="EXISTING_SCOPE_WORK_LIMITED" ||
               b["admission"].toObject()["status"]=="EXISTING_SCOPE_WORK_LIMITED")return {};
            const auto &source=q.sourceArm?b:a;
            std::set<TownIntention> settled;
            const auto &team=q.sourceArm?right:left;
            for(const auto &v:team["replyAlternatives"].toArray())
            {
                const auto reply=v.toObject();if(reply["replyPolicy"]!=policy)continue;
                settled=stockWitnesses(reply);
                for(const auto &item:reply["townDecisionFacts"].toObject()["named"].toArray())
                    if(const auto n=item.toObject();n["answered"].toBool())
                        if(auto due=readTownIntention(n))settled.insert(*due);
            }
            for(const auto *arm:{&a,&b})for(const auto &v:(*arm)["admission"].toObject()["witnesses"].toArray())
            {
                const auto w=v.toObject();const auto key=readTownIntention(w);
                if(!key || (*key)[0]!=q.intention[0] || (*key)[1]!=q.intention[1] || (*key)[2]!=q.intention[2] ||
                   w["stops"].toArray().isEmpty())continue;
                bool already=false;
                for(const auto &item:source["admission"].toObject()["retainedOutcomesAlreadyCovered"].toArray())
                {
                    const auto done=item.toArray();
                    if(done.size()==2 && done[1].toInt()==(*key)[4] && (done[0].toInt()<0 || done[0].toInt()==(*key)[3]))already=true;
                }
                if(already || settled.contains(*key))continue;
                const auto only=w["optimisticTitleOnly"].toInteger();
                if(!only || only==std::numeric_limits<qint64>::min())continue;
                const auto amount=std::abs(only);
                if(best && (best->titleOnly>amount || (best->titleOnly==amount && best->intention<=*key)))continue;
                best=q;best->stage="DATED_PROPERTY_ADMISSION";best->intention=*key;best->titleOnly=amount;best->route=w;
                best->route["witnessArm"]=int(arm==&b);
            }
        }
        return best;
    }

    inline QJsonObject townFrontierPair(const TownDecisionFrontier &q,const QJsonObject &left,const QJsonObject &right)
    {
        auto result=q.record();bool equal=true,both=true;QJsonArray equivalent;
        for(const auto &p:q.policies)
        {
            const auto a=townFrontierHearing(q,p,left),b=townFrontierHearing(q,p,right);
            equal&=a["initialOffer"].toInteger()>0 && a["initialOffer"]==b["initialOffer"] &&
                a["requestedWork"]==b["requestedWork"] && a["initialOffer"]==a["requestedWork"];
            both&=a["answered"].toBool() && b["answered"].toBool();
            if(p==q.policy){result["left"]=a;result["right"]=b;}
            else equivalent.append(QJsonObject{{"replyPolicy",p},{"left",a},{"right",b}});
        }
        if(!equivalent.isEmpty())result["equivalentHearings"]=equivalent;
        result["equalInitialOffers"]=equal && !q.policies.empty();
        result["bothNamedAnswers"]=!q.property() && both && !q.policies.empty();return result;
    }
}
