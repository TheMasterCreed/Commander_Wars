#pragma once

#include "ai/coordinator/combinedfollowup.h"
#include "ai/coordinator/rankedpairproof.h"

namespace Coordinator
{
    struct RequestedFollowup
    {
        int actor{NO_UNIT}, vehicle{NO_UNIT}, target{NO_UNIT}, weapon{-1};
        TilePoint end{INVALID_TILE};
        struct Adverse
        {
            int shooter{NO_UNIT}, weapon{-1};
            TilePoint end{INVALID_TILE};
        };
        std::optional<Adverse> answer;
    };

    // Absolute upper on the isolated immediate, signed material contribution
    // of this particular optional F1 task. The prior volley is already paid.
    // The hostile side can decline further attacks, so a negative F1 exchange
    // is never a positive guaranteed material gain for this isolated task.
    inline std::optional<MilliFunds> upperOptionalFollowupMaterial(const FollowupFrontier &frontier)
    {
        if (!frontier.friendlyComplete || frontier.workExhausted) return {};
        MilliFunds upper=0;
        for (const auto &shot : frontier.shots) upper=std::max(upper,shot.net);
        return upper;
    }

}
