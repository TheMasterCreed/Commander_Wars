#include <cstdint>

#include "ai/coordinator/economicledger.h"

namespace
{
    using Coordinator::CapitalPolicy;
    using Coordinator::divideRoundHalfAwayFromZero;
    using Coordinator::EconomicDelta;
    using Coordinator::MilliFunds;
    using Coordinator::ReplacementCostPolicy;
    using Coordinator::toMilliFunds;
    using Coordinator::UNIT_HP_STEPS;

    constexpr ReplacementCostPolicy REPLACEMENT_POLICY{};
    constexpr const CapitalPolicy & CAPITAL_POLICY = REPLACEMENT_POLICY;

    constexpr MilliFunds UNIT_REPLACEMENT_COST = toMilliFunds(7000);
    constexpr MilliFunds FULL_BOOK_VALUE = REPLACEMENT_POLICY.bookValue(UNIT_REPLACEMENT_COST, UNIT_HP_STEPS);

    constexpr bool satisfiesCapitalContract(const CapitalPolicy & policy, MilliFunds replacementCost)
    {
        if (policy.bookValue(replacementCost, 0) != 0)
        {
            return false;
        }
        if (policy.bookValue(replacementCost, UNIT_HP_STEPS) != replacementCost)
        {
            return false;
        }
        for (std::int64_t steps = 1; steps <= UNIT_HP_STEPS; ++steps)
        {
            if (policy.bookValue(replacementCost, steps) < policy.bookValue(replacementCost, steps - 1))
            {
                return false;
            }
        }
        return true;
    }

    constexpr bool bookValueIsStrictlyIncreasing(const CapitalPolicy & policy, MilliFunds replacementCost)
    {
        for (std::int64_t steps = 1; steps <= UNIT_HP_STEPS; ++steps)
        {
            if (policy.bookValue(replacementCost, steps) <= policy.bookValue(replacementCost, steps - 1))
            {
                return false;
            }
        }
        return true;
    }

    static_assert(FULL_BOOK_VALUE == UNIT_REPLACEMENT_COST);
    static_assert(CAPITAL_POLICY.bookValue(UNIT_REPLACEMENT_COST, UNIT_HP_STEPS) == FULL_BOOK_VALUE);
    static_assert(satisfiesCapitalContract(REPLACEMENT_POLICY, UNIT_REPLACEMENT_COST));
    static_assert(bookValueIsStrictlyIncreasing(REPLACEMENT_POLICY, UNIT_REPLACEMENT_COST));

    // Off the whole-fund scale rounding engages and strictness is no longer guaranteed.
    constexpr MilliFunds SMALL_ODD_COST = 25;
    constexpr MilliFunds SMALL_ODD_HIGH_BOOK = 18;
    constexpr std::int64_t SMALL_ODD_HIGH_STEPS = 7;

    static_assert(satisfiesCapitalContract(REPLACEMENT_POLICY, SMALL_ODD_COST));
    static_assert(REPLACEMENT_POLICY.bookValue(SMALL_ODD_COST, SMALL_ODD_HIGH_STEPS) == SMALL_ODD_HIGH_BOOK);

    constexpr EconomicDelta BUILD_DELTA{
        .friendlyCapital = FULL_BOOK_VALUE,
        .actionCost = -UNIT_REPLACEMENT_COST,
    };

    static_assert(BUILD_DELTA.total() == 0);

    constexpr std::int64_t REPAIR_START_HP_STEPS = 4;
    constexpr std::int64_t REPAIR_HP_STEPS_GAINED = 3;
    constexpr std::int64_t REPAIR_END_HP_STEPS = REPAIR_START_HP_STEPS + REPAIR_HP_STEPS_GAINED;
    // unit.js charges costs / 10 per healed step.
    constexpr MilliFunds REPAIR_COST = UNIT_REPLACEMENT_COST * REPAIR_HP_STEPS_GAINED / UNIT_HP_STEPS;
    constexpr MilliFunds REPAIR_BOOK_GAIN = REPLACEMENT_POLICY.bookValue(UNIT_REPLACEMENT_COST, REPAIR_END_HP_STEPS) -
                                            REPLACEMENT_POLICY.bookValue(UNIT_REPLACEMENT_COST, REPAIR_START_HP_STEPS);
    constexpr EconomicDelta REPAIR_DELTA{
        .friendlyCapital = REPAIR_BOOK_GAIN,
        .actionCost = -REPAIR_COST,
    };

    static_assert(REPAIR_BOOK_GAIN == REPAIR_COST);
    static_assert(REPAIR_DELTA.total() == 0);

    // Off scale the away-from-zero basis exceeds the engine's truncating charge by one.
    constexpr MilliFunds SMALL_ODD_BOOK_GAIN = REPLACEMENT_POLICY.bookValue(SMALL_ODD_COST, REPAIR_END_HP_STEPS) -
                                               REPLACEMENT_POLICY.bookValue(SMALL_ODD_COST, REPAIR_START_HP_STEPS);
    constexpr MilliFunds SMALL_ODD_TRUNCATED_CHARGE =
        SMALL_ODD_COST * REPAIR_HP_STEPS_GAINED / UNIT_HP_STEPS;

    static_assert(SMALL_ODD_BOOK_GAIN == SMALL_ODD_TRUNCATED_CHARGE + 1);

    constexpr EconomicDelta singleStepRepair(std::int64_t fromSteps)
    {
        return EconomicDelta{
            .friendlyCapital = REPLACEMENT_POLICY.bookValue(UNIT_REPLACEMENT_COST, fromSteps + 1) -
                               REPLACEMENT_POLICY.bookValue(UNIT_REPLACEMENT_COST, fromSteps),
            .actionCost = -(UNIT_REPLACEMENT_COST / UNIT_HP_STEPS),
        };
    }

    constexpr EconomicDelta REPAIR_STEPWISE = singleStepRepair(REPAIR_START_HP_STEPS) +
                                              singleStepRepair(REPAIR_START_HP_STEPS + 1) +
                                              singleStepRepair(REPAIR_START_HP_STEPS + 2);

    static_assert(REPAIR_STEPWISE == REPAIR_DELTA);
    static_assert(REPAIR_STEPWISE.total() == 0);

    constexpr MilliFunds LEDGER_INCOME = toMilliFunds(3000);
    constexpr MilliFunds LEDGER_KILL_VALUE = toMilliFunds(2400);
    constexpr MilliFunds LEDGER_LOSS_VALUE = toMilliFunds(1600);
    constexpr MilliFunds LEDGER_AMMO_COST = toMilliFunds(150);
    constexpr MilliFunds LEDGER_SCRIPTED_GRANT = toMilliFunds(500);
    constexpr MilliFunds LEDGER_REVALUATION = toMilliFunds(90);

    constexpr EconomicDelta KILL_TRADE{
        .income = LEDGER_INCOME,
        .friendlyCapital = -LEDGER_LOSS_VALUE,
        .enemyCapital = LEDGER_KILL_VALUE,
        .resourceCost = -LEDGER_AMMO_COST,
    };
    constexpr EconomicDelta SCRIPTED_BUILD{
        .friendlyCapital = FULL_BOOK_VALUE,
        .actionCost = -UNIT_REPLACEMENT_COST,
        .scriptedCapital = LEDGER_SCRIPTED_GRANT,
    };
    constexpr EconomicDelta REVALUED_INCOME{
        .income = LEDGER_INCOME,
        .revaluation = LEDGER_REVALUATION,
    };

    static_assert((KILL_TRADE + SCRIPTED_BUILD).total() == KILL_TRADE.total() + SCRIPTED_BUILD.total());
    static_assert(((KILL_TRADE + SCRIPTED_BUILD) + REVALUED_INCOME).total() ==
                  (KILL_TRADE + (SCRIPTED_BUILD + REVALUED_INCOME)).total());
    static_assert(KILL_TRADE + (-KILL_TRADE) == EconomicDelta{});
    static_assert(-(-SCRIPTED_BUILD) == SCRIPTED_BUILD);
    static_assert((-KILL_TRADE).total() == -KILL_TRADE.total());
    static_assert(KILL_TRADE - SCRIPTED_BUILD == KILL_TRADE + (-SCRIPTED_BUILD));
    static_assert((KILL_TRADE - KILL_TRADE) == EconomicDelta{});

    static_assert(KILL_TRADE.mirrored().mirrored() == KILL_TRADE);
    static_assert(KILL_TRADE.mirrored().total() == -KILL_TRADE.total());
    static_assert(KILL_TRADE.mirrored().friendlyCapital == -KILL_TRADE.enemyCapital);
    static_assert(KILL_TRADE.mirrored().enemyCapital == -KILL_TRADE.friendlyCapital);
    static_assert(KILL_TRADE.mirrored() != -KILL_TRADE);

    constexpr std::int64_t EVEN_DENOMINATOR = UNIT_HP_STEPS;
    constexpr std::int64_t EVEN_DOWN_NUMERATOR = 24;
    constexpr std::int64_t EVEN_HALF_NUMERATOR = 25;
    constexpr std::int64_t EVEN_UP_NUMERATOR = 26;
    constexpr std::int64_t EVEN_DOWN_RESULT = 2;
    constexpr std::int64_t EVEN_HALF_RESULT = 3;
    constexpr std::int64_t EVEN_UP_RESULT = 3;
    constexpr std::int64_t ODD_DENOMINATOR = 3;
    constexpr std::int64_t ODD_DOWN_NUMERATOR = 4;
    constexpr std::int64_t ODD_UP_NUMERATOR = 5;
    constexpr std::int64_t IDENTITY_NUMERATOR = 7;

    static_assert(divideRoundHalfAwayFromZero(0, EVEN_DENOMINATOR) == 0);
    static_assert(divideRoundHalfAwayFromZero(EVEN_DOWN_NUMERATOR, EVEN_DENOMINATOR) == EVEN_DOWN_RESULT);
    static_assert(divideRoundHalfAwayFromZero(EVEN_HALF_NUMERATOR, EVEN_DENOMINATOR) == EVEN_HALF_RESULT);
    static_assert(divideRoundHalfAwayFromZero(EVEN_UP_NUMERATOR, EVEN_DENOMINATOR) == EVEN_UP_RESULT);
    static_assert(divideRoundHalfAwayFromZero(-EVEN_HALF_NUMERATOR, EVEN_DENOMINATOR) == -EVEN_HALF_RESULT);
    static_assert(divideRoundHalfAwayFromZero(-EVEN_DOWN_NUMERATOR, EVEN_DENOMINATOR) ==
                  -divideRoundHalfAwayFromZero(EVEN_DOWN_NUMERATOR, EVEN_DENOMINATOR));
    static_assert(divideRoundHalfAwayFromZero(-EVEN_UP_NUMERATOR, EVEN_DENOMINATOR) ==
                  -divideRoundHalfAwayFromZero(EVEN_UP_NUMERATOR, EVEN_DENOMINATOR));
    static_assert(divideRoundHalfAwayFromZero(ODD_DOWN_NUMERATOR, ODD_DENOMINATOR) == 1);
    static_assert(divideRoundHalfAwayFromZero(ODD_UP_NUMERATOR, ODD_DENOMINATOR) == 2);
    static_assert(divideRoundHalfAwayFromZero(-ODD_DOWN_NUMERATOR, ODD_DENOMINATOR) == -1);
    static_assert(divideRoundHalfAwayFromZero(-ODD_UP_NUMERATOR, ODD_DENOMINATOR) == -2);
    static_assert(divideRoundHalfAwayFromZero(IDENTITY_NUMERATOR, 1) == IDENTITY_NUMERATOR);
    static_assert(divideRoundHalfAwayFromZero(-IDENTITY_NUMERATOR, 1) == -IDENTITY_NUMERATOR);
}

int main()
{
    return 0;
}
