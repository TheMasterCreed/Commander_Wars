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
    };

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

        static AssignmentResult assign(const AssignmentInput & input)
        {
            AssignmentResult result;
            std::vector<ActorProgress> actors = prepareActors(input, result.stats);
            greedyInit(result, actors, input);
            const std::vector<std::int32_t> sweepOrder = actorSweepOrder(actors);
            settle(result.plan, input, actors, sweepOrder, result.stats);
            improveBySwaps(result.plan, input, actors, conflictEdges(actors, sweepOrder), result.stats);
            finishPlan(result);
            countAssignments(result.plan, actors, result.stats);
            return result;
        }

    private:
        struct ActorSeat
        {
            std::int32_t chosen{NO_CANDIDATE};
            std::int32_t actionIndex{NO_ACTION};
            MilliFunds seatedValue{0};
        };

        struct PairSearch
        {
            std::vector<std::int32_t> members;
            std::vector<std::int32_t> leftOptions;
            std::vector<std::int32_t> rightOptions;
            std::int32_t bestLeft{NO_CANDIDATE};
            std::int32_t bestRight{NO_CANDIDATE};
            MilliFunds bestTotal{0};
            bool hasBest{false};
            std::int32_t states{0};
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

        static std::vector<ConflictEdge> conflictEdges(const std::vector<ActorProgress> & actors,
                                                       const std::vector<std::int32_t> & sweepOrder)
        {
            std::vector<ActorResources> resources;
            resources.reserve(actors.size());
            for (const ActorProgress & state : actors)
            {
                resources.push_back(actorResourcesOf(state));
            }
            std::vector<ConflictEdge> edges;
            for (std::size_t left = 0; left < sweepOrder.size(); ++left)
            {
                for (std::size_t right = left + 1; right < sweepOrder.size(); ++right)
                {
                    const std::int32_t leftSlot = sweepOrder[left];
                    const std::int32_t rightSlot = sweepOrder[right];
                    if (actorsContest(resources[static_cast<std::size_t>(leftSlot)],
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

        static bool searchPair(TurnPlan & plan, const AssignmentInput & input, std::vector<ActorProgress> & actors,
                               const ConflictEdge & edge, AssignmentStats & stats)
        {
            PairSearch search;
            search.members = {edge.left, edge.right};
            ActorProgress & left = actors[static_cast<std::size_t>(edge.left)];
            ActorProgress & right = actors[static_cast<std::size_t>(edge.right)];
            search.leftOptions = incumbentFirstOptions(left);
            search.rightOptions = incumbentFirstOptions(right);
            const MilliFunds incumbentTotal = left.seatedValue + right.seatedValue;
            const TurnPlan snapshot = plan;
            const std::vector<ActorSeat> seats = recordSeats(actors, search.members);
            unseatMembers(plan, actors, search.members);
            for (const std::int32_t leftOption : search.leftOptions)
            {
                const SeatOutcome leftOutcome = seatOption(plan, input, left, leftOption);
                ++search.states;
                if (leftOutcome.claim != ReservationResult::Granted)
                {
                    continue;
                }
                for (const std::int32_t rightOption : search.rightOptions)
                {
                    const SeatOutcome rightOutcome = seatOption(plan, input, right, rightOption);
                    ++search.states;
                    if (rightOutcome.claim == ReservationResult::Granted)
                    {
                        const MilliFunds total = leftOutcome.value + rightOutcome.value;
                        if (!search.hasBest || total > search.bestTotal)
                        {
                            search.bestLeft = leftOption;
                            search.bestRight = rightOption;
                            search.bestTotal = total;
                            search.hasBest = true;
                        }
                    }
                    unseatActor(plan, right);
                }
                unseatActor(plan, left);
            }
            stats.swapStates += search.states;
            plan = snapshot;
            restoreSeats(actors, search.members, seats);
            if (!search.hasBest || search.bestTotal <= incumbentTotal)
            {
                return false;
            }
            unseatMembers(plan, actors, search.members);
            const MilliFunds replayed =
                seatOption(plan, input, left, search.bestLeft).value +
                seatOption(plan, input, right, search.bestRight).value;
            if (replayed == search.bestTotal)
            {
                return true;
            }
            ++stats.replayFailures;
            plan = snapshot;
            restoreSeats(actors, search.members, seats);
            return false;
        }

        static void improveBySwaps(TurnPlan & plan, const AssignmentInput & input,
                                   std::vector<ActorProgress> & actors,
                                   const std::vector<ConflictEdge> & edges, AssignmentStats & stats)
        {
            for (std::int32_t sweep = 0; sweep < SWAP_SWEEP_CAP; ++sweep)
            {
                bool improved = false;
                for (const ConflictEdge & edge : edges)
                {
                    if (searchPair(plan, input, actors, edge, stats))
                    {
                        ++stats.swapImprovements;
                        improved = true;
                    }
                }
                if (!improved)
                {
                    return;
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
