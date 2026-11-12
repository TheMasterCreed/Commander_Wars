#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/ownershipschedule.h"

namespace Coordinator
{
    struct OccupantAt
    {
        std::int32_t unitIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
    };

    struct PlannedMove
    {
        std::int32_t unitIndex{NO_UNIT};
        TilePoint origin{INVALID_TILE};
        TilePoint destination{INVALID_TILE};
    };

    // Holds our and allied units after our plan's moves; enemy units are expected to vacate their tiles.
    class PlannedOccupancy
    {
    public:
        PlannedOccupancy(std::int32_t width, std::int32_t height, std::span<const OccupantAt> ours,
                         std::span<const PlannedMove> moves)
            : m_width(width),
              m_height(height),
              m_occupants(static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)),
                          NO_UNIT)
        {
            for (const OccupantAt & unit : ours)
            {
                set(unit.tile, unit.unitIndex);
            }
            for (const PlannedMove & move : moves)
            {
                if (at(move.origin.x, move.origin.y) == move.unitIndex)
                {
                    set(move.origin, NO_UNIT);
                }
            }
            for (const PlannedMove & move : moves)
            {
                set(move.destination, move.unitIndex);
            }
        }

        bool onMap(std::int32_t x, std::int32_t y) const
        {
            return x >= 0 && y >= 0 && x < m_width && y < m_height;
        }

        std::int32_t at(std::int32_t x, std::int32_t y) const
        {
            if (!onMap(x, y))
            {
                return NO_UNIT;
            }
            return m_occupants[slot(x, y)];
        }

    private:
        std::size_t slot(std::int32_t x, std::int32_t y) const
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        }

        void set(const TilePoint & tile, std::int32_t unitIndex)
        {
            if (onMap(tile.x, tile.y))
            {
                m_occupants[slot(tile.x, tile.y)] = unitIndex;
            }
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::vector<std::int32_t> m_occupants;
    };

    struct FiringRange
    {
        TilePoint target{INVALID_TILE};
        std::int32_t minRange{0};
        std::int32_t maxRange{0};
    };

    constexpr bool firingTileBeats(std::int32_t cost, const TilePoint & tile, std::int32_t bestCost, const TilePoint & best)
    {
        if (bestCost == UNREACHABLE)
        {
            return true;
        }
        if (cost != bestCost)
        {
            return cost < bestCost;
        }
        if (tile.y != best.y)
        {
            return tile.y < best.y;
        }
        return tile.x < best.x;
    }

    // reachCost(x, y) is the movement cost or UNREACHABLE; visit(tile, cost) sees every free reachable tile in range.
    template <typename ReachCost, typename Visit>
    void forEachFreeFiringTile(const FiringRange & range, const PlannedOccupancy & occupancy, ReachCost && reachCost,
                               Visit && visit)
    {
        for (std::int32_t y = range.target.y - range.maxRange; y <= range.target.y + range.maxRange; ++y)
        {
            for (std::int32_t x = range.target.x - range.maxRange; x <= range.target.x + range.maxRange; ++x)
            {
                const TilePoint tile{x, y};
                if (!occupancy.onMap(x, y) || occupancy.at(x, y) != NO_UNIT ||
                    !inFireRange(manhattanDistance(tile, range.target), range.minRange, range.maxRange))
                {
                    continue;
                }
                const std::int32_t cost = reachCost(x, y);
                if (cost != UNREACHABLE)
                {
                    visit(tile, cost);
                }
            }
        }
    }

    // Cheapest wins, then the lowest row, then the lowest column.
    template <typename ReachCost>
    TilePoint cheapestFiringTile(const FiringRange & range, const PlannedOccupancy & occupancy, ReachCost && reachCost)
    {
        TilePoint best = INVALID_TILE;
        std::int32_t bestCost = UNREACHABLE;
        forEachFreeFiringTile(range, occupancy, reachCost, [&](const TilePoint & tile, std::int32_t cost)
        {
            if (firingTileBeats(cost, tile, bestCost, best))
            {
                best = tile;
                bestCost = cost;
            }
        });
        return best;
    }

    inline std::int32_t distinctTileCount(std::vector<TilePoint> tiles)
    {
        const auto before = [](const TilePoint & left, const TilePoint & right)
        {
            return left.y != right.y ? left.y < right.y : left.x < right.x;
        };
        std::sort(tiles.begin(), tiles.end(), before);
        return static_cast<std::int32_t>(std::unique(tiles.begin(), tiles.end()) - tiles.begin());
    }

    // Allies stay where they stand and block a firing tile like our own units; cargo blocks nothing.
    constexpr bool blocksEnemyFiringTile(Relation relation, bool loaded)
    {
        return !loaded && (relation == Relation::Own || relation == Relation::Allied);
    }

    // Mirrors the capture candidate: only this actor's own progress on this property carries over.
    constexpr std::int32_t carriedCapturePoints(std::int32_t capturerIndex, std::int32_t capturePoints,
                                                std::int32_t actorIndex)
    {
        return capturerIndex == actorIndex ? capturePoints : 0;
    }

    constexpr bool shooterMustMove(const TilePoint & shooter, const TilePoint & target, std::int32_t minRange,
                                   std::int32_t maxRange)
    {
        return stationaryShotDistance(shooter, target, minRange, maxRange) == NO_SHOT_DISTANCE;
    }

    constexpr bool shooterCapturingHere(std::int32_t capturerIndex, std::int32_t capturePoints, std::int32_t shooterIndex)
    {
        return capturerIndex == shooterIndex && capturePoints > 0;
    }

    // From the shooter's side only our and neutral properties are surely its to capture.
    constexpr bool shooterCouldCaptureInPlace(bool canCapture, bool capturable, OwnerSign ownerFromOurSide)
    {
        return canCapture && capturable && ownerFromOurSide != OwnerSign::Enemy;
    }
}
