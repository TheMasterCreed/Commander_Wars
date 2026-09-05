#pragma once

#include <optional>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/refinementledger.h"

namespace Coordinator
{
    constexpr std::int32_t MASTER_UNASSIGNED = -2;

    struct MasterAssignmentState
    {
        MasterAssignmentIdentity identity;
        std::vector<std::int32_t> choices;

        bool complete() const;
    };

    std::optional<MasterAssignmentState> masterRootState(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger);

    bool validMasterState(const MasterAssignmentProblem & problem,
                          const MasterAssignmentState & state);

    std::optional<std::int64_t> masterWorkProduct(
        std::size_t count, std::size_t units);

    bool chargeMasterWork(RefinementLedger & ledger, std::size_t count,
                          std::size_t units, RefinementWork category);
}
