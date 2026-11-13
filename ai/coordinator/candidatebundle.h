#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ai/coordinator/bundlevaluation.h"

namespace Coordinator
{
    struct ReplyFacts;

    constexpr std::int32_t NO_CANDIDATE = -1;

    struct CandidateBundle
    {
        std::int32_t generationIndex{-1};
        ActionBundle bundle;
        ActorFacts actor;
        std::int32_t movementCost{0};
        bool vacatesActiveFriendlyProduction{false};
        std::vector<MilliFunds> actorNextShotsAtOrigin;
        std::vector<MilliFunds> actorNextShotsAtDestination;
        std::vector<MilliFunds> enemyShotsOnActorAtOrigin;
        std::vector<MilliFunds> enemyShotsOnActorAtDestination;
        BundleValuation valuation;
        // Null unless reply prediction is recorded; nothing prices it.
        std::shared_ptr<const ReplyFacts> replyFacts;
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
