#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/replyfacts.h"

namespace Coordinator
{
    // One live plan action as the planner models it; plannedDamageSteps is the plan's granted damage.
    struct PlanReplyAction
    {
        const CandidateBundle* pCandidate{nullptr};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t plannedDamageSteps{0};
    };

    // shooterSlots index pFacts->tile->shooters, parallel to shots.
    struct PlanReplyEntry
    {
        const ReplyFacts* pFacts{nullptr};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t hpStepsBefore{0};
        TilePoint destination{INVALID_TILE};
        std::vector<std::size_t> shooterSlots;
        std::vector<TilePoint> firingTiles;
        std::vector<ResolvedReplyShot> shots;
    };

    // targets runs parallel to entries; startHp is each pooled shooter's day start hp.
    struct ResolvedPlanReply
    {
        bool complete{false};
        std::vector<PlanReplyEntry> entries;
        std::vector<ReplyTargetFacts> targets;
        TargetHpChain chain;
        std::map<std::int32_t, std::int32_t> startHp;
    };

    namespace ReplyPredictionDetail
    {
        // A plan action fires on at most one target, so its granted damage has one home.
        inline bool addPlannedDamage(const ActionBundle & bundle, std::int32_t plannedDamageSteps,
                                     TargetHpChain & chain)
        {
            const std::vector<std::int32_t> targets = fireTargetsOf(bundle);
            if (targets.size() > 1)
            {
                return false;
            }
            for (const BundleComponent & component : bundle.components)
            {
                if (component.kind != ComponentKind::Fire)
                {
                    continue;
                }
                const PlanWideHp start{component.fire.targetHpSteps, component.fire.targetHpSteps};
                PlanWideHp & target = chain.emplace(component.fire.targetUnitId, start).first->second;
                target.after = std::max(target.after - plannedDamageSteps, 0);
            }
            return true;
        }

        // The planner lists shooters only while the isolated bundle leaves the actor alive.
        inline bool isolatedActorSurvives(const CandidateBundle & candidate)
        {
            return hpAfterAction(candidate.bundle, candidate.actor.hpSteps).hpSteps > 0;
        }

        inline PlanReplyEntry resolveEntry(const PlanReplyAction & action, const PlannedOccupancy & occupancy,
                                           bool listsShooters, ReplyTargetFacts & target,
                                           std::map<std::int32_t, std::int32_t> & startHp)
        {
            const CandidateBundle & candidate = *action.pCandidate;
            const ReplyFacts & facts = *candidate.replyFacts;
            const ActionHpOutcome isolated = hpAfterAction(candidate.bundle, candidate.actor.hpSteps);
            PlanReplyEntry entry{
                .pFacts = &facts,
                .engineUnitId = action.engineUnitId,
                .actionIndex = action.actionIndex,
                .hpStepsBefore = isolated.hpSteps,
                .destination = candidate.bundle.destination,
            };
            target.target = OneActionTarget{action.engineUnitId, realHpAfterAction(facts.actorRealHp, isolated),
                                            candidate.actor.replacementCost};
            if (!listsShooters)
            {
                return entry;
            }
            const std::vector<ReplyShooterFacts> & shooters = facts.tile->shooters;
            for (std::size_t slot = 0; slot < shooters.size(); ++slot)
            {
                const ReplyShooterFacts & shooter = shooters[slot];
                const std::int32_t hpSteps =
                    replyShooterHpSteps(shooter, isolated.victimIndex, isolated.victimHpSteps);
                if (hpSteps <= 0)
                {
                    continue;
                }
                const TilePoint firingTile = replyFiringTile(shooter, occupancy);
                const ResolvedReplyShot shot = resolveReplyShot(shooter, facts.shots[slot], hpSteps, firingTile,
                                                                entry.destination);
                entry.shooterSlots.push_back(slot);
                entry.firingTiles.push_back(firingTile);
                entry.shots.push_back(shot);
                target.shooters.push_back(replySlotFacts(shooter, shot));
                startHp.emplace(shooter.shooterIndex, shooter.dayStartHpSteps);
            }
            return entry;
        }

        inline bool resolvable(const PlanReplyAction & action)
        {
            return action.pCandidate != nullptr && action.pCandidate->replyFacts != nullptr;
        }
    }

    // Every action moves its unit; call only once every action is resolvable.
    inline PlannedOccupancy planReplyOccupancy(std::span<const PlanReplyAction> actions, const ReplyBoard & board)
    {
        std::vector<PlannedMove> moves;
        moves.reserve(actions.size());
        for (const PlanReplyAction & action : actions)
        {
            const ActionBundle & bundle = action.pCandidate->bundle;
            moves.push_back(PlannedMove{bundle.unitId, bundle.origin, bundle.destination});
        }
        return PlannedOccupancy(board.width, board.height, board.standing, moves);
    }

    // Our hp is the isolated bundle's, the counter valueBundle books; enemy hp is the plan's granted damage.
    inline ResolvedPlanReply resolvePlanReply(std::span<const PlanReplyAction> actions, const ReplyBoard & board)
    {
        ResolvedPlanReply resolved;
        for (const PlanReplyAction & action : actions)
        {
            if (!ReplyPredictionDetail::resolvable(action) ||
                !ReplyPredictionDetail::addPlannedDamage(action.pCandidate->bundle, action.plannedDamageSteps,
                                                         resolved.chain))
            {
                return ResolvedPlanReply{};
            }
        }
        const PlannedOccupancy occupancy = planReplyOccupancy(actions, board);
        resolved.entries.reserve(actions.size());
        resolved.targets.resize(actions.size());
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const bool listsShooters = ReplyPredictionDetail::isolatedActorSurvives(*actions[index].pCandidate);
            resolved.entries.push_back(ReplyPredictionDetail::resolveEntry(
                actions[index], occupancy, listsShooters, resolved.targets[index], resolved.startHp));
        }
        resolved.complete = true;
        return resolved;
    }

    inline PlanReplyRun runResolvedPlanReply(const ResolvedPlanReply & resolved)
    {
        return planReply(
            resolved.targets, resolved.chain, [&](std::int32_t shooter) { return resolved.startHp.at(shooter); },
            [&](std::size_t target, std::size_t slot, std::int32_t shooterHpSteps, std::int32_t hpSteps)
            {
                const PlanReplyEntry & entry = resolved.entries[target];
                return pricedTerrainDamage(entry.pFacts->terrain, entry.shots[slot], shooterHpSteps, hpSteps);
            });
    }

    inline PlanReplyPrediction planReplyPrediction(const ResolvedPlanReply & resolved, const PlanReplyRun & run)
    {
        PlanReplyPrediction prediction;
        prediction.complete = resolved.complete;
        prediction.pairsExamined = run.reply.pairsExamined;
        prediction.actors.reserve(resolved.entries.size());
        for (std::size_t index = 0; index < resolved.entries.size(); ++index)
        {
            const PlanReplyEntry & entry = resolved.entries[index];
            const double hpBefore = resolved.targets[index].target.hp;
            const std::int32_t replyStart = ceilHpSteps(hpBefore);
            const ReplyOutcome & outcome = run.reply.outcomes[index];
            prediction.actors.push_back(PlanReplyActorOutcome{
                .engineUnitId = entry.engineUnitId,
                .actionIndex = entry.actionIndex,
                .hpBefore = hpBefore,
                .hpStepsBefore = entry.hpStepsBefore,
                .stepsLost = replyStart - outcome.hpSteps,
                .destroyed = replyStart > 0 && outcome.destroyed,
            });
        }
        return prediction;
    }

    inline PlanReplyPrediction predictPlanReply(std::span<const PlanReplyAction> actions, const ReplyBoard & board)
    {
        const ResolvedPlanReply resolved = resolvePlanReply(actions, board);
        if (!resolved.complete)
        {
            return PlanReplyPrediction{};
        }
        return planReplyPrediction(resolved, runResolvedPlanReply(resolved));
    }
}
