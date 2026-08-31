#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QString>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/turnplan.h"

namespace Coordinator
{
    struct PlanActionIds
    {
        QString wait;
        QString fire;
        QString capture;
    };

    PlanActionIds engineActionIds();

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

    struct KnownUnitLink
    {
        std::int32_t engineUnitId{NO_UNIT};
        TilePoint tile{INVALID_TILE};
    };

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

    inline ReservationResult claimFor(TurnPlan & plan, std::int32_t actionIndex, const CandidateBundle & candidate)
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
                return plan
                    .claimAttack(actionIndex, action.targetUnitId, pComponent->fire.targetHpSteps,
                                 pComponent->fire.damageSteps)
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
            return candidates[static_cast<std::size_t>(left)].valuation.value().economicValue >
                   candidates[static_cast<std::size_t>(right)].valuation.value().economicValue;
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

    constexpr std::int32_t NO_CANDIDATE = -1;
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

    inline MilliFunds proRatedCredit(MilliFunds credit, std::int32_t granted, std::int32_t dealt)
    {
        return divideRoundHalfAwayFromZero(credit * granted, dealt);
    }

    inline MilliFunds grantedFireValue(const FireFacts & fire, MilliFunds staticValue, std::int32_t granted)
    {
        const std::int32_t dealt = std::min(fire.damageSteps, fire.targetHpSteps);
        if (granted == dealt)
        {
            return staticValue;
        }
        MilliFunds staticSurvivingShot = fire.targetBestShotAfter;
        if (dealt >= fire.targetHpSteps)
        {
            staticSurvivingShot = 0;
        }
        const MilliFunds staticCredit = fire.targetBestShotBefore - staticSurvivingShot;
        const MilliFunds staticCapital = bookDamage(fire.targetReplacementCost, dealt);
        const MilliFunds grantedCapital = bookDamage(fire.targetReplacementCost, granted);
        if (staticCredit < 0)
        {
            return staticValue - staticCapital + grantedCapital;
        }
        return staticValue - staticCapital + grantedCapital - staticCredit +
               proRatedCredit(staticCredit, granted, dealt);
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
        static constexpr std::int32_t SETTLING_SWEEP_CAP = 8;
        static constexpr std::int32_t SWAP_SWEEP_CAP = 4;
        static constexpr std::int32_t CLUSTER_ACTOR_CAP = 6;
        static constexpr std::int32_t CLUSTER_CANDIDATE_CAP = 12;
        static constexpr std::int32_t CLUSTER_STATE_CAP = 20000;

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
            improveBySwaps(
                result.plan,
                input,
                actors,
                edges,
                result.stats,
                audits);
            if (timed)
            {
                result.phaseTiming.pairRefinementNanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - phaseStart)
                        .count();
                phaseStart = Clock::now();
            }
            enumerateClusters(result.plan, input, actors, sweepOrder, edges, result.stats);
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
            MilliFunds bookedStockDelta{0};
            MilliFunds bookedCompleteValue{0};
            TurnPlan plan;
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
            MilliFunds seatedValue,
            MilliFunds bookedStockDelta,
            MilliFunds bookedCompleteValue,
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
                    .seatedValue = seatedValue,
                    .bookedStockDelta =
                        bookedStockDelta,
                    .bookedCompleteValue =
                        bookedCompleteValue,
                    .plan = plan,
                });
        }

        static bool settleStockActor(
            TurnPlan & plan,
            const AssignmentInput & input,
            ActorProgress & state,
            std::int32_t sweep,
            DeferredAudits & audits)
        {
            const std::int32_t previousChosen = state.chosen;
            const MilliFunds previousSeated = state.seatedValue;
            const MilliFunds previousStockDelta =
                planStockDelta(input, plan);
            const MilliFunds previousValue =
                previousSeated + previousStockDelta;
            deferStockDecomposition(
                plan,
                input,
                state,
                previousChosen,
                sweep,
                previousSeated,
                previousStockDelta,
                previousValue,
                audits);
            if (previousChosen != NO_CANDIDATE &&
                previousChosen <
                    static_cast<std::int32_t>(
                        state.completeValues.size()))
            {
                state.completeValues[static_cast<std::size_t>(
                    previousChosen)] =
                    AssignmentResult::CandidateCompleteValue{
                        previousValue, true};
            }
            const std::vector<std::int32_t> options = incumbentFirstOptions(state);
            unseatActor(plan, state);
            std::int32_t bestOption = previousChosen;
            MilliFunds bestValue = previousValue;
            for (const std::int32_t option : options)
            {
                if (option == previousChosen)
                {
                    continue;
                }
                const SeatOutcome outcome = seatOption(plan, input, state, option);
                if (outcome.claim != ReservationResult::Granted)
                {
                    if (input.pTrace != nullptr &&
                        input.pTrace->stockDetailsEnabled())
                    {
                        const CandidateBundle* pCandidate =
                            option == NO_CANDIDATE
                                ? nullptr
                                : &state.pActor->candidates[
                                      static_cast<std::size_t>(
                                          option)];
                        input.pTrace->record(
                            QStringLiteral("SETTLING_CHALLENGER"),
                            QStringLiteral(
                                "sweep=%1 actor=%2 previousCandidate=%3 previousSeated=%4 previousStockDelta=%5 previousComplete=%6 challenger=%7 kind=%8 destination=%9 challengerSeated=NA challengerStockDelta=NA challengerComplete=NA claim=%10 accepted=false")
                                .arg(sweep)
                                .arg(state.pActor->engineUnitId)
                                .arg(previousChosen)
                                .arg(previousSeated)
                                .arg(previousStockDelta)
                                .arg(previousValue)
                                .arg(option)
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
                                              pCandidate->bundle
                                                  .destination))
                                .arg(traceReservationResult(
                                    outcome.claim)));
                    }
                    continue;
                }
                const MilliFunds challengerStockDelta =
                    planStockDelta(input, plan);
                const MilliFunds value =
                    outcome.value + challengerStockDelta;
                if (option != NO_CANDIDATE &&
                    option <
                        static_cast<std::int32_t>(
                            state.completeValues.size()))
                {
                    state.completeValues[static_cast<std::size_t>(
                        option)] =
                        AssignmentResult::CandidateCompleteValue{
                            value, true};
                }
                deferStockDecomposition(
                    plan,
                    input,
                    state,
                    option,
                    sweep,
                    outcome.value,
                    challengerStockDelta,
                    value,
                    audits);
                unseatActor(plan, state);
                const bool accepted = value > bestValue;
                if (input.pTrace != nullptr &&
                    input.pTrace->stockDetailsEnabled())
                {
                    const CandidateBundle* pCandidate =
                        option == NO_CANDIDATE
                            ? nullptr
                            : &state.pActor->candidates[
                                  static_cast<std::size_t>(option)];
                    input.pTrace->record(
                        QStringLiteral("SETTLING_CHALLENGER"),
                        QStringLiteral(
                            "sweep=%1 actor=%2 previousCandidate=%3 previousSeated=%4 previousStockDelta=%5 previousComplete=%6 challenger=%7 kind=%8 destination=%9 challengerSeated=%10 challengerStockDelta=%11 challengerComplete=%12 claim=GRANTED accepted=%13")
                            .arg(sweep)
                            .arg(state.pActor->engineUnitId)
                            .arg(previousChosen)
                            .arg(previousSeated)
                            .arg(previousStockDelta)
                            .arg(previousValue)
                            .arg(option)
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
                            .arg(outcome.value)
                            .arg(challengerStockDelta)
                            .arg(value)
                            .arg(traceBool(accepted)));
                }
                if (value > bestValue)
                {
                    bestOption = option;
                    bestValue = value;
                }
            }
            const SeatOutcome winningOutcome =
                seatOption(plan, input, state, bestOption);
            if (winningOutcome.claim != ReservationResult::Granted)
            {
                seatBestClaimable(plan, input, state, options);
            }
            const bool changed = state.chosen != previousChosen;
            if (changed)
            {
                state.phase =
                    AssignmentResult::SelectionPhase::Settling;
            }
            if (input.pTrace != nullptr)
            {
                const CandidateBundle* pWinner =
                    state.chosen == NO_CANDIDATE
                        ? nullptr
                        : &state.pActor->candidates[
                              static_cast<std::size_t>(
                                  state.chosen)];
                bool completeKnown = false;
                MilliFunds completeValue = 0;
                if (state.chosen != NO_CANDIDATE &&
                    state.chosen <
                        static_cast<std::int32_t>(
                            state.completeValues.size()))
                {
                    const AssignmentResult::CandidateCompleteValue &
                        known =
                            state.completeValues[
                                static_cast<std::size_t>(
                                    state.chosen)];
                    completeKnown = known.known;
                    completeValue = known.value;
                }
                input.pTrace->record(
                    QStringLiteral("SETTLING_WINNER"),
                    QStringLiteral(
                        "sweep=%1 actor=%2 candidate=%3 kind=%4 destination=%5 completeValue=%6 completeKnown=%7 changed=%8")
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
                        .arg(completeValue)
                        .arg(traceBool(completeKnown))
                        .arg(traceBool(changed)));
            }
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
                                state,
                                sweep,
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

        static void improveBySwaps(
            TurnPlan & plan,
            const AssignmentInput & input,
            std::vector<ActorProgress> & actors,
            const std::vector<ConflictEdge> & edges,
            AssignmentStats & stats,
            DeferredAudits & audits)
        {
            const bool liveIntervals =
                input.pStockValuer != nullptr &&
                input.pStockValuer->livePairSwapIntervals();
            for (std::int32_t sweep = 0; sweep < SWAP_SWEEP_CAP; ++sweep)
            {
                bool improved = false;
                for (const ConflictEdge & edge : edges)
                {
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
                const bool refined =
                    liveIntervals &&
                    input.pStockValuer->refineLiveAtBoundary(
                        AssignPhase::BetweenSwapSweeps);
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
            const MilliFunds exactCompleteValue =
                request.seatedValue +
                scalarStockDelta;
            const MilliFunds sequentialAdjustment =
                audit.ours.bookedValue -
                audit.ourOpenOptimum -
                (audit.enemy.bookedValue -
                 audit.jointEnemyOptimum);
            const bool observedMatchesAudit =
                audit.available &&
                scalarStockDelta ==
                    request.bookedStockDelta &&
                exactCompleteValue ==
                    request.bookedCompleteValue;
            const bool exact =
                observedMatchesAudit &&
                audit.scalarExact &&
                audit.scalarWitnessReplays &&
                audit.originExact &&
                audit.originWitnessReplays;
            QString reason =
                stockAuditReason(audit, true);
            if (reason == QStringLiteral("PROVEN") &&
                !observedMatchesAudit)
            {
                reason = QStringLiteral(
                    "OBSERVED_SCALAR_MISMATCH");
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
                " sequentialAdjustment=%1 scalarStockAbsolute=%2 originStock=%3 originScalarStock=%4 scalarStockDelta=%5 observedStockDelta=%6 observedCompleteValue=%7 observedMatchesAudit=%8 auditAvailable=%9 exactStockAbsolute=%10 exactStockDelta=%11 exactCompleteValue=%12 exactAvailable=%13 reason=%14")
                .arg(sequentialAdjustment)
                .arg(audit.scalarStockAbsolute)
                .arg(audit.liveOriginStock)
                .arg(audit.originScalarStock)
                .arg(scalarStockDelta)
                .arg(request.bookedStockDelta)
                .arg(request.bookedCompleteValue)
                .arg(traceBool(observedMatchesAudit))
                .arg(traceBool(audit.available))
                .arg(traceAuditValue(
                    exact,
                    audit.scalarStockAbsolute))
                .arg(traceAuditValue(
                    exact,
                    scalarStockDelta))
                .arg(traceAuditValue(
                    exact,
                    exactCompleteValue))
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

        static void enumerateClusters(TurnPlan & plan, const AssignmentInput & input,
                                      std::vector<ActorProgress> & actors,
                                      const std::vector<std::int32_t> & sweepOrder,
                                      const std::vector<ConflictEdge> & edges,
                                      AssignmentStats & stats)
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
            if (input.pTrace == nullptr)
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
            for (const VacateConflict & conflict : result.plan.addVacateEdges(planOccupancy(result.plan)))
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
