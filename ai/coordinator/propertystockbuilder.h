#pragma once

#include <cstdint>
#include <map>
#include <span>

#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"

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

    private:
        std::int32_t rowOf(std::int32_t engineUnitId) const;

        PropertyStockField & m_field;
        std::map<std::int32_t, std::int32_t> m_knowledgeOf;
    };

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns);
}
