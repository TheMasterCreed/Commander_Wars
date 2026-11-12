#pragma once

#include "ai/coordinator/masterassignmentbounds.h"
#include "ai/coordinator/masterassignmentevaluator.h"

namespace Coordinator
{
    constexpr std::int64_t MASTER_ASSIGNMENT_INITIAL_ALLOWANCE = 160000;
    constexpr std::int64_t MASTER_PLAN_PRICING_CHALLENGER_SHARE = 2;
    constexpr std::int64_t MASTER_PLAN_NEIGHBOR_AUDIT_ALLOWANCE = 8000000;

    enum class MasterFutureBoundScope : std::int8_t
    {
        RootOnly,
        EveryNode,
    };

    enum class MasterRefinementAllocation : std::int8_t
    {
        Baseline,
        OperationService,
    };

    enum class MasterPlanBoundCap : std::int8_t
    {
        Off,
        Verified,
    };

    enum class MasterPlanNeighborAudit : std::int8_t
    {
        Off,
        Recorded,
    };

    struct MasterAssignmentSchedule
    {
        std::int32_t currentDualRounds{4};
        std::int64_t continuationSlice{128};
        std::int64_t refinementGrowthFactor{4};
        std::int64_t futureDualAllowance{4096};
        std::uint64_t nodesPerRefinement{8};
        std::uint64_t incumbentRefinementStride{2};
        MasterFutureBoundScope futureBounds{MasterFutureBoundScope::RootOnly};
        bool incumbentCompletions{true};
        bool initialIncumbentAlternatives{false};
        bool initialContributionCompletion{false};
        std::uint64_t challengerRefinementBurst{24};
        MasterRefinementAllocation allocation{MasterRefinementAllocation::Baseline};
        std::int32_t planPricingRounds{0};
        std::int64_t planPricingWork{0};
        std::int64_t planPricingWitnessWork{0};
        MasterPlanBoundCap planBoundCap{MasterPlanBoundCap::Off};
        MasterPlanNeighborAudit planNeighborAudit{MasterPlanNeighborAudit::Off};
        std::int64_t planNeighborAuditAllowance{MASTER_PLAN_NEIGHBOR_AUDIT_ALLOWANCE};
    };

    struct MasterOperationTransaction
    {
        const MasterAssignmentEvaluation & ticket;
        const MasterAssignmentEvaluation* challenger;
        const MasterAssignmentEvaluation* operation;
        MasterRefinementTerm term;
        std::int64_t allowance;
        std::int64_t ticketWork;
        std::int64_t operationWork;
        bool admitted;
        bool completed;
        bool relevantProgress;
        bool oppositeProgress;
    };

    struct MasterRefinementTicketReceipt
    {
        const MasterAssignmentEvaluation & ticket;
        MasterRefinementTerm cursorBefore;
        MasterRefinementTerm cursorAfter;
        std::int64_t aggregateBefore;
        std::int64_t aggregateAfter;
        std::uint64_t refinementsBefore;
        std::uint64_t refinementsAfter;
        std::uint64_t focusBefore;
        std::uint64_t focusAfter;
        bool ticketWasFocus;
        bool workBalancedSelection;
        bool proofBefore;
        bool proofAfter;
        bool completed;
        bool fundingParked{false};
    };

    class MasterAssignmentSearchObserver
    {
    public:
        virtual ~MasterAssignmentSearchObserver() = default;
        virtual void evaluated(const MasterAssignmentEvaluation & evaluation, std::int64_t work) = 0;
        virtual void refined(const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
                             std::int64_t allowance, MasterRefinementStatus status,
                             std::int64_t workBefore, std::int64_t workAfter) = 0;
        virtual void compared(const MasterAssignmentEvaluation & challenger,
                              const RelativeStockComparison & comparison, std::int64_t work) = 0;
        virtual void comparisonObserved(const MasterAssignmentEvaluation &,
                                        const MasterAssignmentEvaluation &,
                                        const RelativeStockComparison &, std::int64_t)
        {
        }
        virtual void guardedCompared(const MasterAssignmentEvaluation &, const MasterAssignmentEvaluation &,
                                     const MasterGuardedComparison &, std::int64_t)
        {
        }
        virtual void ticketStarted(const MasterRefinementTicketReceipt &,
                                   const MasterAssignmentEvaluation &,
                                   const MasterAssignmentEvaluation*, std::int64_t)
        {
        }
        virtual void operationOffered(const MasterAssignmentEvaluation &, const MasterOperationOffer &,
                                      bool, bool, std::int64_t, std::int64_t, std::int64_t)
        {
        }
        virtual void operationStarted(const MasterAssignmentEvaluation &, MasterRefinementTerm,
                                      std::int64_t, std::int64_t)
        {
        }
        virtual void operationExecuted(const MasterAssignmentEvaluation &, MasterRefinementTerm,
                                       const MasterOperationOutcome &, std::int64_t, std::int64_t)
        {
        }
        virtual void operationServiced(const MasterOperationTransaction &)
        {
        }
        virtual void ticketFinished(const MasterRefinementTicketReceipt &)
        {
        }
    };

    struct MasterAssignmentSearchOptions
    {
        MasterAssignmentSchedule schedule;
        bool retainDiagnostics{false};
        MasterAssignmentProfile* profile{nullptr};
        MasterAssignmentSearchObserver* observer{nullptr};
    };

    struct MasterPruneReceipt
    {
        MasterAssignmentState state;
        MilliFunds upper{0};
        MilliFunds feasibleLower{0};
    };

    struct MasterResidualRegion
    {
        MasterAssignmentState state;
        std::optional<MilliFunds> upper;
        bool completeTuple{false};
        bool unsupported{false};
    };

    struct MasterImprovementProof
    {
        MasterAssignmentEvaluation incumbent;
        MasterAssignmentEvaluation challenger;
        RelativeStockComparison comparison;
        std::int64_t work{0};
        std::shared_ptr<const MasterGuardedStockProof> guardedProof;
        std::optional<MilliFunds> incumbentOursCap;
        std::optional<MilliFunds> challengerOursCap;
    };

    enum class MasterPlanPricingOutcome : std::int8_t
    {
        Disabled,
        Unavailable,
        NoChallenger,
        ChallengerInfeasible,
        WitnessDeclined,
        Seeded,
        AdoptionBudgetExhausted,
        ValuerFailure,
    };

    enum class MasterPlanBoundCapStatus : std::int8_t
    {
        NotAttempted,
        Unaffordable,
        ScanInvalid,
        VerifyFailed,
        Contradiction,
        Registered,
        OwnedMismatch,
        CatalogMismatch,
    };

    struct MasterPlanBoundCapStats
    {
        MasterPlanBoundCapStatus status{MasterPlanBoundCapStatus::NotAttempted};
        std::vector<MilliFunds> futurePrices;
        std::optional<MilliFunds> scanUpper;
        std::optional<MilliFunds> oursCeiling;
        std::int64_t work{0};
        std::optional<MilliFunds> incumbentOursUpperBefore;
        bool improvedAtCap{false};
        bool cappedProofDropped{false};
    };

    enum class MasterPlanNeighborAuditStatus : std::int8_t
    {
        Unavailable,
        FloorUnproven,
        CatalogMismatch,
        OwnedMismatch,
        Unrepresentable,
        Unaffordable,
        TuneFailed,
        ScanInvalid,
        WitnessAboveCeiling,
        ArithmeticFailure,
        Recorded,
    };

    enum class MasterNeighborAuditedPlan : std::int8_t
    {
        Incumbent,
        Challenger,
    };

    enum class MasterNeighborVerdict : std::int8_t
    {
        ProvenNotBetter,
        Open,
        Conflicting,
        Unavailable,
    };

    struct MasterNeighborChange
    {
        std::int32_t actorId{0};
        std::int32_t slot{0};
        std::int32_t candidate{0};
        std::optional<MilliFunds> gain;
        bool conflictFree{false};
        MasterNeighborVerdict verdict{MasterNeighborVerdict::Unavailable};
    };

    struct MasterPlanNeighborAuditStats
    {
        MasterPlanNeighborAuditStatus status{MasterPlanNeighborAuditStatus::Unavailable};
        MasterNeighborAuditedPlan plan{MasterNeighborAuditedPlan::Incumbent};
        std::int64_t work{0};
        std::int64_t nanos{0};
        std::int32_t rounds{0};
        std::optional<MilliFunds> scanUpper;
        std::optional<MilliFunds> trustedFloor;
        // Seated economic plus quoted owned plus the tuner's unreplayed friendly witness, in the scan's units.
        std::optional<MilliFunds> witnessValue;
        std::uint64_t provenNotBetter{0};
        std::uint64_t open{0};
        std::uint64_t conflicting{0};
        std::uint64_t unavailable{0};
        std::vector<MasterNeighborChange> changes;
    };

    struct MasterPlanPricingStats
    {
        MasterPlanPricingOutcome outcome{MasterPlanPricingOutcome::Disabled};
        std::int64_t adapterWork{0};
        std::int64_t restrictionWork{0};
        std::int32_t challengerRounds{0};
        std::int64_t challengerWork{0};
        std::int32_t incumbentRounds{0};
        std::int64_t incumbentWork{0};
        std::optional<MilliFunds> adoptedValue;
        bool hintStored{false};
        bool firstComparisonImproved{false};
        std::optional<MasterPlanBoundCapStats> boundCap;
        std::optional<MasterPlanNeighborAuditStats> neighborAudit;
    };

    struct MasterAssignmentSearchStats
    {
        std::uint64_t expanded{0};
        std::uint64_t evaluated{0};
        std::uint64_t infeasible{0};
        std::uint64_t pruned{0};
        std::uint64_t guardedExcluded{0};
        std::uint64_t unsupported{0};
        std::uint64_t arithmeticFailures{0};
        std::uint64_t proofFailures{0};
        std::uint64_t refinements{0};
        std::uint64_t incumbentRefinements{0};
        std::uint64_t cancelledEnemyRefinements{0};
        std::uint64_t workBalancedSelections{0};
        std::uint64_t incumbentCompletions{0};
        std::uint64_t initialIncumbentAlternatives{0};
        std::uint64_t contributionCompletions{0};
        std::uint64_t evaluationCacheHits{0};
        std::uint64_t operationRecords{0};
        std::uint64_t resolvedOperationTickets{0};
        std::uint64_t fundingParkedTickets{0};
        std::int64_t operationPaidWork{0};
        std::int64_t operationScreeningWork{0};
        std::uint64_t peakFrontier{0};
        std::uint64_t peakResidentRegions{0};
        std::uint64_t residualRegions{0};
        std::optional<std::int64_t> firstImprovementWork;
        std::optional<MilliFunds> firstImprovementLower;
        MasterFutureCacheStats futurePricingCache;
        MasterPlanPricingStats planPricing;
        std::size_t retainedGrants{0};
        std::size_t refundableStorageBytes{0};
        RefinementLedgerStats work;
    };

    struct MasterAssignmentSearchResult
    {
        MasterAssignmentSchedule schedule;
        std::shared_ptr<const MasterAssignmentEvaluation> incumbent;
        std::shared_ptr<const MasterAssignmentEvaluation> best;
        std::optional<MasterImprovementProof> improvement;
        std::optional<MasterRefinementRefusal> refinementRefusal;
        std::optional<MasterOperationStatus> operationStop;
        std::optional<MilliFunds> lower;
        std::optional<MilliFunds> residualUpper;
        std::optional<MilliFunds> upper;
        std::optional<MilliFunds> gap;
        bool fullDomain{true};
        bool implicitRoot{false};
        bool domainAccounted{true};
        bool globalOptimal{false};
        bool incumbentPreserved{true};
        bool canonicalRepresentative{false};
        bool noFeasibleAssignment{false};
        bool budgetExhausted{false};
        bool invalidInput{false};
        std::vector<MasterPruneReceipt> prunes;
        std::vector<MasterResidualRegion> residual;
        MasterAssignmentSearchStats stats;
    };

    MasterAssignmentSearchResult searchMasterAssignment(
        const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator,
        RefinementLedger & ledger,
        const MasterAssignmentSearchOptions & options = {});
}
