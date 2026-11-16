#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/replypassevaluation.h"
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

    MasterAssignmentBuildResult extractRecorded(
        AssignmentInput input, const Fixture & fixture,
        std::shared_ptr<ReplyPredictionTally> tally = std::make_shared<ReplyPredictionTally>())
    {
        input.replyPrediction = {
            .mode = ReplyPrediction::Recorded,
            .board = std::make_shared<const ReplyBoard>(
                ReplyBoard{.width = REPLY_BOARD_SIZE, .height = REPLY_BOARD_SIZE}),
            .tally = std::move(tally),
        };
        return extract(input, fixture);
    }

    void exposeEachToOneShooter(AssignmentInput & input)
    {
        for (auto & actor : input.actors)
        {
            readyForReply(actor.candidates[0], {replyShooterAt(actor.candidates[0].bundle.destination)});
        }
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
        exposeEachToOneShooter(input);
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
    constexpr std::int32_t WEAKENING_SHOT = 4;
    constexpr std::int32_t WEAKENED_SHOT = 2;
    constexpr std::int32_t WEAKENED_ISOLATED_COUNTER = 8;

    std::int32_t addShotTarget(AssignmentInput & input)
    {
        input.unitLinks.push_back({SHOT_TARGET_ENGINE_ID, SHOT_TARGET_TILE});
        return static_cast<std::int32_t>(input.unitLinks.size()) - 1;
    }

    // Actor 1 fires first; actor 0 then fires on the target actor 1 weakened, or on a target actor 1 killed.
    AssignmentInput weakenedTargetInput(std::int32_t isolatedCounter)
    {
        auto input = inputWith(2);
        const auto target = addShotTarget(input);
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[0], WEAKENED_SHOT, target, isolatedCounter);
        input.actors[1].candidates.push_back(input.actors[1].candidates[0]);
        addFire(input.actors[1].candidates[0], WEAKENING_SHOT, target);
        addFire(input.actors[1].candidates[1], SHARED_TARGET_HP_STEPS, target);
        for (auto & actor : input.actors)
        {
            for (auto & choice : actor.candidates)
            {
                readyForReply(choice);
            }
        }
        return input;
    }

    constexpr TilePoint DOOMED_TILE{0, 1};

    // Actor 0 dies to its own attack's counter; a direct shooter can only fire from the tile it would hold.
    AssignmentInput ownDeathInput()
    {
        auto input = inputWith(2);
        const auto target = addShotTarget(input);
        input.actors[0].candidates[0] = candidate(0, {0, 0}, DOOMED_TILE, 1);
        addFire(input.actors[0].candidates[0], 2, target, UNIT_HP_STEPS);
        ReplyShooterFacts walker = replyShooterAt(DOOMED_TILE);
        walker.direct = true;
        walker.firingTiles = {DOOMED_TILE};
        for (auto & actor : input.actors)
        {
            readyForReply(actor.candidates[0], {walker});
        }
        return input;
    }

    void testReplyPredictionFollowsExecutionOrder()
    {
        constexpr std::int32_t secondHpAfterScaledCounter = 6;
        constexpr auto extracts = "execution-order prediction fixture extracts";
        constexpr auto ordered = "the higher valued shooter acts first";
        constexpr auto scaled = "the second shooter's counter comes from the target the first shot weakened";
        constexpr auto killed = "a target the first shot kills completes the prediction when nothing fires at it later";
        constexpr auto gone = "a later live shot at a target already destroyed leaves only the prediction incomplete";
        const auto input = weakenedTargetInput(WEAKENED_ISOLATED_COUNTER);
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
        constexpr auto extracts = "own-action death prediction fixture extracts";
        constexpr auto notTarget = "a unit its own attack kills is no reply target and loses nothing to the reply";
        constexpr auto freed = "the tile the dead unit would have held lets the shooter hit the other unit";
        const auto input = ownDeathInput();
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

    const ReplyBoard EVALUATION_BOARD{.width = REPLY_BOARD_SIZE, .height = REPLY_BOARD_SIZE};
    constexpr MilliFunds DESTINATION_SHOT = 1000;
    constexpr MilliFunds ODD_ACTOR_COST = 7001;
    constexpr MilliFunds ROUNDING_ACTOR_COST = 7005;
    constexpr double SPLIT_SHOT_BASE = 30;
    constexpr double SPLIT_COUNTER_BASE = 30;
    constexpr std::int32_t SPLIT_SHOT_STEPS = 3;
    constexpr std::int32_t SECOND_SHOOTER = 8;
    constexpr std::int32_t CAPTURER_ROW = 3;
    constexpr std::int32_t DOOMED_ROW = 4;

    const ReplyPassActor* evaluatedFor(const ReplyPassEvaluation & evaluation, std::int32_t actorSlot)
    {
        const auto found = std::find_if(evaluation.actors.begin(), evaluation.actors.end(),
                                        [&](const ReplyPassActor & actor) { return actor.actorSlot == actorSlot; });
        return found == evaluation.actors.end() ? nullptr : &*found;
    }

    MilliFunds evaluatedTotal(const ReplyPassEvaluation & evaluation)
    {
        MilliFunds total = 0;
        for (const ReplyPassActor & actor : evaluation.actors)
        {
            const ReplyPassTerms & terms = actor.terms;
            total += terms.actingSeated + terms.ownCounterCorrection - terms.replyCapitalLoss +
                     terms.replyCounterCapital + terms.nextShotAfterReply;
        }
        return total;
    }

    bool sameEvaluation(const ReplyPassEvaluation & left, const ReplyPassEvaluation & right)
    {
        return left.reason == right.reason && left.actorSlot == right.actorSlot && left.actors == right.actors &&
               left.shots == right.shots && left.afterOwnActions == right.afterOwnActions &&
               left.afterReply == right.afterReply && left.exactNonStock == right.exactNonStock &&
               left.pairsExamined == right.pairsExamined && left.replay.ordering.order == right.replay.ordering.order &&
               left.replay.actionIndices == right.replay.actionIndices &&
               left.replay.seatedValues == right.replay.seatedValues &&
               left.replay.replyPrediction.has_value() == right.replay.replyPrediction.has_value();
    }

    bool agreesWithPrediction(const ReplyPassEvaluation & evaluation, const MasterAssignmentReplayResult & replay)
    {
        if (!evaluation.valued() || !replay.replyPrediction || !replay.replyPrediction->complete ||
            replay.replyPrediction->actors.size() != evaluation.actors.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < evaluation.actors.size(); ++index)
        {
            const ReplyPassActor & actor = evaluation.actors[index];
            const PlanReplyActorOutcome & predicted = replay.replyPrediction->actors[index];
            if (predicted.engineUnitId != actor.engineUnitId || predicted.hpStepsBefore != actor.stepsAfterOwnActions ||
                predicted.hpStepsBefore - predicted.stepsLost != actor.stepsAfterReply)
            {
                return false;
            }
        }
        return true;
    }

    bool sameTally(const ReplyPredictionTally & left, const ReplyPredictionTally & right)
    {
        return left.replays == right.replays && left.incomplete == right.incomplete && left.pairs == right.pairs &&
               left.nanos == right.nanos;
    }

    void testReplyPassAgreesWithPrediction()
    {
        constexpr auto extracts = "reply pass agreement fixtures extract";
        constexpr auto agrees = "the pass measures the steps the recorded prediction reports, actor by actor";
        constexpr auto untouched = "evaluating a plan leaves the problem's shared reply tally untouched";
        auto exposed = inputWith(2);
        exposeEachToOneShooter(exposed);
        for (const AssignmentInput & input : {exposed, weakenedTargetInput(WEAKENED_ISOLATED_COUNTER), ownDeathInput()})
        {
            const auto tally = std::make_shared<ReplyPredictionTally>();
            const auto extracted = extractRecorded(input, planFrom(input), tally);
            expect(extracted.valid(), extracts);
            if (!extracted.valid())
            {
                continue;
            }
            const auto & problem = *extracted.problem;
            const auto replay = replayMasterAssignment(problem, problem.incumbentChoices());
            const ReplyPredictionTally counted = *tally;
            const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
            expect(sameTally(counted, *tally), untouched);
            expect(agreesWithPrediction(evaluation, replay), agrees);
        }
    }

    AssignmentInput weakenedOddCostInput()
    {
        auto input = weakenedTargetInput(WEAKENED_ISOLATED_COUNTER);
        auto & weakened = input.actors[0].candidates[0];
        weakened.actorNextShotsAtDestination = {DESTINATION_SHOT};
        weakened.actor.replacementCost = ODD_ACTOR_COST;
        weakened.valuation = valueBundle(weakened.bundle, weakened.actor, PositionFacts{}, ValuationContext{});
        return input;
    }

    void testReplyPassWeakenedTargetTerms()
    {
        // The isolated target keeps 8 hp; after both shots it keeps 4, so the counter scales to 8 * 4 / 8.
        constexpr std::int32_t measuredCounter = 4;
        constexpr std::int32_t stepsAfterCounter = UNIT_HP_STEPS - measuredCounter;
        constexpr std::int32_t isolatedHp = UNIT_HP_STEPS - WEAKENED_ISOLATED_COUNTER;
        constexpr auto extracts = "weakened-target pass fixture extracts";
        constexpr auto measured = "the weakened shot takes the measured counter and nothing replies";
        constexpr auto acting = "the acting term is the acting seated value at the granted damage";
        constexpr auto corrected = "a smaller measured counter books a positive correction, one booking per side";
        constexpr auto scaled = "the next shot scales up from the isolated hp to the hp the unit really keeps";
        constexpr auto summed = "the exact non-stock value is the sum of every actor's five terms";
        const auto input = weakenedOddCostInput();
        const auto extracted = extract(input, planFrom(input));
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const ReplyPassActor* actor = evaluatedFor(evaluation, 0);
        expect(evaluation.valued() && actor != nullptr && actor->stepsAfterOwnActions == stepsAfterCounter &&
                   actor->stepsAfterReply == stepsAfterCounter && actor->terms.replyCapitalLoss == 0 &&
                   actor->terms.replyCounterCapital == 0,
               measured);
        if (actor == nullptr)
        {
            return;
        }
        const auto & source = problem.actors()[0].candidates[0].source;
        expect(actor->terms.actingSeated == checkedActingSeatedCandidateValue(source, actor->grantedDamage), acting);
        const MilliFunds correction =
            bookDamage(ODD_ACTOR_COST, WEAKENED_ISOLATED_COUNTER) - bookDamage(ODD_ACTOR_COST, measuredCounter);
        expect(correction > 0 && actor->terms.ownCounterCorrection == correction, corrected);
        expect(actor->terms.nextShotAfterReply == DESTINATION_SHOT * stepsAfterCounter / isolatedHp &&
                   actor->terms.nextShotAfterReply > DESTINATION_SHOT,
               scaled);
        expect(evaluation.exactNonStock == evaluatedTotal(evaluation), summed);
    }

    void testReplyPassActorShotTwice()
    {
        constexpr std::int32_t finalSteps = UNIT_HP_STEPS - 2 * SPLIT_SHOT_STEPS;
        constexpr MilliFunds oddShot = DESTINATION_SHOT + 1;
        constexpr auto extracts = "twice-shot pass fixture extracts";
        constexpr auto shotTwice = "two enemies each take three steps off the one actor, the lower index first";
        constexpr auto counters = "each shot keeps the counter capital its selection priced, summed per unit";
        constexpr auto booking = "the reply loss is one booking over both shots, not the sum of two roundings";
        constexpr auto identity = "the next shot after the reply is the best shot less the reply next-shot loss";
        constexpr auto summed = "the twice-shot exact non-stock value is the sum of the five terms";
        constexpr auto literal = "the twice-shot terms and total match the values computed by hand";
        auto input = inputWith(1);
        auto & choice = input.actors[0].candidates[0];
        ReplyShooterFacts first = replyShooterAt(choice.bundle.destination);
        first.direct = true;
        first.firingTiles = {{1, 0}};
        ReplyShooterFacts second = first;
        second.shooterIndex = SECOND_SHOOTER;
        second.firingTiles = {{0, 1}};
        readyForReply(choice, {first, second});
        choice.actor.replacementCost = ROUNDING_ACTOR_COST;
        choice.actorNextShotsAtDestination = {DESTINATION_SHOT};
        auto facts = std::make_shared<ReplyFacts>(*choice.replyFacts);
        facts->shots.assign(facts->shots.size(), ReplyShotBase{SPLIT_SHOT_BASE, SPLIT_SHOT_BASE, SPLIT_COUNTER_BASE});
        choice.replyFacts = facts;
        const auto extracted = extract(input, planFrom(input));
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const ReplyPassActor* actor = evaluatedFor(evaluation, 0);
        expect(evaluation.valued() && actor != nullptr && actor->stepsAfterReply == finalSteps &&
                   evaluation.shots.size() == 2 && evaluation.shots[0].shooterIndex == REPLY_SHOOTER &&
                   evaluation.shots[1].shooterIndex == SECOND_SHOOTER,
               shotTwice);
        if (actor == nullptr)
        {
            return;
        }
        MilliFunds counterTotal = 0;
        bool priced = true;
        for (const ReplyPassShot & shot : evaluation.shots)
        {
            priced = priced && shot.target == 0 && shot.hpStepsBefore - shot.hpStepsAfter == SPLIT_SHOT_STEPS &&
                     shot.counterCapital > 0 &&
                     shot.counterCapital ==
                         counterCost(SPLIT_COUNTER_BASE, shot.hpStepsAfter, UNIT_HP_STEPS, REPLY_COST);
            counterTotal += shot.counterCapital;
        }
        expect(priced && actor->terms.replyCounterCapital == counterTotal, counters);
        expect(actor->terms.replyCapitalLoss == bookDamage(ROUNDING_ACTOR_COST, 2 * SPLIT_SHOT_STEPS) &&
                   actor->terms.replyCapitalLoss != 2 * bookDamage(ROUNDING_ACTOR_COST, SPLIT_SHOT_STEPS),
               booking);
        bool identical =
            actor->terms.nextShotAfterReply ==
            DESTINATION_SHOT - replyNextShotLoss(DESTINATION_SHOT, UNIT_HP_STEPS, finalSteps);
        for (std::int32_t basis = 1; basis <= UNIT_HP_STEPS; ++basis)
        {
            for (std::int32_t kept = 1; kept <= basis; ++kept)
            {
                identical = identical && ReplyPassEvaluationDetail::scaledNextShot(oddShot, kept, basis) ==
                                             oddShot - replyNextShotLoss(oddShot, basis, kept);
            }
        }
        expect(identical, identity);
        expect(evaluation.exactNonStock == evaluatedTotal(evaluation), summed);
        constexpr ReplyPassTerms byHand{
            .actingSeated = 1, .replyCapitalLoss = 4203, .replyCounterCapital = 2100, .nextShotAfterReply = 400};
        constexpr MilliFunds totalByHand = -1702;
        expect(actor->terms == byHand && evaluation.exactNonStock == totalByHand, literal);
    }

    void testReplyPassReplyKillsCapturer()
    {
        constexpr auto extracts = "reply-kill pass fixture extracts";
        constexpr auto killed = "two shots kill the capturer in the reply";
        constexpr auto lost = "a unit the reply kills has no next shot and books its whole capital once";
        constexpr auto keyed = "the reply-killed capturer is a destroyed row that keeps its capture flag";
        auto input = inputWith(1);
        auto & choice = input.actors[0].candidates[0];
        choice.bundle.components.push_back(captureComponent({.turnsUntilOwned = 1}));
        ReplyShooterFacts second = replyShooterAt(choice.bundle.destination);
        second.shooterIndex = SECOND_SHOOTER;
        readyForReply(choice, {replyShooterAt(choice.bundle.destination), second});
        choice.actorNextShotsAtDestination = {DESTINATION_SHOT};
        auto fixture = planFrom(input);
        fixture.selections[0].stockRow = CAPTURER_ROW;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const ReplyPassActor* actor = evaluatedFor(evaluation, 0);
        expect(evaluation.valued() && actor != nullptr && actor->stepsAfterReply == 0 &&
                   actor->destroyedBy == StockDeathCause::Reply && evaluation.shots.size() == 2,
               killed);
        if (actor == nullptr)
        {
            return;
        }
        expect(actor->terms.nextShotAfterReply == 0 &&
                   actor->terms.replyCapitalLoss == bookDamage(REPLY_COST, UNIT_HP_STEPS),
               lost);
        const TilePoint tile = choice.bundle.destination;
        const ContinuationKey afterOwn{
            *CanonicalPlanActionKey::checkedFromAction(CAPTURER_ROW, tile.x, tile.y, true, 0, false)};
        const ContinuationKey afterReply{
            *CanonicalPlanActionKey::checkedFromAction(CAPTURER_ROW, tile.x, tile.y, true, UNIT_HP_STEPS, true)};
        expect(evaluation.afterOwnActions == afterOwn && evaluation.afterReply == afterReply, keyed);
    }

    void testReplyPassOwnActionDeath()
    {
        constexpr auto extracts = "own-death pass fixture extracts";
        constexpr auto dead = "the unit its own attack kills is destroyed by its own action";
        constexpr auto unreplied = "a unit dead before the reply has no reply terms and no correction";
        constexpr auto keyed = "the self-destroyed unit is a destroyed row in both keys";
        const auto input = ownDeathInput();
        auto fixture = planFrom(input);
        fixture.selections[0].stockRow = DOOMED_ROW;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const ReplyPassActor* actor = evaluatedFor(evaluation, 0);
        expect(evaluation.valued() && actor != nullptr && actor->stepsAfterOwnActions == 0 &&
                   actor->destroyedBy == StockDeathCause::OwnAction,
               dead);
        if (actor == nullptr)
        {
            return;
        }
        const auto & source = problem.actors()[0].candidates[0].source;
        expect(actor->terms == ReplyPassTerms{.actingSeated = *checkedActingSeatedCandidateValue(
                                                  source, actor->grantedDamage)},
               unreplied);
        const ContinuationKey doomed{*CanonicalPlanActionKey::checkedFromAction(
            DOOMED_ROW, DOOMED_TILE.x, DOOMED_TILE.y, false, UNIT_HP_STEPS, true)};
        expect(evaluation.afterOwnActions == doomed && evaluation.afterReply == doomed, keyed);
    }

    void testReplyPassCorrectionRebuildsCandidate()
    {
        constexpr auto extracts = "rebuild-equivalence fixture extracts";
        constexpr auto booked = "the isolated valuation booked the isolated counter with the ledger's own booking";
        constexpr auto rebuilt = "acting value plus correction equals the candidate rebuilt with the measured counter";
        const auto input = weakenedOddCostInput();
        const auto extracted = extract(input, planFrom(input));
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const ReplyPassActor* actor = evaluatedFor(evaluation, 0);
        const auto & source = problem.actors()[0].candidates[0].source;
        expect(source.valuation.ledger.friendlyCapital == -bookDamage(ODD_ACTOR_COST, WEAKENED_ISOLATED_COUNTER),
               booked);
        if (!evaluation.valued() || actor == nullptr)
        {
            expect(false, rebuilt);
            return;
        }
        CandidateBundle measured = source;
        measured.bundle.components.front().fire.counterSteps = actor->dayStartSteps - actor->stepsAfterOwnActions;
        measured.valuation = valueBundle(measured.bundle, measured.actor, PositionFacts{}, ValuationContext{});
        const auto isolatedValue = checkedActingSeatedCandidateValue(source, actor->grantedDamage);
        const auto measuredValue = checkedActingSeatedCandidateValue(measured, actor->grantedDamage);
        expect(isolatedValue && measuredValue && *isolatedValue + actor->terms.ownCounterCorrection == *measuredValue,
               rebuilt);
    }

    ReplyPassEvaluation evaluatedChoices(const AssignmentInput & input, std::span<const std::int32_t> choices)
    {
        const auto extracted = extract(input, planFrom(input));
        expect(extracted.valid(), "no-value fixture extracts");
        if (!extracted.valid())
        {
            return ReplyPassEvaluation{};
        }
        return evaluateReplyPassPlan(*extracted.problem, choices, EVALUATION_BOARD);
    }

    bool refusedFor(const ReplyPassEvaluation & evaluation, ReplyPassNoValue reason, std::int32_t actorSlot)
    {
        return evaluation.reason == reason && evaluation.actorSlot == actorSlot;
    }

    constexpr std::int32_t CHAIN_ACTORS = 3;
    constexpr TilePoint CHAIN_EXIT{CHAIN_ACTORS, 0};
    constexpr std::int32_t CHAIN_LEADER_UNIT = 101;

    AssignmentInput chainInput(bool followers)
    {
        auto input = inputWith(CHAIN_ACTORS);
        input.actors[2].candidates.push_back(candidate(2, {2, 0}, CHAIN_EXIT, 1));
        if (followers)
        {
            const std::array<TilePoint, CHAIN_ACTORS> followTo{TilePoint{1, 0}, TilePoint{2, 0}, TilePoint{1, 0}};
            for (std::size_t slot = 0; slot < input.actors.size(); ++slot)
            {
                auto & actor = input.actors[slot];
                actor.todayCandidates = actor.candidates.size();
                actor.candidates.push_back(
                    candidate(actor.knowledgeUnitIndex, input.unitLinks[slot].tile, followTo[slot], 1));
            }
        }
        for (auto & actor : input.actors)
        {
            for (std::size_t index = 0; index < actor.candidates.size(); ++index)
            {
                actor.candidates[index].generationIndex = static_cast<std::int32_t>(index);
            }
        }
        return input;
    }

    void testFollowCandidatesTrailTodaysCandidates()
    {
        const auto today = chainInput(false);
        const auto followed = chainInput(true);
        const auto fixture = planFrom(today);
        const auto base = extract(today, fixture);
        const auto extended = extract(followed, fixture);
        expect(base.valid() && extended.valid(), "follow candidates extract beside today's");
        if (!base.valid() || !extended.valid())
        {
            return;
        }
        const auto & problem = *extended.problem;
        bool prefixKept = std::ranges::equal(base.problem->incumbentChoices(), problem.incumbentChoices());
        for (std::size_t slot = 0; slot < problem.actors().size(); ++slot)
        {
            const auto & before = base.problem->actors()[slot];
            const auto & after = problem.actors()[slot];
            const auto todayCount = before.candidates.size();
            prefixKept = prefixKept && after.candidates.size() == todayCount + 1 && after.candidates.back().follows &&
                std::equal(before.canonicalCandidates.begin(), before.canonicalCandidates.end(),
                           after.canonicalCandidates.begin()) &&
                after.canonicalCandidates.back() == static_cast<std::int32_t>(todayCount);
            for (std::size_t index = 0; index < todayCount; ++index)
            {
                prefixKept = prefixKept && !after.candidates[index].follows &&
                    after.candidates[index].identity == before.candidates[index].identity &&
                    after.candidates[index].dependencies == before.candidates[index].dependencies;
            }
        }
        expect(prefixKept, "today's candidates keep their indices, canonical order and incumbent behind no follower");
        expect(problem.actors()[0].candidates[1].dependencies ==
                   std::vector<MasterVacateRequirement>{{.tile = {1, 0}, .occupantUnitId = CHAIN_LEADER_UNIT}},
               "a follow candidate requires its leader to vacate");
        const auto chain = replayMasterAssignment(problem, std::vector<std::int32_t>{1, 1, 1});
        expect(chain.complete() && chain.ordering.order == std::vector<std::int32_t>{2, 1, 0},
               "a legal chain of three replays complete with each leader before its follower");
        const auto swap = replayMasterAssignment(problem, std::vector<std::int32_t>{0, 1, 2});
        expect(swap.valid() && !swap.complete(), "two units trading tiles replay incomplete");
        const auto staying = evaluateReplyPassPlan(problem, std::vector<std::int32_t>{1, 0, 0}, EVALUATION_BOARD);
        expect(staying.reason == ReplyPassNoValue::ReplayInvalid &&
                   staying.replay.error == MasterAssignmentReplayError::ClaimRejected,
               "a follower onto a tile whose occupant stays has no value because its replay is invalid");
    }

    void testReplyPassNoValueReasons()
    {
        constexpr auto missing = "a chosen candidate without reply facts has no value, named by its slot";
        constexpr auto saved = "a unit its isolated counter kills but the plan saves has no next-shot basis";
        constexpr auto gone = "a live shot at a target an earlier action destroyed has no value";
        constexpr auto invalid = "an invalid replay has no value";
        constexpr auto incomplete = "a replay left incomplete by a cycle has no value";
        constexpr auto arithmetic = "an overflowing next shot has no value, named by its slot";
        constexpr auto selfService = "a service on the actor itself is recognised for refusal";
        constexpr auto mapped = "every own-action status maps to its named no-value reason";
        constexpr auto unevaluated = "a default-constructed evaluation claims no value";
        expect(!ReplyPassEvaluation{}.valued(), unevaluated);
        const std::vector<std::int32_t> both{0, 0};
        auto unfacted = weakenedTargetInput(WEAKENED_ISOLATED_COUNTER);
        unfacted.actors[0].candidates[0].replyFacts.reset();
        expect(refusedFor(evaluatedChoices(unfacted, both), ReplyPassNoValue::MissingFacts, 0),
               missing);
        const auto savedInput = weakenedTargetInput(UNIT_HP_STEPS);
        expect(refusedFor(evaluatedChoices(savedInput, both),
                          ReplyPassNoValue::MissingNextShotBasis, 0),
               saved);
        const auto weakened = weakenedTargetInput(WEAKENED_ISOLATED_COUNTER);
        expect(refusedFor(evaluatedChoices(weakened, std::vector<std::int32_t>{0, 1}),
                          ReplyPassNoValue::TargetAlreadyGone, 0),
               gone);
        expect(evaluatedChoices(weakened, std::vector<std::int32_t>{0}).reason ==
                   ReplyPassNoValue::ReplayInvalid,
               invalid);
        auto vacate = inputWith(2);
        vacate.actors[0].candidates = {candidate(0, {0, 0}, {-1, 0}, 1), candidate(0, {0, 0}, {1, 0}, 1)};
        vacate.actors[1].candidates = {candidate(1, {1, 0}, {0, 0}, 1), candidate(1, {1, 0}, {2, 0}, 1)};
        expect(evaluatedChoices(vacate, std::vector<std::int32_t>{1, 0}).reason ==
                   ReplyPassNoValue::ReplayIncomplete,
               incomplete);
        auto overflowing = weakenedTargetInput(WEAKENED_ISOLATED_COUNTER);
        overflowing.actors[0].candidates[0].actorNextShotsAtDestination = {std::numeric_limits<MilliFunds>::max() / 2};
        expect(refusedFor(evaluatedChoices(overflowing, both), ReplyPassNoValue::Arithmetic, 0),
               arithmetic);
        ActionBundle serving;
        serving.unitId = 0;
        serving.components = {serviceComponent({.partnerUnitId = 0})};
        ActionBundle servingOther = serving;
        servingOther.components.front().service.partnerUnitId = 1;
        expect(ReplyPassEvaluationDetail::servesSelf(serving) && !ReplyPassEvaluationDetail::servesSelf(servingOther),
               selfService);
        using ReplyPassEvaluationDetail::noValueOf;
        expect(noValueOf(StockQuoteStatus::Available) == ReplyPassNoValue::None &&
                   noValueOf(StockQuoteStatus::MissingField) == ReplyPassNoValue::MissingFacts &&
                   noValueOf(StockQuoteStatus::MissingActionFacts) == ReplyPassNoValue::MissingFacts &&
                   noValueOf(StockQuoteStatus::UnusablePlanChain) == ReplyPassNoValue::UnusablePlanChain &&
                   noValueOf(StockQuoteStatus::ReplayFailed) == ReplyPassNoValue::ReplayInvalid &&
                   noValueOf(StockQuoteStatus::CyclicOrdering) == ReplyPassNoValue::ReplayIncomplete &&
                   noValueOf(StockQuoteStatus::UnsupportedAction) == ReplyPassNoValue::UnsupportedAction &&
                   noValueOf(StockQuoteStatus::TargetAlreadyGone) == ReplyPassNoValue::TargetAlreadyGone,
               mapped);
    }

    void testReplyPassRefusesOverflowingAmounts()
    {
        constexpr MilliFunds largest = std::numeric_limits<MilliFunds>::max();
        constexpr auto target = "a shot target whose capital P1 cannot book refuses before P1 runs";
        constexpr auto shooter = "a shooter cost P1 cannot book refuses before P1 runs";
        constexpr auto capture = "a capture cost P1 cannot subtract refuses before P1 runs";
        const std::vector<std::int32_t> only{0};
        auto costly = inputWith(1);
        auto & costlyChoice = costly.actors[0].candidates[0];
        readyForReply(costlyChoice, {replyShooterAt(costlyChoice.bundle.destination)});
        costlyChoice.actor.replacementCost = largest;
        expect(refusedFor(evaluatedChoices(costly, only), ReplyPassNoValue::Arithmetic, 0), target);
        ReplyShooterFacts expensive = replyShooterAt(TilePoint{0, 0});
        expensive.cost = largest;
        auto shot = inputWith(1);
        readyForReply(shot.actors[0].candidates[0], {expensive});
        expect(refusedFor(evaluatedChoices(shot, only), ReplyPassNoValue::Arithmetic, 0), shooter);
        ReplyShooterFacts capturing = replyShooterAt(TilePoint{0, 0});
        capturing.captureCost = largest;
        auto captured = inputWith(1);
        readyForReply(captured.actors[0].candidates[0], {capturing});
        expect(refusedFor(evaluatedChoices(captured, only), ReplyPassNoValue::Arithmetic, 0), capture);
    }

    // One wait unit at (0,0) shot by one direct shooter from (0,1); adjust edits that shooter and the reply facts.
    template <typename Adjust>
    ReplyPassEvaluation evaluatedOneShooter(Adjust && adjust)
    {
        auto input = inputWith(1);
        auto & choice = input.actors[0].candidates[0];
        ReplyShooterFacts shooter = replyShooterAt(choice.bundle.destination);
        shooter.direct = true;
        shooter.firingTiles = {shooter.tile};
        readyForReply(choice, {shooter});
        auto facts = std::make_shared<ReplyFacts>(*choice.replyFacts);
        auto tile = std::make_shared<ReplyTileFacts>(*facts->tile);
        adjust(tile->shooters.front(), *facts);
        facts->tile = tile;
        choice.replyFacts = facts;
        return evaluatedChoices(input, std::vector<std::int32_t>{0});
    }

    void testReplyPassBoundsReplyArithmeticInputs()
    {
        using ReplyPassEvaluationDetail::REPLY_FACTOR_LIMIT;
        constexpr std::int32_t intMax = std::numeric_limits<std::int32_t>::max();
        constexpr std::int32_t intMin = std::numeric_limits<std::int32_t>::min();
        constexpr double largest = std::numeric_limits<double>::max();
        constexpr auto shooterHp = "a pooled shooter above a full unit's hp steps refuses before P1 runs";
        constexpr auto terrainOver = "a terrain product beyond int range refuses before P1 runs";
        constexpr auto terrainEdges = "terrain products at either end of int range still get a value";
        constexpr auto baseOver = "an oversized or non-finite shot base refuses before P1 runs";
        constexpr auto baseEdge = "a negative shot base at the factor limit still gets a value";
        constexpr auto ammoOver = "an oversized resupplied shot base refuses before P1 runs";
        constexpr auto luckOver = "an oversized terrain luck refuses before P1 runs";
        constexpr auto counterOver = "an oversized counter base refuses before P1 runs";
        const auto refused = [](const ReplyPassEvaluation & evaluation)
        {
            return refusedFor(evaluation, ReplyPassNoValue::Arithmetic, 0);
        };
        expect(refused(evaluatedOneShooter([](ReplyShooterFacts & shooter, ReplyFacts &)
                                           { shooter.dayStartHpSteps = UNIT_HP_STEPS + 1; })),
               shooterHp);
        const auto withTerrain = [](std::int32_t stars, std::int32_t defensePerStar)
        {
            return evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts)
            {
                facts.terrain.stars = stars;
                facts.terrain.defensePerStar = defensePerStar;
            });
        };
        expect(refused(withTerrain(intMax, 2)) && refused(withTerrain(intMin, 2)), terrainOver);
        expect(withTerrain(1, intMax).valued() && withTerrain(intMin, 1).valued(), terrainEdges);
        const auto withBase = [](double base)
        {
            return evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts) { facts.shots[0].rawBase = base; });
        };
        expect(refused(withBase(largest)) && refused(withBase(std::numeric_limits<double>::quiet_NaN())) &&
                   refused(withBase(-largest)),
               baseOver);
        expect(withBase(-REPLY_FACTOR_LIMIT).valued(), baseEdge);
        expect(refused(evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts)
                                           { facts.shots[0].withAmmo = largest; })),
               ammoOver);
        expect(refused(evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts)
                                           { facts.terrain.luck = largest; })),
               luckOver);
        expect(refused(evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts)
                                           { facts.shots[0].counterRawBase = largest; })),
               counterOver);
    }

    void testReplyPassBoundsActorRealHp()
    {
        constexpr double partialHp = 9.3;
        constexpr auto outOfRange = "a non-finite, non-positive or oversized real hp refuses with missing facts";
        constexpr auto partial = "a fractional real hp within its unit's steps still gets a value";
        const auto withRealHp = [](double realHp)
        {
            return evaluatedOneShooter([&](ReplyShooterFacts &, ReplyFacts & facts) { facts.actorRealHp = realHp; });
        };
        constexpr auto predicate = "the real hp predicate accepts exactly the finite hp in (0, UNIT_HP_STEPS]";
        const double justAbove =
            std::nextafter(static_cast<double>(UNIT_HP_STEPS), std::numeric_limits<double>::infinity());
        const std::vector<double> outside{std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::infinity(), 0.0, -1.0, justAbove};
        bool refused = true;
        bool rejected = true;
        for (const double realHp : outside)
        {
            refused = refused && refusedFor(withRealHp(realHp), ReplyPassNoValue::MissingFacts, 0);
            rejected = rejected && !ReplyPassEvaluationDetail::actorRealHpFits(realHp);
        }
        expect(refused, outOfRange);
        expect(withRealHp(partialHp).valued(), partial);
        expect(rejected && ReplyPassEvaluationDetail::actorRealHpFits(partialHp) &&
                   ReplyPassEvaluationDetail::actorRealHpFits(UNIT_HP_STEPS),
               predicate);
    }

    void testReplyPassKeepsTilesOnBoard()
    {
        constexpr std::int32_t intMax = std::numeric_limits<std::int32_t>::max();
        constexpr auto farFiring = "a firing tile far off the board refuses with missing facts";
        constexpr auto farShooter = "a shooter tile off the board refuses with missing facts";
        constexpr auto negative = "a negative firing tile coordinate refuses with missing facts";
        constexpr auto onBoard = "a shooter with on-board and absent tiles still gets a value";
        const auto missing = [](const ReplyPassEvaluation & evaluation)
        {
            return refusedFor(evaluation, ReplyPassNoValue::MissingFacts, 0);
        };
        expect(missing(evaluatedOneShooter([](ReplyShooterFacts & shooter, ReplyFacts &)
                                           { shooter.firingTiles = {{intMax, 1}}; })),
               farFiring);
        expect(missing(evaluatedOneShooter([](ReplyShooterFacts & shooter, ReplyFacts &)
                                           { shooter.tile = {REPLY_BOARD_SIZE, 1}; })),
               farShooter);
        expect(missing(evaluatedOneShooter([](ReplyShooterFacts & shooter, ReplyFacts &)
                                           { shooter.firingTiles = {{0, -2}}; })),
               negative);
        expect(evaluatedOneShooter([](ReplyShooterFacts & shooter, ReplyFacts &)
                                   { shooter.firingTiles = {INVALID_TILE, {1, 1}, shooter.tile}; })
                   .valued(),
               onBoard);
    }

    void testReplyPassKeepsSignedAmounts()
    {
        using ReplyPassEvaluationDetail::ONE_ACTION_AMOUNT_LIMIT;
        constexpr MilliFunds rebate = -1000;
        constexpr auto negative = "a plan whose reply carries a negative capture cost still gets a value";
        constexpr auto edges = "capture costs at either signed end of the amount limit still get a value";
        constexpr auto beyond = "capture costs one past either signed end of the amount limit refuse";
        const auto withCapture = [](MilliFunds cost)
        {
            return evaluatedOneShooter([&](ReplyShooterFacts & shooter, ReplyFacts &) { shooter.captureCost = cost; });
        };
        const auto rebated = withCapture(rebate);
        expect(rebated.valued() && rebated.shots.size() == 1, negative);
        expect(withCapture(-ONE_ACTION_AMOUNT_LIMIT).valued() && withCapture(ONE_ACTION_AMOUNT_LIMIT).valued(), edges);
        expect(refusedFor(withCapture(-ONE_ACTION_AMOUNT_LIMIT - 1), ReplyPassNoValue::Arithmetic, 0) &&
                   refusedFor(withCapture(ONE_ACTION_AMOUNT_LIMIT + 1), ReplyPassNoValue::Arithmetic, 0),
               beyond);
    }

    void testReplyPassRefusesMalformedFacts()
    {
        constexpr auto tileless = "reply facts without a tile refuse with missing facts";
        constexpr auto shotless = "reply facts with fewer shots than tile shooters refuse with missing facts";
        constexpr auto incomplete = "malformed reply facts leave the recorded prediction incomplete";
        const std::vector<std::int32_t> only{0};
        const auto malformed = [](bool keepTile)
        {
            auto input = inputWith(1);
            auto & choice = input.actors[0].candidates[0];
            readyForReply(choice, {replyShooterAt(choice.bundle.destination)});
            auto facts = std::make_shared<ReplyFacts>(*choice.replyFacts);
            if (keepTile)
            {
                facts->shots.clear();
            }
            else
            {
                facts->tile.reset();
            }
            choice.replyFacts = facts;
            return input;
        };
        expect(refusedFor(evaluatedChoices(malformed(false), only), ReplyPassNoValue::MissingFacts, 0), tileless);
        expect(refusedFor(evaluatedChoices(malformed(true), only), ReplyPassNoValue::MissingFacts, 0), shotless);
        bool recordedIncomplete = true;
        for (const bool keepTile : {false, true})
        {
            const auto input = malformed(keepTile);
            const auto tally = std::make_shared<ReplyPredictionTally>();
            const auto extracted = extractRecorded(input, planFrom(input), tally);
            recordedIncomplete = recordedIncomplete && extracted.valid() && tally->incomplete == 1;
            if (extracted.valid())
            {
                const auto replay = replayMasterAssignment(*extracted.problem, extracted.problem->incumbentChoices());
                recordedIncomplete = recordedIncomplete && replay.complete() && replay.replyPrediction &&
                                     !replay.replyPrediction->complete && tally->incomplete == 2;
            }
        }
        expect(recordedIncomplete, incomplete);
    }

    void testReplyPassThreeRowsByHand()
    {
        constexpr std::size_t shotSlots = 2;
        constexpr std::array<double, shotSlots> shotBases{30, 40};
        constexpr std::array<double, shotSlots> counterBases{30, 50};
        constexpr std::array<std::int32_t, 3> rows{5, 6, 7};
        constexpr std::size_t unharmedSlot = 2;
        constexpr MilliFunds unharmedShot = 300;
        constexpr ReplyPassTerms slotZero{
            .actingSeated = 1, .replyCapitalLoss = 2100, .replyCounterCapital = 1400, .nextShotAfterReply = 630};
        constexpr ReplyPassTerms slotOne{
            .actingSeated = 2, .replyCapitalLoss = 2800, .replyCounterCapital = 2100, .nextShotAfterReply = 600};
        constexpr ReplyPassTerms slotTwo{.actingSeated = 3, .nextShotAfterReply = unharmedShot};
        constexpr MilliFunds totalByHand = 136;
        constexpr std::int32_t slotZeroLoss = 3;
        constexpr std::int32_t slotOneLoss = 4;
        constexpr auto extracts = "three-row pass fixture extracts";
        constexpr auto order = "the three actors run in reverse slot order";
        constexpr auto shots = "each shooter fires once on its own unit, the lower shooter index first";
        constexpr auto terms = "every actor's five terms match the values computed by hand";
        constexpr auto total = "the exact non-stock value matches the total computed by hand";
        constexpr auto keys = "both keys list the three rows sorted, with the reply losses after the reply";
        auto input = inputWith(static_cast<std::int32_t>(rows.size()));
        for (std::size_t slot = 0; slot < shotSlots; ++slot)
        {
            auto & choice = input.actors[slot].candidates[0];
            ReplyShooterFacts shooter = replyShooterAt(choice.bundle.destination);
            shooter.shooterIndex = REPLY_SHOOTER + static_cast<std::int32_t>(slot);
            shooter.direct = true;
            shooter.firingTiles = {{choice.bundle.destination.x, 1}};
            readyForReply(choice, {shooter});
            auto facts = std::make_shared<ReplyFacts>(*choice.replyFacts);
            facts->shots = {ReplyShotBase{shotBases[slot], shotBases[slot], counterBases[slot]}};
            choice.replyFacts = facts;
        }
        readyForReply(input.actors[unharmedSlot].candidates[0]);
        input.actors[0].candidates[0].actorNextShotsAtDestination = {900};
        input.actors[1].candidates[0].actorNextShotsAtDestination = {DESTINATION_SHOT, 500};
        input.actors[unharmedSlot].candidates[0].actorNextShotsAtDestination = {unharmedShot};
        auto fixture = planFrom(input);
        for (std::size_t slot = 0; slot < rows.size(); ++slot)
        {
            fixture.selections[slot].stockRow = rows[slot];
        }
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto evaluation = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        expect(evaluation.valued() && evaluation.replay.ordering.order == std::vector<std::int32_t>{2, 1, 0},
               order);
        const std::vector<ReplyPassShot> shotsByHand{
            {REPLY_SHOOTER, 2, UNIT_HP_STEPS, UNIT_HP_STEPS - slotZeroLoss, slotZero.replyCounterCapital},
            {SECOND_SHOOTER, 1, UNIT_HP_STEPS, UNIT_HP_STEPS - slotOneLoss, slotOne.replyCounterCapital},
        };
        expect(evaluation.shots == shotsByHand, shots);
        const ReplyPassActor* zero = evaluatedFor(evaluation, 0);
        const ReplyPassActor* one = evaluatedFor(evaluation, 1);
        const ReplyPassActor* two = evaluatedFor(evaluation, static_cast<std::int32_t>(unharmedSlot));
        expect(zero != nullptr && one != nullptr && two != nullptr && zero->terms == slotZero &&
                   one->terms == slotOne && two->terms == slotTwo,
               terms);
        expect(evaluation.exactNonStock == totalByHand, total);
        const auto keyOf = [&](std::size_t slot, std::int32_t stepsLost)
        {
            return *CanonicalPlanActionKey::checkedFromAction(rows[slot], static_cast<std::int32_t>(slot), 0, false,
                                                              stepsLost, false);
        };
        const ContinuationKey afterOwn{keyOf(0, 0), keyOf(1, 0), keyOf(unharmedSlot, 0)};
        const ContinuationKey afterReply{keyOf(0, slotZeroLoss), keyOf(1, slotOneLoss), keyOf(unharmedSlot, 0)};
        expect(evaluation.afterOwnActions == afterOwn && evaluation.afterReply == afterReply, keys);
    }

    void testReplyPassRepricesFixedActor()
    {
        constexpr std::int32_t mutableDamage = 4;
        constexpr std::int32_t fixedDamage = 9;
        constexpr std::int32_t incumbentGrant = 6;
        constexpr std::size_t fixedSlot = 1;
        constexpr auto extracts = "fixed repricing pass fixture extracts";
        constexpr auto repriced = "a fixed shooter that absorbs a freed share is valued at its new grant";
        auto input = inputWith(2);
        input.unitLinks.push_back({SHOT_TARGET_ENGINE_ID, {2, 1}});
        input.actors[0].candidates.push_back(input.actors[0].candidates[0]);
        addFire(input.actors[0].candidates[0], mutableDamage);
        addFire(input.actors[fixedSlot].candidates[0], fixedDamage);
        for (auto & actor : input.actors)
        {
            for (auto & choice : actor.candidates)
            {
                readyForReply(choice);
            }
        }
        auto fixture = planFrom(input);
        fixture.selections[fixedSlot].role = MasterActorRole::Fixed;
        const auto extracted = extract(input, fixture);
        expect(extracted.valid(), extracts);
        if (!extracted.valid())
        {
            return;
        }
        const auto & problem = *extracted.problem;
        const auto & fixedSource = problem.actors()[fixedSlot].candidates[0].source;
        const auto shared = evaluateReplyPassPlan(problem, problem.incumbentChoices(), EVALUATION_BOARD);
        const auto freed = evaluateReplyPassPlan(problem, std::vector<std::int32_t>{1, 0}, EVALUATION_BOARD);
        const ReplyPassActor* before = evaluatedFor(shared, static_cast<std::int32_t>(fixedSlot));
        const ReplyPassActor* after = evaluatedFor(freed, static_cast<std::int32_t>(fixedSlot));
        expect(shared.valued() && freed.valued() && before != nullptr && after != nullptr &&
                   before->grantedDamage == incumbentGrant && after->grantedDamage == fixedDamage &&
                   before->terms.actingSeated == checkedActingSeatedCandidateValue(fixedSource, incumbentGrant) &&
                   after->terms.actingSeated == checkedActingSeatedCandidateValue(fixedSource, fixedDamage) &&
                   before->terms.actingSeated != after->terms.actingSeated,
               repriced);
    }

    void testReplyPassIsDeterministicAndModeFree()
    {
        constexpr auto extracts = "determinism pass fixtures extract";
        constexpr auto deterministic = "evaluating the same choices twice gives the same result";
        constexpr auto modeFree = "the result does not depend on the problem's own prediction mode";
        auto input = inputWith(2);
        exposeEachToOneShooter(input);
        const auto fixture = planFrom(input);
        const auto off = extract(input, fixture);
        const auto recorded = extractRecorded(input, fixture);
        expect(off.valid() && recorded.valid(), extracts);
        if (!off.valid() || !recorded.valid())
        {
            return;
        }
        const auto first = evaluateReplyPassPlan(*off.problem, off.problem->incumbentChoices(), EVALUATION_BOARD);
        const auto second = evaluateReplyPassPlan(*off.problem, off.problem->incumbentChoices(), EVALUATION_BOARD);
        const auto fromRecorded =
            evaluateReplyPassPlan(*recorded.problem, recorded.problem->incumbentChoices(), EVALUATION_BOARD);
        expect(first.valued() && !first.shots.empty() && sameEvaluation(first, second), deterministic);
        expect(sameEvaluation(first, fromRecorded) && !fromRecorded.replay.replyPrediction, modeFree);
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
    testReplyPassAgreesWithPrediction();
    testReplyPassWeakenedTargetTerms();
    testReplyPassActorShotTwice();
    testReplyPassReplyKillsCapturer();
    testReplyPassOwnActionDeath();
    testReplyPassCorrectionRebuildsCandidate();
    testFollowCandidatesTrailTodaysCandidates();
    testReplyPassNoValueReasons();
    testReplyPassRefusesOverflowingAmounts();
    testReplyPassBoundsReplyArithmeticInputs();
    testReplyPassBoundsActorRealHp();
    testReplyPassKeepsTilesOnBoard();
    testReplyPassKeepsSignedAmounts();
    testReplyPassRefusesMalformedFacts();
    testReplyPassThreeRowsByHand();
    testReplyPassRepricesFixedActor();
    testReplyPassIsDeterministicAndModeFree();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
