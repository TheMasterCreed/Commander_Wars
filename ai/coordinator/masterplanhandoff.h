#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ai/coordinator/masterassignmentproblem.h"

namespace Coordinator
{
    struct AssignmentResult;

    enum class MasterPlanHandoffStatus : std::int8_t
    {
        NotRequested,
        NoProblem,
        SelectionMismatch,
        ReplayFailed,
        PlanMismatch,
        Ready,
    };

    inline constexpr std::array MASTER_PLAN_HANDOFF_STATUS_NAMES{
        "NOT_REQUESTED", "NO_PROBLEM", "SELECTION_MISMATCH", "REPLAY_FAILED", "PLAN_MISMATCH", "READY",
    };

    struct MasterOrderProposal
    {
        std::vector<std::int32_t> choices;
        std::vector<std::int32_t> executionOrder;
    };

    struct MasterPlanHandoff
    {
        MasterAssignmentProblem problem;
        std::vector<std::int32_t> choices;
        std::optional<MasterOrderProposal> stockProposal{};
    };

    struct MasterPlanHandoffResult
    {
        MasterPlanHandoffStatus status{MasterPlanHandoffStatus::NotRequested};
        std::optional<MasterPlanHandoff> handoff;
        std::int64_t nanos{0};
    };

    // Hands off only when the selections, replayed with prediction off, give back assignment's plan and order.
    MasterPlanHandoffResult handOffMasterPlan(const MasterAssignmentProblem* pProblem,
                                              const AssignmentResult & assignment);
}
