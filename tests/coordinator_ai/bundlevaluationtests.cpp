#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "ai/coordinator/bundlevaluation.h"

namespace
{
    using Coordinator::ActionBundle;
    using Coordinator::ActorFacts;
    using Coordinator::bestShot;
    using Coordinator::BundleComponent;
    using Coordinator::BundleValuation;
    using Coordinator::captureComponent;
    using Coordinator::CaptureFacts;
    using Coordinator::ContinuationDelta;
    using Coordinator::DEFAULT_CAPITAL_POLICY;
    using Coordinator::DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
    using Coordinator::fireComponent;
    using Coordinator::FireFacts;
    using Coordinator::MilliFunds;
    using Coordinator::NO_CAPTURE_TURNS;
    using Coordinator::NO_TURN_LIMIT;
    using Coordinator::NO_UNIT;
    using Coordinator::OwnerSign;
    using Coordinator::PlanBundleKind;
    using Coordinator::planBundleKindOf;
    using Coordinator::PositionFacts;
    using Coordinator::PropertyIncome;
    using Coordinator::resolveHorizon;
    using Coordinator::serviceComponent;
    using Coordinator::ServiceFacts;
    using Coordinator::TilePoint;
    using Coordinator::toMilliFunds;
    using Coordinator::UNIT_HP_STEPS;
    using Coordinator::ValuationContext;
    using Coordinator::valueBundle;

    constexpr std::int32_t SHORT_HORIZON_TURNS = 5;
    constexpr std::int32_t LONG_HORIZON_TURNS = 15;
    constexpr std::array<std::int32_t, 3> SENSITIVITY_HORIZONS{
        SHORT_HORIZON_TURNS,
        DEFAULT_ECONOMIC_RELEVANCE_HORIZON,
        LONG_HORIZON_TURNS,
    };

    constexpr std::int32_t ACTOR_UNIT = 1;
    constexpr std::int32_t TARGET_UNIT = 2;
    constexpr std::int32_t SECOND_TARGET_UNIT = 3;
    constexpr std::int32_t PARTNER_UNIT = 4;
    constexpr std::int32_t SECOND_PARTNER_UNIT = 5;
    constexpr TilePoint ORIGIN_TILE{2, 2};
    constexpr TilePoint DESTINATION_TILE{3, 2};

    constexpr MilliFunds ACTOR_REPLACEMENT_COST = toMilliFunds(7000);
    constexpr MilliFunds TARGET_REPLACEMENT_COST = toMilliFunds(4000);
    constexpr MilliFunds NEGATIVE_COST = toMilliFunds(-1);
    constexpr std::int32_t ACTOR_HP_STEPS = 8;
    constexpr std::int32_t TARGET_HP_STEPS = 6;
    constexpr std::int32_t DAMAGE_STEPS = 4;
    constexpr std::int32_t COUNTER_STEPS = 3;
    constexpr std::int32_t NO_COUNTER_STEPS = 0;
    constexpr MilliFunds TARGET_BEST_SHOT_BEFORE = toMilliFunds(3000);
    constexpr MilliFunds TARGET_BEST_SHOT_AFTER = toMilliFunds(1000);
    constexpr MilliFunds CONTINUATION_REMOVED = TARGET_BEST_SHOT_BEFORE - TARGET_BEST_SHOT_AFTER;
    constexpr MilliFunds PROPORTIONAL_CONTINUATION_REMOVED = TARGET_BEST_SHOT_BEFORE * DAMAGE_STEPS / UNIT_HP_STEPS;

    constexpr MilliFunds ACTOR_ORIGIN_WEAK_SHOT = toMilliFunds(500);
    constexpr MilliFunds ACTOR_ORIGIN_BEST_SHOT = toMilliFunds(900);
    constexpr MilliFunds ACTOR_DESTINATION_WEAK_SHOT = toMilliFunds(1200);
    constexpr MilliFunds ACTOR_DESTINATION_BEST_SHOT = toMilliFunds(1500);
    constexpr MilliFunds ENEMY_ORIGIN_SHOT = toMilliFunds(300);
    constexpr MilliFunds ENEMY_DESTINATION_LOW_SHOT = toMilliFunds(400);
    constexpr MilliFunds ENEMY_DESTINATION_MID_SHOT = toMilliFunds(700);
    constexpr MilliFunds ENEMY_DESTINATION_BEST_SHOT = toMilliFunds(1100);
    constexpr MilliFunds ENEMY_DESTINATION_SHOT_SUM =
        ENEMY_DESTINATION_LOW_SHOT + ENEMY_DESTINATION_MID_SHOT + ENEMY_DESTINATION_BEST_SHOT;

    constexpr std::array<MilliFunds, 2> ACTOR_SHOTS_AT_ORIGIN{ACTOR_ORIGIN_WEAK_SHOT, ACTOR_ORIGIN_BEST_SHOT};
    constexpr std::array<MilliFunds, 2> ACTOR_SHOTS_AT_DESTINATION{ACTOR_DESTINATION_BEST_SHOT, ACTOR_DESTINATION_WEAK_SHOT};
    constexpr std::array<MilliFunds, 1> ENEMY_SHOTS_AT_ORIGIN{ENEMY_ORIGIN_SHOT};
    constexpr std::array<MilliFunds, 3> ENEMY_SHOTS_AT_DESTINATION{
        ENEMY_DESTINATION_MID_SHOT,
        ENEMY_DESTINATION_BEST_SHOT,
        ENEMY_DESTINATION_LOW_SHOT,
    };
    constexpr std::array<MilliFunds, 0> NO_SHOTS{};
    constexpr std::array<MilliFunds, 1> MIRRORED_ACTOR_SHOTS_AT_ORIGIN{TARGET_BEST_SHOT_BEFORE};
    constexpr std::array<MilliFunds, 1> MIRRORED_ACTOR_SHOTS_AT_DESTINATION{TARGET_BEST_SHOT_AFTER};

    constexpr ActorFacts EXCHANGE_ACTOR{
        .replacementCost = ACTOR_REPLACEMENT_COST,
        .hpSteps = ACTOR_HP_STEPS,
    };
    constexpr ActorFacts MIRRORED_EXCHANGE_ACTOR{
        .replacementCost = TARGET_REPLACEMENT_COST,
        .hpSteps = TARGET_HP_STEPS,
    };

    constexpr FireFacts EXCHANGE_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    // the relabeled bundle: the mirrored actor takes what it dealt and shoots what shot it
    constexpr FireFacts MIRRORED_EXCHANGE_FIRE{
        .targetUnitId = ACTOR_UNIT,
        .targetReplacementCost = ACTOR_REPLACEMENT_COST,
        .targetHpSteps = ACTOR_HP_STEPS,
        .damageSteps = COUNTER_STEPS,
        .counterSteps = DAMAGE_STEPS,
        .targetBestShotBefore = ACTOR_ORIGIN_BEST_SHOT,
        .targetBestShotAfter = ACTOR_DESTINATION_BEST_SHOT,
    };

    constexpr FireFacts PARTIAL_DAMAGE_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = UNIT_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };

    constexpr std::int32_t KILL_TARGET_HP_STEPS = 5;
    constexpr std::int32_t LETHAL_DAMAGE_STEPS = UNIT_HP_STEPS;
    constexpr FireFacts KILL_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = KILL_TARGET_HP_STEPS,
        .damageSteps = LETHAL_DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    constexpr FireFacts KILL_WITH_QUOTED_COUNTER_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = KILL_TARGET_HP_STEPS,
        .damageSteps = LETHAL_DAMAGE_STEPS,
        .counterSteps = COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    constexpr MilliFunds KILL_REMAINING_BOOK =
        DEFAULT_CAPITAL_POLICY.bookValue(TARGET_REPLACEMENT_COST, KILL_TARGET_HP_STEPS);
    constexpr MilliFunds KILL_VALUE = KILL_REMAINING_BOOK + TARGET_BEST_SHOT_BEFORE;

    constexpr std::int32_t SHORT_ACTOR_HP_STEPS = 3;
    constexpr std::int32_t FIRST_COUNTER_STEPS = 2;
    constexpr std::int32_t SECOND_COUNTER_STEPS = 5;
    constexpr std::int32_t UNCAPPED_COUNTER_STEPS = FIRST_COUNTER_STEPS + SHORT_ACTOR_HP_STEPS;
    constexpr std::int32_t CHAINED_TARGET_HP_STEPS = UNIT_HP_STEPS - DAMAGE_STEPS;
    constexpr MilliFunds CHAINED_SHOT_AFTER_FIRST = toMilliFunds(1800);
    constexpr MilliFunds CHAINED_SHOT_AFTER_SECOND = toMilliFunds(600);

    constexpr ActorFacts FRAGILE_ACTOR{
        .replacementCost = ACTOR_REPLACEMENT_COST,
        .hpSteps = SHORT_ACTOR_HP_STEPS,
    };
    constexpr FireFacts FIRST_COUNTERED_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = UNIT_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = FIRST_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_FIRST,
    };
    constexpr FireFacts SECOND_COUNTERED_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = CHAINED_TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = SECOND_COUNTER_STEPS,
        .targetBestShotBefore = CHAINED_SHOT_AFTER_FIRST,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_SECOND,
    };
    constexpr FireFacts BROKEN_HP_CHAIN_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = UNIT_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = CHAINED_SHOT_AFTER_FIRST,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_SECOND,
    };
    constexpr FireFacts BROKEN_SHOT_CHAIN_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = CHAINED_TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_SECOND,
    };

    constexpr std::int32_t POOL_SECOND_HP_STEPS = TARGET_HP_STEPS - DAMAGE_STEPS;
    constexpr FireFacts POOL_FIRST_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_FIRST,
    };
    constexpr FireFacts POOL_SECOND_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = POOL_SECOND_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = CHAINED_SHOT_AFTER_FIRST,
        .targetBestShotAfter = CHAINED_SHOT_AFTER_SECOND,
    };
    constexpr MilliFunds POOLED_TARGET_BOOK =
        DEFAULT_CAPITAL_POLICY.bookValue(TARGET_REPLACEMENT_COST, TARGET_HP_STEPS);

    constexpr std::int32_t DEATH_DAMAGE_STEPS = 2;
    constexpr FireFacts LETHAL_COUNTER_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DEATH_DAMAGE_STEPS,
        .counterSteps = SHORT_ACTOR_HP_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    constexpr FireFacts POSTHUMOUS_FIRE{
        .targetUnitId = SECOND_TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = NO_COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    constexpr MilliFunds DEATH_FIRE_BOOK =
        DEFAULT_CAPITAL_POLICY.bookValue(TARGET_REPLACEMENT_COST, DEATH_DAMAGE_STEPS);

    constexpr MilliFunds OUR_PROPERTY_RATE = toMilliFunds(1200);
    constexpr MilliFunds ENEMY_PROPERTY_RATE = toMilliFunds(800);
    constexpr PropertyIncome ASYMMETRIC_INCOME{
        .oursPerTurn = OUR_PROPERTY_RATE,
        .enemyPerTurn = ENEMY_PROPERTY_RATE,
    };
    constexpr MilliFunds SYMMETRIC_PROPERTY_RATE = toMilliFunds(1000);
    constexpr PropertyIncome SYMMETRIC_INCOME{
        .oursPerTurn = SYMMETRIC_PROPERTY_RATE,
        .enemyPerTurn = SYMMETRIC_PROPERTY_RATE,
    };
    constexpr std::int32_t TURNS_UNTIL_OWNED = 2;
    constexpr std::int32_t DELAYED_TURNS_UNTIL_OWNED = TURNS_UNTIL_OWNED + 1;

    constexpr CaptureFacts NEUTRAL_CAPTURE{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts ENEMY_CAPTURE{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Enemy,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts UNFINISHED_CAPTURE{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = NO_CAPTURE_TURNS,
    };
    constexpr CaptureFacts MIRROR_CAPTURE{
        .income = ASYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Enemy,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts MIRRORED_MIRROR_CAPTURE{
        .income = ASYMMETRIC_INCOME.mirrored(),
        .ownerBefore = OwnerSign::Ours,
        .ownerAfter = OwnerSign::Enemy,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts LOSS_OF_OURS_NOW{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Ours,
        .ownerAfter = OwnerSign::Enemy,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts LOSS_OF_OURS_DELAYED{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Ours,
        .ownerAfter = OwnerSign::Enemy,
        .turnsUntilOwned = DELAYED_TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts LOSS_OF_NEUTRAL_NOW{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Enemy,
        .turnsUntilOwned = TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts LOSS_OF_NEUTRAL_DELAYED{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Enemy,
        .turnsUntilOwned = DELAYED_TURNS_UNTIL_OWNED,
    };
    constexpr CaptureFacts BROKEN_CAPTURE{
        .income = SYMMETRIC_INCOME,
        .ownerBefore = OwnerSign::Neutral,
        .ownerAfter = OwnerSign::Ours,
        .turnsUntilOwned = NO_CAPTURE_TURNS - 1,
    };

    constexpr MilliFunds PARTNER_REPLACEMENT_COST = toMilliFunds(6000);
    constexpr std::int32_t PARTNER_HP_STEPS_BEFORE = 4;
    constexpr std::int32_t PARTNER_HP_STEPS_AFTER = 7;
    constexpr std::int32_t REPAIRED_HP_STEPS = PARTNER_HP_STEPS_AFTER - PARTNER_HP_STEPS_BEFORE;
    constexpr MilliFunds REPLACEMENT_BASIS_REPAIR_COST = PARTNER_REPLACEMENT_COST * REPAIRED_HP_STEPS / UNIT_HP_STEPS;
    constexpr std::int32_t RESUPPLY_HP_STEPS = 5;
    constexpr MilliFunds PARTNER_BEST_SHOT_BEFORE = toMilliFunds(1000);
    constexpr MilliFunds PARTNER_BEST_SHOT_AFTER = toMilliFunds(2500);
    constexpr MilliFunds PARTNER_CONTINUATION_GAIN = PARTNER_BEST_SHOT_AFTER - PARTNER_BEST_SHOT_BEFORE;

    constexpr ServiceFacts REPLACEMENT_BASIS_REPAIR{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = PARTNER_HP_STEPS_BEFORE,
        .partnerHpStepsAfter = PARTNER_HP_STEPS_AFTER,
        .repairCost = REPLACEMENT_BASIS_REPAIR_COST,
    };
    constexpr ServiceFacts RESUPPLY{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = RESUPPLY_HP_STEPS,
        .partnerHpStepsAfter = RESUPPLY_HP_STEPS,
        .partnerBestShotBefore = PARTNER_BEST_SHOT_BEFORE,
        .partnerBestShotAfter = PARTNER_BEST_SHOT_AFTER,
    };
    constexpr ServiceFacts SECOND_PARTNER_RESUPPLY{
        .partnerUnitId = SECOND_PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = RESUPPLY_HP_STEPS,
        .partnerHpStepsAfter = RESUPPLY_HP_STEPS,
        .partnerBestShotBefore = PARTNER_BEST_SHOT_BEFORE,
        .partnerBestShotAfter = PARTNER_BEST_SHOT_AFTER,
    };
    constexpr std::int32_t SELF_REPAIRED_HP_STEPS = 3;
    constexpr std::int32_t SELF_REPAIR_HP_STEPS_AFTER = SHORT_ACTOR_HP_STEPS + SELF_REPAIRED_HP_STEPS;
    constexpr MilliFunds SELF_REPAIR_COST = ACTOR_REPLACEMENT_COST * SELF_REPAIRED_HP_STEPS / UNIT_HP_STEPS;
    constexpr ServiceFacts SELF_REPAIR{
        .partnerUnitId = ACTOR_UNIT,
        .partnerReplacementCost = ACTOR_REPLACEMENT_COST,
        .partnerHpStepsBefore = SHORT_ACTOR_HP_STEPS,
        .partnerHpStepsAfter = SELF_REPAIR_HP_STEPS_AFTER,
        .repairCost = SELF_REPAIR_COST,
    };
    constexpr MilliFunds SELF_REPAIR_BOOK_GAIN =
        DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SELF_REPAIR_HP_STEPS_AFTER) -
        DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SHORT_ACTOR_HP_STEPS);
    // the hp EXCHANGE_FIRE's counter leaves EXCHANGE_ACTOR with
    constexpr std::int32_t COUNTERED_ACTOR_HP_STEPS = ACTOR_HP_STEPS - COUNTER_STEPS;
    constexpr ServiceFacts COUNTERED_SELF_REPAIR{
        .partnerUnitId = ACTOR_UNIT,
        .partnerReplacementCost = ACTOR_REPLACEMENT_COST,
        .partnerHpStepsBefore = COUNTERED_ACTOR_HP_STEPS,
        .partnerHpStepsAfter = ACTOR_HP_STEPS,
        .repairCost = ACTOR_REPLACEMENT_COST * COUNTER_STEPS / UNIT_HP_STEPS,
    };
    constexpr ServiceFacts UNNAMED_PARTNER_RESUPPLY{
        .partnerUnitId = NO_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = RESUPPLY_HP_STEPS,
        .partnerHpStepsAfter = RESUPPLY_HP_STEPS,
    };
    constexpr FireFacts UNNAMED_TARGET_FIRE{
        .targetUnitId = NO_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
        .counterSteps = COUNTER_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
        .targetBestShotAfter = TARGET_BEST_SHOT_AFTER,
    };
    constexpr ServiceFacts SHRINKING_REPAIR{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = PARTNER_HP_STEPS_AFTER,
        .partnerHpStepsAfter = PARTNER_HP_STEPS_BEFORE,
    };
    constexpr ServiceFacts PAID_BACK_REPAIR{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = PARTNER_HP_STEPS_BEFORE,
        .partnerHpStepsAfter = PARTNER_HP_STEPS_AFTER,
        .repairCost = NEGATIVE_COST,
    };

    constexpr PositionFacts EXCHANGE_POSITION{
        .actorNextShotsAtOrigin = ACTOR_SHOTS_AT_ORIGIN,
        .actorNextShotsAtDestination = ACTOR_SHOTS_AT_DESTINATION,
        .enemyShotsOnActorAtOrigin = ENEMY_SHOTS_AT_ORIGIN,
        .enemyShotsOnActorAtDestination = ENEMY_SHOTS_AT_DESTINATION,
    };
    // exposure has no one bundle mirror counterpart, so the mirror fixtures leave the enemy spans empty
    constexpr PositionFacts MIRROR_FIXTURE_POSITION{
        .actorNextShotsAtOrigin = ACTOR_SHOTS_AT_ORIGIN,
        .actorNextShotsAtDestination = ACTOR_SHOTS_AT_DESTINATION,
    };
    constexpr PositionFacts MIRRORED_FIXTURE_POSITION{
        .actorNextShotsAtOrigin = MIRRORED_ACTOR_SHOTS_AT_ORIGIN,
        .actorNextShotsAtDestination = MIRRORED_ACTOR_SHOTS_AT_DESTINATION,
    };
    constexpr PositionFacts DOMINATED_POSITION{
        .actorNextShotsAtOrigin = ACTOR_SHOTS_AT_ORIGIN,
        .actorNextShotsAtDestination = ACTOR_SHOTS_AT_ORIGIN,
        .enemyShotsOnActorAtOrigin = ENEMY_SHOTS_AT_DESTINATION,
        .enemyShotsOnActorAtDestination = ENEMY_SHOTS_AT_DESTINATION,
    };
    constexpr PositionFacts FOCUSED_ENEMY_POSITION{
        .enemyShotsOnActorAtDestination = ENEMY_SHOTS_AT_DESTINATION,
    };
    constexpr PositionFacts LOST_SHOT_POSITION{
        .actorNextShotsAtOrigin = ACTOR_SHOTS_AT_ORIGIN,
        .actorNextShotsAtDestination = NO_SHOTS,
    };
    constexpr PositionFacts NO_POSITION{};

    constexpr std::int32_t NEGATIVE_HP_STEPS = -1;
    constexpr std::int32_t EXCESSIVE_DAMAGE_STEPS = UNIT_HP_STEPS + 1;
    constexpr std::int32_t NEGATIVE_HORIZON_TURNS = -1;
    constexpr std::int32_t NO_HP_STEPS_LEFT = 0;
    constexpr std::int32_t REMAINING_TURN_LIMIT = 3;
    constexpr std::int32_t FINAL_TURN_LIMIT = 0;

    constexpr ActorFacts BROKEN_ACTOR{
        .replacementCost = ACTOR_REPLACEMENT_COST,
        .hpSteps = NEGATIVE_HP_STEPS,
    };
    constexpr ActorFacts DEAD_ACTOR{
        .replacementCost = ACTOR_REPLACEMENT_COST,
        .hpSteps = NO_HP_STEPS_LEFT,
    };
    constexpr ServiceFacts REVIVAL_REPAIR{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = NO_HP_STEPS_LEFT,
        .partnerHpStepsAfter = RESUPPLY_HP_STEPS,
    };
    constexpr ActorFacts PRICELESS_ACTOR{
        .replacementCost = NEGATIVE_COST,
        .hpSteps = ACTOR_HP_STEPS,
    };
    constexpr FireFacts OVER_DAMAGE_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = EXCESSIVE_DAMAGE_STEPS,
    };
    constexpr FireFacts DEAD_TARGET_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = TARGET_REPLACEMENT_COST,
        .targetHpSteps = NO_HP_STEPS_LEFT,
        .damageSteps = DAMAGE_STEPS,
        .targetBestShotBefore = TARGET_BEST_SHOT_BEFORE,
    };
    constexpr FireFacts PRICELESS_TARGET_FIRE{
        .targetUnitId = TARGET_UNIT,
        .targetReplacementCost = NEGATIVE_COST,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = DAMAGE_STEPS,
    };
    constexpr ServiceFacts BROKEN_PARTNER_SERVICE{
        .partnerUnitId = PARTNER_UNIT,
        .partnerReplacementCost = PARTNER_REPLACEMENT_COST,
        .partnerHpStepsBefore = NEGATIVE_HP_STEPS,
    };

    static_assert(bestShot(NO_SHOTS) == 0);
    static_assert(bestShot(ACTOR_SHOTS_AT_ORIGIN) == ACTOR_ORIGIN_BEST_SHOT);
    static_assert(bestShot(ACTOR_SHOTS_AT_DESTINATION) == ACTOR_DESTINATION_BEST_SHOT);
    static_assert(bestShot(ENEMY_SHOTS_AT_DESTINATION) == ENEMY_DESTINATION_BEST_SHOT);
    static_assert(bestShot(ENEMY_SHOTS_AT_DESTINATION) != ENEMY_DESTINATION_SHOT_SUM);

    static_assert(resolveHorizon(NO_TURN_LIMIT) == DEFAULT_ECONOMIC_RELEVANCE_HORIZON);
    static_assert(resolveHorizon(REMAINING_TURN_LIMIT) == REMAINING_TURN_LIMIT);
    static_assert(resolveHorizon(FINAL_TURN_LIMIT) == FINAL_TURN_LIMIT);

    int failures = 0;

    void expect(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    void expectAt(bool condition, const char* description, std::int32_t horizonTurns)
    {
        if (condition)
        {
            return;
        }
        std::printf("horizon %d ", horizonTurns);
        expect(false, description);
    }

    constexpr ActionBundle bundleOf(std::vector<BundleComponent> components, TilePoint destination)
    {
        ActionBundle bundle;
        bundle.unitId = ACTOR_UNIT;
        bundle.origin = ORIGIN_TILE;
        bundle.destination = destination;
        bundle.path.push_back(ORIGIN_TILE);
        if (destination != ORIGIN_TILE)
        {
            bundle.path.push_back(destination);
        }
        bundle.components = std::move(components);
        return bundle;
    }

    constexpr PlanBundleKind kindOf(std::vector<BundleComponent> components, TilePoint destination)
    {
        return planBundleKindOf(bundleOf(std::move(components), destination));
    }

    static_assert(kindOf({}, ORIGIN_TILE) == PlanBundleKind::Wait);
    static_assert(kindOf({}, DESTINATION_TILE) == PlanBundleKind::Move);
    static_assert(kindOf({fireComponent(EXCHANGE_FIRE)}, ORIGIN_TILE) == PlanBundleKind::Fire);
    static_assert(kindOf({fireComponent(EXCHANGE_FIRE)}, DESTINATION_TILE) == PlanBundleKind::MoveAndFire);
    static_assert(kindOf({captureComponent(NEUTRAL_CAPTURE)}, ORIGIN_TILE) == PlanBundleKind::Capture);
    static_assert(kindOf({captureComponent(NEUTRAL_CAPTURE)}, DESTINATION_TILE) == PlanBundleKind::MoveAndCapture);
    static_assert(kindOf({serviceComponent(RESUPPLY)}, ORIGIN_TILE) == PlanBundleKind::Service);
    static_assert(kindOf({serviceComponent(RESUPPLY)}, DESTINATION_TILE) == PlanBundleKind::MoveAndService);
    static_assert(kindOf({captureComponent(NEUTRAL_CAPTURE), fireComponent(EXCHANGE_FIRE)}, ORIGIN_TILE) ==
                  PlanBundleKind::Compound);
    static_assert(kindOf({captureComponent(NEUTRAL_CAPTURE), fireComponent(EXCHANGE_FIRE)}, DESTINATION_TILE) ==
                  PlanBundleKind::Compound);

    constexpr MilliFunds killBundleValue()
    {
        const ActionBundle bundle = bundleOf({fireComponent(KILL_FIRE)}, ORIGIN_TILE);
        const ValuationContext context{DEFAULT_ECONOMIC_RELEVANCE_HORIZON};
        return valueBundle(bundle, EXCHANGE_ACTOR, NO_POSITION, context).value().economicValue;
    }

    static_assert(killBundleValue() == KILL_VALUE);

    BundleValuation valueOf(const ActionBundle & bundle, const ActorFacts & actor,
                            const PositionFacts & position, std::int32_t horizonTurns)
    {
        const ValuationContext context{horizonTurns};
        return valueBundle(bundle, actor, position, context);
    }

    void testMirroringEveryInputNegatesTheValue(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf(
            {fireComponent(EXCHANGE_FIRE), captureComponent(MIRROR_CAPTURE)}, DESTINATION_TILE);
        const ActionBundle mirrored = bundleOf(
            {fireComponent(MIRRORED_EXCHANGE_FIRE), captureComponent(MIRRORED_MIRROR_CAPTURE)}, DESTINATION_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, MIRROR_FIXTURE_POSITION, horizonTurns);
        const BundleValuation mirroredValuation =
            valueOf(mirrored, MIRRORED_EXCHANGE_ACTOR, MIRRORED_FIXTURE_POSITION, horizonTurns);
        expectAt(valuation.valid && mirroredValuation.valid, "both mirror fixtures are well formed", horizonTurns);
        expectAt(valuation.value().economicValue != 0, "the mirror fixture is not a trivial zero", horizonTurns);
        expectAt(mirroredValuation.ledger == valuation.ledger.mirrored(),
                 "the player mirror swaps the ledger channels", horizonTurns);
        expectAt(mirroredValuation.continuation.total() == -valuation.continuation.total(),
                 "the player mirror negates the continuation", horizonTurns);
        expectAt(mirroredValuation.value().economicValue == -valuation.value().economicValue,
                 "the player mirror negates the bundle value", horizonTurns);
    }

    void testEnemyCaptureIsExactlyTwiceTheNeutralCapture(std::int32_t horizonTurns)
    {
        const ActionBundle neutral = bundleOf({captureComponent(NEUTRAL_CAPTURE)}, ORIGIN_TILE);
        const ActionBundle enemy = bundleOf({captureComponent(ENEMY_CAPTURE)}, ORIGIN_TILE);
        const BundleValuation neutralValuation = valueOf(neutral, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        const BundleValuation enemyValuation = valueOf(enemy, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        const MilliFunds ownedTurns = horizonTurns - TURNS_UNTIL_OWNED;
        expectAt(neutralValuation.continuation.propertyContinuation == SYMMETRIC_PROPERTY_RATE * ownedTurns,
                 "the neutral capture pays its owner time stream", horizonTurns);
        expectAt(enemyValuation.continuation.propertyContinuation == 2 * neutralValuation.continuation.propertyContinuation,
                 "an enemy capture is exactly twice a neutral one", horizonTurns);
    }

    void testDelayingAnEnemyCaptureOfOursPreservesTwiceTheNeutralDelay(std::int32_t horizonTurns)
    {
        const ActionBundle ourLossNow = bundleOf({captureComponent(LOSS_OF_OURS_NOW)}, ORIGIN_TILE);
        const ActionBundle ourLossDelayed = bundleOf({captureComponent(LOSS_OF_OURS_DELAYED)}, ORIGIN_TILE);
        const ActionBundle neutralLossNow = bundleOf({captureComponent(LOSS_OF_NEUTRAL_NOW)}, ORIGIN_TILE);
        const ActionBundle neutralLossDelayed = bundleOf({captureComponent(LOSS_OF_NEUTRAL_DELAYED)}, ORIGIN_TILE);
        const MilliFunds oursPreserved =
            valueOf(ourLossDelayed, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).continuation.propertyContinuation -
            valueOf(ourLossNow, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).continuation.propertyContinuation;
        const MilliFunds neutralPreserved =
            valueOf(neutralLossDelayed, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).continuation.propertyContinuation -
            valueOf(neutralLossNow, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).continuation.propertyContinuation;
        expectAt(neutralPreserved == SYMMETRIC_PROPERTY_RATE, "delaying a neutral capture preserves one turn of income", horizonTurns);
        expectAt(oursPreserved == 2 * neutralPreserved, "delaying an enemy capture of ours preserves exactly twice as much", horizonTurns);
    }

    void testKillIsRemainingBookPlusContinuationRemoved(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({fireComponent(KILL_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        expectAt(valuation.ledger.enemyCapital == KILL_REMAINING_BOOK, "a kill books the remaining hp", horizonTurns);
        expectAt(valuation.ledger.friendlyCapital == 0, "an uncountered kill costs no friendly capital", horizonTurns);
        expectAt(valuation.continuation.targetContinuationRemoved == TARGET_BEST_SHOT_BEFORE,
                 "a kill removes the whole previous continuation", horizonTurns);
        expectAt(valuation.value().economicValue == KILL_VALUE, "a kill is book plus continuation and nothing else", horizonTurns);
    }

    void testAKillChargesNoCounter(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({fireComponent(KILL_WITH_QUOTED_COUNTER_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        expectAt(valuation.ledger.friendlyCapital == 0, "a dead target never counters", horizonTurns);
        expectAt(valuation.value().economicValue == KILL_VALUE, "the quoted counter changes nothing on a kill", horizonTurns);
    }

    void testAnActorThatDiesStopsBookingAndKeepsNoPost(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf(
            {fireComponent(LETHAL_COUNTER_FIRE), fireComponent(POSTHUMOUS_FIRE)}, DESTINATION_TILE);
        const BundleValuation valuation = valueOf(bundle, FRAGILE_ACTOR, EXCHANGE_POSITION, horizonTurns);
        const MilliFunds actorBook = DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SHORT_ACTOR_HP_STEPS);
        expectAt(valuation.valid, "a suicide is a legal bundle, not a malformed one", horizonTurns);
        expectAt(valuation.ledger.enemyCapital == DEATH_FIRE_BOOK, "a dead actor books no further damage", horizonTurns);
        expectAt(valuation.ledger.friendlyCapital == -actorBook, "the actor loses its whole remaining book", horizonTurns);
        expectAt(valuation.continuation.targetContinuationRemoved == CONTINUATION_REMOVED,
                 "a dead actor removes no further continuation", horizonTurns);
        expectAt(valuation.continuation.repositioning == -ACTOR_ORIGIN_BEST_SHOT,
                 "a dead actor gives up its origin shot and reaches no post", horizonTurns);
        expectAt(valuation.continuation.exposure == ENEMY_ORIGIN_SHOT,
                 "a dead actor takes the enemy's shot on it off the board", horizonTurns);
    }

    void testDominatedPositionalOptionIsWorthNothing(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({}, DESTINATION_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, DOMINATED_POSITION, horizonTurns);
        expectAt(valuation.continuation == ContinuationDelta{}, "a dominated move moves no continuation channel", horizonTurns);
        expectAt(valuation.value().economicValue == 0, "a dominated positional option values exactly zero", horizonTurns);
    }

    void testMoveAndWaitHaveNoImmediateLedger(std::int32_t horizonTurns)
    {
        const ActionBundle move = bundleOf({}, DESTINATION_TILE);
        const ActionBundle wait = bundleOf({}, ORIGIN_TILE);
        const ActionBundle capture = bundleOf({captureComponent(NEUTRAL_CAPTURE)}, DESTINATION_TILE);
        const ActionBundle progress = bundleOf({captureComponent(UNFINISHED_CAPTURE)}, ORIGIN_TILE);
        expectAt(valueOf(move, EXCHANGE_ACTOR, EXCHANGE_POSITION, horizonTurns).ledger.total() == 0,
                 "a move has no immediate ledger", horizonTurns);
        expectAt(valueOf(wait, EXCHANGE_ACTOR, EXCHANGE_POSITION, horizonTurns).ledger.total() == 0,
                 "a wait has no immediate ledger", horizonTurns);
        expectAt(valueOf(capture, EXCHANGE_ACTOR, EXCHANGE_POSITION, horizonTurns).ledger.total() == 0,
                 "a capture has no immediate ledger", horizonTurns);
        expectAt(valueOf(progress, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).value().economicValue == 0,
                 "capture progress that never completes is worth nothing", horizonTurns);
    }

    void testResourceUseChangesContinuationOnly(std::int32_t horizonTurns)
    {
        const ActionBundle resupply = bundleOf({serviceComponent(RESUPPLY)}, ORIGIN_TILE);
        const ActionBundle repair = bundleOf({serviceComponent(REPLACEMENT_BASIS_REPAIR)}, ORIGIN_TILE);
        const BundleValuation resupplyValuation = valueOf(resupply, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        const BundleValuation repairValuation = valueOf(repair, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        expectAt(resupplyValuation.ledger.total() == 0, "an unpriced resupply moves no capital", horizonTurns);
        expectAt(resupplyValuation.continuation.partnerContinuation == PARTNER_CONTINUATION_GAIN,
                 "a resupply pays the partner continuation gain", horizonTurns);
        expectAt(repairValuation.ledger.total() == 0, "a replacement basis repair is zero net", horizonTurns);
        expectAt(repairValuation.continuation.partnerContinuation == 0,
                 "an unquoted partner shot leaves continuation untouched", horizonTurns);
    }

    void testServiceOnSelfFeedsTheActorHpPool(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf(
            {serviceComponent(SELF_REPAIR), fireComponent(SECOND_COUNTERED_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, FRAGILE_ACTOR, NO_POSITION, horizonTurns);
        const MilliFunds healedCounter = DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SECOND_COUNTER_STEPS);
        const MilliFunds unhealedCounter = DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SHORT_ACTOR_HP_STEPS);
        expectAt(valuation.valid, "a service on the actor itself is legal", horizonTurns);
        expectAt(valuation.ledger.friendlyCapital == SELF_REPAIR_BOOK_GAIN - healedCounter,
                 "the healed actor can pay a larger counter", horizonTurns);
        expectAt(valuation.ledger.friendlyCapital != SELF_REPAIR_BOOK_GAIN - unhealedCounter,
                 "the counter is not capped at the hp the actor started with", horizonTurns);
    }

    void testASecondServiceOnOnePartnerIsMalformed(std::int32_t horizonTurns)
    {
        const ActionBundle twice = bundleOf(
            {serviceComponent(RESUPPLY), serviceComponent(REPLACEMENT_BASIS_REPAIR)}, ORIGIN_TILE);
        const ActionBundle twoPartners = bundleOf(
            {serviceComponent(RESUPPLY), serviceComponent(SECOND_PARTNER_RESUPPLY)}, ORIGIN_TILE);
        expectAt(!valueOf(twice, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "one partner cannot be serviced twice in one bundle", horizonTurns);
        expectAt(valueOf(twoPartners, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "two different partners are fine", horizonTurns);
    }

    void testExposureIsTheBestSingleEnemyShotUntilTheAssignmentLands(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({}, DESTINATION_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, FOCUSED_ENEMY_POSITION, horizonTurns);
        expectAt(valuation.continuation.exposure == -ENEMY_DESTINATION_BEST_SHOT,
                 "exposure is minus the strongest enemy shot", horizonTurns);
        expectAt(valuation.continuation.exposure != -ENEMY_DESTINATION_SHOT_SUM,
                 "exposure never sums the enemy shots", horizonTurns);
    }

    void testAnEmptyNextShotSpanRepositionsForNothing(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({}, DESTINATION_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, LOST_SHOT_POSITION, horizonTurns);
        expectAt(valuation.continuation.repositioning == -ACTOR_ORIGIN_BEST_SHOT,
                 "an empty destination span gives up the whole origin shot", horizonTurns);
    }

    void testLostContinuationIsADifferenceNotAProportion(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf({fireComponent(PARTIAL_DAMAGE_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        expectAt(valuation.continuation.targetContinuationRemoved == CONTINUATION_REMOVED,
                 "continuation removed is the best shot difference", horizonTurns);
        expectAt(valuation.continuation.targetContinuationRemoved != PROPORTIONAL_CONTINUATION_REMOVED,
                 "continuation removed is not scaled by damage", horizonTurns);
    }

    void testMalformedInputsAreReportedNotClamped(std::int32_t horizonTurns)
    {
        const ActionBundle wait = bundleOf({}, ORIGIN_TILE);
        const ActionBundle overDamage = bundleOf({fireComponent(OVER_DAMAGE_FIRE)}, ORIGIN_TILE);
        const ActionBundle deadTarget = bundleOf({fireComponent(DEAD_TARGET_FIRE)}, ORIGIN_TILE);
        const ActionBundle negativePartner = bundleOf({serviceComponent(BROKEN_PARTNER_SERVICE)}, ORIGIN_TILE);
        const ActionBundle brokenCapture = bundleOf({captureComponent(BROKEN_CAPTURE)}, ORIGIN_TILE);
        const ActionBundle lateFailure = bundleOf(
            {fireComponent(EXCHANGE_FIRE), serviceComponent(BROKEN_PARTNER_SERVICE)}, ORIGIN_TILE);
        const ActionBundle posthumousFailure = bundleOf(
            {fireComponent(LETHAL_COUNTER_FIRE), fireComponent(OVER_DAMAGE_FIRE)}, ORIGIN_TILE);
        expectAt(valueOf(wait, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid, "a well formed wait is valid", horizonTurns);
        expectAt(!valueOf(wait, BROKEN_ACTOR, NO_POSITION, horizonTurns).valid, "negative actor hp is malformed", horizonTurns);
        expectAt(!valueOf(wait, DEAD_ACTOR, NO_POSITION, horizonTurns).valid, "an actor with no hp left is malformed", horizonTurns);
        expectAt(!valueOf(posthumousFailure, FRAGILE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a malformed component after the actor's death is still malformed", horizonTurns);
        expectAt(!valueOf(wait, EXCHANGE_ACTOR, NO_POSITION, NEGATIVE_HORIZON_TURNS).valid,
                 "a negative horizon is malformed", horizonTurns);
        expectAt(!valueOf(overDamage, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "damage beyond the hp scale is malformed", horizonTurns);
        expectAt(!valueOf(deadTarget, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a target with no hp left is malformed", horizonTurns);
        expectAt(!valueOf(negativePartner, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "negative partner hp is malformed", horizonTurns);
        expectAt(!valueOf(brokenCapture, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "an unknown capture turn count is malformed", horizonTurns);
        expectAt(valueOf(overDamage, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).value().economicValue == 0,
                 "a malformed bundle scores nothing", horizonTurns);
        expectAt(valueOf(lateFailure, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).value().economicValue == 0,
                 "a late failure discards what earlier components booked", horizonTurns);
    }

    void testNegativeCostsAndShrinkingRepairsAreMalformed(std::int32_t horizonTurns)
    {
        const ActionBundle wait = bundleOf({}, ORIGIN_TILE);
        const ActionBundle pricelessTarget = bundleOf({fireComponent(PRICELESS_TARGET_FIRE)}, ORIGIN_TILE);
        const ActionBundle shrinking = bundleOf({serviceComponent(SHRINKING_REPAIR)}, ORIGIN_TILE);
        const ActionBundle paidBack = bundleOf({serviceComponent(PAID_BACK_REPAIR)}, ORIGIN_TILE);
        expectAt(!valueOf(wait, PRICELESS_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a negative actor replacement cost is malformed", horizonTurns);
        expectAt(!valueOf(pricelessTarget, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a negative target replacement cost is malformed", horizonTurns);
        expectAt(!valueOf(shrinking, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a service that lowers partner hp is malformed", horizonTurns);
        expectAt(!valueOf(paidBack, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a repair that pays funds back is malformed", horizonTurns);
    }

    void testComponentOrderCapsTheSecondCounter(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf(
            {fireComponent(FIRST_COUNTERED_FIRE), fireComponent(SECOND_COUNTERED_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, FRAGILE_ACTOR, NO_POSITION, horizonTurns);
        const MilliFunds spentBook = DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, SHORT_ACTOR_HP_STEPS);
        const MilliFunds uncappedBook = DEFAULT_CAPITAL_POLICY.bookValue(ACTOR_REPLACEMENT_COST, UNCAPPED_COUNTER_STEPS);
        expectAt(valuation.ledger.friendlyCapital == -spentBook,
                 "the second counter stops at the hp the first one left", horizonTurns);
        expectAt(valuation.ledger.friendlyCapital != -uncappedBook,
                 "the second counter does not restart from full hp", horizonTurns);
    }

    void testRepeatFireSpendsOneSharedTargetHpPool(std::int32_t horizonTurns)
    {
        const ActionBundle bundle = bundleOf(
            {fireComponent(POOL_FIRST_FIRE), fireComponent(POOL_SECOND_FIRE)}, ORIGIN_TILE);
        const BundleValuation valuation = valueOf(bundle, EXCHANGE_ACTOR, NO_POSITION, horizonTurns);
        expectAt(valuation.valid, "chained facts on one target are well formed", horizonTurns);
        expectAt(valuation.ledger.enemyCapital == POOLED_TARGET_BOOK,
                 "two hits book the target's hp once, not twice", horizonTurns);
        expectAt(valuation.continuation.targetContinuationRemoved == TARGET_BEST_SHOT_BEFORE,
                 "the chain removes the first before minus the final after", horizonTurns);
    }

    void testRepeatFireMustChainTheTargetFacts(std::int32_t horizonTurns)
    {
        const ActionBundle brokenHp = bundleOf(
            {fireComponent(FIRST_COUNTERED_FIRE), fireComponent(BROKEN_HP_CHAIN_FIRE)}, ORIGIN_TILE);
        const ActionBundle brokenShot = bundleOf(
            {fireComponent(FIRST_COUNTERED_FIRE), fireComponent(BROKEN_SHOT_CHAIN_FIRE)}, ORIGIN_TILE);
        expectAt(!valueOf(brokenHp, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a repeat fire that re-books the target's hp is malformed", horizonTurns);
        expectAt(!valueOf(brokenShot, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a repeat fire that forgets the earlier shot is malformed", horizonTurns);
    }

    void testEveryParticipantNeedsAUnitId(std::int32_t horizonTurns)
    {
        ActionBundle unnamedActor = bundleOf({fireComponent(EXCHANGE_FIRE)}, ORIGIN_TILE);
        unnamedActor.unitId = NO_UNIT;
        const ActionBundle unnamedTarget = bundleOf({fireComponent(UNNAMED_TARGET_FIRE)}, ORIGIN_TILE);
        const ActionBundle unnamedPartner = bundleOf({serviceComponent(UNNAMED_PARTNER_RESUPPLY)}, ORIGIN_TILE);
        expectAt(!valueOf(unnamedActor, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a bundle without an acting unit is malformed", horizonTurns);
        expectAt(!valueOf(unnamedTarget, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a fire without a target unit is malformed", horizonTurns);
        expectAt(!valueOf(unnamedPartner, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a service without a partner unit is malformed", horizonTurns);
    }

    void testValidityNeverDependsOnTheActorsHp(std::int32_t horizonTurns)
    {
        const ActionBundle posthumousChainBreak = bundleOf(
            {fireComponent(LETHAL_COUNTER_FIRE), fireComponent(BROKEN_HP_CHAIN_FIRE)}, ORIGIN_TILE);
        const ActionBundle posthumousDuplicate = bundleOf(
            {fireComponent(LETHAL_COUNTER_FIRE), serviceComponent(RESUPPLY), serviceComponent(RESUPPLY)}, ORIGIN_TILE);
        expectAt(!valueOf(posthumousChainBreak, FRAGILE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a chain break after the actor's death is still malformed", horizonTurns);
        expectAt(!valueOf(posthumousDuplicate, FRAGILE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a duplicate service after the actor's death is still malformed", horizonTurns);
    }

    void testARepairCannotReviveAndASelfRepairMustChain(std::int32_t horizonTurns)
    {
        const ActionBundle revival = bundleOf({serviceComponent(REVIVAL_REPAIR)}, ORIGIN_TILE);
        const ActionBundle staleSelfRepair = bundleOf(
            {fireComponent(EXCHANGE_FIRE), serviceComponent(SELF_REPAIR)}, ORIGIN_TILE);
        const ActionBundle chainedSelfRepair = bundleOf(
            {fireComponent(EXCHANGE_FIRE), serviceComponent(COUNTERED_SELF_REPAIR)}, ORIGIN_TILE);
        expectAt(!valueOf(revival, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "repairing a destroyed partner is malformed", horizonTurns);
        expectAt(!valueOf(staleSelfRepair, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a self repair that ignores the counter already taken is malformed", horizonTurns);
        expectAt(valueOf(chainedSelfRepair, EXCHANGE_ACTOR, NO_POSITION, horizonTurns).valid,
                 "a self repair that starts from the countered hp is legal", horizonTurns);
    }

    void testResolveHorizonPrefersARealTurnLimit()
    {
        expect(resolveHorizon(NO_TURN_LIMIT) == DEFAULT_ECONOMIC_RELEVANCE_HORIZON, "no turn limit falls back to the default horizon");
        expect(resolveHorizon(REMAINING_TURN_LIMIT) == REMAINING_TURN_LIMIT, "a real turn limit is the horizon");
        expect(resolveHorizon(FINAL_TURN_LIMIT) == FINAL_TURN_LIMIT, "the final turn leaves no horizon");
    }

    void testBundleKindFollowsMovementAndComponents()
    {
        expect(kindOf({}, ORIGIN_TILE) == PlanBundleKind::Wait, "no components and no move is a wait");
        expect(kindOf({fireComponent(EXCHANGE_FIRE)}, DESTINATION_TILE) == PlanBundleKind::MoveAndFire,
               "a move with fire is move and fire");
        expect(kindOf({serviceComponent(RESUPPLY)}, DESTINATION_TILE) == PlanBundleKind::MoveAndService,
               "a service that drives is move and service");
        expect(kindOf({captureComponent(NEUTRAL_CAPTURE), fireComponent(EXCHANGE_FIRE)}, ORIGIN_TILE) ==
                   PlanBundleKind::Compound,
               "two components make a compound bundle");
    }

    void runInvariantSuite(std::int32_t horizonTurns)
    {
        testMirroringEveryInputNegatesTheValue(horizonTurns);
        testEnemyCaptureIsExactlyTwiceTheNeutralCapture(horizonTurns);
        testDelayingAnEnemyCaptureOfOursPreservesTwiceTheNeutralDelay(horizonTurns);
        testKillIsRemainingBookPlusContinuationRemoved(horizonTurns);
        testAKillChargesNoCounter(horizonTurns);
        testAnActorThatDiesStopsBookingAndKeepsNoPost(horizonTurns);
        testDominatedPositionalOptionIsWorthNothing(horizonTurns);
        testMoveAndWaitHaveNoImmediateLedger(horizonTurns);
        testResourceUseChangesContinuationOnly(horizonTurns);
        testServiceOnSelfFeedsTheActorHpPool(horizonTurns);
        testASecondServiceOnOnePartnerIsMalformed(horizonTurns);
        testExposureIsTheBestSingleEnemyShotUntilTheAssignmentLands(horizonTurns);
        testAnEmptyNextShotSpanRepositionsForNothing(horizonTurns);
        testLostContinuationIsADifferenceNotAProportion(horizonTurns);
        testMalformedInputsAreReportedNotClamped(horizonTurns);
        testNegativeCostsAndShrinkingRepairsAreMalformed(horizonTurns);
        testComponentOrderCapsTheSecondCounter(horizonTurns);
        testRepeatFireSpendsOneSharedTargetHpPool(horizonTurns);
        testRepeatFireMustChainTheTargetFacts(horizonTurns);
        testEveryParticipantNeedsAUnitId(horizonTurns);
        testValidityNeverDependsOnTheActorsHp(horizonTurns);
        testARepairCannotReviveAndASelfRepairMustChain(horizonTurns);
    }
}

int main()
{
    for (const std::int32_t horizonTurns : SENSITIVITY_HORIZONS)
    {
        runInvariantSuite(horizonTurns);
    }
    testResolveHorizonPrefersARealTurnLimit();
    testBundleKindFollowsMovementAndComponents();
    if (failures == 0)
    {
        return 0;
    }
    return 1;
}
