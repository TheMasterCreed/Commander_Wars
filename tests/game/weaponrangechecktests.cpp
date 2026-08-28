#include "game/weaponrangecheck.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
using WeaponRangeCheck::canAttack;
using WeaponRangeCheck::DIRECT_RANGE;
using WeaponRangeCheck::isInRange;
using WeaponRangeCheck::RangeCheck;
using WeaponRangeCheck::Slot;
using WeaponRangeCheck::WeaponType;

constexpr Slot ARMED{true, true};
constexpr Slot NO_WEAPON{false, true};
constexpr Slot NO_AMMO{true, false};
constexpr Slot EMPTY_SLOT{false, false};

constexpr std::int32_t INDIRECT_MIN_RANGE = 2;
constexpr std::int32_t INDIRECT_MAX_RANGE = 3;
constexpr std::int32_t DIRECT_MIN_RANGE = 1;
constexpr std::int32_t DIRECT_MAX_RANGE = 1;

constexpr std::int32_t ADJACENT = 1;
constexpr std::int32_t TWO_TILES = 2;
constexpr std::int32_t THREE_TILES = 3;
constexpr std::int32_t FOUR_TILES = 4;

int failureCount = 0;

void check(bool actual, bool expected, std::string_view message)
{
    if (actual != expected)
    {
        std::cerr << message << ": expected " << (expected ? "true" : "false")
                  << ", got " << (actual ? "true" : "false") << '\n';
        ++failureCount;
    }
}

void testUnusableSlotNeverAttacks()
{
    check(canAttack(NO_WEAPON, WeaponType::Both, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "empty weapon id adjacent");
    check(canAttack(NO_AMMO, WeaponType::Both, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "spent ammo adjacent");
    check(canAttack(EMPTY_SLOT, WeaponType::Both, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "absent slot adjacent");
}

void testUnusableSlotIgnoresPermissivePaths()
{
    check(canAttack(EMPTY_SLOT, WeaponType::Direct, FOUR_TILES, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, RangeCheck::None),
          false, "absent slot with range check disabled");
    check(canAttack(NO_WEAPON, WeaponType::Indirect, TWO_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "empty weapon id inside indirect range");
    check(canAttack(NO_AMMO, WeaponType::Both, TWO_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "spent ammo inside indirect range");
}

void testArmedIndirectKeepsItsMinimumRange()
{
    check(canAttack(ARMED, WeaponType::Indirect, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "armed indirect cannot hit an adjacent tile");
    check(canAttack(ARMED, WeaponType::Indirect, TWO_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed indirect at its minimum range");
    check(canAttack(ARMED, WeaponType::Indirect, THREE_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed indirect at its maximum range");
    check(canAttack(ARMED, WeaponType::Indirect, FOUR_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "armed indirect past its maximum range");
}

void testArmedDirectOnlyReachesAdjacent()
{
    check(canAttack(ARMED, WeaponType::Direct, ADJACENT, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed direct adjacent");
    check(canAttack(ARMED, WeaponType::Direct, TWO_TILES, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, RangeCheck::All),
          false, "armed direct past adjacency");
    check(canAttack(ARMED, WeaponType::Direct, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed direct still reaches adjacency below minimum");
    check(canAttack(ARMED, WeaponType::Direct, TWO_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "armed direct cannot borrow indirect range");
}

void testArmedBothKeepsItsPermissiveAdjacency()
{
    check(canAttack(ARMED, WeaponType::Both, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed both adjacent below minimum");
    check(canAttack(ARMED, WeaponType::Both, TWO_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          true, "armed both inside range");
    check(canAttack(ARMED, WeaponType::Both, FOUR_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::All),
          false, "armed both past maximum");
}

void testDisabledRangeCheckAcceptsEveryArmedType()
{
    check(canAttack(ARMED, WeaponType::Both, FOUR_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::None),
          true, "range check disabled for both");
    check(canAttack(ARMED, WeaponType::Direct, FOUR_TILES, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, RangeCheck::None),
          true, "range check disabled for direct");
    check(canAttack(ARMED, WeaponType::Indirect, ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::None),
          true, "range check disabled for indirect");
}

void testPartialRangeChecksDropOneBound()
{
    check(isInRange(FOUR_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::OnlyMin),
          true, "only min ignores maximum");
    check(isInRange(ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::OnlyMin),
          false, "only min enforces minimum");
    check(isInRange(ADJACENT, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::OnlyMax),
          true, "only max ignores minimum");
    check(isInRange(FOUR_TILES, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE, RangeCheck::OnlyMax),
          false, "only max enforces maximum");
}

static_assert(DIRECT_RANGE == ADJACENT);
static_assert(ARMED.isUsable());
static_assert(!NO_WEAPON.isUsable());
static_assert(!NO_AMMO.isUsable());
static_assert(!EMPTY_SLOT.isUsable());
}

int main()
{
    testUnusableSlotNeverAttacks();
    testUnusableSlotIgnoresPermissivePaths();
    testArmedIndirectKeepsItsMinimumRange();
    testArmedDirectOnlyReachesAdjacent();
    testArmedBothKeepsItsPermissiveAdjacency();
    testDisabledRangeCheckAcceptsEveryArmedType();
    testPartialRangeChecksDropOneBound();
    return failureCount == 0 ? 0 : 1;
}
