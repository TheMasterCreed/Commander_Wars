#pragma once

#include <cstdint>
#include <vector>

#include "ai/coordinator/bundlevaluation.h"

namespace Coordinator
{
    struct CandidateBundle
    {
        ActionBundle bundle;
        ActorFacts actor;
        std::int32_t movementCost{0};
        std::vector<MilliFunds> actorNextShotsAtOrigin;
        std::vector<MilliFunds> actorNextShotsAtDestination;
        std::vector<MilliFunds> enemyShotsOnActorAtOrigin;
        std::vector<MilliFunds> enemyShotsOnActorAtDestination;
        BundleValuation valuation;
    };

    inline PositionFacts positionFacts(const CandidateBundle & candidate)
    {
        return PositionFacts{
            .actorNextShotsAtOrigin = candidate.actorNextShotsAtOrigin,
            .actorNextShotsAtDestination = candidate.actorNextShotsAtDestination,
            .enemyShotsOnActorAtOrigin = candidate.enemyShotsOnActorAtOrigin,
            .enemyShotsOnActorAtDestination = candidate.enemyShotsOnActorAtDestination,
        };
    }
}
