#include <algorithm>
#include <cstdio>
#include <limits>
#include <type_traits>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/replyprediction.h"

namespace
{
    using namespace Coordinator;

    int checks = 0;
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }

    CandidateBundle candidate(std::int32_t knowledge, TilePoint from,
                              TilePoint to, MilliFunds value)
    {
        CandidateBundle result;
        result.bundle.unitId = knowledge;
        result.bundle.origin = from;
        result.bundle.destination = to;
        result.bundle.path = {from};
        if (from != to)
        {
            result.bundle.path.push_back(to);
        }
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        return result;
    }

    AssignmentInput inputWith(std::int32_t count)
    {
        AssignmentInput input;
        input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"),
                           QStringLiteral("ACTION_CAPTURE")};
        for (std::int32_t slot = 0; slot < count; ++slot)
        {
            const auto tile = TilePoint{slot, 0};
            input.unitLinks.push_back({100 + slot, tile});
            input.actors.push_back({slot, 100 + slot, {candidate(slot, tile, tile, slot + 1)}});
        }
        return input;
    }

    struct Fixture
    {
        TurnPlan plan;
        std::vector<MasterActorSelection> selections;
        std::vector<std::int32_t> order;
    };

    Fixture planFrom(const AssignmentInput & input,
                     std::span<const std::int32_t> choices = {},
                     std::span<const std::int32_t> grants = {})
    {
        Fixture result;
        for (std::size_t slot = 0; slot < input.actors.size(); ++slot)
        {
            const auto & actor = input.actors[slot];
            const auto choice = choices.empty() ? 0 : choices[slot];
            const auto & selected = actor.candidates[static_cast<std::size_t>(choice)];
            const auto action = plannedActionFrom(input.actionIds, input.unitLinks, actor.engineUnitId, selected);
            const auto index = result.plan.addAction(action);
            expect(index != NO_ACTION, "fixture action is accepted");
            expect(claimFor(result.plan, index, selected,
                            grants.empty() ? std::nullopt : std::optional{grants[slot]}) == ReservationResult::Granted,
                   "fixture claims are accepted");
            result.selections.push_back({actor.engineUnitId, index, choice, MasterActorRole::Mutable});
        }
        expect(result.plan.rebuildVacateEdges(planOccupancy(result.plan)).empty(), "fixture has no vacate conflict");
        result.order = result.plan.executionOrder().order;
        return result;
    }

    MasterAssignmentBuildResult extract(const AssignmentInput & input, const Fixture & fixture)
    {
        return buildMasterAssignmentProblem(input, {
            .sessionIdentity = 23,
            .selections = fixture.selections,
            .pIncumbentPlan = &fixture.plan,
            .incumbentExecutionOrder = fixture.order,
        });
    }

    void testOwnershipAndIdentity()
    {
        auto input = inputWith(2);
        auto & source = input.actors[0].candidates.front();
        source.generationIndex = 17;
        source.actorNextShotsAtDestination = {100, 200};
        source.movementCost = 3;
        source.vacatesActiveFriendlyProduction = true;
        const auto fixture = planFrom(input);
        auto extracted = extract(input, fixture);
        expect(extracted.valid(), "simple input extracts");
        if (!extracted.valid())
        {
            return;
        }
        const auto identity = extracted.problem->identity();
        const auto copy = *extracted.problem;
        expect(identity.sameLiveProblem(copy.identity()), "problem copies retain live identity");
        auto independent = extract(input, fixture);
        expect(independent.valid() && !identity.sameLiveProblem(independent.problem->identity()),
               "equal caller session numbers cannot alias independent problems");
        input.actors.clear();
        input.unitLinks.clear();
        expect(copy.actors().size() == 2 && copy.actors()[0].candidates[0].source.generationIndex == 17 &&
                   copy.actors()[0].candidates[0].source.actorNextShotsAtDestination == std::vector<MilliFunds>{100, 200} &&
                   copy.actors()[0].candidates[0].source.movementCost == 3 &&
                   copy.actors()[0].candidates[0].source.vacatesActiveFriendlyProduction,
               "problem owns all original candidate metadata");
        const auto replay = replayMasterAssignment(copy, copy.incumbentChoices());
        expect(replay.complete() && replay.plan.equivalentState(fixture.plan),
               "destroying input cannot change replay");
        static_assert(std::is_same_v<decltype(copy.actors()), std::span<const MasterActor>>);
        const auto expired = independent.problem->identity();
        independent.problem.reset();
        expect(!expired.established() && expired.session() == 0 && !expired.sameLiveProblem(expired),
               "expired problem identities cannot validate certificates");
    }

    void testCandidatesAndFixedActors()
    {
        auto input = inputWith(2);
        input.actors[0].candidates.push_back(candidate(0, {0, 0}, {0, 1}, -321));
        input.actors[1].candidates.push_back(candidate(1, {1, 0}, {1, 1}, 50));
        auto fixture = planFrom(input);
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "fixed and mutable actors extract");
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        expect(problem.actors()[0].role == MasterActorRole::Mutable &&
                   problem.actors()[1].role == MasterActorRole::Fixed &&
                   problem.actors()[0].candidates.size() == 2 &&
                   problem.actors()[0].candidates[1].identity.originalCandidateIndex == 1 &&
                   problem.actors()[0].candidates[1].immediateValue.economicValue == -321,
               "roles, candidate positions and exact values survive extraction");
        const auto moved = replayMasterAssignment(problem, std::vector<std::int32_t>{1, 0});
        expect(moved.complete() && moved.plan.action(1) == fixture.plan.action(1) &&
                   moved.plan.destinationClaimant({1, 0}) == 1,
               "a mutable choice preserves fixed action and reservation");
        expect(!replayMasterAssignment(problem, std::vector<std::int32_t>{NO_CANDIDATE, 0}).valid(),
               "an active mutable actor cannot silently give up");
        expect(!replayMasterAssignment(problem, std::vector<std::int32_t>{0}).valid(),
               "one choice per actor is mandatory");
        expect(replayMasterAssignment(problem, std::vector<std::int32_t>{0, 1}).error ==
                   MasterAssignmentReplayError::FixedChoiceChanged,
               "fixed actors cannot change even to another legal candidate");
    }

    void testResourcesBeyondSixtyFour()
    {
        auto input = inputWith(130);
        input.actors[0].candidates.push_back(candidate(0, {0, 0}, {129, 0}, 0));
        auto fixture = planFrom(input);
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "130-resource input extracts");
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        expect(problem.resources().size() == 130, "resources have no 64-bit ceiling");
        for (const auto index : {62, 63, 64, 65, 127, 128, 129})
        {
            expect(problem.actors()[static_cast<std::size_t>(index)].candidates[0].resources[0].resourceIndex == index,
                   "high resource indices remain distinct");
        }
        expect(problem.incompatible({0, 1}, {129, 0}) &&
                   !problem.incompatible({0, 1}, {128, 0}), "high-index conflict has no alias");
        auto choices = std::vector<std::int32_t>(130, 0);
        choices[0] = 1;
        expect(!replayMasterAssignment(problem, choices).valid(), "TurnPlan rejects high-index conflict");
    }

    void testVacateReversalAndCycles()
    {
        auto input = inputWith(2);
        input.actors[0].candidates = {
            candidate(0, {0, 0}, {-1, 0}, 1),
            candidate(0, {0, 0}, {1, 0}, 1),
        };
        input.actors[1].candidates = {
            candidate(1, {1, 0}, {0, 0}, 1),
            candidate(1, {1, 0}, {2, 0}, 1),
        };
        const auto fixture = planFrom(input);
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "vacate chain extracts");
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto reversed = replayMasterAssignment(problem, std::vector<std::int32_t>{1, 1});
        expect(reversed.complete() && reversed.plan.edgeCount() == 1 &&
                   reversed.plan.edge(0) == PlanEdge{1, 0, PlanEdgeKind::Vacate} &&
                   reversed.ordering.order == std::vector<std::int32_t>{1, 0},
               "a new tuple rebuilds the opposite vacate dependency");
        const auto cycle = replayMasterAssignment(problem, std::vector<std::int32_t>{1, 0});
        expect(cycle.valid() && !cycle.complete() && cycle.ordering.cycleMembers.size() == 2,
               "a representable cycle is not a feasible complete leaf");
    }

    void testDamageAllocationAndCapture()
    {
        auto input = inputWith(3);
        input.unitLinks.push_back({999, {5, 5}});
        for (std::int32_t slot = 0; slot < 2; ++slot)
        {
            input.actors[static_cast<std::size_t>(slot)].candidates[0].bundle.components.push_back(
                fireComponent({.targetUnitId = 3, .targetReplacementCost = 1000,
                               .targetHpSteps = 10, .damageSteps = 8}));
        }
        input.actors[2].candidates[0].bundle.components.push_back(captureComponent({.turnsUntilOwned = 1}));
        auto fixture = planFrom(input);
        expect(fixture.plan.replan(0, fixture.plan.action(0)).accepted, "release original first shot");
        expect(fixture.plan.claimAttack(1, 999, 10, 8).result == ReservationResult::Granted,
               "second shot takes eight after first release");
        expect(claimFor(fixture.plan, 0, input.actors[0].candidates[0]) == ReservationResult::Granted,
               "first shot is reseated with only two damage");
        fixture.selections[0].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "history-dependent damage allocation extracts");
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto replay = replayMasterAssignment(problem, problem.incumbentChoices());
        expect(replay.complete() && replay.plan.equivalentState(fixture.plan) &&
                   replay.plan.action(0).plannedDamageSteps == 2 &&
                   replay.plan.action(1).plannedDamageSteps == 8 &&
                   replay.plan.captureClaimant({2, 0}) == 2,
               "replay preserves 2/8 allocation and capture claims exactly");
        const auto fixedDamage = std::find_if(
            problem.fixedResourceUsage().begin(), problem.fixedResourceUsage().end(),
            [&](const auto & usage)
            {
                return problem.resources()[static_cast<std::size_t>(usage.usage.resourceIndex)].kind ==
                       MasterResourceKind::DamageTarget;
            });
        expect(fixedDamage != problem.fixedResourceUsage().end() && fixedDamage->usage.demand == 2,
               "fixed resource use records granted damage");
        expect(problem.actors()[2].candidates[0].capture->turnsUntilOwned == 1,
               "capture consequence is retained");
    }

    void testCanonicalActorOrder()
    {
        auto input = inputWith(3);
        const auto fixture = planFrom(input);
        const auto first = extract(input, fixture);
        std::reverse(input.actors.begin(), input.actors.end());
        const auto second = extract(input, fixture);
        expect(first.valid() && second.valid(), "reordered inputs extract");
        if (!first.valid() || !second.valid())
        {
            return;
        }
        for (std::size_t actor = 0; actor < 3; ++actor)
        {
            expect(first.problem->actors()[actor].candidates[0].identity ==
                       second.problem->actors()[actor].candidates[0].identity,
                   "canonical actor identity ignores input insertion order");
        }
        expect(std::equal(first.problem->resources().begin(), first.problem->resources().end(),
                          second.problem->resources().begin(), second.problem->resources().end()),
               "canonical resources ignore input insertion order");
    }

    void testInactiveAndLegacySelections()
    {
        auto input = inputWith(2);
        input.actors[1].candidates[0].valuation.ledger.enemyCapital = -100;
        const auto disabled = MaximumValueAssignment::assign(input);
        expect(disabled.selections.empty(), "disabled audit retains no extra selections");
        input.retainSelections = true;
        const auto legacy = MaximumValueAssignment::assign(input);
        expect(legacy.selections.size() == input.actors.size(), "requested selection identity does not require logging");
        expect(disabled.plan.equivalentState(legacy.plan) && disabled.executionOrder == legacy.executionOrder,
               "observational selection retention does not change actions, claims or order");
        const auto extracted = buildMasterAssignmentProblem(input, legacy, 23);
        expect(extracted.valid(), "legacy abandoned action remains representable");
        if (extracted.valid())
        {
            const auto replay = replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices());
            expect(replay.valid() && replay.plan.equivalentState(legacy.plan),
                   "inactive action fields and empty claims survive");
        }
    }

    void testMalformedInputs()
    {
        const auto input = inputWith(3);
        const auto fixture = planFrom(input);
        auto malformed = input;
        malformed.actors[2].knowledgeUnitIndex = 0;
        expect(!extract(malformed, fixture).valid(), "nonadjacent duplicate knowledge identity fails closed");
        malformed = input;
        malformed.actors[1].candidates[0].bundle.unitId = 0;
        expect(!extract(malformed, fixture).valid(), "candidate actor mismatch fails closed");
        malformed = input;
        malformed.actors[0].candidates[0].bundle.destination = {8, 8};
        expect(!extract(malformed, fixture).valid(), "inconsistent moving path fails closed");
        malformed = input;
        malformed.actors[0].candidates[0].bundle.components.push_back(
            fireComponent({.targetUnitId = 30, .targetHpSteps = 10, .damageSteps = 5}));
        expect(!extract(malformed, fixture).valid(), "unlinked fire target fails closed");
        malformed = input;
        malformed.actors[0].candidates[0].valuation.ledger.income = std::numeric_limits<MilliFunds>::max();
        expect(!extract(malformed, fixture).valid(), "immediate arithmetic overflow fails closed");
        auto wrong = fixture;
        wrong.selections[0].candidateIndex = 4;
        expect(!extract(input, wrong).valid(), "missing selected candidate fails closed");
        wrong = fixture;
        wrong.order.clear();
        expect(!extract(input, wrong).valid(), "incorrect execution order fails closed");
        wrong = fixture;
        expect(wrong.plan.claimService(0, 999) == ReservationResult::Granted, "extra claim fixture");
        expect(!extract(input, wrong).valid(), "unrepresented private claim cannot pass equivalence");
    }

    PlannedAction directAction(std::int32_t actor, TilePoint origin, TilePoint destination,
                               MilliFunds value, bool fires = false)
    {
        PlannedAction action;
        action.unitId = actor;
        action.kind = fires ? PlanBundleKind::Fire :
            (origin == destination ? PlanBundleKind::Wait : PlanBundleKind::Move);
        action.actionId = fires ? QStringLiteral("ACTION_FIRE") : QStringLiteral("ACTION_WAIT");
        action.path = {origin};
        if (origin != destination)
        {
            action.path.push_back(destination);
        }
        action.destination = destination;
        action.marginalValue.economicValue = value;
        if (fires)
        {
            action.target = {5, 5};
            action.targetUnitId = 999;
        }
        return action;
    }

    constexpr std::int32_t SHARED_TARGET_HP_STEPS = 10;
    constexpr MilliFunds SHARED_TARGET_COST = 1000;

    void addFire(CandidateBundle & choice, std::int32_t damage, std::int32_t target = 2, std::int32_t counter = 0)
    {
        choice.bundle.components.push_back(fireComponent({
            .targetUnitId = target, .targetReplacementCost = SHARED_TARGET_COST,
            .targetHpSteps = SHARED_TARGET_HP_STEPS, .damageSteps = damage, .counterSteps = counter,
        }));
    }

    void testFixedShareAgainstNewAttacker()
    {
        auto input = inputWith(2);
        input.unitLinks.push_back({999, {5, 5}});
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[1], 8);
        addFire(input.actors[1].candidates[0], 8);
        auto fixture = planFrom(input);
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "fixed-share fixture extracts");
        if (!extracted.valid())
        {
            return;
        }
        TurnPlan expected;
        expected.addAction(directAction(100, {0, 0}, {0, 0}, 1, true));
        expected.addAction(directAction(101, {1, 0}, {1, 0}, 2, true));
        expect(expected.claimDestination(1, {1, 0}) == ReservationResult::Granted &&
                   expected.claimAttack(1, 999, 10, 8).result == ReservationResult::Granted &&
                   expected.claimDestination(0, {0, 0}) == ReservationResult::Granted &&
                   expected.claimAttack(0, 999, 10, 8).result == ReservationResult::Granted,
               "independent construction reserves fixed eight before new mutable two");
        const auto replay = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{1, 0});
        expect(replay.complete() && replay.plan.equivalentState(expected) &&
                   replay.plan.action(1) == fixture.plan.action(1),
               "new attacker cannot steal a fixed attacker's reserved damage");
    }

    void testFixedShareAbsorbsUnclaimedDamage(PlanBundleKind kind = PlanBundleKind::Fire)
    {
        constexpr std::int32_t mutableDamage = 4;
        constexpr std::int32_t fixedDamage = 9;
        constexpr std::int32_t incumbentGrant = 6;
        constexpr std::int32_t targetUnitId = 999;
        constexpr auto extracts = "fixed leftover-damage fixture extracts";
        constexpr auto incumbentExact = "incumbent replay preserves the fixed grant and seated value";
        constexpr auto challengerGrant = "fixed shooter absorbs unclaimed damage and receives its full seated value";
        constexpr auto challengerTotal = "challenger economic total sums the replayed seated values";
        constexpr auto sharedGrant = "a different firing candidate preserves the fixed incumbent grant";
        constexpr auto movingGrant = "fixed moving shooter absorbs unclaimed damage at its full seated value";
        constexpr auto actionKind = "fixed-share fixture uses the requested fire action kind";
        auto input = inputWith(2);
        input.unitLinks.push_back({targetUnitId, {2, 1}});
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[0], mutableDamage);
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        if (kind == PlanBundleKind::MoveAndFire)
        {
            input.actors[1].candidates[0] = candidate(1, {1, 0}, {1, 1}, 2);
        }
        addFire(input.actors[1].candidates[0], fixedDamage);
        auto fixture = planFrom(input);
        expect(fixture.plan.action(1).kind == kind, actionKind);
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto & fixedCandidate = problem.actors()[1].candidates[0].source;
        const auto incumbent = replayMasterAssignment(problem, problem.incumbentChoices());
        expect(incumbent.complete() && incumbent.plan.equivalentState(fixture.plan) &&
                   incumbent.plan.action(1).plannedDamageSteps == incumbentGrant &&
                   incumbent.seatedValues[1] == checkedSeatedCandidateValue(fixedCandidate, incumbentGrant),
               incumbentExact);
        const auto challenger = replayMasterAssignment(problem, std::vector<std::int32_t>{1, 0});
        expect(challenger.complete() && challenger.plan.action(1).plannedDamageSteps == fixedDamage &&
                   challenger.seatedValues[1] == checkedSeatedCandidateValue(fixedCandidate, fixedDamage),
               kind == PlanBundleKind::MoveAndFire ? movingGrant : challengerGrant);
        if (challenger.complete())
        {
            expect(challenger.seatedEconomicTotal == challenger.seatedValues[0] + challenger.seatedValues[1],
                   challengerTotal);
        }
        const auto shared = replayMasterAssignment(problem, std::vector<std::int32_t>{2, 0});
        expect(shared.complete() && shared.plan.action(1).plannedDamageSteps == incumbentGrant &&
                   shared.seatedValues[1] == checkedSeatedCandidateValue(fixedCandidate, incumbentGrant),
               sharedGrant);
    }

    void testFixedZeroDamageOnFullTarget()
    {
        constexpr std::int32_t actorCount = 3;
        constexpr std::int32_t targetUnitId = 999;
        constexpr auto extracts = "zero-damage fixed shot on a full target extracts";
        constexpr auto unchanged = "unrelated move preserves both fixed grants on a full target";
        auto input = inputWith(actorCount);
        input.unitLinks.push_back({targetUnitId, {actorCount, 1}});
        addFire(input.actors[0].candidates[0], 0, actorCount);
        addFire(input.actors[1].candidates[0], SHARED_TARGET_HP_STEPS, actorCount);
        input.actors[2].candidates.push_back(candidate(2, {2, 0}, {2, 1}, 1));
        auto fixture = planFrom(input);
        fixture.selections[0].role = MasterActorRole::Fixed;
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto replay = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 0, 1});
        expect(replay.complete() && replay.plan.action(0).plannedDamageSteps == 0 &&
                   replay.plan.action(1).plannedDamageSteps == SHARED_TARGET_HP_STEPS &&
                   replay.plan.actionReservations(0) == fixture.plan.actionReservations(0) &&
                   replay.plan.actionReservations(1) == fixture.plan.actionReservations(1) &&
                   replay.plan.action(2).destination == TilePoint{2, 1}, unchanged);
    }

    void testFixedZeroGrantAbsorbsUnclaimedDamage()
    {
        constexpr std::int32_t fixedDamage = 9;
        constexpr std::int32_t targetUnitId = 999;
        constexpr auto extracts = "positive-damage fixed shooter with a historical zero grant extracts";
        constexpr auto reservation = "a zero incumbent grant retains its target damage reservation";
        constexpr auto grows = "a fixed zero grant grows and reprices when its partner leaves";
        auto input = inputWith(2);
        input.unitLinks.push_back({targetUnitId, {2, 1}});
        input.actors[1].candidates.push_back(input.actors[1].candidates[0]);
        addFire(input.actors[0].candidates[0], fixedDamage);
        addFire(input.actors[1].candidates[0], SHARED_TARGET_HP_STEPS);
        const std::vector<std::int32_t> grants{0, SHARED_TARGET_HP_STEPS};
        auto fixture = planFrom(input, {}, grants);
        fixture.selections[0].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & actor = extracted.problem->actors()[0];
        expect(actor.incumbentDamageSteps == 0 && actor.incumbentReservations.damage ==
                   std::vector<ActionDamageReservation>{{targetUnitId, SHARED_TARGET_HP_STEPS, 0}}, reservation);
        const auto replay = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 1});
        expect(replay.complete() && replay.plan.action(0).plannedDamageSteps == fixedDamage &&
                   replay.seatedValues[0] == checkedSeatedCandidateValue(actor.candidates[0].source, fixedDamage), grows);
    }

    void testChangedMutableAllocation()
    {
        auto input = inputWith(2);
        input.unitLinks.push_back({999, {5, 5}});
        input.actors[1].candidates.push_back(input.actors[1].candidates[0]);
        addFire(input.actors[0].candidates[0], 8);
        addFire(input.actors[1].candidates[0], 8);
        Fixture fixture;
        fixture.plan.addAction(directAction(100, {0, 0}, {0, 0}, 1, true));
        fixture.plan.addAction(directAction(101, {1, 0}, {1, 0}, 2, true));
        fixture.plan.claimDestination(0, {0, 0});
        fixture.plan.claimDestination(1, {1, 0});
        fixture.plan.claimAttack(1, 999, 10, 8);
        fixture.plan.claimAttack(0, 999, 10, 8);
        fixture.selections = {{100, 0, 0, MasterActorRole::Mutable}, {101, 1, 0, MasterActorRole::Mutable}};
        fixture.order = fixture.plan.executionOrder().order;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "independent historical 2/8 allocation extracts");
        if (!extracted.valid())
        {
            return;
        }
        const auto incumbent = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 0});
        expect(incumbent.complete() && incumbent.plan.equivalentState(fixture.plan),
               "unchanged tuple retains historical allocation");
        TurnPlan expected;
        expected.addAction(directAction(100, {0, 0}, {0, 0}, 1, true));
        expected.addAction(directAction(101, {1, 0}, {1, 0}, 2));
        expected.claimDestination(0, {0, 0});
        expected.claimDestination(1, {1, 0});
        expected.claimAttack(0, 999, 10, 8);
        const auto replay = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 1});
        expect(replay.complete() && replay.plan.equivalentState(expected),
               "changed tuple freshly books eight for the unchanged mutable attacker");
    }

    void testUnassignedAndRevivedActors()
    {
        auto input = inputWith(2);
        input.actors[0].candidates.push_back(candidate(0, {0, 0}, {1, 0}, 1));
        input.actors[1].candidates.push_back(candidate(1, {1, 0}, {2, 0}, 2));
        for (const bool hasAbandonedSlot : {false, true})
        {
            Fixture fixture;
            fixture.plan.addAction(directAction(100, {0, 0}, {0, 0}, 1));
            fixture.plan.claimDestination(0, {0, 0});
            if (hasAbandonedSlot)
            {
                auto abandoned = directAction(101, {1, 0}, {1, 0}, 2);
                abandoned.state = PlanActionState::Abandoned;
                fixture.plan.addAction(abandoned);
            }
            fixture.selections = {{100, 0, 0, MasterActorRole::Mutable},
                                  {101, hasAbandonedSlot ? 1 : NO_ACTION, NO_CANDIDATE, MasterActorRole::Mutable}};
            fixture.order = fixture.plan.executionOrder().order;
            const auto extracted = extract(input, fixture);
            expect(extracted.valid(), "legal unassigned incumbent extracts with or without a slot");
            if (!extracted.valid())
            {
                continue;
            }
            const auto incumbent = replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices());
            expect(incumbent.valid() && !incumbent.complete(), "unassigned incumbent is valid but incomplete");
            TurnPlan expected;
            expected.addAction(directAction(100, {0, 0}, {1, 0}, 1));
            expected.addAction(directAction(101, {1, 0}, {2, 0}, 2));
            expected.claimDestination(0, {1, 0});
            expected.claimDestination(1, {2, 0});
            const std::vector<OccupancyEntry> occupancy{{100, {0, 0}}, {101, {1, 0}}};
            expect(expected.rebuildVacateEdges(occupancy).empty(), "independent revived tuple is feasible");
            const auto replay = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{1, 1});
            expect(replay.complete() && replay.plan.equivalentState(expected) &&
                       replay.ordering.order == std::vector<std::int32_t>{1, 0},
                   "reviving an unassigned actor retains its origin occupancy and fresh vacate order");
        }
    }

    void testNonVacateDependencies()
    {
        auto input = inputWith(2);
        input.unitLinks.push_back({999, {5, 5}});
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[0], 10);
        addFire(input.actors[0].candidates[2], 8);
        input.actors[1].candidates[0] = candidate(1, {1, 0}, {2, 0}, 2);
        auto fixture = planFrom(input);
        expect(fixture.plan.addWallbreakEdge(0, 1), "independent lethal wallbreak prerequisite holds");
        fixture.order = fixture.plan.executionOrder().order;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "supported lethal wallbreak incumbent extracts");
        if (extracted.valid())
        {
            const auto same = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 0});
            expect(same.complete() && same.plan.equivalentState(fixture.plan), "wallbreak incumbent survives replay");
            for (const auto choice : {1, 2})
            {
                TurnPlan independent;
                independent.addAction(directAction(100, {0, 0}, {0, 0}, 1, choice == 2));
                independent.addAction(directAction(101, {1, 0}, {2, 0}, 2));
                independent.claimDestination(0, {0, 0});
                independent.claimDestination(1, {2, 0});
                if (choice == 2)
                {
                    independent.claimAttack(0, 999, 10, 8);
                }
                expect(!independent.addWallbreakEdge(0, 1), "removed or weakened attack cannot open wallbreak");
                expect(!replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{choice, 0}).complete(),
                       "master cannot advertise movement without its lethal prerequisite as complete");
            }
        }
        auto mixed = inputWith(3);
        mixed.actors[0].candidates[0] = candidate(0, {0, 0}, {-1, 0}, 1);
        mixed.actors[1].candidates[0] = candidate(1, {1, 0}, {0, 0}, 2);
        auto mixedFixture = planFrom(mixed);
        expect(mixedFixture.plan.addEdge(2, 1, PlanEdgeKind::Screen), "screen follows a vacate edge in storage");
        mixedFixture.order = mixedFixture.plan.executionOrder().order;
        const auto mixedProblem = extract(mixed, mixedFixture);
        expect(mixedProblem.valid(), "mixed Vacate and non-Vacate edge order round-trips exactly");
    }

    void testInactiveFixedCompletenessAndServiceMetadata()
    {
        auto input = inputWith(1);
        auto fixture = planFrom(input);
        fixture.selections[0].role = MasterActorRole::Fixed;
        fixture.plan.markFailed(0);
        fixture.order = fixture.plan.executionOrder().order;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid() &&
                   !replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices()).complete(),
               "an inactive selected fixed action is not a complete executable plan");
        input.unitLinks.push_back({999, {5, 5}});
        auto service = input.actors[0].candidates[0];
        service.bundle.components.push_back(serviceComponent({
            .partnerUnitId = 1, .partnerHpStepsBefore = 5, .partnerHpStepsAfter = 6,
        }));
        input.actors[0].candidates.push_back(service);
        const auto serviceFixture = planFrom(input);
        const auto serviceProblem = extract(input, serviceFixture);
        expect(serviceProblem.valid(), "unsupported service candidate metadata can be recorded");
        if (serviceProblem.valid())
        {
            const auto & recorded = serviceProblem.problem->actors()[0].candidates[1];
            expect(!recorded.plannable && recorded.resources.size() == 2 &&
                       !replayMasterAssignment(*serviceProblem.problem, std::vector<std::int32_t>{1}).valid(),
                   "service-resource metadata does not claim engine service reconstruction support");
        }
        TurnPlan serviceClaims;
        serviceClaims.addAction(directAction(100, {0, 0}, {0, 0}, 1));
        serviceClaims.addAction(directAction(101, {1, 0}, {1, 0}, 2));
        expect(serviceClaims.claimService(0, 999) == ReservationResult::Granted &&
                   serviceClaims.claimService(1, 999) == ReservationResult::Conflict,
               "native TurnPlan service capacity remains authoritative");
    }

    void testEconomicTotalsAndStockKeys()
    {
        auto input = inputWith(2);
        input.unitLinks.push_back({999, {5, 5}});
        auto & first = input.actors[0].candidates[0];
        auto & second = input.actors[1].candidates[0];
        first.valuation.ledger.enemyCapital = 3900;
        second.valuation.ledger.enemyCapital = 1300;
        first.bundle.components.push_back(fireComponent({
            .targetUnitId = 2, .targetReplacementCost = 10000, .targetHpSteps = 3,
            .damageSteps = 3, .targetBestShotBefore = 900,
        }));
        second.bundle.components.push_back(fireComponent({
            .targetUnitId = 2, .targetReplacementCost = 10000, .targetHpSteps = 3,
            .damageSteps = 1, .targetBestShotBefore = 900, .targetBestShotAfter = 600,
        }));
        Fixture fixture;
        fixture.plan.addAction(directAction(100, {0, 0}, {0, 0}, 3900, true));
        fixture.plan.addAction(directAction(101, {1, 0}, {1, 0}, 1300, true));
        fixture.plan.claimDestination(0, {0, 0});
        fixture.plan.claimDestination(1, {1, 0});
        fixture.plan.claimAttack(1, 999, 3, 1);
        fixture.plan.claimAttack(0, 999, 3, 3);
        fixture.selections = {{100, 0, 0, MasterActorRole::Mutable, 12, 2600},
                              {101, 1, 0, MasterActorRole::Fixed, 2, 1300}};
        fixture.order = fixture.plan.executionOrder().order;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "independently booked exact seated values extract");
        if (extracted.valid())
        {
            const auto & problem = *extracted.problem;
            const auto replay = replayMasterAssignment(problem, problem.incumbentChoices());
            expect(problem.actors()[0].candidates[0].immediateValue.economicValue == 3900 &&
                       problem.actors()[0].incumbentSeatedValue == 2600 &&
                       problem.incumbentEconomicTotal() == 3900 && replay.seatedEconomicTotal == 3900,
                   "static and seated economics remain distinct and exact");
            const ContinuationKey expected{
                CanonicalPlanActionKey::fromAction(2, 1, 0, false),
                CanonicalPlanActionKey::fromAction(12, 0, 0, false),
            };
            expect(problem.incumbentContinuationKey() == expected && replay.continuationKey == expected,
                   "stock keys use canonical rows and exact endpoints");
            auto relabeled = replay.plan;
            auto action = relabeled.action(0);
            action.actionId = QStringLiteral("UNRELATED_ACTION_LABEL");
            relabeled.replan(0, action);
            expect(masterPlanContinuationKey(problem, relabeled) == expected,
                   "engine action labels do not alter stock-key identity");
            action.kind = PlanBundleKind::Capture;
            relabeled.replan(0, action);
            auto captured = expected;
            captured[1] = CanonicalPlanActionKey::fromAction(12, 0, 0, true);
            expect(masterPlanContinuationKey(problem, relabeled) == captured,
                   "the actual capture consequence is retained in the continuation key");
        }
        auto wrong = fixture;
        wrong.selections[0].seatedValue = 2601;
        expect(extract(input, wrong).error == MasterAssignmentProblemError::IncumbentEconomicMismatch,
               "an incorrect cached incumbent value cannot pass the receipt");
        wrong = fixture;
        wrong.selections[1].stockRow = 12;
        expect(extract(input, wrong).error == MasterAssignmentProblemError::InvalidStockIdentity,
               "duplicate stock-row ownership fails closed");
        AssignmentResult cached;
        cached.plan = fixture.plan;
        cached.selections = {
            {.engineUnitId = 100, .actionIndex = 0, .seatedValue = 2600},
            {.engineUnitId = 101, .actionIndex = 1, .seatedValue = 1300},
        };
        expect(checkedAssignmentEconomicTotal(cached) == 3900, "cached incumbent total is checked independently");
        cached.plan.markFailed(1);
        expect(checkedAssignmentEconomicTotal(cached) == 2600, "inactive cached selections contribute nothing");
        cached.selections.push_back(cached.selections[0]);
        expect(!checkedAssignmentEconomicTotal(cached), "duplicate live-action ownership cannot double count");
        cached.selections.clear();
        expect(!checkedAssignmentEconomicTotal(cached), "missing live-action ownership cannot omit a value");
        cached.plan = fixture.plan;
        cached.selections = {
            {.engineUnitId = 100, .actionIndex = 0, .seatedValue = std::numeric_limits<MilliFunds>::max()},
            {.engineUnitId = 101, .actionIndex = 1, .seatedValue = 1},
        };
        expect(!checkedAssignmentEconomicTotal(cached), "cached incumbent total overflow fails closed");
    }

    void testCandidateOrderingAndCompleteTupleEnumeration()
    {
        for (const auto & label : {QStringLiteral("ACTION_WAIT"), QStringLiteral("NUMERICALLY_IDENTICAL_LABEL")})
        {
            auto input = inputWith(2);
            input.actionIds.wait = label;
            input.actors[0].candidates.push_back(candidate(0, {0, 0}, {1, 0}, 1));
            input.actors[0].candidates.push_back(candidate(0, {0, 0}, {2, 0}, 1));
            input.actors[1].candidates.push_back(candidate(1, {1, 0}, {0, 0}, 2));
            input.actors[1].candidates.push_back(candidate(1, {1, 0}, {2, 0}, 2));
            for (auto & actor : input.actors)
            {
                actor.candidates[0].generationIndex = 20;
                actor.candidates[1].generationIndex = 10;
                actor.candidates[2].generationIndex = 30;
            }
            const auto fixture = planFrom(input);
            const auto extracted = extract(input, fixture);
            expect(extracted.valid(), "complete two-actor diagnostic domain extracts under either action label");
            if (!extracted.valid())
            {
                continue;
            }
            expect(extracted.problem->actors()[0].canonicalCandidates == std::vector<std::int32_t>{1, 0, 2},
                   "canonical candidate order uses stable generation identity and retains original indices");
            std::int32_t feasible = 0;
            for (std::int32_t first = 0; first < 3; ++first)
            {
                for (std::int32_t second = 0; second < 3; ++second)
                {
                    TurnPlan independent;
                    const auto destination0 = input.actors[0].candidates[first].bundle.destination;
                    const auto destination1 = input.actors[1].candidates[second].bundle.destination;
                    auto action0 = directAction(100, {0, 0}, destination0, 1);
                    auto action1 = directAction(101, {1, 0}, destination1, 2);
                    action0.actionId = label;
                    action1.actionId = label;
                    independent.addAction(action0);
                    independent.addAction(action1);
                    bool complete = independent.claimDestination(0, destination0) == ReservationResult::Granted;
                    complete = independent.claimDestination(1, destination1) == ReservationResult::Granted && complete;
                    const std::vector<OccupancyEntry> occupancy{{100, {0, 0}}, {101, {1, 0}}};
                    complete = independent.rebuildVacateEdges(occupancy).empty() && complete;
                    const auto order = independent.executionOrder();
                    complete = order.cycleMembers.empty() && order.blockedByCycle.empty() && complete;
                    feasible += complete ? 1 : 0;
                    const auto replay = replayMasterAssignment(*extracted.problem,
                        std::vector<std::int32_t>{first, second});
                    expect(replay.complete() == complete, "enumeration detects both false acceptance and false rejection");
                    if (complete)
                    {
                        expect(replay.plan.equivalentState(independent) && replay.ordering.order == order.order &&
                                   replay.seatedEconomicTotal == 3,
                               "every feasible tuple matches independent actions, claims, order and economics");
                    }
                }
            }
            expect(feasible == 5, "independent two-actor domain has five executable tuples");
            auto reordered = input;
            std::reverse(reordered.actors[0].candidates.begin(), reordered.actors[0].candidates.end());
            auto reorderedFixture = fixture;
            reorderedFixture.selections[0].candidateIndex = 2;
            const auto reorderedProblem = extract(reordered, reorderedFixture);
            expect(reorderedProblem.valid(), "candidate storage reordering preserves incumbent mapping");
            if (reorderedProblem.valid())
            {
                std::vector<std::int32_t> generations;
                const auto & actor = reorderedProblem.problem->actors()[0];
                for (const auto index : actor.canonicalCandidates)
                {
                    generations.push_back(actor.candidates[index].identity.generationIndex);
                }
                expect(generations == std::vector<std::int32_t>{10, 20, 30},
                       "canonical generation order is independent of candidate storage order");
            }
        }
    }

    void testReviewedSnapshotBoundaries()
    {
        const auto input = inputWith(1);
        auto fixture = planFrom(input);
        auto abandoned = directAction(999, {3, 0}, {3, 0}, 0);
        abandoned.state = PlanActionState::Abandoned;
        fixture.plan.addAction(abandoned);
        expect(fixture.plan.addEdge(1, 0, PlanEdgeKind::Screen), "inactive non-member prerequisite is retained");
        fixture.order = fixture.plan.executionOrder().order;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "inactive prerequisite representation remains exact");
        if (extracted.valid())
        {
            const auto replay = replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices());
            expect(replay.valid() && replay.plan.equivalentState(fixture.plan) && !replay.complete(),
                   "an inactive unowned prerequisite cannot become a complete executable plan");
        }
        const auto simple = planFrom(input);
        auto malformed = input;
        malformed.unitLinks[0].tile = INVALID_TILE;
        expect(!extract(malformed, simple).valid(), "an actor cannot silently lose its occupancy");
        malformed = input;
        malformed.unitLinks[0].tile = {8, 8};
        expect(!extract(malformed, simple).valid(), "actor origins must agree with the occupancy snapshot");
        malformed = input;
        malformed.unitLinks.push_back({100, {9, 9}});
        expect(!extract(malformed, simple).valid(), "duplicate engine links cannot define ambiguous occupancy");
        malformed = input;
        malformed.unitLinks.push_back({999, {0, 0}});
        expect(!extract(malformed, simple).valid(), "two occupants cannot silently alias the same tile");
        for (const auto kind : {PlanEdgeKind::Screen, PlanEdgeKind::Coverage, PlanEdgeKind::Service})
        {
            auto domain = inputWith(2);
            domain.actors[0].candidates.push_back(candidate(0, {0, 0}, {2, 0}, 1));
            auto incumbent = planFrom(domain);
            incumbent.plan.addEdge(0, 1, kind);
            incumbent.order = incumbent.plan.executionOrder().order;
            const auto problem = extract(domain, incumbent);
            expect(problem.valid(), "unchanged non-Vacate prerequisite round-trips");
            if (problem.valid())
            {
                const auto changed = replayMasterAssignment(*problem.problem, std::vector<std::int32_t>{1, 0});
                expect(!changed.complete() && changed.error == MasterAssignmentReplayError::UnsupportedDependency,
                       "changed semantic prerequisite is unsupported, not proven infeasible");
            }
        }
    }

    void testCommittedActionsCannotBeReselected()
    {
        auto input = inputWith(2);
        input.actors[0].candidates.push_back(candidate(0, {0, 0}, {0, 1}, 1));
        input.actors[1].candidates.push_back(candidate(1, {1, 0}, {1, 1}, 2));
        auto fixture = planFrom(input);
        expect(fixture.plan.commit(0), "committed incumbent fixture");
        auto native = fixture.plan;
        expect(!native.replan(0, directAction(100, {0, 0}, {0, 1}, 1)).accepted,
               "native TurnPlan cannot change a committed action");
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), "committed incumbent remains representable");
        if (!extracted.valid())
        {
            return;
        }
        expect(!extracted.problem->canChangeActor(0) && extracted.problem->canChangeActor(1),
               "committed state makes a declared mutable actor immutable for this problem");
        const auto otherChanged = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 1});
        expect(otherChanged.complete() && otherChanged.plan.action(0) == fixture.plan.action(0) &&
                   otherChanged.plan.actionReservations(0) == fixture.plan.actionReservations(0),
               "changing another actor cannot reset a committed action to pending");
        expect(!replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{1, 0}).valid(),
               "a mutable role cannot permit reselection of a committed action");
    }

    constexpr std::int32_t REPLY_SHOOTER = 7;
    constexpr double REPLY_BASE = 60;
    constexpr MilliFunds REPLY_COST = 7000;
    // A 60 base at 10 hp on neutral terrain is 60 damage, taking a 10 hp unit to 4.
    constexpr std::int32_t REPLY_STEPS_LOST = 6;
    constexpr std::int32_t NO_STEPS_RECORDED = -1;

    std::shared_ptr<const ReplyFacts> replyFactsFor(const TilePoint & target, std::vector<ReplyShooterFacts> shooters)
    {
        auto tile = std::make_shared<ReplyTileFacts>();
        tile->target = target;
        tile->shooters = std::move(shooters);
        auto facts = std::make_shared<ReplyFacts>();
        facts->tile = tile;
        facts->actorRealHp = UNIT_HP_STEPS;
        facts->shots.assign(tile->shooters.size(), ReplyShotBase{.rawBase = REPLY_BASE, .withAmmo = REPLY_BASE});
        return facts;
    }

    ReplyShooterFacts replyShooterAt(const TilePoint & target)
    {
        return ReplyShooterFacts{
            .shooterIndex = REPLY_SHOOTER, .tile = {target.x, 1}, .dayStartHpSteps = UNIT_HP_STEPS, .distance = 1,
            .live = true, .cost = REPLY_COST,
        };
    }

    void readyForReply(CandidateBundle & choice, std::vector<ReplyShooterFacts> shooters = {})
    {
        choice.actor.hpSteps = UNIT_HP_STEPS;
        choice.actor.replacementCost = REPLY_COST;
        choice.replyFacts = replyFactsFor(choice.bundle.destination, std::move(shooters));
    }

    constexpr std::int32_t REPLY_BOARD_SIZE = 3;

    MasterAssignmentBuildResult extractRecorded(AssignmentInput input, const Fixture & fixture)
    {
        input.replyPrediction = {
            .mode = ReplyPrediction::Recorded,
            .board = std::make_shared<const ReplyBoard>(
                ReplyBoard{.width = REPLY_BOARD_SIZE, .height = REPLY_BOARD_SIZE}),
            .tally = std::make_shared<ReplyPredictionTally>(),
        };
        return extract(input, fixture);
    }

    const PlanReplyActorOutcome* predictedFor(const MasterAssignmentReplayResult & replay, std::int32_t engineUnitId)
    {
        if (!replay.replyPrediction)
        {
            return nullptr;
        }
        for (const PlanReplyActorOutcome & actor : replay.replyPrediction->actors)
        {
            if (actor.engineUnitId == engineUnitId)
            {
                return &actor;
            }
        }
        return nullptr;
    }

    void testRecordedReplyPrediction()
    {
        auto input = inputWith(2);
        for (auto & actor : input.actors)
        {
            readyForReply(actor.candidates[0], {replyShooterAt(actor.candidates[0].bundle.destination)});
        }
        const auto fixture = planFrom(input);
        const auto off = extract(input, fixture);
        const auto board = std::make_shared<const ReplyBoard>(ReplyBoard{.width = 2, .height = 2});
        const auto tally = std::make_shared<ReplyPredictionTally>();
        input.replyPrediction = {.mode = ReplyPrediction::Recorded, .board = board, .tally = tally};
        const auto recorded = extract(input, fixture);
        expect(off.valid() && recorded.valid(), "reply prediction fixtures extract");
        if (!off.valid() || !recorded.valid())
        {
            return;
        }
        expect(tally->replays == 1, "the incumbent verification replay is counted");
        const auto offReplay = replayMasterAssignment(*off.problem, off.problem->incumbentChoices());
        const auto replay = replayMasterAssignment(*recorded.problem, recorded.problem->incumbentChoices());
        expect(offReplay.complete() && !offReplay.replyPrediction, "off records no reply prediction");
        expect(replay.complete() && replay.plan.equivalentState(offReplay.plan) &&
                   replay.ordering.order == offReplay.ordering.order &&
                   replay.seatedValues == offReplay.seatedValues && replay.error == offReplay.error,
               "recorded replay leaves every existing field unchanged");
        std::vector<PlanReplyAction> actions;
        for (const std::int32_t actionIndex : replay.ordering.order)
        {
            const auto & actor = input.actors[static_cast<std::size_t>(actionIndex)];
            actions.push_back({&actor.candidates[0], actor.engineUnitId, actionIndex});
        }
        const PlanReplyPrediction expected = predictPlanReply(actions, *board);
        expect(replay.replyPrediction && expected.complete && expected.actors.size() == 2 &&
                   *replay.replyPrediction == expected,
               "recorded replay stores the shared plan reply prediction");
        expect(tally->replays == 2 && tally->incomplete == 0 && tally->pairs == 2 * expected.pairsExamined,
               "the tally counts every recorded replay");
        const auto lossOf = [&](std::int32_t engineUnitId)
        {
            const PlanReplyActorOutcome* actor = predictedFor(replay, engineUnitId);
            return actor == nullptr ? NO_STEPS_RECORDED : actor->stepsLost;
        };
        expect(replay.replyPrediction && lossOf(input.actors[0].engineUnitId) == REPLY_STEPS_LOST &&
                   lossOf(input.actors[1].engineUnitId) == 0,
               "the one shooter fires once, on the lower unit id at an equal net, for six steps");
        auto missing = input;
        missing.actors[1].candidates[0].replyFacts.reset();
        const auto missingTally = std::make_shared<ReplyPredictionTally>();
        missing.replyPrediction.tally = missingTally;
        const auto unfacted = extract(missing, fixture);
        expect(unfacted.valid() && missingTally->replays == 1 && missingTally->incomplete == 1,
               "a chosen candidate without reply facts leaves the prediction incomplete and is tallied");
        if (unfacted.valid())
        {
            const auto incomplete = replayMasterAssignment(*unfacted.problem, unfacted.problem->incumbentChoices());
            expect(incomplete.complete() && incomplete.replyPrediction && !incomplete.replyPrediction->complete &&
                       incomplete.replyPrediction->actors.empty() && missingTally->incomplete == 2,
                   "the replay stays complete and stores an incomplete prediction");
        }
    }

    constexpr std::int32_t SHOT_TARGET_ENGINE_ID = 999;
    constexpr TilePoint SHOT_TARGET_TILE{5, 5};

    void testReplyPredictionFollowsExecutionOrder()
    {
        constexpr std::int32_t firstShot = 4;
        constexpr std::int32_t secondIsolatedCounter = 8;
        constexpr std::int32_t secondHpAfterScaledCounter = 6;
        constexpr auto extracts = "execution-order prediction fixture extracts";
        constexpr auto ordered = "the higher valued shooter acts first";
        constexpr auto scaled = "the second shooter's counter comes from the target the first shot weakened";
        constexpr auto killed = "a target the first shot kills completes the prediction when nothing fires at it later";
        constexpr auto gone = "a later live shot at a target already destroyed leaves only the prediction incomplete";
        auto input = inputWith(2);
        const auto target = static_cast<std::int32_t>(input.unitLinks.size());
        input.unitLinks.push_back({SHOT_TARGET_ENGINE_ID, SHOT_TARGET_TILE});
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[0], 2, target, secondIsolatedCounter);
        input.actors[1].candidates.push_back(input.actors[1].candidates[0]);
        addFire(input.actors[1].candidates[0], firstShot, target);
        addFire(input.actors[1].candidates[1], SHARED_TARGET_HP_STEPS, target);
        for (auto & actor : input.actors)
        {
            for (auto & choice : actor.candidates)
            {
                readyForReply(choice);
            }
        }
        const auto extracted = extractRecorded(input, planFrom(input));
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto first = input.actors[1].engineUnitId;
        const auto second = input.actors[0].engineUnitId;
        const auto weakened = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 0});
        expect(weakened.complete() && weakened.ordering.order == std::vector<std::int32_t>{1, 0}, ordered);
        const PlanReplyActorOutcome* secondShooter = predictedFor(weakened, second);
        const PlanReplyActorOutcome* firstShooter = predictedFor(weakened, first);
        expect(weakened.replyPrediction && weakened.replyPrediction->complete && secondShooter != nullptr &&
                   firstShooter != nullptr && secondShooter->hpStepsBefore == secondHpAfterScaledCounter &&
                   secondShooter->hpBefore == secondHpAfterScaledCounter &&
                   firstShooter->hpStepsBefore == UNIT_HP_STEPS,
               scaled);
        const auto alone = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{1, 1});
        const PlanReplyActorOutcome* waiting = predictedFor(alone, second);
        expect(alone.complete() && alone.replyPrediction && alone.replyPrediction->complete && waiting != nullptr &&
                   waiting->hpStepsBefore == UNIT_HP_STEPS,
               killed);
        const auto late = replayMasterAssignment(*extracted.problem, std::vector<std::int32_t>{0, 1});
        expect(late.complete() && late.ordering.order == std::vector<std::int32_t>{1, 0} && late.replyPrediction &&
                   !late.replyPrediction->complete && late.replyPrediction->actors.empty(),
               gone);
    }

    void testReplyPredictionFreesOwnActionDeaths()
    {
        constexpr TilePoint doomedTile{0, 1};
        constexpr auto extracts = "own-action death prediction fixture extracts";
        constexpr auto notTarget = "a unit its own attack kills is no reply target and loses nothing to the reply";
        constexpr auto freed = "the tile the dead unit would have held lets the shooter hit the other unit";
        auto input = inputWith(2);
        const auto target = static_cast<std::int32_t>(input.unitLinks.size());
        input.unitLinks.push_back({SHOT_TARGET_ENGINE_ID, SHOT_TARGET_TILE});
        input.actors[0].candidates[0] = candidate(0, {0, 0}, doomedTile, 1);
        addFire(input.actors[0].candidates[0], 2, target, UNIT_HP_STEPS);
        ReplyShooterFacts walker = replyShooterAt(doomedTile);
        walker.direct = true;
        walker.firingTiles = {doomedTile};
        for (auto & actor : input.actors)
        {
            readyForReply(actor.candidates[0], {walker});
        }
        const auto extracted = extractRecorded(input, planFrom(input));
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto replay = replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices());
        const PlanReplyActorOutcome* doomed = predictedFor(replay, input.actors[0].engineUnitId);
        const PlanReplyActorOutcome* other = predictedFor(replay, input.actors[1].engineUnitId);
        expect(replay.complete() && replay.replyPrediction && replay.replyPrediction->complete && doomed != nullptr &&
                   doomed->hpStepsBefore == 0 && doomed->stepsLost == 0 && !doomed->destroyed,
               notTarget);
        expect(other != nullptr && other->hpStepsBefore == UNIT_HP_STEPS && other->stepsLost == REPLY_STEPS_LOST,
               freed);
    }

    void testReplyPredictionBooksRealDamage()
    {
        constexpr std::int32_t realShot = 7;
        constexpr std::int32_t grantedShare = 4;
        constexpr std::int32_t secondRealShot = 5;
        constexpr std::int32_t secondGrantedShare = 3;
        constexpr std::int32_t lossAtThreeHp = 1;
        constexpr std::int32_t actorCount = 3;
        constexpr std::size_t exposedSlot = 2;
        constexpr auto extracts = "real-damage prediction fixtures extract";
        constexpr auto real = "the enemy replies with the hp the real shot leaves, not the granted share";
        constexpr auto clipped = "real shots clipped at remaining hp kill a target the granted split leaves alive";
        auto input = inputWith(actorCount);
        const auto target = static_cast<std::int32_t>(input.unitLinks.size());
        input.unitLinks.push_back({SHOT_TARGET_ENGINE_ID, SHOT_TARGET_TILE});
        addFire(input.actors[0].candidates[0], realShot, target);
        input.actors[1].candidates.push_back(input.actors[1].candidates[0]);
        addFire(input.actors[1].candidates[1], secondRealShot, target);
        const ReplyShooterFacts enemy{
            .shooterIndex = target, .tile = SHOT_TARGET_TILE, .dayStartHpSteps = UNIT_HP_STEPS, .live = true,
            .cost = REPLY_COST,
        };
        for (std::size_t slot = 0; slot < input.actors.size(); ++slot)
        {
            for (auto & choice : input.actors[slot].candidates)
            {
                readyForReply(choice, slot == exposedSlot ? std::vector<ReplyShooterFacts>{enemy}
                                                          : std::vector<ReplyShooterFacts>{});
            }
        }
        const std::vector<std::int32_t> oneShot{0, 0, 0};
        const std::vector<std::int32_t> oneShotGrants{grantedShare, 0, 0};
        const std::vector<std::int32_t> twoShots{0, 1, 0};
        const std::vector<std::int32_t> twoShotGrants{grantedShare, secondGrantedShare, 0};
        const auto single = extractRecorded(input, planFrom(input, oneShot, oneShotGrants));
        const auto pair = extractRecorded(input, planFrom(input, twoShots, twoShotGrants));
        expect(single.valid() && pair.valid(), extracts);
        if (!single.valid() || !pair.valid())
        {
            return;
        }
        const auto exposed = input.actors[exposedSlot].engineUnitId;
        const auto singleReplay = replayMasterAssignment(*single.problem, single.problem->incumbentChoices());
        const PlanReplyActorOutcome* singleLoss = predictedFor(singleReplay, exposed);
        expect(singleReplay.complete() && singleReplay.plan.action(0).plannedDamageSteps == grantedShare &&
                   singleReplay.replyPrediction && singleReplay.replyPrediction->complete && singleLoss != nullptr &&
                   singleLoss->stepsLost == lossAtThreeHp,
               real);
        const auto pairReplay = replayMasterAssignment(*pair.problem, pair.problem->incumbentChoices());
        const PlanReplyActorOutcome* pairLoss = predictedFor(pairReplay, exposed);
        expect(pairReplay.complete() && pairReplay.plan.action(1).plannedDamageSteps == secondGrantedShare &&
                   pairReplay.ordering.order == std::vector<std::int32_t>{2, 1, 0} && pairReplay.replyPrediction &&
                   pairReplay.replyPrediction->complete && pairLoss != nullptr && pairLoss->stepsLost == 0,
               clipped);
    }
}

int main()
{
    testOwnershipAndIdentity();
    testCandidatesAndFixedActors();
    testResourcesBeyondSixtyFour();
    testVacateReversalAndCycles();
    testDamageAllocationAndCapture();
    testCanonicalActorOrder();
    testInactiveAndLegacySelections();
    testMalformedInputs();
    testFixedShareAgainstNewAttacker();
    testFixedShareAbsorbsUnclaimedDamage();
    testFixedShareAbsorbsUnclaimedDamage(PlanBundleKind::MoveAndFire);
    testFixedZeroDamageOnFullTarget();
    testFixedZeroGrantAbsorbsUnclaimedDamage();
    testChangedMutableAllocation();
    testUnassignedAndRevivedActors();
    testNonVacateDependencies();
    testInactiveFixedCompletenessAndServiceMetadata();
    testEconomicTotalsAndStockKeys();
    testCandidateOrderingAndCompleteTupleEnumeration();
    testReviewedSnapshotBoundaries();
    testCommittedActionsCannotBeReselected();
    testRecordedReplyPrediction();
    testReplyPredictionFollowsExecutionOrder();
    testReplyPredictionFreesOwnActionDeaths();
    testReplyPredictionBooksRealDamage();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
