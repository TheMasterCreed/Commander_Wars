#include <cstdint>
#include <cstdio>
#include <vector>

#include "ai/coordinator/mobilityfield.h"

namespace
{
    using Coordinator::DistanceField;
    using Coordinator::EntryDirection;
    using Coordinator::FieldExpansion;
    using Coordinator::IMPASSABLE_COST;
    using Coordinator::MobilityCostGrid;
    using Coordinator::TilePoint;
    using Coordinator::UNREACHABLE;

    constexpr std::int32_t SMALL_WIDTH = 3;
    constexpr std::int32_t SMALL_HEIGHT = 3;
    constexpr std::int32_t ROW_WIDTH = 3;
    constexpr std::int32_t ROW_HEIGHT = 1;
    constexpr std::int32_t RANDOM_WIDTH = 6;
    constexpr std::int32_t RANDOM_HEIGHT = 5;
    constexpr std::int32_t PLAIN_COST = 1;
    constexpr std::int32_t STEEP_COST = 5;
    constexpr std::int32_t GATE_COST = 3;
    constexpr std::int32_t DETOUR_COST = 6;
    constexpr std::int32_t CORNER_DISTANCE = 4;
    constexpr std::int32_t HUGE_ALLOWANCE = 1000;
    constexpr std::int32_t OFF_GRID = 5;
    constexpr std::uint32_t LCG_MULTIPLIER = 1664525u;
    constexpr std::uint32_t LCG_INCREMENT = 1013904223u;
    constexpr std::uint32_t LCG_SEED = 20260822u;
    constexpr std::uint32_t RANDOM_COST_CHOICES = 4;

    int failures = 0;

    void expect(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    MobilityCostGrid uniformGrid(std::int32_t width, std::int32_t height, std::int32_t cost)
    {
        MobilityCostGrid grid(width, height);
        for (std::int32_t y = 0; y < height; ++y)
        {
            for (std::int32_t x = 0; x < width; ++x)
            {
                grid.setTileCost(x, y, cost);
            }
        }
        return grid;
    }

    void testUniformGrid()
    {
        const MobilityCostGrid grid = uniformGrid(SMALL_WIDTH, SMALL_HEIGHT, PLAIN_COST);
        const DistanceField field = DistanceField::build(grid, {{0, 0}}, FieldExpansion::FromSources);
        expect(field.distance(0, 0) == 0, "source sits at distance zero");
        expect(field.distance(2, 2) == CORNER_DISTANCE, "far corner is four plain steps away");
        expect(field.distance(1, 1) == 2, "center is two plain steps away");
        expect(field.reachable(1, 1, 2), "center reachable with two movement points");
        expect(!field.reachable(2, 2, CORNER_DISTANCE - 1), "corner not reachable with three movement points");
        expect(field.reachable(2, 2, CORNER_DISTANCE), "corner reachable with four movement points");
    }

    void testWallForcesDetour()
    {
        MobilityCostGrid grid = uniformGrid(SMALL_WIDTH, SMALL_HEIGHT, PLAIN_COST);
        grid.setTileCost(1, 0, IMPASSABLE_COST);
        grid.setTileCost(1, 1, IMPASSABLE_COST);
        const DistanceField field = DistanceField::build(grid, {{0, 0}}, FieldExpansion::FromSources);
        expect(field.distance(2, 0) == DETOUR_COST, "wall forces the six step detour");
        expect(field.distance(1, 0) == UNREACHABLE, "impassable tile is unreachable");
        expect(!field.reachable(1, 0, HUGE_ALLOWANCE), "impassable tile never reachable");
    }

    void testDirectionalCosts()
    {
        MobilityCostGrid grid = uniformGrid(ROW_WIDTH, ROW_HEIGHT, PLAIN_COST);
        grid.setCost(1, 0, EntryDirection::East, STEEP_COST);
        const DistanceField fromLeft = DistanceField::build(grid, {{0, 0}}, FieldExpansion::FromSources);
        const DistanceField fromRight = DistanceField::build(grid, {{2, 0}}, FieldExpansion::FromSources);
        expect(fromLeft.distance(1, 0) == STEEP_COST, "entering eastwards pays the steep cost");
        expect(fromRight.distance(1, 0) == PLAIN_COST, "entering westwards pays the plain cost");
        expect(fromLeft.distance(2, 0) == STEEP_COST + PLAIN_COST, "steep entry carries into the next tile");
    }

    void testToTargetsChargesEnteredTile()
    {
        MobilityCostGrid grid = uniformGrid(ROW_WIDTH, ROW_HEIGHT, PLAIN_COST);
        grid.setCost(0, 0, EntryDirection::West, GATE_COST);
        grid.setCost(1, 0, EntryDirection::East, STEEP_COST);
        const DistanceField toTarget = DistanceField::build(grid, {{0, 0}}, FieldExpansion::ToTargets);
        expect(toTarget.distance(0, 0) == 0, "target sits at distance zero");
        expect(toTarget.distance(1, 0) == GATE_COST, "mover next to the target pays the target entry cost");
        expect(toTarget.distance(2, 0) == GATE_COST + PLAIN_COST, "mover two tiles away pays both entries");
        const DistanceField fromSource = DistanceField::build(grid, {{0, 0}}, FieldExpansion::FromSources);
        expect(fromSource.distance(1, 0) == STEEP_COST, "the same grid read forwards pays the steep east entry");
    }

    void testSourceOnImpassableTile()
    {
        MobilityCostGrid grid = uniformGrid(ROW_WIDTH, ROW_HEIGHT, PLAIN_COST);
        grid.setTileCost(0, 0, IMPASSABLE_COST);
        const DistanceField field = DistanceField::build(grid, {{0, 0}}, FieldExpansion::FromSources);
        expect(field.distance(0, 0) == 0, "a unit already standing on an impassable tile is at zero");
        expect(field.distance(1, 0) == PLAIN_COST, "it can still leave the tile");
    }

    void testMultiSourceAndSourceHygiene()
    {
        const MobilityCostGrid grid = uniformGrid(SMALL_WIDTH, SMALL_HEIGHT, PLAIN_COST);
        const DistanceField field = DistanceField::build(grid, {{0, 0}, {2, 2}}, FieldExpansion::FromSources);
        expect(field.distance(1, 1) == 2, "center is two steps from either source");
        expect(field.distance(2, 1) == PLAIN_COST, "tile next to the second source is one step away");
        expect(field.distance(2, 2) == 0, "second source sits at distance zero");
        const DistanceField messy = DistanceField::build(grid, {{OFF_GRID, OFF_GRID}, {0, 0}, {0, 0}},
                                                         FieldExpansion::FromSources);
        expect(messy.distance(2, 2) == CORNER_DISTANCE, "off grid and duplicate sources change nothing");
        expect(messy.distance(OFF_GRID, OFF_GRID) == UNREACHABLE, "off grid query is unreachable");
        expect(!messy.reachable(-1, 0, HUGE_ALLOWANCE), "negative coordinates are never reachable");
    }

    void testGridEquality()
    {
        MobilityCostGrid first = uniformGrid(SMALL_WIDTH, SMALL_HEIGHT, PLAIN_COST);
        MobilityCostGrid second = uniformGrid(SMALL_WIDTH, SMALL_HEIGHT, PLAIN_COST);
        expect(first == second, "identical grids compare equal");
        second.setCost(1, 1, EntryDirection::North, STEEP_COST);
        expect(first != second, "one differing entry breaks equality");
        const MobilityCostGrid other = uniformGrid(ROW_WIDTH, ROW_HEIGHT, PLAIN_COST);
        expect(first != other, "different sizes never compare equal");
    }

    std::int32_t nextRandomCost(std::uint32_t & state)
    {
        state = state * LCG_MULTIPLIER + LCG_INCREMENT;
        const std::uint32_t choice = (state >> 16u) % RANDOM_COST_CHOICES;
        if (choice == 0)
        {
            return IMPASSABLE_COST;
        }
        return static_cast<std::int32_t>(choice);
    }

    void testReversalIdentity()
    {
        MobilityCostGrid grid(RANDOM_WIDTH, RANDOM_HEIGHT);
        std::uint32_t state = LCG_SEED;
        for (std::int32_t y = 0; y < RANDOM_HEIGHT; ++y)
        {
            for (std::int32_t x = 0; x < RANDOM_WIDTH; ++x)
            {
                grid.setCost(x, y, EntryDirection::West, nextRandomCost(state));
                grid.setCost(x, y, EntryDirection::East, nextRandomCost(state));
                grid.setCost(x, y, EntryDirection::South, nextRandomCost(state));
                grid.setCost(x, y, EntryDirection::North, nextRandomCost(state));
            }
        }
        bool identityHolds = true;
        bool sawReachablePair = false;
        for (std::int32_t sy = 0; sy < RANDOM_HEIGHT; ++sy)
        {
            for (std::int32_t sx = 0; sx < RANDOM_WIDTH; ++sx)
            {
                const DistanceField forward = DistanceField::build(grid, {{sx, sy}}, FieldExpansion::FromSources);
                for (std::int32_t ty = 0; ty < RANDOM_HEIGHT; ++ty)
                {
                    for (std::int32_t tx = 0; tx < RANDOM_WIDTH; ++tx)
                    {
                        const DistanceField backward = DistanceField::build(grid, {{tx, ty}}, FieldExpansion::ToTargets);
                        if (forward.distance(tx, ty) != backward.distance(sx, sy))
                        {
                            identityHolds = false;
                        }
                        if (forward.distance(tx, ty) > 0)
                        {
                            sawReachablePair = true;
                        }
                    }
                }
            }
        }
        expect(identityHolds, "to targets field mirrors the from sources field on a random directional grid");
        expect(sawReachablePair, "random grid contains reachable pairs");
    }
}

int main()
{
    testUniformGrid();
    testWallForcesDetour();
    testDirectionalCosts();
    testToTargetsChargesEnteredTile();
    testSourceOnImpassableTile();
    testMultiSourceAndSourceHygiene();
    testGridEquality();
    testReversalIdentity();
    return failures == 0 ? 0 : 1;
}
