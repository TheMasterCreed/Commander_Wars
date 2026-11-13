#include "ai/coordinator/masterassignmentstate.h"

#include <algorithm>

namespace Coordinator
{
    bool MasterAssignmentState::complete() const
    {
        return std::find(choices.begin(), choices.end(), MASTER_UNASSIGNED) == choices.end();
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
