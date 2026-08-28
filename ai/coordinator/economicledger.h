#pragma once

#include <cstdint>

namespace Coordinator
{
    // Fixed point funds, no floats in the ledger.
    using MilliFunds = std::int64_t;

    // 1000 keeps replacementCost * hpSteps / UNIT_HP_STEPS exact for whole-fund costs.
    constexpr std::int64_t MILLI_FUNDS_PER_FUND = 1000;
    // Matches Unit::MAX_UNIT_HP; hpSteps means Unit::getHpRounded, 0..10.
    constexpr std::int32_t UNIT_HP_STEPS = 10;

    constexpr bool isHpStepCount(std::int32_t steps)
    {
        return steps >= 0 && steps <= UNIT_HP_STEPS;
    }

    // a unit at 0 hp no longer exists, so it can neither act nor be a target
    constexpr bool isLiveHpStepCount(std::int32_t steps)
    {
        return steps > 0 && steps <= UNIT_HP_STEPS;
    }

    constexpr MilliFunds toMilliFunds(std::int64_t funds)
    {
        return funds * MILLI_FUNDS_PER_FUND;
    }

    // Odd symmetry preserves the player mirror invariant; denominator must be positive.
    constexpr std::int64_t divideRoundHalfAwayFromZero(std::int64_t numerator, std::int64_t denominator)
    {
        const std::int64_t half = denominator / 2;
        return numerator >= 0
                   ? (numerator + half) / denominator
                   : -((-numerator + half) / denominator);
    }

    // Signed our-minus-enemy funds.
    struct EconomicDelta
    {
        MilliFunds income{0};
        MilliFunds friendlyCapital{0};
        MilliFunds enemyCapital{0};  // a kill books positive here, costs stay negative
        MilliFunds actionCost{0};
        MilliFunds resourceCost{0};
        MilliFunds scriptedCapital{0};
        MilliFunds revaluation{0};

        constexpr MilliFunds total() const
        {
            return income + friendlyCapital + enemyCapital + actionCost + resourceCost + scriptedCapital + revaluation;
        }

        // The player mirror swaps the capital channels; plain negation does not.
        constexpr EconomicDelta mirrored() const
        {
            return EconomicDelta{
                .income = -income,
                .friendlyCapital = -enemyCapital,
                .enemyCapital = -friendlyCapital,
                .actionCost = -actionCost,
                .resourceCost = -resourceCost,
                .scriptedCapital = -scriptedCapital,
                .revaluation = -revaluation,
            };
        }

        friend constexpr EconomicDelta operator+(const EconomicDelta & lhs, const EconomicDelta & rhs)
        {
            return EconomicDelta{
                .income = lhs.income + rhs.income,
                .friendlyCapital = lhs.friendlyCapital + rhs.friendlyCapital,
                .enemyCapital = lhs.enemyCapital + rhs.enemyCapital,
                .actionCost = lhs.actionCost + rhs.actionCost,
                .resourceCost = lhs.resourceCost + rhs.resourceCost,
                .scriptedCapital = lhs.scriptedCapital + rhs.scriptedCapital,
                .revaluation = lhs.revaluation + rhs.revaluation,
            };
        }

        friend constexpr EconomicDelta operator-(const EconomicDelta & value)
        {
            return EconomicDelta{
                .income = -value.income,
                .friendlyCapital = -value.friendlyCapital,
                .enemyCapital = -value.enemyCapital,
                .actionCost = -value.actionCost,
                .resourceCost = -value.resourceCost,
                .scriptedCapital = -value.scriptedCapital,
                .revaluation = -value.revaluation,
            };
        }

        friend constexpr EconomicDelta operator-(const EconomicDelta & lhs, const EconomicDelta & rhs)
        {
            return lhs + (-rhs);
        }

        friend constexpr bool operator==(const EconomicDelta &, const EconomicDelta &) = default;
    };

    class CapitalPolicy
    {
    public:
        virtual constexpr ~CapitalPolicy() = default;
        // Repair cost modifiers land in revaluation, never in the basis.
        virtual constexpr MilliFunds bookValue(MilliFunds replacementCost, std::int64_t hpSteps) const = 0;
    };

    class ReplacementCostPolicy final : public CapitalPolicy
    {
    public:
        constexpr MilliFunds bookValue(MilliFunds replacementCost, std::int64_t hpSteps) const override
        {
            return divideRoundHalfAwayFromZero(replacementCost * hpSteps, UNIT_HP_STEPS);
        }
    };
}
