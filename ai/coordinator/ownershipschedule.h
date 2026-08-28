#pragma once

#include <cstdint>
#include <span>

#include "ai/coordinator/economicledger.h"

namespace Coordinator
{
    enum class OwnerSign : std::int8_t
    {
        Enemy = -1,
        Neutral = 0,
        Ours = 1,
    };

    constexpr OwnerSign mirroredSign(OwnerSign sign)
    {
        return static_cast<OwnerSign>(-static_cast<std::int8_t>(sign));
    }

    struct PropertyIncome
    {
        // Separate rates because owner modifiers pay each side differently.
        MilliFunds oursPerTurn{0};
        MilliFunds enemyPerTurn{0};

        constexpr MilliFunds valuePerTurn(OwnerSign owner) const
        {
            switch (owner)
            {
                case OwnerSign::Ours:
                    return oursPerTurn;
                case OwnerSign::Enemy:
                    return -enemyPerTurn;
                case OwnerSign::Neutral:
                    break;
            }
            return 0;
        }

        constexpr PropertyIncome mirrored() const
        {
            return PropertyIncome{
                .oursPerTurn = enemyPerTurn,
                .enemyPerTurn = oursPerTurn,
            };
        }

        friend constexpr bool operator==(const PropertyIncome &, const PropertyIncome &) = default;
    };

    // Flat sums are the gamma = 1 finite-horizon branch of section 13.1.
    constexpr MilliFunds incomeSwing(const PropertyIncome & income, std::int64_t horizonTurns, OwnerSign before, OwnerSign after)
    {
        return (income.valuePerTurn(after) - income.valuePerTurn(before)) * horizonTurns;
    }

    struct OwnershipInterval
    {
        OwnerSign sign{OwnerSign::Neutral};
        std::int64_t turns{0};
    };

    constexpr std::int64_t scheduleTurns(std::span<const OwnershipInterval> intervals)
    {
        std::int64_t total = 0;
        for (const OwnershipInterval & interval : intervals)
        {
            total += interval.turns;
        }
        return total;
    }

    // Schedules being differenced must cover the same total turns.
    constexpr MilliFunds scheduleIncome(const PropertyIncome & income, std::span<const OwnershipInterval> intervals)
    {
        MilliFunds total = 0;
        for (const OwnershipInterval & interval : intervals)
        {
            total += income.valuePerTurn(interval.sign) * interval.turns;
        }
        return total;
    }
}
