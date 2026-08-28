#pragma once

#include <cstdint>

namespace WeaponRangeCheck
{
enum class WeaponType : std::int32_t
{
    Both = 0,
    Direct = 1,
    Indirect = 2,
};

enum class RangeCheck : std::int32_t
{
    All = 0,
    None = 1,
    OnlyMin = 2,
    OnlyMax = 3,
};

constexpr std::int32_t DIRECT_RANGE = 1;

struct Slot
{
    bool hasWeapon{false};
    bool hasAmmo{false};

    constexpr bool isUsable() const
    {
        return hasWeapon && hasAmmo;
    }
};

constexpr bool isInRange(std::int32_t distance,
                         std::int32_t minRange,
                         std::int32_t maxRange,
                         RangeCheck rangeCheck)
{
    const bool minSatisfied =
        distance >= minRange || rangeCheck == RangeCheck::None || rangeCheck == RangeCheck::OnlyMax;
    const bool maxSatisfied =
        distance <= maxRange || rangeCheck == RangeCheck::None || rangeCheck == RangeCheck::OnlyMin;
    return minSatisfied && maxSatisfied;
}

constexpr bool canAttack(Slot slot,
                         WeaponType weaponType,
                         std::int32_t distance,
                         std::int32_t minRange,
                         std::int32_t maxRange,
                         RangeCheck rangeCheck)
{
    if (!slot.isUsable())
    {
        return false;
    }

    const bool inIndirectRange = isInRange(distance, minRange, maxRange, rangeCheck);
    if (weaponType == WeaponType::Both)
    {
        return inIndirectRange || distance == DIRECT_RANGE;
    }
    return (weaponType == WeaponType::Direct && distance == DIRECT_RANGE) ||
           rangeCheck == RangeCheck::None ||
           (weaponType == WeaponType::Indirect && inIndirectRange);
}
}
