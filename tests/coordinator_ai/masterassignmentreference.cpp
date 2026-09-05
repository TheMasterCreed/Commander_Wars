#include "tests/coordinator_ai/masterassignmentoracle.h"

#include <algorithm>
#include <numeric>

#include "ai/coordinator/assignmentvaluation.h"

namespace
{
    using namespace MasterAssignmentOracle;

    bool frozen(const MasterAssignmentProblem & problem, const MasterActor & actor)
    {
        return actor.role == MasterActorRole::Fixed ||
               (actor.actionIndex != NO_ACTION &&
                problem.actionTemplates()[actor.actionIndex].state == PlanActionState::Committed);
    }

    bool incumbentTuple(const MasterAssignmentProblem & problem, std::span<const std::int32_t> choices)
    {
        return std::equal(choices.begin(), choices.end(),
                          problem.incumbentChoices().begin(), problem.incumbentChoices().end());
    }

    TupleStatus assembleAction(const MasterAssignmentProblem & problem, const MasterActor & actor,
                               const CandidateBundle & candidate, PlannedAction & action)
    {
        const auto & bundle = candidate.bundle;
        if (bundle.components.size() > 1)
        {
            return TupleStatus::Unsupported;
        }
        action.unitId = actor.engineUnitId;
        action.path = bundle.path;
        action.destination = bundle.destination;
        action.kind = planBundleKindOf(bundle);
        std::int32_t fullDamage = 0;
        action.actionId = problem.actionIds().wait;
        if (!bundle.components.empty())
        {
            const auto & component = bundle.components.front();
            if (component.kind == ComponentKind::Service)
            {
                return TupleStatus::Unsupported;
            }
            if (component.kind == ComponentKind::Capture)
            {
                action.actionId = problem.actionIds().capture;
                action.target = bundle.destination;
            }
            else if (component.kind == ComponentKind::Fire)
            {
                const auto target = component.fire.targetUnitId;
                if (target < 0 || static_cast<std::size_t>(target) >= problem.unitLinks().size())
                {
                    return TupleStatus::InvalidInput;
                }
                action.actionId = problem.actionIds().fire;
                action.target = problem.unitLinks()[target].tile;
                action.targetUnitId = problem.unitLinks()[target].engineUnitId;
                fullDamage = std::min(component.fire.damageSteps, component.fire.targetHpSteps);
            }
            else
            {
                return TupleStatus::Unsupported;
            }
        }
        if (action.actionId.isEmpty())
        {
            return TupleStatus::Unsupported;
        }
        if (!checkedSeatedCandidateValue(candidate, fullDamage))
        {
            return TupleStatus::ArithmeticFailure;
        }
        action.marginalValue = candidate.valuation.value();
        return TupleStatus::Feasible;
    }

    bool sameIntent(const PlannedAction & left, const PlannedAction & right)
    {
        return left.kind == right.kind && left.actionId == right.actionId && left.path == right.path &&
               left.destination == right.destination && left.target == right.target &&
               left.targetUnitId == right.targetUnitId;
    }

    std::vector<std::pair<std::int32_t, std::int32_t>> liveVacates(const TurnPlan & plan)
    {
        std::vector<std::pair<std::int32_t, std::int32_t>> result;
        for (std::int32_t index = 0; index < plan.edgeCount(); ++index)
        {
            const auto & edge = plan.edge(index);
            if (edge.kind == PlanEdgeKind::Vacate && isLiveState(plan.action(edge.from).state) &&
                isLiveState(plan.action(edge.to).state))
            {
                result.emplace_back(edge.from, edge.to);
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    TupleStatus applyDependencies(const MasterAssignmentProblem & problem, bool incumbent,
                                  TurnPlan & plan, bool & executable)
    {
        for (const auto & edge : problem.incumbentEdges())
        {
            if (edge.kind == PlanEdgeKind::Vacate && !incumbent)
            {
                continue;
            }
            if (!plan.hasAction(edge.from) || !plan.hasAction(edge.to))
            {
                return TupleStatus::InvalidInput;
            }
            const auto & prerequisite = plan.action(edge.from);
            const auto & dependent = plan.action(edge.to);
            if (isLiveState(dependent.state))
            {
                if (!isLiveState(prerequisite.state))
                {
                    executable = false;
                }
                else if (edge.kind == PlanEdgeKind::Wallbreak)
                {
                    if (prerequisite.targetUnitId != problem.actionTemplates()[edge.from].targetUnitId ||
                        !plan.targetIsLethal(prerequisite.targetUnitId))
                    {
                        return TupleStatus::Infeasible;
                    }
                }
                else if (edge.kind != PlanEdgeKind::Vacate && !incumbent &&
                         (!sameIntent(prerequisite, problem.actionTemplates()[edge.from]) ||
                          !sameIntent(dependent, problem.actionTemplates()[edge.to])))
                {
                    return TupleStatus::Unsupported;
                }
            }
            if (!plan.addEdge(edge.from, edge.to, edge.kind))
            {
                return TupleStatus::InvalidInput;
            }
        }
        if (incumbent)
        {
            TurnPlan validation = plan;
            if (!validation.rebuildVacateEdges(problem.occupancy()).empty() ||
                liveVacates(validation) != liveVacates(plan))
            {
                return TupleStatus::Infeasible;
            }
        }
        else if (!plan.rebuildVacateEdges(problem.occupancy()).empty())
        {
            return TupleStatus::Infeasible;
        }
        return TupleStatus::Feasible;
    }
}

namespace MasterAssignmentOracle
{
    ReferencePlan constructReference(
        const MasterAssignmentProblem & problem, std::span<const std::int32_t> choices)
    {
        ReferencePlan result;
        if (choices.size() != problem.actors().size())
        {
            result.reason = QStringLiteral("choice count");
            return result;
        }
        const bool incumbent = incumbentTuple(problem, choices);
        const auto actors = problem.actors();
        std::vector<PlannedAction> actions(problem.actionTemplates().begin(), problem.actionTemplates().end());
        result.actionIndices.resize(actors.size(), NO_ACTION);
        result.seatedValues.resize(actors.size(), 0);
        std::vector<std::int32_t> damage(actors.size(), 0);
        for (std::size_t slot = 0; slot < actors.size(); ++slot)
        {
            const auto & actor = actors[slot];
            const auto choice = choices[slot];
            if (choice < NO_CANDIDATE || choice >= static_cast<std::int32_t>(actor.candidates.size()) ||
                (choice == NO_CANDIDATE && actor.incumbentCandidateIndex != NO_CANDIDATE) ||
                (frozen(problem, actor) && choice != actor.incumbentCandidateIndex))
            {
                result.status = TupleStatus::InvalidInput;
                result.reason = QStringLiteral("invalid or changed fixed choice");
                return result;
            }
            result.actionIndices[slot] = actor.actionIndex;
            if (choice == NO_CANDIDATE)
            {
                if (actor.actionIndex != NO_ACTION)
                {
                    actions[actor.actionIndex].plannedDamageSteps = 0;
                }
                continue;
            }
            const auto & candidate = actor.candidates[choice].source;
            PlannedAction action;
            result.status = assembleAction(problem, actor, candidate, action);
            if (result.status != TupleStatus::Feasible)
            {
                result.reason = QStringLiteral("action semantics");
                return result;
            }
            if (actor.actionIndex != NO_ACTION)
            {
                if (incumbent || frozen(problem, actor))
                {
                    action.state = actions[actor.actionIndex].state;
                }
                actions[actor.actionIndex] = std::move(action);
            }
            else
            {
                result.actionIndices[slot] = static_cast<std::int32_t>(actions.size());
                actions.push_back(std::move(action));
            }
            if (!candidate.bundle.components.empty() &&
                candidate.bundle.components.front().kind == ComponentKind::Fire)
            {
                damage[slot] = incumbent || frozen(problem, actor)
                    ? actor.incumbentDamageSteps : candidate.bundle.components.front().fire.damageSteps;
            }
        }
        for (auto & action : actions)
        {
            if (result.plan.addAction(std::move(action)) == NO_ACTION)
            {
                result.status = TupleStatus::Infeasible;
                result.reason = QStringLiteral("native action construction");
                return result;
            }
        }
        std::vector<std::size_t> actorOrder(actors.size());
        std::iota(actorOrder.begin(), actorOrder.end(), 0);
        std::sort(actorOrder.begin(), actorOrder.end(), [&](auto left, auto right)
        {
            return result.actionIndices[left] < result.actionIndices[right];
        });
        // Native attack claims reject even zero additions after saturation.
        for (int pass = 0; pass < 3; ++pass)
        {
            for (const auto slot : actorOrder)
            {
                const auto index = result.actionIndices[slot];
                if (choices[slot] == NO_CANDIDATE || index == NO_ACTION ||
                    !isLiveState(result.plan.action(index).state))
                {
                    continue;
                }
                const int claimPass = damage[slot] == 0 ? 0 : frozen(problem, actors[slot]) ? 1 : 2;
                if (pass != claimPass)
                {
                    continue;
                }
                const auto & action = result.plan.action(index);
                auto claimed = result.plan.claimDestination(index, action.destination);
                const auto & components = actors[slot].candidates[choices[slot]].source.bundle.components;
                if (claimed == ReservationResult::Granted && !components.empty())
                {
                    const auto & component = components.front();
                    if (component.kind == ComponentKind::Capture)
                    {
                        claimed = result.plan.claimCapture(index, action.destination);
                    }
                    else if (component.kind == ComponentKind::Fire)
                    {
                        claimed = result.plan.claimAttack(
                            index, action.targetUnitId, component.fire.targetHpSteps, damage[slot]).result;
                    }
                }
                if (claimed != ReservationResult::Granted)
                {
                    result.status = TupleStatus::Infeasible;
                    result.reason = QStringLiteral("native reservation rejected");
                    return result;
                }
            }
        }
        bool executable = true;
        result.status = applyDependencies(problem, incumbent, result.plan, executable);
        if (result.status != TupleStatus::Feasible)
        {
            result.reason = QStringLiteral("dependency validation");
            return result;
        }
        for (std::size_t slot = 0; slot < actors.size(); ++slot)
        {
            const auto & actor = actors[slot];
            const auto index = result.actionIndices[slot];
            if (frozen(problem, actor) && actor.actionIndex != NO_ACTION &&
                (result.plan.action(index) != problem.actionTemplates()[actor.actionIndex] ||
                 result.plan.actionReservations(index) != actor.incumbentReservations))
            {
                result.status = TupleStatus::Infeasible;
                result.reason = QStringLiteral("fixed action or reservation changed");
                return result;
            }
            if (index == NO_ACTION || choices[slot] == NO_CANDIDATE ||
                !isLiveState(result.plan.action(index).state))
            {
                executable = false;
                continue;
            }
            const auto & action = result.plan.action(index);
            const auto value = checkedSeatedCandidateValue(
                actor.candidates[choices[slot]].source, action.plannedDamageSteps);
            if (!value || !RelativeStockDetail::checkedAdd(result.seatedTotal, *value))
            {
                result.status = TupleStatus::ArithmeticFailure;
                result.reason = QStringLiteral("checked seated economics");
                return result;
            }
            result.seatedValues[slot] = *value;
        }
        for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
        {
            const auto & action = result.plan.action(index);
            if (!isLiveState(action.state))
            {
                continue;
            }
            const auto actor = std::find_if(actors.begin(), actors.end(), [&](const auto & candidate)
            {
                return candidate.engineUnitId == action.unitId;
            });
            if (actor == actors.end())
            {
                result.status = TupleStatus::InvalidInput;
                result.reason = QStringLiteral("live action without actor");
                return result;
            }
            if (actor->stockRow != NO_STOCK_ROW)
            {
                const bool captures = action.kind == PlanBundleKind::Capture ||
                                      action.kind == PlanBundleKind::MoveAndCapture;
                result.key.push_back(CanonicalPlanActionKey::fromAction(
                    actor->stockRow, action.destination.x, action.destination.y, captures));
            }
        }
        std::sort(result.key.begin(), result.key.end());
        result.ordering = result.plan.executionOrder();
        result.representationValid = true;
        result.complete = executable && result.ordering.cycleMembers.empty() &&
                          result.ordering.blockedByCycle.empty();
        result.status = result.complete ? TupleStatus::Feasible : TupleStatus::Infeasible;
        result.reason = result.complete ? QString() : QStringLiteral("incomplete executable plan");
        return result;
    }
}
