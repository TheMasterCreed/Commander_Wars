#pragma once

#include <array>
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
    constexpr std::int32_t MASTER_PLAN_PRICING_ROUNDS = 200;
    constexpr std::int64_t MASTER_PLAN_PRICING_WORK = 8000000;
    constexpr std::int64_t MASTER_PLAN_PRICING_WITNESS_WORK = 250000;
    constexpr std::int32_t MASTER_PLAN_WITNESS_INTERVAL = 10;
    constexpr std::int32_t MASTER_PLAN_POLISH_PASSES = 8;
    constexpr MilliFunds MASTER_PLAN_PRICE_LIMIT = 1'000'000'000'000'000;

    struct MasterPlanPricingCandidate
    {
        std::int32_t candidateIndex{0};
        CanonicalPlanActionKey key{CanonicalPlanActionKey::fromAction(NO_STOCK_ROW, 0, 0, false)};
        std::vector<MasterResourceUsage> resources;
        MilliFunds immediateValue{0};
        std::int32_t capturedNode{NO_SEQUENTIAL_NODE};
        SequentialRow row;
        std::int32_t actionIndex{0};
        bool follows{false};
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

    inline constexpr std::array MASTER_PLAN_PRICING_STATUS_NAMES{
        "VALID", "UNAVAILABLE", "LIMIT_REACHED", "ARITHMETIC_FAILURE", "INVALID", "INCONSISTENT",
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
        std::int64_t tieSwitches{0};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::int64_t skipViolations{0};
#endif
        std::int64_t states{0};
        std::int64_t pickSearchWork{0};
        std::int64_t polishSearchWork{0};
    };

    struct MasterPlanPricingLimits
    {
        std::int32_t rounds{0};
        std::int64_t maximumWork{0};
        std::int64_t witnessBuildWork{0};
        // Zero keeps the exact-match stop; a known floor only enters the stop test.
        MilliFunds targetGap{0};
        std::optional<MilliFunds> knownFloor;
    };

    struct MasterPlanPricingResult
    {
        MasterPlanPricingStatus status{MasterPlanPricingStatus::Unavailable};
        std::vector<MilliFunds> prices;
        MasterPlanWitness witness;
        std::int32_t rounds{0};
        bool stoppedOnTarget{false};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        MilliFunds ceiling{0};
        std::optional<MilliFunds> floor;
        std::int64_t pricingWork{0};
        std::int64_t witnessWork{0};
        MasterPlanWitnessStats witnessStats;
#endif
    };

    struct MasterPlanPolishResult
    {
        MasterPlanPricingStatus status{MasterPlanPricingStatus::Unavailable};
        MasterPlanWitness witness;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
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

    namespace MasterPlanPricingDetail
    {
        // Callers pass a captured node that is NO_SEQUENTIAL_NODE or indexes prices.
        bool addMasterPlanTerms(MilliFunds & total, std::span<const MilliFunds> terms);
        std::optional<MilliFunds> masterPlanCandidateValue(MilliFunds immediateValue, std::int32_t capturedNode,
                                                           MilliFunds futureUpper, std::span<const MilliFunds> prices);
    }

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
                                                 MasterPlanWitnessOptions options = {},
                                                 std::span<const MilliFunds> seed = {});
    MasterPlanPolishResult polishMasterPlanWitness(const MasterPlanPricingModel & model,
                                                   const MasterPlanWitness & start,
                                                   std::span<const std::int32_t> rowOrder, std::int64_t maximumWork,
                                                   RefinementLedger & ledger, MasterPlanWitnessOptions options = {});
}
