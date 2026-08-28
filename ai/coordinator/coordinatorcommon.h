#pragma once

#include <cstdint>

namespace Coordinator
{
    constexpr std::int32_t NO_OWNER = -1;
    constexpr std::int32_t NO_TEAM = -1;
    constexpr std::int32_t NO_UNIT = -1;

    struct TilePoint
    {
        std::int32_t x;
        std::int32_t y;

        friend constexpr bool operator==(const TilePoint &, const TilePoint &) = default;
    };

    constexpr TilePoint INVALID_TILE{-1, -1};

    enum class ComponentKind : std::int8_t
    {
        Fire,
        Capture,
        Service,
    };

    enum class PlanBundleKind : std::int8_t
    {
        Wait,
        Move,
        Fire,
        MoveAndFire,
        Capture,
        MoveAndCapture,
        Service,
        MoveAndService,
        Compound,
    };
}
