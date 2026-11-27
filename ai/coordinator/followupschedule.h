#pragma once

#include "ai/coordinator/followupdecision.h"

namespace Coordinator
{
    struct FollowupVisit
    {
        DecisionRange range;                  // Fully ranked or genuinely one-sided.
        std::optional<DecisionRank> nominal;  // Can guide ordering; never published as an endpoint.
        QJsonObject record;
    };

    inline QJsonArray followupRank(const DecisionRank &r)
    {
        return {static_cast<int>(std::get<0>(r)),std::get<1>(r),std::get<2>(r),qint64(std::get<3>(r))};
    }

    inline void describeFollowupVisit(QJsonObject &to,const FollowupVisit &v)
    {
        if(v.range.lower) to["lower"]=followupRank(*v.range.lower);
        if(v.range.upper) to["upper"]=followupRank(*v.range.upper);
        if(v.nominal) to["quoted"]=followupRank(*v.nominal);
    }

    inline std::optional<RequestedFollowup> followupRequest(const QJsonObject &v)
    {
        const auto at=v["end"].toArray();
        if(at.size()!=2 || v["unit"].toInt(-1)<0 || v["vehicle"].toInt(-1)<0 ||
            v["target"].toInt(-1)<0 || v["weapon"].toInt(-1)<0 || v["weapon"].toInt()>1) return {};
        RequestedFollowup q;
        q.actor=v["unit"].toInt(); q.vehicle=v["vehicle"].toInt(); q.target=v["target"].toInt();q.weapon=v["weapon"].toInt();
        q.end={at[0].toInt(-1),at[1].toInt(-1)};
        if(q.end.x<0 || q.end.y<0) return {};
        return q;
    }

    inline QString followupLabel(const RequestedFollowup &q)
    {
        return QStringLiteral("FIRE_%1_%2_%3_%4_%5_%6").arg(q.actor).arg(q.vehicle).arg(q.target)
            .arg(q.end.x).arg(q.end.y).arg(q.weapon);
    }

    // Existing debt is a constraint on the dated parent. A child that did
    // no new uncertain work cannot retire somebody else's evidence.
    inline bool followupHistoryClosed(const QJsonObject &parent,bool townEvidenceComplete,
        int outstandingStock,bool fundedPrefixBound,bool financialInputsBound)
    {
        if(!townEvidenceComplete || outstandingStock || !fundedPrefixBound || !financialInputsBound)return false;
        if(parent.value("priceStatus")=="UNRESOLVED" || parent.contains("boundedByReply"))return false;
        for(auto i=parent.begin();i!=parent.end();++i){
            if(!i.key().startsWith("unpriced"))continue;
            const auto v=i.value();
            if(v.isNull() || v.isUndefined() || (v.isArray() && v.toArray().isEmpty()) ||
               (v.isObject() && v.toObject().isEmpty()) || (v.isBool() && !v.toBool()) ||
               (v.isString() && v.toString().isEmpty()))continue;
            return false;
        }
        return true;
    }

    // Callback ranks a particular *friendly* order with its decision-specific
    // tail. Hostile choices must be minimized inside a committed approach.
    template<class Replay>
    FollowupVisit runFriendlyFollowups(const QJsonObject &menu,Replay replay,QJsonArray &audit)
    {
        const auto offers=menu["shots"].toArray();
        bool complete=menu["sourceFriendlyMenuComplete"].toBool();
        std::vector<DecisionChild> children;
        FollowupVisit best;
        const auto visit=[&](QString name,const std::optional<RequestedFollowup> &order,bool take,bool available){
            FollowupVisit v;
            if(available)v=replay(order,take);
            else v.record["open"]="DECLINED_SOURCE_ACTOR_DEBIT_UNBOUND";
            QJsonObject detail{{"choice",name},{"take",take}};
            for(const auto key:{"open","tail","replayWork","orderedReplies","responseQuantifier"})
                if(v.record.contains(key))detail[key]=v.record.value(key);
            describeFollowupVisit(detail,v);audit.append(detail);
            children.push_back({name,v.range});
            if(v.nominal && (!best.nominal || *v.nominal>*best.nominal))best=v;
        };
        const auto decline=menu["declineOffer"].toObject();
        auto retained=decline.isEmpty()?std::optional<RequestedFollowup>{}:followupRequest(decline);
        visit("DECLINE_FOLLOWUP",retained,false,menu["declinePriorKnown"].toBool() && (decline.isEmpty() || retained));
        std::set<QString> seen;
        for(const auto &val:offers) {
            const auto q=followupRequest(val.toObject());
            if(!q){complete=false;continue;}
            const auto name=followupLabel(*q);
            if(!seen.insert(name).second){complete=false;continue;}
            visit(name,q,true,true);
        }
        best.range=backUpDecisionRange(DecisionNode::FriendlyMax,children,complete);
        return best;
    }

    // The *same* fixed friendly branch is passed to each E1 callback. Its
    // previous E1 passes, reservations, buyer and capture commitments are
    // retained. `pass` prices the legal refusal of the additional hostile shot.
    template<class Replay>
    FollowupVisit runAdverseFollowups(FollowupVisit pass,const QJsonObject &menu,Replay replay,QJsonArray &audit)
    {
        std::vector<DecisionChild> choices{{"DECLINE_ADDITIONAL_ATTACK",pass.range}};
        QJsonObject first{{"answer","DECLINE_ADDITIONAL_ATTACK"}};
        describeFollowupVisit(first,pass);audit.append(first);
        FollowupVisit worst=pass;
        bool closed=menu["physicalE1MenuComplete"].toBool() && menu["enumerationComplete"].toBool();
        std::set<QString> named;
        for(const auto &a:menu["answers"].toArray()) {
            const auto v=a.toObject();const auto xy=v["end"].toArray();
            const int id=v["shooter"].toInt(-1),slot=v["weapon"].toInt(-1);
            if(id<0 || slot<0 || slot>=2 || xy.size()!=2){closed=false;continue;}
            const TilePoint end{xy[0].toInt(-1),xy[1].toInt(-1)};
            if(end.x<0 || end.y<0){closed=false;continue;}
            const auto key=QStringLiteral("ANSWER_%1_%2_%3_%4").arg(id).arg(slot).arg(end.x).arg(end.y);
            if(!named.insert(key).second){closed=false;continue;}
            auto branch=replay(RequestedFollowup::Adverse{id,slot,end});
            QJsonObject row{{"answer",key}};
            for(const auto k:{"open","replayWork","tail"})if(branch.record.contains(k))row[k]=branch.record.value(k);
            describeFollowupVisit(row,branch);audit.append(row);
            choices.push_back({key,branch.range});
            if(branch.nominal && (!worst.nominal || *branch.nominal<*worst.nominal))worst=std::move(branch);
        }
        worst.range=backUpDecisionRange(DecisionNode::OpponentMin,choices,closed);
        return worst;
    }
}
