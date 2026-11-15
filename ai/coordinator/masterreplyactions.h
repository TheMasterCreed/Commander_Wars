#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/replyprediction.h"

namespace Coordinator
{
    // actorSlots runs parallel to actions.
    struct MasterReplyActions
    {
        std::vector<PlanReplyAction> actions;
        std::vector<std::size_t> actorSlots;
    };

    // The replay's live actions in execution order; empty when one has no actor with a chosen candidate.
    inline std::optional<MasterReplyActions> masterReplyActions(std::span<const MasterActor> actors,
                                                                std::span<const std::int32_t> choices,
                                                                const MasterAssignmentReplayResult & replay)
    {
        MasterReplyActions result;
        result.actions.reserve(replay.ordering.order.size());
        result.actorSlots.reserve(replay.ordering.order.size());
        for (const std::int32_t actionIndex : replay.ordering.order)
        {
            const auto found = std::find(replay.actionIndices.begin(), replay.actionIndices.end(), actionIndex);
            const auto slot = static_cast<std::size_t>(found - replay.actionIndices.begin());
            if (found == replay.actionIndices.end() || choices[slot] == NO_CANDIDATE)
            {
                return std::nullopt;
            }
            const MasterActor & actor = actors[slot];
            result.actions.push_back(PlanReplyAction{
                .pCandidate = &actor.candidates[static_cast<std::size_t>(choices[slot])].source,
                .engineUnitId = actor.engineUnitId,
                .actionIndex = actionIndex,
            });
            result.actorSlots.push_back(slot);
        }
        return result;
    }
}
