#include "ai/coordinator/masterassignmentcapture.h"

#include <bit>

#include "ai/coordinator/bundleassignment.h"

namespace Coordinator
{
    MasterCaptureResult captureMasterAssignment(
        const AssignmentInput & input, const AssignmentResult & incumbent, std::uint64_t sessionIdentity,
        RefinementLedger & ledger, MasterAssignmentProfile* profile)
    {
        MasterProfileScope timing(profile, MasterPhase::Extraction);
        MasterCaptureResult result;
        const auto before = ledger.stats().spent();
        std::int64_t payload = 0;
        for (const auto & actor : input.actors)
        {
            if (!chargeMasterWork(ledger, actor.candidates.size() + 1, 1, RefinementWork::MasterPreparation))
            {
                result.budgetExhausted = true;
                result.workUsed = ledger.stats().spent() - before;
                return result;
            }
            for (const auto & candidate : actor.candidates)
            {
                const auto size = masterWorkProduct(candidate.bundle.components.size() + candidate.bundle.path.size() + 1, 1);
                if (!size || !RelativeStockDetail::checkedAdd(payload, *size))
                {
                    result.arithmeticFailure = true;
                    result.workUsed = ledger.stats().spent() - before;
                    return result;
                }
            }
        }
        const auto graph = input.actors.size() + input.unitLinks.size() + incumbent.selections.size() +
                           static_cast<std::size_t>(incumbent.plan.actionCount()) +
                           static_cast<std::size_t>(incumbent.plan.edgeCount()) + 1;
        const auto structural = masterWorkProduct(graph, graph);
        const auto alternatives = masterWorkProduct(static_cast<std::size_t>(payload),
            graph + std::bit_width(static_cast<std::uint64_t>(payload)) + 1);
        auto tariff = structural.value_or(0);
        if (!structural || !alternatives || !RelativeStockDetail::checkedAdd(tariff, *alternatives))
        {
            result.arithmeticFailure = true;
        }
        else if (ledger.draw(tariff, RefinementWork::MasterPreparation) == NO_GRANT)
        {
            result.budgetExhausted = true;
        }
        else
        {
            result.extraction = buildMasterAssignmentProblem(input, incumbent, sessionIdentity);
        }
        result.workUsed = ledger.stats().spent() - before;
        return result;
    }
}
