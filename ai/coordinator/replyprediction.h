#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/ownactionmeasurement.h"
#include "ai/coordinator/replyfacts.h"

namespace Coordinator
{
    struct PlanReplyAction
    {
        const CandidateBundle* pCandidate{nullptr};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
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

    // actions arrive live and in execution order; anything the own-action measurement cannot take leaves it incomplete.
    inline ResolvedPlanReply resolvePlanReply(std::span<const PlanReplyAction> actions, const ReplyBoard & board)
    {
        std::vector<PlanChainEntry> ordered;
        std::vector<ReplyTargetFacts> targets;
        ordered.reserve(actions.size());
        targets.reserve(actions.size());
        for (const PlanReplyAction & action : actions)
        {
            if (!ReplyPredictionDetail::resolvable(action))
            {
                return ResolvedPlanReply{};
            }
            const CandidateBundle & candidate = *action.pCandidate;
            const PlanChainEntry entry{&candidate.bundle, candidate.actor.hpSteps};
            // A candidate always carries its bundle, so its reply start is known.
            if (ownActionFacts(entry, candidate.replyFacts->actorRealHp, true) != StockQuoteStatus::Available)
            {
                return ResolvedPlanReply{};
            }
            ordered.push_back(entry);
            targets.push_back(ReplyTargetFacts{.target = OneActionTarget{
                action.engineUnitId, candidate.replyFacts->actorRealHp, candidate.actor.replacementCost}});
        }
        const OwnActionMeasurement own = measureOwnActions(ordered, targets);
        if (own.status != StockQuoteStatus::Available)
        {
            return ResolvedPlanReply{};
        }
        ResolvedPlanReply resolved{.chain = own.chain.chain};
        resolved.entries.reserve(actions.size());
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const CandidateBundle & candidate = *actions[index].pCandidate;
            resolved.entries.push_back(PlanReplyEntry{
                .pFacts = candidate.replyFacts.get(),
                .engineUnitId = actions[index].engineUnitId,
                .actionIndex = actions[index].actionIndex,
                .hpStepsBefore = own.chain.outcomes[index].hpSteps,
                .destination = candidate.bundle.destination,
            });
        }
        prepareSurvivorReplies(own.chain, planReplyOccupancy(actions, board),
            [&](std::size_t index) { return actions[index].pCandidate->bundle.unitId; },
            [&](std::size_t index) -> PlanReplyEntry & { return resolved.entries[index]; }, targets);
        for (const PlanReplyEntry & entry : resolved.entries)
        {
            for (const std::size_t slot : entry.shooterSlots)
            {
                const ReplyShooterFacts & shooter = entry.pFacts->tile->shooters[slot];
                resolved.startHp.emplace(shooter.shooterIndex, shooter.dayStartHpSteps);
            }
        }
        resolved.targets = std::move(targets);
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
