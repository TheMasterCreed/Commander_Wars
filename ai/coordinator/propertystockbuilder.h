#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#ifdef COW_COORDINATED_REFINEMENT_TESTING
#include <set>
#endif
#include <span>
#include <vector>

#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/continuationpricing.h"
#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/propertystocksequential.h"
#include "ai/coordinator/refinementledger.h"
#include "ai/coordinator/relativestocksession.h"
#ifdef COW_COORDINATED_REFINEMENT_TESTING
#include "ai/coordinator/sequentialthreshold.h"
#endif

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;
    class TurnPlan;
    struct AssignmentInput;
    struct KnownUnitLink;

    inline CanonicalPlanActionKey canonicalPlanActionKey(
        const PlanRowAction & action)
    {
        return CanonicalPlanActionKey::fromAction(
            action.row,
            action.destination.x,
            action.destination.y,
            action.captures);
    }

    inline ContinuationKey canonicalPlanKey(
        std::span<const PlanRowAction> actions)
    {
        ContinuationKey key;
        key.reserve(actions.size());
        for (const PlanRowAction & action : actions)
        {
            key.push_back(canonicalPlanActionKey(action));
        }
        std::sort(key.begin(), key.end());
        return key;
    }

    struct ContinuationPricingCatalog
    {
        std::vector<PlanRowAction> actions;
        std::int64_t mandatoryTariff{0};
        std::int64_t refinementBudget{0};
        std::int64_t ledgerTotal{0};
        bool valid{false};
    };

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    struct DiagnosticSequentialThresholdProof
    {
        SequentialThresholdResult proof;
        std::int64_t tariff{0};
        std::int64_t stateCap{0};
        std::int64_t workUsed{0};
        bool witnessReplays{false};
        bool valid{false};
    };
#endif

    class JointPlanStockValuer final : public PlanStockValuer
    {
    public:
        JointPlanStockValuer(PropertyStockField & field, std::span<const KnownUnitLink> unitLinks);
        MilliFunds planStock(const TurnPlan & plan) override;
        MilliFunds originStock() const override;
        MilliFunds stockCeiling() const override;
        bool affectsStock(std::int32_t engineUnitId) const override;
        std::int32_t stockRow(std::int32_t engineUnitId) const override;
        std::optional<ContinuationKey> continuationKey(const TurnPlan & plan) const override;
        bool livePairSwapIntervals() const override;
        LivePlanStockQuote livePlanStock(
            const TurnPlan & plan,
            MilliFunds economicValue,
            bool pricingLeaf) override;
        LiveRefinementResult refineLiveAtBoundary(AssignPhase phase, std::int64_t maximumWork) override;
        RelativeStockSession* relativeStockSession() override;
        PlanStockAudit auditPlanStock(const TurnPlan & plan) override;
        MilliFunds planStockFloor(const TurnPlan & plan);
        ContinuationPricingCatalog continuationPricingCatalog(
            const AssignmentInput & input) const;
        bool prepareContinuationPricing(
            ContinuationPricingCatalog catalog,
            RefinementLedger & ledger);
        bool prepareMasterContinuationPricing(ContinuationPricingCatalog catalog, RefinementLedger & ledger);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        bool prepareDiagnosticContinuationPricing(
            ContinuationPricingCatalog catalog, RefinementLedger & ledger, std::int64_t diagnosticWorkLimit);
        const SequentialTierResult & diagnosticLastOurRefinement() const
        {
            return m_diagnosticLastOurRefinement;
        }
        DiagnosticSequentialThresholdProof
        diagnosticProveOurBelow(
            const ContinuationKey & key,
            MilliFunds threshold,
            std::int64_t maximumWork,
            SequentialCacheAccounting cacheAccounting,
            SequentialThresholdOptions options = {});
#endif

    private:
        enum class PricingBudgetPolicy : std::int8_t
        {
            Dedicated,
            Shared,
        };

        bool prepareContinuationPricing(
            ContinuationPricingCatalog catalog, RefinementLedger & ledger, std::int64_t refinementBudget,
            PricingBudgetPolicy policy = PricingBudgetPolicy::Dedicated);

        enum class RefinementSchedule : std::int8_t
        {
            Rung,
            Relative,
        };

        class RelativeSession final : public RelativeStockSession
        {
        public:
            RelativeSession(JointPlanStockValuer & owner, const PropertyStockField & snapshot,
                            const PropertyStockInstance & enemyModel);
            RelativeStockQuote quote(const TurnPlan & plan, MilliFunds economic) override;
            RelativeStockQuote quote(const ContinuationKey & key, MilliFunds economic) override;
            const CheapPricingModel* pricingModel() const override;
            const MasterFutureCatalog* futureCatalog() const override;
            RelativeStockRefinedTerm refineOur(const ContinuationKey & key, std::int64_t stateCap) override;
            RelativeStockRefinedTerm refineEnemy(
                const std::vector<std::int32_t> & capturedColumns, std::int64_t stateCap) override;
            bool supportsVerifiedDual() const override
            {
                return true;
            }
            std::int64_t certificatePreparationWork() const override;
            RelativeStockDualTightening
            tightenOurUpperWithVerifiedDual(
                const RelativeStockQuote & quote,
                std::int64_t maximumWork) override;
            bool replayOurWitness(const RelativeStockQuote & quote) override;
            bool replayEnemyWitness(const RelativeStockQuote & quote) override;
            std::shared_ptr<const RelativeStockProofSnapshot> captureProof(const RelativeStockQuote & quote) override;
            bool replayLowerProof(const RelativeStockProofSnapshot & proof) const override;
            std::int64_t remainingWork() const override;
            std::int64_t ourRerunWork() const override;
            std::int64_t enemyRerunWork(const std::vector<std::int32_t> & capturedColumns) const override;

        private:
            GrantId grant(std::int64_t tariff, std::int64_t stateCap, RefinementWork work);
            bool verifiedUpperMatches(
                const RelativeStockQuote & quote) const override;
            JointPlanStockValuer & m_owner;
        };

        struct EnemySequentialSolve
        {
            SequentialTierResult tier;
            bool witnessReplays{false};
        };

        struct OurSequentialSolve
        {
            MilliFunds owned{0};
            std::vector<std::int32_t> capturedColumns;
            SequentialTierResult tier;
            bool witnessReplays{false};
        };

        struct OurSequentialProblem
        {
            MilliFunds owned{0};
            MilliFunds floorValue{0};
            std::vector<std::int32_t> capturedColumns;
            std::vector<std::vector<std::int64_t>> rowKeys;
            SequentialInstance instance;
        };

        struct RefinedWitness
        {
            std::vector<SequentialRowWitness> rows;
            MilliFunds value{0};
            bool replays{false};
        };

        std::int32_t rowOf(std::int32_t engineUnitId) const;
        std::vector<PlanRowAction> planActions(const TurnPlan & plan) const;
        static std::vector<PlanRowAction> planActions(
            const ContinuationKey & key);
        PlanStockAudit stockAudit(
            std::span<const PlanRowAction> actions);
        static SequentialStockAudit sequentialAudit(
            const SequentialTierResult & tier,
            bool witnessReplays);
        std::vector<PlanStockActionAudit> actionAudits(
            std::span<const PlanRowAction> actions) const;
        std::int64_t mandatoryTariffFor(std::size_t actionCount) const;
        std::int64_t ourRerunTariff() const;
        std::int64_t enemyRerunTariff(
            const std::vector<std::int32_t> & capturedColumns) const;
        std::int32_t liveDemandCapacity() const;
        bool buildCheapModel(std::span<const PlanRowAction> actions, bool retainSequentialRows = false);
        CheapRowTerms cheapRowTerms(
            const SequentialInstance & instance,
            const SequentialRow & row,
            std::int32_t rowIndex,
            std::span<const std::int32_t> nodeSlots) const;
        MilliFunds enemyUnionCeiling(
            PropertyStockField & field,
            std::span<const std::int32_t>
                admissibleCapturedColumns) const;
        bool priceCheap(const ContinuationKey & key,
                        CheapInitialPrice & price) const;
        LivePlanStockQuote liveQuote(
            const CheapInitialPrice & initial,
            MilliFunds economicValue) const;
        LiveRefinementResult refineLiveKey(const ContinuationKey & key, std::int64_t maximumWork);
        bool refineOur(ContinuationEntry & entry,
                       GrantId grant,
                       std::int64_t stateCap,
                       RefinementSchedule schedule = RefinementSchedule::Rung);
        bool refineEnemy(
            EnemySetBounds & enemy,
            const std::vector<std::int32_t> & capturedColumns,
            GrantId grant,
            std::int64_t stateCap,
            RefinementSchedule schedule = RefinementSchedule::Rung);
        MilliFunds sequentialJointStock(std::span<const PlanRowAction> actions) const;
        OurSequentialSolve solveOurSequential(
            std::span<const PlanRowAction> actions,
            std::int64_t stateCap,
            bool captureWitness,
            const RefinedWitness* pReplay = nullptr) const;
        OurSequentialProblem buildOurSequentialProblem(
            std::span<const PlanRowAction> actions) const;
        const PackingDualFieldIdentity &
        packingDualFieldIdentity() const;
        PackingDualProblemIdentity packingDualProblemIdentity(
            const RelativeStockQuote & quote,
            const OurSequentialProblem & problem) const;
        RelativeStockDualTightening
        tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote,
            std::uint64_t sessionIdentity,
            std::int64_t maximumWork);
        bool verifiedUpperMatches(
            const RelativeStockQuote & quote,
            std::uint64_t sessionIdentity) const;
        PropertyStockField & continuationField() const;
        void ensureOurSequentialState() const;
        std::int32_t ourClassIndexFor(const PropertyStockRow & row) const;
        std::vector<std::int32_t> nodeArrivalsFor(
            PropertyStockField & field,
            std::int32_t gridIdentity,
            std::int32_t movementPoints,
            const std::vector<std::int32_t> & nodeSlots) const;
        SequentialRow sequentialRowFor(
            PropertyStockField & field,
            const PropertyStockInstance & instance,
            const PropertyStockRow & row,
            std::int32_t rowIndex,
            const PropertyStockActor* pMover,
            const std::vector<std::int32_t> & nodeSlots,
            std::int32_t classIndex) const;
        MilliFunds enemySequentialOptimum(
            const std::vector<std::int32_t> & capturedColumns) const;
        EnemySequentialSolve solveEnemySequential(
            const std::vector<std::int32_t> & capturedColumns,
            std::int64_t stateCap,
            bool captureWitness,
            const RefinedWitness* pReplay = nullptr) const;

        PropertyStockField & m_field;
        std::map<std::int32_t, std::int32_t> m_knowledgeOf;
        mutable bool m_ourStateBuilt{false};
        mutable std::vector<std::int32_t> m_ourNodeSlots;
        mutable std::vector<std::int32_t> m_ourNodeOfColumn;
        mutable std::vector<PropertyStockColumn> m_ourNodeColumns;
        mutable std::vector<SequentialClassKey> m_ourClassKeys;
        mutable std::vector<SequentialClassTable> m_ourClassTables;
        mutable std::map<std::vector<std::int32_t>, MilliFunds>
            m_enemySequentialOptima;
        std::optional<PlanStockAudit> m_auditOrigin;
        mutable bool m_originCached{false};
        mutable MilliFunds m_sequentialOrigin{0};
        mutable std::map<std::vector<std::int64_t>,
                         SequentialDetail::CachedRowEnumeration>
            m_rowOptionCache;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        mutable std::set<std::vector<std::int64_t>>
            m_rowOptionPaidKeys;
        SequentialTierResult m_diagnosticLastOurRefinement;
#endif
        std::unique_ptr<PropertyStockField> m_pricingField;
        std::optional<RelativeSession> m_relativeSession;
        CheapPricingModel m_cheapModel;
        std::shared_ptr<const MasterFutureCatalog> m_masterFutureCatalog;
        std::int64_t m_masterOurSearchStates{0};
        mutable CheapUpperState m_cheapState;
        ContinuationPricingState m_pricingState{
            ContinuationPricingState::Unprepared
        };
        ContinuationKey m_preparedCatalogKey;
        RefinementLedger* m_pRefinementLedger{nullptr};
        mutable ContinuationStore m_continuationStore;
        mutable ContenderRegistry m_contenderRegistry;
        std::map<ContinuationKey, RefinedWitness>
            m_refinedOurWitnesses;
        std::map<std::vector<std::int32_t>, RefinedWitness>
            m_refinedEnemyWitnesses;
        struct VerifiedOurUpper
        {
            std::uint64_t id{0};
            std::shared_ptr<const PackingDualCertificate>
                certificate;
        };
        std::map<ContinuationKey, VerifiedOurUpper>
            m_verifiedOurUppers;
        std::uint64_t m_nextCertificateId{1};
        mutable std::optional<PackingDualFieldIdentity>
            m_packingDualFieldIdentity;
        mutable bool m_pricingFailure{false};
        bool m_refinementClosed{false};
    };

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns);
}
