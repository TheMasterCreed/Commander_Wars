#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/masterassignmentfuture.h"

namespace Coordinator
{
    constexpr std::int32_t MASTER_PLAN_UNRESTRICTED = -1;
    constexpr std::int32_t MASTER_PLAN_NO_ACTOR = -1;

    struct MasterPlanPricingCandidate
    {
        std::int32_t candidateIndex{0};
        CanonicalPlanActionKey key{CanonicalPlanActionKey::fromAction(NO_STOCK_ROW, 0, 0, false)};
        std::vector<MasterResourceUsage> resources;
        MilliFunds immediateValue{0};
        std::int32_t capturedNode{NO_SEQUENTIAL_NODE};
        SequentialRow row;
        std::int32_t actionIndex{0};
    };

    struct MasterPlanPricingActor
    {
        std::int32_t row{0};
        std::vector<MasterPlanPricingCandidate> candidates;
    };

    struct MasterPlanPricingModel
    {
        std::vector<MasterPlanPricingActor> actors;
        std::vector<MasterResourceUsage> occupiedResources;
        std::vector<std::int32_t> rowActors;
        MasterFutureCatalog catalog;
    };

    enum class MasterPlanPricingStatus : std::int8_t
    {
        Valid,
        Unavailable,
        LimitReached,
        ArithmeticFailure,
        Invalid,
        Inconsistent,
    };

    struct MasterPlanCeiling
    {
        MasterPlanPricingStatus status{MasterPlanPricingStatus::Unavailable};
        MilliFunds ceiling{0};
        MasterFuturePricing pricing;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::vector<std::int32_t> choicePositions;
#endif
        std::vector<MilliFunds> actorMaxima;
        std::vector<std::int32_t> nodeDemand;
    };

    struct MasterPlanWitness
    {
        bool valid{false};
        std::vector<std::int32_t> candidateIndices;
        std::vector<SequentialRowWitness> rows;
        MilliFunds value{0};
    };

    struct MasterPlanWitnessStats
    {
        std::int64_t builds{0};
        std::int64_t abandonedBuilds{0};
        std::int64_t responses{0};
        std::int64_t skippedResponses{0};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::int64_t skipViolations{0};
#endif
        std::int64_t states{0};
        std::int64_t tables{0};
        std::int64_t tableWork{0};
        std::int64_t pickSearchWork{0};
        std::int64_t polishSearchWork{0};
    };

    struct MasterPlanPricingLimits
    {
        std::int32_t rounds{0};
        std::int64_t maximumWork{0};
        std::int64_t witnessBuildWork{0};
    };

    struct MasterPlanPricingResult
    {
        MasterPlanPricingStatus status{MasterPlanPricingStatus::Unavailable};
        std::vector<MilliFunds> prices;
        MasterPlanWitness witness;
        std::int32_t rounds{0};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        MilliFunds ceiling{0};
        std::optional<MilliFunds> floor;
        std::int64_t pricingWork{0};
        std::int64_t witnessWork{0};
        MasterPlanWitnessStats witnessStats;
#endif
    };

    enum class MasterPlanSearchMode : std::int8_t
    {
        Pruned,
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        Exhaustive,
#endif
    };

    struct MasterPlanWitnessOptions
    {
        MasterPlanSearchMode mode{MasterPlanSearchMode::Pruned};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        bool auditSkips{false};
#endif
    };

    void removeCapturedNode(SequentialRow & row, std::int32_t node);
    std::map<std::int32_t, std::int64_t> masterPlanOccupiedUsage(const MasterPlanPricingModel & model);
    bool indexMasterPlanActions(MasterPlanPricingModel & model);
    std::optional<MasterPlanPricingModel> restrictMasterPlanModel(
        const MasterPlanPricingModel & model, std::span<const std::int32_t> forced);
    const MasterPlanPricingCandidate* findMasterPlanCandidate(const MasterPlanPricingActor & actor,
                                                             std::int32_t candidateIndex);
    MasterPlanCeiling priceMasterPlan(const MasterPlanPricingModel & model, std::span<const MilliFunds> prices,
                                      RefinementLedger & ledger);
    MasterPlanPricingResult tuneMasterPlanPrices(const MasterPlanPricingModel & model,
                                                 const MasterPlanPricingLimits & limits,
                                                 RefinementLedger & ledger,
                                                 MasterPlanWitnessOptions options = {});
}
