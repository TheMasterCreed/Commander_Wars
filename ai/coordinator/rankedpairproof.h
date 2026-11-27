#pragma once

#include "ai/coordinator/terminalvalue.h"
#include "ai/coordinator/towncomparisonevidence.h"
#include <QStringList>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

namespace Coordinator
{
    using DecisionRank = std::tuple<TerminalClass,int,int,qint64>;
    struct DecisionRange
    {
        std::optional<DecisionRank> lower,upper;
        QString lowerBasis,upperBasis,adversePolicy;
        QStringList lowerActions,upperActions;
        QString error;

        bool valid() const {return error.isEmpty() && (!lower || !upper || *lower<=*upper);}
        bool exact() const {return valid() && lower && lower==upper;}
        void raiseLower(const DecisionRank &v,const QString &basis)
        {
            if(!lower || *lower<v){lower=v;lowerBasis=basis;lowerActions.clear();}
        }
        void reduceUpper(const DecisionRank &v,const QString &basis,const QString &policy={})
        {
            if(!upper || v<*upper){upper=v;upperBasis=basis;adversePolicy=policy;upperActions.clear();}
        }
    };
    enum class DecisionNode { FriendlyMax, OpponentMin };
    struct DecisionChild
    {
        QString action;
        DecisionRange range;
    };

    // A child is an executable option at this information set. A conditional
    // strategy chosen before a revelation must be the same across its outcomes.
    inline DecisionRange backUpDecisionRange(DecisionNode owner,std::span<const DecisionChild> children,bool completeMenu)
    {
        DecisionRange parent;
        if(children.empty())return parent;
        const bool maximize=owner==DecisionNode::FriendlyMax;
        bool allLower=completeMenu,allUpper=completeMenu;
        std::set<QString> names;
        QString lowerAction,upperAction;
        QStringList actions;
        for(const auto &[action,child]:children)
        {
            if(action.isEmpty() || !names.insert(action).second || !child.valid())
            {
                DecisionRange invalid;
                invalid.error=child.valid()?QStringLiteral("MISSING_OR_DUPLICATE_ACTION"):QStringLiteral("INVALID_CHILD_RANGE");
                return invalid;
            }
            actions.append(action);
            allLower &= child.lower.has_value();
            allUpper &= child.upper.has_value();
            if(child.lower && (!parent.lower || (maximize?*child.lower>*parent.lower:*child.lower<*parent.lower)))
            {parent.lower=child.lower;lowerAction=action;}
            if(child.upper && (!parent.upper || (maximize?*child.upper>*parent.upper:*child.upper<*parent.upper)))
            {parent.upper=child.upper;upperAction=action;}
        }
        if(!maximize && !allLower)parent.lower.reset();
        if(maximize && !allUpper)parent.upper.reset();
        if(parent.lower)
        {
            parent.lowerBasis=maximize?"ACHIEVABLE_FRIENDLY_ACTION":"FLOOR_FOR_ALL_OPPONENT_ACTIONS";
            parent.lowerActions=maximize?QStringList{lowerAction}:actions;
        }
        if(parent.upper)
        {
            parent.upperBasis=maximize?"CEILING_FOR_ALL_FRIENDLY_ACTIONS":"FEASIBLE_OPPONENT_ACTION";
            parent.upperActions=maximize?actions:QStringList{upperAction};
            if(!maximize)parent.adversePolicy=upperAction;
        }
        return parent;
    }

    enum class PairPreference { Open, First, Second, Tie };

    inline PairPreference strictPairPreference(const DecisionRange &first,const DecisionRange &second)
    {
        if(!first.valid() || !second.valid())return PairPreference::Open;
        if(first.lower && second.upper && *first.lower>*second.upper)return PairPreference::First;
        if(second.lower && first.upper && *second.lower>*first.upper)return PairPreference::Second;
        if(first.exact() && second.exact() && first.lower==second.lower)return PairPreference::Tie;
        return PairPreference::Open;
    }
    inline std::optional<DecisionRank> fullPolicyRank(const QJsonObject &row)
    {
        const auto terminal=row.value("terminalClass"),security=row.value("hqSecurity"),money=row.value("policyTotal");
        const auto urgency=row.value("conditionalThreatRank");
        if((row.contains("priceStatus") && row.value("priceStatus")!="WITNESSED") || !readSharedTownReply(row).complete || !terminal.isDouble() ||
           !security.isDouble() || security.toInt(0)!=security.toInt(1) || !money.isDouble() ||
           money.toInteger(0)!=money.toInteger(1) || terminal.toInt(-2)<-1 || terminal.toInt(-2)>1 ||
           (urgency.toInt(0)!=urgency.toInt(1) && !(urgency.isUndefined() && terminal.toInt()!=0)))return {};
        return DecisionRank{static_cast<TerminalClass>(terminal.toInt()),security.toInt(),urgency.toInt(),money.toInteger()};
    }
    inline QJsonArray decisionRankRecord(const DecisionRank &r)
    {return {int(std::get<0>(r)),std::get<1>(r),std::get<2>(r),std::get<3>(r)};}

    inline std::optional<DecisionRank> economicContinuationEndpoint(const QJsonValue &value)
    {
        const auto rank=value.toArray();
        if(rank.size()!=4 || rank[0].toInt(-2)!=int(TerminalClass::Unresolved) ||
           rank[1].toInt(1)>0 || rank[1].toInt(-3)<-2 || rank[2].toInt(1)>0 || rank[2].toInt(-7)<-6 ||
           !rank[3].isDouble() || rank[3].toInteger(0)!=rank[3].toInteger(1))return {};
        return DecisionRank{TerminalClass::Unresolved,rank[1].toInt(),rank[2].toInt(),rank[3].toInteger()};
    }

    // These continuation endpoints cover only the capture-closed economic
    // projection supplied by the caller. A witnessed adverse reply can cap
    // its own team even when a different reply on that team remains unexplored.
    inline std::array<DecisionRange,2> matchedContinuationBounds(const QJsonObject &a,const QJsonObject &b,
        bool matchedScope,bool noFutureCaptureChallenges)
    {
        if(!matchedScope || !noFutureCaptureChallenges)return {};
        std::array<DecisionRange,2> ranges;
        const std::array<const QJsonObject*,2> teams{&a,&b};
        for(int arm=0;arm<2;++arm)
        {
            const auto &team=*teams[arm];
            const bool completeMenu=team.value("completeOpeningReplyMenu").toBool() &&
                !team.contains("unpricedReplyPolicy") && !team.contains("boundedByReply");
            std::set<QString> menu;
            std::vector<DecisionChild> children;
            for(const auto &entry:team.value("replyAlternatives").toArray())
            {
                const auto row=entry.toObject();
                const auto name=row.value("replyPolicy").toString();
                if(name.isEmpty() || !menu.insert(name).second)return {};
                DecisionRange child;
                if(row.value("continuationRangeInputsComplete").toBool())
                {
                    if(auto point=fullPolicyRank(row);point && std::get<0>(*point)==TerminalClass::Unresolved)
                        child.lower=child.upper=point;
                    else if(row.value("priceStatus")=="UNRESOLVED")
                    {
                        const auto branch=row.value("optionalAttackDecisionRange").toObject();
                        child.lower=economicContinuationEndpoint(branch.value("passive"));
                        child.upper=economicContinuationEndpoint(branch.value("bestIfEnemyPasses"));
                        if((branch.contains("passive") && !child.lower) ||
                           (branch.contains("bestIfEnemyPasses") && !child.upper) || !child.valid())return {};
                    }
                }
                // An incomplete sibling may veto a point without vetoing a
                // polarity backed by fully priced witnesses at the sole F1
                // decision. Preserve all outer matched-scope requirements.
                const auto p=row.value("verifiedFollowupPolarity").toObject();
                const auto seal=team.value("followupTeamBinding").toString();
                if(!p.isEmpty() && row.value("priceStatus")=="UNRESOLVED" &&
                   p.value("scope")=="ONLY_OUTER_FRIENDLY_DECISION_AFTER_FIXED_REPLY" &&
                   p.value("ancestorBound").toBool() && p.value("siblingDebtCoveredByPolarity").toBool() &&
                   p.value("outerChoices").toInt()==1 &&
                   !seal.isEmpty() && p.value("team").toString()==seal && p.value("policy").toString()==name &&
                   !row.value("followupResponseBinding").toString().isEmpty() && p.value("response")==row.value("followupResponseBinding")){
                    const auto program=p.value("program").toObject();
                    if(program.value("owner")!="F1_FRIENDLY_MAX")return {};
                    std::vector<DecisionChild> local;
                    std::set<QString> expected{"DECLINE_FOLLOWUP"},heard;
                    for(const auto &item:program.value("physicalOffers").toArray()){
                        const auto q=item.toObject();const auto at=q["end"].toArray();
                        if(at.size()!=2 || q["unit"].toInt(-1)<0 || q["vehicle"].toInt(-1)<0 || q["target"].toInt(-1)<0 ||
                           q["weapon"].toInt(-1)<0 || q["weapon"].toInt()>1)return {};
                        const auto action=QStringLiteral("FIRE_%1_%2_%3_%4_%5_%6").arg(q["unit"].toInt()).arg(q["vehicle"].toInt())
                            .arg(q["target"].toInt()).arg(at[0].toInt()).arg(at[1].toInt()).arg(q["weapon"].toInt());
                        if(!expected.insert(action).second)return {};
                    }
                    for(const auto &childValue:program.value("choices").toArray()){
                        const auto witness=childValue.toObject();
                        const auto label=witness.value("choice").toString();DecisionRange tail;
                        if(!expected.contains(label) || !heard.insert(label).second)return {};
                        if(witness.contains("lower")){tail.lower=economicContinuationEndpoint(witness.value("lower"));if(!tail.lower)return {};}
                        if(witness.contains("upper")){tail.upper=economicContinuationEndpoint(witness.value("upper"));if(!tail.upper)return {};}
                        if(label=="DECLINE_FOLLOWUP" && !program.value("declinePriorKnown").toBool() && (tail.lower || tail.upper))return {};
                        local.push_back({label,tail});
                    }
                    if(!heard.contains("DECLINE_FOLLOWUP") || (program.value("friendlyMenuComplete").toBool() && heard!=expected))return {};
                    const auto checked=backUpDecisionRange(DecisionNode::FriendlyMax,local,program.value("friendlyMenuComplete").toBool());
                    if(!checked.valid())return {};
                    if(p.contains("lower")){
                        const auto lower=economicContinuationEndpoint(p.value("lower"));
                        if(!lower || lower!=checked.lower || p.value("lowerActions").toArray()!=QJsonArray::fromStringList(checked.lowerActions))return {};
                        child.raiseLower(*lower,"WITNESSED_F1_POLICY_AT_BOUND_HISTORY");
                    }
                    if(p.contains("upper")){
                        const auto upper=economicContinuationEndpoint(p.value("upper"));
                        if(!upper || upper!=checked.upper || p.value("upperActions").toArray()!=QJsonArray::fromStringList(checked.upperActions))return {};
                        child.reduceUpper(*upper,"ALL_F1_ORDERS_HAVE_CEILINGS",name);
                    }
                    if(!child.valid())return {};
                }
                children.push_back({name,std::move(child)});
            }
            ranges[arm]=backUpDecisionRange(DecisionNode::OpponentMin,children,completeMenu);
        }
        return ranges;
    }

    inline QJsonObject unresolvedTeamConsequence(const QJsonObject &team)
    {
        QJsonObject worst;
        std::optional<DecisionRank> priority;
        for(const auto &v:team.value("replyAlternatives").toArray())
        {
            const auto row=v.toObject();const auto amount=row.value("partialPolicyTotal");
            if(!amount.isDouble() || amount.toInteger(0)!=amount.toInteger(1) || row["priceStatus"]=="WITNESSED")continue;
            const auto term=row["terminalClass"].toInt(-2);
            if(term< -1 || term>1)continue;
            const DecisionRank provisional{TerminalClass(term),row["hqSecurity"].toInt(),
                row["conditionalThreatRank"].toInt(),amount.toInteger()};
            if(priority && *priority<=provisional)continue;
            const auto gap=row["unpricedFinancialInputs"].toObject();
            const auto leg=gap["deferredStockLeg"].toObject();
            const int who=leg["unit"].toInt(-1),entry=leg["proposedEntryTick"].toInt(-1);
            if(who<0 || entry<0 || leg["tile"].toArray().size()!=2)continue;
            worst={{"replyPolicy",row["replyPolicy"]},{"unit",who},{"tile",leg["tile"]},{"entryTick",entry},
                {"reason",leg["status"]},{"consequence","CAPTURE_INCOME_UNIT_SURVIVAL_AND_ALTERNATE_DUTIES"},
                {"provisionalRankForSearch",decisionRankRecord(provisional)}};
            for(auto key:{"from","proposedCompletionTick","conditionalGunDuties","shots","captureInstallments"})
                if(leg.contains(key))worst[key]=leg[key];
            QJsonArray titles;
            for(const auto &bid:row["captureClaims"].toArray())
            {
                const auto c=bid.toObject();const int starts=c["entryDayOffset"].toInt(-1),ends=c["completionDayOffset"].toInt(-1);
                if(c["unit"].toInt(-1)<0 || c["tile"].toArray().size()!=2 || starts<0 || ends<starts || !c["enemy"].isBool())continue;
                const int side=int(c["enemy"].toBool());
                titles.append(QJsonObject{{"unit",c["unit"]},{"tile",c["tile"]},{"entryTick",2*starts+side},
                    {"completionTick",2*ends+side},{"bookedNominalIncome",c["signedIncome"]},{"foregoneShot",c["foregoneShot"]}});
            }
            if(!titles.isEmpty())worst["titleChangesOnThisSameCalendar"]=titles;
            priority=provisional;
        }
        const auto family=team.value("unpricedOpeningReplyFamily").toObject();
        if(!family.isEmpty())
        {
            QJsonArray exposures;
            const auto buys=team.value("purchaseBoundary").toObject().value("modeledOpening").toArray();
            for(const auto &v:team.value("orderedRecruitResponses").toArray())
            {
                const auto row=v.toObject();const auto site=row["factory"].toArray();
                if(site.size()!=2 || row["enemyShooter"].toInt(-1)<0)continue;
                QJsonObject firing{{"unit",row["enemyShooter"]},{"tile",site},{"entryTick",1},{"reason",row["reason"]},
                    {"consequence","MAY_HIT_PURCHASE_AFTER_OTHER_ENEMY_ORDERS"}};
                for(const auto &p:buys)if(p.toObject()["factory"].toArray()==site)firing["targetType"]=p.toObject()["type"];
                exposures.append(firing);
            }
            if(!exposures.isEmpty())
            {
                auto opening=exposures.first().toObject();opening["openingShotOptions"]=exposures;
                if(!worst.isEmpty())opening["knownReplyContinuation"]=worst;
                return opening;
            }
        }
        if(!worst.isEmpty())return worst;
        const auto phase=team.value("unpricedReplyDetails").toObject();
        if(phase.isEmpty())return {};
        return {{"replyPolicy",team.value("unpricedReplyPolicy")},{"reason",team.value("unpricedReplyReason")},
                {"refusal",phase},{"consequence","UNPRICED_OPPONENT_RESPONSE"}};
    }

    struct RankedPairProof
    {
        std::array<DecisionRange,2> range;
        std::array<int,2> completePolicies{};
        std::array<std::optional<DecisionRank>,2> localAdverse;
        std::array<QString,2> localPolicy;
        std::array<QString,2> missing;
        std::array<QJsonObject,2> openEvidence,openingSurvival,openingLoss,economicEndpoint;
        PairPreference result() const {return strictPairPreference(range[0],range[1]);}
        QJsonObject record(const QString &first,const QString &second,const QJsonObject &frontier={}) const
        {
            const auto outcome=result();QJsonArray arms;
            for(int i=0;i<2;++i){const auto &b=range[i];QJsonObject arm{{"plan",i==0?first:second},{"witnessedReplies",completePolicies[i]}};
                if(b.lower){arm["lower"]=decisionRankRecord(*b.lower);arm["lowerBasis"]=b.lowerBasis;}
                if(b.upper){arm["upper"]=decisionRankRecord(*b.upper);arm["upperBasis"]=b.upperBasis;if(!b.adversePolicy.isEmpty())arm["adversePolicy"]=b.adversePolicy;}
                if(b.upperBasis=="WINNING_MOBILE_HEADQUARTERS_CAPTURE" && !openingLoss[i].isEmpty()){
                    arm["upperProvedPrefix"]=QJsonArray{int(TerminalClass::ForcedLoss)};
                    arm["openingLossProof"]=openingLoss[i];
                }
                if(b.lowerBasis=="SURVIVES_EVERY_OPENING_RESPONSE" && !openingSurvival[i].isEmpty())
                {
                    arm["lowerKnownPrefix"]=QJsonArray{static_cast<int>(TerminalClass::Unresolved)};
                    arm["openingSurvivalProof"]=openingSurvival[i];
                }
                if(!economicEndpoint[i].isEmpty())arm["economicEndpoint"]=economicEndpoint[i];
                if(!b.lowerActions.isEmpty())arm["lowerActions"]=QJsonArray::fromStringList(b.lowerActions);
                if(!b.upperActions.isEmpty())arm["upperActions"]=QJsonArray::fromStringList(b.upperActions);
                if(!b.valid())arm["boundError"]=b.error.isEmpty()?QStringLiteral("CONTRADICTORY_ENDPOINTS"):b.error;
                if(localAdverse[i] && !b.upper){arm["witnessOnCurrentScope"]=decisionRankRecord(*localAdverse[i]);arm["witnessPolicy"]=localPolicy[i];}
                if(!missing[i].isEmpty())arm["missing"]=missing[i];arms.append(arm);}
            QJsonObject out{{"status",outcome==PairPreference::First?"FIRST_PROVED":outcome==PairPreference::Second?"SECOND_PROVED":outcome==PairPreference::Tie?"EXACT_TIE":"INSUFFICIENT_BOUNDS"},{"arms",arms}};
            if(outcome==PairPreference::First || outcome==PairPreference::Second)out["preferred"]=outcome==PairPreference::First?first:second;
            else if(outcome==PairPreference::Open && !frontier.isEmpty() && !frontier["bothNamedAnswers"].toBool())out["unresolvedConsequence"]=frontier;
            if(outcome==PairPreference::Open &&
               (!out.contains("unresolvedConsequence") || frontier["capper"].toInt(-1)<0 || frontier["entryTick"].toInt(-1)<0))
            {
                QJsonArray questions;
                for(int arm=0;arm<2;++arm)if(!openEvidence[arm].isEmpty())
                {
                    auto question=openEvidence[arm];question["plan"]=arm?second:first;questions.append(question);
                }
                if(!questions.isEmpty())
                {
                    auto cause=out["unresolvedConsequence"].toObject();
                    if(cause.isEmpty())cause={{"stage","UNRESOLVED_WHOLE_TEAM_CONTINUATIONS"},{"use","SEARCH_PRIORITY_ONLY"}};
                    cause["replies"]=questions;out["unresolvedConsequence"]=cause;
                }
            }
            return out;
        }
    };

    // A completed priced board is sufficient when the common discovery contract has closed. If the peer is partial,
    // a new response may still expose a competing town or stock route. The caller must prove the owner's scope immutable.
    template<class Closed> RankedPairProof priceDecisionBounds(const QJsonObject &a,const QJsonObject &b,
        bool matchedScope,Closed closedAgainstFutureDiscovery,const std::array<DecisionRange,2> &independent={})
    {
        RankedPairProof out;out.range=independent;const std::array<const QJsonObject*,2> plans{&a,&b};
        std::array<QJsonArray,2> replies{a.value("replyAlternatives").toArray(),b.value("replyAlternatives").toArray()};
        const auto observed=a["economicObservation"].toObject();
        const bool sameObservation=!observed.isEmpty() && a["economicObservation"]==b["economicObservation"];
        std::array<std::optional<DecisionRank>,2> exact;
        std::array<bool,2> witnessed{},menuIdentified{};
        std::array<std::set<QString>,2> menus;
        for(int i=0;i<2;++i)
        {
            const auto &plan=*plans[i];
            const auto bindsOrder=[&](const QJsonObject &p){
                return !p["physicalOrders"].toArray().isEmpty() && p["physicalOrders"]==plan["openingOrderBinding"] &&
                       p["purchaseBoundary"].isObject() && p["purchaseBoundary"]==plan["purchaseBoundary"];
            };
            const auto loss=plan["openingLossProof"].toObject();
            if(loss["status"]=="FEASIBLE_OPPONENT_OPENING_WINS" && loss["scope"]=="OBSERVED_F0_TEAM_AND_BUYER" &&
               !loss["actions"].toArray().isEmpty() && !loss["basis"].toString().isEmpty() && bindsOrder(loss)){
                out.range[i].reduceUpper({TerminalClass::ForcedLoss,std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::max(),std::numeric_limits<qint64>::max()},"WINNING_MOBILE_HEADQUARTERS_CAPTURE");
                out.openingLoss[i]=loss;
            }
            const auto survival=plan.value("openingSurvivalProof").toObject();
            if(survival["status"]=="ALL_OPPONENT_OPENINGS_SURVIVE" &&
               survival["scope"]=="OBSERVED_F0_TEAM_AND_BUYER" && !survival["actions"].toArray().isEmpty() &&
               !survival["basis"].toString().isEmpty() && plan["purchaseBoundary"].isObject() &&
               survival["purchaseBoundary"]==plan["purchaseBoundary"] &&
               (!survival.contains("physicalOrders") || bindsOrder(survival)))
            {
                // This is a lower bound on the complete lexicographic rank.
                // No financial assertion follows from surviving the opening.
                out.range[i].raiseLower({TerminalClass::Unresolved,std::numeric_limits<int>::lowest(),
                    std::numeric_limits<int>::lowest(),std::numeric_limits<qint64>::lowest()},
                    QStringLiteral("SURVIVES_EVERY_OPENING_RESPONSE"));
                out.openingSurvival[i]=survival;
            }
            const auto endpoint=plan["economicEndpoint"].toObject();
            if(!endpoint.isEmpty()){
                const auto identity=plan["economicOrderIdentity"].toObject();
                const auto rank=economicContinuationEndpoint(endpoint["rank"]);
                const auto basis=endpoint["basis"].toString();
                const auto physical=identity["physicalOrders"].toArray();
                if(sameObservation && rank && !basis.isEmpty() && !physical.isEmpty() &&
                   endpoint["identity"]==identity && identity["observation"]==observed &&
                   identity["buyer"]==plan["purchaseBoundary"] && endpoint["purchaseBoundary"]==plan["purchaseBoundary"] &&
                   endpoint["physicalOrders"]==physical && physical==plan["economicOrderBinding"].toArray() &&
                   !endpoint["actions"].toArray().isEmpty()){
                    QStringList actions;
                    for(const auto &action:endpoint["actions"].toArray())actions.append(action.toString());
                    if(endpoint["direction"]=="LOWER"){
                        auto &bounds=out.range[i];
                        if(!bounds.lower || *bounds.lower<*rank){bounds.raiseLower(*rank,basis);bounds.lowerActions=actions;}
                        out.economicEndpoint[i]=endpoint;
                    }else if(endpoint["direction"]=="UPPER"){
                        auto &bounds=out.range[i];
                        if(!bounds.upper || *rank<*bounds.upper){bounds.reduceUpper(*rank,basis,endpoint["adversePolicy"].toString());bounds.upperActions=actions;}
                        out.economicEndpoint[i]=endpoint;
                    }
                }
            }
            bool validLabels=!replies[i].isEmpty();
            std::vector<DecisionChild> children;
            children.reserve(replies[i].size());
            DecisionRange physical;
            for(const auto &entry:replies[i])
            {
                const auto row=entry.toObject();
                const auto label=row.value("replyPolicy").toString();
                if(label.isEmpty() || !menus[i].insert(label).second)validLabels=false;
                DecisionRange child;
                if(const auto rank=fullPolicyRank(row))
                {
                    ++out.completePolicies[i];
                    child.lower=child.upper=rank;
                    if(row.value("physicalDecisionBoundary")=="LOST_ON_FIRST_REPLY" &&
                       row.value("hqStatus")=="ENEMY_CAN_WIN_NEXT_ACTION" &&
                       row.value("projectedTerminalHalfTurn").toInt(-1)==1 && std::get<0>(*rank)==TerminalClass::ForcedLoss)
                        physical.reduceUpper(*rank,"LOST_ON_FIRST_REPLY",label);
                }
                children.push_back({label,std::move(child)});
            }
            menuIdentified[i]=validLabels;
            const bool completeMenu=plan.value("completeOpeningReplyMenu").toBool() &&
                !plan.contains("boundedByReply") && !plan.contains("unpricedReplyPolicy");
            const auto local=backUpDecisionRange(DecisionNode::OpponentMin,children,completeMenu);
            out.localAdverse[i]=local.upper;
            out.localPolicy[i]=local.adversePolicy;
            if(validLabels && physical.upper)out.range[i].reduceUpper(*physical.upper,physical.upperBasis,physical.adversePolicy);
            exact[i]=fullPolicyRank(plan);
            witnessed[i]=plan.value("priceStatus")=="WITNESSED" && local.exact() && exact[i]==local.lower;
            if(!witnessed[i])out.missing[i]=!out.completePolicies[i]?"NO_FULL_HORIZON_POLICY_OR_TEAM_LOWER_BOUND":"NO_FULL_HORIZON_TEAM_LOWER_BOUND";
            if(!validLabels && !replies[i].isEmpty())out.missing[i]="MISSING_OR_DUPLICATE_REPLY_POLICY";
        }
        for(int i=0;i<2;++i)
        {
            const int j=1-i;const auto &owner=*plans[i];
            if(!matchedScope || !witnessed[i] || !menuIdentified[j])continue;
            const bool commonExact=witnessed[j];
            if(!commonExact && (!plans[j]->value("completeOpeningReplyMenu").toBool() ||
                               plans[j]->contains("boundedByReply") || plans[j]->contains("unpricedReplyPolicy")))
            {
                out.missing[i]="OPPONENT_REPLY_MENU_STILL_OPEN";continue;
            }
            // Each team has its own legal menu. The adverse witness on the other
            // team still needs matched stock/town evidence from its actual policy.
            SharedReplyBound<DecisionRank> bound(&owner);
            if(!commonExact && !closedAgainstFutureDiscovery(owner,bound,menus[i])){
                out.missing[i]="WINNER_SCOPE_CAN_CHANGE_ON_AN_UNPRICED_CONTINUATION";continue;}
            auto &certified=out.range[i];
            if(!certified.lower || *certified.lower<*exact[i])
            {
                certified.raiseLower(*exact[i],"COMPLETE_MATCHED_WORST_REPLY");
                for(const auto &policy:menus[i])certified.lowerActions.append(policy);
            }
            certified.reduceUpper(*exact[i],"COMPLETE_MATCHED_WORST_REPLY",out.localPolicy[i]);
            out.missing[i].clear();
            if(commonExact)continue;
            for(const auto &v:replies[j]){const auto q=v.toObject();const auto r=fullPolicyRank(q);
                if(r)bound.observe(q,*r);else bound.observeOpen(q);}
            if(bound.hasMatchedUpper()){out.range[j].reduceUpper(bound.upper(),"MATCHED_ADVERSE_REPLY",bound.witness());}
        }
        for(int i=0;i<2;++i)if(out.range[i].lower)out.missing[i].clear();
        if(out.result()==PairPreference::Open)
            for(int i=0;i<2;++i){
                out.openEvidence[i]=unresolvedTeamConsequence(*plans[i]);
                if(plans[i]->contains("economicSourceGap"))
                    out.openEvidence[i]["economicSourceGap"]=plans[i]->value("economicSourceGap");
            }
        return out;
    }
}
