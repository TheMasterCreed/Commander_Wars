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
}
