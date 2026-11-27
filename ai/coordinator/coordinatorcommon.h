#pragma once

#include <cstdint>

namespace Coordinator
{
    constexpr std::int32_t NO_OWNER = -1;
    constexpr std::int32_t NO_TEAM = -1;
    constexpr std::int32_t NO_UNIT = -1;
    constexpr std::int32_t TERRAIN_TARGET_ID_BASE = 100'000'000;
    constexpr std::int32_t TERRAIN_TARGET_STRIDE = 10'000;
    constexpr std::int32_t TERRAIN_HP_PER_STEP = 10;

    struct TilePoint
    {
        std::int32_t x;
        std::int32_t y;

        friend constexpr bool operator==(const TilePoint &, const TilePoint &) = default;
    };

    constexpr TilePoint INVALID_TILE{-1, -1};

    constexpr bool isTerrainTargetId(std::int32_t id)
    {
        return id >= TERRAIN_TARGET_ID_BASE;
    }

    constexpr std::int32_t encodeTerrainTargetId(TilePoint tile)
    {
        return TERRAIN_TARGET_ID_BASE + tile.y * TERRAIN_TARGET_STRIDE + tile.x;
    }

    constexpr TilePoint decodeTerrainTargetTile(std::int32_t id)
    {
        if (!isTerrainTargetId(id))
            return INVALID_TILE;
        const std::int32_t offset = id - TERRAIN_TARGET_ID_BASE;
        return TilePoint{offset % TERRAIN_TARGET_STRIDE, offset / TERRAIN_TARGET_STRIDE};
    }

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

    using UiEventPumpFn = void (*)();

    inline UiEventPumpFn & uiEventPumpHook()
    {
        static UiEventPumpFn hook = nullptr;
        return hook;
    }

    inline void pumpUiEventsIfDue()
    {
        if (const UiEventPumpFn hook = uiEventPumpHook(); hook != nullptr)
            hook();
    }
}
