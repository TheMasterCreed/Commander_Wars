#pragma once

#include <cstdint>

#include "ai/coordinator/economicledger.h"

namespace Coordinator
{
    class TurnPlan;

    class PlanStockValuer
    {
    public:
        virtual ~PlanStockValuer() = default;
        virtual MilliFunds planStock(const TurnPlan & plan) = 0;
        virtual MilliFunds originStock() const = 0;
        virtual MilliFunds stockCeiling() const = 0;
        virtual bool affectsStock(std::int32_t engineUnitId) const = 0;
    };
}
