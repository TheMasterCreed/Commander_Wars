#pragma once

#include <algorithm>
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

    constexpr std::size_t NO_FAILED_ACTION = static_cast<std::size_t>(-1);

    // targets runs parallel to entries; when incomplete, own.status says why failedAction could not be measured.
    struct ResolvedPlanReply
    {
        bool complete{false};
        std::vector<PlanReplyEntry> entries;
        std::vector<ReplyTargetFacts> targets;
        OwnActionMeasurement own;
        std::map<std::int32_t, std::int32_t> startHp;
        std::size_t failedAction{NO_FAILED_ACTION};
    };

    namespace ReplyPredictionDetail
    {
        // shots runs parallel to tile->shooters, so every listed shooter needs its shot.
        inline bool resolvable(const PlanReplyAction & action)
        {
            if (action.pCandidate == nullptr || action.pCandidate->replyFacts == nullptr)
            {
                return false;
            }
            const ReplyFacts & facts = *action.pCandidate->replyFacts;
            return facts.tile != nullptr && facts.shots.size() >= facts.tile->shooters.size();
        }

        inline ResolvedPlanReply refused(StockQuoteStatus status, std::size_t failedAction)
        {
            ResolvedPlanReply refusal;
            refusal.own.status = status;
            refusal.failedAction = failedAction;
            return refusal;
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
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const PlanReplyAction & action = actions[index];
            if (!ReplyPredictionDetail::resolvable(action))
            {
                return ReplyPredictionDetail::refused(StockQuoteStatus::MissingActionFacts, index);
            }
            const CandidateBundle & candidate = *action.pCandidate;
            const PlanChainEntry entry{&candidate.bundle, candidate.actor.hpSteps};
            // A candidate always carries its bundle, so its reply start is known.
            const StockQuoteStatus facts = ownActionFacts(entry, candidate.replyFacts->actorRealHp, true);
            if (facts != StockQuoteStatus::Available)
            {
                return ReplyPredictionDetail::refused(facts, index);
            }
            ordered.push_back(entry);
            targets.push_back(ReplyTargetFacts{.target = OneActionTarget{
                action.engineUnitId, candidate.replyFacts->actorRealHp, candidate.actor.replacementCost}});
        }
        ResolvedPlanReply resolved{.own = measureOwnActions(ordered, targets)};
        const OwnActionMeasurement & own = resolved.own;
        if (own.status != StockQuoteStatus::Available)
        {
            const auto failed = std::find_if(own.ownStatus.begin(), own.ownStatus.end(),
                [](StockQuoteStatus status) { return status != StockQuoteStatus::Available; });
            return ReplyPredictionDetail::refused(own.status, static_cast<std::size_t>(failed - own.ownStatus.begin()));
        }
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
            resolved.targets, resolved.own.chain.chain,
            [&](std::int32_t shooter) { return resolved.startHp.at(shooter); },
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
