#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/propertystocksequential.h"

namespace MasterAssignmentOracle
{
    using namespace Coordinator;

    enum class TupleStatus : std::int8_t
    {
        Feasible,
        Infeasible,
        Unsupported,
        InvalidInput,
        ArithmeticFailure,
    };

    enum class ResultStatus : std::int8_t
    {
        ExactOptimum,
        NoFeasibleAssignment,
        Incomplete,
        Unsupported,
        NonExactObjective,
        InvalidInput,
        ArithmeticFailure,
        ReconstructionMismatch,
    };

    enum class ObjectiveStatus : std::int8_t
    {
        Exact,
        Unavailable,
        Incomplete,
        InvalidInput,
        ArithmeticFailure,
        WitnessFailure,
    };

    enum class ContinuationMode : std::int8_t
    {
        None,
        Unavailable,
        Sequential,
    };

    enum class SearchClaimCheck : std::int8_t
    {
        Consistent,
        NoExactReference,
        InvalidInterval,
        LowerAboveOptimum,
        UpperBelowOptimum,
        CompletedValueMismatch,
    };

    struct ReferencePlan
    {
        TupleStatus status{TupleStatus::InvalidInput};
        bool representationValid{false};
        bool complete{false};
        QString reason;
        TurnPlan plan;
        OrderingResult ordering;
        std::vector<std::int32_t> actionIndices;
        std::vector<MilliFunds> seatedValues;
        MilliFunds seatedTotal{0};
        ContinuationKey key;
    };

    struct SequentialClassInput
    {
        std::int32_t captureTurns{NO_CAPTURE_TURNS};
        std::vector<std::int32_t> interArrivals;
    };

    struct SequentialInput
    {
        std::int32_t horizon{DEFAULT_ECONOMIC_RELEVANCE_HORIZON};
        std::vector<PropertyStockColumn> columns;
        std::vector<SequentialClassInput> classes;
        std::vector<SequentialRow> rows;
        std::vector<bool> capturedNodes;
    };

    struct SequentialExactResult
    {
        ObjectiveStatus status{ObjectiveStatus::InvalidInput};
        MilliFunds value{0};
        std::uint64_t work{0};
        std::uint64_t completePackings{0};
        std::vector<SequentialRowWitness> witness;
    };

    struct ContinuationCase
    {
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        MilliFunds owned{0};
        SequentialInput friendly;
        SequentialInput enemy;
    };

    struct ContinuationInput
    {
        ContinuationMode mode{ContinuationMode::None};
        QString immutableReference;
        std::vector<ContinuationCase> cases;
    };

    struct Objective
    {
        ObjectiveStatus status{ObjectiveStatus::Unavailable};
        MilliFunds seated{0};
        MilliFunds owned{0};
        MilliFunds friendly{0};
        MilliFunds enemy{0};
        MilliFunds total{0};
        std::vector<std::int32_t> capturedColumns;
        SequentialExactResult friendlyProof;
        SequentialExactResult enemyProof;
    };

    struct Request
    {
        QString sourceRevision;
        QString fixture;
        std::uint64_t seed{0};
        std::vector<std::vector<std::int32_t>> domain;
        QString restriction;
        ContinuationInput continuation;
        std::uint64_t tupleLimit{100000};
        std::uint64_t continuationLimit{1000000};
        std::function<bool()> interrupted;
    };

    struct TupleReceipt
    {
        std::vector<std::int32_t> choices;
        ReferencePlan reference;
        Objective objective;
        bool productionValid{false};
        bool productionComplete{false};
        MasterAssignmentReplayError productionError{MasterAssignmentReplayError::None};
        bool comparisonApplicable{false};
        bool comparisonEqual{false};
    };

    struct Result
    {
        ResultStatus status{ResultStatus::InvalidInput};
        QString reason;
        std::vector<std::vector<std::int32_t>> domain;
        std::optional<std::uint64_t> fullTupleProduct;
        std::optional<std::uint64_t> tupleProduct;
        std::uint64_t evaluated{0};
        std::uint64_t feasible{0};
        std::uint64_t infeasible{0};
        std::uint64_t unsupported{0};
        std::uint64_t unresolved{0};
        std::uint64_t falsePositives{0};
        std::uint64_t falseNegatives{0};
        std::uint64_t reconstructionMismatches{0};
        std::uint64_t continuationWork{0};
        bool enumerationComplete{false};
        bool reducedDomain{false};
        bool incumbentRetained{false};
        std::optional<MilliFunds> exactOptimum;
        std::optional<std::size_t> representative;
        std::uint64_t elapsedNanos{0};
        std::vector<TupleReceipt> tuples;
    };

    ReferencePlan constructReference(
        const MasterAssignmentProblem & problem, std::span<const std::int32_t> choices);
    std::optional<std::uint64_t> tupleProduct(
        const std::vector<std::vector<std::int32_t>> & domain);
    std::vector<std::vector<std::int32_t>> fullDomain(const MasterAssignmentProblem & problem);
    SequentialExactResult exhaustSequential(const SequentialInput & input, std::uint64_t workLimit);
    Objective evaluateObjective(const ReferencePlan & plan, const ContinuationInput & input,
                                std::uint64_t workLimit);
    Result enumerate(const MasterAssignmentProblem & problem, const Request & request);
    SearchClaimCheck checkSearchClaim(const Result & exact, MilliFunds lower, MilliFunds upper,
                                     std::optional<MilliFunds> completed = std::nullopt);
    bool retainsOptimum(const Result & exact, std::span<const std::vector<std::int32_t>> remainingTuples);
    QByteArray exportDiagnostic(const MasterAssignmentProblem & problem, const Request & request,
                                const Result & result);
}
