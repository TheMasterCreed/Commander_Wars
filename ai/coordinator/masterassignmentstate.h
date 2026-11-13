#pragma once

#include <cstdint>
#include <limits>
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

    inline std::optional<std::int64_t> masterWorkProduct(
        std::size_t count, std::size_t units)
    {
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        if (count > static_cast<std::uint64_t>(maximum) ||
            units > static_cast<std::uint64_t>(maximum) ||
            (units != 0 && count > static_cast<std::uint64_t>(maximum) / units))
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(count) * static_cast<std::int64_t>(units);
    }

    inline bool chargeMasterWork(RefinementLedger & ledger, std::size_t count,
                                 std::size_t units, RefinementWork category)
    {
        const auto work = masterWorkProduct(count, units);
        return work && ledger.consume(*work, category);
    }
}
