#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <QString>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/coordinatorcommon.h"
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
        TurnPlan plan;
        std::vector<std::int32_t> executionOrder;
        AssignmentStats stats;
    };

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
                             AssignmentStats & stats)
    {
        const std::vector<std::int32_t> order = plannableCandidateOrder(actionIds, links, candidates, stats);
        const std::int32_t engineUnitId = plan.action(actionIndex).unitId;
        for (const std::int32_t slot : order)
        {
            const CandidateBundle & candidate = candidates[static_cast<std::size_t>(slot)];
            const PlannedAction action = plannedActionFrom(actionIds, links, engineUnitId, candidate);
            const ReservationResult claim = installCandidate(plan, actionIndex, action, candidate);
            if (claim == ReservationResult::Granted)
            {
                return true;
            }
            countRejection(stats, claim);
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
        MilliFunds seatedValue{0};
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
            AssignmentResult result;
            std::vector<ActorProgress> actors = prepareActors(input, result.stats);
            greedyInit(result, actors, input);
            const std::vector<std::int32_t> sweepOrder = actorSweepOrder(actors);
            settle(result.plan, input, actors, sweepOrder, result.stats);
            const std::vector<ConflictEdge> edges = conflictEdges(input, actors, sweepOrder);
            improveBySwaps(result.plan, input, actors, edges, result.stats);
            enumerateClusters(result.plan, input, actors, sweepOrder, edges, result.stats);
            finishPlan(result);
            countAssignments(result.plan, actors, result.stats);
            return result;
        }

    private:
        enum class SearchPricingMode : std::int8_t
        {
            LegacyScalar,
            PairSwapInterval,
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
                entry.done = entry.order.empty();
                actors.push_back(std::move(entry));
            }
            return actors;
        }

        static void greedyInit(AssignmentResult & result, std::vector<ActorProgress> & actors,
                               const AssignmentInput & input)
        {
            while (true)
            {
                const std::size_t slot = pickBestActor(actors);
                if (slot >= actors.size())
                {
                    return;
                }
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
                entry.seatedValue = seatedOptionValue(result.plan, entry, entry.chosen);
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

        static bool settleStockActor(TurnPlan & plan, const AssignmentInput & input, ActorProgress & state)
        {
            const std::int32_t previousChosen = state.chosen;
            const MilliFunds previousValue = state.seatedValue + planStockDelta(input, plan);
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
                    continue;
                }
                const MilliFunds value = outcome.value + planStockDelta(input, plan);
                unseatActor(plan, state);
                if (value > bestValue)
                {
                    bestOption = option;
                    bestValue = value;
                }
            }
            if (seatOption(plan, input, state, bestOption).claim != ReservationResult::Granted)
            {
                seatBestClaimable(plan, input, state, options);
            }
            return state.chosen != previousChosen;
        }

        static void settle(TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
                           const std::vector<std::int32_t> & sweepOrder, AssignmentStats & stats)
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
                        if (settleStockActor(plan, input, state))
                        {
                            moved = true;
                            ++stats.settlingMoves;
                        }
                        continue;
                    }
                    const MilliFunds previousValue = state.seatedValue;
                    const std::vector<std::int32_t> options = incumbentFirstOptions(state);
                    unseatActor(plan, state);
                    seatBestClaimable(plan, input, state, options);
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

        static void improveBySwaps(TurnPlan & plan, const AssignmentInput & input,
                                   std::vector<ActorProgress> & actors,
                                   const std::vector<ConflictEdge> & edges, AssignmentStats & stats)
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
                    const ExactSearchOutcome outcome =
                        runExactSearch(
                            plan,
                            input,
                            actors,
                            pair,
                            NO_CANDIDATE_CAP,
                            stats,
                            liveIntervals
                                ? SearchPricingMode::PairSwapInterval
                                : SearchPricingMode::LegacyScalar);
                    stats.swapStates += outcome.states;
                    if (outcome.improved)
                    {
                        ++stats.swapImprovements;
                        improved = true;
                    }
                }
                const bool refined =
                    liveIntervals &&
                    input.pStockValuer->refineLiveAtBoundary(
                        AssignPhase::BetweenSwapSweeps);
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
                    if (upper <= search.incumbentLower)
                    {
                        return;
                    }
                    if (lower > search.incumbentLower)
                    {
                        search.bestFeasibleEconomic = economic;
                        search.bestFeasibleQuote = quote;
                        search.incumbentLower = lower;
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
                if (!search.hasBest || value > search.bestTotal)
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
                    return;
                }
                const SeatOutcome outcome = seatOption(plan, input, state, option);
                if (outcome.claim != ReservationResult::Granted)
                {
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
                    search.incumbentLower)
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
            const bool liveIntervals =
                search.stockLive &&
                pricingMode ==
                    SearchPricingMode::PairSwapInterval;
            MilliFunds incumbentTotal = seatedTotal(actors, members);
            const TurnPlan snapshot = plan;
            const std::vector<ActorSeat> seats = recordSeats(actors, members);
            std::vector<std::int32_t> incumbentChoices;
            incumbentChoices.reserve(seats.size());
            for (const ActorSeat & seat : seats)
            {
                incumbentChoices.push_back(seat.chosen);
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
                    ExactSearchOutcome fallback =
                        runExactSearch(
                            plan,
                            input,
                            actors,
                            members,
                            candidateCap,
                            stats,
                            SearchPricingMode::LegacyScalar);
                    fallback.liveFailure = true;
                    return fallback;
                }
                search.bestFeasiblePlan = incumbentChoices;
                search.incumbentLower =
                    search.bestFeasibleEconomic +
                    search.bestFeasibleQuote
                        .stockAbsolute.lower;
                search.hasBest = true;
            }
            else if (search.stockLive)
            {
                search.propertyOriginInSearchBasis =
                    input.pStockValuer->originStock();
                incumbentTotal += planStockDelta(input, plan);
            }
            unseatMembers(plan, actors, members);
            search.fixedEconomic =
                seatedPlanTotal(plan, actors);
            searchCluster(plan, input, actors, search, 0, 0, stats);
            ExactSearchOutcome outcome;
            outcome.states = search.states;
            outcome.aborted = search.aborted;
            outcome.liveFailure = search.liveFailure;
            plan = snapshot;
            restoreSeats(actors, members, seats);
            if (liveIntervals)
            {
                if (search.liveFailure)
                {
                    ExactSearchOutcome fallback =
                        runExactSearch(
                            plan,
                            input,
                            actors,
                            members,
                            candidateCap,
                            stats,
                            SearchPricingMode::LegacyScalar);
                    fallback.states += outcome.states;
                    fallback.liveFailure = true;
                    return fallback;
                }
                if (search.aborted ||
                    !search.hasBest ||
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
                    return outcome;
                }
                ExactSearchOutcome fallback =
                    runExactSearch(
                        plan,
                        input,
                        actors,
                        members,
                        candidateCap,
                        stats,
                        SearchPricingMode::LegacyScalar);
                fallback.states += outcome.states;
                fallback.liveFailure = true;
                return fallback;
            }
            if (search.aborted || !search.hasBest || search.bestTotal <= incumbentTotal)
            {
                return outcome;
            }
            outcome.improved = replaySolution(
                plan, input, actors, search, snapshot, seats, stats);
            return outcome;
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
                    continue;
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
            }
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
