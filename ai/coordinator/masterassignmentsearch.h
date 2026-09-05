#pragma once

#include "ai/coordinator/masterassignmentbounds.h"
#include "ai/coordinator/masterassignmentevaluator.h"

namespace Coordinator
{
    constexpr std::int64_t MASTER_ASSIGNMENT_INITIAL_ALLOWANCE = 160000;

    struct MasterAssignmentSchedule
    {
        std::int32_t currentDualRounds{4};
        std::int64_t continuationSlice{128};
        std::int64_t futureDualAllowance{4096};
        std::uint64_t nodesPerRefinement{8};
    };

    struct MasterAssignmentSearchOptions
    {
        MasterAssignmentSchedule schedule;
        bool retainDiagnostics{false};
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
    };

    struct MasterAssignmentSearchStats
    {
        std::uint64_t expanded{0};
        std::uint64_t evaluated{0};
        std::uint64_t infeasible{0};
        std::uint64_t pruned{0};
        std::uint64_t unsupported{0};
        std::uint64_t arithmeticFailures{0};
        std::uint64_t proofFailures{0};
        std::uint64_t refinements{0};
        std::uint64_t peakFrontier{0};
        std::uint64_t peakResidentRegions{0};
        std::uint64_t residualRegions{0};
        std::optional<std::int64_t> firstImprovementWork;
        RefinementLedgerStats work;
    };

    struct MasterAssignmentSearchResult
    {
        MasterAssignmentSchedule schedule;
        std::shared_ptr<const MasterAssignmentEvaluation> incumbent;
        std::shared_ptr<const MasterAssignmentEvaluation> best;
        std::optional<MasterImprovementProof> improvement;
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
