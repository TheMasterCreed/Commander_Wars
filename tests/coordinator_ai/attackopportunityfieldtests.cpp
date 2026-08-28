#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ai/coordinator/attackopportunityfield.h"

namespace
{
    using Coordinator::AttackerSpec;
    using Coordinator::AttackOpportunityField;
    using Coordinator::IMPASSABLE_COST;
    using Coordinator::MobilityCostGrid;
    using Coordinator::UNREACHABLE;

    constexpr std::int32_t FIELD_SIZE = 5;
    constexpr std::int32_t PLAIN_COST = 1;
    constexpr std::int32_t CENTER_X = 2;
    constexpr std::int32_t CENTER_Y = 2;
    constexpr std::int32_t MELEE_RANGE = 1;
    constexpr std::int32_t SHORT_MOVEMENT = 1;
    constexpr std::int32_t DIRECT_MOVEMENT = 2;
    constexpr std::int32_t DIRECT_THREAT_DISTANCE = 3;
    constexpr std::int32_t INDIRECT_MIN_RANGE = 2;
    constexpr std::int32_t INDIRECT_MAX_RANGE = 3;
    constexpr std::int32_t GENEROUS_MOVEMENT = 5;
    constexpr std::int32_t WALL_COLUMN = 1;
    constexpr std::int32_t WALLED_MOVER_Y = 2;
    constexpr std::int32_t RING_RANGE = 2;
    constexpr std::int32_t INVERTED_MIN_RANGE = 3;
    constexpr std::int32_t INVERTED_MAX_RANGE = 2;
    constexpr std::int32_t FIRST_UNIT_INDEX = 7;
    constexpr std::int32_t SECOND_UNIT_INDEX = 3;
    constexpr std::int32_t SHARED_TILE_X = 1;
    constexpr std::int32_t SHARED_TILE_Y = 1;
    constexpr std::size_t SHARED_TILE_ATTACKERS = 2;
    constexpr std::int32_t OFF_GRID = 7;

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

    std::int32_t manhattan(std::int32_t fromX, std::int32_t fromY, std::int32_t toX, std::int32_t toY)
    {
        const std::int32_t dx = toX - fromX;
        const std::int32_t dy = toY - fromY;
        return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }

    AttackOpportunityField buildField(const std::vector<AttackerSpec> & attackers)
    {
        return AttackOpportunityField::build(FIELD_SIZE, FIELD_SIZE, attackers);
    }

    void testDirectMoverThreatensMovementPlusRange()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField field = buildField({AttackerSpec{
            .unitIndex = FIRST_UNIT_INDEX,
            .x = CENTER_X,
            .y = CENTER_Y,
            .movementPoints = DIRECT_MOVEMENT,
            .minRange = MELEE_RANGE,
            .maxRange = MELEE_RANGE,
            .canMoveAndFire = true,
            .grid = &grid,
        }});
        bool targetsMatch = true;
        bool originsMatch = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                const std::int32_t steps = manhattan(CENTER_X, CENTER_Y, x, y);
                if (field.canAttack(0, x, y) != (steps <= DIRECT_THREAT_DISTANCE))
                {
                    targetsMatch = false;
                }
                if (field.canOriginateFrom(0, x, y) != (steps <= DIRECT_MOVEMENT))
                {
                    originsMatch = false;
                }
            }
        }
        expect(targetsMatch, "direct mover threatens exactly the tiles within movement plus range");
        expect(originsMatch, "direct mover originates exactly from the tiles it can reach");
        expect(field.canAttack(0, CENTER_X, CENTER_Y), "direct mover threatens its own tile");
        expect(field.canOriginateFrom(0, CENTER_X, CENTER_Y), "direct mover may fire without moving");
        expect(!field.threatened(0, 0), "the far corner stays outside the threat");
        expect(field.width() == FIELD_SIZE && field.height() == FIELD_SIZE, "field keeps the requested dimensions");
        expect(field.attackerCount() == 1, "one spec occupies one slot");
        expect(field.reach(0).distance(CENTER_X, CENTER_Y) == 0, "reach field is rooted at the attacker");
    }

    void testIndirectThreatensOnlyItsRing()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField field = buildField({AttackerSpec{
            .unitIndex = FIRST_UNIT_INDEX,
            .x = CENTER_X,
            .y = CENTER_Y,
            .movementPoints = GENEROUS_MOVEMENT,
            .minRange = INDIRECT_MIN_RANGE,
            .maxRange = INDIRECT_MAX_RANGE,
            .canMoveAndFire = false,
            .grid = &grid,
        }});
        bool targetsMatch = true;
        bool originsMatch = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                const std::int32_t steps = manhattan(CENTER_X, CENTER_Y, x, y);
                const bool inRing = steps >= INDIRECT_MIN_RANGE && steps <= INDIRECT_MAX_RANGE;
                if (field.canAttack(0, x, y) != inRing)
                {
                    targetsMatch = false;
                }
                if (field.canOriginateFrom(0, x, y) != (x == CENTER_X && y == CENTER_Y))
                {
                    originsMatch = false;
                }
            }
        }
        expect(targetsMatch, "indirect threatens exactly its own range ring");
        expect(originsMatch, "indirect originates only from its own tile despite the movement points");
        expect(!field.threatened(CENTER_X, CENTER_Y), "indirect cannot hit its own tile");
        expect(!field.threatened(CENTER_X + 1, CENTER_Y), "indirect cannot hit the adjacent tile");
        expect(field.threatened(CENTER_X, CENTER_Y - INDIRECT_MIN_RANGE), "the near ring edge is threatened");
    }

    void testImpassableTilesCutReach()
    {
        MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            grid.setTileCost(WALL_COLUMN, y, IMPASSABLE_COST);
        }
        const AttackOpportunityField field = buildField({AttackerSpec{
            .unitIndex = FIRST_UNIT_INDEX,
            .x = 0,
            .y = WALLED_MOVER_Y,
            .movementPoints = DIRECT_MOVEMENT,
            .minRange = MELEE_RANGE,
            .maxRange = MELEE_RANGE,
            .canMoveAndFire = true,
            .grid = &grid,
        }});
        bool targetsMatch = true;
        bool originsMatch = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                if (field.canAttack(0, x, y) != (x <= WALL_COLUMN))
                {
                    targetsMatch = false;
                }
                if (field.canOriginateFrom(0, x, y) != (x == 0))
                {
                    originsMatch = false;
                }
            }
        }
        expect(targetsMatch, "the wall caps targets at the column the mover can still shoot into");
        expect(originsMatch, "the wall confines origins to the free column");
        expect(field.reach(0).distance(WALL_COLUMN, WALLED_MOVER_Y) == UNREACHABLE, "the wall tile is unreachable");
        expect(field.canAttack(0, WALL_COLUMN, WALLED_MOVER_Y), "an unreachable tile is still attackable");
    }

    std::vector<AttackerSpec> pairedAttackers(const MobilityCostGrid & grid)
    {
        return {
            AttackerSpec{
                .unitIndex = FIRST_UNIT_INDEX,
                .x = 0,
                .y = 0,
                .movementPoints = SHORT_MOVEMENT,
                .minRange = MELEE_RANGE,
                .maxRange = MELEE_RANGE,
                .canMoveAndFire = true,
                .grid = &grid,
            },
            AttackerSpec{
                .unitIndex = SECOND_UNIT_INDEX,
                .x = CENTER_X,
                .y = CENTER_Y,
                .movementPoints = 0,
                .minRange = RING_RANGE,
                .maxRange = RING_RANGE,
                .canMoveAndFire = false,
                .grid = &grid,
            },
        };
    }

    bool tileListAgreesWithQueries(const AttackOpportunityField & field, std::int32_t x, std::int32_t y)
    {
        const std::span<const std::int32_t> slots = field.attackerSlotsAt(x, y);
        std::int32_t previous = -1;
        for (const std::int32_t slot : slots)
        {
            if (slot <= previous || !field.canAttack(slot, x, y))
            {
                return false;
            }
            previous = slot;
        }
        std::size_t attacking = 0;
        for (std::int32_t slot = 0; slot < field.attackerCount(); ++slot)
        {
            if (field.canAttack(slot, x, y))
            {
                ++attacking;
            }
        }
        return attacking == slots.size() && field.threatened(x, y) == !slots.empty();
    }

    void testTwoAttackersShareTiles()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField field = buildField(pairedAttackers(grid));
        bool listsAgree = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                if (!tileListAgreesWithQueries(field, x, y))
                {
                    listsAgree = false;
                }
            }
        }
        expect(listsAgree, "every tile list is ascending and agrees with canAttack and threatened");
        const std::span<const std::int32_t> shared = field.attackerSlotsAt(SHARED_TILE_X, SHARED_TILE_Y);
        expect(shared.size() == SHARED_TILE_ATTACKERS, "the contested tile lists both attackers");
        expect(shared.size() == SHARED_TILE_ATTACKERS && shared[0] == 0 && shared[1] == 1, "the contested tile lists them in slot order");
        expect(field.attacker(0).unitIndex == FIRST_UNIT_INDEX, "the first slot echoes its unit index");
        expect(field.attacker(1).unitIndex == SECOND_UNIT_INDEX, "the second slot echoes its unit index");
        expect(field.attacker(1).x == CENTER_X && field.attacker(1).y == CENTER_Y, "specs are echoed unchanged");
    }

    void testOffGridQueriesAndDegenerateSpecs()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField field = buildField({
            AttackerSpec{
                .unitIndex = FIRST_UNIT_INDEX,
                .x = OFF_GRID,
                .y = OFF_GRID,
                .movementPoints = DIRECT_MOVEMENT,
                .minRange = MELEE_RANGE,
                .maxRange = MELEE_RANGE,
                .canMoveAndFire = true,
                .grid = &grid,
            },
            AttackerSpec{
                .unitIndex = SECOND_UNIT_INDEX,
                .x = CENTER_X,
                .y = CENTER_Y,
                .movementPoints = DIRECT_MOVEMENT,
                .minRange = MELEE_RANGE,
                .maxRange = MELEE_RANGE,
                .canMoveAndFire = true,
                .grid = nullptr,
            },
        });
        expect(field.attackerCount() == 2, "degenerate specs still occupy their slots");
        bool silent = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                for (std::int32_t slot = 0; slot < field.attackerCount(); ++slot)
                {
                    if (field.canAttack(slot, x, y) || field.canOriginateFrom(slot, x, y))
                    {
                        silent = false;
                    }
                }
                if (field.threatened(x, y))
                {
                    silent = false;
                }
            }
        }
        expect(silent, "an off grid placement and a null grid contribute nothing");
        expect(field.reach(1).distance(CENTER_X, CENTER_Y) == UNREACHABLE, "a null grid leaves an empty reach field");
        expect(field.attackerSlotsAt(-1, 0).empty(), "a negative query is empty");
        expect(field.attackerSlotsAt(FIELD_SIZE, FIELD_SIZE).empty(), "a query past the edge is empty");
        expect(!field.canAttack(0, -1, 0), "an off grid tile is never attackable");
        expect(!field.canAttack(-1, CENTER_X, CENTER_Y), "a negative slot is never attackable");
        expect(!field.canAttack(field.attackerCount(), CENTER_X, CENTER_Y), "a slot past the end is never attackable");
        expect(!field.canOriginateFrom(field.attackerCount(), CENTER_X, CENTER_Y), "a slot past the end never originates");
        expect(!field.threatened(OFF_GRID, OFF_GRID), "an off grid tile is never threatened");
    }

    void testInvertedRangeHasNoTargets()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField field = buildField({AttackerSpec{
            .unitIndex = FIRST_UNIT_INDEX,
            .x = CENTER_X,
            .y = CENTER_Y,
            .movementPoints = DIRECT_MOVEMENT,
            .minRange = INVERTED_MIN_RANGE,
            .maxRange = INVERTED_MAX_RANGE,
            .canMoveAndFire = true,
            .grid = &grid,
        }});
        bool silent = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                if (field.threatened(x, y))
                {
                    silent = false;
                }
            }
        }
        expect(silent, "a maximum range below the minimum yields no targets");
        expect(field.canOriginateFrom(0, CENTER_X, CENTER_Y), "the mover keeps its origins");
    }

    void testRepeatedBuildsMatch()
    {
        const MobilityCostGrid grid = uniformGrid(FIELD_SIZE, FIELD_SIZE, PLAIN_COST);
        const AttackOpportunityField first = buildField(pairedAttackers(grid));
        const AttackOpportunityField second = buildField(pairedAttackers(grid));
        bool identical = true;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                const std::span<const std::int32_t> left = first.attackerSlotsAt(x, y);
                const std::span<const std::int32_t> right = second.attackerSlotsAt(x, y);
                if (left.size() != right.size())
                {
                    identical = false;
                    continue;
                }
                for (std::size_t entry = 0; entry < left.size(); ++entry)
                {
                    if (left[entry] != right[entry])
                    {
                        identical = false;
                    }
                }
            }
        }
        expect(identical, "repeated builds from the same inputs produce identical tile lists");
    }
}

int main()
{
    testDirectMoverThreatensMovementPlusRange();
    testIndirectThreatensOnlyItsRing();
    testImpassableTilesCutReach();
    testTwoAttackersShareTiles();
    testOffGridQueriesAndDegenerateSpecs();
    testInvertedRangeHasNoTargets();
    testRepeatedBuildsMatch();
    return failures == 0 ? 0 : 1;
}
