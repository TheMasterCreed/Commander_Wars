#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <QString>

#include "ai/coordinator/assignmentvaluation.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/planidentity.h"
#include "ai/coordinator/relativestockaudit.h"
#include "ai/coordinator/turnplan.h"

#ifdef COW_COORDINATED_REFINEMENT_TESTING
class ArenaRelativeStockSupportAudit;
#endif

namespace Coordinator
{
    // Empty for a kind no single engine action performs.
    inline QString planActionIdFor(const PlanActionIds & actionIds, PlanBundleKind kind)
    {
        switch (kind)
        {
            case PlanBundleKind::Wait:
            case PlanBundleKind::Move:
                return actionIds.wait;
            case PlanBundleKind::Fire:
            case PlanBundleKind::MoveAndFire:
                return actionIds.fire;
            case PlanBundleKind::Capture:
            case PlanBundleKind::MoveAndCapture:
                return actionIds.capture;
            case PlanBundleKind::Service:
            case PlanBundleKind::MoveAndService:
            case PlanBundleKind::Compound:
                break;
        }
        return QString();
    }

    struct AssignmentActor
    {
        std::int32_t knowledgeUnitIndex{NO_UNIT};
        std::int32_t engineUnitId{NO_UNIT};
        std::vector<CandidateBundle> candidates;
    };

    struct AssignmentInput
    {
        PlanActionIds actionIds;
        std::vector<KnownUnitLink> unitLinks;
        std::vector<AssignmentActor> actors;
        PlanStockValuer* pStockValuer{nullptr};
        DecisionTrace* pTrace{nullptr};
        bool measureTimings{false};
        std::int64_t relativeStockWorkLimit{DEFAULT_RELATIVE_STOCK_WORK_LIMIT};
        RelativeStockSchedule relativeStockSchedule{RelativeStockSchedule::Current};
        RelativeStockObserver* pRelativeStockObserver{nullptr};
        bool retainSelections{false};
    };

    inline MilliFunds planStockDelta(const AssignmentInput & input, const TurnPlan & plan)
    {
        if (input.pStockValuer == nullptr)
        {
            return 0;
        }
        return input.pStockValuer->planStock(plan) - input.pStockValuer->originStock();
    }

    inline bool stockCoupled(const AssignmentInput & input, std::int32_t engineUnitId)
    {
        return input.pStockValuer != nullptr && input.pStockValuer->affectsStock(engineUnitId);
    }

    struct AssignmentStats
    {
        std::int32_t assignedUnits{0};
        std::int32_t unassignedUnits{0};
        std::int32_t candidates{0};
        std::int32_t unsupportedCandidates{0};
        std::int32_t conflicts{0};
        std::int32_t overkillRejections{0};
        std::int32_t staleRejections{0};
        std::int32_t invalidActions{0};
        std::int32_t vacateConflicts{0};
        std::int32_t unorderedActions{0};
        std::int32_t settlingSweeps{0};
        std::int32_t settlingMoves{0};
        std::int32_t swapImprovements{0};
        std::int32_t clustersTotal{0};
        std::int32_t clustersEnumerated{0};
        std::int32_t clustersCapped{0};
        std::int32_t clustersSkippedStockBudget{0};
        std::int32_t enumerationStates{0};
        std::int32_t swapStates{0};
        std::int32_t replayFailures{0};
        RelativeStockStats stockPairs;
        LiveRefinementStats mixedPairs;
        RelativeStockStats stockJoints;

        friend bool operator==(const AssignmentStats &, const AssignmentStats &) = default;
    };

    struct AssignmentResult
    {
        enum class SelectionPhase : std::int8_t
        {
            None,
            Greedy,
            Settling,
            PairRefinement,
            Cluster,
        };

        struct CandidateCompleteValue
        {
            MilliFunds value{0};
            bool known{false};
        };

        struct Selection
        {
            std::int32_t knowledgeUnitIndex{NO_UNIT};
            std::int32_t engineUnitId{NO_UNIT};
            std::int32_t actionIndex{NO_ACTION};
            std::int32_t candidateIndex{-1};
            std::int32_t greedyCandidateIndex{-1};
            MilliFunds seatedValue{0};
            bool stockCoupled{false};
            SelectionPhase phase{SelectionPhase::None};
            std::vector<CandidateCompleteValue> completeValues;
        };

        struct PhaseTiming
        {
            std::int64_t prepareNanos{0};
            std::int64_t greedyNanos{0};
            std::int64_t settlingNanos{0};
            std::int64_t pairRefinementNanos{0};
            std::int64_t clusterNanos{0};
            std::int64_t finishPlanNanos{0};
            RelativeStockTiming stockPairs;
            RelativeStockTiming stockJoints;
        };

        TurnPlan plan;
        std::vector<std::int32_t> executionOrder;
        AssignmentStats stats;
        std::vector<Selection> selections;
        PhaseTiming phaseTiming;
    };

    inline QString traceSelectionPhase(AssignmentResult::SelectionPhase phase)
    {
        switch (phase)
        {
            case AssignmentResult::SelectionPhase::None:
                return QStringLiteral("NONE");
            case AssignmentResult::SelectionPhase::Greedy:
                return QStringLiteral("GREEDY");
            case AssignmentResult::SelectionPhase::Settling:
                return QStringLiteral("SETTLING");
            case AssignmentResult::SelectionPhase::PairRefinement:
                return QStringLiteral("PAIR_REFINEMENT");
            case AssignmentResult::SelectionPhase::Cluster:
                return QStringLiteral("CLUSTER");
        }
        return QStringLiteral("UNKNOWN");
    }

    inline KnownUnitLink linkOf(std::span<const KnownUnitLink> links, std::int32_t knowledgeUnitIndex)
    {
        if (knowledgeUnitIndex < 0 || knowledgeUnitIndex >= static_cast<std::int32_t>(links.size()))
        {
            return KnownUnitLink{};
        }
        return links[static_cast<std::size_t>(knowledgeUnitIndex)];
    }

    inline const BundleComponent* singleComponentOf(const ActionBundle & bundle)
    {
        if (bundle.components.size() == 1)
        {
            return &bundle.components.front();
        }
        return nullptr;
    }

    inline PlannedAction plannedActionFrom(const PlanActionIds & actionIds, std::span<const KnownUnitLink> links,
                                           std::int32_t engineUnitId, const CandidateBundle & candidate)
    {
        PlannedAction action;
        action.unitId = engineUnitId;
        action.kind = planBundleKindOf(candidate.bundle);
        action.actionId = planActionIdFor(actionIds, action.kind);
        action.path = candidate.bundle.path;
        action.destination = candidate.bundle.destination;
        action.marginalValue = candidate.valuation.value();
        const BundleComponent* pComponent = singleComponentOf(candidate.bundle);
        if (pComponent == nullptr)
        {
            return action;
        }
        switch (pComponent->kind)
        {
            case ComponentKind::Fire:
            {
                const KnownUnitLink target = linkOf(links, pComponent->fire.targetUnitId);
                action.targetUnitId = target.engineUnitId;
                action.target = target.tile;
                break;
            }
            case ComponentKind::Capture:
                action.target = candidate.bundle.destination;
                break;
            case ComponentKind::Service:
                break;
        }
        return action;
    }

    inline bool isPlannableCandidate(const PlanActionIds & actionIds, std::span<const KnownUnitLink> links,
                                     const CandidateBundle & candidate)
    {
        const PlanBundleKind kind = planBundleKindOf(candidate.bundle);
        if (planActionIdFor(actionIds, kind).isEmpty())
        {
            return false;
        }
        if (kind != PlanBundleKind::Fire && kind != PlanBundleKind::MoveAndFire)
        {
            return true;
        }
        const BundleComponent* pComponent = singleComponentOf(candidate.bundle);
        if (pComponent == nullptr)
        {
            return false;
        }
        const KnownUnitLink target = linkOf(links, pComponent->fire.targetUnitId);
        return target.engineUnitId != NO_UNIT && target.tile != INVALID_TILE;
    }

    inline ReservationResult claimFor(TurnPlan & plan, std::int32_t actionIndex, const CandidateBundle & candidate,
                                      std::optional<std::int32_t> replayDamage = std::nullopt)
    {
        const PlannedAction & action = plan.action(actionIndex);
        const ReservationResult destination = plan.claimDestination(actionIndex, action.destination);
        if (destination != ReservationResult::Granted)
        {
            return destination;
        }
        const BundleComponent* pComponent = singleComponentOf(candidate.bundle);
        if (pComponent == nullptr)
        {
            return ReservationResult::Granted;
        }
        switch (pComponent->kind)
        {
            case ComponentKind::Fire:
                if (replayDamage &&
                    (*replayDamage < 0 || *replayDamage > pComponent->fire.damageSteps))
                {
                    return ReservationResult::Invalid;
                }
                return plan
                    .claimAttack(actionIndex, action.targetUnitId, pComponent->fire.targetHpSteps,
                                  replayDamage.value_or(pComponent->fire.damageSteps))
                    .result;
            case ComponentKind::Capture:
                return plan.claimCapture(actionIndex, action.destination);
            case ComponentKind::Service:
                break;
        }
        return ReservationResult::Invalid;
    }

    inline ReservationResult claimOrRollback(TurnPlan & plan, std::int32_t actionIndex, const PlannedAction & action,
                                             const CandidateBundle & candidate)
    {
        const ReservationResult result = claimFor(plan, actionIndex, candidate);
        if (result != ReservationResult::Granted)
        {
            plan.replan(actionIndex, action);
        }
        return result;
    }

    inline ReservationResult installCandidate(TurnPlan & plan, std::int32_t actionIndex, const PlannedAction & action,
                                              const CandidateBundle & candidate)
    {
        if (!plan.replan(actionIndex, action).accepted)
        {
            return ReservationResult::Invalid;
        }
        return claimOrRollback(plan, actionIndex, action, candidate);
    }

    inline void countRejection(AssignmentStats & stats, ReservationResult result)
    {
        switch (result)
        {
            case ReservationResult::Granted:
                break;
            case ReservationResult::Conflict:
                ++stats.conflicts;
                break;
            case ReservationResult::Overkill:
                ++stats.overkillRejections;
                break;
            case ReservationResult::StaleTarget:
                ++stats.staleRejections;
                break;
            case ReservationResult::Invalid:
                ++stats.invalidActions;
                break;
        }
    }

    struct CandidateValueOrder
    {
        const std::vector<CandidateBundle>* pCandidates{nullptr};

        bool operator()(std::int32_t left, std::int32_t right) const
        {
            const std::vector<CandidateBundle> & candidates = *pCandidates;
            const CandidateBundle & leftCandidate =
                candidates[static_cast<std::size_t>(left)];
            const CandidateBundle & rightCandidate =
                candidates[static_cast<std::size_t>(right)];
            const MilliFunds leftValue =
                leftCandidate.valuation.value().economicValue;
            const MilliFunds rightValue =
                rightCandidate.valuation.value().economicValue;
            if (leftValue != rightValue)
            {
                return leftValue > rightValue;
            }
            return leftCandidate.vacatesActiveFriendlyProduction &&
                   !rightCandidate.vacatesActiveFriendlyProduction;
        }
    };

    inline std::vector<std::int32_t> plannableCandidateOrder(const PlanActionIds & actionIds,
                                                            std::span<const KnownUnitLink> links,
                                                            const std::vector<CandidateBundle> & candidates,
                                                            AssignmentStats & stats)
    {
        std::vector<std::int32_t> order;
        order.reserve(candidates.size());
        for (std::size_t slot = 0; slot < candidates.size(); ++slot)
        {
            ++stats.candidates;
            if (!isPlannableCandidate(actionIds, links, candidates[slot]))
            {
                ++stats.unsupportedCandidates;
                continue;
            }
            order.push_back(static_cast<std::int32_t>(slot));
        }
        std::stable_sort(order.begin(), order.end(), CandidateValueOrder{&candidates});
        return order;
    }

    inline bool replanAction(TurnPlan & plan, std::int32_t actionIndex, const PlanActionIds & actionIds,
                             std::span<const KnownUnitLink> links, const std::vector<CandidateBundle> & candidates,
                             AssignmentStats & stats, DecisionTrace* pTrace = nullptr,
                             std::int32_t* pSelectedCandidate = nullptr)
    {
        const std::vector<std::int32_t> order = plannableCandidateOrder(actionIds, links, candidates, stats);
        const std::int32_t engineUnitId = plan.action(actionIndex).unitId;
        for (const std::int32_t slot : order)
        {
            const CandidateBundle & candidate = candidates[static_cast<std::size_t>(slot)];
            const PlannedAction action = plannedActionFrom(actionIds, links, engineUnitId, candidate);
            const ReservationResult claim = installCandidate(plan, actionIndex, action, candidate);
            if (pTrace != nullptr)
            {
                pTrace->record(
                    QStringLiteral("REPLAN_CANDIDATE"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 candidate=%3 generated=%4 kind=%5 destination=%6 claim=%7 selected=%8")
                        .arg(engineUnitId)
                        .arg(actionIndex)
                        .arg(slot)
                        .arg(candidate.generationIndex)
                        .arg(traceBundleKind(
                            planBundleKindOf(candidate.bundle)))
                        .arg(traceTile(
                            candidate.bundle.destination))
                        .arg(traceReservationResult(claim))
                        .arg(traceBool(
                            claim == ReservationResult::Granted)));
            }
            if (claim == ReservationResult::Granted)
            {
                if (pSelectedCandidate != nullptr)
                {
                    *pSelectedCandidate = slot;
                }
                return true;
            }
            countRejection(stats, claim);
        }
        if (pSelectedCandidate != nullptr)
        {
            *pSelectedCandidate = -1;
        }
        plan.markFailed(actionIndex);
        return false;
    }

    inline std::vector<OccupancyEntry> planOccupancy(const TurnPlan & plan)
    {
        std::vector<OccupancyEntry> occupancy;
        for (std::int32_t index = 0; index < plan.actionCount(); ++index)
        {
            const PlannedAction & action = plan.action(index);
            if (isLiveState(action.state))
            {
                occupancy.push_back(OccupancyEntry{
                    action.unitId,
                    action.path.empty() ? INVALID_TILE : action.path.front(),
                });
            }
        }
        return occupancy;
    }

    constexpr std::int32_t NO_CANDIDATE_CAP = -1;
    constexpr std::int32_t ASSIGNMENT_STATE_BUDGET = 200000;

    inline bool searchBudgetExhausted(const AssignmentStats & stats, std::int32_t pendingStates = 0)
    {
        return stats.swapStates + stats.enumerationStates + pendingStates >= ASSIGNMENT_STATE_BUDGET;
    }

    struct ActorProgress
    {
        const AssignmentActor* pActor{nullptr};
        std::vector<std::int32_t> order;
        std::vector<std::int32_t> options;
        std::size_t cursor{0};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t chosen{NO_CANDIDATE};
        std::int32_t greedyChosen{NO_CANDIDATE};
        MilliFunds seatedValue{0};
        AssignmentResult::SelectionPhase phase{
            AssignmentResult::SelectionPhase::None};
        std::vector<AssignmentResult::CandidateCompleteValue> completeValues;
        bool done{false};

        const CandidateBundle & candidate() const
        {
            return pActor->candidates[static_cast<std::size_t>(order[cursor])];
        }

        MilliFunds economicValue() const
        {
            return candidate().valuation.value().economicValue;
        }
    };

    inline void invalidateCompleteValues(
        std::vector<ActorProgress> & actors)
    {
        for (ActorProgress & state : actors)
        {
            for (AssignmentResult::CandidateCompleteValue & value :
                 state.completeValues)
            {
                value.known = false;
            }
        }
    }

    inline MilliFunds optionValue(const ActorProgress & state, std::int32_t option)
    {
        if (option == NO_CANDIDATE)
        {
            return 0;
        }
        return state.pActor->candidates[static_cast<std::size_t>(option)].valuation.value().economicValue;
    }

    inline MilliFunds assignmentUpperBound(MilliFunds seatedValue, MilliFunds optionValue,
                                           MilliFunds remainingCeiling)
    {
        return seatedValue + optionValue + remainingCeiling;
    }

    inline MilliFunds assignmentUpperBound(MilliFunds seatedBase, MilliFunds optionBase,
                                           MilliFunds remainingBaseCeiling, MilliFunds propertyCeiling,
                                           MilliFunds propertyAtStart)
    {
        return seatedBase + optionBase + remainingBaseCeiling + propertyCeiling - propertyAtStart;
    }

    inline bool isPruneOrderLicensed(std::span<const MilliFunds> orderedValues)
    {
        for (std::size_t slot = 1; slot < orderedValues.size(); ++slot)
        {
            if (orderedValues[slot] > orderedValues[slot - 1])
            {
                return false;
            }
        }
        return true;
    }

    inline MilliFunds seatedOptionValue(const TurnPlan & plan, const ActorProgress & state, std::int32_t option)
    {
        const MilliFunds staticValue = optionValue(state, option);
        if (option == NO_CANDIDATE)
        {
            return staticValue;
        }
        const ActionBundle & bundle = state.pActor->candidates[static_cast<std::size_t>(option)].bundle;
        const BundleComponent* pComponent = singleComponentOf(bundle);
        if (pComponent == nullptr || pComponent->kind != ComponentKind::Fire)
        {
            return staticValue;
        }
        return grantedFireValue(pComponent->fire, staticValue, plan.action(state.actionIndex).plannedDamageSteps);
    }

    struct SeatOutcome
    {
        ReservationResult claim{ReservationResult::Invalid};
        MilliFunds value{0};
    };

    inline std::vector<std::int32_t> buildOptionOrder(const ActorProgress & state)
    {
        std::vector<std::int32_t> options;
        options.reserve(state.order.size() + 1);
        bool placed = false;
        for (const std::int32_t slot : state.order)
        {
            if (!placed && optionValue(state, slot) < 0)
            {
                options.push_back(NO_CANDIDATE);
                placed = true;
            }
            options.push_back(slot);
        }
        if (!placed)
        {
            options.push_back(NO_CANDIDATE);
        }
        return options;
    }

    inline std::vector<std::int32_t> incumbentFirstOptions(const ActorProgress & state)
    {
        if (state.chosen == NO_CANDIDATE)
        {
            return state.options;
        }
        const MilliFunds tier = optionValue(state, state.chosen);
        std::vector<std::int32_t> ordered;
        ordered.reserve(state.options.size());
        bool hoisted = false;
        for (const std::int32_t option : state.options)
        {
            if (!hoisted && optionValue(state, option) <= tier)
            {
                ordered.push_back(state.chosen);
                hoisted = true;
            }
            if (option != state.chosen)
            {
                ordered.push_back(option);
            }
        }
        if (!hoisted)
        {
            ordered.push_back(state.chosen);
        }
        return ordered;
    }

    inline std::vector<std::int32_t> cappedOptions(const std::vector<std::int32_t> & options, std::int32_t cap)
    {
        if (cap == NO_CANDIDATE_CAP || static_cast<std::int32_t>(options.size()) <= cap)
        {
            return options;
        }
        std::vector<std::int32_t> capped(options.begin(), options.begin() + cap);
        if (std::find(capped.begin(), capped.end(), NO_CANDIDATE) == capped.end())
        {
            capped.push_back(NO_CANDIDATE);
        }
        return capped;
    }

    inline void unseatActor(TurnPlan & plan, ActorProgress & state)
    {
        if (state.actionIndex != NO_ACTION)
        {
            plan.markFailed(state.actionIndex);
        }
        state.chosen = NO_CANDIDATE;
        state.seatedValue = 0;
    }

    inline SeatOutcome seatOption(TurnPlan & plan, const AssignmentInput & input, ActorProgress & state,
                                  std::int32_t option)
    {
        if (option == NO_CANDIDATE)
        {
            unseatActor(plan, state);
            return SeatOutcome{ReservationResult::Granted, 0};
        }
        const CandidateBundle & candidate = state.pActor->candidates[static_cast<std::size_t>(option)];
        const PlannedAction action =
            plannedActionFrom(input.actionIds, input.unitLinks, state.pActor->engineUnitId, candidate);
        ReservationResult claim = ReservationResult::Invalid;
        if (state.actionIndex == NO_ACTION)
        {
            state.actionIndex = plan.addAction(action);
            if (state.actionIndex != NO_ACTION)
            {
                claim = claimOrRollback(plan, state.actionIndex, action, candidate);
            }
        }
        else
        {
            claim = installCandidate(plan, state.actionIndex, action, candidate);
        }
        if (claim != ReservationResult::Granted)
        {
            unseatActor(plan, state);
            return SeatOutcome{claim, 0};
        }
        state.chosen = option;
        state.seatedValue = seatedOptionValue(plan, state, option);
        return SeatOutcome{claim, state.seatedValue};
    }

    inline void seatBestClaimable(TurnPlan & plan, const AssignmentInput & input, ActorProgress & state,
                                  const std::vector<std::int32_t> & options)
    {
        bool hasBest = false;
        std::int32_t bestOption = NO_CANDIDATE;
        MilliFunds bestValue = 0;
        for (const std::int32_t option : options)
        {
            if (hasBest && optionValue(state, option) <= bestValue)
            {
                break;
            }
            const SeatOutcome outcome = seatOption(plan, input, state, option);
            if (outcome.claim != ReservationResult::Granted)
            {
                continue;
            }
            if (hasBest && outcome.value <= bestValue)
            {
                continue;
            }
            hasBest = true;
            bestOption = option;
            bestValue = outcome.value;
        }
        if (state.chosen != bestOption)
        {
            seatOption(plan, input, state, bestOption);
        }
    }

    inline bool tilePrecedes(const TilePoint & left, const TilePoint & right)
    {
        if (left.y != right.y)
        {
            return left.y < right.y;
        }
        return left.x < right.x;
    }

    struct TileOrder
    {
        bool operator()(const TilePoint & left, const TilePoint & right) const
        {
            return tilePrecedes(left, right);
        }
    };

    struct ActorIdOrder
    {
        const std::vector<ActorProgress>* pActors{nullptr};

        bool operator()(std::int32_t left, std::int32_t right) const
        {
            const std::vector<ActorProgress> & actors = *pActors;
            const std::int32_t leftId = actors[static_cast<std::size_t>(left)].pActor->engineUnitId;
            const std::int32_t rightId = actors[static_cast<std::size_t>(right)].pActor->engineUnitId;
            if (leftId != rightId)
            {
                return leftId < rightId;
            }
            return left < right;
        }
    };

    inline bool canonicalActorSupportFirst(
        const std::vector<ActorProgress> & actors,
        std::span<const std::int32_t> left,
        std::span<const std::int32_t> right)
    {
        return std::lexicographical_compare(
            left.begin(),
            left.end(),
            right.begin(),
            right.end(),
            ActorIdOrder{&actors});
    }

    inline bool canonicalCandidateTupleFirst(
        std::span<const std::int32_t> actorOrder,
        std::span<const std::int32_t> left,
        std::span<const std::int32_t> right)
    {
        for (const std::int32_t slot : actorOrder)
        {
            const auto index =
                static_cast<std::size_t>(slot);
            if (left[index] != right[index])
            {
                return left[index] < right[index];
            }
        }
        return false;
    }

    struct ActorResources
    {
        std::vector<TilePoint> destinations;
        std::vector<std::int32_t> targets;
    };

    inline void sortUniqueTiles(std::vector<TilePoint> & tiles)
    {
        std::sort(tiles.begin(), tiles.end(), TileOrder{});
        tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    }

    inline void sortUniqueIds(std::vector<std::int32_t> & ids)
    {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    inline ActorResources actorResourcesOf(const ActorProgress & state)
    {
        ActorResources resources;
        for (const std::int32_t slot : state.order)
        {
            const CandidateBundle & candidate = state.pActor->candidates[static_cast<std::size_t>(slot)];
            resources.destinations.push_back(candidate.bundle.destination);
            const BundleComponent* pComponent = singleComponentOf(candidate.bundle);
            if (pComponent != nullptr && pComponent->kind == ComponentKind::Fire)
            {
                resources.targets.push_back(pComponent->fire.targetUnitId);
            }
        }
        sortUniqueTiles(resources.destinations);
        sortUniqueIds(resources.targets);
        return resources;
    }

    inline bool sharesTile(const std::vector<TilePoint> & left, const std::vector<TilePoint> & right)
    {
        std::size_t leftSlot = 0;
        std::size_t rightSlot = 0;
        while (leftSlot < left.size() && rightSlot < right.size())
        {
            if (left[leftSlot] == right[rightSlot])
            {
                return true;
            }
            if (tilePrecedes(left[leftSlot], right[rightSlot]))
            {
                ++leftSlot;
            }
            else
            {
                ++rightSlot;
            }
        }
        return false;
    }

    inline bool sharesId(const std::vector<std::int32_t> & left, const std::vector<std::int32_t> & right)
    {
        std::size_t leftSlot = 0;
        std::size_t rightSlot = 0;
        while (leftSlot < left.size() && rightSlot < right.size())
        {
            if (left[leftSlot] == right[rightSlot])
            {
                return true;
            }
            if (left[leftSlot] < right[rightSlot])
            {
                ++leftSlot;
            }
            else
            {
                ++rightSlot;
            }
        }
        return false;
    }

    inline bool actorsContest(const ActorResources & left, const ActorResources & right)
    {
        return sharesTile(left.destinations, right.destinations) || sharesId(left.targets, right.targets);
    }

    struct ConflictEdge
    {
        std::int32_t left{NO_UNIT};
        std::int32_t right{NO_UNIT};
    };

    inline std::size_t pickBestActor(const std::vector<ActorProgress> & progress)
    {
        std::size_t best = progress.size();
        for (std::size_t slot = 0; slot < progress.size(); ++slot)
        {
            if (progress[slot].done)
            {
                continue;
            }
            if (best == progress.size() || progress[slot].economicValue() > progress[best].economicValue() ||
                (progress[slot].economicValue() == progress[best].economicValue() &&
                 progress[slot].pActor->engineUnitId < progress[best].pActor->engineUnitId))
            {
                best = slot;
            }
        }
        return best;
    }

    struct MaximumValueAssignment
    {
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        friend class ::ArenaRelativeStockSupportAudit;
#endif

        static constexpr std::int32_t SETTLING_SWEEP_CAP = 8;
        static constexpr std::int32_t SWAP_SWEEP_CAP = 4;
        static constexpr std::int32_t CLUSTER_ACTOR_CAP = 6;
        static constexpr std::int32_t CLUSTER_CANDIDATE_CAP = 12;
        static constexpr std::int32_t CLUSTER_STATE_CAP = 20000;
        static constexpr std::size_t RELATIVE_JOINT_MIN_ACTORS = 3;
        static constexpr std::size_t RELATIVE_JOINT_MAX_ACTORS = 4;
        static constexpr std::int64_t RELATIVE_JOINT_PRODUCT_CAP = 50000;

        static AssignmentResult assign(const AssignmentInput & input)
        {
            using Clock = std::chrono::steady_clock;
            AssignmentResult result;
            DeferredAudits audits;
            const bool timed =
                input.measureTimings ||
                input.pTrace != nullptr;
            Clock::time_point phaseStart;
            if (timed)
            {
                phaseStart = Clock::now();
            }
            std::vector<ActorProgress> actors = prepareActors(input, result.stats);
            if (timed)
            {
                result.phaseTiming.prepareNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            greedyInit(result, actors, input);
            if (timed)
            {
                result.phaseTiming.greedyNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            const std::vector<std::int32_t> sweepOrder = actorSweepOrder(actors);
            settle(
                result.plan,
                input,
                actors,
                sweepOrder,
                result.stats,
                audits);
            if (timed)
            {
                result.phaseTiming.settlingNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            const std::vector<ConflictEdge> edges = conflictEdges(input, actors, sweepOrder);
            const RelativeAssignmentBudget relativeBudget = relativeAssignmentBudget(result.plan, input, actors, edges);
            improveBySwaps(
                result.plan,
                input,
                actors,
                edges,
                result.stats,
                audits,
                relativeBudget,
                result.phaseTiming.stockPairs);
            if (timed)
            {
                result.phaseTiming.pairRefinementNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            enumerateClusters(result.plan, input, actors, sweepOrder, edges, result.stats,
                              relativeBudget, result.phaseTiming.stockJoints);
            if (timed)
            {
                result.phaseTiming.clusterNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            finishPlan(result);
            countAssignments(result.plan, actors, result.stats);
            recordFinalAssignment(result, actors, input);
            if (timed)
            {
                result.phaseTiming.finishPlanNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
            }
            recordDeferredAudits(input, audits);
            if (input.pRelativeStockObserver != nullptr)
            {
                const auto remaining = relativeBudget.session == nullptr ? 0 : relativeBudget.session->remainingWork();
                input.pRelativeStockObserver->finishPlan(result, {
                    relativeBudget.initialBalance, relativeBudget.totalLimit, relativeBudget.pairLimit,
                    relativeBudget.initialBalance - remaining, remaining,
                });
            }
            return result;
        }

    private:
        enum class SearchPricingMode : std::int8_t
        {
            LegacyScalar,
            PairSwapInterval,
        };

        enum class PairPricingResult : std::int8_t
        {
            NotPair,
            NonStockExactScalar,
            LiveInterval,
            LiveIntervalUnavailable,
            LiveIntervalFailure,
        };

        struct ActorSeat
        {
            std::int32_t chosen{NO_CANDIDATE};
            std::int32_t actionIndex{NO_ACTION};
            MilliFunds seatedValue{0};
        };

        struct RelativeAssignmentBudget
        {
            RelativeStockSession* session{nullptr};
            std::int64_t initialBalance{0};
            std::int64_t totalLimit{0};
            std::int64_t pairLimit{0};

            std::int64_t remaining(RelativeStockPhase phase) const
            {
                if (session == nullptr)
                {
                    return 0;
                }
                const auto spent = initialBalance - session->remainingWork();
                const auto limit = phase == RelativeStockPhase::Pair ? pairLimit : totalLimit;
                return std::max<std::int64_t>(0, limit - spent);
            }
        };

        struct RelativeContender
        {
            std::vector<std::int32_t> members;
            std::vector<std::int32_t> tuple;
            RelativeStockQuote quote;
            RelativeStockComparison comparison;
            RelativeStockRefinementCursor cursor;
            bool visited{false};
        };

        struct RelativeDomain
        {
            std::vector<std::int32_t> members;
            std::vector<std::vector<std::int32_t>> options;
            std::int64_t product{1};
            std::int64_t maximumStates{0};
            std::int64_t states{0};
            std::int64_t claimable{0};
            RelativeStockDomainStatus status{RelativeStockDomainStatus::Complete};
        };

        struct RelativeFrontier
        {
            std::vector<RelativeContender> contenders;
            bool valid{true};
        };

        struct RelativeTrial
        {
            TurnPlan plan;
            std::vector<ActorSeat> seats;
            std::vector<std::int32_t> order;
            MilliFunds economic{0};
            bool valid{false};
        };

        struct ExactSearchOutcome
        {
            bool improved{false};
            bool aborted{false};
            bool liveFailure{false};
            std::int32_t states{0};
            MilliFunds incumbentLower{0};
            MilliFunds incumbentUpper{0};
            MilliFunds resultLower{0};
            MilliFunds resultUpper{0};
            bool resultKnown{false};
            PairPricingResult pairPricing{
                PairPricingResult::NotPair
            };
            struct PairExactAuditSnapshot
            {
                TurnPlan incumbentPlan;
                TurnPlan winnerPlan;
                MilliFunds incumbentEconomic{0};
                MilliFunds winnerEconomic{0};
                std::vector<std::int32_t> incumbentChoices;
                std::vector<std::int32_t> winnerChoices;
                bool liveIntervalPricing{false};
            };
            std::optional<PairExactAuditSnapshot> pairAudit;
        };

        static QString tracePairPricing(
            PairPricingResult result)
        {
            switch (result)
            {
                case PairPricingResult::
                    NonStockExactScalar:
                    return QStringLiteral(
                        "NON_STOCK_EXACT_SCALAR");
                case PairPricingResult::LiveInterval:
                    return QStringLiteral(
                        "LIVE_INTERVAL");
                case PairPricingResult::
                    LiveIntervalUnavailable:
                    return QStringLiteral(
                        "LIVE_INTERVAL_UNAVAILABLE");
                case PairPricingResult::
                    LiveIntervalFailure:
                    return QStringLiteral(
                        "LIVE_INTERVAL_FAILURE");
                case PairPricingResult::NotPair:
                    return QStringLiteral(
                        "LEGACY_SCALAR");
            }
            return QStringLiteral("UNKNOWN");
        }

        static QString tracePairRequestedPricing(
            PairPricingResult result)
        {
            return result ==
                       PairPricingResult::
                           NonStockExactScalar
                       ? QStringLiteral(
                             "NON_STOCK_EXACT_SCALAR")
                       : QStringLiteral(
                             "LIVE_INTERVAL");
        }

        static QString tracePairResultReason(
            const ExactSearchOutcome & outcome)
        {
            switch (outcome.pairPricing)
            {
                case PairPricingResult::
                    LiveIntervalUnavailable:
                    return QStringLiteral(
                        "LIVE_INTERVAL_UNAVAILABLE");
                case PairPricingResult::
                    LiveIntervalFailure:
                    return QStringLiteral(
                        "LIVE_INTERVAL_FAILURE");
                case PairPricingResult::LiveInterval:
                    return outcome.improved
                               ? QStringLiteral(
                                     "LOWER_ABOVE_INCUMBENT_UPPER")
                               : QStringLiteral(
                                     "NO_CERTIFIED_REPLACEMENT");
                case PairPricingResult::
                    NonStockExactScalar:
                    return outcome.improved
                               ? QStringLiteral(
                                     "EXACT_SCALAR_IMPROVEMENT")
                               : QStringLiteral(
                                     "EXACT_SCALAR_RETAINED");
                case PairPricingResult::NotPair:
                    return QStringLiteral("NOT_PAIR");
            }
            return QStringLiteral("UNKNOWN");
        }

        struct StockDecompositionRequest
        {
            std::int32_t sweep{0};
            std::int32_t actor{NO_UNIT};
            std::int32_t actorKnowledge{NO_UNIT};
            std::int32_t candidate{NO_CANDIDATE};
            PlanBundleKind kind{PlanBundleKind::Wait};
            TilePoint destination{INVALID_TILE};
            MilliFunds seatedValue{0};
            MilliFunds fullEconomic{0};
            MilliFunds liveStockLower{0};
            MilliFunds liveStockUpper{0};
            MilliFunds liveCompleteLower{0};
            MilliFunds liveCompleteUpper{0};
            bool liveValid{false};
            bool lowerWitnessReplays{false};
            TurnPlan plan;
        };

        struct StockSettlingValue
        {
            MilliFunds economic{0};
            MilliFunds lower{0};
            MilliFunds upper{0};
            LivePlanStockQuote quote;
        };

        struct PairExactAuditRequest
        {
            std::int32_t sweep{0};
            std::vector<std::int32_t> actors;
            MilliFunds incumbentLower{0};
            MilliFunds incumbentUpper{0};
            MilliFunds winnerLower{0};
            MilliFunds winnerUpper{0};
            ExactSearchOutcome::PairExactAuditSnapshot snapshot;
        };

        struct DeferredAudits
        {
            std::vector<StockDecompositionRequest>
                stockDecompositions;
            std::vector<PairExactAuditRequest> pairAudits;
        };

        struct ClusterSearch
        {
            std::vector<std::int32_t> members;
            std::vector<std::vector<std::int32_t>> options;
            std::vector<MilliFunds> ceilings;
            std::vector<std::int32_t> current;
            std::vector<std::int32_t> legacyBest;
            std::vector<std::int32_t> bestFeasiblePlan;
            MilliFunds bestTotal{0};
            bool stockLive{false};
            MilliFunds propertyCeilingInSearchBasis{0};
            MilliFunds propertyOriginInSearchBasis{0};
            MilliFunds fixedEconomic{0};
            MilliFunds incumbentLower{0};
            MilliFunds incumbentUpper{0};
            MilliFunds bestFeasibleEconomic{0};
            bool hasBest{false};
            std::int32_t states{0};
            bool aborted{false};
            bool liveFailure{false};
            SearchPricingMode pricingMode{
                SearchPricingMode::LegacyScalar
            };
            LivePlanStockQuote bestFeasibleQuote;
        };

        static std::vector<ActorProgress> prepareActors(const AssignmentInput & input, AssignmentStats & stats)
        {
            std::vector<ActorProgress> actors;
            actors.reserve(input.actors.size());
            for (const AssignmentActor & actor : input.actors)
            {
                ActorProgress entry;
                entry.pActor = &actor;
                entry.order = plannableCandidateOrder(input.actionIds, input.unitLinks, actor.candidates, stats);
                entry.options = buildOptionOrder(entry);
                if (input.pTrace != nullptr)
                {
                    entry.completeValues.resize(actor.candidates.size());
                }
                entry.done = entry.order.empty();
                if (input.pTrace != nullptr &&
                    input.pTrace->candidateDetailsEnabled())
                {
                    for (std::size_t slot = 0;
                         slot < actor.candidates.size();
                         ++slot)
                    {
                        const CandidateBundle & candidate =
                            actor.candidates[slot];
                        const bool plannable = isPlannableCandidate(
                            input.actionIds,
                            input.unitLinks,
                            candidate);
                        const PlanBundleKind kind =
                            planBundleKindOf(candidate.bundle);
                        const QString actionId =
                            planActionIdFor(input.actionIds, kind);
                        const BundleComponent* pComponent =
                            singleComponentOf(
                                candidate.bundle);
                        const std::int32_t targetKnowledge =
                            pComponent != nullptr &&
                                    pComponent->kind ==
                                        ComponentKind::Fire
                                ? pComponent->fire.targetUnitId
                                : NO_UNIT;
                        const KnownUnitLink targetLink =
                            linkOf(
                                input.unitLinks,
                                targetKnowledge);
                        QString support =
                            QStringLiteral("SUPPORTED");
                        if (!plannable &&
                            actionId.isEmpty())
                        {
                            support =
                                QStringLiteral(
                                    "UNSUPPORTED_ACTION_KIND");
                        }
                        else if (!plannable &&
                                 pComponent == nullptr)
                        {
                            support =
                                QStringLiteral(
                                    "FIRE_COMPONENT_MISSING");
                        }
                        else if (!plannable)
                        {
                            support =
                                QStringLiteral(
                                    "FIRE_TARGET_UNLINKED");
                        }
                        input.pTrace->record(
                            QStringLiteral("CANDIDATE_SUPPORT"),
                            QStringLiteral(
                                "actor=%1 actorKnowledge=%2 candidate=%3 generated=%4 kind=%5 actionId=%6 plannable=%7 destinationClaim=%8 targetKnowledge=%9 targetEngine=%10 reservationTargetHpSteps=%11 reservationRequestedDamageSteps=%12 support=%13")
                                .arg(actor.engineUnitId)
                                .arg(actor.knowledgeUnitIndex)
                                .arg(slot)
                                .arg(candidate.generationIndex)
                                .arg(traceBundleKind(kind))
                                .arg(actionId.isEmpty()
                                         ? QStringLiteral("NONE")
                                         : actionId)
                                .arg(traceBool(plannable))
                                .arg(traceTile(
                                    candidate.bundle.destination))
                                .arg(targetKnowledge)
                                .arg(targetLink.engineUnitId)
                                .arg(
                                    pComponent != nullptr &&
                                            pComponent->kind ==
                                                ComponentKind::Fire
                                        ? QString::number(
                                              pComponent->fire.targetHpSteps)
                                        : QStringLiteral("NA"))
                                .arg(
                                    pComponent != nullptr &&
                                            pComponent->kind ==
                                                ComponentKind::Fire
                                        ? QString::number(
                                              pComponent->fire.damageSteps)
                                        : QStringLiteral("NA"))
                                .arg(support));
                    }
                    for (std::size_t orderSlot = 0;
                         orderSlot < entry.order.size();
                         ++orderSlot)
                    {
                        const std::int32_t candidateIndex =
                            entry.order[orderSlot];
                        const MilliFunds value =
                            optionValue(entry, candidateIndex);
                        const bool tied =
                            orderSlot > 0 &&
                            value ==
                                optionValue(
                                    entry,
                                    entry.order[orderSlot - 1]);
                        input.pTrace->record(
                            QStringLiteral("CANDIDATE_ORDER"),
                            QStringLiteral(
                                "actor=%1 order=%2 candidate=%3 generated=%4 value=%5 tieWithPrevious=%6")
                                .arg(actor.engineUnitId)
                                .arg(orderSlot)
                                .arg(candidateIndex)
                                .arg(actor.candidates[static_cast<std::size_t>(
                                         candidateIndex)]
                                         .generationIndex)
                                .arg(value)
                                .arg(traceBool(tied)));
                    }
                }
                actors.push_back(std::move(entry));
            }
            return actors;
        }

        static void greedyInit(AssignmentResult & result, std::vector<ActorProgress> & actors,
                               const AssignmentInput & input)
        {
            std::int32_t selectionOrder = 0;
            while (true)
            {
                const std::size_t slot = pickBestActor(actors);
                if (slot >= actors.size())
                {
                    return;
                }
                if (input.pTrace != nullptr)
                {
                    const ActorProgress & state = actors[slot];
                    input.pTrace->record(
                        QStringLiteral("GREEDY_PICK"),
                        QStringLiteral(
                            "order=%1 actor=%2 actorKnowledge=%3 candidate=%4 value=%5")
                            .arg(selectionOrder)
                            .arg(state.pActor->engineUnitId)
                            .arg(state.pActor->knowledgeUnitIndex)
                            .arg(state.order[state.cursor])
                            .arg(state.economicValue()));
                }
                ++selectionOrder;
                takeBestCandidate(result, actors[slot], input);
            }
        }

        static void takeBestCandidate(AssignmentResult & result, ActorProgress & entry, const AssignmentInput & input)
        {
            const CandidateBundle & candidate = entry.candidate();
            const PlannedAction action =
                plannedActionFrom(input.actionIds, input.unitLinks, entry.pActor->engineUnitId, candidate);
            ReservationResult claim = ReservationResult::Invalid;
            if (entry.actionIndex == NO_ACTION)
            {
                entry.actionIndex = result.plan.addAction(action);
                if (entry.actionIndex != NO_ACTION)
                {
                    claim = claimOrRollback(result.plan, entry.actionIndex, action, candidate);
                }
            }
            else
            {
                claim = installCandidate(result.plan, entry.actionIndex, action, candidate);
            }
            if (claim == ReservationResult::Granted)
            {
                entry.done = true;
                entry.chosen = entry.order[entry.cursor];
                entry.greedyChosen = entry.chosen;
                entry.seatedValue = seatedOptionValue(result.plan, entry, entry.chosen);
                entry.phase = AssignmentResult::SelectionPhase::Greedy;
            }
            if (input.pTrace != nullptr)
            {
                input.pTrace->record(
                    QStringLiteral("GREEDY"),
                    QStringLiteral(
                        "actor=%1 actorKnowledge=%2 candidate=%3 generated=%4 kind=%5 value=%6 claim=%7 selected=%8 seatedValue=%9")
                        .arg(entry.pActor->engineUnitId)
                        .arg(entry.pActor->knowledgeUnitIndex)
                        .arg(entry.order[entry.cursor])
                        .arg(candidate.generationIndex)
                        .arg(traceBundleKind(
                            planBundleKindOf(candidate.bundle)))
                        .arg(candidate.valuation.value().economicValue)
                        .arg(traceReservationResult(claim))
                        .arg(traceBool(
                            claim == ReservationResult::Granted))
                        .arg(
                            claim == ReservationResult::Granted
                                ? QString::number(entry.seatedValue)
                                : QStringLiteral("NA")));
            }
            if (claim == ReservationResult::Granted)
            {
                return;
            }
            countRejection(result.stats, claim);
            ++entry.cursor;
            if (entry.cursor < entry.order.size())
            {
                return;
            }
            entry.done = true;
            if (entry.actionIndex != NO_ACTION)
            {
                result.plan.markFailed(entry.actionIndex);
            }
        }

        static std::vector<std::int32_t> actorSweepOrder(const std::vector<ActorProgress> & actors)
        {
            std::vector<std::int32_t> order;
            order.reserve(actors.size());
            for (std::size_t slot = 0; slot < actors.size(); ++slot)
            {
                order.push_back(static_cast<std::int32_t>(slot));
            }
            std::sort(order.begin(), order.end(), ActorIdOrder{&actors});
            return order;
        }

        static bool isCaptureKind(PlanBundleKind kind)
        {
            return kind == PlanBundleKind::Capture ||
                   kind == PlanBundleKind::MoveAndCapture;
        }

        static bool hasSameDestinationCapturePeer(
            const ActorProgress & state,
            std::int32_t option)
        {
            if (option < 0 ||
                option >= static_cast<std::int32_t>(
                              state.pActor->candidates.size()))
            {
                return false;
            }
            const CandidateBundle & candidate =
                state.pActor->candidates[
                    static_cast<std::size_t>(option)];
            const bool captures =
                isCaptureKind(planBundleKindOf(candidate.bundle));
            for (const std::int32_t peer : state.options)
            {
                if (peer < 0 ||
                    peer == option ||
                    peer >= static_cast<std::int32_t>(
                                state.pActor->candidates.size()))
                {
                    continue;
                }
                const CandidateBundle & other =
                    state.pActor->candidates[
                        static_cast<std::size_t>(peer)];
                if (other.bundle.destination ==
                        candidate.bundle.destination &&
                    isCaptureKind(
                        planBundleKindOf(other.bundle)) !=
                        captures)
                {
                    return true;
                }
            }
            return false;
        }

        static void deferStockDecomposition(
            const TurnPlan & plan,
            const AssignmentInput & input,
            const ActorProgress & state,
            std::int32_t option,
            std::int32_t sweep,
            const StockSettlingValue & value,
            DeferredAudits & audits)
        {
            if (input.pTrace == nullptr ||
                !input.pTrace->stockDetailsEnabled() ||
                input.pStockValuer == nullptr ||
                !hasSameDestinationCapturePeer(state, option))
            {
                return;
            }
            const CandidateBundle & candidate =
                state.pActor->candidates[
                    static_cast<std::size_t>(option)];
            audits.stockDecompositions.push_back(
                StockDecompositionRequest{
                    .sweep = sweep,
                    .actor = state.pActor->engineUnitId,
                    .actorKnowledge =
                        state.pActor->knowledgeUnitIndex,
                    .candidate = option,
                    .kind =
                        planBundleKindOf(candidate.bundle),
                    .destination =
                        candidate.bundle.destination,
                    .seatedValue = state.seatedValue,
                    .fullEconomic = value.economic,
                    .liveStockLower =
                        value.quote.stockAbsolute.lower,
                    .liveStockUpper =
                        value.quote.stockAbsolute.upper,
                    .liveCompleteLower = value.lower,
                    .liveCompleteUpper = value.upper,
                    .liveValid = value.quote.valid,
                    .lowerWitnessReplays =
                        value.quote.lowerWitnessReplays,
                    .plan = plan,
                });
        }

        static void recordStockSettlingComparison(
            const AssignmentInput & input,
            const ActorProgress & state,
            std::int32_t sweep,
            std::int32_t previousChosen,
            std::int32_t challenger,
            const StockSettlingValue & incumbent,
            const std::optional<StockSettlingValue> & challengerValue,
            ReservationResult claim,
            const QString & reason,
            bool accepted)
        {
            if (input.pTrace == nullptr ||
                !input.pTrace->stockDetailsEnabled())
            {
                return;
            }
            const CandidateBundle* pCandidate =
                challenger == NO_CANDIDATE
                    ? nullptr
                    : &state.pActor->candidates[
                          static_cast<std::size_t>(challenger)];
            const bool incumbentValid =
                incumbent.quote.valid &&
                incumbent.quote.lowerWitnessReplays;
            const bool challengerValid =
                challengerValue.has_value() &&
                challengerValue->quote.valid &&
                challengerValue->quote.lowerWitnessReplays;
            QString fields = QStringLiteral(
                "sweep=%1 actor=%2 previousCandidate=%3 challenger=%4 kind=%5 destination=%6 claim=%7")
                                 .arg(sweep)
                                 .arg(state.pActor->engineUnitId)
                                 .arg(previousChosen)
                                 .arg(challenger)
                                 .arg(
                                     pCandidate == nullptr
                                         ? QStringLiteral("NONE")
                                         : traceBundleKind(
                                               planBundleKindOf(
                                                   pCandidate->bundle)))
                                 .arg(
                                     pCandidate == nullptr
                                         ? traceTile(INVALID_TILE)
                                         : traceTile(
                                               pCandidate->bundle.destination))
                                 .arg(traceReservationResult(claim));
            fields += QStringLiteral(
                " incumbentEconomic=%1 incumbentStockLower=%2 incumbentStockUpper=%3 incumbentLower=%4 incumbentUpper=%5 incumbentValid=%6 incumbentLowerWitnessReplays=%7")
                          .arg(incumbent.economic)
                          .arg(
                              incumbentValid
                                  ? QString::number(
                                        incumbent.quote.stockAbsolute.lower)
                                  : QStringLiteral("NA"))
                          .arg(
                              incumbentValid
                                  ? QString::number(
                                        incumbent.quote.stockAbsolute.upper)
                                  : QStringLiteral("NA"))
                          .arg(
                              incumbentValid
                                  ? QString::number(incumbent.lower)
                                  : QStringLiteral("NA"))
                          .arg(
                              incumbentValid
                                  ? QString::number(incumbent.upper)
                                  : QStringLiteral("NA"))
                          .arg(traceBool(incumbent.quote.valid))
                          .arg(traceBool(
                              incumbent.quote.lowerWitnessReplays));
            fields += QStringLiteral(
                " challengerEconomic=%1 challengerStockLower=%2 challengerStockUpper=%3 challengerLower=%4 challengerUpper=%5 valid=%6 lowerWitnessReplays=%7 exact=%8 reason=%9 accepted=%10")
                          .arg(
                              challengerValue.has_value()
                                  ? QString::number(
                                        challengerValue->economic)
                                  : QStringLiteral("NA"))
                          .arg(
                              challengerValid
                                  ? QString::number(
                                        challengerValue->quote
                                            .stockAbsolute.lower)
                                  : QStringLiteral("NA"))
                          .arg(
                              challengerValid
                                  ? QString::number(
                                        challengerValue->quote
                                            .stockAbsolute.upper)
                                  : QStringLiteral("NA"))
                          .arg(
                              challengerValid
                                  ? QString::number(
                                        challengerValue->lower)
                                  : QStringLiteral("NA"))
                          .arg(
                              challengerValid
                                  ? QString::number(
                                        challengerValue->upper)
                                  : QStringLiteral("NA"))
                          .arg(traceBool(
                              challengerValue.has_value() &&
                              challengerValue->quote.valid))
                          .arg(traceBool(
                              challengerValue.has_value() &&
                              challengerValue->quote
                                  .lowerWitnessReplays))
                          .arg(traceBool(
                              challengerValid &&
                              challengerValue->lower ==
                                  challengerValue->upper))
                          .arg(reason)
                          .arg(traceBool(accepted));
            input.pTrace->record(
                QStringLiteral("SETTLING_CHALLENGER"),
                fields);
        }

        static void recordStockSettlingWinner(
            const AssignmentInput & input,
            const ActorProgress & state,
            std::int32_t sweep,
            const StockSettlingValue & winner,
            const QString & reason,
            bool changed)
        {
            if (input.pTrace == nullptr)
            {
                return;
            }
            const CandidateBundle* pWinner =
                state.chosen == NO_CANDIDATE
                    ? nullptr
                    : &state.pActor->candidates[
                          static_cast<std::size_t>(state.chosen)];
            const bool valid =
                winner.quote.valid &&
                winner.quote.lowerWitnessReplays;
            const bool exact =
                valid && winner.lower == winner.upper;
            bool completeKnown = false;
            MilliFunds completeValue = 0;
            if (state.chosen != NO_CANDIDATE &&
                state.chosen >= 0 &&
                state.chosen <
                    static_cast<std::int32_t>(
                        state.completeValues.size()))
            {
                const AssignmentResult::CandidateCompleteValue &
                    complete =
                        state.completeValues[
                            static_cast<std::size_t>(
                                state.chosen)];
                completeKnown = complete.known;
                completeValue = complete.value;
            }
            input.pTrace->record(
                QStringLiteral("SETTLING_WINNER"),
                QStringLiteral(
                    "sweep=%1 actor=%2 candidate=%3 kind=%4 destination=%5 economic=%6 stockLower=%7 stockUpper=%8 lower=%9 upper=%10 valid=%11 lowerWitnessReplays=%12 exact=%13 completeValue=%14 completeKnown=%15 changed=%16 reason=%17")
                    .arg(sweep)
                    .arg(state.pActor->engineUnitId)
                    .arg(state.chosen)
                    .arg(
                        pWinner == nullptr
                            ? QStringLiteral("NONE")
                            : traceBundleKind(
                                  planBundleKindOf(
                                      pWinner->bundle)))
                    .arg(
                        pWinner == nullptr
                            ? traceTile(INVALID_TILE)
                            : traceTile(
                                  pWinner->bundle.destination))
                    .arg(winner.economic)
                    .arg(
                        valid
                            ? QString::number(
                                  winner.quote.stockAbsolute.lower)
                            : QStringLiteral("NA"))
                    .arg(
                        valid
                            ? QString::number(
                                  winner.quote.stockAbsolute.upper)
                            : QStringLiteral("NA"))
                    .arg(
                        valid
                            ? QString::number(winner.lower)
                            : QStringLiteral("NA"))
                    .arg(
                        valid
                            ? QString::number(winner.upper)
                            : QStringLiteral("NA"))
                    .arg(traceBool(winner.quote.valid))
                    .arg(traceBool(
                        winner.quote.lowerWitnessReplays))
                    .arg(traceBool(exact))
                    .arg(
                        completeKnown
                            ? QString::number(completeValue)
                            : QStringLiteral("NA"))
                    .arg(traceBool(completeKnown))
                    .arg(traceBool(changed))
                    .arg(reason));
        }

        static void recordStockSettlingReplay(
            const AssignmentInput & input,
            const ActorProgress & state,
            std::int32_t sweep,
            std::int32_t retainedOption,
            const StockSettlingValue & retained,
            const StockSettlingValue & replayed,
            ReservationResult claim,
            bool replayValid)
        {
            if (input.pTrace == nullptr ||
                !input.pTrace->stockDetailsEnabled())
            {
                return;
            }
            const bool keyMatches =
                replayed.quote.key ==
                retained.quote.key;
            input.pTrace->record(
                QStringLiteral("SETTLING_REPLAY"),
                QStringLiteral(
                    "sweep=%1 actor=%2 retainedCandidate=%3 claim=%4 retainedLower=%5 retainedUpper=%6 replayLower=%7 replayUpper=%8 valid=%9 lowerWitnessReplays=%10 keyMatches=%11 lowerMatches=%12 upperMatches=%13 reason=%14")
                    .arg(sweep)
                    .arg(state.pActor->engineUnitId)
                    .arg(retainedOption)
                    .arg(traceReservationResult(claim))
                    .arg(retained.lower)
                    .arg(retained.upper)
                    .arg(replayed.lower)
                    .arg(replayed.upper)
                    .arg(traceBool(replayed.quote.valid))
                    .arg(traceBool(
                        replayed.quote.lowerWitnessReplays))
                    .arg(traceBool(keyMatches))
                    .arg(traceBool(
                        replayed.lower == retained.lower))
                    .arg(traceBool(
                        replayed.upper == retained.upper))
                    .arg(
                        replayValid
                            ? QStringLiteral("REPLAY_VALID")
                            : QStringLiteral("REPLAY_FAILURE")));
        }

        static void setCompleteValue(
            ActorProgress & state,
            std::int32_t option,
            const StockSettlingValue & value)
        {
            if (option == NO_CANDIDATE ||
                option < 0 ||
                option >=
                    static_cast<std::int32_t>(
                        state.completeValues.size()))
            {
                return;
            }
            AssignmentResult::CandidateCompleteValue & complete =
                state.completeValues[
                    static_cast<std::size_t>(option)];
            complete.known =
                value.quote.valid &&
                value.quote.lowerWitnessReplays &&
                value.lower == value.upper;
            complete.value =
                complete.known ? value.lower : 0;
        }

        static StockSettlingValue quoteStockSettlingValue(
            const TurnPlan & plan,
            const AssignmentInput & input,
            const std::vector<ActorProgress> & actors)
        {
            StockSettlingValue value;
            value.economic =
                seatedPlanTotal(plan, actors);
            value.quote =
                input.pStockValuer->livePlanStock(
                    plan,
                    value.economic,
                    false);
            value.lower =
                value.economic +
                value.quote.stockAbsolute.lower;
            value.upper =
                value.economic +
                value.quote.stockAbsolute.upper;
            return value;
        }

        static void restoreStockSettlingState(
            TurnPlan & plan,
            ActorProgress & state,
            const TurnPlan & originalPlan,
            const ActorSeat & originalSeat,
            AssignmentResult::SelectionPhase originalPhase,
            const std::vector<
                AssignmentResult::CandidateCompleteValue> &
                originalCompleteValues)
        {
            plan = originalPlan;
            state.chosen = originalSeat.chosen;
            state.actionIndex = originalSeat.actionIndex;
            state.seatedValue = originalSeat.seatedValue;
            state.phase = originalPhase;
            state.completeValues = originalCompleteValues;
        }

        static bool settleStockActor(
            TurnPlan & plan,
            const AssignmentInput & input,
            std::vector<ActorProgress> & actors,
            std::int32_t actorSlot,
            std::int32_t sweep,
            AssignmentStats & stats,
            DeferredAudits & audits)
        {
            ActorProgress & state =
                actors[static_cast<std::size_t>(actorSlot)];
            const TurnPlan originalPlan = plan;
            const ActorSeat originalSeat{
                state.chosen,
                state.actionIndex,
                state.seatedValue,
            };
            const AssignmentResult::SelectionPhase originalPhase =
                state.phase;
            const std::vector<
                AssignmentResult::CandidateCompleteValue>
                originalCompleteValues =
                    state.completeValues;
            const std::int32_t previousChosen = state.chosen;
            for (AssignmentResult::CandidateCompleteValue & value :
                 state.completeValues)
            {
                value = AssignmentResult::CandidateCompleteValue{};
            }

            StockSettlingValue retained;
            retained.economic =
                seatedPlanTotal(plan, actors);
            if (input.pStockValuer == nullptr ||
                !input.pStockValuer->liveSettlingIntervals())
            {
                recordStockSettlingComparison(
                    input,
                    state,
                    sweep,
                    previousChosen,
                    NO_CANDIDATE,
                    retained,
                    std::nullopt,
                    ReservationResult::Granted,
                    QStringLiteral(
                        "LIVE_INTERVAL_UNAVAILABLE"),
                    false);
                recordStockSettlingWinner(
                    input,
                    state,
                    sweep,
                    retained,
                    QStringLiteral(
                        "LIVE_INTERVAL_UNAVAILABLE"),
                    false);
                return false;
            }

            retained =
                quoteStockSettlingValue(
                    plan,
                    input,
                    actors);
            if (!retained.quote.valid ||
                !retained.quote.lowerWitnessReplays)
            {
                recordStockSettlingComparison(
                    input,
                    state,
                    sweep,
                    previousChosen,
                    NO_CANDIDATE,
                    retained,
                    std::nullopt,
                    ReservationResult::Granted,
                    QStringLiteral(
                        "LIVE_INTERVAL_FAILURE"),
                    false);
                recordStockSettlingWinner(
                    input,
                    state,
                    sweep,
                    retained,
                    QStringLiteral(
                        "LIVE_INTERVAL_FAILURE"),
                    false);
                return false;
            }
            const StockSettlingValue originalIncumbent =
                retained;
            setCompleteValue(
                state,
                previousChosen,
                retained);
            deferStockDecomposition(
                plan,
                input,
                state,
                previousChosen,
                sweep,
                retained,
                audits);

            const std::vector<std::int32_t> options =
                incumbentFirstOptions(state);
            unseatActor(plan, state);
            std::int32_t retainedOption = previousChosen;
            for (const std::int32_t option : options)
            {
                if (option == previousChosen)
                {
                    continue;
                }
                const SeatOutcome outcome =
                    seatOption(
                        plan,
                        input,
                        state,
                        option);
                if (outcome.claim !=
                    ReservationResult::Granted)
                {
                    recordStockSettlingComparison(
                        input,
                        state,
                        sweep,
                        retainedOption,
                        option,
                        retained,
                        std::nullopt,
                        outcome.claim,
                        QStringLiteral(
                            "CLAIM_NOT_GRANTED"),
                        false);
                    continue;
                }

                const StockSettlingValue challenger =
                    quoteStockSettlingValue(
                        plan,
                        input,
                        actors);
                if (!challenger.quote.valid ||
                    !challenger.quote
                         .lowerWitnessReplays)
                {
                    recordStockSettlingComparison(
                        input,
                        state,
                        sweep,
                        retainedOption,
                        option,
                        retained,
                        challenger,
                        outcome.claim,
                        QStringLiteral(
                            "LIVE_INTERVAL_FAILURE"),
                        false);
                    restoreStockSettlingState(
                        plan,
                        state,
                        originalPlan,
                        originalSeat,
                        originalPhase,
                        originalCompleteValues);
                    recordStockSettlingWinner(
                        input,
                        state,
                        sweep,
                        originalIncumbent,
                        QStringLiteral(
                            "LIVE_INTERVAL_FAILURE"),
                        false);
                    return false;
                }
                setCompleteValue(
                    state,
                    option,
                    challenger);
                deferStockDecomposition(
                    plan,
                    input,
                    state,
                    option,
                    sweep,
                    challenger,
                    audits);
                const bool pruned =
                    challenger.upper <= retained.lower;
                const bool accepted =
                    challenger.lower > retained.upper;
                recordStockSettlingComparison(
                    input,
                    state,
                    sweep,
                    retainedOption,
                    option,
                    retained,
                    challenger,
                    outcome.claim,
                    pruned
                        ? QStringLiteral(
                              "UPPER_NOT_ABOVE_INCUMBENT_LOWER")
                        : accepted
                              ? QStringLiteral(
                                    "LOWER_ABOVE_INCUMBENT_UPPER")
                              : QStringLiteral(
                                    "INTERVAL_OVERLAP"),
                    accepted);
                unseatActor(plan, state);
                if (accepted)
                {
                    retainedOption = option;
                    retained = challenger;
                }
            }

            const SeatOutcome winningOutcome =
                seatOption(
                    plan,
                    input,
                    state,
                    retainedOption);
            const StockSettlingValue replayed =
                winningOutcome.claim ==
                        ReservationResult::Granted
                    ? quoteStockSettlingValue(
                          plan,
                          input,
                          actors)
                    : StockSettlingValue{};
            const bool replayValid =
                winningOutcome.claim ==
                    ReservationResult::Granted &&
                replayed.quote.valid &&
                replayed.quote.lowerWitnessReplays &&
                replayed.quote.key ==
                    retained.quote.key &&
                replayed.lower == retained.lower &&
                replayed.upper == retained.upper;
            recordStockSettlingReplay(
                input,
                state,
                sweep,
                retainedOption,
                retained,
                replayed,
                winningOutcome.claim,
                replayValid);
            if (!replayValid)
            {
                ++stats.replayFailures;
                restoreStockSettlingState(
                    plan,
                    state,
                    originalPlan,
                    originalSeat,
                    originalPhase,
                    originalCompleteValues);
                recordStockSettlingWinner(
                    input,
                    state,
                    sweep,
                    originalIncumbent,
                    QStringLiteral("REPLAY_FAILURE"),
                    false);
                return false;
            }

            setCompleteValue(
                state,
                retainedOption,
                replayed);
            const bool changed =
                state.chosen != previousChosen;
            if (changed)
            {
                state.phase =
                    AssignmentResult::SelectionPhase::Settling;
            }
            recordStockSettlingWinner(
                input,
                state,
                sweep,
                replayed,
                QStringLiteral("REPLAY_VALID"),
                changed);
            return changed;
        }

        static void settle(
            TurnPlan & plan,
            const AssignmentInput & input,
            std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & sweepOrder,
            AssignmentStats & stats,
            DeferredAudits & audits)
        {
            for (std::int32_t sweep = 0; sweep < SETTLING_SWEEP_CAP; ++sweep)
            {
                ++stats.settlingSweeps;
                bool moved = false;
                for (const std::int32_t slot : sweepOrder)
                {
                    ActorProgress & state = actors[static_cast<std::size_t>(slot)];
                    if (stockCoupled(input, state.pActor->engineUnitId))
                    {
                        if (settleStockActor(
                                plan,
                                input,
                                actors,
                                slot,
                                sweep,
                                stats,
                                audits))
                        {
                            moved = true;
                            ++stats.settlingMoves;
                        }
                        continue;
                    }
                    const std::int32_t previousChosen =
                        state.chosen;
                    const MilliFunds previousValue = state.seatedValue;
                    const std::vector<std::int32_t> options = incumbentFirstOptions(state);
                    unseatActor(plan, state);
                    seatBestClaimable(plan, input, state, options);
                    const bool changed =
                        state.chosen != previousChosen;
                    if (changed)
                    {
                        state.phase =
                            AssignmentResult::SelectionPhase::Settling;
                    }
                    if (input.pTrace != nullptr)
                    {
                        input.pTrace->record(
                            QStringLiteral("SETTLING_NON_STOCK"),
                            QStringLiteral(
                                "sweep=%1 actor=%2 previousCandidate=%3 previousValue=%4 winner=%5 winnerValue=%6 changed=%7")
                                .arg(sweep)
                                .arg(state.pActor->engineUnitId)
                                .arg(previousChosen)
                                .arg(previousValue)
                                .arg(state.chosen)
                                .arg(state.seatedValue)
                                .arg(traceBool(changed)));
                    }
                    if (state.seatedValue > previousValue)
                    {
                        moved = true;
                        ++stats.settlingMoves;
                    }
                }
                if (!moved)
                {
                    return;
                }
            }
        }

        static std::vector<ConflictEdge> conflictEdges(const AssignmentInput & input,
                                                       const std::vector<ActorProgress> & actors,
                                                       const std::vector<std::int32_t> & sweepOrder)
        {
            std::vector<ActorResources> resources;
            std::vector<bool> coupled;
            resources.reserve(actors.size());
            coupled.reserve(actors.size());
            for (const ActorProgress & state : actors)
            {
                resources.push_back(actorResourcesOf(state));
                coupled.push_back(stockCoupled(input, state.pActor->engineUnitId));
            }
            std::vector<ConflictEdge> edges;
            for (std::size_t left = 0; left < sweepOrder.size(); ++left)
            {
                for (std::size_t right = left + 1; right < sweepOrder.size(); ++right)
                {
                    const std::int32_t leftSlot = sweepOrder[left];
                    const std::int32_t rightSlot = sweepOrder[right];
                    const bool stockPair = coupled[static_cast<std::size_t>(leftSlot)] &&
                                           coupled[static_cast<std::size_t>(rightSlot)];
                    if (stockPair ||
                        actorsContest(resources[static_cast<std::size_t>(leftSlot)],
                                      resources[static_cast<std::size_t>(rightSlot)]))
                    {
                        edges.push_back(ConflictEdge{leftSlot, rightSlot});
                    }
                }
            }
            return edges;
        }

        static QString traceActorIds(
            const std::vector<ActorProgress> & actors,
            std::span<const std::int32_t> members)
        {
            QString text;
            for (std::size_t slot = 0;
                 slot < members.size();
                 ++slot)
            {
                if (slot > 0)
                {
                    text += QLatin1Char(',');
                }
                text += QString::number(
                    actors[static_cast<std::size_t>(
                               members[slot])]
                        .pActor->engineUnitId);
            }
            return text;
        }

        static std::vector<std::int32_t> chosenOptions(
            const std::vector<ActorProgress> & actors,
            std::span<const std::int32_t> members)
        {
            std::vector<std::int32_t> choices;
            choices.reserve(members.size());
            for (const std::int32_t slot : members)
            {
                choices.push_back(
                    actors[static_cast<std::size_t>(slot)]
                        .chosen);
            }
            return choices;
        }

        static std::vector<ActorSeat> recordSeats(const std::vector<ActorProgress> & actors,
                                                  const std::vector<std::int32_t> & members)
        {
            std::vector<ActorSeat> seats;
            seats.reserve(members.size());
            for (const std::int32_t slot : members)
            {
                const ActorProgress & state = actors[static_cast<std::size_t>(slot)];
                seats.push_back(ActorSeat{state.chosen, state.actionIndex, state.seatedValue});
            }
            return seats;
        }

        static void restoreSeats(std::vector<ActorProgress> & actors, const std::vector<std::int32_t> & members,
                                 const std::vector<ActorSeat> & seats)
        {
            for (std::size_t depth = 0; depth < members.size(); ++depth)
            {
                ActorProgress & state = actors[static_cast<std::size_t>(members[depth])];
                state.chosen = seats[depth].chosen;
                state.actionIndex = seats[depth].actionIndex;
                state.seatedValue = seats[depth].seatedValue;
            }
        }

        static void unseatMembers(TurnPlan & plan, std::vector<ActorProgress> & actors,
                                  const std::vector<std::int32_t> & members)
        {
            for (const std::int32_t slot : members)
            {
                unseatActor(plan, actors[static_cast<std::size_t>(slot)]);
            }
        }

        static std::vector<std::int32_t> allChoices(const std::vector<ActorProgress> & actors)
        {
            std::vector<std::int32_t> tuple;
            tuple.reserve(actors.size());
            for (const auto & actor : actors)
            {
                tuple.push_back(actor.chosen);
            }
            return tuple;
        }

        static bool completeRelativePlan(const TurnPlan & plan, const std::vector<ActorProgress> & actors,
                                         std::vector<std::int32_t> & order)
        {
            for (const auto & actor : actors)
            {
                if (actor.chosen == NO_CANDIDATE || !plan.hasAction(actor.actionIndex) ||
                    !isLiveState(plan.action(actor.actionIndex).state) ||
                    plan.action(actor.actionIndex).unitId != actor.pActor->engineUnitId)
                {
                    return false;
                }
            }
            auto ordered = plan;
            if (!ordered.rebuildVacateEdges(planOccupancy(ordered)).empty())
            {
                return false;
            }
            auto result = ordered.executionOrder();
            if (!result.cycleMembers.empty() || !result.blockedByCycle.empty() || result.order.size() != actors.size())
            {
                return false;
            }
            order = std::move(result.order);
            return true;
        }

        static bool bothStock(const AssignmentInput & input, const std::vector<ActorProgress> & actors,
                              const ConflictEdge & edge)
        {
            return stockCoupled(input, actors[static_cast<std::size_t>(edge.left)].pActor->engineUnitId) &&
                stockCoupled(input, actors[static_cast<std::size_t>(edge.right)].pActor->engineUnitId);
        }

        static RelativeDomain relativeDomain(
            const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & members, RelativeStockPhase phase, std::int64_t remainingStates)
        {
            RelativeDomain domain;
            domain.members = members;
            const bool joint = phase == RelativeStockPhase::Joint;
            if ((joint && (members.size() < RELATIVE_JOINT_MIN_ACTORS ||
                           members.size() > RELATIVE_JOINT_MAX_ACTORS)) ||
                (!joint && members.size() != 2))
            {
                domain.status = RelativeStockDomainStatus::UnsupportedActors;
                return domain;
            }
            const auto productLimit = joint ? RELATIVE_JOINT_PRODUCT_CAP : CLUSTER_STATE_CAP;
            for (const auto slot : members)
            {
                const auto & actor = actors[static_cast<std::size_t>(slot)];
                if (!stockCoupled(input, actor.pActor->engineUnitId))
                {
                    domain.status = RelativeStockDomainStatus::UnsupportedActors;
                    return domain;
                }
                auto options = incumbentFirstOptions(actor);
                const auto count = static_cast<std::int64_t>(options.size());
                if (count == 0 || count > productLimit / domain.product)
                {
                    domain.status = RelativeStockDomainStatus::ProductLimit;
                    domain.product = productLimit + 1;
                    return domain;
                }
                domain.product *= count;
                domain.maximumStates += domain.product;
                domain.options.push_back(std::move(options));
            }
            if (domain.maximumStates > remainingStates || (!joint && domain.maximumStates > CLUSTER_STATE_CAP))
            {
                domain.status = RelativeStockDomainStatus::EnumerationBudget;
            }
            return domain;
        }

        static RelativeAssignmentBudget relativeAssignmentBudget(
            const TurnPlan & plan, const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            const std::vector<ConflictEdge> & edges)
        {
            RelativeAssignmentBudget budget;
            budget.session = input.pStockValuer == nullptr ? nullptr : input.pStockValuer->relativeStockSession();
            if (budget.session == nullptr)
            {
                return budget;
            }
            budget.initialBalance = budget.session->remainingWork();
            budget.totalLimit = std::clamp<std::int64_t>(input.relativeStockWorkLimit, 0,
                std::min(budget.initialBalance, LIVE_PAIR_REFINEMENT_BUDGET));
            budget.pairLimit = budget.totalLimit;
            std::vector<std::int32_t> order;
            if (!completeRelativePlan(plan, actors, order))
            {
                return budget;
            }
            const auto adjacency = buildAdjacency(actors.size(), edges);
            std::vector<bool> visited(actors.size(), false);
            for (const auto root : actorSweepOrder(actors))
            {
                if (visited[static_cast<std::size_t>(root)])
                {
                    continue;
                }
                const auto members = collectComponent(root, actors, adjacency, visited);
                if (relativeDomain(input, actors, members, RelativeStockPhase::Joint,
                                   ASSIGNMENT_STATE_BUDGET).status == RelativeStockDomainStatus::Complete)
                {
                    budget.pairLimit = budget.totalLimit / 2;
                    break;
                }
            }
            return budget;
        }

        static RelativeTrial relativeTrial(
            const TurnPlan & base, const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & members, const std::vector<std::int32_t> & tuple)
        {
            RelativeTrial trial;
            if (tuple.size() != actors.size() ||
                std::find(tuple.begin(), tuple.end(), NO_CANDIDATE) != tuple.end())
            {
                return trial;
            }
            trial.plan = base;
            trial.plan.clearEdgesOfKind(PlanEdgeKind::Vacate);
            std::vector<ActorProgress> changed;
            changed.reserve(members.size());
            for (const auto slot : members)
            {
                const auto & original = actors[static_cast<std::size_t>(slot)];
                ActorProgress actor;
                actor.pActor = original.pActor;
                actor.actionIndex = original.actionIndex;
                actor.chosen = original.chosen;
                actor.seatedValue = original.seatedValue;
                unseatActor(trial.plan, actor);
                changed.push_back(std::move(actor));
            }
            for (std::size_t index = 0; index < members.size(); ++index)
            {
                const auto slot = static_cast<std::size_t>(members[index]);
                if (seatOption(trial.plan, input, changed[index], tuple[slot]).claim != ReservationResult::Granted)
                {
                    return trial;
                }
                trial.seats.push_back({changed[index].chosen, changed[index].actionIndex, changed[index].seatedValue});
            }
            for (std::size_t slot = 0; slot < actors.size(); ++slot)
            {
                const auto member = std::find(members.begin(), members.end(), static_cast<std::int32_t>(slot));
                if (member == members.end() &&
                    (tuple[slot] != actors[slot].chosen ||
                     trial.plan.action(actors[slot].actionIndex) != base.action(actors[slot].actionIndex)))
                {
                    return trial;
                }
                const auto value = member == members.end() ? actors[slot].seatedValue :
                    trial.seats[static_cast<std::size_t>(member - members.begin())].seatedValue;
                if (!RelativeStockDetail::checkedAdd(trial.economic, value))
                {
                    return trial;
                }
            }
            trial.valid = completeRelativePlan(trial.plan, actors, trial.order);
            return trial;
        }

        static bool replayRelativeClaims(
            RelativeTrial & trial, const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & tuple)
        {
            auto replayed = trial.plan;
            MilliFunds economic = 0;
            for (std::size_t slot = 0; slot < actors.size(); ++slot)
            {
                const auto & actor = actors[slot];
                const auto & candidate = actor.pActor->candidates[static_cast<std::size_t>(tuple[slot])];
                auto expected = plannedActionFrom(input.actionIds, input.unitLinks, actor.pActor->engineUnitId, candidate);
                const auto & action = trial.plan.action(actor.actionIndex);
                expected.actionIndex = action.actionIndex;
                expected.plannedDamageSteps = action.plannedDamageSteps;
                expected.state = action.state;
                if (expected != action || claimFor(replayed, actor.actionIndex, candidate) != ReservationResult::Granted ||
                    !RelativeStockDetail::checkedAdd(economic, seatedOptionValue(replayed, actor, tuple[slot])))
                {
                    return false;
                }
            }
            for (std::int32_t index = 0; index < replayed.actionCount(); ++index)
            {
                if (replayed.action(index) != trial.plan.action(index))
                {
                    return false;
                }
            }
            std::vector<std::int32_t> order;
            return economic == trial.economic && completeRelativePlan(replayed, actors, order) && order == trial.order;
        }

        static void enumerateRelativeChoices(
            const TurnPlan & plan, const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            RelativeDomain & domain, std::size_t depth, std::vector<std::int32_t> & tuple,
            RelativeStockSession & session, const RelativeStockQuote & incumbent, RelativeFrontier & frontier)
        {
            if (depth == domain.members.size())
            {
                auto trial = relativeTrial(plan, input, actors, domain.members, tuple);
                if (!trial.valid)
                {
                    return;
                }
                ++domain.claimable;
                if (tuple == allChoices(actors))
                {
                    return;
                }
                auto quote = session.quote(trial.plan, trial.economic);
                const auto comparison = session.compare(incumbent, quote);
                if (!comparison.valid())
                {
                    domain.status = RelativeStockDomainStatus::InvalidQuote;
                    frontier.valid = false;
                    return;
                }
                if (comparison.deltaUpper > 0)
                {
                    frontier.contenders.push_back({domain.members, tuple, std::move(quote), comparison});
                }
                return;
            }
            const auto slot = static_cast<std::size_t>(domain.members[depth]);
            for (const auto option : domain.options[depth])
            {
                ++domain.states;
                tuple[slot] = option;
                enumerateRelativeChoices(plan, input, actors, domain, depth + 1, tuple, session, incumbent, frontier);
                if (!frontier.valid)
                {
                    return;
                }
            }
            tuple[slot] = actors[slot].chosen;
        }

        static QString relativePhaseName(RelativeStockPhase phase)
        {
            return phase == RelativeStockPhase::Pair ? QStringLiteral("PAIR") : QStringLiteral("JOINT");
        }

        static void recordRelativeDomain(
            const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            const RelativeDomain & domain, RelativeStockPhase phase, std::int64_t contenders)
        {
            if (input.pTrace == nullptr && input.pRelativeStockObserver == nullptr)
            {
                return;
            }
            RelativeStockDomainAudit audit;
            audit.phase = phase;
            audit.status = domain.status;
            audit.product = domain.product;
            audit.states = domain.states;
            audit.claimable = domain.claimable;
            audit.contenders = contenders;
            for (const auto slot : domain.members)
            {
                audit.actors.push_back(actors[static_cast<std::size_t>(slot)].pActor->engineUnitId);
            }
            if (input.pRelativeStockObserver != nullptr)
            {
                input.pRelativeStockObserver->domain(audit);
            }
            if (input.pTrace != nullptr)
            {
                input.pTrace->record(QStringLiteral("STOCK_RELATIVE_DOMAIN"),
                    QStringLiteral("phase=%1 actors=%2 product=%3 states=%4 claimable=%5 contenders=%6 status=%7")
                        .arg(relativePhaseName(phase)).arg(traceIndices(audit.actors)).arg(domain.product)
                        .arg(domain.states).arg(domain.claimable).arg(contenders).arg(static_cast<int>(domain.status)));
            }
        }

        static void appendRelativeDomain(
            const TurnPlan & plan, const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            RelativeDomain domain, RelativeStockPhase phase, RelativeStockSession & session,
            const RelativeStockQuote & incumbent, RelativeFrontier & frontier, AssignmentStats & stats)
        {
            auto & counters = phase == RelativeStockPhase::Pair ? stats.stockPairs : stats.stockJoints;
            ++counters.domains;
            counters.candidateProduct += domain.product;
            const auto previous = frontier.contenders.size();
            if (domain.status == RelativeStockDomainStatus::Complete)
            {
                auto tuple = allChoices(actors);
                enumerateRelativeChoices(plan, input, actors, domain, 0, tuple, session, incumbent, frontier);
            }
            if (domain.status != RelativeStockDomainStatus::Complete)
            {
                ++counters.skippedDomains;
            }
            counters.enumerationStates += domain.states;
            counters.claimableLeaves += domain.claimable;
            if (phase == RelativeStockPhase::Pair)
            {
                stats.swapStates += static_cast<std::int32_t>(domain.states);
            }
            else
            {
                stats.enumerationStates += static_cast<std::int32_t>(domain.states);
            }
            recordRelativeDomain(input, actors, domain, phase,
                                 static_cast<std::int64_t>(frontier.contenders.size() - previous));
        }

        struct RelativeStepRecorder
        {
            const AssignmentInput & input;
            RelativeStockPhase phase;

            void operator()(const RelativeStockStep & step, const RelativeStockQuote & incumbent,
                            const RelativeStockQuote & challenger) const
            {
                if (input.pRelativeStockObserver != nullptr)
                {
                    input.pRelativeStockObserver->step(phase, step, incumbent, challenger);
                }
                if (input.pTrace != nullptr && input.pTrace->stockDetailsEnabled())
                {
                    input.pTrace->record(QStringLiteral("STOCK_RELATIVE_STEP"),
                        QStringLiteral("phase=%1 target=%2 cap=%3 states=%4 work=%5 beforeLower=%6 beforeUpper=%7 lower=%8 upper=%9 completed=%10 witness=%11 adopted=%12 deltaLower=%13 deltaUpper=%14 commonEnemy=%15 verifiedDual=%16 certifiedUpper=%17 catalogWork=%18 optimizerWork=%19 verifierWork=%20 tariffWork=%21 certificate=%22 localExhaustion=%23 globalFailure=%24")
                            .arg(relativePhaseName(phase)).arg(static_cast<int>(step.target)).arg(step.requestedCap)
                            .arg(step.refined.statesUsed).arg(step.refined.workUsed).arg(step.before.lower)
                            .arg(step.before.upper).arg(step.refined.interval.lower).arg(step.refined.interval.upper)
                            .arg(traceBool(step.refined.searchCompleted)).arg(traceBool(step.refined.lowerWitnessReplays))
                            .arg(traceBool(step.adopted)).arg(step.comparison.deltaLower).arg(step.comparison.deltaUpper)
                            .arg(traceBool(step.comparison.commonEnemyCancelled))
                            .arg(traceBool(step.verifiedDual)).arg(step.dual.certifiedUpper)
                            .arg(step.dual.catalogWork).arg(step.dual.optimizerWork)
                            .arg(step.dual.verifierWork).arg(step.dual.tariffWork)
                            .arg(step.dual.certificateId)
                            .arg(traceBool(step.dual.locallyBudgetExhausted))
                            .arg(traceBool(step.dual.globalFailure)));
                }
            }
        };

        static QString relativeQuoteFields(const QString & prefix, const RelativeStockQuote & quote)
        {
            return QStringLiteral(
                " %1Economic=%2 %1Owned=%3 %1OurLower=%4 %1OurUpper=%5 %1EnemyLower=%6 %1EnemyUpper=%7 %1Captured=%8 %1OurWitness=%9 %1EnemyWitness=%10 %1OurCompleted=%11 %1EnemyCompleted=%12 %1AbsoluteExact=%13 %1OurUpperCertificate=%14")
                .arg(prefix).arg(quote.economic).arg(quote.owned).arg(quote.ours.lower).arg(quote.ours.upper)
                .arg(quote.enemy.lower).arg(quote.enemy.upper).arg(traceIndices(quote.capturedColumns))
                .arg(traceBool(quote.ourLowerWitnessReplays)).arg(traceBool(quote.enemyLowerWitnessReplays))
                .arg(traceBool(quote.ourSearchCompleted)).arg(traceBool(quote.enemySearchCompleted))
                .arg(traceBool(quote.absoluteExact()))
                .arg(static_cast<qulonglong>(
                    quote.ourUpperCertificateId));
        }

        static void recordRelativeComparison(
            const AssignmentInput & input, RelativeStockPhase phase, const RelativeContender & contender,
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
            const RelativeStockRefinementResult & result, bool accepted)
        {
            if (input.pRelativeStockObserver != nullptr)
            {
                input.pRelativeStockObserver->comparison(phase, contender.tuple, incumbent, challenger, result, accepted);
            }
            if (input.pTrace == nullptr)
            {
                return;
            }
            const auto & comparison = result.comparison;
            QString fields = QStringLiteral(
                "phase=%1 candidates=%2 deltaLower=%3 deltaUpper=%4 commonEnemy=%5 accepted=%6 states=%7 enemyStates=%8 work=%9 stop=%10")
                .arg(relativePhaseName(phase)).arg(traceIndices(contender.tuple)).arg(comparison.deltaLower)
                .arg(comparison.deltaUpper).arg(traceBool(comparison.commonEnemyCancelled)).arg(traceBool(accepted))
                .arg(result.statesUsed).arg(result.enemyStatesUsed).arg(result.workUsed).arg(static_cast<int>(result.stop));
            if (input.pTrace->stockDetailsEnabled())
            {
                fields += relativeQuoteFields(QStringLiteral("incumbent"), incumbent);
                fields += relativeQuoteFields(QStringLiteral("challenger"), challenger);
            }
            input.pTrace->record(QStringLiteral("STOCK_RELATIVE_RESULT"), fields);
        }

        static bool refreshRelativeFrontier(
            RelativeFrontier & frontier,
            RelativeStockSession & session,
            const RelativeStockQuote & incumbent,
            const std::vector<ActorProgress> & actors)
        {
            for (auto & contender : frontier.contenders)
            {
                contender.quote = session.quote(
                    contender.quote.key,
                    contender.quote.economic);
                contender.comparison =
                    session.compare(
                        incumbent,
                        contender.quote);
                if (!contender.comparison.valid())
                {
                    return false;
                }
            }
            const auto canonicalActorOrder =
                actorSweepOrder(actors);
            std::stable_sort(frontier.contenders.begin(), frontier.contenders.end(),
                [&actors, &canonicalActorOrder](const RelativeContender & left, const RelativeContender & right)
            {
                if (left.comparison.improves() !=
                    right.comparison.improves())
                {
                    return left.comparison.improves();
                }
                if (left.comparison.improves())
                {
                    if (left.comparison.deltaLower !=
                        right.comparison.deltaLower)
                    {
                        return left.comparison.deltaLower >
                            right.comparison.deltaLower;
                    }
                    if (left.comparison.deltaUpper !=
                        right.comparison.deltaUpper)
                    {
                        return left.comparison.deltaUpper >
                            right.comparison.deltaUpper;
                    }
                    if (left.members != right.members)
                    {
                        return canonicalActorSupportFirst(
                            actors,
                            left.members,
                            right.members);
                    }
                    return canonicalCandidateTupleFirst(
                        canonicalActorOrder,
                        left.tuple,
                        right.tuple);
                }
                if (left.visited != right.visited)
                {
                    return !left.visited;
                }
                return relativeStockUpperFirst(left.comparison, left.tuple, right.comparison, right.tuple);
            });
            return true;
        }

        static bool installRelativeContender(
            TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
            RelativeStockSession & session, const RelativeContender & contender,
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
            RelativeStockPhase phase, AssignmentStats & stats)
        {
            auto trial = relativeTrial(plan, input, actors, contender.members, contender.tuple);
            RelativeTrial incumbentTrial;
            incumbentTrial.plan = plan;
            incumbentTrial.economic = seatedPlanTotal(plan, actors);
            incumbentTrial.valid = completeRelativePlan(incumbentTrial.plan, actors, incumbentTrial.order);
            if (!trial.valid || !incumbentTrial.valid ||
                !replayRelativeClaims(trial, input, actors, contender.tuple) ||
                !replayRelativeClaims(incumbentTrial, input, actors, allChoices(actors)) ||
                !replayRelativeStockProof(session, incumbent, challenger,
                    session.quote(plan, incumbentTrial.economic), session.quote(trial.plan, trial.economic)))
            {
                ++stats.replayFailures;
                return false;
            }
            plan = std::move(trial.plan);
            for (std::size_t index = 0; index < contender.members.size(); ++index)
            {
                auto & actor = actors[static_cast<std::size_t>(contender.members[index])];
                if (actor.chosen != trial.seats[index].chosen)
                {
                    actor.phase = phase == RelativeStockPhase::Pair ? AssignmentResult::SelectionPhase::PairRefinement :
                        AssignmentResult::SelectionPhase::Cluster;
                }
                actor.chosen = trial.seats[index].chosen;
                actor.actionIndex = trial.seats[index].actionIndex;
                actor.seatedValue = trial.seats[index].seatedValue;
            }
            invalidateCompleteValues(actors);
            return true;
        }

        static std::size_t selectRelativeContender(
            const RelativeFrontier & frontier, RelativeStockSchedule schedule,
            const std::map<std::vector<std::int32_t>, std::int32_t> & domainSteps)
        {
            if (schedule == RelativeStockSchedule::Current)
            {
                return 0;
            }
            std::size_t selected = frontier.contenders.size();
            const auto steps = [&](const RelativeContender & contender)
            {
                const auto found = domainSteps.find(contender.members);
                return found == domainSteps.end() ? 0 : found->second;
            };
            for (std::size_t index = 0; index < frontier.contenders.size(); ++index)
            {
                const auto & contender = frontier.contenders[index];
                if (contender.visited || contender.comparison.deltaUpper <= 0)
                {
                    continue;
                }
                if (contender.comparison.improves())
                {
                    return index;
                }
                if (selected == frontier.contenders.size() ||
                    (schedule == RelativeStockSchedule::FairDomains &&
                     steps(contender) < steps(frontier.contenders[selected])))
                {
                    selected = index;
                }
            }
            return selected;
        }

        static void recordRelativeContender(
            const AssignmentInput & input, const std::vector<ActorProgress> & actors,
            RelativeStockPhase phase, const RelativeContender & contender,
            const RelativeStockQuote & incumbent, std::int64_t rank, bool selected)
        {
            RelativeStockFrontierAudit audit;
            audit.phase = phase;
            audit.rank = rank;
            audit.tuple = contender.tuple;
            audit.incumbent = incumbent;
            audit.challenger = contender.quote;
            audit.comparison = contender.comparison;
            audit.selected = selected;
            for (const auto member : contender.members)
            {
                audit.actors.push_back(actors[static_cast<std::size_t>(member)].pActor->engineUnitId);
            }
            for (std::size_t slot = 0; slot < actors.size(); ++slot)
            {
                const auto & actor = *actors[slot].pActor;
                const auto option = contender.tuple[slot];
                const auto & candidate = actor.candidates[static_cast<std::size_t>(option)];
                const auto kind = planBundleKindOf(candidate.bundle);
                audit.actions.push_back({actor.engineUnitId, option, kind,
                                         candidate.bundle.destination, isCaptureKind(kind)});
            }
            input.pRelativeStockObserver->contender(audit);
        }

        static bool improveRelativeFrontier(
            TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
            RelativeFrontier & frontier, const RelativeAssignmentBudget & budget, RelativeStockPhase phase,
            AssignmentStats & stats, RelativeStockTiming & timing, RelativeStockSlice slice)
        {
            using Clock = std::chrono::steady_clock;
            auto & counters = phase == RelativeStockPhase::Pair ? stats.stockPairs : stats.stockJoints;
            auto & session = *budget.session;
            const auto started = Clock::now();
            const auto replayBefore = timing.replayNanos;
            bool accepted = false;
            bool recordedFrontier = false;
            bool proofWorkExhausted = false;
            const bool boundedStep = input.relativeStockSchedule == RelativeStockSchedule::Requeue ||
                input.relativeStockSchedule == RelativeStockSchedule::FairDomains;
            std::map<std::vector<std::int32_t>, std::int32_t> domainSteps;
            const bool recordFrontier = input.pRelativeStockObserver != nullptr &&
                input.pRelativeStockObserver->frontierReceiptsEnabled();
            while (frontier.valid)
            {
                auto incumbent = session.quote(plan, seatedPlanTotal(plan, actors));
                if (!refreshRelativeFrontier(
                        frontier,
                        session,
                        incumbent,
                        actors))
                {
                    break;
                }
                if (recordFrontier && !recordedFrontier)
                {
                    input.pRelativeStockObserver->beginFrontier(
                        phase, static_cast<std::int64_t>(frontier.contenders.size()), budget.remaining(phase));
                    for (std::size_t rank = 0; rank < frontier.contenders.size(); ++rank)
                    {
                        recordRelativeContender(input, actors, phase, frontier.contenders[rank], incumbent,
                                                static_cast<std::int64_t>(rank), false);
                    }
                    recordedFrontier = true;
                }
                const auto selected = selectRelativeContender(frontier, input.relativeStockSchedule, domainSteps);
                if (selected >= frontier.contenders.size() || frontier.contenders[selected].visited ||
                    frontier.contenders[selected].comparison.deltaUpper <= 0)
                {
                    break;
                }
                auto & contender = frontier.contenders[selected];
                if (proofWorkExhausted && !contender.comparison.improves())
                {
                    break;
                }
                if (budget.remaining(phase) == 0 && !contender.comparison.improves())
                {
                    ++counters.budgetExhaustions;
                    break;
                }
                contender.visited = true;
                if (recordFrontier)
                {
                    recordRelativeContender(input, actors, phase, contender, incumbent,
                                            static_cast<std::int64_t>(selected), true);
                }
                auto challenger = contender.quote;
                const auto result = refineRelativeStock(session, incumbent, challenger,
                    {.workLimit = budget.remaining(phase),
                     .slice = boundedStep ? RelativeStockSlice::Doubling : slice,
                     .maximumSteps = boundedStep ? 1 : 0},
                    contender.cursor, RelativeStepRecorder{input, phase});
                contender.visited = result.stop != RelativeStockStop::StepLimit;
                contender.quote = challenger;
                contender.comparison = result.comparison;
                if (result.workUsed > 0)
                {
                    ++domainSteps[contender.members];
                }
                ++counters.comparisons;
                counters.statesUsed += result.statesUsed;
                counters.enemyStatesUsed += result.enemyStatesUsed;
                counters.workUsed += result.workUsed;
                if (result.budgetExhausted)
                {
                    ++counters.budgetExhaustions;
                }
                RelativeContender recordedContender =
                    contender;
                RelativeStockQuote recordedChallenger =
                    challenger;
                RelativeStockRefinementResult recordedResult =
                    result;
                if (result.comparison.valid() &&
                    result.stop !=
                        RelativeStockStop::InvalidComparison &&
                    result.stop !=
                        RelativeStockStop::InvalidRefinement)
                {
                    if (!refreshRelativeFrontier(
                            frontier,
                            session,
                            incumbent,
                            actors))
                    {
                        break;
                    }
                    const auto winner = std::find_if(
                        frontier.contenders.begin(),
                        frontier.contenders.end(),
                        [](const RelativeContender & candidate)
                    {
                        return candidate.comparison.improves();
                    });
                    if (winner !=
                        frontier.contenders.end())
                    {
                        recordedContender = *winner;
                        recordedChallenger =
                            winner->quote;
                        recordedResult.comparison =
                            winner->comparison;
                        const auto replayStarted =
                            Clock::now();
                        accepted =
                            installRelativeContender(
                                plan,
                                input,
                                actors,
                                session,
                                *winner,
                                incumbent,
                                winner->quote,
                                phase,
                                stats);
                        timing.replayNanos +=
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                    Clock::now() -
                                    replayStarted)
                                .count();
                        if (accepted)
                        {
                            ++counters.improvements;
                        }
                    }
                }
                recordRelativeComparison(
                    input,
                    phase,
                    recordedContender,
                    incumbent,
                    recordedChallenger,
                    recordedResult,
                    accepted);
                proofWorkExhausted = result.stop == RelativeStockStop::WorkBudget;
                if (accepted || result.stop == RelativeStockStop::InvalidComparison ||
                    result.stop == RelativeStockStop::InvalidRefinement ||
                    (proofWorkExhausted && input.relativeStockSchedule == RelativeStockSchedule::Current) ||
                    result.comparison.improves())
                {
                    break;
                }
            }
            counters.unresolved = 0;
            const auto current = session.quote(plan, seatedPlanTotal(plan, actors));
            for (const auto & contender : frontier.contenders)
            {
                const auto comparison = session.compare(current, session.quote(contender.quote.key, contender.quote.economic));
                if (!comparison.valid() || comparison.deltaUpper > 0)
                {
                    ++counters.unresolved;
                }
            }
            timing.refinementNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - started).count() - (timing.replayNanos - replayBefore);
            return accepted;
        }

        static bool improveRelativePairs(
            TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
            const std::vector<ConflictEdge> & edges, const RelativeAssignmentBudget & budget,
            AssignmentStats & stats, RelativeStockTiming & timing, RelativeStockSlice slice)
        {
            using Clock = std::chrono::steady_clock;
            std::vector<std::int32_t> order;
            if (!completeRelativePlan(plan, actors, order))
            {
                return false;
            }
            const auto started = Clock::now();
            const auto incumbent = budget.session->quote(plan, seatedPlanTotal(plan, actors));
            RelativeFrontier frontier;
            for (const auto & edge : edges)
            {
                if (!bothStock(input, actors, edge))
                {
                    continue;
                }
                auto domain = relativeDomain(input, actors, {edge.left, edge.right}, RelativeStockPhase::Pair,
                    ASSIGNMENT_STATE_BUDGET - stats.swapStates - stats.enumerationStates);
                appendRelativeDomain(plan, input, actors, std::move(domain), RelativeStockPhase::Pair,
                                     *budget.session, incumbent, frontier, stats);
                if (!frontier.valid)
                {
                    break;
                }
            }
            timing.enumerationNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            return frontier.valid && improveRelativeFrontier(plan, input, actors, frontier, budget,
                                                             RelativeStockPhase::Pair, stats, timing, slice);
        }

        static LiveRefinementResult refineLegacyPairs(
            const AssignmentInput & input, const RelativeAssignmentBudget & budget, AssignmentStats & stats)
        {
            const auto maximumWork = budget.session != nullptr ? budget.remaining(RelativeStockPhase::Pair) :
                std::max<std::int64_t>(0, std::clamp<std::int64_t>(
                    input.relativeStockWorkLimit, 0, LIVE_PAIR_REFINEMENT_BUDGET) - stats.mixedPairs.workUsed);
            const auto before = budget.session != nullptr ? budget.session->remainingWork() : 0;
            auto result = input.pStockValuer->refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, maximumWork);
            result.valid = result.valid && result.workUsed >= 0 && result.workUsed <= maximumWork &&
                result.statesUsed >= 0 && result.statesUsed <= result.workUsed &&
                result.enemyStatesUsed >= 0 && result.enemyStatesUsed <= result.statesUsed &&
                (budget.session == nullptr || before - budget.session->remainingWork() == result.workUsed);
            auto & counters = stats.mixedPairs;
            ++counters.calls;
            counters.keysRefined += result.keysRefined;
            counters.statesUsed += result.statesUsed;
            counters.enemyStatesUsed += result.enemyStatesUsed;
            counters.workUsed += result.workUsed;
            counters.budgetExhaustions += result.budgetExhausted ? 1 : 0;
            counters.failures += result.valid ? 0 : 1;
            if (input.pRelativeStockObserver != nullptr)
            {
                input.pRelativeStockObserver->mixedBoundary(maximumWork, result);
            }
            if (input.pTrace != nullptr && input.pTrace->stockDetailsEnabled())
            {
                input.pTrace->record(QStringLiteral("MIXED_REFINEMENT_BOUNDARY"),
                    QStringLiteral("allowance=%1 keys=%2 states=%3 enemyStates=%4 work=%5 refined=%6 budgetExhausted=%7 valid=%8")
                        .arg(maximumWork).arg(result.keysRefined).arg(result.statesUsed).arg(result.enemyStatesUsed)
                        .arg(result.workUsed).arg(traceBool(result.refined)).arg(traceBool(result.budgetExhausted))
                        .arg(traceBool(result.valid)));
            }
            return result;
        }

        static void improveBySwaps(
            TurnPlan & plan,
            const AssignmentInput & input,
            std::vector<ActorProgress> & actors,
            const std::vector<ConflictEdge> & edges,
            AssignmentStats & stats,
            DeferredAudits & audits,
            const RelativeAssignmentBudget & relativeBudget,
            RelativeStockTiming & relativeTiming)
        {
            for (std::int32_t sweep = 0; sweep < SWAP_SWEEP_CAP; ++sweep)
            {
                bool improved = false;
                bool legacyLiveEncountered = false;
                for (const ConflictEdge & edge : edges)
                {
                    if (relativeBudget.session != nullptr && bothStock(input, actors, edge))
                    {
                        continue;
                    }
                    if (searchBudgetExhausted(stats))
                    {
                        return;
                    }
                    const std::vector<std::int32_t> pair{
                        edge.left,
                        edge.right,
                    };
                    std::vector<std::int32_t>
                        incumbentChoices;
                    if (input.pTrace != nullptr)
                    {
                        incumbentChoices =
                            chosenOptions(actors, pair);
                    }
                    ExactSearchOutcome outcome =
                        runExactSearch(
                            plan,
                            input,
                            actors,
                            pair,
                            NO_CANDIDATE_CAP,
                            stats,
                            SearchPricingMode::PairSwapInterval);
                    stats.swapStates += outcome.states;
                    if (outcome.pairPricing == PairPricingResult::LiveInterval)
                    {
                        legacyLiveEncountered = true;
                        ++stats.mixedPairs.domains;
                    }
                    if (outcome.improved)
                    {
                        ++stats.swapImprovements;
                        improved = true;
                        if (input.pTrace != nullptr)
                        {
                            invalidateCompleteValues(actors);
                        }
                        if (outcome.pairAudit.has_value())
                        {
                            std::vector<std::int32_t>
                                actorIds;
                            actorIds.reserve(pair.size());
                            for (const std::int32_t slot : pair)
                            {
                                actorIds.push_back(
                                    actors[
                                        static_cast<std::size_t>(
                                            slot)]
                                        .pActor
                                        ->engineUnitId);
                            }
                            audits.pairAudits.push_back(
                                PairExactAuditRequest{
                                    .sweep = sweep,
                                    .actors =
                                        std::move(actorIds),
                                    .incumbentLower =
                                        outcome.incumbentLower,
                                    .incumbentUpper =
                                        outcome.incumbentUpper,
                                    .winnerLower =
                                        outcome.resultLower,
                                    .winnerUpper =
                                        outcome.resultUpper,
                                    .snapshot =
                                        std::move(
                                            *outcome.pairAudit),
                                });
                        }
                    }
                    if (input.pTrace != nullptr)
                    {
                        const std::vector<std::int32_t>
                            finalChoices =
                                chosenOptions(
                                    actors, pair);
                        if (outcome.improved)
                        {
                            for (std::size_t depth = 0;
                             depth < pair.size();
                             ++depth)
                            {
                                if (incumbentChoices[depth] !=
                                    finalChoices[depth])
                                {
                                    actors[static_cast<std::size_t>(
                                               pair[depth])]
                                        .phase =
                                        AssignmentResult::SelectionPhase::
                                            PairRefinement;
                                }
                            }
                        }
                        input.pTrace->record(
                            QStringLiteral("PAIR_RESULT"),
                            QStringLiteral(
                                "sweep=%1 actors=%2 incumbent=%3 final=%4 states=%5 accepted=%6 aborted=%7 liveFailure=%8 requestedPricing=%9 effectivePricing=%10 incumbentLower=%11 incumbentUpper=%12 winnerLower=%13 winnerUpper=%14 winnerKnown=%15 winnerExact=%16 reason=%17")
                                .arg(sweep)
                                .arg(traceActorIds(actors, pair))
                                .arg(traceIndices(
                                    incumbentChoices))
                                .arg(traceIndices(finalChoices))
                                .arg(outcome.states)
                                .arg(traceBool(
                                    outcome.improved))
                                .arg(traceBool(
                                    outcome.aborted))
                                .arg(traceBool(
                                    outcome.liveFailure))
                                .arg(tracePairRequestedPricing(
                                    outcome.pairPricing))
                                .arg(tracePairPricing(
                                    outcome.pairPricing))
                                .arg(outcome.incumbentLower)
                                .arg(outcome.incumbentUpper)
                                .arg(outcome.resultLower)
                                .arg(outcome.resultUpper)
                                .arg(traceBool(
                                    outcome.resultKnown))
                                .arg(traceBool(
                                    outcome.resultKnown &&
                                    outcome.resultLower ==
                                        outcome.resultUpper))
                                .arg(tracePairResultReason(
                                    outcome)));
                    }
                }
                bool refined = false;
                if (legacyLiveEncountered)
                {
                    const auto boundary = refineLegacyPairs(input, relativeBudget, stats);
                    if (!boundary.valid)
                    {
                        return;
                    }
                    refined = boundary.refined;
                }
                if (relativeBudget.session != nullptr)
                {
                    const auto slice = legacyLiveEncountered ? RelativeStockSlice::Doubling : RelativeStockSlice::Direct;
                    const bool relativeImproved = improveRelativePairs(
                        plan, input, actors, edges, relativeBudget, stats, relativeTiming, slice);
                    improved = improved || relativeImproved;
                    stats.swapImprovements += relativeImproved ? 1 : 0;
                }
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled())
                {
                    input.pTrace->record(
                        QStringLiteral("PAIR_REFINEMENT_BOUNDARY"),
                        QStringLiteral(
                            "sweep=%1 phase=BETWEEN_SWAP_SWEEPS refined=%2")
                            .arg(sweep)
                            .arg(traceBool(refined)));
                }
                if (!improved && !refined)
                {
                    return;
                }
            }
        }

        static std::vector<std::vector<std::int32_t>> buildAdjacency(
            std::size_t actorCount, const std::vector<ConflictEdge> & edges)
        {
            std::vector<std::vector<std::int32_t>> adjacency(actorCount);
            for (const ConflictEdge & edge : edges)
            {
                adjacency[static_cast<std::size_t>(edge.left)].push_back(edge.right);
                adjacency[static_cast<std::size_t>(edge.right)].push_back(edge.left);
            }
            return adjacency;
        }

        static std::vector<std::int32_t> collectComponent(
            std::int32_t root, const std::vector<ActorProgress> & actors,
            const std::vector<std::vector<std::int32_t>> & adjacency, std::vector<bool> & visited)
        {
            std::vector<std::int32_t> members;
            std::vector<std::int32_t> frontier{root};
            visited[static_cast<std::size_t>(root)] = true;
            while (!frontier.empty())
            {
                const std::int32_t current = frontier.back();
                frontier.pop_back();
                members.push_back(current);
                for (const std::int32_t neighbour : adjacency[static_cast<std::size_t>(current)])
                {
                    if (!visited[static_cast<std::size_t>(neighbour)])
                    {
                        visited[static_cast<std::size_t>(neighbour)] = true;
                        frontier.push_back(neighbour);
                    }
                }
            }
            std::sort(members.begin(), members.end(), ActorIdOrder{&actors});
            return members;
        }

        static MilliFunds seatedTotal(const std::vector<ActorProgress> & actors,
                                      const std::vector<std::int32_t> & members)
        {
            MilliFunds total = 0;
            for (const std::int32_t slot : members)
            {
                total += actors[static_cast<std::size_t>(slot)].seatedValue;
            }
            return total;
        }

        static MilliFunds seatedPlanTotal(
            const TurnPlan & plan,
            const std::vector<ActorProgress> & actors)
        {
            MilliFunds total = 0;
            for (const ActorProgress & state : actors)
            {
                if (state.actionIndex != NO_ACTION &&
                    isLiveState(
                        plan.action(state.actionIndex).state))
                {
                    total += state.seatedValue;
                }
            }
            return total;
        }

        static void buildSearchOptions(
            const std::vector<ActorProgress> & actors,
            ClusterSearch & search,
            std::int32_t candidateCap)
        {
            for (const std::int32_t slot : search.members)
            {
                const ActorProgress & state = actors[static_cast<std::size_t>(slot)];
                search.options.push_back(cappedOptions(
                    incumbentFirstOptions(state),
                    candidateCap));
            }
            search.ceilings.assign(search.members.size() + 1, 0);
            for (std::size_t depth = search.members.size(); depth > 0; --depth)
            {
                const ActorProgress & state =
                    actors[static_cast<std::size_t>(search.members[depth - 1])];
                MilliFunds best = 0;
                for (const std::int32_t option : search.options[depth - 1])
                {
                    best = std::max(best, optionValue(state, option));
                }
                search.ceilings[depth - 1] = best + search.ceilings[depth];
            }
            search.current.assign(search.members.size(), NO_CANDIDATE);
        }

        static void searchCluster(TurnPlan & plan, const AssignmentInput & input,
                                  std::vector<ActorProgress> & actors, ClusterSearch & search,
                                  std::size_t depth, MilliFunds total, AssignmentStats & stats)
        {
            if (depth == search.members.size())
            {
                if (search.stockLive &&
                    search.pricingMode ==
                        SearchPricingMode::PairSwapInterval)
                {
                    const MilliFunds economic =
                        search.fixedEconomic + total;
                    const LivePlanStockQuote quote =
                        input.pStockValuer->livePlanStock(
                            plan,
                            economic,
                            true);
                    if (!quote.valid ||
                        !quote.lowerWitnessReplays)
                    {
                        if (input.pTrace != nullptr &&
                            input.pTrace->stockDetailsEnabled() &&
                            search.members.size() == 2)
                        {
                            input.pTrace->record(
                                QStringLiteral("PAIR_COMPARE"),
                                QStringLiteral(
                                    "actors=%1 challenger=%2 economic=%3 stockLower=NA stockUpper=NA challengerLower=NA challengerUpper=NA incumbentLower=%4 incumbentUpper=%5 valid=%6 lowerWitnessReplays=%7 states=%8 reason=LIVE_INTERVAL_FAILURE accepted=false")
                                    .arg(traceActorIds(
                                        actors,
                                        search.members))
                                    .arg(traceIndices(
                                        search.current))
                                    .arg(economic)
                                    .arg(search.incumbentLower)
                                    .arg(search.incumbentUpper)
                                    .arg(traceBool(quote.valid))
                                    .arg(traceBool(
                                        quote.lowerWitnessReplays))
                                    .arg(search.states));
                        }
                        search.liveFailure = true;
                        search.aborted = true;
                        return;
                    }
                    const MilliFunds lower =
                        economic +
                        quote.stockAbsolute.lower;
                    const MilliFunds upper =
                        economic +
                        quote.stockAbsolute.upper;
                    const bool pruned =
                        upper <= search.incumbentLower;
                    const bool accepted =
                        lower > search.incumbentUpper;
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled() &&
                        search.members.size() == 2)
                    {
                        input.pTrace->record(
                            QStringLiteral("PAIR_COMPARE"),
                            QStringLiteral(
                                "actors=%1 challenger=%2 economic=%3 stockLower=%4 stockUpper=%5 challengerLower=%6 challengerUpper=%7 incumbentLower=%8 incumbentUpper=%9 exact=%10 completeValue=%11 states=%12 reason=%13 accepted=%14")
                                .arg(traceActorIds(
                                    actors,
                                    search.members))
                                .arg(traceIndices(
                                    search.current))
                                .arg(economic)
                                .arg(quote.stockAbsolute.lower)
                                .arg(quote.stockAbsolute.upper)
                                .arg(lower)
                                .arg(upper)
                                .arg(search.incumbentLower)
                                .arg(search.incumbentUpper)
                                .arg(traceBool(lower == upper))
                                .arg(
                                    lower == upper
                                        ? QString::number(lower)
                                        : QStringLiteral("NA"))
                                .arg(search.states)
                                .arg(
                                    pruned
                                        ? QStringLiteral(
                                              "UPPER_NOT_ABOVE_INCUMBENT_LOWER")
                                        : accepted
                                              ? QStringLiteral(
                                                    "LOWER_ABOVE_INCUMBENT_UPPER")
                                              : QStringLiteral(
                                                    "INTERVAL_OVERLAP"))
                                .arg(traceBool(accepted)));
                    }
                    if (upper <= search.incumbentLower)
                    {
                        return;
                    }
                    if (lower > search.incumbentUpper)
                    {
                        search.bestFeasibleEconomic = economic;
                        search.bestFeasibleQuote = quote;
                        search.incumbentLower = lower;
                        search.incumbentUpper = upper;
                        search.bestFeasiblePlan =
                            search.current;
                    }
                    return;
                }
                MilliFunds value = total;
                if (search.stockLive)
                {
                    value += planStockDelta(input, plan);
                }
                const bool accepted =
                    !search.hasBest || value > search.bestTotal;
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled() &&
                    search.members.size() == 2)
                {
                    input.pTrace->record(
                        QStringLiteral("PAIR_COMPARE"),
                        QStringLiteral(
                            "actors=%1 challenger=%2 challengerLower=%3 challengerUpper=%3 incumbentLower=%4 incumbentUpper=%4 completeValue=%3 incumbentComplete=%4 exact=true states=%5 reason=%6 accepted=%7")
                            .arg(traceActorIds(
                                actors,
                                search.members))
                            .arg(traceIndices(search.current))
                            .arg(value)
                            .arg(
                                search.hasBest
                                    ? QString::number(
                                          search.bestTotal)
                                    : QStringLiteral("NONE"))
                            .arg(search.states)
                            .arg(
                                accepted
                                    ? QStringLiteral(
                                          "COMPLETE_ABOVE_INCUMBENT")
                                    : QStringLiteral(
                                          "COMPLETE_NOT_ABOVE_INCUMBENT"))
                            .arg(traceBool(accepted)));
                }
                if (accepted)
                {
                    search.bestTotal = value;
                    search.legacyBest = search.current;
                    search.hasBest = true;
                }
                return;
            }
            ActorProgress & state = actors[static_cast<std::size_t>(search.members[depth])];
            for (const std::int32_t option : search.options[depth])
            {
                if (search.states >= CLUSTER_STATE_CAP ||
                    searchBudgetExhausted(stats, search.states))
                {
                    search.aborted = true;
                    return;
                }
                ++search.states;
                const MilliFunds seatedBase =
                    search.stockLive &&
                            search.pricingMode ==
                                SearchPricingMode::PairSwapInterval
                        ? search.fixedEconomic + total
                        : total;
                const MilliFunds propertyOrigin =
                    search.pricingMode ==
                            SearchPricingMode::PairSwapInterval
                        ? 0
                        : search.propertyOriginInSearchBasis;
                const MilliFunds incumbentThreshold =
                    search.pricingMode ==
                            SearchPricingMode::PairSwapInterval
                        ? search.incumbentLower
                        : search.bestTotal;
                const MilliFunds upper =
                    assignmentUpperBound(
                        seatedBase,
                        optionValue(state, option),
                        search.ceilings[depth + 1],
                        search.propertyCeilingInSearchBasis,
                        propertyOrigin);
                if (search.hasBest &&
                    upper <= incumbentThreshold)
                {
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled() &&
                        search.members.size() == 2)
                    {
                        input.pTrace->record(
                            QStringLiteral("PAIR_PRUNE"),
                            QStringLiteral(
                                "actors=%1 depth=%2 partial=%3 option=%4 upper=%5 incumbent=%6 states=%7 reason=SEARCH_UPPER_BOUND")
                                .arg(traceActorIds(
                                    actors,
                                    search.members))
                                .arg(depth)
                                .arg(traceIndices(
                                    std::span<const std::int32_t>(
                                        search.current.data(),
                                        depth)))
                                .arg(option)
                                .arg(upper)
                                .arg(incumbentThreshold)
                                .arg(search.states));
                    }
                    return;
                }
                const SeatOutcome outcome = seatOption(plan, input, state, option);
                if (outcome.claim != ReservationResult::Granted)
                {
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled() &&
                        search.members.size() == 2)
                    {
                        input.pTrace->record(
                            QStringLiteral("PAIR_REJECTION"),
                            QStringLiteral(
                                "actors=%1 depth=%2 actor=%3 option=%4 claim=%5 states=%6")
                                .arg(traceActorIds(
                                    actors,
                                    search.members))
                                .arg(depth)
                                .arg(state.pActor->engineUnitId)
                                .arg(option)
                                .arg(traceReservationResult(
                                    outcome.claim))
                                .arg(search.states));
                    }
                    continue;
                }
                search.current[depth] = option;
                searchCluster(plan, input, actors, search, depth + 1,
                              total + outcome.value, stats);
                unseatActor(plan, state);
                if (search.aborted)
                {
                    return;
                }
            }
        }

        static bool replayLiveSolution(
            TurnPlan & plan,
            const AssignmentInput & input,
            std::vector<ActorProgress> & actors,
            const ClusterSearch & search,
            const TurnPlan & snapshot,
            const std::vector<ActorSeat> & seats,
            AssignmentStats & stats)
        {
            unseatMembers(plan, actors, search.members);
            bool complete = true;
            for (std::size_t depth = 0;
                 depth < search.members.size();
                 ++depth)
            {
                ActorProgress & state =
                    actors[static_cast<std::size_t>(
                        search.members[depth])];
                const SeatOutcome outcome =
                    seatOption(
                        plan,
                        input,
                        state,
                        search.bestFeasiblePlan[depth]);
                if (outcome.claim !=
                    ReservationResult::Granted)
                {
                    complete = false;
                    break;
                }
            }
            const MilliFunds economic =
                seatedPlanTotal(plan, actors);
            const LivePlanStockQuote replayed =
                complete
                    ? input.pStockValuer->livePlanStock(
                          plan,
                          economic,
                          false)
                    : LivePlanStockQuote{};
            if (complete &&
                replayed.valid &&
                replayed.lowerWitnessReplays &&
                replayed.key ==
                    search.bestFeasibleQuote.key &&
                economic +
                        replayed.stockAbsolute.lower ==
                    search.incumbentLower &&
                economic +
                        replayed.stockAbsolute.upper ==
                    search.incumbentUpper)
            {
                return true;
            }
            ++stats.replayFailures;
            plan = snapshot;
            restoreSeats(actors, search.members, seats);
            return false;
        }

        static bool replaySolution(TurnPlan & plan, const AssignmentInput & input,
                                   std::vector<ActorProgress> & actors, const ClusterSearch & search,
                                   const TurnPlan & snapshot, const std::vector<ActorSeat> & seats,
                                   AssignmentStats & stats)
        {
            unseatMembers(plan, actors, search.members);
            MilliFunds replayed = 0;
            bool complete = true;
            for (std::size_t depth = 0; depth < search.members.size(); ++depth)
            {
                ActorProgress & state =
                    actors[static_cast<std::size_t>(search.members[depth])];
                const SeatOutcome outcome =
                    seatOption(
                        plan,
                        input,
                        state,
                        search.legacyBest[depth]);
                if (outcome.claim != ReservationResult::Granted)
                {
                    complete = false;
                    break;
                }
                replayed += outcome.value;
            }
            if (complete && search.stockLive)
            {
                replayed += planStockDelta(input, plan);
            }
            if (complete && replayed == search.bestTotal)
            {
                return true;
            }
            ++stats.replayFailures;
            plan = snapshot;
            restoreSeats(actors, search.members, seats);
            return false;
        }

        static ExactSearchOutcome runExactSearch(
            TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & members,
            std::int32_t candidateCap,
            AssignmentStats & stats,
            SearchPricingMode pricingMode)
        {
            ClusterSearch search;
            search.members = members;
            search.pricingMode = pricingMode;
            buildSearchOptions(actors, search, candidateCap);
            for (const std::int32_t slot : members)
            {
                if (stockCoupled(input, actors[static_cast<std::size_t>(slot)].pActor->engineUnitId))
                {
                    search.stockLive = true;
                    search.propertyCeilingInSearchBasis =
                        input.pStockValuer->stockCeiling();
                    break;
                }
            }
            const bool pairSearch =
                pricingMode ==
                SearchPricingMode::PairSwapInterval;
            const bool liveIntervalSupport =
                input.pStockValuer != nullptr &&
                input.pStockValuer->livePairSwapIntervals();
            const bool liveIntervals =
                search.stockLive &&
                pairSearch &&
                liveIntervalSupport;
            ExactSearchOutcome outcome;
            if (pairSearch)
            {
                outcome.pairPricing =
                    !search.stockLive
                        ? PairPricingResult::
                              NonStockExactScalar
                        : liveIntervalSupport
                              ? PairPricingResult::
                                    LiveInterval
                              : PairPricingResult::
                                    LiveIntervalUnavailable;
            }
            MilliFunds incumbentTotal = seatedTotal(actors, members);
            MilliFunds incumbentLower = incumbentTotal;
            MilliFunds incumbentUpper = incumbentTotal;
            const TurnPlan snapshot = plan;
            MilliFunds incumbentEconomic = 0;
            if (input.pTrace != nullptr &&
                input.pTrace->stockDetailsEnabled() &&
                members.size() == 2)
            {
                incumbentEconomic =
                    seatedPlanTotal(plan, actors);
            }
            const std::vector<ActorSeat> seats = recordSeats(actors, members);
            std::vector<std::int32_t> incumbentChoices;
            incumbentChoices.reserve(seats.size());
            for (const ActorSeat & seat : seats)
            {
                incumbentChoices.push_back(seat.chosen);
            }
            if (input.pTrace != nullptr &&
                input.pTrace->stockDetailsEnabled() &&
                members.size() == 2)
            {
                input.pTrace->record(
                    QStringLiteral("PAIR_BEGIN"),
                    QStringLiteral(
                        "actors=%1 incumbent=%2 incumbentSeated=%3 stockLive=%4 pricing=%5 candidateCap=%6 stateBudgetRemaining=%7")
                        .arg(traceActorIds(actors, members))
                        .arg(traceIndices(incumbentChoices))
                        .arg(incumbentTotal)
                        .arg(traceBool(search.stockLive))
                        .arg(tracePairPricing(
                            outcome.pairPricing))
                        .arg(candidateCap)
                        .arg(
                            ASSIGNMENT_STATE_BUDGET -
                            stats.swapStates -
                            stats.enumerationStates));
            }
            if (outcome.pairPricing ==
                PairPricingResult::
                    LiveIntervalUnavailable)
            {
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled() &&
                    members.size() == 2)
                {
                    input.pTrace->record(
                        QStringLiteral("PAIR_COMPARE"),
                        QStringLiteral(
                            "actors=%1 challenger=NONE challengerLower=NA challengerUpper=NA incumbentLower=NA incumbentUpper=NA states=0 reason=LIVE_INTERVAL_UNAVAILABLE accepted=false")
                            .arg(traceActorIds(
                                actors,
                                members)));
                }
                return outcome;
            }
            if (liveIntervals)
            {
                search.bestFeasibleEconomic =
                    seatedPlanTotal(plan, actors);
                search.bestFeasibleQuote =
                    input.pStockValuer->livePlanStock(
                        plan,
                        search.bestFeasibleEconomic,
                        false);
                if (!search.bestFeasibleQuote.valid ||
                    !search.bestFeasibleQuote
                         .lowerWitnessReplays)
                {
                    outcome.liveFailure = true;
                    outcome.pairPricing =
                        PairPricingResult::
                            LiveIntervalFailure;
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled() &&
                        members.size() == 2)
                    {
                        input.pTrace->record(
                            QStringLiteral("PAIR_COMPARE"),
                            QStringLiteral(
                                "actors=%1 challenger=NONE challengerLower=NA challengerUpper=NA incumbentLower=NA incumbentUpper=NA valid=%2 lowerWitnessReplays=%3 states=0 reason=LIVE_INTERVAL_FAILURE accepted=false")
                                .arg(traceActorIds(
                                    actors,
                                    members))
                                .arg(traceBool(
                                    search.bestFeasibleQuote
                                        .valid))
                                .arg(traceBool(
                                    search.bestFeasibleQuote
                                        .lowerWitnessReplays)));
                    }
                    return outcome;
                }
                search.bestFeasiblePlan = incumbentChoices;
                search.incumbentLower =
                    search.bestFeasibleEconomic +
                    search.bestFeasibleQuote
                        .stockAbsolute.lower;
                search.incumbentUpper =
                    search.bestFeasibleEconomic +
                    search.bestFeasibleQuote
                        .stockAbsolute.upper;
                incumbentLower = search.incumbentLower;
                incumbentUpper = search.incumbentUpper;
                search.hasBest = true;
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled() &&
                    members.size() == 2)
                {
                    input.pTrace->record(
                        QStringLiteral("PAIR_INCUMBENT"),
                        QStringLiteral(
                            "actors=%1 candidates=%2 economic=%3 stockLower=%4 stockUpper=%5 incumbentLower=%6 incumbentUpper=%7 exact=%8 lowerWitnessReplays=%9")
                            .arg(traceActorIds(
                                actors,
                                members))
                            .arg(traceIndices(
                                incumbentChoices))
                            .arg(search.bestFeasibleEconomic)
                            .arg(search.bestFeasibleQuote
                                     .stockAbsolute.lower)
                            .arg(search.bestFeasibleQuote
                                     .stockAbsolute.upper)
                            .arg(search.incumbentLower)
                            .arg(search.incumbentUpper)
                            .arg(traceBool(
                                search.bestFeasibleQuote
                                        .stockAbsolute.lower ==
                                    search.bestFeasibleQuote
                                        .stockAbsolute.upper))
                            .arg(traceBool(
                                search.bestFeasibleQuote
                                    .lowerWitnessReplays)));
                }
            }
            else if (search.stockLive)
            {
                search.propertyOriginInSearchBasis =
                    input.pStockValuer->originStock();
                incumbentTotal += planStockDelta(input, plan);
                incumbentLower = incumbentTotal;
                incumbentUpper = incumbentTotal;
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled() &&
                    members.size() == 2)
                {
                    input.pTrace->record(
                        QStringLiteral("PAIR_INCUMBENT"),
                        QStringLiteral(
                            "actors=%1 candidates=%2 completeValue=%3 exact=true pricing=LEGACY_SCALAR")
                            .arg(traceActorIds(
                                actors,
                                members))
                            .arg(traceIndices(
                                incumbentChoices))
                            .arg(incumbentTotal));
                }
            }
            unseatMembers(plan, actors, members);
            search.fixedEconomic =
                seatedPlanTotal(plan, actors);
            searchCluster(plan, input, actors, search, 0, 0, stats);
            outcome.states = search.states;
            outcome.aborted = search.aborted;
            outcome.liveFailure = search.liveFailure;
            outcome.incumbentLower = incumbentLower;
            outcome.incumbentUpper = incumbentUpper;
            if (liveIntervals && search.hasBest)
            {
                outcome.resultLower =
                    search.incumbentLower;
                outcome.resultUpper =
                    search.incumbentUpper;
                outcome.resultKnown = true;
            }
            else if (search.hasBest)
            {
                outcome.resultLower = search.bestTotal;
                outcome.resultUpper = search.bestTotal;
                outcome.resultKnown = true;
            }
            plan = snapshot;
            restoreSeats(actors, members, seats);
            if (liveIntervals)
            {
                if (search.liveFailure)
                {
                    outcome.pairPricing =
                        PairPricingResult::
                            LiveIntervalFailure;
                    outcome.resultLower = incumbentLower;
                    outcome.resultUpper = incumbentUpper;
                    outcome.resultKnown = true;
                    return outcome;
                }
                if (search.aborted)
                {
                    outcome.resultLower = incumbentLower;
                    outcome.resultUpper = incumbentUpper;
                    outcome.resultKnown = true;
                    return outcome;
                }
                if (!search.hasBest ||
                    search.bestFeasiblePlan ==
                        incumbentChoices)
                {
                    return outcome;
                }
                outcome.improved =
                    replayLiveSolution(
                        plan,
                        input,
                        actors,
                        search,
                        snapshot,
                        seats,
                        stats);
                if (outcome.improved)
                {
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled() &&
                        members.size() == 2)
                    {
                        outcome.pairAudit =
                            ExactSearchOutcome::
                                PairExactAuditSnapshot{
                                    .incumbentPlan =
                                        snapshot,
                                    .winnerPlan = plan,
                                    .incumbentEconomic =
                                        incumbentEconomic,
                                    .winnerEconomic =
                                        seatedPlanTotal(
                                            plan,
                                            actors),
                                    .incumbentChoices =
                                        incumbentChoices,
                                    .winnerChoices =
                                        search.bestFeasiblePlan,
                                    .liveIntervalPricing =
                                        true,
                                };
                    }
                    return outcome;
                }
                outcome.liveFailure = true;
                outcome.pairPricing =
                    PairPricingResult::
                        LiveIntervalFailure;
                outcome.resultLower = incumbentLower;
                outcome.resultUpper = incumbentUpper;
                outcome.resultKnown = true;
                return outcome;
            }
            if (search.aborted || !search.hasBest || search.bestTotal <= incumbentTotal)
            {
                return outcome;
            }
            outcome.improved = replaySolution(
                plan, input, actors, search, snapshot, seats, stats);
            if (outcome.improved &&
                input.pTrace != nullptr &&
                input.pTrace->stockDetailsEnabled() &&
                input.pStockValuer != nullptr &&
                members.size() == 2)
            {
                outcome.pairAudit =
                    ExactSearchOutcome::
                        PairExactAuditSnapshot{
                            .incumbentPlan = snapshot,
                            .winnerPlan = plan,
                            .incumbentEconomic =
                                incumbentEconomic,
                            .winnerEconomic =
                                seatedPlanTotal(
                                    plan,
                                    actors),
                            .incumbentChoices =
                                incumbentChoices,
                            .winnerChoices =
                                chosenOptions(
                                    actors,
                                    members),
                            .liveIntervalPricing =
                                false,
                        };
            }
            return outcome;
        }

        static QString traceAuditValue(
            bool available,
            MilliFunds value)
        {
            return available
                ? QString::number(value)
                : QStringLiteral("NA");
        }

        static QString traceAuditIndices(
            const std::vector<std::int32_t> & values)
        {
            const QString text = traceIndices(values);
            return text.isEmpty()
                ? QStringLiteral("NONE")
                : text;
        }

        static QString stockAuditReason(
            const PlanStockAudit & audit,
            bool requireOrigin)
        {
            if (!audit.available)
            {
                return QStringLiteral(
                    "DECOMPOSITION_MISMATCH");
            }
            if (!audit.scalarExact)
            {
                return QStringLiteral("SCALAR_UNPROVEN");
            }
            if (!audit.scalarWitnessReplays)
            {
                return QStringLiteral(
                    "SCALAR_WITNESS_REPLAY_FAILED");
            }
            if (requireOrigin && !audit.originExact)
            {
                return QStringLiteral("ORIGIN_UNPROVEN");
            }
            if (requireOrigin &&
                !audit.originWitnessReplays)
            {
                return QStringLiteral(
                    "ORIGIN_WITNESS_REPLAY_FAILED");
            }
            return QStringLiteral("PROVEN");
        }

        static const PlanStockActionAudit*
        matchingActionAudit(
            const PlanStockAudit & audit,
            const StockDecompositionRequest & request)
        {
            const bool captures =
                isCaptureKind(request.kind);
            for (const PlanStockActionAudit & action :
                 audit.actions)
            {
                if (action.knowledgeIndex ==
                        request.actorKnowledge &&
                    action.destination ==
                        request.destination &&
                    action.captures == captures)
                {
                    return &action;
                }
            }
            return nullptr;
        }

        static void recordStockDecomposition(
            const AssignmentInput & input,
            const StockDecompositionRequest & request,
            std::int32_t comparisonCandidate)
        {
            const PlanStockAudit audit =
                input.pStockValuer->auditPlanStock(
                    request.plan);
            const MilliFunds scalarStockDelta =
                audit.scalarStockAbsolute -
                audit.liveOriginStock;
            const MilliFunds legacyActorCompleteValue =
                request.seatedValue +
                scalarStockDelta;
            const MilliFunds legacyFullCompleteValue =
                request.fullEconomic +
                audit.scalarStockAbsolute;
            const MilliFunds sequentialAdjustment =
                audit.ours.bookedValue -
                audit.ourOpenOptimum -
                (audit.enemy.bookedValue -
                 audit.jointEnemyOptimum);
            const bool liveIntervalValid =
                request.liveValid &&
                request.lowerWitnessReplays &&
                request.liveStockLower <=
                    request.liveStockUpper &&
                request.liveCompleteLower <=
                    request.liveCompleteUpper;
            const bool scalarInLiveBounds =
                audit.available &&
                liveIntervalValid &&
                audit.scalarStockAbsolute >=
                    request.liveStockLower &&
                audit.scalarStockAbsolute <=
                    request.liveStockUpper &&
                legacyFullCompleteValue >=
                    request.liveCompleteLower &&
                legacyFullCompleteValue <=
                    request.liveCompleteUpper;
            const bool liveExact =
                liveIntervalValid &&
                request.liveCompleteLower ==
                    request.liveCompleteUpper;
            const bool exact =
                liveExact &&
                legacyFullCompleteValue ==
                    request.liveCompleteLower &&
                audit.scalarExact &&
                audit.scalarWitnessReplays &&
                audit.originExact &&
                audit.originWitnessReplays;
            QString reason =
                stockAuditReason(audit, true);
            if (reason == QStringLiteral("PROVEN") &&
                !liveIntervalValid)
            {
                reason = QStringLiteral(
                    "LIVE_INTERVAL_FAILURE");
            }
            else if (
                reason == QStringLiteral("PROVEN") &&
                !liveExact)
            {
                reason = QStringLiteral(
                    "LIVE_INTERVAL_UNRESOLVED");
            }
            else if (
                reason == QStringLiteral("PROVEN") &&
                !exact)
            {
                reason = QStringLiteral(
                    "LIVE_SCALAR_MISMATCH");
            }
            const PlanStockActionAudit* pAction =
                matchingActionAudit(audit, request);

            QString fields = QStringLiteral(
                "sweep=%1 actor=%2 actorKnowledge=%3 candidate=%4 comparisonCandidate=%5 kind=%6 destination=%7 economicSeatedValue=%8 capturedColumns=%9 ownedBaseline=%10 ownershipFlipSwingTotal=%11 ourOpenOptimum=%12 jointEnemyOptimum=%13 floorStockAbsolute=%14")
                .arg(request.sweep)
                .arg(request.actor)
                .arg(request.actorKnowledge)
                .arg(request.candidate)
                .arg(comparisonCandidate)
                .arg(traceBundleKind(request.kind))
                .arg(traceTile(request.destination))
                .arg(request.seatedValue)
                .arg(traceAuditIndices(
                    audit.capturedColumns))
                .arg(audit.ownedBaseline)
                .arg(audit.ownershipFlipSwingTotal)
                .arg(audit.ourOpenOptimum)
                .arg(audit.jointEnemyOptimum)
                .arg(audit.floorStockAbsolute);
            fields += QStringLiteral(
                " ourFloor=%1 ourRelaxed=%2 ourRepair=%3 ourSearch=%4 ourBooked=%5 ourSearchStates=%6 ourCertificate=%7 ourSearchCompleted=%8 ourProvenExact=%9 ourWitnessReplays=%10")
                .arg(audit.ours.floorValue)
                .arg(audit.ours.relaxedValue)
                .arg(audit.ours.repairValue)
                .arg(audit.ours.searchValue)
                .arg(audit.ours.bookedValue)
                .arg(audit.ours.searchStates)
                .arg(traceBool(audit.ours.certificate))
                .arg(traceBool(
                    audit.ours.searchCompleted))
                .arg(traceBool(audit.ours.provenExact))
                .arg(traceBool(
                    audit.ours.witnessReplays));
            fields += QStringLiteral(
                " enemyFloor=%1 enemyRelaxed=%2 enemyRepair=%3 enemySearch=%4 enemyBooked=%5 enemySearchStates=%6 enemyCertificate=%7 enemySearchCompleted=%8 enemyProvenExact=%9 enemyWitnessReplays=%10")
                .arg(audit.enemy.floorValue)
                .arg(audit.enemy.relaxedValue)
                .arg(audit.enemy.repairValue)
                .arg(audit.enemy.searchValue)
                .arg(audit.enemy.bookedValue)
                .arg(audit.enemy.searchStates)
                .arg(traceBool(
                    audit.enemy.certificate))
                .arg(traceBool(
                    audit.enemy.searchCompleted))
                .arg(traceBool(
                    audit.enemy.provenExact))
                .arg(traceBool(
                    audit.enemy.witnessReplays));
            fields += QStringLiteral(
                " sequentialAdjustment=%1 scalarStockAbsolute=%2 originStock=%3 originScalarStock=%4 scalarStockDelta=%5 legacyActorCompleteValue=%6 legacyFullCompleteValue=%7 liveStockLower=%8 liveStockUpper=%9 liveCompleteLower=%10 liveCompleteUpper=%11 liveValid=%12 lowerWitnessReplays=%13 liveExact=%14 scalarInLiveBounds=%15 auditAvailable=%16 exactStockAbsolute=%17 exactStockDelta=%18 exactCompleteValue=%19 exactAvailable=%20 reason=%21")
                .arg(sequentialAdjustment)
                .arg(audit.scalarStockAbsolute)
                .arg(audit.liveOriginStock)
                .arg(audit.originScalarStock)
                .arg(scalarStockDelta)
                .arg(legacyActorCompleteValue)
                .arg(legacyFullCompleteValue)
                .arg(request.liveStockLower)
                .arg(request.liveStockUpper)
                .arg(request.liveCompleteLower)
                .arg(request.liveCompleteUpper)
                .arg(traceBool(request.liveValid))
                .arg(traceBool(
                    request.lowerWitnessReplays))
                .arg(traceBool(liveExact))
                .arg(traceBool(scalarInLiveBounds))
                .arg(traceBool(audit.available))
                .arg(traceAuditValue(
                    exact,
                    audit.scalarStockAbsolute))
                .arg(traceAuditValue(
                    exact,
                    scalarStockDelta))
                .arg(traceAuditValue(
                    exact,
                    legacyFullCompleteValue))
                .arg(traceBool(exact))
                .arg(reason);
            if (pAction == nullptr)
            {
                fields += QStringLiteral(
                    " propertyIndex=NA stockColumn=NA currentOwnerSign=NA currentCapturePoints=NA currentCapturerKnowledge=NA carriedCapturePoints=NA captureRate=NA turnsUntilOwned=NA capturedColumn=NA");
            }
            else
            {
                fields += QStringLiteral(
                    " propertyIndex=%1 stockColumn=%2 currentOwnerSign=%3 currentCapturePoints=%4 currentCapturerKnowledge=%5 carriedCapturePoints=%6 captureRate=%7 turnsUntilOwned=%8 capturedColumn=%9")
                    .arg(pAction->propertyIndex)
                    .arg(pAction->stockColumn)
                    .arg(pAction->currentOwnerSign)
                    .arg(pAction->currentCapturePoints)
                    .arg(
                        pAction
                            ->currentCapturerKnowledge)
                    .arg(pAction->carriedCapturePoints)
                    .arg(pAction->captureRate)
                    .arg(pAction->turnsUntilOwned)
                    .arg(traceBool(
                        pAction->capturedColumn));
            }
            input.pTrace->record(
                QStringLiteral("STOCK_DECOMPOSITION"),
                fields);
        }

        static QString pairAuditReason(
            const PlanStockAudit & incumbent,
            const PlanStockAudit & winner)
        {
            const QString incumbentReason =
                stockAuditReason(incumbent, false);
            if (incumbentReason != QStringLiteral("PROVEN"))
            {
                return QStringLiteral("INCUMBENT_%1")
                    .arg(incumbentReason);
            }
            const QString winnerReason =
                stockAuditReason(winner, false);
            if (winnerReason != QStringLiteral("PROVEN"))
            {
                return QStringLiteral("WINNER_%1")
                    .arg(winnerReason);
            }
            return QStringLiteral("PROVEN");
        }

        static void recordPairExactAudit(
            const AssignmentInput & input,
            const PairExactAuditRequest & request)
        {
            const PlanStockAudit incumbent =
                input.pStockValuer->auditPlanStock(
                    request.snapshot.incumbentPlan);
            const PlanStockAudit winner =
                input.pStockValuer->auditPlanStock(
                    request.snapshot.winnerPlan);
            const bool exact =
                incumbent.available &&
                incumbent.scalarExact &&
                incumbent.scalarWitnessReplays &&
                winner.available &&
                winner.scalarExact &&
                winner.scalarWitnessReplays;
            const MilliFunds incumbentComplete =
                request.snapshot.incumbentEconomic +
                incumbent.scalarStockAbsolute;
            const MilliFunds winnerComplete =
                request.snapshot.winnerEconomic +
                winner.scalarStockAbsolute;
            const MilliFunds exactDelta =
                winnerComplete - incumbentComplete;
            const bool liveBoundsComparable =
                exact &&
                request.snapshot.liveIntervalPricing;
            const QString incumbentInLiveBounds =
                liveBoundsComparable
                    ? traceBool(
                          incumbentComplete >=
                              request.incumbentLower &&
                          incumbentComplete <=
                              request.incumbentUpper)
                    : QStringLiteral("NA");
            const QString winnerInLiveBounds =
                liveBoundsComparable
                    ? traceBool(
                          winnerComplete >=
                              request.winnerLower &&
                          winnerComplete <=
                              request.winnerUpper)
                    : QStringLiteral("NA");
            const QString exactImprovement =
                exact
                    ? traceBool(exactDelta > 0)
                    : QStringLiteral("NA");
            const QString pricing =
                request.snapshot.liveIntervalPricing
                    ? QStringLiteral("LIVE_INTERVAL")
                    : QStringLiteral(
                          "NON_STOCK_EXACT_SCALAR");
            input.pTrace->record(
                QStringLiteral("PAIR_EXACT_AUDIT"),
                QStringLiteral(
                    "sweep=%1 actors=%2 incumbent=%3 winner=%4 liveIncumbentLower=%5 liveIncumbentUpper=%6 liveWinnerLower=%7 liveWinnerUpper=%8 incumbentEconomic=%9 winnerEconomic=%10 incumbentScalarStockAbsolute=%11 winnerScalarStockAbsolute=%12 scalarIncumbentCompleteValue=%13 scalarWinnerCompleteValue=%14 scalarDelta=%15 exactIncumbentCompleteValue=%16 exactWinnerCompleteValue=%17 exactDelta=%18 exactImprovement=%19 auditValid=%20 incumbentInLiveBounds=%21 winnerInLiveBounds=%22 pricing=%23 reason=%24 basis=FULL_PLAN_ECONOMIC_PLUS_STOCK_ABSOLUTE")
                    .arg(request.sweep)
                    .arg(traceAuditIndices(
                        request.actors))
                    .arg(traceAuditIndices(
                        request.snapshot
                            .incumbentChoices))
                    .arg(traceAuditIndices(
                        request.snapshot
                            .winnerChoices))
                    .arg(request.incumbentLower)
                    .arg(request.incumbentUpper)
                    .arg(request.winnerLower)
                    .arg(request.winnerUpper)
                    .arg(
                        request.snapshot
                            .incumbentEconomic)
                    .arg(
                        request.snapshot
                            .winnerEconomic)
                    .arg(
                        incumbent
                            .scalarStockAbsolute)
                    .arg(winner.scalarStockAbsolute)
                    .arg(incumbentComplete)
                    .arg(winnerComplete)
                    .arg(exactDelta)
                    .arg(traceAuditValue(
                        exact,
                        incumbentComplete))
                    .arg(traceAuditValue(
                        exact,
                        winnerComplete))
                    .arg(traceAuditValue(
                        exact,
                        exactDelta))
                    .arg(exactImprovement)
                    .arg(traceBool(exact))
                    .arg(incumbentInLiveBounds)
                    .arg(winnerInLiveBounds)
                    .arg(pricing)
                    .arg(pairAuditReason(
                        incumbent,
                        winner)));
        }

        static void recordDeferredAudits(
            const AssignmentInput & input,
            const DeferredAudits & audits)
        {
            if (input.pTrace == nullptr ||
                !input.pTrace->stockDetailsEnabled() ||
                input.pStockValuer == nullptr)
            {
                return;
            }
            for (const StockDecompositionRequest & request :
                 audits.stockDecompositions)
            {
                const bool captures =
                    isCaptureKind(request.kind);
                const StockDecompositionRequest* pPeer =
                    nullptr;
                for (const StockDecompositionRequest & other :
                     audits.stockDecompositions)
                {
                    if (&other != &request &&
                        other.sweep == request.sweep &&
                        other.actor == request.actor &&
                        other.destination ==
                            request.destination &&
                        isCaptureKind(other.kind) !=
                            captures)
                    {
                        pPeer = &other;
                        break;
                    }
                }
                if (pPeer != nullptr)
                {
                    recordStockDecomposition(
                        input,
                        request,
                        pPeer->candidate);
                }
            }
            for (const PairExactAuditRequest & request :
                 audits.pairAudits)
            {
                recordPairExactAudit(input, request);
            }
        }

        static bool runRelativeStockCluster(
            TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
            const std::vector<std::int32_t> & members, const RelativeAssignmentBudget & budget,
            AssignmentStats & stats, RelativeStockTiming & timing)
        {
            if (budget.session == nullptr)
            {
                return false;
            }
            using Clock = std::chrono::steady_clock;
            const auto started = Clock::now();
            auto domain = relativeDomain(input, actors, members, RelativeStockPhase::Joint,
                ASSIGNMENT_STATE_BUDGET - stats.swapStates - stats.enumerationStates);
            if (domain.status == RelativeStockDomainStatus::Complete && budget.remaining(RelativeStockPhase::Joint) == 0)
            {
                domain.status = RelativeStockDomainStatus::RefinementBudget;
                ++stats.stockJoints.budgetExhaustions;
            }
            std::vector<std::int32_t> order;
            if (domain.status == RelativeStockDomainStatus::Complete && !completeRelativePlan(plan, actors, order))
            {
                domain.status = RelativeStockDomainStatus::IncompletePlan;
            }
            RelativeFrontier frontier;
            const auto incumbent = budget.session->quote(plan, seatedPlanTotal(plan, actors));
            if (domain.status == RelativeStockDomainStatus::Complete && !incumbent.valid)
            {
                domain.status = RelativeStockDomainStatus::InvalidQuote;
            }
            const bool supported = domain.status == RelativeStockDomainStatus::Complete;
            appendRelativeDomain(plan, input, actors, std::move(domain), RelativeStockPhase::Joint,
                                 *budget.session, incumbent, frontier, stats);
            timing.enumerationNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            if (!supported || !frontier.valid)
            {
                return false;
            }
            ++stats.clustersEnumerated;
            improveRelativeFrontier(plan, input, actors, frontier, budget, RelativeStockPhase::Joint,
                                    stats, timing, RelativeStockSlice::Direct);
            return true;
        }

        static void enumerateClusters(TurnPlan & plan, const AssignmentInput & input,
                                      std::vector<ActorProgress> & actors,
                                      const std::vector<std::int32_t> & sweepOrder,
                                      const std::vector<ConflictEdge> & edges,
                                      AssignmentStats & stats,
                                      const RelativeAssignmentBudget & relativeBudget,
                                      RelativeStockTiming & relativeTiming)
        {
            const std::vector<std::vector<std::int32_t>> adjacency =
                buildAdjacency(actors.size(), edges);
            std::vector<bool> visited(actors.size(), false);
            for (const std::int32_t root : sweepOrder)
            {
                if (visited[static_cast<std::size_t>(root)])
                {
                    continue;
                }
                const std::vector<std::int32_t> members =
                    collectComponent(root, actors, adjacency, visited);
                if (members.size() < 2)
                {
                    continue;
                }
                ++stats.clustersTotal;
                if (static_cast<std::int32_t>(members.size()) > CLUSTER_ACTOR_CAP ||
                    searchBudgetExhausted(stats))
                {
                    ++stats.clustersCapped;
                    if (input.pTrace != nullptr)
                    {
                        input.pTrace->record(
                            QStringLiteral("CLUSTER"),
                            QStringLiteral(
                                "actors=%1 candidates=%2 reason=ACTOR_OR_GLOBAL_BUDGET accepted=false")
                                .arg(traceActorIds(
                                    actors,
                                    members))
                                .arg(traceIndices(
                                    chosenOptions(
                                        actors,
                                        members))));
                    }
                    continue;
                }
                const bool stockCluster =
                    std::any_of(
                        members.begin(),
                        members.end(),
                        [&](std::int32_t slot)
                        {
                            return stockCoupled(
                                input,
                                actors[static_cast<std::size_t>(slot)]
                                    .pActor->engineUnitId);
                        });
                if (stockCluster)
                {
                    if (runRelativeStockCluster(plan, input, actors, members, relativeBudget, stats, relativeTiming))
                    {
                        continue;
                    }
                    ++stats.clustersCapped;
                    ++stats.clustersSkippedStockBudget;
                    if (input.pTrace != nullptr)
                    {
                        input.pTrace->record(
                            QStringLiteral("CLUSTER"),
                            QStringLiteral(
                                "actors=%1 candidates=%2 reason=STOCK_BUDGET_POLICY accepted=false")
                                .arg(traceActorIds(
                                    actors,
                                    members))
                                .arg(traceIndices(
                                    chosenOptions(
                                        actors,
                                        members))));
                    }
                    continue;
                }
                std::vector<std::int32_t>
                    incumbentChoices;
                if (input.pTrace != nullptr)
                {
                    incumbentChoices =
                        chosenOptions(actors, members);
                }
                const ExactSearchOutcome outcome =
                    runExactSearch(
                        plan,
                        input,
                        actors,
                        members,
                        CLUSTER_CANDIDATE_CAP,
                        stats,
                        SearchPricingMode::LegacyScalar);
                stats.enumerationStates += outcome.states;
                if (outcome.aborted)
                {
                    ++stats.clustersCapped;
                }
                else
                {
                    ++stats.clustersEnumerated;
                }
                if (outcome.improved &&
                    input.pTrace != nullptr)
                {
                    invalidateCompleteValues(actors);
                }
                if (input.pTrace != nullptr)
                {
                    const std::vector<std::int32_t>
                        finalChoices =
                            chosenOptions(
                                actors, members);
                    if (outcome.improved)
                    {
                        for (std::size_t depth = 0;
                             depth < members.size();
                             ++depth)
                        {
                            if (incumbentChoices[depth] !=
                                finalChoices[depth])
                            {
                                ActorProgress & state =
                                    actors[static_cast<std::size_t>(
                                        members[depth])];
                                state.phase =
                                    AssignmentResult::SelectionPhase::
                                        Cluster;
                            }
                        }
                    }
                    input.pTrace->record(
                        QStringLiteral("CLUSTER"),
                        QStringLiteral(
                            "actors=%1 incumbent=%2 final=%3 states=%4 accepted=%5 aborted=%6 incumbentLower=%7 incumbentUpper=%8 winnerLower=%9 winnerUpper=%10 winnerKnown=%11 winnerExact=%12")
                            .arg(traceActorIds(
                                actors,
                                members))
                            .arg(traceIndices(
                                incumbentChoices))
                            .arg(traceIndices(finalChoices))
                            .arg(outcome.states)
                            .arg(traceBool(outcome.improved))
                            .arg(traceBool(outcome.aborted))
                            .arg(outcome.incumbentLower)
                            .arg(outcome.incumbentUpper)
                            .arg(outcome.resultLower)
                            .arg(outcome.resultUpper)
                            .arg(traceBool(
                                outcome.resultKnown))
                            .arg(traceBool(
                                outcome.resultKnown &&
                                outcome.resultLower ==
                                    outcome.resultUpper)));
                }
            }
        }

        static void recordFinalAssignment(
            AssignmentResult & result,
            const std::vector<ActorProgress> & actors,
            const AssignmentInput & input)
        {
            if (input.pTrace == nullptr && !input.retainSelections)
            {
                return;
            }
            result.selections.reserve(actors.size());
            for (const ActorProgress & state : actors)
            {
                AssignmentResult::Selection selection;
                selection.knowledgeUnitIndex =
                    state.pActor->knowledgeUnitIndex;
                selection.engineUnitId =
                    state.pActor->engineUnitId;
                selection.actionIndex = state.actionIndex;
                selection.candidateIndex = state.chosen;
                selection.greedyCandidateIndex =
                    state.greedyChosen;
                selection.seatedValue = state.seatedValue;
                selection.stockCoupled =
                    stockCoupled(
                        input,
                        state.pActor->engineUnitId);
                selection.phase = state.phase;
                selection.completeValues =
                    state.completeValues;
                result.selections.push_back(
                    std::move(selection));
            }
            if (input.pTrace == nullptr)
            {
                return;
            }

            input.pTrace->record(
                QStringLiteral("FINAL_PLAN"),
                QStringLiteral(
                    "actors=%1 executionActions=%2 assigned=%3 unassigned=%4")
                    .arg(actors.size())
                    .arg(result.executionOrder.size())
                    .arg(result.stats.assignedUnits)
                    .arg(result.stats.unassignedUnits));

            std::vector<bool> recorded(
                result.selections.size(), false);
            auto recordSelection =
                [&](std::size_t selectionSlot,
                    std::int32_t executionPosition)
            {
                const AssignmentResult::Selection & selection =
                    result.selections[selectionSlot];
                const ActorProgress & state =
                    actors[selectionSlot];
                const CandidateBundle* pCandidate =
                    selection.candidateIndex == NO_CANDIDATE
                        ? nullptr
                        : &state.pActor->candidates[
                              static_cast<std::size_t>(
                                  selection.candidateIndex)];
                const PlannedAction* pAction =
                    selection.actionIndex == NO_ACTION
                        ? nullptr
                        : &result.plan.action(
                              selection.actionIndex);
                bool completeKnown = false;
                MilliFunds completeValue = 0;
                if (selection.candidateIndex !=
                        NO_CANDIDATE &&
                    selection.candidateIndex <
                        static_cast<std::int32_t>(
                            selection.completeValues.size()))
                {
                    const AssignmentResult::
                        CandidateCompleteValue & known =
                            selection.completeValues[
                                static_cast<std::size_t>(
                                    selection.candidateIndex)];
                    completeKnown = known.known;
                    completeValue = known.value;
                }
                input.pTrace->record(
                    QStringLiteral("FINAL_ACTOR"),
                    QStringLiteral(
                        "execution=%1 actor=%2 actorKnowledge=%3 candidate=%4 generated=%5 kind=%6 actionId=%7 origin=%8 destination=%9 target=%10 targetUnit=%11 seatedValue=%12 stockCoupled=%13 completeValue=%14 completeKnown=%15 completeValueScope=SETTLING_TRIAL_SNAPSHOT selectionPhase=%16 actionState=%17")
                        .arg(executionPosition)
                        .arg(selection.engineUnitId)
                        .arg(selection.knowledgeUnitIndex)
                        .arg(selection.candidateIndex)
                        .arg(
                            pCandidate == nullptr
                                ? -1
                                : pCandidate
                                      ->generationIndex)
                        .arg(
                            pCandidate == nullptr
                                ? QStringLiteral("NONE")
                                : traceBundleKind(
                                      planBundleKindOf(
                                          pCandidate->bundle)))
                        .arg(
                            pAction == nullptr ||
                                    pAction->actionId.isEmpty()
                                ? QStringLiteral("NONE")
                                : pAction->actionId)
                        .arg(
                            pCandidate == nullptr
                                ? traceTile(INVALID_TILE)
                                : traceTile(
                                      pCandidate->bundle.origin))
                        .arg(
                            pAction == nullptr
                                ? traceTile(INVALID_TILE)
                                : traceTile(
                                      pAction->destination))
                        .arg(
                            pAction == nullptr
                                ? traceTile(INVALID_TILE)
                                : traceTile(pAction->target))
                        .arg(
                            pAction == nullptr
                                ? NO_UNIT
                                : pAction->targetUnitId)
                        .arg(selection.seatedValue)
                        .arg(traceBool(
                            selection.stockCoupled))
                        .arg(completeValue)
                        .arg(traceBool(completeKnown))
                        .arg(traceSelectionPhase(
                            selection.phase))
                        .arg(
                            pAction == nullptr
                                ? QStringLiteral("NONE")
                                : tracePlanActionState(
                                      pAction->state)));
                recorded[selectionSlot] = true;
            };

            for (std::size_t executionPosition = 0;
                 executionPosition <
                 result.executionOrder.size();
                 ++executionPosition)
            {
                const std::int32_t actionIndex =
                    result.executionOrder[executionPosition];
                for (std::size_t selectionSlot = 0;
                     selectionSlot <
                     result.selections.size();
                     ++selectionSlot)
                {
                    if (result.selections[selectionSlot]
                            .actionIndex == actionIndex)
                    {
                        recordSelection(
                            selectionSlot,
                            static_cast<std::int32_t>(
                                executionPosition));
                        break;
                    }
                }
            }
            for (std::size_t selectionSlot = 0;
                 selectionSlot < result.selections.size();
                 ++selectionSlot)
            {
                if (!recorded[selectionSlot])
                {
                    recordSelection(selectionSlot, -1);
                }
            }

            const AssignmentStats & stats = result.stats;
            input.pTrace->record(
                QStringLiteral("FINAL_STATS"),
                QStringLiteral(
                    "candidates=%1 unsupportedCandidates=%2 conflicts=%3 overkillRejections=%4 staleRejections=%5 invalidActions=%6 vacateConflicts=%7 unorderedActions=%8 settlingSweeps=%9 settlingMoves=%10 swapStates=%11 swapImprovements=%12 clustersTotal=%13 clustersEnumerated=%14 clustersCapped=%15 clustersSkippedStockBudget=%16 enumerationStates=%17 replayFailures=%18")
                    .arg(stats.candidates)
                    .arg(stats.unsupportedCandidates)
                    .arg(stats.conflicts)
                    .arg(stats.overkillRejections)
                    .arg(stats.staleRejections)
                    .arg(stats.invalidActions)
                    .arg(stats.vacateConflicts)
                    .arg(stats.unorderedActions)
                    .arg(stats.settlingSweeps)
                    .arg(stats.settlingMoves)
                    .arg(stats.swapStates)
                    .arg(stats.swapImprovements)
                    .arg(stats.clustersTotal)
                    .arg(stats.clustersEnumerated)
                    .arg(stats.clustersCapped)
                    .arg(stats.clustersSkippedStockBudget)
                    .arg(stats.enumerationStates)
                    .arg(stats.replayFailures));
        }

        static void countAssignments(const TurnPlan & plan, const std::vector<ActorProgress> & actors,
                                     AssignmentStats & stats)
        {
            for (const ActorProgress & state : actors)
            {
                if (state.actionIndex != NO_ACTION && isLiveState(plan.action(state.actionIndex).state))
                {
                    ++stats.assignedUnits;
                }
                else
                {
                    ++stats.unassignedUnits;
                }
            }
        }

        static void finishPlan(AssignmentResult & result)
        {
            for (const VacateConflict & conflict : result.plan.rebuildVacateEdges(planOccupancy(result.plan)))
            {
                ++result.stats.vacateConflicts;
                result.plan.markFailed(conflict.moverAction);
            }
            const OrderingResult ordering = result.plan.executionOrder();
            result.executionOrder = ordering.order;
            result.stats.unorderedActions =
                static_cast<std::int32_t>(ordering.cycleMembers.size() + ordering.blockedByCycle.size());
        }
    };
}
