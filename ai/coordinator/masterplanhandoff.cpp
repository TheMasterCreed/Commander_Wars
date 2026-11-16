#include "ai/coordinator/masterplanhandoff.h"

#include <algorithm>
#include <utility>

#include "ai/coordinator/bundleassignment.h"

namespace Coordinator
{
    MasterPlanHandoffResult handOffMasterPlan(const MasterAssignmentProblem* pProblem,
                                              const AssignmentResult & assignment)
    {
        MasterPlanHandoffResult result;
        if (pProblem == nullptr)
        {
            result.status = MasterPlanHandoffStatus::NoProblem;
            return result;
        }
        const auto actors = pProblem->actors();
        result.status = MasterPlanHandoffStatus::SelectionMismatch;
        if (assignment.selections.size() != actors.size())
        {
            return result;
        }
        std::vector<std::int32_t> choices(actors.size(), NO_CANDIDATE);
        for (const MasterActor & actor : actors)
        {
            const auto selected = std::find_if(assignment.selections.begin(), assignment.selections.end(),
                [&](const AssignmentResult::Selection & entry) { return entry.engineUnitId == actor.engineUnitId; });
            if (selected == assignment.selections.end() || actor.canonicalSlot < 0 ||
                static_cast<std::size_t>(actor.canonicalSlot) >= choices.size())
            {
                return result;
            }
            choices[static_cast<std::size_t>(actor.canonicalSlot)] = selected->candidateIndex;
        }
        const auto replay = replayMasterAssignment(*pProblem, choices, ReplyPredictionSetup{});
        if (!replay.complete())
        {
            result.status = MasterPlanHandoffStatus::ReplayFailed;
            return result;
        }
        if (!masterPlanEquivalent(*pProblem, replay.plan, replay.ordering.order, assignment.plan,
                                  assignment.executionOrder))
        {
            result.status = MasterPlanHandoffStatus::PlanMismatch;
            return result;
        }
        result.status = MasterPlanHandoffStatus::Ready;
        result.handoff = MasterPlanHandoff{*pProblem, std::move(choices)};
        return result;
    }
}
