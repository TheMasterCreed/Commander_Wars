#pragma once

#include "ai/coordinator/masterassignmentbounds.h"
#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/relativestocksession.h"
#include "ai/coordinator/masterassignmentprofile.h"
#include "ai/coordinator/masterguardedstock.h"

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
        GuardedRootPrepared,
    };

    struct MasterAssignmentEvaluation
    {
        MasterAssignmentState state;
        MasterEvaluationStatus status{MasterEvaluationStatus::Invalid};
        MasterObjectiveStatus objectiveStatus{MasterObjectiveStatus::Unavailable};
        std::optional<MasterAssignmentReplayResult> replay;
        std::shared_ptr<const RelativeStockProofSnapshot> proof;
        std::shared_ptr<const MasterGuardedPlanRoot> guardedRoot;
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

    enum class MasterOperationStatus : std::int8_t
    {
        Ready,
        Unavailable,
        MissingPrerequisite,
        FundingRefused,
        Invalid,
    };

    constexpr std::int64_t MASTER_OPERATION_GROWTH_FACTOR = 4;
    constexpr std::int64_t MASTER_OPERATION_QUOTA_WORK = 4;

    struct MasterOperationReadiness
    {
        MasterAssignmentIdentity identity;
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        RelativeStockOperationPreparation preparation;
    };

    struct MasterOperationOffer
    {
        MasterOperationStatus status{MasterOperationStatus::Invalid};
        MasterRefinementTerm term{MasterRefinementTerm::Friendly};
        std::int64_t allowance{0};
        std::int64_t remainingRequiredWork{0};
        std::int64_t inspectionWork{0};
        std::int64_t preparationWork{0};
        std::int64_t quotaWork{0};
        MasterAssignmentIdentity identity;
        std::shared_ptr<const RelativeStockProofSnapshot> proof;
        RelativeStockOperationMetadata metadata;
        RelativeStockOperationPreparation preparation;

    private:
        friend class MasterAssignmentEvaluator;
        struct InspectionPermit
        {
            struct Binding
            {
            };
            std::weak_ptr<const Binding> binding;
            bool consumed{false};
        };
        std::shared_ptr<InspectionPermit> m_inspection;
    };

    struct MasterOperationOutcome
    {
        MasterRefinementStatus status{MasterRefinementStatus::Invalid};
        std::int64_t executionWork{0};
        std::int64_t nativeWork{0};
        std::int64_t nativeStates{0};
        std::optional<StockInterval> nativeBefore;
        std::optional<StockInterval> nativeAfter;
        bool admitted{false};
        bool completed{false};
        bool lowerProgress{false};
        bool upperProgress{false};
        bool certificateProgress{false};
    };

    enum class MasterOursCapStatus : std::int8_t
    {
        Registered,
        Rejected,
        Contradiction,
        OwnedMismatch,
        CatalogMismatch,
    };

    enum class MasterOursBasisStatus : std::int8_t
    {
        Proven,
        Unproven,
        CatalogMismatch,
        OwnedMismatch,
        BudgetExhausted,
    };

    struct MasterOursCapReceipt
    {
        MasterOursCapStatus status{MasterOursCapStatus::Rejected};
        std::optional<MilliFunds> verifiedUpper;
        std::optional<MilliFunds> ceiling;
    };

    class MasterAssignmentEvaluator final
    {
    public:
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  MasterAssignmentProfile* profile = nullptr);
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  RelativeStockSession & session, MasterValuationTariffs tariffs,
                                  MasterAssignmentProfile* profile = nullptr);
        MasterAssignmentEvaluator(const MasterAssignmentProblem & problem, RefinementLedger & ledger,
                                  MasterGuardedStockOwner & owner, MasterAssignmentProfile* profile = nullptr);

        std::optional<std::int64_t> finalizationWork();
        MasterAssignmentEvaluation evaluate(const MasterAssignmentState & state);
        bool refresh(MasterAssignmentEvaluation & evaluation);
        MasterRefinementStatus refine(MasterAssignmentEvaluation & evaluation,
                                      MasterRefinementTerm term, std::int64_t allowance);
        MasterRefinementStatus adoptFriendlyWitness(MasterAssignmentEvaluation & evaluation,
                                                    std::span<const SequentialRowWitness> witness, MilliFunds value);
        bool hintFriendlyUpperPrices(const MasterAssignmentEvaluation & evaluation,
                                     std::span<const MilliFunds> futureNodePrices);
        MasterOperationOffer operationOffer(
            const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
            std::int64_t allowance, std::shared_ptr<const MasterOperationReadiness> & readiness);
        MasterOperationOutcome executeOperation(MasterAssignmentEvaluation & evaluation,
                                                const MasterOperationOffer & offer);
        RelativeStockComparison compare(MasterAssignmentEvaluation & incumbent,
                                        MasterAssignmentEvaluation & challenger);
        // Applies exactly the recorded ceilings; a present ceiling must equal the verified one for that state.
        RelativeStockComparison compareRecorded(MasterAssignmentEvaluation & incumbent,
                                                MasterAssignmentEvaluation & challenger,
                                                std::optional<MilliFunds> incumbentCap,
                                                std::optional<MilliFunds> challengerCap);
        MasterOursCapReceipt capOursUpper(const MasterAssignmentEvaluation & evaluation,
                                          const MasterBoundCertificate & certificate,
                                          const MasterAssignmentBounds & bounds);
        // Proven when seated economic plus quoted owned and ours lower is a replayed floor in the bound scan's units.
        MasterOursBasisStatus oursBasis(const MasterAssignmentEvaluation & evaluation, RefinementLedger & ledger) const;
        std::int64_t oursCapCheckWork() const;
        std::optional<MilliFunds> oursCap(const MasterAssignmentEvaluation & evaluation) const;
        // Empty when the evaluation has no quote or a stored cap no longer holds for it.
        std::optional<RelativeStockQuote> comparisonQuote(const MasterAssignmentEvaluation & evaluation) const;
        RelativeStockComparison recordedComparison(const MasterAssignmentEvaluation & incumbent,
                                                   const MasterAssignmentEvaluation & challenger,
                                                   std::optional<MilliFunds> incumbentCap,
                                                   std::optional<MilliFunds> challengerCap) const;
        std::uint64_t oursCapContradictions() const;
        MasterGuardedComparison compareGuarded(const MasterAssignmentEvaluation & incumbent,
                                              const MasterAssignmentEvaluation & challenger);
        GuardedRelationStatus verifyGuarded(const MasterGuardedStockProof & proof, bool fresh);
        bool hasGuardedStock() const;
        bool guardedAdmits(std::size_t actor, std::int32_t candidate) const;
        // Owned-snapshot scheduling eligibility does not authorize a fresh proof.
        bool unresolvedCommonEnemy(const MasterAssignmentEvaluation & incumbent,
                                   const MasterAssignmentEvaluation & challenger);
        bool hasContinuation() const;
        const CheapPricingModel* stockModel() const;
        const MasterFutureCatalog* futureCatalog() const;
        bool matches(const MasterAssignmentProblem & problem, const RefinementLedger & ledger) const;
        bool ownsRetainedEvaluation(const MasterAssignmentEvaluation & evaluation) const;
        std::optional<std::int64_t> retainedCopyWork(const MasterAssignmentEvaluation & evaluation);
        MasterObjectiveStatus replayRetainedEvaluation(const MasterAssignmentEvaluation & evaluation);
        const std::optional<MasterRefinementRefusal> & refinementRefusal() const;
        const std::optional<RelativeStockCertificatePreparation> & certificatePreparation() const;

    private:
        struct OursCap
        {
            ContinuationKey key;
            RelativeStockIdentity identity;
            MilliFunds upper{0};
            MilliFunds owned{0};
            MasterBoundCertificate certificate;
        };

        enum class CeilingStatus : std::int8_t
        {
            None,
            Usable,
            Contradiction,
        };

        struct Ceiling
        {
            CeilingStatus status{CeilingStatus::None};
            MilliFunds value{0};
        };

        const OursCap* applicableOursCap(const MasterAssignmentEvaluation & evaluation) const;
        Ceiling verifiedCeiling(const MasterAssignmentEvaluation & evaluation) const;
        std::optional<RelativeStockQuote> recordedQuote(const MasterAssignmentEvaluation & evaluation,
                                                        std::optional<MilliFunds> cap) const;
        RelativeStockComparison compareQuotes(MasterAssignmentEvaluation & incumbent,
                                              MasterAssignmentEvaluation & challenger,
                                              const std::optional<RelativeStockQuote> & left,
                                              const std::optional<RelativeStockQuote> & right);
        bool checkOursCaps(const MasterAssignmentEvaluation & incumbent, const MasterAssignmentEvaluation & challenger);
        static std::optional<MilliFunds> oursCeiling(MilliFunds upper, const MasterAssignmentEvaluation & evaluation);
        std::optional<MilliFunds> verifiedUpper(const MasterBoundCertificate & certificate,
                                                const MasterAssignmentBounds & bounds);
        void rejectOursCaps();
        static RelativeStockComparison includeSeatedEconomics(
            RelativeStockComparison comparison, MilliFunds incumbent, MilliFunds challenger);
        bool price(MasterAssignmentEvaluation & evaluation);
        const RelativeStockQuote* beginRefinement(const MasterAssignmentEvaluation & evaluation);
        std::optional<std::int64_t> rerunTotal(std::int64_t tariff, std::int64_t allowance) const;
        MasterRefinementStatus refineCertificate(MasterAssignmentEvaluation & evaluation, std::int64_t allowance);
        std::optional<std::int64_t> operationInspectionWork(const MasterAssignmentEvaluation & evaluation) const;
        bool validOperationMetadata(const RelativeStockOperationMetadata & metadata,
                                    const RelativeStockQuote & quote) const;
        bool prepareOperation(MasterOperationOffer & offer,
                              std::shared_ptr<const MasterOperationReadiness> & readiness);
        bool planOperationAllowance(MasterOperationOffer & offer);
        void refuseOperation(MasterOperationOffer & offer, std::int64_t requested, std::int64_t preparation);
        std::optional<std::int64_t> operationRequiredWork(const MasterOperationOffer & offer) const;
        MasterOperationOutcome executePreparedOperation(MasterAssignmentEvaluation & evaluation,
                                                        const MasterOperationOffer & offer);
        MasterOperationOutcome runOperation(MasterAssignmentEvaluation & evaluation,
                                           const MasterOperationOffer & offer);
        static bool retainedWitnessFits(const RelativeStockProofSnapshot & proof, std::int64_t work);
        static std::optional<std::int64_t> candidateReplayWork(const MasterCandidate & candidate);
        std::optional<std::int64_t> replayWork(std::int64_t payload) const;
        std::optional<std::int64_t> replayTariff(const MasterAssignmentState & state);

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentIdentity m_problemIdentity;
        RefinementLedger & m_ledger;
        RelativeStockSession* m_session{nullptr};
        MasterGuardedStockOwner* m_guardedOwner{nullptr};
        MasterValuationTariffs m_tariffs;
        MasterAssignmentProfile* m_profile;
        std::shared_ptr<const MasterOperationOffer::InspectionPermit::Binding> m_operationBinding;
        std::optional<MasterRefinementRefusal> m_refinementRefusal;
        std::optional<RelativeStockCertificatePreparation> m_certificatePreparation;
        std::vector<OursCap> m_oursCaps;
        std::uint64_t m_oursCapContradictions{0};
    };
}
