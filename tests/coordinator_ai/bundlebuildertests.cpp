#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/bundlebuilder.h"

namespace
{
    using Coordinator::AttackerSpec;
    using Coordinator::AttackOpportunityField;
    using Coordinator::bookDamage;
    using Coordinator::CandidateBundle;
    using Coordinator::captureIncome;
    using Coordinator::captureOwnerBefore;
    using Coordinator::capturePropertyContinuation;
    using Coordinator::CaptureFacts;
    using Coordinator::ceilHpSteps;
    using Coordinator::damageStepsFrom;
    using Coordinator::DistanceField;
    using Coordinator::appendFollowersBehind;
    using Coordinator::BundleComponent;
    using Coordinator::ComponentKind;
    using Coordinator::FieldExpansion;
    using Coordinator::FireFacts;
    using Coordinator::followTileOffered;
    using Coordinator::followTilesFor;
    using Coordinator::hpStepsAfterDamage;
    using Coordinator::inFireRange;
    using Coordinator::lostShotContinuation;
    using Coordinator::manhattanDistance;
    using Coordinator::MilliFunds;
    using Coordinator::MobilityCostGrid;
    using Coordinator::movedShotDistance;
    using Coordinator::NO_CAPTURE_TURNS;
    using Coordinator::NO_OWNER;
    using Coordinator::NO_SHOT_DISTANCE;
    using Coordinator::NO_TURN_LIMIT;
    using Coordinator::opposingSide;
    using Coordinator::OwnerSign;
    using Coordinator::PlannableUnitFacts;
    using Coordinator::PositionFacts;
    using Coordinator::positionFacts;
    using Coordinator::PropertyFacts;
    using Coordinator::PropertyIncome;
    using Coordinator::Relation;
    using Coordinator::remainingTurnLimit;
    using Coordinator::resolveHorizon;
    using Coordinator::Side;
    using Coordinator::sideOf;
    using Coordinator::stationaryShotDistance;
    using Coordinator::TilePoint;
    using Coordinator::toMilliFunds;
    using Coordinator::UNIT_HP_STEPS;
    using Coordinator::unitPlannable;

    constexpr std::int32_t NO_ENGINE_TURN_LIMIT = 0;
    constexpr std::int32_t NEGATIVE_ENGINE_TURN_LIMIT = -1;
    constexpr std::int32_t ENGINE_TURN_LIMIT = 12;
    constexpr std::int32_t EARLY_DAY = 3;
    constexpr std::int32_t REMAINING_ON_EARLY_DAY = 9;
    constexpr std::int32_t FINAL_DAY = ENGINE_TURN_LIMIT;
    constexpr std::int32_t OVERDUE_DAY = ENGINE_TURN_LIMIT + 1;

    static_assert(remainingTurnLimit(NO_ENGINE_TURN_LIMIT, EARLY_DAY) == NO_TURN_LIMIT);
    static_assert(remainingTurnLimit(NEGATIVE_ENGINE_TURN_LIMIT, EARLY_DAY) == NO_TURN_LIMIT);
    static_assert(remainingTurnLimit(ENGINE_TURN_LIMIT, EARLY_DAY) == REMAINING_ON_EARLY_DAY);
    static_assert(remainingTurnLimit(ENGINE_TURN_LIMIT, FINAL_DAY) == 0);
    static_assert(remainingTurnLimit(ENGINE_TURN_LIMIT, OVERDUE_DAY) == 0);

    constexpr double FULL_HP = 10.0;
    constexpr double HALF_HP = 5.0;
    constexpr double PARTIAL_HP = 4.5;
    constexpr double SPENT_HP = 3.0;
    constexpr double SURVIVABLE_REPORT = 55.0;
    constexpr double PARTIAL_REPORT = 30.0;
    constexpr double LETHAL_REPORT = 100.0;
    constexpr double EXACTLY_LETHAL_REPORT = 30.0;
    constexpr double IMPOSSIBLE_REPORT = -1.0;
    constexpr std::int32_t SURVIVING_HP_STEPS = 5;
    constexpr std::int32_t WOUNDED_PARTIAL_HP_STEPS = 2;

    static_assert(ceilHpSteps(PARTIAL_HP) == SURVIVING_HP_STEPS);
    static_assert(ceilHpSteps(FULL_HP) == UNIT_HP_STEPS);
    static_assert(hpStepsAfterDamage(FULL_HP, SURVIVABLE_REPORT) == SURVIVING_HP_STEPS);
    static_assert(hpStepsAfterDamage(FULL_HP, LETHAL_REPORT) == 0);
    // A report that lands exactly on zero hp is a kill, never a rounded up survivor.
    static_assert(hpStepsAfterDamage(SPENT_HP, EXACTLY_LETHAL_REPORT) == 0);
    static_assert(hpStepsAfterDamage(PARTIAL_HP, PARTIAL_REPORT) == WOUNDED_PARTIAL_HP_STEPS);
    static_assert(damageStepsFrom(FULL_HP, SURVIVABLE_REPORT) == UNIT_HP_STEPS - SURVIVING_HP_STEPS);
    static_assert(damageStepsFrom(FULL_HP, LETHAL_REPORT) == UNIT_HP_STEPS);
    static_assert(damageStepsFrom(FULL_HP, IMPOSSIBLE_REPORT) == 0);
    static_assert(damageStepsFrom(HALF_HP, LETHAL_REPORT) == SURVIVING_HP_STEPS);
    static_assert(damageStepsFrom(PARTIAL_HP, PARTIAL_REPORT) == SURVIVING_HP_STEPS - WOUNDED_PARTIAL_HP_STEPS);
    static_assert(damageStepsFrom(SPENT_HP, EXACTLY_LETHAL_REPORT) == ceilHpSteps(SPENT_HP));

    static_assert(sideOf(Relation::Own) == Side::Ours);
    static_assert(sideOf(Relation::Allied) == Side::Ours);
    static_assert(sideOf(Relation::Enemy) == Side::Theirs);
    static_assert(sideOf(Relation::Neutral) == Side::Bystander);
    static_assert(opposingSide(Side::Ours) == Side::Theirs);
    static_assert(opposingSide(Side::Theirs) == Side::Ours);
    static_assert(opposingSide(Side::Bystander) == Side::Bystander);

    constexpr TilePoint ORIGIN_TILE{2, 2};
    constexpr TilePoint DESTINATION_TILE{3, 2};
    constexpr TilePoint DISTANT_TILE{6, 5};
    constexpr std::int32_t DIRECT_MIN_RANGE = 1;
    constexpr std::int32_t DIRECT_MAX_RANGE = 1;
    constexpr std::int32_t INDIRECT_MIN_RANGE = 2;
    constexpr std::int32_t INDIRECT_MAX_RANGE = 3;

    static_assert(manhattanDistance(ORIGIN_TILE, DESTINATION_TILE) == 1);
    static_assert(manhattanDistance(ORIGIN_TILE, DISTANT_TILE) == 7);
    static_assert(manhattanDistance(ORIGIN_TILE, ORIGIN_TILE) == 0);
    static_assert(inFireRange(INDIRECT_MIN_RANGE, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE));
    static_assert(!inFireRange(DIRECT_MAX_RANGE, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE));
    static_assert(stationaryShotDistance(ORIGIN_TILE, DESTINATION_TILE, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE) == 1);
    static_assert(stationaryShotDistance(ORIGIN_TILE, DESTINATION_TILE, INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE) ==
                  NO_SHOT_DISTANCE);
    static_assert(stationaryShotDistance(ORIGIN_TILE, DISTANT_TILE, DIRECT_MIN_RANGE, DIRECT_MAX_RANGE) ==
                  NO_SHOT_DISTANCE);

    constexpr std::int32_t OWN_PLAYER = 0;
    constexpr std::int32_t ENEMY_PLAYER = 1;
    constexpr std::int32_t ALLIED_PLAYER = 2;
    constexpr std::int32_t UNRELATED_PLAYER = 3;

    static_assert(captureOwnerBefore(NO_OWNER, Relation::Neutral) == OwnerSign::Neutral);
    // A neutral property answers Neutral whatever relation the lookup reports for its absent owner.
    static_assert(captureOwnerBefore(NO_OWNER, Relation::Enemy) == OwnerSign::Neutral);
    static_assert(captureOwnerBefore(ENEMY_PLAYER, Relation::Enemy) == OwnerSign::Enemy);
    static_assert(captureOwnerBefore(OWN_PLAYER, Relation::Own) == OwnerSign::Ours);
    static_assert(captureOwnerBefore(ALLIED_PLAYER, Relation::Allied) == OwnerSign::Ours);
    static_assert(captureOwnerBefore(UNRELATED_PLAYER, Relation::Neutral) == OwnerSign::Ours);

    constexpr std::int32_t BASE_INCOME = 1000;
    constexpr std::int32_t COVERED_TILES = 2;
    constexpr std::int32_t OWNED_INCOME_PER_TURN = 1500;

    PropertyFacts propertyOwnedBy(std::int32_t ownerId, std::int32_t incomePerTurn)
    {
        PropertyFacts facts;
        facts.ownerId = ownerId;
        facts.baseIncome = BASE_INCOME;
        facts.coveredTileCount = COVERED_TILES;
        facts.incomePerTurn = incomePerTurn;
        facts.capturable = true;
        return facts;
    }

    constexpr std::int32_t CAPTURE_COMPLETES_NOW = 0;
    constexpr std::int32_t SHORT_HORIZON_TURNS = 5;
    constexpr CaptureFacts NEUTRAL_CAPTURE{
        .income = PropertyIncome{toMilliFunds(BASE_INCOME), toMilliFunds(BASE_INCOME)},
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = CAPTURE_COMPLETES_NOW,
    };

    // The final turn pays nothing for owning anything afterwards, however fast the capture lands.
    static_assert(resolveHorizon(0) == 0);
    static_assert(capturePropertyContinuation(NEUTRAL_CAPTURE, resolveHorizon(0)) == 0);
    static_assert(capturePropertyContinuation(NEUTRAL_CAPTURE, SHORT_HORIZON_TURNS) ==
                  toMilliFunds(BASE_INCOME) * SHORT_HORIZON_TURNS);

    constexpr MilliFunds TARGET_REPLACEMENT_COST = toMilliFunds(4000);
    constexpr std::int32_t DAMAGE_STEPS = 4;
    constexpr MilliFunds TARGET_BEST_SHOT_BEFORE = toMilliFunds(3000);
    constexpr MilliFunds TARGET_BEST_SHOT_AFTER = toMilliFunds(1000);
    constexpr MilliFunds PROPORTIONAL_LOSS = TARGET_BEST_SHOT_BEFORE * DAMAGE_STEPS / UNIT_HP_STEPS;

    static_assert(lostShotContinuation(TARGET_BEST_SHOT_BEFORE, TARGET_BEST_SHOT_AFTER) ==
                  TARGET_BEST_SHOT_BEFORE - TARGET_BEST_SHOT_AFTER);
    static_assert(lostShotContinuation(TARGET_BEST_SHOT_BEFORE, TARGET_BEST_SHOT_AFTER) != PROPORTIONAL_LOSS);
    static_assert(lostShotContinuation(TARGET_BEST_SHOT_BEFORE, TARGET_BEST_SHOT_BEFORE) == 0);

    static_assert(bookDamage(TARGET_REPLACEMENT_COST, UNIT_HP_STEPS) == TARGET_REPLACEMENT_COST);
    static_assert(bookDamage(TARGET_REPLACEMENT_COST, 0) == 0);
    static_assert(bookDamage(TARGET_REPLACEMENT_COST, DAMAGE_STEPS) ==
                  TARGET_REPLACEMENT_COST * DAMAGE_STEPS / UNIT_HP_STEPS);

    constexpr MilliFunds ACTOR_ORIGIN_SHOT = toMilliFunds(900);
    constexpr MilliFunds ACTOR_DESTINATION_SHOT = toMilliFunds(1500);
    constexpr MilliFunds ENEMY_ORIGIN_SHOT = toMilliFunds(300);
    constexpr MilliFunds ENEMY_DESTINATION_SHOT = toMilliFunds(1100);
    constexpr std::int32_t CANDIDATE_GROWTH_COUNT = 10;

    constexpr std::int32_t FIELD_SIZE = 9;
    constexpr std::int32_t STEP_COST = 1;
    constexpr std::int32_t BLOCKED_COLUMN = 5;
    constexpr std::int32_t SPEC_MOVEMENT_POINTS = 3;
    constexpr std::int32_t ATTACKER_SLOT = 0;

    int failures = 0;
    int checks = 0;

    void expect(bool condition, const char* description)
    {
        ++checks;
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    // A wall splits the grid, so reachability and not raw geometry has to decide the differential.
    MobilityCostGrid walledGrid()
    {
        MobilityCostGrid grid(FIELD_SIZE, FIELD_SIZE);
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                if (x != BLOCKED_COLUMN)
                {
                    grid.setTileCost(x, y, STEP_COST);
                }
            }
        }
        return grid;
    }

    void testMovedShotDistanceMatchesTheAttackField(std::int32_t minRange, std::int32_t maxRange, const char* description)
    {
        const MobilityCostGrid grid = walledGrid();
        const AttackerSpec spec{
            .unitIndex = ATTACKER_SLOT,
            .x = ORIGIN_TILE.x,
            .y = ORIGIN_TILE.y,
            .movementPoints = SPEC_MOVEMENT_POINTS,
            .minRange = minRange,
            .maxRange = maxRange,
            .canMoveAndFire = true,
            .grid = &grid,
        };
        const AttackOpportunityField field = AttackOpportunityField::build(FIELD_SIZE, FIELD_SIZE, {spec});
        const DistanceField reach = DistanceField::build(grid, {ORIGIN_TILE}, FieldExpansion::FromSources);
        bool agrees = true;
        bool sawShot = false;
        bool sawMiss = false;
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                const std::int32_t distance = movedShotDistance(reach, SPEC_MOVEMENT_POINTS, minRange, maxRange,
                                                                TilePoint{x, y});
                const bool canShoot = distance != NO_SHOT_DISTANCE;
                agrees = agrees && canShoot == field.canAttack(ATTACKER_SLOT, x, y);
                agrees = agrees && (!canShoot || inFireRange(distance, minRange, maxRange));
                sawShot = sawShot || canShoot;
                sawMiss = sawMiss || !canShoot;
            }
        }
        expect(agrees, description);
        expect(sawShot && sawMiss, "the differential covers both answers");
    }

    constexpr TilePoint SHOT_TARGET{4, 4};
    constexpr TilePoint INDIRECT_SHOOTER_TILE{4, 1};
    constexpr TilePoint MOBILE_SHOOTER_TILE{6, 5};
    constexpr TilePoint DEAD_SHOOTER_TILE{4, 6};
    constexpr TilePoint DAMAGED_SHOOTER_TILE{2, 4};
    constexpr std::int32_t MOBILE_UNIT = 0;
    constexpr std::int32_t DEAD_UNIT = 1;
    constexpr std::int32_t INDIRECT_UNIT = 2;
    constexpr std::int32_t DAMAGED_UNIT = 3;
    constexpr std::int32_t IN_RANGE_MOBILE_UNIT = 4;
    constexpr std::int32_t SHOOTER_UNIT_COUNT = 5;
    constexpr TilePoint IN_RANGE_MOBILE_TILE{4, 2};
    constexpr std::int32_t IN_RANGE_MOBILE_SLOT = 4;
    constexpr std::size_t DAMAGED_ENTRY = 2;
    constexpr std::int32_t MOBILE_HP = 7;
    constexpr std::int32_t DAMAGED_HP = 9;
    constexpr std::int32_t DAMAGED_HP_OVERRIDE = 4;
    constexpr std::int32_t FAR_MIN_RANGE = 2;
    constexpr std::int32_t FAR_MAX_RANGE = 3;
    constexpr std::int32_t INDIRECT_DISTANCE = 3;
    constexpr std::int32_t RING_DISTANCE = 2;
    constexpr std::int32_t DAMAGED_SLOT = 3;

    struct VisitedShot
    {
        std::int32_t slot;
        std::int32_t unitIndex;
        TilePoint tile;
        std::int32_t hpSteps;
        std::int32_t distance;

        friend bool operator==(const VisitedShot &, const VisitedShot &) = default;
    };

    void testEnemyShotEnumeration()
    {
        MobilityCostGrid grid(FIELD_SIZE, FIELD_SIZE);
        for (std::int32_t y = 0; y < FIELD_SIZE; ++y)
        {
            for (std::int32_t x = 0; x < FIELD_SIZE; ++x)
            {
                grid.setTileCost(x, y, STEP_COST);
            }
        }
        const auto stationary = [&](std::int32_t unitIndex, TilePoint tile, std::int32_t minRange, std::int32_t maxRange)
        {
            return AttackerSpec{.unitIndex = unitIndex, .x = tile.x, .y = tile.y, .movementPoints = 0,
                                .minRange = minRange, .maxRange = maxRange, .canMoveAndFire = false, .grid = &grid};
        };
        const std::vector<AttackerSpec> specs{
            stationary(INDIRECT_UNIT, INDIRECT_SHOOTER_TILE, FAR_MIN_RANGE, FAR_MAX_RANGE),
            AttackerSpec{.unitIndex = MOBILE_UNIT, .x = MOBILE_SHOOTER_TILE.x, .y = MOBILE_SHOOTER_TILE.y,
                         .movementPoints = SPEC_MOVEMENT_POINTS, .minRange = 1, .maxRange = 1, .canMoveAndFire = true,
                         .grid = &grid},
            stationary(DEAD_UNIT, DEAD_SHOOTER_TILE, 1, FAR_MIN_RANGE),
            stationary(DAMAGED_UNIT, DAMAGED_SHOOTER_TILE, FAR_MIN_RANGE, FAR_MIN_RANGE),
            AttackerSpec{.unitIndex = IN_RANGE_MOBILE_UNIT, .x = IN_RANGE_MOBILE_TILE.x, .y = IN_RANGE_MOBILE_TILE.y,
                         .movementPoints = SPEC_MOVEMENT_POINTS, .minRange = 1, .maxRange = FAR_MIN_RANGE,
                         .canMoveAndFire = true, .grid = &grid},
        };
        const AttackOpportunityField field = AttackOpportunityField::build(FIELD_SIZE, FIELD_SIZE, specs);
        std::vector<Coordinator::KnownUnit> units(SHOOTER_UNIT_COUNT);
        units[MOBILE_UNIT].hpRounded = MOBILE_HP;
        units[DEAD_UNIT].hpRounded = 0;
        units[INDIRECT_UNIT].hpRounded = UNIT_HP_STEPS;
        units[DAMAGED_UNIT].hpRounded = DAMAGED_HP;
        units[IN_RANGE_MOBILE_UNIT].hpRounded = UNIT_HP_STEPS;
        const auto visitAll = [&](std::int32_t damagedUnit, std::int32_t damagedHp)
        {
            std::vector<VisitedShot> visited;
            Coordinator::forEachEnemyShot(units, field, SHOT_TARGET, damagedUnit, damagedHp,
                                          [&](const Coordinator::EnemyShot & shot)
                                          {
                                              visited.push_back(VisitedShot{shot.slot, shot.unitIndex, shot.tile,
                                                                            shot.hpSteps, shot.distance});
                                          });
            return visited;
        };
        const std::vector<VisitedShot> expected{
            {0, INDIRECT_UNIT, INDIRECT_SHOOTER_TILE, UNIT_HP_STEPS, INDIRECT_DISTANCE},
            {1, MOBILE_UNIT, MOBILE_SHOOTER_TILE, MOBILE_HP, 1},
            {DAMAGED_SLOT, DAMAGED_UNIT, DAMAGED_SHOOTER_TILE, DAMAGED_HP_OVERRIDE, RING_DISTANCE},
            {IN_RANGE_MOBILE_SLOT, IN_RANGE_MOBILE_UNIT, IN_RANGE_MOBILE_TILE, UNIT_HP_STEPS, 1},
        };
        expect(field.attackerSlotsAt(SHOT_TARGET.x, SHOT_TARGET.y).size() == specs.size(),
               "every hand built shooter reaches the target");
        expect(stationaryShotDistance(IN_RANGE_MOBILE_TILE, SHOT_TARGET, 1, FAR_MIN_RANGE) == RING_DISTANCE,
               "the in range move and fire shooter could already fire from where it stands");
        expect(visitAll(DAMAGED_UNIT, DAMAGED_HP_OVERRIDE) == expected,
               "shots come in slot order, skip the dead shooter, apply the override and keep each distance");
        std::vector<VisitedShot> unchanged = expected;
        unchanged[DAMAGED_ENTRY].hpSteps = DAMAGED_HP;
        expect(visitAll(Coordinator::NO_UNIT, 0) == unchanged, "without an override every shooter keeps its known hp");
        expect(visitAll(MOBILE_UNIT, 0).size() == expected.size() - 1, "an override to zero hp removes that shooter");
    }

    CandidateBundle spannedCandidate()
    {
        CandidateBundle candidate;
        candidate.actorNextShotsAtOrigin.push_back(ACTOR_ORIGIN_SHOT);
        candidate.actorNextShotsAtDestination.push_back(ACTOR_DESTINATION_SHOT);
        candidate.enemyShotsOnActorAtOrigin.push_back(ENEMY_ORIGIN_SHOT);
        candidate.enemyShotsOnActorAtDestination.push_back(ENEMY_DESTINATION_SHOT);
        return candidate;
    }

    bool aliases(std::span<const MilliFunds> view, const std::vector<MilliFunds> & owner)
    {
        return view.data() == owner.data() && view.size() == owner.size();
    }

    void testPositionFactsAliasTheOwnedVectors()
    {
        const CandidateBundle candidate = spannedCandidate();
        const PositionFacts facts = positionFacts(candidate);
        expect(aliases(facts.actorNextShotsAtOrigin, candidate.actorNextShotsAtOrigin), "actor origin span aliases");
        expect(aliases(facts.actorNextShotsAtDestination, candidate.actorNextShotsAtDestination),
               "actor destination span aliases");
        expect(aliases(facts.enemyShotsOnActorAtOrigin, candidate.enemyShotsOnActorAtOrigin), "enemy origin span aliases");
        expect(aliases(facts.enemyShotsOnActorAtDestination, candidate.enemyShotsOnActorAtDestination),
               "enemy destination span aliases");
    }

    void testCaptureIncomePricesEveryOwner()
    {
        const PropertyIncome neutral = captureIncome(propertyOwnedBy(NO_OWNER, 0));
        // A neutral property pays nobody yet, so every covered tile of its base rate stands in.
        expect(neutral.oursPerTurn == toMilliFunds(BASE_INCOME * COVERED_TILES), "neutral rate counts covered tiles");
        expect(neutral.oursPerTurn == neutral.enemyPerTurn, "neutral rate is the same for both sides");
        const PropertyIncome enemy = captureIncome(propertyOwnedBy(ENEMY_PLAYER, OWNED_INCOME_PER_TURN));
        expect(enemy.oursPerTurn == toMilliFunds(OWNED_INCOME_PER_TURN), "enemy rate is what the owner is paid");
        expect(enemy.enemyPerTurn == toMilliFunds(OWNED_INCOME_PER_TURN), "enemy rate mirrors to us unchanged");
        const PropertyIncome own = captureIncome(propertyOwnedBy(OWN_PLAYER, OWNED_INCOME_PER_TURN));
        expect(own.oursPerTurn == toMilliFunds(OWNED_INCOME_PER_TURN), "own rate is what we are paid");
    }

    void testMovingACandidateKeepsItsSpansValid()
    {
        std::vector<CandidateBundle> candidates;
        candidates.push_back(spannedCandidate());
        const MilliFunds* origin = candidates.front().actorNextShotsAtOrigin.data();
        for (std::int32_t grown = 0; grown < CANDIDATE_GROWTH_COUNT; ++grown)
        {
            candidates.push_back(spannedCandidate());
        }
        const PositionFacts facts = positionFacts(candidates.front());
        expect(facts.actorNextShotsAtOrigin.data() == origin, "a reallocated candidate keeps its span buffer");
        expect(facts.enemyShotsOnActorAtDestination.front() == ENEMY_DESTINATION_SHOT, "moved spans keep their values");
    }

    PlannableUnitFacts plannableOccupant()
    {
        return PlannableUnitFacts{
            .relation = Relation::Own,
            .movedInSnapshot = false,
            .mapped = true,
            .movedLive = false,
            .normalAi = true,
            .firesOrCaptures = true,
        };
    }

    void testOnlyAPlannableOccupantIsFollowed()
    {
        expect(unitPlannable(plannableOccupant()), "an unmoved own normal unit that acts is plannable");
        using Refusal = void (*)(PlannableUnitFacts &);
        const std::vector<std::pair<Refusal, const char*>> refusals{
            {[](PlannableUnitFacts & facts) { facts.relation = Relation::Allied; }, "an allied occupant blocks"},
            {[](PlannableUnitFacts & facts) { facts.relation = Relation::Enemy; }, "an enemy occupant blocks"},
            {[](PlannableUnitFacts & facts) { facts.relation = Relation::Neutral; }, "a neutral occupant blocks"},
            {[](PlannableUnitFacts & facts) { facts.movedInSnapshot = true; }, "an occupant that already acted blocks"},
            {[](PlannableUnitFacts & facts) { facts.movedLive = true; }, "an occupant moved since the snapshot blocks"},
            {[](PlannableUnitFacts & facts) { facts.mapped = false; }, "an occupant that cannot be mapped blocks"},
            {[](PlannableUnitFacts & facts) { facts.normalAi = false; }, "an occupant in another AI mode blocks"},
            {[](PlannableUnitFacts & facts) { facts.firesOrCaptures = false; }, "an occupant that cannot act blocks"},
        };
        for (const auto & [refuse, description] : refusals)
        {
            PlannableUnitFacts occupant = plannableOccupant();
            refuse(occupant);
            expect(!unitPlannable(occupant), description);
        }
        const TilePoint actorTile{0, 0};
        const TilePoint leaderTile{1, 0};
        const TilePoint blockedTile{0, 1};
        const std::vector<TilePoint> followTiles{actorTile, leaderTile};
        expect(followTileOffered(followTiles, leaderTile, actorTile), "a plannable unit's tile is offered");
        expect(!followTileOffered(followTiles, actorTile, actorTile), "the actor's own tile is its stay, not a follow");
        expect(!followTileOffered(followTiles, blockedTile, actorTile), "a tile outside the plannable set is blocked");
    }

    void testFollowTilesOnlyUnderStockControl()
    {
        const std::vector<TilePoint> plannableTiles{TilePoint{0, 0}, TilePoint{1, 0}};
        expect(followTilesFor(true, plannableTiles) == plannableTiles, "stock control collects every plannable tile");
        expect(followTilesFor(false, plannableTiles).empty(), "without stock control no tile is offered to follow");
    }

    void testFollowersAppendBehindToday()
    {
        constexpr std::size_t TODAY_COUNT = 3;
        constexpr std::size_t FIRE_ENTRY = 1;
        constexpr std::size_t CAPTURE_ENTRY = 2;
        std::int32_t generation = 0;
        // The destination column repeats the generation index, so order and numbering are checked together.
        const auto add = [&](std::vector<CandidateBundle> & candidates, std::optional<BundleComponent> component)
        {
            CandidateBundle candidate;
            candidate.generationIndex = generation;
            candidate.bundle.destination = TilePoint{generation, 0};
            ++generation;
            if (component)
            {
                candidate.bundle.components.push_back(*component);
            }
            candidates.push_back(std::move(candidate));
        };
        const auto addToday = [&](std::vector<CandidateBundle> & candidates)
        {
            add(candidates, std::nullopt);
            add(candidates, Coordinator::fireComponent(FireFacts{}));
            add(candidates, Coordinator::captureComponent(CaptureFacts{}));
        };
        std::vector<CandidateBundle> followed;
        const auto count = appendFollowersBehind(followed, addToday,
                                                 [&](std::vector<CandidateBundle> & candidates)
                                                 {
                                                     add(candidates, std::nullopt);
                                                 });
        bool ordered = followed.size() == TODAY_COUNT + 1;
        for (std::size_t index = 0; ordered && index < followed.size(); ++index)
        {
            const auto & bundle = followed[index].bundle;
            const auto expectedIndex = static_cast<std::int32_t>(index);
            ordered = followed[index].generationIndex == expectedIndex && bundle.destination.x == expectedIndex &&
                (index == FIRE_ENTRY || index == CAPTURE_ENTRY) == !bundle.components.empty();
        }
        ordered = ordered && followed[FIRE_ENTRY].bundle.components.front().kind == ComponentKind::Fire &&
            followed[CAPTURE_ENTRY].bundle.components.front().kind == ComponentKind::Capture;
        expect(count == TODAY_COUNT && ordered,
               "positional, fire and capture come first in generation order, then the follower, counted before it");
        generation = 0;
        std::vector<CandidateBundle> alone;
        const auto none = appendFollowersBehind(alone, addToday, [](std::vector<CandidateBundle> &) {});
        expect(!none.has_value() && alone.size() == TODAY_COUNT, "with no surviving follower there is no count");
    }
}

int main()
{
    testPositionFactsAliasTheOwnedVectors();
    testCaptureIncomePricesEveryOwner();
    testMovingACandidateKeepsItsSpansValid();
    testOnlyAPlannableOccupantIsFollowed();
    testFollowTilesOnlyUnderStockControl();
    testFollowersAppendBehindToday();
    testEnemyShotEnumeration();
    testMovedShotDistanceMatchesTheAttackField(DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, "direct reach matches the field");
    testMovedShotDistanceMatchesTheAttackField(INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE,
                                               "indirect reach matches the field");
    std::printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0)
    {
        return 0;
    }
    return 1;
}
