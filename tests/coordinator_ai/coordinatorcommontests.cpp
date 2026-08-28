#include "ai/coordinator/coordinatorcommon.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    using Coordinator::ComponentKind;
    using Coordinator::INVALID_TILE;
    using Coordinator::PlanBundleKind;
    using Coordinator::TilePoint;

    static_assert(Coordinator::NO_OWNER == -1);
    static_assert(Coordinator::NO_TEAM == -1);
    static_assert(Coordinator::NO_UNIT == -1);
    static_assert(INVALID_TILE == TilePoint{-1, -1});
    static_assert(TilePoint{3, 5} == TilePoint{3, 5});
    static_assert(TilePoint{3, 5} != TilePoint{5, 3});
    static_assert(static_cast<std::int8_t>(ComponentKind::Fire) == 0);
    static_assert(static_cast<std::int8_t>(ComponentKind::Capture) == 1);
    static_assert(static_cast<std::int8_t>(ComponentKind::Service) == 2);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Wait) == 0);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Move) == 1);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Fire) == 2);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::MoveAndFire) == 3);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Capture) == 4);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::MoveAndCapture) == 5);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Service) == 6);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::MoveAndService) == 7);
    static_assert(static_cast<std::int8_t>(PlanBundleKind::Compound) == 8);

    int failureCount = 0;

    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void testCoordinatesPreserveSignedValues()
    {
        constexpr TilePoint point{-7, 11};
        check(point.x == -7, "tile x changed");
        check(point.y == 11, "tile y changed");
    }

    void testSentinelsRemainOutsideValidIdentitySpace()
    {
        check(Coordinator::NO_OWNER < 0, "owner sentinel became valid");
        check(Coordinator::NO_TEAM < 0, "team sentinel became valid");
        check(Coordinator::NO_UNIT < 0, "unit sentinel became valid");
    }
}

int main()
{
    testCoordinatesPreserveSignedValues();
    testSentinelsRemainOutsideValidIdentitySpace();
    return failureCount == 0 ? 0 : 1;
}
