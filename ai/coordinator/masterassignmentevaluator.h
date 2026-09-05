#pragma once

#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/relativestocksession.h"
#include "ai/coordinator/masterassignmentprofile.h"

namespace Coordinator
{
    enum class MasterEvaluationStatus : std::int8_t
    {
        Feasible,
        Infeasible,
        Unsupported,
        Invalid,
        ArithmeticFailure,
        BudgetExhausted,
    };

    enum class MasterObjectiveStatus : std::int8_t
    {
        Unavailable,
        Verified,
        InvalidIdentity,
        WitnessFailure,
        ArithmeticFailure,
        BudgetExhausted,
    };

    struct MasterAssignmentEvaluation
    {
        MasterAssignmentState state;
        MasterEvaluationStatus status{MasterEvaluationStatus::Invalid};
        MasterObjectiveStatus objectiveStatus{MasterObjectiveStatus::Unavailable};
        std::optional<MasterAssignmentReplayResult> replay;
        std::shared_ptr<const RelativeStockProofSnapshot> proof;
        std::optional<StockInterval> objective;
        bool lowerReplayed{false};
        bool upperVerified{false};
        bool exact{false};

        const RelativeStockQuote* quote() const
        {
            return proof ? &proof->quote : nullptr;
        }
    };

    struct MasterValuationTariffs
    {
        std::int64_t quote{1};
        std::int64_t witness{1};
        std::int64_t certificateIdentity{1};
    };

    enum class MasterRefinementTerm : std::int8_t
    {
        Friendly,
        Enemy,
        FutureDual,
    };

    enum class MasterRefinementStatus : std::int8_t
    {
        Updated,
        Unavailable,
        Invalid,
        BudgetExhausted,
    };

    class MasterAssignmentEvaluator final
    {
    public:
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  MasterAssignmentProfile* profile = nullptr);
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  RelativeStockSession & session, MasterValuationTariffs tariffs,
                                  MasterAssignmentProfile* profile = nullptr);

        MasterAssignmentEvaluation evaluate(const MasterAssignmentState & state);
        bool refresh(MasterAssignmentEvaluation & evaluation);
        MasterRefinementStatus refine(MasterAssignmentEvaluation & evaluation,
                                      MasterRefinementTerm term, std::int64_t allowance);
        RelativeStockComparison compare(MasterAssignmentEvaluation & incumbent,
                                        MasterAssignmentEvaluation & challenger);
        bool hasContinuation() const;
        const CheapPricingModel* stockModel() const;
        const MasterFutureCatalog* futureCatalog() const;
        bool matches(const MasterAssignmentProblem & problem, const RefinementLedger & ledger) const;

    private:
        bool price(MasterAssignmentEvaluation & evaluation);
        std::optional<std::int64_t> replayTariff(const MasterAssignmentState & state);

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentIdentity m_problemIdentity;
        RefinementLedger & m_ledger;
        RelativeStockSession* m_session{nullptr};
        MasterValuationTariffs m_tariffs;
        MasterAssignmentProfile* m_profile;
    };
}
