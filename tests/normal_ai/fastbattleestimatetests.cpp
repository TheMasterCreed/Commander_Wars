#include "ai/fastbattleestimate.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    using FastBattleEstimate::Combatant;
    using FastBattleEstimate::evaluate;
    using FastBattleEstimate::exchangeBaseDamage;
    using FastBattleEstimate::fillDamageData;
    using FastBattleEstimate::NO_COUNTER_DAMAGE;
    using FastBattleEstimate::Result;
    constexpr double NO_DAMAGE = 0.0;
    constexpr double WEAK_HIT_DAMAGE = 30.0;
    constexpr double DIRECT_HIT_DAMAGE = 55.0;
    constexpr double SUPPORT_HIT_DAMAGE = 60.0;
    constexpr double PLINK_COUNTER_DAMAGE = 5.0;
    constexpr double LETHAL_DAMAGE = 120.0;
    constexpr double FULL_HP = 10.0;
    constexpr double CRIPPLED_HP = 2.0;
    constexpr std::int32_t INFANTRY_COSTS = 1000;
    constexpr std::int32_t ARTILLERY_COSTS = 6000;
    constexpr std::int32_t TANK_COSTS = 7000;
    constexpr double FULL_OWN_UNIT_VALUE = 1.0;
    constexpr Combatant HEALTHY_TANK{FULL_HP, TANK_COSTS};
    constexpr Combatant CRIPPLED_TANK{CRIPPLED_HP, TANK_COSTS};
    constexpr Combatant HEALTHY_ARTILLERY{FULL_HP, ARTILLERY_COSTS};
    constexpr Combatant CRIPPLED_ARTILLERY{CRIPPLED_HP, ARTILLERY_COSTS};
    constexpr Combatant HEALTHY_INFANTRY{FULL_HP, INFANTRY_COSTS};
    constexpr float HP_TOLERANCE = 0.000001f;
    constexpr float FUNDS_TOLERANCE = 0.001f;

    constexpr std::int32_t MIN_SWEEP_DAMAGE = -1;
    constexpr std::int32_t MAX_SWEEP_DAMAGE = 120;
    constexpr std::int32_t MIN_SWEEP_HP = 0;
    constexpr std::int32_t MAX_SWEEP_HP = 10;

    constexpr std::int32_t ORACLE_ATTACKER_SCALE = 10;
    constexpr std::int32_t ATTACKER_UNIT = 3;
    constexpr std::int32_t DEFENDER_UNIT = 7;

    constexpr std::int32_t orderedBaseDamage(std::int32_t attacker, std::int32_t defender)
    {
        return ORACLE_ATTACKER_SCALE * attacker + defender;
    }

    struct MirrorFundsDamage
    {
        float atkHpDamage{0.0f};
        float fundsDamage{0.0f};
        float hpDamage{0.0f};
    };

    struct MirrorDamageData
    {
        float fundsDamage{0.0f};
        float hpDamage{0.0f};
        float hpDamageDifference{0.0f};
    };

    constexpr float INFLICTED_HP_SENTINEL = 4.0f;
    constexpr float FUNDS_SENTINEL = 2500.0f;
    constexpr float DIFFERENCE_SENTINEL = 1.5f;

    static_assert(evaluate(NO_DAMAGE, NO_COUNTER_DAMAGE, HEALTHY_TANK, HEALTHY_ARTILLERY, FULL_OWN_UNIT_VALUE) == Result{});

    int failureCount = 0;

    bool isNear(float actual, float expected, float tolerance)
    {
        return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
    }

    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void checkEstimate(const Result & actual, const Result & expected, std::string_view message)
    {
        if (!isNear(actual.attackerHpDamage, expected.attackerHpDamage, HP_TOLERANCE) ||
            !isNear(actual.hpDamageDifference, expected.hpDamageDifference, HP_TOLERANCE) ||
            !isNear(actual.fundsDamage, expected.fundsDamage, FUNDS_TOLERANCE))
        {
            std::cerr << message
                      << ": expected hp damage " << expected.attackerHpDamage
                      << " difference " << expected.hpDamageDifference
                      << " funds " << expected.fundsDamage
                      << ", got hp damage " << actual.attackerHpDamage
                      << " difference " << actual.hpDamageDifference
                      << " funds " << actual.fundsDamage << '\n';
            ++failureCount;
        }
    }

    void checkSlot(std::int32_t actual, std::int32_t expected, std::string_view message)
    {
        if (actual != expected)
        {
            std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
            ++failureCount;
        }
    }

    void checkField(float actual, float expected, float tolerance, std::string_view message)
    {
        if (!isNear(actual, expected, tolerance))
        {
            std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
            ++failureCount;
        }
    }

    void testUncounterableTargetCostsNothing()
    {
        constexpr float inflictedHp = 5.5f;
        constexpr float fundsGain = 3300.0f;
        checkEstimate(evaluate(DIRECT_HIT_DAMAGE, NO_COUNTER_DAMAGE, HEALTHY_TANK, HEALTHY_ARTILLERY, FULL_OWN_UNIT_VALUE),
                      Result{inflictedHp, inflictedHp, fundsGain},
                      "uncounterable target");
    }

    void testExpensiveSupporterKeepsAPositiveTrade()
    {
        constexpr float inflictedHp = 6.0f;
        constexpr float remainingHp = 5.5f;
        constexpr float fundsGain = 250.0f;
        checkEstimate(evaluate(SUPPORT_HIT_DAMAGE, PLINK_COUNTER_DAMAGE, HEALTHY_TANK, HEALTHY_INFANTRY, FULL_OWN_UNIT_VALUE),
                      Result{inflictedHp, remainingHp, fundsGain},
                      "expensive supporter");
    }

    void testCounterArgumentChangesTheEstimate()
    {
        const Result plinked = evaluate(DIRECT_HIT_DAMAGE, PLINK_COUNTER_DAMAGE, HEALTHY_TANK, HEALTHY_ARTILLERY, FULL_OWN_UNIT_VALUE);
        const Result mirrored = evaluate(DIRECT_HIT_DAMAGE, DIRECT_HIT_DAMAGE, HEALTHY_TANK, HEALTHY_ARTILLERY, FULL_OWN_UNIT_VALUE);
        check(!isNear(plinked.fundsDamage, mirrored.fundsDamage, FUNDS_TOLERANCE), "counter argument moves the funds damage");
        check(!isNear(plinked.hpDamageDifference, mirrored.hpDamageDifference, HP_TOLERANCE), "counter argument moves the hp difference");
        check(isNear(plinked.attackerHpDamage, mirrored.attackerHpDamage, HP_TOLERANCE), "counter argument leaves the inflicted damage alone");
    }

    void testOverkillStopsAtTheDefendersHp()
    {
        constexpr float inflictedHp = 2.0f;
        constexpr float fundsGain = 1200.0f;
        checkEstimate(evaluate(LETHAL_DAMAGE, NO_COUNTER_DAMAGE, HEALTHY_TANK, CRIPPLED_ARTILLERY, FULL_OWN_UNIT_VALUE),
                      Result{inflictedHp, inflictedHp, fundsGain},
                      "overkill on a crippled defender");
    }

    void testLethalCounterStopsAtTheAttackersHp()
    {
        constexpr float inflictedHp = 3.0f;
        constexpr float remainingHp = -9.0f;
        constexpr float fundsLoss = -1100.0f;
        checkEstimate(evaluate(WEAK_HIT_DAMAGE, LETHAL_DAMAGE, CRIPPLED_TANK, HEALTHY_INFANTRY, FULL_OWN_UNIT_VALUE),
                      Result{inflictedHp, remainingHp, fundsLoss},
                      "lethal counter on a crippled attacker");
    }

    void testEstimateIsTotalOverTheRealDomain()
    {
        for (std::int32_t damage = MIN_SWEEP_DAMAGE; damage <= MAX_SWEEP_DAMAGE; ++damage)
        {
            for (std::int32_t counter = MIN_SWEEP_DAMAGE; counter <= MAX_SWEEP_DAMAGE; ++counter)
            {
                for (std::int32_t attackerHp = MIN_SWEEP_HP; attackerHp <= MAX_SWEEP_HP; ++attackerHp)
                {
                    for (std::int32_t defenderHp = MIN_SWEEP_HP; defenderHp <= MAX_SWEEP_HP; ++defenderHp)
                    {
                        const Combatant attacker{static_cast<double>(attackerHp), TANK_COSTS};
                        const Combatant defender{static_cast<double>(defenderHp), INFANTRY_COSTS};
                        const Result result = evaluate(damage, counter, attacker, defender, FULL_OWN_UNIT_VALUE);
                        check(std::isfinite(result.attackerHpDamage) &&
                              std::isfinite(result.hpDamageDifference) &&
                              std::isfinite(result.fundsDamage),
                              "sweep produced a non finite estimate");
                        check(result.attackerHpDamage <= static_cast<float>(defenderHp),
                              "sweep inflicted more hp damage than the defender has");
                    }
                }
            }
        }
    }

    void testCounterSlotReadsTheDefendersDamage()
    {
        const auto exchanged = exchangeBaseDamage(orderedBaseDamage, ATTACKER_UNIT, DEFENDER_UNIT);
        checkSlot(exchanged.attack, orderedBaseDamage(ATTACKER_UNIT, DEFENDER_UNIT), "attack slot");
        checkSlot(exchanged.counter, orderedBaseDamage(DEFENDER_UNIT, ATTACKER_UNIT), "counter slot");
    }

    void testDamageDataMappingKeepsTheInflictedHp()
    {
        constexpr MirrorFundsDamage funds{INFLICTED_HP_SENTINEL, FUNDS_SENTINEL, DIFFERENCE_SENTINEL};
        MirrorDamageData data;
        fillDamageData(funds, data);
        checkField(data.fundsDamage, FUNDS_SENTINEL, FUNDS_TOLERANCE, "mapped funds damage");
        checkField(data.hpDamage, INFLICTED_HP_SENTINEL, HP_TOLERANCE, "mapped hp damage");
        checkField(data.hpDamageDifference, DIFFERENCE_SENTINEL, HP_TOLERANCE, "mapped hp damage difference");
    }
}

int main()
{
    testUncounterableTargetCostsNothing();
    testExpensiveSupporterKeepsAPositiveTrade();
    testCounterArgumentChangesTheEstimate();
    testOverkillStopsAtTheDefendersHp();
    testLethalCounterStopsAtTheAttackersHp();
    testEstimateIsTotalOverTheRealDomain();
    testCounterSlotReadsTheDefendersDamage();
    testDamageDataMappingKeepsTheInflictedHp();
    return failureCount == 0 ? 0 : 1;
}
