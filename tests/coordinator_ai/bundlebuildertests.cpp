#include <cstdint>
#include <cstdio>
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
    using Coordinator::FieldExpansion;
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

    void expect(bool condition, const char* description)
    {
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
}

int main()
{
    testPositionFactsAliasTheOwnedVectors();
    testCaptureIncomePricesEveryOwner();
    testMovingACandidateKeepsItsSpansValid();
    testMovedShotDistanceMatchesTheAttackField(DIRECT_MIN_RANGE, DIRECT_MAX_RANGE, "direct reach matches the field");
    testMovedShotDistanceMatchesTheAttackField(INDIRECT_MIN_RANGE, INDIRECT_MAX_RANGE,
                                               "indirect reach matches the field");
    if (failures == 0)
    {
        return 0;
    }
    return 1;
}
