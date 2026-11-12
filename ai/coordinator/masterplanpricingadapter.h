#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ai/coordinator/continuationpricing.h"
#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/masterplanpricing.h"

namespace Coordinator
{
    struct MasterPlanPricingInstance
    {
        MasterPlanPricingModel model;
        std::vector<std::int32_t> actorSlots;
        std::int64_t work{0};
    };

    struct MasterPlanFriendlyWitness
    {
        std::vector<SequentialRowWitness> rows;
        MilliFunds value{0};
    };

    std::optional<MasterPlanPricingInstance> buildMasterPlanPricingInstance(
        const MasterAssignmentProblem & problem, const CheapPricingModel & stock,
        const MasterFutureCatalog & catalog, RefinementLedger & ledger, std::int64_t reserve = 0);
    std::optional<std::int64_t> masterPlanRestrictionWork(const MasterPlanPricingModel & model);
    std::optional<MasterAssignmentState> masterPlanWitnessState(
        const MasterAssignmentProblem & problem, const MasterPlanPricingInstance & instance,
        const MasterPlanWitness & witness);
    std::optional<MasterPlanFriendlyWitness> masterPlanFriendlyWitness(const MasterPlanWitness & witness);
    std::optional<std::int64_t> masterPlanAdapterWork(const MasterAssignmentProblem & problem,
                                                      const MasterFutureCatalog & catalog);
    std::optional<std::vector<std::int32_t>> masterPlanStateChoices(
        const MasterAssignmentProblem & problem, const MasterPlanPricingInstance & instance,
        const MasterAssignmentState & state);
}
