#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include <QStringList>

#include "ai/coordinator/bundleassignment.h"

namespace
{
using Coordinator::ActorProgress;
using Coordinator::ActorResources;
using Coordinator::AssignmentActor;
using Coordinator::AssignmentInput;
using Coordinator::AssignmentResult;
using Coordinator::AssignmentStats;
using Coordinator::AssignPhase;
using Coordinator::CandidateBundle;
using Coordinator::captureComponent;
using Coordinator::CaptureFacts;
using Coordinator::DecisionTrace;
using Coordinator::fireComponent;
using Coordinator::FireFacts;
using Coordinator::KnownUnitLink;
using Coordinator::MaximumValueAssignment;
using Coordinator::MilliFunds;
using Coordinator::NO_ACTION;
using Coordinator::NO_CANDIDATE;
using Coordinator::PlanActionIds;
using Coordinator::PlanStockValuer;
using Coordinator::PlanActionState;
using Coordinator::PlanBundleKind;
using Coordinator::PlannedAction;
using Coordinator::ReservationResult;
using Coordinator::StockInterval;
using Coordinator::TilePoint;
using Coordinator::TurnPlan;

constexpr std::int32_t ATTACKER_INDEX = 0;
constexpr std::int32_t SUPPORT_INDEX = 1;
constexpr std::int32_t TARGET_INDEX = 2;
constexpr std::int32_t ATTACKER_UNIT = 11;
constexpr std::int32_t SUPPORT_UNIT = 12;
constexpr std::int32_t TARGET_UNIT = 13;

constexpr TilePoint ATTACKER_TILE{1, 1};
constexpr TilePoint SUPPORT_TILE{1, 3};
constexpr TilePoint TARGET_TILE{4, 1};
constexpr TilePoint CONTESTED_TILE{2, 1};
constexpr TilePoint SPARE_TILE{2, 3};
constexpr TilePoint FAR_TILE{3, 3};

constexpr MilliFunds BEST_VALUE = 1000;
constexpr MilliFunds GOOD_VALUE = 900;
constexpr MilliFunds FAIR_VALUE = 800;
constexpr MilliFunds POOR_VALUE = 100;
constexpr MilliFunds LOSING_VALUE = -50;
constexpr MilliFunds CLUSTER_OPTIMUM = 2450;
constexpr MilliFunds CLUSTER_SETTLED = 1800;
constexpr MilliFunds CLUSTER_MIDDLE_VALUE = 850;
constexpr MilliFunds CLUSTER_TAIL_VALUE = 700;
constexpr std::int32_t TARGET_HP_STEPS = 3;
constexpr std::int32_t LETHAL_DAMAGE_STEPS = 3;

int failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition)
    {
        std::printf("FAILED: %s\n", description);
        ++failures;
    }
}

enum class IntervalFailure
{
    None,
    IncumbentWitness,
    LeafQuote,
    ReplayQuote,
};

class PairIntervalValuer final : public PlanStockValuer
{
public:
    explicit PairIntervalValuer(
        IntervalFailure failure,
        std::int32_t affectedUnit = Coordinator::NO_UNIT)
        : m_failure(failure),
          m_affectedUnit(affectedUnit)
    {
    }

    MilliFunds planStock(const TurnPlan &) override
    {
        ++scalarCalls;
        if (m_liveQuoteSeen)
        {
            ++scalarCallsAfterLiveQuote;
        }
        return 0;
    }

    MilliFunds originStock() const override
    {
        return 0;
    }

    MilliFunds stockCeiling() const override
    {
        return 0;
    }

    bool affectsStock(std::int32_t engineUnitId) const override
    {
        return m_affectedUnit == Coordinator::NO_UNIT ||
               m_affectedUnit == engineUnitId;
    }

    bool livePairSwapIntervals() const override
    {
        return true;
    }

    Coordinator::LivePlanStockQuote livePlanStock(
        const TurnPlan & plan,
        MilliFunds,
        bool pricingLeaf) override
    {
        m_liveQuoteSeen = true;
        Coordinator::LivePlanStockQuote quote;
        if (pricingLeaf)
        {
            ++liveLeaves;
            if (m_failure == IntervalFailure::LeafQuote &&
                !m_failed)
            {
                m_failed = true;
                return quote;
            }
        }
        else
        {
            ++nonLeafQuotes;
            if (m_failure ==
                    IntervalFailure::IncumbentWitness &&
                !m_failed)
            {
                m_failed = true;
                quote.valid = true;
                return quote;
            }
        }
        for (std::int32_t index = 0;
             index < plan.actionCount();
             ++index)
        {
            const PlannedAction & action = plan.action(index);
            if (!Coordinator::isLiveState(action.state))
            {
                continue;
            }
            quote.key.push_back(
                Coordinator::CanonicalPlanActionKey::fromAction(
                    action.unitId,
                    action.destination.x,
                    action.destination.y,
                    false));
        }
        std::sort(quote.key.begin(), quote.key.end());
        if (!pricingLeaf &&
            m_failure == IntervalFailure::ReplayQuote &&
            nonLeafQuotes == 2)
        {
            quote.key.push_back(
                Coordinator::CanonicalPlanActionKey::fromAction(
                    -1,
                    -1,
                    -1,
                    false));
        }
        quote.stockAbsolute = StockInterval{0, 0};
        quote.valid = true;
        quote.lowerWitnessReplays = true;
        return quote;
    }

    bool refineLiveAtBoundary(AssignPhase phase) override
    {
        ++refinementBoundaries;
        wrongRefinementPhase =
            wrongRefinementPhase ||
            phase != AssignPhase::BetweenSwapSweeps;
        return false;
    }

    std::int32_t scalarCalls{0};
    std::int32_t scalarCallsAfterLiveQuote{0};
    std::int32_t liveLeaves{0};
    std::int32_t nonLeafQuotes{0};
    std::int32_t refinementBoundaries{0};
    bool wrongRefinementPhase{false};

private:
    IntervalFailure m_failure;
    std::int32_t m_affectedUnit;
    bool m_failed{false};
    bool m_liveQuoteSeen{false};
};

class BudgetExhaustingValuer final : public PlanStockValuer
{
public:
    MilliFunds planStock(const TurnPlan &) override
    {
        return 0;
    }

    MilliFunds originStock() const override
    {
        return 0;
    }

    MilliFunds stockCeiling() const override
    {
        return SEARCH_SLACK;
    }

    bool affectsStock(std::int32_t) const override
    {
        return true;
    }

private:
    static constexpr MilliFunds SEARCH_SLACK = 1;
};

class RecordingTrace final : public DecisionTrace
{
public:
    bool candidateDetailsEnabled() const override
    {
        return true;
    }

    bool stockDetailsEnabled() const override
    {
        return true;
    }

    void record(
        const QString & category,
        const QString & fields) override
    {
        records.push_back(category + QLatin1Char(' ') + fields);
    }

    void flush() override
    {
        ++flushes;
    }

    bool contains(const QString & category) const
    {
        return std::any_of(
            records.begin(),
            records.end(),
            [&](const QString & record)
            {
                return record.startsWith(
                    category + QLatin1Char(' '));
            });
    }

    QStringList records;
    std::int32_t flushes{0};
};

PlanActionIds testActionIds()
{
    return PlanActionIds{
        .wait = QStringLiteral("ACTION_WAIT"),
        .fire = QStringLiteral("ACTION_FIRE"),
        .capture = QStringLiteral("ACTION_CAPTURE"),
    };
}

std::vector<KnownUnitLink> testUnitLinks()
{
    return {
        KnownUnitLink{ATTACKER_UNIT, ATTACKER_TILE},
        KnownUnitLink{SUPPORT_UNIT, SUPPORT_TILE},
        KnownUnitLink{TARGET_UNIT, TARGET_TILE},
    };
}

CandidateBundle valuedCandidate(std::int32_t unitIndex, TilePoint origin, TilePoint destination, MilliFunds value)
{
    CandidateBundle candidate;
    candidate.bundle.unitId = unitIndex;
    candidate.bundle.origin = origin;
    candidate.bundle.destination = destination;
    candidate.bundle.path.push_back(origin);
    if (origin != destination)
    {
        candidate.bundle.path.push_back(destination);
    }
    candidate.valuation.ledger.enemyCapital = value;
    candidate.valuation.valid = true;
    return candidate;
}

CandidateBundle firingCandidate(std::int32_t unitIndex, TilePoint origin, TilePoint destination,
                                std::int32_t damageSteps, MilliFunds value)
{
    CandidateBundle candidate = valuedCandidate(unitIndex, origin, destination, value);
    const FireFacts fire{
        .targetUnitId = TARGET_INDEX,
        .targetReplacementCost = 0,
        .targetHpSteps = TARGET_HP_STEPS,
        .damageSteps = damageSteps,
        .counterSteps = 0,
        .targetBestShotBefore = 0,
        .targetBestShotAfter = 0,
    };
    candidate.bundle.components.push_back(fireComponent(fire));
    return candidate;
}

CandidateBundle capturingCandidate(std::int32_t unitIndex, TilePoint origin, TilePoint destination,
                                   MilliFunds value)
{
    CandidateBundle candidate = valuedCandidate(unitIndex, origin, destination, value);
    candidate.bundle.components.push_back(captureComponent(CaptureFacts{}));
    return candidate;
}

AssignmentActor actorWith(std::int32_t knowledgeIndex, std::int32_t engineUnitId,
                          std::vector<CandidateBundle> candidates)
{
    AssignmentActor actor;
    actor.knowledgeUnitIndex = knowledgeIndex;
    actor.engineUnitId = engineUnitId;
    actor.candidates = std::move(candidates);
    return actor;
}

AssignmentInput inputWith(std::vector<AssignmentActor> actors)
{
    AssignmentInput input;
    input.actionIds = testActionIds();
    input.unitLinks = testUnitLinks();
    input.actors = std::move(actors);
    return input;
}

std::int32_t addAndClaim(TurnPlan & plan, const AssignmentInput & input, std::int32_t engineUnitId,
                         const CandidateBundle & candidate)
{
    const PlannedAction action =
        Coordinator::plannedActionFrom(input.actionIds, input.unitLinks, engineUnitId, candidate);
    const std::int32_t index = plan.addAction(action);
    if (index != NO_ACTION)
    {
        Coordinator::claimOrRollback(plan, index, action, candidate);
    }
    return index;
}

const PlannedAction* actionOf(const AssignmentResult & result, std::int32_t engineUnitId)
{
    for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
    {
        if (result.plan.action(index).unitId == engineUnitId)
        {
            return &result.plan.action(index);
        }
    }
    return nullptr;
}

MilliFunds plannedTotal(const AssignmentResult & result)
{
    MilliFunds total = 0;
    for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
    {
        const PlannedAction & action = result.plan.action(index);
        if (Coordinator::isLiveState(action.state))
        {
            total += action.marginalValue.economicValue;
        }
    }
    return total;
}

void testConversionMapsEngineActionsAndTargets()
{
    const PlanActionIds ids = testActionIds();
    const std::vector<KnownUnitLink> links = testUnitLinks();
    const CandidateBundle move = valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, POOR_VALUE);
    const PlannedAction moved = Coordinator::plannedActionFrom(ids, links, ATTACKER_UNIT, move);
    expect(moved.kind == PlanBundleKind::Move && moved.actionId == ids.wait, "move maps to wait");
    expect(moved.path.front() == ATTACKER_TILE && moved.destination == CONTESTED_TILE, "move keeps its endpoints");

    const CandidateBundle fire =
        firingCandidate(ATTACKER_INDEX, ATTACKER_TILE, ATTACKER_TILE, LETHAL_DAMAGE_STEPS, BEST_VALUE);
    const PlannedAction fired = Coordinator::plannedActionFrom(ids, links, ATTACKER_UNIT, fire);
    expect(fired.kind == PlanBundleKind::Fire && fired.actionId == ids.fire, "fire maps to fire");
    expect(fired.targetUnitId == TARGET_UNIT && fired.target == TARGET_TILE, "fire resolves its target link");

    const CandidateBundle capture = capturingCandidate(SUPPORT_INDEX, SUPPORT_TILE, FAR_TILE, FAIR_VALUE);
    const PlannedAction captured = Coordinator::plannedActionFrom(ids, links, SUPPORT_UNIT, capture);
    expect(captured.kind == PlanBundleKind::MoveAndCapture && captured.actionId == ids.capture,
           "capture maps to capture");
    expect(captured.target == FAR_TILE, "capture targets its destination");

    CandidateBundle compound = fire;
    compound.bundle.components.push_back(captureComponent(CaptureFacts{}));
    expect(!Coordinator::isPlannableCandidate(ids, links, compound), "compound candidates are rejected");
}

void testDestinationConflictRollsBackTheRefusedAction()
{
    const AssignmentInput input = inputWith({});
    TurnPlan plan;
    const CandidateBundle first =
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, BEST_VALUE);
    const CandidateBundle second =
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, GOOD_VALUE);
    const std::int32_t firstIndex = addAndClaim(plan, input, ATTACKER_UNIT, first);
    const PlannedAction secondAction =
        Coordinator::plannedActionFrom(input.actionIds, input.unitLinks, SUPPORT_UNIT, second);
    const std::int32_t secondIndex = plan.addAction(secondAction);
    const ReservationResult result =
        Coordinator::claimOrRollback(plan, secondIndex, secondAction, second);
    expect(firstIndex != NO_ACTION && secondIndex != NO_ACTION, "both actions are installed");
    expect(result == ReservationResult::Conflict, "the second destination conflicts");
    expect(plan.destinationClaimant(CONTESTED_TILE) == firstIndex, "the first claim remains");
    expect(plan.action(secondIndex).state == PlanActionState::Pending, "the refused action stays pending");
}

void testAttackAndCaptureClaimsRejectOverbooking()
{
    const AssignmentInput input = inputWith({});
    TurnPlan attackPlan;
    const CandidateBundle lethal =
        firingCandidate(ATTACKER_INDEX, ATTACKER_TILE, ATTACKER_TILE, LETHAL_DAMAGE_STEPS, BEST_VALUE);
    const CandidateBundle overkill =
        firingCandidate(SUPPORT_INDEX, SUPPORT_TILE, SPARE_TILE, LETHAL_DAMAGE_STEPS, GOOD_VALUE);
    const std::int32_t lethalIndex = addAndClaim(attackPlan, input, ATTACKER_UNIT, lethal);
    const PlannedAction overkillAction =
        Coordinator::plannedActionFrom(input.actionIds, input.unitLinks, SUPPORT_UNIT, overkill);
    const std::int32_t overkillIndex = attackPlan.addAction(overkillAction);
    const ReservationResult attackResult =
        Coordinator::claimOrRollback(attackPlan, overkillIndex, overkillAction, overkill);
    expect(lethalIndex != NO_ACTION && attackPlan.targetIsLethal(TARGET_UNIT), "the first shot books lethal");
    expect(attackResult == ReservationResult::Overkill, "the second lethal shot is rejected");
    expect(attackPlan.destinationClaimant(SPARE_TILE) == NO_ACTION, "overkill releases its destination");

    TurnPlan capturePlan;
    const CandidateBundle firstCapture =
        capturingCandidate(ATTACKER_INDEX, ATTACKER_TILE, FAR_TILE, GOOD_VALUE);
    const CandidateBundle secondCapture =
        capturingCandidate(SUPPORT_INDEX, SUPPORT_TILE, FAR_TILE, FAIR_VALUE);
    const std::int32_t captureIndex = addAndClaim(capturePlan, input, ATTACKER_UNIT, firstCapture);
    const PlannedAction secondCaptureAction =
        Coordinator::plannedActionFrom(input.actionIds, input.unitLinks, SUPPORT_UNIT, secondCapture);
    const std::int32_t secondCaptureIndex = capturePlan.addAction(secondCaptureAction);
    const ReservationResult captureResult =
        Coordinator::claimOrRollback(capturePlan, secondCaptureIndex, secondCaptureAction, secondCapture);
    expect(captureResult == ReservationResult::Conflict, "a property has one capture claimant");
    expect(capturePlan.captureClaimant(FAR_TILE) == captureIndex, "the first capture claim remains");
}

void testCandidatePreparationIsStableAndIncludesGivingUp()
{
    std::vector<CandidateBundle> candidates;
    candidates.push_back(valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, GOOD_VALUE));
    candidates.push_back(valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, SPARE_TILE, GOOD_VALUE));
    candidates.push_back(valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, FAR_TILE, LOSING_VALUE));
    CandidateBundle unsupported = candidates.front();
    unsupported.bundle.components.push_back(fireComponent(FireFacts{}));
    unsupported.bundle.components.push_back(captureComponent(CaptureFacts{}));
    candidates.push_back(std::move(unsupported));

    AssignmentStats stats;
    const std::vector<KnownUnitLink> links = testUnitLinks();
    const std::vector<std::int32_t> order =
        Coordinator::plannableCandidateOrder(testActionIds(), links, candidates, stats);
    expect(order == std::vector<std::int32_t>({0, 1, 2}), "candidate order is stable on ties");
    expect(stats.candidates == 4 && stats.unsupportedCandidates == 1, "preparation counts every candidate");

    AssignmentActor actor = actorWith(ATTACKER_INDEX, ATTACKER_UNIT, std::move(candidates));
    ActorProgress state;
    state.pActor = &actor;
    state.order = order;
    state.options = Coordinator::buildOptionOrder(state);
    expect(state.options == std::vector<std::int32_t>({0, 1, NO_CANDIDATE, 2}),
           "giving up sorts before a losing action");
    state.chosen = 1;
    expect(Coordinator::incumbentFirstOptions(state) ==
               std::vector<std::int32_t>({1, 0, NO_CANDIDATE, 2}),
           "the incumbent leads its equal value tier");
}

void testSeatBestClaimableFallsThroughAConflict()
{
    std::vector<CandidateBundle> supportCandidates;
    supportCandidates.push_back(valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, BEST_VALUE));
    supportCandidates.push_back(valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, SPARE_TILE, FAIR_VALUE));
    AssignmentActor support = actorWith(SUPPORT_INDEX, SUPPORT_UNIT, std::move(supportCandidates));
    AssignmentInput input = inputWith({support});

    TurnPlan plan;
    const CandidateBundle blocker =
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, GOOD_VALUE);
    addAndClaim(plan, input, ATTACKER_UNIT, blocker);

    ActorProgress state;
    state.pActor = &input.actors.front();
    AssignmentStats stats;
    state.order = Coordinator::plannableCandidateOrder(
        input.actionIds, input.unitLinks, state.pActor->candidates, stats);
    state.options = Coordinator::buildOptionOrder(state);
    Coordinator::seatBestClaimable(plan, input, state, state.options);
    expect(state.chosen == 1 && state.seatedValue == FAIR_VALUE, "seating takes the best claimable option");
    expect(plan.destinationClaimant(SPARE_TILE) == state.actionIndex, "the fallback destination is claimed");
}

void testReplanRevivesAndCanAbandonAnAction()
{
    const AssignmentInput input = inputWith({});
    TurnPlan plan;
    const CandidateBundle initial =
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, SUPPORT_TILE, POOR_VALUE);
    const std::int32_t actionIndex = addAndClaim(plan, input, SUPPORT_UNIT, initial);
    plan.markFailed(actionIndex);
    const std::vector<CandidateBundle> fresh{
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, SPARE_TILE, FAIR_VALUE),
    };
    AssignmentStats stats;
    const bool revived = Coordinator::replanAction(
        plan, actionIndex, input.actionIds, input.unitLinks, fresh, stats);
    expect(revived && plan.action(actionIndex).state == PlanActionState::Pending, "fresh options revive an action");
    expect(plan.destinationClaimant(SPARE_TILE) == actionIndex, "the revived action claims its destination");

    const CandidateBundle blocker =
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, BEST_VALUE);
    addAndClaim(plan, input, ATTACKER_UNIT, blocker);
    const std::vector<CandidateBundle> blocked{
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, GOOD_VALUE),
    };
    const bool blockedResult = Coordinator::replanAction(
        plan, actionIndex, input.actionIds, input.unitLinks, blocked, stats);
    expect(!blockedResult && plan.action(actionIndex).state == PlanActionState::Abandoned,
           "an exhausted replan abandons the action");
    expect(plan.destinationClaimant(SPARE_TILE) == NO_ACTION, "abandoning releases the old claim");
}

void testActorResourcesExposeTileAndTargetConflicts()
{
    std::vector<CandidateBundle> attackerCandidates;
    attackerCandidates.push_back(valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, GOOD_VALUE));
    attackerCandidates.push_back(
        firingCandidate(ATTACKER_INDEX, ATTACKER_TILE, ATTACKER_TILE, 1, FAIR_VALUE));
    AssignmentActor attacker = actorWith(ATTACKER_INDEX, ATTACKER_UNIT, std::move(attackerCandidates));
    ActorProgress attackerState;
    attackerState.pActor = &attacker;
    attackerState.order = {0, 1};
    const ActorResources attackerResources = Coordinator::actorResourcesOf(attackerState);

    ActorResources destinationConflict;
    destinationConflict.destinations.push_back(CONTESTED_TILE);
    ActorResources targetConflict;
    targetConflict.targets.push_back(TARGET_INDEX);
    ActorResources independent;
    independent.destinations.push_back(FAR_TILE);
    expect(Coordinator::actorsContest(attackerResources, destinationConflict), "shared destinations conflict");
    expect(Coordinator::actorsContest(attackerResources, targetConflict), "shared fire targets conflict");
    expect(!Coordinator::actorsContest(attackerResources, independent), "independent resources do not conflict");
}

AssignmentInput losingFallbackInput()
{
    std::vector<CandidateBundle> attacker{
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, BEST_VALUE),
    };
    std::vector<CandidateBundle> support{
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, GOOD_VALUE),
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, SUPPORT_TILE, LOSING_VALUE),
    };
    return inputWith({
        actorWith(ATTACKER_INDEX, ATTACKER_UNIT, std::move(attacker)),
        actorWith(SUPPORT_INDEX, SUPPORT_UNIT, std::move(support)),
    });
}

void testSettlingDropsALosingFallback()
{
    const AssignmentResult result = MaximumValueAssignment::assign(losingFallbackInput());
    const PlannedAction* pAttacker = actionOf(result, ATTACKER_UNIT);
    const PlannedAction* pSupport = actionOf(result, SUPPORT_UNIT);
    expect(pAttacker != nullptr && pAttacker->destination == CONTESTED_TILE, "the winner keeps the tile");
    expect(pSupport != nullptr && pSupport->state == PlanActionState::Abandoned, "the losing fallback is abandoned");
    expect(plannedTotal(result) == BEST_VALUE, "settling removes the losing value");
    expect(result.stats.settlingMoves == 1 && result.stats.settlingSweeps == 2,
           "one moving sweep has one confirming sweep");
    expect(result.stats.unassignedUnits == 1, "the stranded actor is unassigned");
}

AssignmentInput destinationSwapInput()
{
    std::vector<CandidateBundle> attacker{
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, BEST_VALUE),
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, SPARE_TILE, GOOD_VALUE),
    };
    std::vector<CandidateBundle> support{
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, FAIR_VALUE),
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, SUPPORT_TILE, 0),
    };
    return inputWith({
        actorWith(ATTACKER_INDEX, ATTACKER_UNIT, std::move(attacker)),
        actorWith(SUPPORT_INDEX, SUPPORT_UNIT, std::move(support)),
    });
}

void testPairSearchResolvesADestinationConflict()
{
    const AssignmentResult result = MaximumValueAssignment::assign(destinationSwapInput());
    const PlannedAction* pAttacker = actionOf(result, ATTACKER_UNIT);
    const PlannedAction* pSupport = actionOf(result, SUPPORT_UNIT);
    expect(pAttacker != nullptr && pAttacker->destination == SPARE_TILE, "the first actor steps aside");
    expect(pSupport != nullptr && pSupport->destination == CONTESTED_TILE, "the second actor takes the tile");
    expect(plannedTotal(result) == GOOD_VALUE + FAIR_VALUE, "the pair reaches the better total");
    expect(result.stats.settlingMoves == 0, "settling alone cannot reach the pair improvement");
    expect(result.stats.swapImprovements == 1 && result.stats.swapStates > 0, "the exact pair search is reported");
}

bool isExpectedSwap(const AssignmentResult & result)
{
    const PlannedAction* pAttacker =
        actionOf(result, ATTACKER_UNIT);
    const PlannedAction* pSupport =
        actionOf(result, SUPPORT_UNIT);
    return
        pAttacker != nullptr &&
        pAttacker->destination == SPARE_TILE &&
        pSupport != nullptr &&
        pSupport->destination == CONTESTED_TILE &&
        plannedTotal(result) == GOOD_VALUE + FAIR_VALUE;
}

void testPairIntervalsPreserveExactAssignment()
{
    PairIntervalValuer valuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    AssignmentInput input = destinationSwapInput();
    input.pStockValuer = &valuer;
    const AssignmentResult result =
        MaximumValueAssignment::assign(input);
    expect(isExpectedSwap(result),
           "exact pair intervals preserve the scalar assignment");
    expect(valuer.liveLeaves > 0 &&
               valuer.nonLeafQuotes >= 2,
           "pair search prices leaves and replays its winner");
    expect(valuer.refinementBoundaries > 0 &&
               !valuer.wrongRefinementPhase,
           "refinement runs only between swap sweeps");
    expect(result.plan.actionCount() == 2 &&
               result.executionOrder.size() == 2,
           "the retained pair winner remains replayable");
    expect(result.stats.swapImprovements == 1 &&
               result.stats.replayFailures == 0,
           "the live pair improvement lands without replay failure");
    expect(result.stats.clustersTotal == 1 &&
               result.stats.clustersSkippedStockBudget == 1 &&
               result.stats.clustersCapped == 1,
           "the later stock component is explicitly skipped");
    expect(result.stats.clustersEnumerated == 0 &&
               result.stats.enumerationStates == 0,
           "the stock component does not enter cluster enumeration");
    expect(valuer.scalarCallsAfterLiveQuote == 0,
           "cluster bypass makes no scalar stock call after live pair pricing");
}

void testPairIntervalFailuresFallBackExactly()
{
    PairIntervalValuer exact(IntervalFailure::None);
    AssignmentInput exactInput = destinationSwapInput();
    exactInput.pStockValuer = &exact;
    MaximumValueAssignment::assign(exactInput);

    std::int32_t exactFallbacks = 0;
    const std::vector<IntervalFailure> failures{
        IntervalFailure::IncumbentWitness,
        IntervalFailure::LeafQuote,
        IntervalFailure::ReplayQuote,
    };
    for (const IntervalFailure failure : failures)
    {
        PairIntervalValuer valuer(failure);
        AssignmentInput input = destinationSwapInput();
        input.pStockValuer = &valuer;
        const AssignmentResult result =
            MaximumValueAssignment::assign(input);
        expect(isExpectedSwap(result),
               "live failure preserves the scalar winner");
        if (valuer.scalarCalls > exact.scalarCalls)
        {
            ++exactFallbacks;
        }
        if (failure == IntervalFailure::ReplayQuote)
        {
            expect(result.stats.replayFailures > 0,
                   "winner replay mismatch is detected");
        }
    }
    expect(exactFallbacks ==
               static_cast<std::int32_t>(failures.size()),
           "each live failure performs an exact scalar fallback");
}

void testPairAssignmentReplaysDeterministically()
{
    const AssignmentResult first = MaximumValueAssignment::assign(destinationSwapInput());
    const AssignmentResult second = MaximumValueAssignment::assign(destinationSwapInput());
    expect(first.executionOrder == second.executionOrder, "execution order replays");
    bool same = first.plan.actionCount() == second.plan.actionCount();
    for (std::int32_t index = 0; index < first.plan.actionCount() && same; ++index)
    {
        same = first.plan.action(index).unitId == second.plan.action(index).unitId &&
               first.plan.action(index).destination == second.plan.action(index).destination &&
               first.plan.action(index).state == second.plan.action(index).state;
    }
    expect(same, "pair assignments replay");
}

void testTrimmedFireValueUsesOnlyGrantedDamage()
{
    constexpr FireFacts fire{
        .targetUnitId = TARGET_INDEX,
        .targetReplacementCost = 10000,
        .targetHpSteps = 3,
        .damageSteps = 3,
        .counterSteps = 0,
        .targetBestShotBefore = 900,
        .targetBestShotAfter = 0,
    };
    constexpr MilliFunds staticValue = 3900;
    expect(Coordinator::grantedFireValue(fire, staticValue, 2) == 2600,
           "trimmed fire keeps granted capital and prorated credit");
}

AssignmentInput threeActorClusterInput()
{
    std::vector<CandidateBundle> attacker{
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, CONTESTED_TILE, BEST_VALUE),
        valuedCandidate(ATTACKER_INDEX, ATTACKER_TILE, SPARE_TILE, GOOD_VALUE),
    };
    std::vector<CandidateBundle> support{
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, CONTESTED_TILE, CLUSTER_MIDDLE_VALUE),
        valuedCandidate(SUPPORT_INDEX, SUPPORT_TILE, FAR_TILE, FAIR_VALUE),
    };
    std::vector<CandidateBundle> tail{
        valuedCandidate(TARGET_INDEX, TARGET_TILE, FAR_TILE, CLUSTER_TAIL_VALUE),
    };
    return inputWith({
        actorWith(ATTACKER_INDEX, ATTACKER_UNIT, std::move(attacker)),
        actorWith(SUPPORT_INDEX, SUPPORT_UNIT, std::move(support)),
        actorWith(TARGET_INDEX, TARGET_UNIT, std::move(tail)),
    });
}

void testClusterSearchFindsTheThreeActorImprovement()
{
    const AssignmentResult result = MaximumValueAssignment::assign(threeActorClusterInput());
    const PlannedAction* pAttacker = actionOf(result, ATTACKER_UNIT);
    const PlannedAction* pSupport = actionOf(result, SUPPORT_UNIT);
    const PlannedAction* pTail = actionOf(result, TARGET_UNIT);
    expect(result.stats.settlingMoves == 0, "settling cannot unlock the cluster");
    expect(result.stats.swapImprovements == 0, "no pair improves the settled total");
    expect(CLUSTER_OPTIMUM > CLUSTER_SETTLED, "the fixture has a strict cluster gain");
    expect(plannedTotal(result) == CLUSTER_OPTIMUM, "cluster search reaches the exact optimum");
    expect(pAttacker != nullptr && pAttacker->destination == SPARE_TILE,
           "the first actor releases the shared tile");
    expect(pSupport != nullptr && pSupport->destination == CONTESTED_TILE,
           "the middle actor takes the shared tile");
    expect(pTail != nullptr && pTail->destination == FAR_TILE,
           "the tail actor takes the released middle tile");
    expect(result.stats.clustersTotal == 1 && result.stats.clustersEnumerated == 1,
           "the connected component is enumerated once");
    expect(result.stats.clustersCapped == 0 &&
               result.stats.clustersSkippedStockBudget == 0 &&
               result.stats.enumerationStates > 0,
           "the small component completes inside its caps");
}

void testMixedStockClusterKeepsTheSettledPlan()
{
    PairIntervalValuer valuer(
        IntervalFailure::None,
        TARGET_UNIT);
    AssignmentInput input = threeActorClusterInput();
    input.pStockValuer = &valuer;
    const AssignmentResult result =
        MaximumValueAssignment::assign(input);
    const PlannedAction* pAttacker =
        actionOf(result, ATTACKER_UNIT);
    const PlannedAction* pSupport =
        actionOf(result, SUPPORT_UNIT);
    const PlannedAction* pTail =
        actionOf(result, TARGET_UNIT);
    expect(result.stats.settlingMoves == 0 &&
               result.stats.swapImprovements == 0,
           "settling and pair search retain the mixed incumbent");
    expect(result.plan.actionCount() == 3 &&
               result.executionOrder.size() == 2,
           "the pre-cluster action set and live order are retained");
    expect(pAttacker != nullptr &&
               pAttacker->destination == CONTESTED_TILE &&
               pAttacker->marginalValue.economicValue == BEST_VALUE &&
               pAttacker->state == PlanActionState::Pending,
           "the settled attacker seat is retained");
    expect(pSupport != nullptr &&
               pSupport->destination == FAR_TILE &&
               pSupport->marginalValue.economicValue == FAIR_VALUE &&
               pSupport->state == PlanActionState::Pending,
           "the settled support seat is retained");
    expect(pTail != nullptr &&
               pTail->state == PlanActionState::Abandoned,
           "the blocked mixed-component tail remains unassigned");
    expect(plannedTotal(result) == CLUSTER_SETTLED &&
               result.stats.assignedUnits == 2 &&
               result.stats.unassignedUnits == 1,
           "the exact settled value and assignment counts are retained");
    expect(result.plan.destinationClaimant(CONTESTED_TILE) ==
               pAttacker->actionIndex &&
               result.plan.destinationClaimant(FAR_TILE) ==
                   pSupport->actionIndex &&
               result.stats.replayFailures == 0,
           "the retained result keeps replayable destination claims");
    expect(result.stats.clustersTotal == 1 &&
               result.stats.clustersSkippedStockBudget == 1 &&
               result.stats.clustersCapped == 1,
           "one stock member skips the whole mixed component");
    expect(result.stats.clustersEnumerated == 0 &&
               result.stats.enumerationStates == 0 &&
               valuer.scalarCallsAfterLiveQuote == 0,
           "the mixed component performs no scalar cluster search");
}

AssignmentInput oversizedComponentInput()
{
    constexpr std::int32_t actorCount = MaximumValueAssignment::CLUSTER_ACTOR_CAP + 1;
    constexpr std::int32_t firstUnit = 100;
    constexpr MilliFunds privateValue = 100;
    std::vector<AssignmentActor> actors;
    for (std::int32_t slot = 0; slot < actorCount; ++slot)
    {
        const TilePoint origin{0, slot};
        const TilePoint privateTile{5, slot};
        std::vector<CandidateBundle> candidates{
            valuedCandidate(slot, origin, CONTESTED_TILE, BEST_VALUE - slot * 10),
            valuedCandidate(slot, origin, privateTile, privateValue),
        };
        actors.push_back(actorWith(slot, firstUnit + slot, std::move(candidates)));
    }
    return inputWith(std::move(actors));
}

void testOversizedComponentKeepsItsSettledPlan()
{
    constexpr std::int32_t actorCount = MaximumValueAssignment::CLUSTER_ACTOR_CAP + 1;
    constexpr MilliFunds expectedTotal = BEST_VALUE + (actorCount - 1) * POOR_VALUE;
    PairIntervalValuer valuer(IntervalFailure::None);
    AssignmentInput input = oversizedComponentInput();
    input.pStockValuer = &valuer;
    const AssignmentResult result =
        MaximumValueAssignment::assign(input);
    expect(result.stats.clustersTotal == 1, "the overlap graph has one component");
    expect(result.stats.clustersCapped == 1 &&
               result.stats.clustersEnumerated == 0 &&
               result.stats.clustersSkippedStockBudget == 0,
           "the oversized component is not enumerated");
    expect(plannedTotal(result) == expectedTotal, "the cap preserves the settled plan");
    expect(result.stats.assignedUnits == actorCount, "every capped actor keeps a feasible action");
    expect(valuer.scalarCallsAfterLiveQuote == 0,
           "the actor cap takes precedence over stock classification");
}

bool samePlan(const AssignmentResult & left, const AssignmentResult & right)
{
    if (left.executionOrder != right.executionOrder ||
        left.plan.actionCount() != right.plan.actionCount())
    {
        return false;
    }
    for (std::int32_t index = 0; index < left.plan.actionCount(); ++index)
    {
        const PlannedAction & leftAction = left.plan.action(index);
        const PlannedAction & rightAction = right.plan.action(index);
        if (leftAction.actionIndex != rightAction.actionIndex ||
            leftAction.unitId != rightAction.unitId ||
            leftAction.kind != rightAction.kind ||
            leftAction.actionId != rightAction.actionId ||
            leftAction.path != rightAction.path ||
            leftAction.destination != rightAction.destination ||
            leftAction.target != rightAction.target ||
            leftAction.targetUnitId != rightAction.targetUnitId ||
            leftAction.marginalValue != rightAction.marginalValue ||
            leftAction.plannedDamageSteps != rightAction.plannedDamageSteps ||
            leftAction.state != rightAction.state)
        {
            return false;
        }
    }
    return true;
}

std::vector<std::int32_t> statsSnapshot(
    const AssignmentStats & stats)
{
    return {
        stats.assignedUnits,
        stats.unassignedUnits,
        stats.candidates,
        stats.unsupportedCandidates,
        stats.conflicts,
        stats.overkillRejections,
        stats.staleRejections,
        stats.invalidActions,
        stats.vacateConflicts,
        stats.unorderedActions,
        stats.settlingSweeps,
        stats.settlingMoves,
        stats.swapImprovements,
        stats.clustersTotal,
        stats.clustersEnumerated,
        stats.clustersCapped,
        stats.clustersSkippedStockBudget,
        stats.enumerationStates,
        stats.swapStates,
        stats.replayFailures,
    };
}

void testStockPairBypassReplaysDeterministically()
{
    PairIntervalValuer firstValuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    AssignmentInput firstInput = destinationSwapInput();
    firstInput.pStockValuer = &firstValuer;
    const AssignmentResult first =
        MaximumValueAssignment::assign(firstInput);

    PairIntervalValuer replayValuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    AssignmentInput replayInput = destinationSwapInput();
    replayInput.pStockValuer = &replayValuer;
    const AssignmentResult replay =
        MaximumValueAssignment::assign(replayInput);

    expect(samePlan(first, replay),
           "stock bypass replays the exact actions, order, and values");
    expect(statsSnapshot(first.stats) ==
               statsSnapshot(replay.stats),
           "stock bypass repeats every assignment statistic");
    expect(plannedTotal(first) == GOOD_VALUE + FAIR_VALUE &&
               plannedTotal(first) == plannedTotal(replay),
           "stock bypass repeats the selected economic value");
    expect(firstValuer.scalarCalls ==
               replayValuer.scalarCalls &&
               firstValuer.scalarCallsAfterLiveQuote == 0 &&
               replayValuer.scalarCallsAfterLiveQuote == 0,
           "stock bypass repeats pricing without cluster scalar calls");
}

void testClusterSearchReplaysDeterministically()
{
    const AssignmentResult first = MaximumValueAssignment::assign(threeActorClusterInput());
    const AssignmentResult replay = MaximumValueAssignment::assign(threeActorClusterInput());
    expect(samePlan(first, replay), "cluster assignment replays exactly");
    expect(first.stats.enumerationStates == replay.stats.enumerationStates,
           "cluster traversal visits the same states");
    expect(first.stats.replayFailures == 0 && replay.stats.replayFailures == 0,
           "both winning searches reproduce their selected seats");
}

void testOneStockActorDoesNotCountAsAClusterSkip()
{
    PairIntervalValuer valuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    AssignmentInput input = destinationSwapInput();
    input.actors.resize(1);
    input.pStockValuer = &valuer;
    const AssignmentResult result =
        MaximumValueAssignment::assign(input);
    expect(result.stats.clustersTotal == 0 &&
               result.stats.clustersEnumerated == 0 &&
               result.stats.clustersCapped == 0 &&
               result.stats.clustersSkippedStockBudget == 0 &&
               result.stats.enumerationStates == 0,
           "a one-actor component is not a skipped cluster");
    expect(result.stats.assignedUnits == 1 &&
               plannedTotal(result) == BEST_VALUE,
           "the single stock actor keeps its exact selected action");
}

AssignmentInput sharedBudgetInput()
{
    constexpr std::int32_t actorCount = 5;
    constexpr std::int32_t candidatesPerActor = 150;
    constexpr std::int32_t firstUnit = 100;
    constexpr std::int32_t privateTileOffset = 10;
    std::vector<AssignmentActor> actors;
    actors.reserve(actorCount);
    for (std::int32_t slot = 0; slot < actorCount; ++slot)
    {
        const TilePoint origin{slot, 0};
        std::vector<CandidateBundle> candidates;
        candidates.reserve(candidatesPerActor);
        for (std::int32_t option = 0;
             option < candidatesPerActor;
             ++option)
        {
            candidates.push_back(
                valuedCandidate(
                    slot,
                    origin,
                    TilePoint{
                        slot + privateTileOffset,
                        option + privateTileOffset},
                    BEST_VALUE));
        }
        actors.push_back(
            actorWith(
                slot,
                firstUnit + slot,
                std::move(candidates)));
    }
    return inputWith(std::move(actors));
}

void testSharedBudgetCapPrecedesStockClassification()
{
    BudgetExhaustingValuer valuer;
    AssignmentInput input = sharedBudgetInput();
    input.pStockValuer = &valuer;
    const AssignmentResult result =
        MaximumValueAssignment::assign(input);
    expect(result.stats.swapStates ==
               Coordinator::ASSIGNMENT_STATE_BUDGET &&
               Coordinator::searchBudgetExhausted(result.stats),
           "pair searches exhaust the shared assignment budget");
    expect(result.stats.clustersTotal == 1 &&
               result.stats.clustersCapped == 1 &&
               result.stats.clustersEnumerated == 0,
           "the exhausted component uses the existing capped path");
    expect(result.stats.clustersSkippedStockBudget == 0 &&
               result.stats.enumerationStates == 0,
           "shared-budget exhaustion is not double-counted as a stock skip");
}

void testCapsAndSharedBudgetBoundTheSearch()
{
    const std::vector<std::int32_t> options{0, 1, 2, NO_CANDIDATE, 3};
    expect(Coordinator::cappedOptions(options, 2) ==
               std::vector<std::int32_t>({0, 1, NO_CANDIDATE}),
           "candidate capping preserves the unassigned option");
    expect(Coordinator::cappedOptions(options, Coordinator::NO_CANDIDATE_CAP) == options,
           "the uncapped sentinel preserves every option");

    AssignmentStats stats;
    stats.swapStates = Coordinator::ASSIGNMENT_STATE_BUDGET - 1;
    expect(!Coordinator::searchBudgetExhausted(stats),
           "the final shared-budget state remains available");
    stats.enumerationStates = 1;
    expect(Coordinator::searchBudgetExhausted(stats),
           "pair and cluster visits share one hard budget");
}

void testDecisionTraceIsNeutralAndDeterministic()
{
    PairIntervalValuer baselineValuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    AssignmentInput baselineInput =
        destinationSwapInput();
    baselineInput.pStockValuer =
        &baselineValuer;
    const AssignmentResult baseline =
        MaximumValueAssignment::assign(
            baselineInput);

    PairIntervalValuer tracedValuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    RecordingTrace firstTrace;
    AssignmentInput tracedInput =
        destinationSwapInput();
    tracedInput.pStockValuer =
        &tracedValuer;
    tracedInput.pTrace = &firstTrace;
    const AssignmentResult traced =
        MaximumValueAssignment::assign(
            tracedInput);

    PairIntervalValuer replayValuer(
        IntervalFailure::None,
        ATTACKER_UNIT);
    RecordingTrace replayTrace;
    AssignmentInput replayInput =
        destinationSwapInput();
    replayInput.pStockValuer =
        &replayValuer;
    replayInput.pTrace = &replayTrace;
    const AssignmentResult replay =
        MaximumValueAssignment::assign(
            replayInput);

    expect(samePlan(baseline, traced) &&
               samePlan(traced, replay),
           "decision tracing preserves every planned action and order");
    expect(statsSnapshot(baseline.stats) ==
               statsSnapshot(traced.stats) &&
               statsSnapshot(traced.stats) ==
                   statsSnapshot(replay.stats),
           "decision tracing preserves every assignment statistic");
    expect(firstTrace.records == replayTrace.records,
           "decision tracing repeats every structural record exactly");
    expect(firstTrace.contains(
               QStringLiteral("GREEDY_PICK")) &&
               firstTrace.contains(
                   QStringLiteral("GREEDY")) &&
               firstTrace.contains(
                   QStringLiteral("SETTLING_CHALLENGER")) &&
               firstTrace.contains(
                   QStringLiteral("PAIR_RESULT")) &&
               firstTrace.contains(
                   QStringLiteral("FINAL_PLAN")) &&
               firstTrace.contains(
                   QStringLiteral("FINAL_STATS")),
           "decision tracing covers assignment decision phases");
    expect(traced.selections.size() ==
               tracedInput.actors.size(),
           "decision tracing retains one final selection receipt per actor");
}
}

int main()
{
    testConversionMapsEngineActionsAndTargets();
    testDestinationConflictRollsBackTheRefusedAction();
    testAttackAndCaptureClaimsRejectOverbooking();
    testCandidatePreparationIsStableAndIncludesGivingUp();
    testSeatBestClaimableFallsThroughAConflict();
    testReplanRevivesAndCanAbandonAnAction();
    testActorResourcesExposeTileAndTargetConflicts();
    testSettlingDropsALosingFallback();
    testPairSearchResolvesADestinationConflict();
    testPairIntervalsPreserveExactAssignment();
    testPairIntervalFailuresFallBackExactly();
    testPairAssignmentReplaysDeterministically();
    testTrimmedFireValueUsesOnlyGrantedDamage();
    testClusterSearchFindsTheThreeActorImprovement();
    testMixedStockClusterKeepsTheSettledPlan();
    testOversizedComponentKeepsItsSettledPlan();
    testStockPairBypassReplaysDeterministically();
    testClusterSearchReplaysDeterministically();
    testOneStockActorDoesNotCountAsAClusterSkip();
    testSharedBudgetCapPrecedesStockClassification();
    testCapsAndSharedBudgetBoundTheSearch();
    testDecisionTraceIsNeutralAndDeterministic();
    return failures == 0 ? 0 : 1;
}
