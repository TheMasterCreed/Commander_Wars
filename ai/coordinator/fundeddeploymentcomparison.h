#pragma once

#include "ai/coordinator/rankedpairproof.h"

namespace Coordinator
{
    // A single funded unit chooses one deployment for all the raids it may
    // later encounter. Group questions by purchase, never by future target.
    using FundedDeploymentId=std::tuple<int,int,int,int,QString>;
    using FundedDeploymentMenu=std::map<FundedDeploymentId,std::set<std::pair<int,int>>>;

    inline std::optional<FundedDeploymentId> postPurchase(const QJsonObject &question)
    {
        const auto site=question["factory"].toArray();
        const int tick=question["build"].toInt(-1);
        const auto type=question["type"].toString();
        if(site.size()!=2 || site[0].toInt(-1)<0 || site[1].toInt(-1)<0 || tick<0 ||
           !question["enemy"].isBool() || type.isEmpty() || tick%2!=int(question["enemy"].toBool()))return {};
        return FundedDeploymentId{int(question["enemy"].toBool()),site[0].toInt(),site[1].toInt(),tick,type};
    }

    inline FundedDeploymentMenu proposedDeploymentMenu(const QJsonArray &questions)
    {
        FundedDeploymentMenu menu;
        for(const auto &v:questions)
        {
            const auto q=v.toObject();
            if(auto id=postPurchase(q))
                for(const auto &z:q["posts"].toArray())
                {
                    const auto xy=z.toArray();
                    if(xy.size()==2 && xy[0].toInt(-1)>=0 && xy[1].toInt(-1)>=0)
                        menu[*id].emplace(xy[0].toInt(),xy[1].toInt());
                }
        }
        return menu;
    }

    inline bool paidAtPost(const QJsonObject &o,const FundedDeploymentId &id,const QJsonArray &post)
    {
        const auto &[side,x,y,tick,type]=id;
        return o.value("conditionedFundedPost").toBool() && o.value("enemy").isBool() && int(o["enemy"].toBool())==side &&
               o["factory"]==QJsonArray{x,y} && o["buildHalfTurn"].toInt(-1)==tick && o["type"]==type &&
               o["firstStepHalfTurn"].toInt(-1)==tick+2 && o["firstStop"]==post;
    }

    struct VerifiedPost {FundedDeploymentId purchase;QJsonArray post;};

    inline QString fundedDeploymentAction(const FundedDeploymentId &id,int px,int py)
    {
        const auto &[side,x,y,tick,type]=id;
        return QStringLiteral("%1 %2 from (%3,%4), build %5, post (%6,%7) at %8")
            .arg(side?"ENEMY":"FRIENDLY",type).arg(x).arg(y).arg(tick).arg(px).arg(py).arg(tick+2);
    }

    inline bool canDiscoverAnotherDeployment(const QJsonObject &team,const VerifiedPost &commitment)
    {
        for(const auto &v:team.value("replyAlternatives").toArray())
            for(const auto &item:v.toObject().value("productionOrders").toArray())
            {
                const auto o=item.toObject();
                if(o["kind"]=="DEPLOY" && o["retainedDeployment"].toBool() && o["end"].toArray().size()==2 &&
                   o["end"]==o["firstStop"] && !paidAtPost(o,commitment.purchase,commitment.post))return true;
            }
        return false;
    }

    inline DecisionRange backedFundedDeployment(int side,const QJsonArray &continuations)
    {
        std::vector<DecisionChild> choices;
        for(const auto &v:continuations)
        {
            const auto c=v.toObject();
            const auto at=c["post"].toArray();
            if(at.size()!=2 || at[0].toInt(-1)<0 || at[1].toInt(-1)<0)return {};
            DecisionRange child;
            if(c["allScoredLeavesAtCommittedPost"].toBool() && c["sameStockPrefixCertified"].toBool() && c["continuationRangeInputsComplete"].toBool())
                if(const auto rank=fullPolicyRank(c))child.lower=child.upper=*rank;
            choices.push_back({QString::number(at[0].toInt())+","+QString::number(at[1].toInt()),std::move(child)});
        }
        // The observed postings are a subset. Unheard deployments can improve
        // the chooser's outcome; they cannot erase an attainable choice.
        return backUpDecisionRange(side?DecisionNode::OpponentMin:DecisionNode::FriendlyMax,choices,false);
    }

    // Isolate every whole-calendar price on its own evidence. Discovery on a
    // different commitment must not turn a local bound into a team bound.
    template<class Closed> DecisionRange fundedTeamBounds(const QJsonObject &team,Closed closed)
    {
        const auto replies=team.value("replyAlternatives").toArray();
        bool possiblyBounded=false;
        for(const auto &v:replies)for(const auto &g:v.toObject().value("fundedDeploymentContinuations").toArray())
            possiblyBounded|=g.toObject().contains("achievableLower") || g.toObject().contains("adverseUpper");
        if(!possiblyBounded)return {};
        using Choice=std::tuple<FundedDeploymentId,int,int>;
        std::map<Choice,std::vector<DecisionChild>> friendly;
        std::map<QString,QString> adversePosts;
        std::vector<DecisionChild> adverse;
        bool exactLabels=true;
        std::set<QString> seen;
        for(const auto &entry:replies)
        {
            const auto row=entry.toObject();const auto label=row.value("replyPolicy").toString();
            exactLabels &= !label.isEmpty() && seen.insert(label).second;
            const auto witnessed=[&](QJsonObject leaf,const VerifiedPost *commitment=nullptr){
                leaf["replyPolicy"]=label;
                QJsonObject owner{{"priceStatus","WITNESSED"},{"replyAlternatives",QJsonArray{leaf}}};
                return closed(owner,SharedReplyBound<DecisionRank>(&owner),std::set<QString>{label},commitment);
            };
            DecisionRange reply;
            for(const auto &v:row.value("fundedDeploymentContinuations").toArray())
            {
                const auto group=v.toObject();const auto id=postPurchase(group);
                if(!id)return {};
                QJsonArray admitted;
                for(const auto &c:group.value("continuations").toArray())
                {
                    const auto line=c.toObject();
                    const VerifiedPost commitment{*id,line["post"].toArray()};
                    if(auto rank=fullPolicyRank(line); rank && line["allScoredLeavesAtCommittedPost"].toBool() && line["sameStockPrefixCertified"].toBool() &&
                       line["continuationRangeInputsComplete"].toBool() && commitment.post.size()==2 && witnessed(line,&commitment))
                    {
                        admitted.append(line);
                        if(!group["enemy"].toBool())
                        {
                            const Choice choice{*id,commitment.post[0].toInt(),commitment.post[1].toInt()};
                            DecisionRange held;held.lower=*rank;
                            friendly[choice].push_back({label,std::move(held)});
                        }
                    }
                }
                const auto bounds=backedFundedDeployment(group["enemy"].toBool(),admitted);
                if(!bounds.valid())return {};
                if(bounds.upper && (!reply.upper || *bounds.upper<*reply.upper))
                {
                    reply.reduceUpper(*bounds.upper,"FIXED_OPPOSING_DEPLOYMENT",label);
                    for(const auto &x:admitted)
                    {
                        const auto c=x.toObject();
                        if(fullPolicyRank(c)!=bounds.upper)continue;
                        const auto p=c["post"].toArray();
                        adversePosts[label]=fundedDeploymentAction(*id,p[0].toInt(),p[1].toInt());
                        break;
                    }
                }
            }
            adverse.push_back({label,std::move(reply)});
        }
        if(!exactLabels)return {};
        const bool complete=team.value("completeOpeningReplyMenu").toBool() &&
            !team.contains("unpricedReplyPolicy") && !team.contains("boundedByReply");
        auto range=backUpDecisionRange(DecisionNode::OpponentMin,adverse,false);
        if(range.upper)range.upperActions.append(adversePosts.at(range.adversePolicy));
        // Different labels need not be observable to the deploying player.
        // This floor uses precisely one post for every legal opponent opening.
        if(complete)for(const auto &[choice,children]:friendly)
        {
            if(children.size()!=static_cast<std::size_t>(replies.size()))continue;
            const auto held=backUpDecisionRange(DecisionNode::OpponentMin,children,true);
            if(!held.valid())return {};
            if(held.lower && (!range.lower || *held.lower>*range.lower))
            {
                range.raiseLower(*held.lower,"ONE_PRECOMMITTED_POST_FOR_ALL_OPPONENT_OPENINGS");
                range.lowerActions={fundedDeploymentAction(std::get<0>(choice),std::get<1>(choice),std::get<2>(choice))};
                range.lowerActions.append(held.lowerActions);
            }
        }
        return range;
    }
}
