#pragma once

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/mobilityfield.h"
#include <set>

namespace Coordinator
{
    using HeadquartersTransitTiles = std::set<std::pair<int, int>>;

    // An admission test, using the same two enemy activations as the HQ watch.
    // A posted unit matters here only if blocking its tile changes the earliest
    // arrival on the terrain grid. The economic comparison still owns combat,
    // powers, other occupants and whether a legal team can maintain that post.
    inline void addHeadquartersTransitTiles(HeadquartersTransitTiles &posts, const MobilityCostGrid &grid,
        TilePoint from, TilePoint home, int movement)
    {
        if (movement <= 0 || from == home || manhattanDistance(from, home) > 2LL * movement) return;
        ActivationField reach;
        const std::vector<TilePoint> goal{home};
        reach.build(grid, from, movement, 2, goal);
        const int available = reach.activations(home.x, home.y);
        if (available <= 0) return;
        const auto length = static_cast<std::int64_t>(available) * movement;
        for (int y = 0; y < grid.height(); ++y)
            for (int x = 0; x < grid.width(); ++x)
            {
                const TilePoint tile{x,y};
                if (tile == from || tile == home || posts.contains({x,y}) ||
                    static_cast<std::int64_t>(manhattanDistance(from,tile)) + manhattanDistance(tile,home) > length)
                    continue;
                reach.build(grid.withBlockedTiles({tile}), from, movement, 2, goal);
                if (reach.activations(home.x, home.y) != available)
                    posts.emplace(x,y);
            }
    }
}
