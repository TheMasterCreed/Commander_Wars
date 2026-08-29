#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/continuationpricing.h"
#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/propertystocksequential.h"
#include "ai/coordinator/refinementledger.h"

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

    class JointPlanStockValuer final : public PlanStockValuer
    {
    public:
        JointPlanStockValuer(PropertyStockField & field, std::span<const KnownUnitLink> unitLinks);
        MilliFunds planStock(const TurnPlan & plan) override;
        MilliFunds originStock() const override;
        MilliFunds stockCeiling() const override;
        bool affectsStock(std::int32_t engineUnitId) const override;
        bool livePairSwapIntervals() const override;
        LivePlanStockQuote livePlanStock(
            const TurnPlan & plan,
            MilliFunds economicValue,
            bool pricingLeaf) override;
        bool refineLiveAtBoundary(AssignPhase phase) override;
        MilliFunds planStockFloor(const TurnPlan & plan);
        ContinuationPricingCatalog continuationPricingCatalog(
            const AssignmentInput & input) const;
        bool prepareContinuationPricing(
            ContinuationPricingCatalog catalog,
            RefinementLedger & ledger);

    private:
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
        std::int64_t mandatoryTariffFor(std::size_t actionCount) const;
        std::int64_t ourRerunTariff() const;
        std::int64_t enemyRerunTariff(
            const std::vector<std::int32_t> & capturedColumns) const;
        std::int32_t liveDemandCapacity() const;
        bool buildCheapModel(std::span<const PlanRowAction> actions);
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
        bool refineLiveKey(const ContinuationKey & key);
        bool refineOur(ContinuationEntry & entry,
                       GrantId grant,
                       std::int64_t stateCap);
        bool refineEnemy(
            EnemySetBounds & enemy,
            const std::vector<std::int32_t> & capturedColumns,
            GrantId grant,
            std::int64_t stateCap);
        MilliFunds sequentialJointStock(std::span<const PlanRowAction> actions) const;
        OurSequentialSolve solveOurSequential(
            std::span<const PlanRowAction> actions,
            std::int64_t stateCap,
            bool captureWitness) const;
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
            bool captureWitness) const;

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
        mutable bool m_originCached{false};
        mutable MilliFunds m_sequentialOrigin{0};
        mutable std::map<std::vector<std::int64_t>,
                         SequentialDetail::CachedRowEnumeration>
            m_rowOptionCache;
        std::unique_ptr<PropertyStockField> m_pricingField;
        CheapPricingModel m_cheapModel;
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
        mutable bool m_pricingFailure{false};
        bool m_refinementClosed{false};
    };

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns);
}
