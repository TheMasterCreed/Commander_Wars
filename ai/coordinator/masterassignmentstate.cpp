#include "ai/coordinator/masterassignmentstate.h"

#include <algorithm>
#include <limits>

namespace Coordinator
{
    bool MasterAssignmentState::complete() const
    {
        return std::find(choices.begin(), choices.end(), MASTER_UNASSIGNED) == choices.end();
    }

    std::optional<std::int64_t> masterWorkProduct(
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

    bool chargeMasterWork(RefinementLedger & ledger, std::size_t count,
                          std::size_t units, RefinementWork category)
    {
        const auto work = masterWorkProduct(count, units);
        return work && ledger.consume(*work, category);
    }

    std::optional<MasterAssignmentState> masterRootState(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger)
    {
        if (!chargeMasterWork(ledger, problem.actors().size() + 1, 1,
                              RefinementWork::MasterPreparation))
        {
            return std::nullopt;
        }
        MasterAssignmentState state;
        state.identity = problem.identity();
        for (const auto & actor : problem.actors())
        {
            state.choices.push_back(problem.canChangeActor(actor.canonicalSlot)
                                        ? MASTER_UNASSIGNED : actor.incumbentCandidateIndex);
        }
        return state;
    }

    bool validMasterState(const MasterAssignmentProblem & problem,
                          const MasterAssignmentState & state)
    {
        if (!state.identity.sameLiveProblem(problem.identity()) ||
            state.choices.size() != problem.actors().size())
        {
            return false;
        }
        for (const auto & actor : problem.actors())
        {
            const auto choice = state.choices[static_cast<std::size_t>(actor.canonicalSlot)];
            if (!problem.canChangeActor(actor.canonicalSlot))
            {
                if (choice != actor.incumbentCandidateIndex)
                {
                    return false;
                }
            }
            else if (choice != MASTER_UNASSIGNED &&
                     (choice < 0 || choice >= static_cast<std::int32_t>(actor.candidates.size())))
            {
                return false;
            }
        }
        return true;
    }
}
