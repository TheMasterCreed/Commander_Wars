#include <array>
#include <cstdint>

#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/ownershipschedule.h"

namespace
{
    using Coordinator::CapitalPolicy;
    using Coordinator::divideRoundHalfAwayFromZero;
    using Coordinator::EconomicDelta;
    using Coordinator::incomeSwing;
    using Coordinator::MilliFunds;
    using Coordinator::mirroredSign;
    using Coordinator::OwnershipInterval;
    using Coordinator::OwnerSign;
    using Coordinator::PropertyIncome;
    using Coordinator::ReplacementCostPolicy;
    using Coordinator::scheduleIncome;
    using Coordinator::scheduleTurns;
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

    constexpr MilliFunds SYMMETRIC_RATE = toMilliFunds(1000);
    constexpr PropertyIncome SYMMETRIC_INCOME{
        .oursPerTurn = SYMMETRIC_RATE,
        .enemyPerTurn = SYMMETRIC_RATE,
    };
    constexpr MilliFunds OUR_ASYMMETRIC_RATE = toMilliFunds(1200);
    constexpr MilliFunds ENEMY_ASYMMETRIC_RATE = toMilliFunds(800);
    constexpr PropertyIncome ASYMMETRIC_INCOME{
        .oursPerTurn = OUR_ASYMMETRIC_RATE,
        .enemyPerTurn = ENEMY_ASYMMETRIC_RATE,
    };
    constexpr std::int64_t HORIZON_TURNS = 8;
    constexpr std::int64_t DELAY_TURNS = 3;

    constexpr MilliFunds NEUTRAL_CAPTURE_SWING =
        incomeSwing(SYMMETRIC_INCOME, HORIZON_TURNS, OwnerSign::Neutral, OwnerSign::Ours);
    constexpr MilliFunds ENEMY_CAPTURE_SWING =
        incomeSwing(SYMMETRIC_INCOME, HORIZON_TURNS, OwnerSign::Enemy, OwnerSign::Ours);

    static_assert(NEUTRAL_CAPTURE_SWING == SYMMETRIC_RATE * HORIZON_TURNS);
    static_assert(ENEMY_CAPTURE_SWING == 2 * NEUTRAL_CAPTURE_SWING);
    static_assert(incomeSwing(SYMMETRIC_INCOME, HORIZON_TURNS, OwnerSign::Ours, OwnerSign::Ours) == 0);

    static_assert(incomeSwing(ASYMMETRIC_INCOME, HORIZON_TURNS, OwnerSign::Enemy, OwnerSign::Ours) ==
                  (OUR_ASYMMETRIC_RATE + ENEMY_ASYMMETRIC_RATE) * HORIZON_TURNS);
    static_assert(incomeSwing(ASYMMETRIC_INCOME.mirrored(), HORIZON_TURNS,
                              mirroredSign(OwnerSign::Enemy), mirroredSign(OwnerSign::Ours)) ==
                  -incomeSwing(ASYMMETRIC_INCOME, HORIZON_TURNS, OwnerSign::Enemy, OwnerSign::Ours));

    constexpr std::array<OwnershipInterval, 2> DELAYED_LOSS_OF_OURS{
        OwnershipInterval{OwnerSign::Ours, DELAY_TURNS},
        OwnershipInterval{OwnerSign::Enemy, HORIZON_TURNS - DELAY_TURNS},
    };
    constexpr std::array<OwnershipInterval, 2> DELAYED_LOSS_OF_NEUTRAL{
        OwnershipInterval{OwnerSign::Neutral, DELAY_TURNS},
        OwnershipInterval{OwnerSign::Enemy, HORIZON_TURNS - DELAY_TURNS},
    };
    constexpr std::array<OwnershipInterval, 1> IMMEDIATE_LOSS{
        OwnershipInterval{OwnerSign::Enemy, HORIZON_TURNS},
    };

    static_assert(scheduleTurns(DELAYED_LOSS_OF_OURS) == HORIZON_TURNS);
    static_assert(scheduleTurns(DELAYED_LOSS_OF_NEUTRAL) == HORIZON_TURNS);
    static_assert(scheduleTurns(IMMEDIATE_LOSS) == HORIZON_TURNS);

    constexpr MilliFunds PRESERVED_BY_DELAYING_OURS =
        scheduleIncome(SYMMETRIC_INCOME, DELAYED_LOSS_OF_OURS) - scheduleIncome(SYMMETRIC_INCOME, IMMEDIATE_LOSS);
    constexpr MilliFunds PRESERVED_BY_DELAYING_NEUTRAL =
        scheduleIncome(SYMMETRIC_INCOME, DELAYED_LOSS_OF_NEUTRAL) - scheduleIncome(SYMMETRIC_INCOME, IMMEDIATE_LOSS);

    static_assert(PRESERVED_BY_DELAYING_OURS == 2 * PRESERVED_BY_DELAYING_NEUTRAL);
    static_assert(PRESERVED_BY_DELAYING_OURS ==
                  incomeSwing(SYMMETRIC_INCOME, DELAY_TURNS, OwnerSign::Enemy, OwnerSign::Ours));

    constexpr std::array<OwnershipInterval, 2> MIRRORED_DELAYED_LOSS{
        OwnershipInterval{mirroredSign(OwnerSign::Ours), DELAY_TURNS},
        OwnershipInterval{mirroredSign(OwnerSign::Enemy), HORIZON_TURNS - DELAY_TURNS},
    };

    static_assert(scheduleIncome(ASYMMETRIC_INCOME.mirrored(), MIRRORED_DELAYED_LOSS) ==
                  -scheduleIncome(ASYMMETRIC_INCOME, DELAYED_LOSS_OF_OURS));

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
