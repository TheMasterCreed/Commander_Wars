#include "ai/buildingthreat.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    using BuildingThreat::Target;
    using BuildingThreat::threatValue;
    constexpr double CANNON_DAMAGE = 5.0;
    constexpr double CRYSTAL_HEAL_DAMAGE = -2.0;
    constexpr double NO_DAMAGE = 0.0;
    constexpr double FULL_HP = 10.0;
    constexpr double CRIPPLED_HP = 3.0;
    constexpr double DESTROYED_HP = 0.0;
    constexpr std::int32_t TANK_COSTS = 6000;
    constexpr Target HEALTHY_TANK{FULL_HP, TANK_COSTS};
    constexpr Target CRIPPLED_TANK{CRIPPLED_HP, TANK_COSTS};
    constexpr Target DYING_TANK{DESTROYED_HP, TANK_COSTS};
    constexpr float THREAT_TOLERANCE = 0.001f;
    constexpr float HEALTHY_TANK_THREAT = 3000.0f;
    constexpr std::int32_t MIN_SWEEP_DAMAGE = -20;
    constexpr std::int32_t MAX_SWEEP_DAMAGE = 120;
    constexpr std::int32_t MIN_SWEEP_HP = 0;
    constexpr std::int32_t MAX_SWEEP_HP = 10;
    static_assert(threatValue(CANNON_DAMAGE, HEALTHY_TANK) == HEALTHY_TANK_THREAT);

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

    void checkThreat(float actual, float expected, std::string_view message)
    {
        if (!isNear(actual, expected, THREAT_TOLERANCE))
        {
            std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
            ++failureCount;
        }
    }

    void testCrippledTargetIsNotOverpriced()
    {
        constexpr float crippledThreat = 1800.0f;
        checkThreat(threatValue(CANNON_DAMAGE, CRIPPLED_TANK), crippledThreat, "crippled target threat");
    }

    void testTargetWithNoHpLeftIsNoThreat()
    {
        checkThreat(threatValue(CANNON_DAMAGE, DYING_TANK), 0.0f, "target with no hp left");
    }

    void testDamageUnderTheCapIsUnchanged()
    {
        checkThreat(threatValue(CANNON_DAMAGE, HEALTHY_TANK), HEALTHY_TANK_THREAT, "damage under the cap");
    }

    void testDamageEqualToTheRemainingHpIsUnclamped()
    {
        constexpr Target exactlySpentTank{CANNON_DAMAGE, TANK_COSTS};
        constexpr float exactThreat = 3000.0f;
        checkThreat(threatValue(CANNON_DAMAGE, exactlySpentTank), exactThreat, "damage equal to the remaining hp");
    }

    void testHealingSourceStaysABonus()
    {
        constexpr float healBonus = -1200.0f;
        checkThreat(threatValue(CRYSTAL_HEAL_DAMAGE, HEALTHY_TANK), healBonus, "healing source bonus");
        checkThreat(threatValue(CRYSTAL_HEAL_DAMAGE, DYING_TANK), healBonus, "healing source on a dying target");
    }

    void testHarmlessSourceIsNoThreat()
    {
        checkThreat(threatValue(NO_DAMAGE, HEALTHY_TANK), 0.0f, "harmless source");
    }

    void testThreatNeverExceedsTheTargetValue()
    {
        bool allFinite = true;
        bool allWithinTargetValue = true;
        for (std::int32_t damage = MIN_SWEEP_DAMAGE; damage <= MAX_SWEEP_DAMAGE; ++damage)
        {
            for (std::int32_t hp = MIN_SWEEP_HP; hp <= MAX_SWEEP_HP; ++hp)
            {
                const Target target{static_cast<double>(hp), TANK_COSTS};
                const float threat = threatValue(static_cast<double>(damage), target);
                allFinite = allFinite && std::isfinite(threat);
                if (damage >= 0)
                {
                    allWithinTargetValue = allWithinTargetValue && threat <= static_cast<float>(TANK_COSTS);
                }
            }
        }
        check(allFinite, "sweep produced a non finite threat");
        check(allWithinTargetValue, "sweep threat exceeds the target value");
    }
}

int main()
{
    testCrippledTargetIsNotOverpriced();
    testTargetWithNoHpLeftIsNoThreat();
    testDamageUnderTheCapIsUnchanged();
    testDamageEqualToTheRemainingHpIsUnclamped();
    testHealingSourceStaysABonus();
    testHarmlessSourceIsNoThreat();
    testThreatNeverExceedsTheTargetValue();
    return failureCount == 0 ? 0 : 1;
}
