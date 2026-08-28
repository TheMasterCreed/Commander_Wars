#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ai/coordinator/turnplan.h"

namespace
{
    using Coordinator::AttackClaim;
    using Coordinator::ConflictOutcome;
    using Coordinator::INVALID_TILE;
    using Coordinator::NO_ACTION;
    using Coordinator::NO_UNIT;
    using Coordinator::OccupancyEntry;
    using Coordinator::OrderingResult;
    using Coordinator::PlanActionState;
    using Coordinator::PlanBundleKind;
    using Coordinator::PlanChange;
    using Coordinator::PlanEdgeKind;
    using Coordinator::PlannedAction;
    using Coordinator::ReservationResult;
    using Coordinator::TerminalClass;
    using Coordinator::TerminalValue;
    using Coordinator::TilePoint;
    using Coordinator::TurnPlan;
    using Coordinator::UNIT_HP_STEPS;
    using Coordinator::VacateConflict;

    constexpr TerminalValue EQUAL_VALUE{TerminalClass::Unresolved, 100};
    constexpr TerminalValue LOWER_VALUE = EQUAL_VALUE;
    constexpr TerminalValue HIGHER_VALUE{TerminalClass::Unresolved, 400};
    constexpr TerminalValue FORCED_WIN_VALUE{TerminalClass::ForcedWin, 1};

    constexpr std::int32_t CHAIN_HEAD_UNIT = 13;
    constexpr std::int32_t CHAIN_MIDDLE_UNIT = 12;
    constexpr std::int32_t CHAIN_TAIL_UNIT = 11;
    constexpr TilePoint CHAIN_HEAD_TILE{3, 0};
    constexpr TilePoint CHAIN_HEAD_GOAL{4, 0};
    constexpr TilePoint CHAIN_MIDDLE_TILE{2, 0};
    constexpr TilePoint CHAIN_TAIL_TILE{1, 0};

    constexpr std::int32_t FOCUS_TARGET_UNIT = 21;
    constexpr std::int32_t FIRST_SHOOTER_UNIT = 22;
    constexpr std::int32_t SECOND_SHOOTER_UNIT = 23;
    constexpr std::int32_t THIRD_SHOOTER_UNIT = 24;
    constexpr std::int32_t FOCUS_TARGET_HP_STEPS = 4;
    constexpr std::int32_t SHOT_HP_STEPS = 3;
    constexpr std::int32_t LETHAL_REMAINDER_HP_STEPS = 1;
    constexpr std::int32_t PARTIAL_HP_STEPS = 2;
    constexpr std::int32_t STALE_HP_STEPS = 5;
    constexpr std::int32_t NO_HP_STEPS_LEFT = 0;
    constexpr std::int32_t OUT_OF_RANGE_HP_STEPS = UNIT_HP_STEPS + 1;
    constexpr TilePoint FOCUS_TARGET_TILE{6, 6};

    constexpr std::int32_t STANDING_UNIT = 31;
    constexpr TilePoint STANDING_TILE{2, 2};

    constexpr std::int32_t FIRST_CAPTURER_UNIT = 41;
    constexpr std::int32_t SECOND_CAPTURER_UNIT = 42;
    constexpr TilePoint CONTESTED_BUILDING{5, 5};

    constexpr std::int32_t INFANTRY_UNIT = 51;
    constexpr std::int32_t INDIRECT_UNIT = 52;
    constexpr TilePoint INDIRECT_POST{2, 0};
    constexpr TilePoint INFANTRY_TILE{1, 0};
    constexpr TilePoint INFANTRY_GOAL{3, 0};
    constexpr TilePoint INDIRECT_TILE{0, 0};
    constexpr TilePoint SIDE_STEP_TILE{1, 1};

    constexpr std::int32_t SCREEN_UNIT = 61;
    constexpr std::int32_t CAPTURE_UNIT = 62;
    constexpr TilePoint SCREEN_TILE{4, 1};
    constexpr TilePoint SCREEN_POST{4, 2};
    constexpr TilePoint CAPTURE_START{5, 1};
    constexpr TilePoint CAPTURE_BUILDING{5, 2};

    constexpr std::int32_t SWAP_LEFT_UNIT = 81;
    constexpr std::int32_t SWAP_RIGHT_UNIT = 82;
    constexpr std::int32_t FOLLOWER_UNIT = 83;
    constexpr TilePoint SWAP_LEFT_TILE{1, 0};
    constexpr TilePoint SWAP_RIGHT_TILE{2, 0};
    constexpr TilePoint FOLLOWER_TILE{1, 2};
    constexpr TilePoint FOLLOWER_GOAL{2, 2};
    constexpr std::size_t SWAP_CYCLE_MEMBERS = 2;

    constexpr std::int32_t REAR_UNIT = 91;
    constexpr std::int32_t FRONT_UNIT = 92;
    constexpr std::int32_t WALL_UNIT = 93;
    constexpr std::int32_t ESCORT_UNIT = 94;
    constexpr std::int32_t RELIEF_SHOOTER_UNIT = 95;
    constexpr std::int32_t WALL_HP_STEPS = 3;
    constexpr TilePoint REAR_TILE{1, 1};
    constexpr TilePoint WALL_TILE{2, 0};
    constexpr TilePoint FRONT_TILE{3, 0};
    constexpr TilePoint PUSH_GOAL{1, 0};
    constexpr TilePoint ESCORT_TILE{3, 1};
    constexpr TilePoint ESCORT_GOAL{2, 1};

    constexpr std::int32_t CONFLICT_LOW_VALUE_UNIT = 111;
    constexpr std::int32_t CONFLICT_HIGH_VALUE_UNIT = 112;
    constexpr std::int32_t TIED_LOW_ID_UNIT = 121;
    constexpr std::int32_t TIED_HIGH_ID_UNIT = 122;
    constexpr std::int32_t LEADER_UNIT = 123;
    constexpr std::int32_t LOSER_UNIT = 124;
    constexpr std::int32_t TRAILER_UNIT = 125;
    constexpr std::int32_t WINNER_UNIT = 126;
    constexpr TilePoint CONTESTED_TILE{7, 7};
    constexpr TilePoint LOW_VALUE_START{6, 7};
    constexpr TilePoint HIGH_VALUE_START{8, 7};
    constexpr TilePoint LEADER_TILE{6, 8};
    constexpr TilePoint LEADER_GOAL{6, 9};
    constexpr TilePoint LOSER_TILE{7, 8};
    constexpr TilePoint LOSER_GOAL{7, 9};
    constexpr TilePoint TRAILER_TILE{8, 8};
    constexpr TilePoint TRAILER_GOAL{8, 9};

    constexpr std::int32_t DEAD_TARGET_UNIT = 131;
    constexpr std::int32_t FULL_TARGET_UNIT = 132;
    constexpr std::int32_t BOUNDS_SHOOTER_UNIT = 134;

    constexpr std::int32_t SERVICE_UNIT = 141;
    constexpr std::int32_t RIVAL_SERVICE_UNIT = 142;
    constexpr std::int32_t SERVICE_PARTNER_UNIT = 143;

    constexpr std::int32_t REPLAN_UNIT = 151;
    constexpr std::int32_t SQUATTER_UNIT = 152;
    constexpr TilePoint REPLAN_TILE{9, 1};
    constexpr TilePoint REPLAN_FIRST_GOAL{9, 2};
    constexpr TilePoint REPLAN_SECOND_GOAL{9, 0};

    constexpr std::int32_t ABANDONED_UNIT = 161;
    constexpr std::int32_t BLOCKED_MOVER_UNIT = 162;
    constexpr std::int32_t DEAD_MOVER_UNIT = 163;
    constexpr TilePoint ABANDONED_TILE{4, 4};
    constexpr TilePoint ABANDONED_GOAL{4, 5};
    constexpr TilePoint BLOCKED_MOVER_TILE{3, 4};
    constexpr TilePoint DEAD_MOVER_TILE{2, 4};

    constexpr std::int32_t DIAMOND_TOP_UNIT = 181;
    constexpr std::int32_t DIAMOND_LEFT_UNIT = 182;
    constexpr std::int32_t DIAMOND_RIGHT_UNIT = 183;
    constexpr std::int32_t DIAMOND_BOTTOM_UNIT = 184;
    constexpr TilePoint DIAMOND_TOP_TILE{12, 0};
    constexpr TilePoint DIAMOND_TOP_GOAL{13, 0};
    constexpr TilePoint DIAMOND_LEFT_TILE{12, 1};
    constexpr TilePoint DIAMOND_LEFT_GOAL{13, 1};
    constexpr TilePoint DIAMOND_RIGHT_TILE{12, 2};
    constexpr TilePoint DIAMOND_RIGHT_GOAL{13, 2};
    constexpr TilePoint DIAMOND_BOTTOM_TILE{12, 3};
    constexpr TilePoint DIAMOND_BOTTOM_GOAL{13, 3};
    constexpr std::size_t DIAMOND_DEPENDENTS = 3;

    constexpr TilePoint REAR_GOAL{0, 1};

    constexpr std::int32_t COMMITTED_UNIT = 171;
    constexpr std::int32_t DEPENDENT_UNIT = 172;
    constexpr TilePoint COMMITTED_TILE{10, 1};
    constexpr TilePoint COMMITTED_GOAL{10, 2};
    constexpr TilePoint DEPENDENT_TILE{11, 1};
    constexpr TilePoint DEPENDENT_GOAL{11, 2};

    constexpr std::int32_t UNKNOWN_ACTION_INDEX = 99;

    int failures = 0;

    void expect(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    PlannedAction moveAction(std::int32_t unitId, TilePoint from, TilePoint to, TerminalValue value)
    {
        PlannedAction action;
        action.unitId = unitId;
        action.kind = PlanBundleKind::Move;
        action.path.push_back(from);
        action.path.push_back(to);
        action.destination = to;
        action.marginalValue = value;
        return action;
    }

    PlannedAction pathMoveAction(std::int32_t unitId, const std::vector<TilePoint> & path, TerminalValue value)
    {
        PlannedAction action;
        action.unitId = unitId;
        action.kind = PlanBundleKind::Move;
        action.path = path;
        action.destination = path.back();
        action.marginalValue = value;
        return action;
    }

    PlannedAction fireAction(std::int32_t unitId, TilePoint tile, TilePoint targetTile,
                             std::int32_t targetUnitId, TerminalValue value)
    {
        PlannedAction action;
        action.unitId = unitId;
        action.kind = PlanBundleKind::Fire;
        action.path.push_back(tile);
        action.destination = tile;
        action.target = targetTile;
        action.targetUnitId = targetUnitId;
        action.marginalValue = value;
        return action;
    }

    PlannedAction moveAndFireAction(std::int32_t unitId, TilePoint tile, TilePoint targetTile,
                                    std::int32_t targetUnitId, TerminalValue value)
    {
        PlannedAction action = fireAction(unitId, tile, targetTile, targetUnitId, value);
        action.kind = PlanBundleKind::MoveAndFire;
        return action;
    }

    PlannedAction captureAction(std::int32_t unitId, TilePoint from, TilePoint buildingTile, TerminalValue value)
    {
        PlannedAction action;
        action.unitId = unitId;
        action.kind = PlanBundleKind::MoveAndCapture;
        action.path.push_back(from);
        action.path.push_back(buildingTile);
        action.destination = buildingTile;
        action.target = buildingTile;
        action.marginalValue = value;
        return action;
    }

    ReservationResult claimDamage(TurnPlan & plan, std::int32_t actionIndex, std::int32_t targetUnitId,
                                  std::int32_t remainingHpSteps, std::int32_t damageSteps)
    {
        return plan.claimAttack(actionIndex, targetUnitId, remainingHpSteps, damageSteps).result;
    }

    std::vector<std::int32_t> orderedUnits(const TurnPlan & plan, const OrderingResult & result)
    {
        std::vector<std::int32_t> units;
        units.reserve(result.order.size());
        for (std::int32_t index : result.order)
        {
            units.push_back(plan.action(index).unitId);
        }
        return units;
    }

    bool sameSequence(const std::vector<std::int32_t> & left, const std::vector<std::int32_t> & right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t entry = 0; entry < left.size(); ++entry)
        {
            if (left[entry] != right[entry])
            {
                return false;
            }
        }
        return true;
    }

    std::vector<OccupancyEntry> chainOccupancy()
    {
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE});
        occupancy.push_back(OccupancyEntry{CHAIN_MIDDLE_UNIT, CHAIN_MIDDLE_TILE});
        occupancy.push_back(OccupancyEntry{CHAIN_TAIL_UNIT, CHAIN_TAIL_TILE});
        return occupancy;
    }

    void addChainActionsHeadFirst(TurnPlan & plan)
    {
        plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE));
        plan.addAction(moveAction(CHAIN_MIDDLE_UNIT, CHAIN_MIDDLE_TILE, CHAIN_HEAD_TILE, EQUAL_VALUE));
        plan.addAction(moveAction(CHAIN_TAIL_UNIT, CHAIN_TAIL_TILE, CHAIN_MIDDLE_TILE, EQUAL_VALUE));
    }

    void addChainActionsTailFirst(TurnPlan & plan)
    {
        plan.addAction(moveAction(CHAIN_TAIL_UNIT, CHAIN_TAIL_TILE, CHAIN_MIDDLE_TILE, EQUAL_VALUE));
        plan.addAction(moveAction(CHAIN_MIDDLE_UNIT, CHAIN_MIDDLE_TILE, CHAIN_HEAD_TILE, EQUAL_VALUE));
        plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE));
    }

    void testReinforcementChainMovesTheHeadFirstSoFollowersAdvance()
    {
        TurnPlan plan;
        addChainActionsHeadFirst(plan);
        const std::vector<OccupancyEntry> occupancy = chainOccupancy();
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "a moving chain produces no reservation conflicts");
        expect(plan.edgeCount() == 2, "each follower gets one vacate edge");
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{CHAIN_HEAD_UNIT, CHAIN_MIDDLE_UNIT, CHAIN_TAIL_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "the chain head executes first, tail last");
        expect(result.cycleMembers.empty() && result.blockedByCycle.empty(), "a chain leaves nothing unordered");
        TurnPlan unordered;
        addChainActionsHeadFirst(unordered);
        const std::vector<std::int32_t> tieBreakOnly{CHAIN_TAIL_UNIT, CHAIN_MIDDLE_UNIT, CHAIN_HEAD_UNIT};
        expect(sameSequence(orderedUnits(unordered, unordered.executionOrder()), tieBreakOnly),
               "without vacate edges the chain falls back to the unit id tie break");
    }

    void testFocusFireStopsAtLethalAndLaterClaimsAreOverkill()
    {
        TurnPlan plan;
        const std::int32_t first = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        const std::int32_t second = plan.addAction(
            fireAction(SECOND_SHOOTER_UNIT, CHAIN_MIDDLE_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        const std::int32_t third = plan.addAction(
            fireAction(THIRD_SHOOTER_UNIT, CHAIN_HEAD_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        expect(claimDamage(plan, first, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Granted,
               "the first shot below lethal is granted");
        expect(!plan.targetIsLethal(FOCUS_TARGET_UNIT), "three of four hp steps is not lethal yet");
        expect(claimDamage(plan, second, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Granted,
               "the shot that reaches lethal is still granted");
        expect(plan.targetIsLethal(FOCUS_TARGET_UNIT), "cumulative damage at remaining hp is lethal");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == FOCUS_TARGET_HP_STEPS, "planned damage never exceeds remaining hp");
        expect(plan.action(second).plannedDamageSteps == LETHAL_REMAINDER_HP_STEPS, "the lethal shot only books what is left");
        expect(claimDamage(plan, third, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Overkill,
               "a shot claimed after lethal is overkill");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == FOCUS_TARGET_HP_STEPS, "an overkill claim adds no damage");
        expect(plan.action(third).plannedDamageSteps == 0, "an overkill claim leaves the action undamaged");
    }

    void testRepeatClaimReplacesTheSameActionsShare()
    {
        TurnPlan plan;
        const std::int32_t shooter = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        expect(claimDamage(plan, shooter, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the first rescore is granted");
        expect(claimDamage(plan, shooter, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Granted,
               "the rescored claim is granted again");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == SHOT_HP_STEPS, "a repeat claim replaces its own share");
        expect(plan.action(shooter).plannedDamageSteps == SHOT_HP_STEPS, "the action agrees with the reservation");
    }

    void testStaleRemainingHpIsReportedNotGranted()
    {
        TurnPlan plan;
        const std::int32_t first = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        const std::int32_t second = plan.addAction(
            fireAction(SECOND_SHOOTER_UNIT, CHAIN_MIDDLE_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        expect(claimDamage(plan, first, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the first claim fixes the remaining hp");
        expect(claimDamage(plan, second, FOCUS_TARGET_UNIT, STALE_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::StaleTarget,
               "a disagreeing remaining hp is reported");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == PARTIAL_HP_STEPS, "a stale claim books nothing");
        expect(plan.remainingHpStepsOn(FOCUS_TARGET_UNIT) == FOCUS_TARGET_HP_STEPS, "the first claim's remaining hp survives");
    }

    void testMalformedRequestsAreReported()
    {
        TurnPlan plan;
        const std::int32_t shooter = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        const std::int32_t mover = plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE));
        expect(claimDamage(plan, UNKNOWN_ACTION_INDEX, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS)
                   == ReservationResult::Invalid,
               "a claim from an unknown action is invalid");
        expect(claimDamage(plan, shooter, NO_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Invalid,
               "a claim without a target is invalid");
        expect(claimDamage(plan, shooter, FOCUS_TARGET_UNIT, OUT_OF_RANGE_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Invalid,
               "remaining hp out of range is invalid");
        expect(claimDamage(plan, shooter, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, OUT_OF_RANGE_HP_STEPS)
                   == ReservationResult::Invalid,
               "damage out of range is invalid");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == 0, "no invalid request books damage");
        expect(plan.claimDestination(UNKNOWN_ACTION_INDEX, CONTESTED_TILE) == ReservationResult::Invalid,
               "a destination claim from an unknown action is invalid");
        expect(plan.claimDestination(mover, INVALID_TILE) == ReservationResult::Invalid, "an invalid tile cannot be claimed");
        expect(plan.claimService(shooter, NO_UNIT) == ReservationResult::Invalid, "a service claim without a partner is invalid");
        expect(!plan.commit(UNKNOWN_ACTION_INDEX), "commit reports an unknown index");
        expect(!plan.addEdge(shooter, UNKNOWN_ACTION_INDEX, PlanEdgeKind::Screen), "addEdge reports an unknown index");
        expect(!plan.addEdge(shooter, shooter, PlanEdgeKind::Screen), "addEdge refuses a self loop");
        expect(plan.addEdge(shooter, mover, PlanEdgeKind::Screen), "the first edge is accepted");
        expect(!plan.addEdge(shooter, mover, PlanEdgeKind::Coverage), "addEdge refuses a repeated pair");
        expect(plan.edgeCount() == 1, "a refused edge is not stored");
    }

    void testMalformedActionShapesAreRejected()
    {
        TurnPlan plan;
        PlannedAction pathless;
        pathless.unitId = CHAIN_HEAD_UNIT;
        pathless.kind = PlanBundleKind::Move;
        pathless.destination = CHAIN_HEAD_GOAL;
        expect(plan.addAction(pathless) == NO_ACTION, "a moving bundle without a path is rejected");
        PlannedAction mismatched = moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE);
        mismatched.destination = CHAIN_MIDDLE_TILE;
        expect(plan.addAction(mismatched) == NO_ACTION, "a path that contradicts the destination is rejected");
        PlannedAction unowned = moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE);
        unowned.unitId = NO_UNIT;
        expect(plan.addAction(unowned) == NO_ACTION, "an action without a unit is rejected");
        expect(plan.actionCount() == 0, "a rejected action is not stored");
    }

    void testDuplicateUnitActionIsRejected()
    {
        TurnPlan plan;
        expect(plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, EQUAL_VALUE)) == 0,
               "the first action for a unit is accepted");
        expect(plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_MIDDLE_TILE, EQUAL_VALUE)) == NO_ACTION,
               "a second action for the same unit is rejected");
        expect(plan.actionCount() == 1, "one planned action per unit");
    }

    void testDestinationOnTheOwnTileNeedsNoEdge()
    {
        TurnPlan plan;
        plan.addAction(moveAction(STANDING_UNIT, STANDING_TILE, STANDING_TILE, EQUAL_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{STANDING_UNIT, STANDING_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "a unit does not block itself");
        expect(plan.edgeCount() == 0, "a destination on the own tile adds no edge");
    }

    void testCaptureClaimsAreExclusivePerBuilding()
    {
        TurnPlan plan;
        const std::int32_t first = plan.addAction(
            captureAction(FIRST_CAPTURER_UNIT, CHAIN_TAIL_TILE, CONTESTED_BUILDING, EQUAL_VALUE));
        const std::int32_t second = plan.addAction(
            captureAction(SECOND_CAPTURER_UNIT, CHAIN_HEAD_TILE, CONTESTED_BUILDING, EQUAL_VALUE));
        expect(plan.claimCapture(first, CONTESTED_BUILDING) == ReservationResult::Granted, "the first capturer claims the building");
        expect(plan.claimCapture(second, CONTESTED_BUILDING) == ReservationResult::Conflict, "a second capturer conflicts");
        expect(plan.claimCapture(first, CONTESTED_BUILDING) == ReservationResult::Granted, "the holder may reclaim its own building");
        expect(plan.captureClaimant(CONTESTED_BUILDING) == first, "the first capturer keeps the claim");
    }

    void testInfantryVacatesThePostBeforeTheIndirectTakesIt()
    {
        TurnPlan plan;
        plan.addAction(moveAction(INFANTRY_UNIT, INDIRECT_POST, INFANTRY_GOAL, LOWER_VALUE));
        plan.addAction(moveAction(INDIRECT_UNIT, INDIRECT_TILE, INDIRECT_POST, HIGHER_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{INFANTRY_UNIT, INDIRECT_POST});
        occupancy.push_back(OccupancyEntry{INDIRECT_UNIT, INDIRECT_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "a vacating occupant is an edge, not a conflict");
        expect(plan.edgeCount() == 1, "the indirect waits on one vacate edge");
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{INFANTRY_UNIT, INDIRECT_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "the infantry vacates before the higher value indirect moves");
    }

    void testScreenEstablishesBeforeTheThreatenedCapture()
    {
        TurnPlan plan;
        const std::int32_t screen = plan.addAction(moveAction(SCREEN_UNIT, SCREEN_TILE, SCREEN_POST, LOWER_VALUE));
        const std::int32_t capture = plan.addAction(captureAction(CAPTURE_UNIT, CAPTURE_START, CAPTURE_BUILDING, HIGHER_VALUE));
        expect(plan.addEdge(screen, capture, PlanEdgeKind::Screen), "the screen edge is accepted");
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{SCREEN_UNIT, CAPTURE_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "the screen runs before the more valuable capture");
    }

    void testStayingOccupantIsAReservationConflictNotAnEdge()
    {
        TurnPlan plan;
        const std::int32_t fire = plan.addAction(
            fireAction(INDIRECT_UNIT, INDIRECT_POST, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, LOWER_VALUE));
        const std::int32_t advance = plan.addAction(moveAction(INFANTRY_UNIT, INFANTRY_TILE, INDIRECT_POST, HIGHER_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{INDIRECT_UNIT, INDIRECT_POST});
        occupancy.push_back(OccupancyEntry{INFANTRY_UNIT, INFANTRY_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(plan.edgeCount() == 0, "an occupant that stays produces no ordering edge");
        expect(conflicts.size() == 1, "the blocked mover is reported once");
        expect(conflicts.front().moverAction == advance, "the conflict names the mover");
        expect(conflicts.front().occupantUnit == INDIRECT_UNIT, "the conflict names the staying unit");
        expect(conflicts.front().occupantAction == fire, "the conflict names the staying action");
    }

    void testStationaryIndirectFiresBeforeNearbyMovesViaCoverageEdge()
    {
        TurnPlan plan;
        const std::int32_t fire = plan.addAction(
            fireAction(INDIRECT_UNIT, INDIRECT_POST, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, LOWER_VALUE));
        const std::int32_t advance = plan.addAction(moveAction(INFANTRY_UNIT, INFANTRY_TILE, SIDE_STEP_TILE, HIGHER_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{INDIRECT_UNIT, INDIRECT_POST});
        occupancy.push_back(OccupancyEntry{INFANTRY_UNIT, INFANTRY_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "a move to a free tile is neither edge nor conflict");
        expect(plan.addEdge(fire, advance, PlanEdgeKind::Coverage), "the coverage edge is accepted");
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{INDIRECT_UNIT, INFANTRY_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "the stationary shot resolves before nearby movement");
    }

    void testMovementCycleIsReportedApartFromItsDownstream()
    {
        TurnPlan plan;
        plan.addAction(moveAction(SWAP_LEFT_UNIT, SWAP_LEFT_TILE, SWAP_RIGHT_TILE, EQUAL_VALUE));
        const std::int32_t right = plan.addAction(moveAction(SWAP_RIGHT_UNIT, SWAP_RIGHT_TILE, SWAP_LEFT_TILE, EQUAL_VALUE));
        const std::int32_t follower = plan.addAction(moveAction(FOLLOWER_UNIT, FOLLOWER_TILE, FOLLOWER_GOAL, EQUAL_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{SWAP_LEFT_UNIT, SWAP_LEFT_TILE});
        occupancy.push_back(OccupancyEntry{SWAP_RIGHT_UNIT, SWAP_RIGHT_TILE});
        occupancy.push_back(OccupancyEntry{FOLLOWER_UNIT, FOLLOWER_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "both swappers move, so both get edges");
        expect(plan.edgeCount() == 2, "a tile swap produces two vacate edges");
        expect(plan.addEdge(right, follower, PlanEdgeKind::Screen), "the downstream edge is accepted");
        const OrderingResult result = plan.executionOrder();
        expect(result.order.empty(), "a cycle upstream orders nothing");
        expect(result.cycleMembers.size() == SWAP_CYCLE_MEMBERS, "only the swappers are cycle members");
        expect(result.blockedByCycle.size() == 1 && result.blockedByCycle.front() == follower,
               "the innocent follower is blocked by the cycle, not part of it");
    }

    void testWallbreakNeedsLethalAndFailureReleasesTheKill()
    {
        TurnPlan plan;
        const std::int32_t rear = plan.addAction(
            moveAndFireAction(REAR_UNIT, REAR_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<TilePoint> pushPath;
        pushPath.push_back(FRONT_TILE);
        pushPath.push_back(WALL_TILE);
        pushPath.push_back(PUSH_GOAL);
        const std::int32_t front = plan.addAction(pathMoveAction(FRONT_UNIT, pushPath, HIGHER_VALUE));
        const std::int32_t escort = plan.addAction(moveAction(ESCORT_UNIT, ESCORT_TILE, ESCORT_GOAL, HIGHER_VALUE));
        const std::int32_t relief = plan.addAction(
            fireAction(RELIEF_SHOOTER_UNIT, CHAIN_TAIL_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{REAR_UNIT, REAR_TILE});
        occupancy.push_back(OccupancyEntry{WALL_UNIT, WALL_TILE});
        occupancy.push_back(OccupancyEntry{FRONT_UNIT, FRONT_TILE});
        occupancy.push_back(OccupancyEntry{ESCORT_UNIT, ESCORT_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(conflicts.empty(), "an attack on an occupied tile is not a destination conflict");
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the partial wallbreak claim is granted");
        expect(!plan.addWallbreakEdge(rear, front), "the push is refused while the kill falls short");
        expect(plan.edgeCount() == 0, "a refused wallbreak stores no edge");
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, WALL_HP_STEPS) == ReservationResult::Granted,
               "the rescored wallbreak claim is granted");
        expect(plan.targetIsLethal(WALL_UNIT), "the rescored claim reaches lethal");
        expect(plan.addWallbreakEdge(rear, front), "the push is legal once the kill is lethal");
        expect(plan.addEdge(rear, escort, PlanEdgeKind::Screen), "the escort edge is accepted");
        expect(plan.commit(escort), "the escort commits");
        const OrderingResult order = plan.executionOrder();
        const std::vector<std::int32_t> expected{REAR_UNIT, FRONT_UNIT, ESCORT_UNIT, RELIEF_SHOOTER_UNIT};
        expect(sameSequence(orderedUnits(plan, order), expected), "the wallbreak runs before the push it enables");
        const std::vector<std::int32_t> affected = plan.markFailed(rear);
        expect(plan.action(rear).state == PlanActionState::Abandoned, "the failed action is abandoned");
        expect(plan.action(front).state == PlanActionState::Blocked, "the dependent push is blocked");
        expect(affected.size() == 1 && affected.front() == front, "only the pending dependent is reported");
        expect(plan.action(escort).state == PlanActionState::Committed, "a committed dependent is never touched");
        expect(!plan.targetIsLethal(WALL_UNIT), "the abandoned kill releases its damage claim");
        expect(plan.plannedDamageOn(WALL_UNIT) == 0, "no damage stays booked on the wall");
        expect(claimDamage(plan, relief, WALL_UNIT, WALL_HP_STEPS, WALL_HP_STEPS) == ReservationResult::Granted,
               "another shooter can book the wall after the failure");
    }

    void testInsertionOrderDoesNotChangeExecutionOrder()
    {
        TurnPlan freeHeadFirst;
        addChainActionsHeadFirst(freeHeadFirst);
        TurnPlan freeTailFirst;
        addChainActionsTailFirst(freeTailFirst);
        expect(sameSequence(orderedUnits(freeHeadFirst, freeHeadFirst.executionOrder()),
                            orderedUnits(freeTailFirst, freeTailFirst.executionOrder())),
               "unconstrained actions order by value and unit id, not by insertion");
        TurnPlan headFirst;
        addChainActionsHeadFirst(headFirst);
        TurnPlan tailFirst;
        addChainActionsTailFirst(tailFirst);
        const std::vector<OccupancyEntry> occupancy = chainOccupancy();
        headFirst.addVacateEdges(occupancy);
        tailFirst.addVacateEdges(occupancy);
        expect(sameSequence(orderedUnits(headFirst, headFirst.executionOrder()),
                            orderedUnits(tailFirst, tailFirst.executionOrder())),
               "permuted insertion yields the same dependency order");
    }

    void testDestinationConflictKeepsTheHigherValueThenTheLowerUnitId()
    {
        TurnPlan plan;
        const std::int32_t lowValue = plan.addAction(
            moveAction(CONFLICT_LOW_VALUE_UNIT, LOW_VALUE_START, CONTESTED_TILE, LOWER_VALUE));
        const std::int32_t highValue = plan.addAction(
            moveAction(CONFLICT_HIGH_VALUE_UNIT, HIGH_VALUE_START, CONTESTED_TILE, HIGHER_VALUE));
        expect(plan.claimDestination(lowValue, CONTESTED_TILE) == ReservationResult::Granted, "the first mover claims the tile");
        expect(plan.claimDestination(highValue, CONTESTED_TILE) == ReservationResult::Conflict, "a second mover conflicts");
        expect(plan.resolveDestinationConflict(lowValue, highValue).loser == lowValue, "the lower value action loses the tile");
        expect(plan.action(lowValue).state == PlanActionState::Blocked, "the loser is blocked for replanning");
        expect(plan.destinationClaimant(CONTESTED_TILE) == NO_ACTION, "the loser releases its destination claim");
        expect(plan.claimDestination(highValue, CONTESTED_TILE) == ReservationResult::Granted, "the winner may now claim the tile");

        TurnPlan tied;
        const std::int32_t lowId = tied.addAction(moveAction(TIED_LOW_ID_UNIT, LOW_VALUE_START, CONTESTED_TILE, EQUAL_VALUE));
        const std::int32_t highId = tied.addAction(moveAction(TIED_HIGH_ID_UNIT, HIGH_VALUE_START, CONTESTED_TILE, EQUAL_VALUE));
        expect(tied.resolveDestinationConflict(lowId, highId).loser == highId, "equal value ties to the lower unit id");

        TurnPlan committed;
        const std::int32_t settled = committed.addAction(
            moveAction(CONFLICT_LOW_VALUE_UNIT, LOW_VALUE_START, CONTESTED_TILE, LOWER_VALUE));
        const std::int32_t challenger = committed.addAction(
            moveAction(CONFLICT_HIGH_VALUE_UNIT, HIGH_VALUE_START, CONTESTED_TILE, HIGHER_VALUE));
        expect(committed.commit(settled), "the settled action commits");
        const ConflictOutcome refused = committed.resolveDestinationConflict(settled, challenger);
        expect(refused.loser == NO_ACTION, "a committed loser is refused");
        expect(refused.blocked.empty(), "a refused conflict blocks nothing");
        expect(committed.action(settled).state == PlanActionState::Committed, "the committed action keeps its state");
        expect(committed.action(challenger).state == PlanActionState::Pending, "the refused conflict changes nothing");
    }

    void testConflictLoserBlocksItsDependents()
    {
        TurnPlan plan;
        const std::int32_t leader = plan.addAction(moveAction(LEADER_UNIT, LEADER_TILE, LEADER_GOAL, EQUAL_VALUE));
        const std::int32_t loser = plan.addAction(moveAction(LOSER_UNIT, LOSER_TILE, LOSER_GOAL, LOWER_VALUE));
        const std::int32_t trailer = plan.addAction(moveAction(TRAILER_UNIT, TRAILER_TILE, TRAILER_GOAL, EQUAL_VALUE));
        const std::int32_t winner = plan.addAction(moveAction(WINNER_UNIT, HIGH_VALUE_START, CONTESTED_TILE, HIGHER_VALUE));
        expect(plan.addEdge(leader, loser, PlanEdgeKind::Vacate), "the upstream edge is accepted");
        expect(plan.addEdge(loser, trailer, PlanEdgeKind::Vacate), "the downstream edge is accepted");
        const ConflictOutcome outcome = plan.resolveDestinationConflict(loser, winner);
        expect(outcome.loser == loser, "the lower value action loses");
        const std::vector<std::int32_t> expectedBlocked{loser, trailer};
        expect(sameSequence(outcome.blocked, expectedBlocked), "the loser and its dependent are reported ascending");
        expect(plan.action(loser).state == PlanActionState::Blocked, "the loser is blocked");
        expect(plan.action(trailer).state == PlanActionState::Blocked, "the loser's dependent is blocked too");
        expect(plan.action(leader).state == PlanActionState::Pending, "an upstream action is untouched");
    }

    void testForcedWinOutranksAHigherUnresolvedValue()
    {
        TurnPlan plan;
        plan.addAction(moveAction(CHAIN_TAIL_UNIT, CHAIN_TAIL_TILE, CHAIN_MIDDLE_TILE, HIGHER_VALUE));
        plan.addAction(moveAction(CHAIN_HEAD_UNIT, CHAIN_HEAD_TILE, CHAIN_HEAD_GOAL, FORCED_WIN_VALUE));
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{CHAIN_HEAD_UNIT, CHAIN_TAIL_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "a proven win outranks a larger economic value");
    }

    void testAbandonedActionsLeaveTheLivePlan()
    {
        TurnPlan plan;
        const std::int32_t squatter = plan.addAction(moveAction(ABANDONED_UNIT, ABANDONED_TILE, ABANDONED_GOAL, EQUAL_VALUE));
        const std::int32_t mover = plan.addAction(moveAction(BLOCKED_MOVER_UNIT, BLOCKED_MOVER_TILE, ABANDONED_TILE, EQUAL_VALUE));
        const std::int32_t deadMover = plan.addAction(
            moveAction(DEAD_MOVER_UNIT, DEAD_MOVER_TILE, BLOCKED_MOVER_TILE, EQUAL_VALUE));
        plan.markFailed(squatter);
        plan.markFailed(deadMover);
        std::vector<OccupancyEntry> occupancy;
        occupancy.push_back(OccupancyEntry{ABANDONED_UNIT, ABANDONED_TILE});
        occupancy.push_back(OccupancyEntry{BLOCKED_MOVER_UNIT, BLOCKED_MOVER_TILE});
        occupancy.push_back(OccupancyEntry{DEAD_MOVER_UNIT, DEAD_MOVER_TILE});
        const std::vector<VacateConflict> conflicts = plan.addVacateEdges(occupancy);
        expect(plan.edgeCount() == 0, "neither an abandoned occupant nor an abandoned mover adds an edge");
        expect(conflicts.size() == 1, "only the live mover reports a conflict");
        expect(conflicts.front().moverAction == mover, "the live mover is the one reported");
        expect(conflicts.front().occupantAction == NO_ACTION, "the abandoned occupant has no live action");
        const OrderingResult result = plan.executionOrder();
        const std::vector<std::int32_t> expected{BLOCKED_MOVER_UNIT};
        expect(sameSequence(orderedUnits(plan, result), expected), "an abandoned action is absent from the order");
        expect(result.cycleMembers.empty() && result.blockedByCycle.empty(), "a dead action is not reported as unordered");
    }

    void testReplanReleasesTheOldClaims()
    {
        TurnPlan plan;
        const std::int32_t replanned = plan.addAction(moveAction(REPLAN_UNIT, REPLAN_TILE, REPLAN_FIRST_GOAL, EQUAL_VALUE));
        const std::int32_t squatter = plan.addAction(moveAction(SQUATTER_UNIT, LOW_VALUE_START, CONTESTED_TILE, EQUAL_VALUE));
        expect(plan.claimDestination(replanned, REPLAN_FIRST_GOAL) == ReservationResult::Granted, "the first destination is claimed");
        expect(plan.replan(replanned, moveAction(REPLAN_UNIT, REPLAN_TILE, REPLAN_SECOND_GOAL, HIGHER_VALUE)).accepted,
               "a pending action can be replanned");
        expect(plan.action(replanned).state == PlanActionState::Pending, "a replanned action is pending again");
        expect(plan.action(replanned).destination == REPLAN_SECOND_GOAL, "the replacement destination is stored");
        expect(plan.action(replanned).unitId == REPLAN_UNIT, "the unit id survives the replan");
        expect(plan.destinationClaimant(REPLAN_FIRST_GOAL) == NO_ACTION, "the old destination claim is released");
        expect(plan.claimDestination(squatter, REPLAN_FIRST_GOAL) == ReservationResult::Granted,
               "another action can take the released tile");
        PlannedAction pathless;
        pathless.unitId = REPLAN_UNIT;
        pathless.kind = PlanBundleKind::Move;
        pathless.destination = REPLAN_FIRST_GOAL;
        expect(!plan.replan(replanned, pathless).accepted, "replan rejects what addAction rejects");
        expect(plan.action(replanned).destination == REPLAN_SECOND_GOAL, "a rejected replan changes nothing");
    }

    void testHpStepBoundsForOverkillAndLethal()
    {
        TurnPlan plan;
        const std::int32_t shooter = plan.addAction(
            fireAction(BOUNDS_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, DEAD_TARGET_UNIT, EQUAL_VALUE));
        expect(claimDamage(plan, shooter, DEAD_TARGET_UNIT, NO_HP_STEPS_LEFT, SHOT_HP_STEPS) == ReservationResult::Overkill,
               "a target with no hp steps left is already lethal");
        expect(!plan.targetIsLethal(DEAD_TARGET_UNIT), "a refused probe stores no claim");
        expect(plan.remainingHpStepsOn(DEAD_TARGET_UNIT) == NO_HP_STEPS_LEFT, "a refused probe books no remaining hp");
        expect(claimDamage(plan, shooter, DEAD_TARGET_UNIT, WALL_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "a later claim with a fresh remaining hp is granted, not stale");
        expect(claimDamage(plan, shooter, FULL_TARGET_UNIT, UNIT_HP_STEPS, UNIT_HP_STEPS) == ReservationResult::Granted,
               "a full strength claim on a full strength target is granted");
        expect(plan.targetIsLethal(FULL_TARGET_UNIT), "a full strength claim reaches lethal");
        expect(plan.plannedDamageOn(FULL_TARGET_UNIT) == UNIT_HP_STEPS, "the full claim books every hp step");
    }

    void testReleasingOneContributorKeepsTheOther()
    {
        TurnPlan plan;
        const std::int32_t first = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        const std::int32_t second = plan.addAction(
            fireAction(SECOND_SHOOTER_UNIT, CHAIN_MIDDLE_TILE, FOCUS_TARGET_TILE, FOCUS_TARGET_UNIT, EQUAL_VALUE));
        expect(claimDamage(plan, first, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, LETHAL_REMAINDER_HP_STEPS)
                   == ReservationResult::Granted,
               "the first contributor books one hp step");
        expect(claimDamage(plan, second, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the second contributor books two hp steps");
        plan.markFailed(first);
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == PARTIAL_HP_STEPS, "only the failed contributor's share is removed");
        expect(plan.remainingHpStepsOn(FOCUS_TARGET_UNIT) == FOCUS_TARGET_HP_STEPS, "the surviving claim keeps its remaining hp");
        expect(!plan.targetIsLethal(FOCUS_TARGET_UNIT), "two of four hp steps is not lethal");
        expect(claimDamage(plan, second, FOCUS_TARGET_UNIT, FOCUS_TARGET_HP_STEPS, SHOT_HP_STEPS) == ReservationResult::Granted,
               "the surviving contributor can rescore");
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == SHOT_HP_STEPS, "the survivor's own share is still tracked");
        plan.markFailed(second);
        expect(plan.plannedDamageOn(FOCUS_TARGET_UNIT) == 0, "the last contributor drops the claim");
        expect(plan.remainingHpStepsOn(FOCUS_TARGET_UNIT) == 0, "no claim is left behind");
    }

    void testWallbreakEdgeDropsWhenTheKillFallsShort()
    {
        TurnPlan plan;
        const std::int32_t rear = plan.addAction(
            moveAndFireAction(REAR_UNIT, REAR_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<TilePoint> pushPath;
        pushPath.push_back(FRONT_TILE);
        pushPath.push_back(WALL_TILE);
        pushPath.push_back(PUSH_GOAL);
        const std::int32_t front = plan.addAction(pathMoveAction(FRONT_UNIT, pushPath, HIGHER_VALUE));
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, WALL_HP_STEPS) == ReservationResult::Granted,
               "the lethal claim is granted");
        expect(plan.addWallbreakEdge(rear, front), "the push is legal while the kill is lethal");
        const AttackClaim rescore = plan.claimAttack(rear, WALL_UNIT, WALL_HP_STEPS, LETHAL_REMAINDER_HP_STEPS);
        expect(rescore.result == ReservationResult::Granted, "the downward rescore is granted");
        expect(rescore.blocked.size() == 1 && rescore.blocked.front() == front,
               "the rescore reports the push it blocked");
        expect(!plan.targetIsLethal(WALL_UNIT), "the rescored kill falls short");
        expect(plan.edgeCount() == 0, "the stale wallbreak edge is removed");
        expect(plan.action(front).state == PlanActionState::Blocked, "the push is blocked for replanning");
        expect(plan.action(rear).state == PlanActionState::Pending, "the attack itself stays pending");
    }

    void testReplanningTheAttackDropsItsWallbreak()
    {
        TurnPlan plan;
        const std::int32_t rear = plan.addAction(
            moveAndFireAction(REAR_UNIT, REAR_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<TilePoint> pushPath;
        pushPath.push_back(FRONT_TILE);
        pushPath.push_back(WALL_TILE);
        pushPath.push_back(PUSH_GOAL);
        const std::int32_t front = plan.addAction(pathMoveAction(FRONT_UNIT, pushPath, HIGHER_VALUE));
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, WALL_HP_STEPS) == ReservationResult::Granted,
               "the lethal claim is granted");
        expect(plan.addWallbreakEdge(rear, front), "the push is legal while the kill is lethal");
        const PlanChange change = plan.replan(rear, moveAction(REAR_UNIT, REAR_TILE, REAR_GOAL, EQUAL_VALUE));
        expect(change.accepted, "the attacker is replanned");
        expect(change.blocked.size() == 1 && change.blocked.front() == front, "the replan reports the push it blocked");
        expect(!plan.targetIsLethal(WALL_UNIT), "the replan releases the kill");
        expect(plan.edgeCount() == 0, "the wallbreak edge does not survive the replan");
        expect(plan.action(front).state == PlanActionState::Blocked, "the push is blocked for replanning");
    }

    void testCoAttackerRescoreDropsTheWallbreak()
    {
        TurnPlan plan;
        const std::int32_t helper = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, WALL_TILE, WALL_UNIT, EQUAL_VALUE));
        const std::int32_t rear = plan.addAction(
            moveAndFireAction(REAR_UNIT, REAR_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<TilePoint> pushPath;
        pushPath.push_back(FRONT_TILE);
        pushPath.push_back(WALL_TILE);
        pushPath.push_back(PUSH_GOAL);
        const std::int32_t front = plan.addAction(pathMoveAction(FRONT_UNIT, pushPath, HIGHER_VALUE));
        expect(claimDamage(plan, helper, WALL_UNIT, WALL_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the helper books two of three hp steps");
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, LETHAL_REMAINDER_HP_STEPS) == ReservationResult::Granted,
               "the rear unit books the last hp step");
        expect(plan.targetIsLethal(WALL_UNIT), "the shared kill is lethal");
        expect(plan.addWallbreakEdge(rear, front), "the push is legal while the shared kill is lethal");
        expect(claimDamage(plan, helper, WALL_UNIT, WALL_HP_STEPS, LETHAL_REMAINDER_HP_STEPS) == ReservationResult::Granted,
               "the helper rescores downward");
        expect(!plan.targetIsLethal(WALL_UNIT), "the shared kill falls short");
        expect(plan.edgeCount() == 0, "a co attacker's rescore drops the other attacker's wallbreak");
        expect(plan.action(front).state == PlanActionState::Blocked, "the push is blocked for replanning");
        expect(plan.action(rear).state == PlanActionState::Pending, "the untouched attacker stays pending");
    }

    void testCoAttackerFailureDropsTheWallbreak()
    {
        TurnPlan plan;
        const std::int32_t helper = plan.addAction(
            fireAction(FIRST_SHOOTER_UNIT, CHAIN_TAIL_TILE, WALL_TILE, WALL_UNIT, EQUAL_VALUE));
        const std::int32_t rear = plan.addAction(
            moveAndFireAction(REAR_UNIT, REAR_TILE, WALL_TILE, WALL_UNIT, LOWER_VALUE));
        std::vector<TilePoint> pushPath;
        pushPath.push_back(FRONT_TILE);
        pushPath.push_back(WALL_TILE);
        pushPath.push_back(PUSH_GOAL);
        const std::int32_t front = plan.addAction(pathMoveAction(FRONT_UNIT, pushPath, HIGHER_VALUE));
        expect(claimDamage(plan, helper, WALL_UNIT, WALL_HP_STEPS, PARTIAL_HP_STEPS) == ReservationResult::Granted,
               "the helper books two of three hp steps");
        expect(claimDamage(plan, rear, WALL_UNIT, WALL_HP_STEPS, LETHAL_REMAINDER_HP_STEPS) == ReservationResult::Granted,
               "the rear unit books the last hp step");
        expect(plan.addWallbreakEdge(rear, front), "the push is legal while the shared kill is lethal");
        const std::vector<std::int32_t> affected = plan.markFailed(helper);
        expect(!plan.targetIsLethal(WALL_UNIT), "losing the helper's share ends the kill");
        expect(plan.plannedDamageOn(WALL_UNIT) == LETHAL_REMAINDER_HP_STEPS, "the surviving attacker keeps its share");
        expect(plan.edgeCount() == 0, "a co attacker's failure drops the other attacker's wallbreak");
        expect(plan.action(front).state == PlanActionState::Blocked, "the push is blocked for replanning");
        expect(affected.size() == 1 && affected.front() == front, "the blocked push is reported to the caller");
        expect(plan.action(rear).state == PlanActionState::Pending, "the surviving attacker stays pending");
    }

    void testFailurePropagatesThroughADiamondInAscendingOrder()
    {
        TurnPlan plan;
        const std::int32_t top = plan.addAction(moveAction(DIAMOND_TOP_UNIT, DIAMOND_TOP_TILE, DIAMOND_TOP_GOAL, EQUAL_VALUE));
        const std::int32_t left = plan.addAction(moveAction(DIAMOND_LEFT_UNIT, DIAMOND_LEFT_TILE, DIAMOND_LEFT_GOAL, EQUAL_VALUE));
        const std::int32_t right = plan.addAction(
            moveAction(DIAMOND_RIGHT_UNIT, DIAMOND_RIGHT_TILE, DIAMOND_RIGHT_GOAL, EQUAL_VALUE));
        const std::int32_t bottom = plan.addAction(
            moveAction(DIAMOND_BOTTOM_UNIT, DIAMOND_BOTTOM_TILE, DIAMOND_BOTTOM_GOAL, EQUAL_VALUE));
        expect(plan.addEdge(top, left, PlanEdgeKind::Vacate), "the left branch is accepted");
        expect(plan.addEdge(top, right, PlanEdgeKind::Vacate), "the right branch is accepted");
        expect(plan.addEdge(left, bottom, PlanEdgeKind::Vacate), "the left join is accepted");
        expect(plan.addEdge(right, bottom, PlanEdgeKind::Vacate), "the right join is accepted");
        const std::vector<std::int32_t> affected = plan.markFailed(top);
        const std::vector<std::int32_t> expected{left, right, bottom};
        expect(affected.size() == DIAMOND_DEPENDENTS, "the shared dependent is reported once");
        expect(sameSequence(affected, expected), "the affected list is ascending");
        expect(plan.action(bottom).state == PlanActionState::Blocked, "the joined dependent is blocked");
    }

    void testBlockedActionCanBeReplanned()
    {
        TurnPlan plan;
        const std::int32_t loser = plan.addAction(moveAction(REPLAN_UNIT, REPLAN_TILE, CONTESTED_TILE, LOWER_VALUE));
        const std::int32_t winner = plan.addAction(moveAction(SQUATTER_UNIT, HIGH_VALUE_START, CONTESTED_TILE, HIGHER_VALUE));
        expect(plan.claimDestination(loser, CONTESTED_TILE) == ReservationResult::Granted, "the first mover claims the tile");
        expect(plan.resolveDestinationConflict(loser, winner).loser == loser, "the lower value mover loses");
        expect(plan.action(loser).state == PlanActionState::Blocked, "the loser is blocked");
        expect(plan.replan(loser, moveAction(REPLAN_UNIT, REPLAN_TILE, REPLAN_SECOND_GOAL, EQUAL_VALUE)).accepted,
               "a blocked action can be replanned");
        expect(plan.action(loser).state == PlanActionState::Pending, "the replanned action is pending again");
        expect(plan.action(loser).destination == REPLAN_SECOND_GOAL, "the replacement destination is stored");
    }

    void testMarkFailedRefusesACommittedActionAndRepeatsAreIdempotent()
    {
        TurnPlan plan;
        const std::int32_t settled = plan.addAction(moveAction(COMMITTED_UNIT, COMMITTED_TILE, COMMITTED_GOAL, EQUAL_VALUE));
        const std::int32_t dependent = plan.addAction(moveAction(DEPENDENT_UNIT, DEPENDENT_TILE, DEPENDENT_GOAL, EQUAL_VALUE));
        expect(plan.claimDestination(settled, COMMITTED_GOAL) == ReservationResult::Granted, "the committed action claims its tile");
        expect(plan.addEdge(settled, dependent, PlanEdgeKind::Vacate), "the dependency edge is accepted");
        expect(plan.commit(settled), "a pending action commits");
        expect(!plan.commit(settled), "a committed action cannot commit twice");
        expect(plan.markFailed(settled).empty(), "a committed action cannot fail");
        expect(plan.action(settled).state == PlanActionState::Committed, "the committed state survives");
        expect(plan.action(dependent).state == PlanActionState::Pending, "the dependent of a committed action is untouched");
        expect(plan.destinationClaimant(COMMITTED_GOAL) == settled, "the committed action keeps its destination claim");
        expect(!plan.replan(settled, moveAction(COMMITTED_UNIT, COMMITTED_TILE, DEPENDENT_GOAL, HIGHER_VALUE)).accepted,
               "a committed action cannot be replanned");
        expect(plan.action(settled).destination == COMMITTED_GOAL, "the refused replan changes nothing");
        expect(plan.markFailed(dependent).empty(), "a leaf failure reports no dependents");
        expect(plan.action(dependent).state == PlanActionState::Abandoned, "the leaf action is abandoned");
        expect(!plan.commit(dependent), "an abandoned action cannot be committed");
        expect(plan.markFailed(dependent).empty(), "a repeated failure reports nothing new");
        expect(plan.action(dependent).state == PlanActionState::Abandoned, "a repeated failure keeps the abandoned state");
    }

    void testServiceSlotIsExclusivePerPartner()
    {
        TurnPlan plan;
        const std::int32_t service = plan.addAction(moveAction(SERVICE_UNIT, ESCORT_TILE, ESCORT_GOAL, EQUAL_VALUE));
        const std::int32_t rival = plan.addAction(moveAction(RIVAL_SERVICE_UNIT, FRONT_TILE, WALL_TILE, EQUAL_VALUE));
        expect(plan.claimService(service, SERVICE_PARTNER_UNIT) == ReservationResult::Granted, "the first service slot is granted");
        expect(plan.claimService(rival, SERVICE_PARTNER_UNIT) == ReservationResult::Conflict, "a second service action conflicts");
        expect(plan.serviceClaimant(SERVICE_PARTNER_UNIT) == service, "the first service action keeps the slot");
        plan.markFailed(service);
        expect(plan.serviceClaimant(SERVICE_PARTNER_UNIT) == NO_ACTION, "a failed action releases its service slot");
        expect(plan.claimService(rival, SERVICE_PARTNER_UNIT) == ReservationResult::Granted, "the rival can take the free slot");
    }
}

int main()
{
    testReinforcementChainMovesTheHeadFirstSoFollowersAdvance();
    testFocusFireStopsAtLethalAndLaterClaimsAreOverkill();
    testRepeatClaimReplacesTheSameActionsShare();
    testStaleRemainingHpIsReportedNotGranted();
    testMalformedRequestsAreReported();
    testMalformedActionShapesAreRejected();
    testDuplicateUnitActionIsRejected();
    testDestinationOnTheOwnTileNeedsNoEdge();
    testCaptureClaimsAreExclusivePerBuilding();
    testInfantryVacatesThePostBeforeTheIndirectTakesIt();
    testScreenEstablishesBeforeTheThreatenedCapture();
    testStayingOccupantIsAReservationConflictNotAnEdge();
    testStationaryIndirectFiresBeforeNearbyMovesViaCoverageEdge();
    testMovementCycleIsReportedApartFromItsDownstream();
    testWallbreakNeedsLethalAndFailureReleasesTheKill();
    testInsertionOrderDoesNotChangeExecutionOrder();
    testDestinationConflictKeepsTheHigherValueThenTheLowerUnitId();
    testConflictLoserBlocksItsDependents();
    testForcedWinOutranksAHigherUnresolvedValue();
    testAbandonedActionsLeaveTheLivePlan();
    testReplanReleasesTheOldClaims();
    testHpStepBoundsForOverkillAndLethal();
    testReleasingOneContributorKeepsTheOther();
    testWallbreakEdgeDropsWhenTheKillFallsShort();
    testReplanningTheAttackDropsItsWallbreak();
    testCoAttackerRescoreDropsTheWallbreak();
    testCoAttackerFailureDropsTheWallbreak();
    testFailurePropagatesThroughADiamondInAscendingOrder();
    testBlockedActionCanBeReplanned();
    testMarkFailedRefusesACommittedActionAndRepeatsAreIdempotent();
    testServiceSlotIsExclusivePerPartner();
    if (failures == 0)
    {
        return 0;
    }
    return 1;
}
