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

    struct OwnShotOutcome
    {
        int target{NO_UNIT};
        double attackerBefore{}, attackerAfter{}, targetBefore{}, targetAfter{};
        OwnFireReport report;
        MilliFunds targetCapital{}, counterCapital{}, targetCost{}, actorCost{};
        bool operator==(const OwnShotOutcome &) const = default;
    };

    // Every vector runs parallel to the execution-ordered actions it measured.
    struct OwnActionMeasurement
    {
        StockQuoteStatus status{StockQuoteStatus::UnusablePlanChain};
        std::vector<StockQuoteStatus> ownStatus;
        PlanChainRun chain;
        std::vector<double> hpAfterActions;
        std::vector<MeasuredCounter> counters;
        std::vector<OwnShotOutcome> ownShots;
        std::map<int,double> targetRealAfter;
        std::map<int,std::array<int, 2>> targetAmmoAfter;
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

    // Combat candidates retain the uncapped native report for their first hit. A
    // subsequent hit is quoted at the target's remaining real HP and ammunition.
    inline OwnActionMeasurement measureCombatOwnActions(std::span<const PlanChainEntry> ordered,
                                                         std::span<ReplyTargetFacts> targets, const OwnFireAt &fireAt)
    {
        OwnActionMeasurement result;
        result.status = StockQuoteStatus::Available;
        result.ownStatus.assign(ordered.size(), StockQuoteStatus::Available);
        result.ownShots.resize(ordered.size());
        result.counters.resize(ordered.size());
        for (std::size_t i = 0; i < ordered.size(); ++i)
        {
            const auto &entry = ordered[i];
            const auto &bundle = *entry.pBundle;
            auto &actorHp = targets[i].target.hp;
            const bool alive = targetsAliveIn(fireTargetsOf(bundle), result.chain.chain);
            result.chain.targetsAlive.push_back(alive);
            if (!alive)
            {
                result.ownStatus[i] = result.status = StockQuoteStatus::TargetAlreadyGone;
                return result;
            }
            const auto *part = bundle.components.size() == 1 ? &bundle.components.front() : nullptr;
            if (!part || part->kind != ComponentKind::Fire)
            {
                const auto outcome = hpAfterActionOn(bundle, entry.actorHpSteps, result.chain.chain);
                actorHp = realHpAfterAction(actorHp, outcome);
                result.chain.outcomes.push_back(outcome);
                result.hpAfterActions.push_back(actorHp);
                continue;
            }
            const FireFacts &fire = part->fire;
            if (!std::isfinite(fire.targetRealHp) || fire.targetRealHp <= 0 || fire.targetRealHp > UNIT_HP_STEPS ||
                ceilHpSteps(fire.targetRealHp) != fire.targetHpSteps)
            {
                result.ownStatus[i] = result.status = StockQuoteStatus::MissingActionFacts;
                return result;
            }
            auto &hp = result.targetRealAfter.try_emplace(fire.targetUnitId, fire.targetRealHp).first->second;
            auto &ammo = result.targetAmmoAfter.try_emplace(fire.targetUnitId, fire.targetAmmo).first->second;
            const bool terrainTarget = isTerrainTargetId(fire.targetUnitId);
            const bool changed = !terrainTarget && (hp != fire.targetRealHp || ammo != fire.targetAmmo);
            OwnFireReport quote = changed ? fireAt(bundle.unitId, bundle.destination, fire.targetUnitId, actorHp, hp, ammo)
                                          : fire.isolated;
            if (terrainTarget && entry.actorHpSteps > 0 && ceilHpSteps(actorHp) != entry.actorHpSteps && quote.damage > 0)
                quote.damage = quote.damage * (static_cast<double>(ceilHpSteps(actorHp)) / static_cast<double>(entry.actorHpSteps));
            if (!std::isfinite(quote.damage) || quote.damage < 0 || !std::isfinite(quote.counter))
            {
                result.ownStatus[i] = result.status = StockQuoteStatus::UnusablePlanChain;
                return result;
            }
            auto &shot = result.ownShots[i];
            shot = {fire.targetUnitId, actorHp, engineHpAfterReply(actorHp, engineShotDamage(quote.counter)), hp,
                    engineHpAfterReply(hp, engineShotDamage(quote.damage)), quote};
            if (quote.counter >= 0 && quote.counterWeapon >= 0 && quote.counterWeapon < 2 && ammo[quote.counterWeapon] > 0)
                --ammo[quote.counterWeapon];
            hp = shot.targetAfter;
            actorHp = shot.attackerAfter;
            const int actorSteps = ceilHpSteps(actorHp), targetSteps = ceilHpSteps(hp);
            result.chain.chain[fire.targetUnitId] = {fire.targetHpSteps, targetSteps};
            result.chain.outcomes.push_back({.hpSteps = actorSteps, .counterApplied = quote.counter > 0,
                                             .victimIndex = fire.targetUnitId, .victimHpSteps = targetSteps});
            result.counters[i] = {.isolatedSteps = fire.counterSteps, .steps = entry.actorHpSteps - actorSteps,
                                  .counterScaled = changed, .hpAfter = actorHp};
            result.hpAfterActions.push_back(actorHp);
            if (changed)
            {
                ++result.scaledCounterUnits;
                result.scaledCounterSurvivalDifferences += (actorSteps <= 0) != (fire.counterSteps >= entry.actorHpSteps);
            }
            else
                ++result.certainCounterUnits;
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
