#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ai/coordinator/capturedangerdecisions.h"
#include "ai/coordinator/capturedangerpricing.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/replyfacts.h"
#include "ai/fastbattleestimate.h"

namespace Coordinator
{
    enum class StockQuoteStatus : std::int8_t
    {
        Available,
        MissingField,
        UnusablePlanChain,
        ReplayFailed,
        CyclicOrdering,
        MissingActionFacts,
        UnsupportedAction,
        TargetAlreadyGone,
    };

    struct MeasuredCounter
    {
        StockQuoteStatus status{StockQuoteStatus::Available};
        std::int32_t isolatedSteps{0};
        std::int32_t steps{0};
        bool counterScaled{false};
        double hpAfter{0};
    };

    inline MeasuredCounter measuredCounter(const FireFacts & fire, std::int32_t targetHp,
                                           std::int32_t actorHp, double actorRealHp)
    {
        MeasuredCounter result{.isolatedSteps = fire.counterSteps, .hpAfter = actorRealHp};
        if (targetHp <= 0)
        {
            return result;
        }
        const auto isolatedHp = fire.targetHpSteps - std::min(fire.damageSteps, fire.targetHpSteps);
        if (isolatedHp <= 0 || targetHp > isolatedHp)
        {
            result.status = StockQuoteStatus::UnusablePlanChain;
            return result;
        }
        result.steps = std::min(fire.counterSteps, actorHp);
        if (targetHp < isolatedHp)
        {
            // FireFacts retains steps only; use the oracle's real-hp loss conversion after scaling.
            const double damage = static_cast<double>(fire.counterSteps) * targetHp * FastBattleEstimate::HP_SCALE / isolatedHp;
            result.steps = std::min(damageStepsFrom(actorRealHp, damage), actorHp);
            result.counterScaled = true;
        }
        result.hpAfter = realHpAfterAction(actorRealHp, ActionHpOutcome{
            .hpSteps = actorHp - result.steps, .realHpFromSteps = result.steps > 0,
        });
        return result;
    }

    constexpr std::int32_t UNKNOWN_ACTION_HP = -1;

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

    // Every vector runs parallel to the execution-ordered actions it measured.
    struct OwnActionMeasurement
    {
        StockQuoteStatus status{StockQuoteStatus::UnusablePlanChain};
        std::vector<StockQuoteStatus> ownStatus;
        PlanChainRun chain;
        std::vector<double> hpAfterActions;
        std::vector<MeasuredCounter> counters;
        std::int32_t certainCounterUnits{0};
        std::int32_t scaledCounterUnits{0};
        std::int32_t scaledCounterSurvivalDifferences{0};
    };

    // startKnown is replyStartKnown for the action; actorRealHp is its actor's real hp at day start.
    inline StockQuoteStatus ownActionFacts(const PlanChainEntry & entry, double actorRealHp, bool startKnown)
    {
        if (entry.actorHpSteps == UNKNOWN_ACTION_HP || entry.actorHpSteps != ceilHpSteps(actorRealHp) || !startKnown)
        {
            return StockQuoteStatus::MissingActionFacts;
        }
        if (entry.pBundle != nullptr && (entry.pBundle->components.size() > 1 || servesOtherUnit(*entry.pBundle)))
        {
            return StockQuoteStatus::UnsupportedAction;
        }
        return StockQuoteStatus::Available;
    }

    // ordered and targets run in execution order; each target's hp enters as day start and leaves after own actions.
    inline OwnActionMeasurement measureOwnActions(std::span<const PlanChainEntry> ordered,
                                                  std::span<ReplyTargetFacts> targets)
    {
        OwnActionMeasurement result;
        result.ownStatus.assign(ordered.size(), StockQuoteStatus::Available);
        std::vector<bool> hasCounterResult(ordered.size(), false);
        result.counters.resize(ordered.size());
        result.chain = planChain(ordered, [&](std::size_t index, const FireFacts & fire,
                                              std::int32_t targetHp, std::int32_t actorHp)
        {
            const auto counter = measuredCounter(fire, targetHp, actorHp, targets[index].target.hp);
            result.counters[index] = counter;
            result.ownStatus[index] = counter.status;
            hasCounterResult[index] = true;
            if (counter.status != StockQuoteStatus::Available)
            {
                return ActionCounterDamage{};
            }
            targets[index].target.hp = counter.hpAfter;
            return ActionCounterDamage{counter.steps, ActionCounterSource::Repriced};
        });
        result.status = StockQuoteStatus::Available;
        result.hpAfterActions.resize(ordered.size());
        for (std::size_t index = 0; index < ordered.size(); ++index)
        {
            if (!result.chain.targetsAlive[index])
            {
                result.ownStatus[index] = StockQuoteStatus::TargetAlreadyGone;
            }
            if (result.ownStatus[index] != StockQuoteStatus::Available)
            {
                if (result.status == StockQuoteStatus::Available)
                {
                    result.status = result.ownStatus[index];
                }
                continue;
            }
            if (!hasCounterResult[index])
            {
                targets[index].target.hp = realHpAfterAction(targets[index].target.hp, result.chain.outcomes[index]);
            }
            else if (result.counters[index].counterScaled)
            {
                ++result.scaledCounterUnits;
                const auto isolatedHp = ordered[index].actorHpSteps -
                    std::min(result.counters[index].isolatedSteps, ordered[index].actorHpSteps);
                result.scaledCounterSurvivalDifferences +=
                    (result.chain.outcomes[index].hpSteps <= 0) != (isolatedHp <= 0) ? 1 : 0;
            }
            else
            {
                ++result.certainCounterUnits;
            }
            result.hpAfterActions[index] = targets[index].target.hp;
        }
        return result;
    }

    // unitAt(index) is the occupancy unit index of the actor of ordered action index.
    template <typename UnitAt>
    PlannedOccupancy withoutOwnActionDeaths(const PlannedOccupancy & live, const PlanChainRun & chain, UnitAt && unitAt)
    {
        std::vector<std::int32_t> dead;
        for (std::size_t index = 0; index < chain.outcomes.size(); ++index)
        {
            if (chain.outcomes[index].hpSteps <= 0)
            {
                dead.push_back(unitAt(index));
            }
        }
        return live.withoutUnits(dead);
    }

    // Every tile shooter is listed against a survivor at its day start hp; the reply prices it at the chain's hp.
    template <typename UnitAt, typename ReplyAt>
    void prepareSurvivorReplies(const PlanChainRun & chain, const PlannedOccupancy & live, UnitAt && unitAt,
                                ReplyAt && replyAt, std::span<ReplyTargetFacts> targets)
    {
        const auto occupancy = withoutOwnActionDeaths(live, chain, unitAt);
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            if (targets[index].target.hp <= 0)
            {
                continue;
            }
            PlanReplyEntry & reply = replyAt(index);
            const auto & facts = *reply.pFacts;
            for (std::size_t shooter = 0; shooter < facts.tile->shooters.size(); ++shooter)
            {
                const auto & source = facts.tile->shooters[shooter];
                const auto firingTile = replyFiringTile(source, occupancy);
                reply.shooterSlots.push_back(shooter);
                reply.firingTiles.push_back(firingTile);
                reply.shots.push_back(resolveReplyShot(source, facts.shots[shooter], source.dayStartHpSteps,
                                                       firingTile, reply.destination));
                targets[index].shooters.push_back(replySlotFacts(source, reply.shots.back()));
            }
        }
    }
}
