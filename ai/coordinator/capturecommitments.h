#pragma once

#include "ai/coordinator/factorycommitments.h"

namespace Coordinator
{
    inline const QString NEXT_CAPTURE_STARTS = QStringLiteral("COORDINATED_NEXT_CAPTURE_STARTS_V1");

    inline bool isNextCaptureStart(const QJsonObject &claim)
    {
        return !claim["enemy"].toBool(true) && claim["departureDayOffset"].toInt(-1) == 1 &&
               claim["entryDayOffset"].toInt(-1) == 1 && claim["carry"].toInt(-1) == 0 &&
               claim["signedIncome"].toInteger(0) > 0;
    }

    inline bool captureStartTermsMatch(const QJsonObject &order, int rate, OwnerSign owner)
    {
        return order["rate"].toInt(-1)==rate && order["ownerBefore"].toInt(99)==static_cast<int>(owner);
    }

    inline QJsonArray matureCaptureStarts(const QJsonArray &prior,
        const MasterAssignmentProblem &problem, std::span<const int> publishedChoices, QJsonArray &abandoned,
        const std::function<int(TilePoint,TilePoint)> &rateAt)
    {
        QJsonArray due;
        std::set<std::pair<int,int>> datedSites;
        for (const auto &v : prior)
        {
            auto order = v.toObject();
            const auto at = factoryPoint(order["tile"]);
            const int unit = order["unit"].toInt(NO_UNIT);
            if (at == INVALID_TILE || unit == NO_UNIT) continue;
            int slot = -1, option = -1, lockedSlot = -1, lockedOption = -1;
            bool changedTerms=false;
            for (const auto &actor : problem.actors())
            {
                for (int i=0; i<static_cast<int>(actor.candidates.size()); ++i)
                {
                    const auto &c=actor.candidates[i];
                    if (!c.plannable || !capturesInPlan(c.actionKind) || c.destination!=at) continue;
                    // A non-mutable actor having a generated capture does not commit it.
                    // Only the exact effective choice handed to this comparison is binding.
                    if (!problem.canChangeActor(actor.canonicalSlot) &&
                        publishedChoices[actor.canonicalSlot]==i)
                    {
                        lockedSlot=actor.canonicalSlot;
                        lockedOption=i;
                    }
                    if (actor.engineUnitId==unit && problem.canChangeActor(actor.canonicalSlot))
                    {
                        slot=actor.canonicalSlot;
                        option=i;
                        const int observed=rateAt(c.origin,at);
                        changedTerms=!c.capture || !captureStartTermsMatch(order,observed,c.capture->ownerBefore);
                        order["observedRate"]=observed;
                    }
                    break;
                }
            }
            if (lockedSlot>=0)
            {
                order["slot"]=lockedSlot;
                order["candidate"]=lockedOption;
                order["lockedStart"]=true;
                due.append(order);
                continue;
            }
            if (slot<0)
            {
                order["status"]="NO_CURRENT_CAPTURE";
                abandoned.append(order);
                continue;
            }
            const auto key=std::pair{at.x,at.y};
            // A different capture rate or ownership changes the promised contract. Still
            // compare today's legal order; only a fresh forecast may date another start.
            if (!changedTerms && !datedSites.insert(key).second) continue;
            order["revisedTerms"]=changedTerms;
            order["slot"]=slot;
            order["candidate"]=option;
            due.append(order);
        }
        return due;
    }

    inline void storeCaptureStarts(ScriptVariables *variables, int day, int player, const QJsonArray &due,
        const QJsonObject &policy, const ReplyPassEvaluation &plan, QJsonObject &receipt)
    {
        if (!variables) return;
        std::map<std::pair<int,int>,int> started;
        for (const auto &a:plan.actors)
        {
            const auto &order=plan.replay.plan.action(a.actionIndex);
            if (capturesInPlan(order.kind)) started.emplace(std::pair{order.destination.x,order.destination.y},a.engineUnitId);
        }
        QJsonArray next,resolution;
        std::set<std::pair<int,int>> scheduled;
        auto remember=[&](const QJsonObject &order,int dueDay) {
            const auto at=factoryPoint(order["tile"]);
            const auto key=std::pair{at.x,at.y};
            if (at==INVALID_TILE || started.contains(key) || !scheduled.insert(key).second) return;
            next.append(QJsonObject{{"unit",order["unit"]},{"tile",order["tile"]},{"firstDueDay",dueDay},
                                    {"rate",order["rate"]},{"ownerBefore",order["ownerBefore"]}});
        };
        for (const auto &v:due)
        {
            auto order=v.toObject();
            const auto at=factoryPoint(order["tile"]);
            const auto who=started.find({at.x,at.y});
            if (who!=started.end())
            {
                order["status"]="STARTED";
                order["startedBy"]=who->second;
            }
            else
            {
                const bool revised=order["revisedTerms"].toBool();
                order["status"]=revised ? "REVISED" : "NOT_STARTED";
                if (!revised) remember(order,order["firstDueDay"].toInt(day));
            }
            resolution.append(order);
        }
        for (const auto &v:policy["captureClaims"].toArray())
        {
            const auto claim=v.toObject();
            if (isNextCaptureStart(claim)) remember(claim,day+1);
        }
        receipt["captureStartResults"]=resolution;
        receipt["nextCaptureStarts"]=next;
        writeDatedOrders(variables,NEXT_CAPTURE_STARTS,day,player,due,next);
    }
}
