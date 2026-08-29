#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/propertystocksequential.h"

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;
    class TurnPlan;
    struct KnownUnitLink;

    class JointPlanStockValuer final : public PlanStockValuer
    {
    public:
        JointPlanStockValuer(PropertyStockField & field, std::span<const KnownUnitLink> unitLinks);
        MilliFunds planStock(const TurnPlan & plan) override;
        MilliFunds originStock() const override;
        MilliFunds stockCeiling() const override;
        bool affectsStock(std::int32_t engineUnitId) const override;
        MilliFunds planStockFloor(const TurnPlan & plan);

    private:
        std::int32_t rowOf(std::int32_t engineUnitId) const;
        std::vector<PlanRowAction> planActions(const TurnPlan & plan) const;
        MilliFunds sequentialJointStock(std::span<const PlanRowAction> actions) const;
        void ensureOurSequentialState() const;
        std::int32_t ourClassIndexFor(const PropertyStockRow & row) const;
        std::vector<std::int32_t> nodeArrivalsFor(
            std::int32_t gridIdentity,
            std::int32_t movementPoints,
            const std::vector<std::int32_t> & nodeSlots) const;
        SequentialRow sequentialRowFor(
            const PropertyStockInstance & instance,
            const PropertyStockRow & row,
            std::int32_t rowIndex,
            const PropertyStockActor* pMover,
            const std::vector<std::int32_t> & nodeSlots,
            std::int32_t classIndex) const;
        MilliFunds enemySequentialOptimum(
            const std::vector<std::int32_t> & capturedColumns) const;

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
    };

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns);
}
