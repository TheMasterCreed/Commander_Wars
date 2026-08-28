#pragma once

#include <cstdint>

namespace BuildingThreat
{
    // Mirrors Unit::MAX_UNIT_HP.
    constexpr double HP_SCALE = 10.0;

    struct Target
    {
        double hp{0.0};
        std::int32_t costs{0};
    };

    // A target can only lose the hp it still has, so it caps the threat.
    constexpr float threatValue(double sourceDamage, const Target & target)
    {
        float damage = sourceDamage;
        if (damage > target.hp)
        {
            damage = target.hp;
        }
        return damage / HP_SCALE * target.costs;
    }
}
