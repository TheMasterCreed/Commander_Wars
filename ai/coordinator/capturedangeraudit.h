#pragma once

#include <span>

#include "ai/coordinator/attackopportunityfield.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/planidentity.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/turnplan.h"

class GameMap;

namespace Coordinator
{
    struct CaptureDangerAuditView
    {
        GameMap & map;
        const BattlefieldKnowledge & knowledge;
        const AttackOpportunityField & enemyReach;
        std::span<const PropertyFacts> properties;
        const PropertyStockField & propertyStock;
        std::span<const AssignmentActor> actors;
        std::span<const KnownUnitLink> unitLinks;
        const TurnPlan & plan;
        const DecisionTraceIdentity & identity;
        ReplyPredictionTelemetry reply{};
    };

    // Change pricing never orders a vacate, so a follower is never offered as a replacement.
    inline std::span<const CandidateBundle> replacementAlternatives(const AssignmentActor * pActor,
                                                                    const CandidateBundle * pInstalled)
    {
        if (pActor == nullptr || pInstalled == nullptr)
        {
            return {};
        }
        return todayCandidateSpan(*pActor);
    }

    // Measure only: nothing it computes reaches the planner and it owns its damage memo, but EXACT calls run CO scripts that can append blank variables.
    void recordCaptureDangerAudit(const CaptureDangerAuditView & view);
}
