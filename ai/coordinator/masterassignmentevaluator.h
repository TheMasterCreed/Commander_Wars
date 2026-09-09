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

    struct MasterRefinementRefusal
    {
        MasterRefinementTerm term;
        std::int64_t requested;
        std::int64_t balance;
        std::int64_t allowance;
        std::int64_t preparation;

        bool operator==(const MasterRefinementRefusal &) const = default;
    };

    class MasterAssignmentEvaluator final
    {
    public:
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  MasterAssignmentProfile* profile = nullptr);
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  RelativeStockSession & session, MasterValuationTariffs tariffs,
                                  MasterAssignmentProfile* profile = nullptr);

        std::optional<std::int64_t> finalizationWork();
        MasterAssignmentEvaluation evaluate(const MasterAssignmentState & state);
        bool refresh(MasterAssignmentEvaluation & evaluation);
        MasterRefinementStatus refine(MasterAssignmentEvaluation & evaluation,
                                      MasterRefinementTerm term, std::int64_t allowance);
        RelativeStockComparison compare(MasterAssignmentEvaluation & incumbent,
                                        MasterAssignmentEvaluation & challenger);
        // Owned-snapshot scheduling eligibility does not authorize a fresh proof.
        bool unresolvedCommonEnemy(const MasterAssignmentEvaluation & incumbent,
                                   const MasterAssignmentEvaluation & challenger);
        bool hasContinuation() const;
        const CheapPricingModel* stockModel() const;
        const MasterFutureCatalog* futureCatalog() const;
        bool matches(const MasterAssignmentProblem & problem, const RefinementLedger & ledger) const;
        bool ownsRetainedEvaluation(const MasterAssignmentEvaluation & evaluation) const;
        MasterObjectiveStatus replayRetainedEvaluation(const MasterAssignmentEvaluation & evaluation);
        const std::optional<MasterRefinementRefusal> & refinementRefusal() const;
        const std::optional<RelativeStockCertificatePreparation> & certificatePreparation() const;

    private:
        static RelativeStockComparison includeSeatedEconomics(
            RelativeStockComparison comparison, MilliFunds incumbent, MilliFunds challenger);
        bool price(MasterAssignmentEvaluation & evaluation);
        MasterRefinementStatus refineCertificate(MasterAssignmentEvaluation & evaluation, std::int64_t allowance);
        static bool retainedWitnessFits(const RelativeStockProofSnapshot & proof, std::int64_t work);
        static std::optional<std::int64_t> candidateReplayWork(const MasterCandidate & candidate);
        std::optional<std::int64_t> replayWork(std::int64_t payload) const;
        std::optional<std::int64_t> replayTariff(const MasterAssignmentState & state);

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentIdentity m_problemIdentity;
        RefinementLedger & m_ledger;
        RelativeStockSession* m_session{nullptr};
        MasterValuationTariffs m_tariffs;
        MasterAssignmentProfile* m_profile;
        std::optional<MasterRefinementRefusal> m_refinementRefusal;
        std::optional<RelativeStockCertificatePreparation> m_certificatePreparation;
    };
}
