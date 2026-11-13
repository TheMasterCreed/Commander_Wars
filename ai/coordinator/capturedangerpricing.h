#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/capturedangerdecisions.h"
#include "ai/coordinator/capturedangerstream.h"

namespace Coordinator
{
    enum class ChangeReason : std::int8_t
    {
        Shot,
        HitsShooter,
    };

    enum class ChangeOmission : std::int8_t
    {
        None,
        DestinationTaken,
        TargetGone,
        BreaksLaterAction,
    };

    struct ReplySlotFacts
    {
        std::int32_t shooterIndex{NO_UNIT};
        bool onOwnBuilding{false};
        PricedResolution resolution{PricedResolution::Known};
        double counterRawBase{NO_COUNTER};
        MilliFunds shooterCost{0};
        MilliFunds captureCost{0};
    };

    struct ReplyTargetFacts
    {
        OneActionTarget target;
        std::vector<ReplySlotFacts> shooters;
    };

    struct PlanReplyRun
    {
        std::vector<std::int32_t> pool;
        std::vector<std::int32_t> poolHp;
        OneActionReply reply;
    };

    // startHp(index) is a shooter's hp before the plan; damageAt(target, slot, shooterHpSteps, hpSteps) is its shot.
    template <typename StartHp, typename DamageAt>
    PlanReplyRun planReply(std::span<const ReplyTargetFacts> entries, const TargetHpChain & chain, StartHp && startHp,
                           DamageAt && damageAt)
    {
        PlanReplyRun run;
        std::set<std::int32_t> pooled;
        std::vector<std::map<std::int32_t, std::size_t>> shooterSlots(entries.size());
        std::vector<OneActionTarget> targets;
        targets.reserve(entries.size());
        for (std::size_t entry = 0; entry < entries.size(); ++entry)
        {
            targets.push_back(entries[entry].target);
            for (std::size_t slot = 0; slot < entries[entry].shooters.size(); ++slot)
            {
                pooled.insert(entries[entry].shooters[slot].shooterIndex);
                shooterSlots[entry].emplace(entries[entry].shooters[slot].shooterIndex, slot);
            }
        }
        run.pool.assign(pooled.begin(), pooled.end());
        run.poolHp.reserve(run.pool.size());
        for (const std::int32_t shooter : run.pool)
        {
            const auto damaged = chain.find(shooter);
            run.poolHp.push_back(damaged == chain.end() ? startHp(shooter) : damaged->second.after);
        }
        const auto slotOf = [&](std::size_t shooter, std::size_t target)
        {
            return shooterSlots[target].find(run.pool[shooter])->second;
        };
        // The whole plan's damage, then the shooter's own building repair, sets the hp P1 prices it at.
        const auto shooterHpAt = [&](std::size_t shooter, std::size_t target)
        {
            const ReplySlotFacts & slot = entries[target].shooters[slotOf(shooter, target)];
            return oneActionShooterHpSteps(run.poolHp[shooter], slot.onOwnBuilding);
        };
        run.reply = oneActionReply(
            targets, run.pool,
            [&](std::size_t shooter, std::size_t target)
            {
                return shooterSlots[target].contains(run.pool[shooter]) && shooterHpAt(shooter, target) > 0 &&
                       pricedShotPossible(entries[target].shooters[slotOf(shooter, target)].resolution);
            },
            [&](std::size_t shooter, std::size_t target, std::int32_t hpSteps)
            {
                return damageAt(target, slotOf(shooter, target), shooterHpAt(shooter, target), hpSteps);
            },
            [&](std::size_t shooter, std::size_t target, std::int32_t hpStepsAfter)
            {
                const ReplySlotFacts & slot = entries[target].shooters[slotOf(shooter, target)];
                return counterCost(slot.counterRawBase, hpStepsAfter, shooterHpAt(shooter, target), slot.shooterCost);
            },
            [&](std::size_t shooter, std::size_t target)
            {
                return entries[target].shooters[slotOf(shooter, target)].captureCost;
            });
        return run;
    }

    struct ReplyLoss
    {
        MilliFunds capital{0};
        MilliFunds nextShot{0};
        MilliFunds capture{0};

        constexpr MilliFunds total() const
        {
            return capital + nextShot + capture;
        }

        constexpr ReplyLoss & operator+=(const ReplyLoss & other)
        {
            capital += other.capital;
            nextShot += other.nextShot;
            capture += other.capture;
            return *this;
        }

        friend constexpr ReplyLoss operator-(const ReplyLoss & left, const ReplyLoss & right)
        {
            return ReplyLoss{
                .capital = left.capital - right.capital,
                .nextShot = left.nextShot - right.nextShot,
                .capture = left.capture - right.capture,
            };
        }

        friend constexpr bool operator==(const ReplyLoss &, const ReplyLoss &) = default;
    };

    struct CaptureStake
    {
        PropertyStockColumn column;
        CaptureWalkInput input;
    };

    // bestNextShot is the entry's best next shot from its destination; capture is set only for a priced capture.
    struct ReplyLossFacts
    {
        MilliFunds actorCost{0};
        MilliFunds bestNextShot{0};
        std::optional<CaptureStake> capture;
    };

    constexpr MilliFunds replyCapitalLoss(MilliFunds actorCost, std::int32_t hpBefore, std::int32_t hpAfter)
    {
        return hpBefore > hpAfter ? bookDamage(actorCost, hpBefore - hpAfter) : 0;
    }

    // Linear in the hp kept, an approximation of what a weakened unit's next shot is worth.
    constexpr MilliFunds replyNextShotLoss(MilliFunds bestNextShot, std::int32_t hpBefore, std::int32_t hpAfter)
    {
        if (hpBefore <= 0)
        {
            return 0;
        }
        return bestNextShot - bestNextShot * std::max(hpAfter, 0) / hpBefore;
    }

    // A later delivery streams less, so this is negative only on a column whose per-turn swing is negative.
    inline MilliFunds replyCaptureLoss(const CaptureStake & stake, double hpAfterReply)
    {
        const std::int32_t horizon = stake.input.horizonTurns;
        return captureWalkStream(stake.column, unrepliedDeliveredTurn(stake.input), horizon) -
               captureWalkStream(stake.column, deliveredTurnAfterReply(stake.input, hpAfterReply), horizon);
    }

    inline ReplyLoss replyLoss(const ReplyLossFacts & facts, std::int32_t hpBefore, const ReplyOutcome & outcome)
    {
        if (hpBefore <= 0)
        {
            return ReplyLoss{};
        }
        return ReplyLoss{
            .capital = replyCapitalLoss(facts.actorCost, hpBefore, outcome.hpSteps),
            .nextShot = replyNextShotLoss(facts.bestNextShot, hpBefore, outcome.hpSteps),
            .capture = facts.capture ? replyCaptureLoss(*facts.capture, outcome.hp) : 0,
        };
    }

    inline ReplyLoss totalLoss(std::span<const ReplyLoss> losses)
    {
        ReplyLoss total;
        for (const ReplyLoss & loss : losses)
        {
            total += loss;
        }
        return total;
    }

    inline std::vector<std::int32_t> fireTargetsOf(const ActionBundle & bundle)
    {
        std::vector<std::int32_t> targets;
        for (const BundleComponent & component : bundle.components)
        {
            if (component.kind == ComponentKind::Fire)
            {
                targets.push_back(component.fire.targetUnitId);
            }
        }
        return targets;
    }

    inline bool firesOnAny(const ActionBundle & bundle, const std::set<std::int32_t> & targets)
    {
        const std::vector<std::int32_t> fired = fireTargetsOf(bundle);
        return std::any_of(fired.begin(), fired.end(), [&](std::int32_t target) { return targets.contains(target); });
    }

    // A self service changes only the actor, whose own hp the plan chain already tracks.
    inline std::set<std::int32_t> servicedPartnersOf(const ActionBundle & bundle)
    {
        std::set<std::int32_t> partners;
        for (const BundleComponent & component : bundle.components)
        {
            if (component.kind == ComponentKind::Service && component.service.partnerUnitId != bundle.unitId)
            {
                partners.insert(component.service.partnerUnitId);
            }
        }
        return partners;
    }

    inline bool servesOtherUnit(const ActionBundle & bundle)
    {
        return !servicedPartnersOf(bundle).empty();
    }

    // slotPartners lists, per plan slot, the other units its installed action services.
    inline bool servicedByOtherSlot(std::span<const std::set<std::int32_t>> slotPartners, std::size_t slot,
                                    std::int32_t unitIndex)
    {
        for (std::size_t other = 0; other < slotPartners.size(); ++other)
        {
            if (other != slot && slotPartners[other].contains(unitIndex))
            {
                return true;
            }
        }
        return false;
    }

    inline bool serviceInteraction(const ActionBundle & installed, const ActionBundle & replacement,
                                   std::span<const std::set<std::int32_t>> slotPartners, std::size_t slot,
                                   std::int32_t unitIndex)
    {
        return servesOtherUnit(installed) || servesOtherUnit(replacement) ||
               servicedByOtherSlot(slotPartners, slot, unitIndex);
    }

    // A HITS_SHOOTER unit is changed only to candidates that fire on a shooter the installed reply uses.
    inline bool consideredFor(ChangeReason reason, const ActionBundle & bundle,
                              const std::set<std::int32_t> & firingShooters)
    {
        return reason == ChangeReason::Shot || firesOnAny(bundle, firingShooters);
    }

    // A slot without a bundle touches no hp, but its listed fire targets still need to be alive when it acts.
    struct PlanChainEntry
    {
        const ActionBundle* pBundle{nullptr};
        std::int32_t actorHpSteps{0};
        std::vector<std::int32_t> firedTargets;
    };

    struct PlanChainRun
    {
        TargetHpChain chain;
        std::vector<ActionHpOutcome> outcomes;
        std::vector<bool> targetsAlive;
    };

    inline bool targetsAliveIn(std::span<const std::int32_t> targets, const TargetHpChain & chain)
    {
        return std::none_of(targets.begin(), targets.end(), [&](std::int32_t target)
        {
            const auto found = chain.find(target);
            return found != chain.end() && found->second.after <= 0;
        });
    }

    inline PlanChainRun planChain(std::span<const PlanChainEntry> entries)
    {
        PlanChainRun run;
        run.outcomes.reserve(entries.size());
        run.targetsAlive.reserve(entries.size());
        for (const PlanChainEntry & entry : entries)
        {
            if (entry.pBundle == nullptr)
            {
                run.outcomes.push_back(ActionHpOutcome{.hpSteps = entry.actorHpSteps});
                run.targetsAlive.push_back(targetsAliveIn(entry.firedTargets, run.chain));
                continue;
            }
            run.targetsAlive.push_back(targetsAliveIn(fireTargetsOf(*entry.pBundle), run.chain));
            run.outcomes.push_back(hpAfterActionOn(*entry.pBundle, entry.actorHpSteps, run.chain));
        }
        return run;
    }

    // A later entry breaks only when its target was alive for it in the installed plan.
    inline ChangeOmission chainOmission(const PlanChainRun & installed, const PlanChainRun & changed, std::size_t slot)
    {
        if (!changed.targetsAlive[slot])
        {
            return ChangeOmission::TargetGone;
        }
        for (std::size_t later = slot + 1; later < changed.targetsAlive.size(); ++later)
        {
            if (installed.targetsAlive[later] && !changed.targetsAlive[later])
            {
                return ChangeOmission::BreaksLaterAction;
            }
        }
        return ChangeOmission::None;
    }

    // slotDestinations holds a destination only for a live slot.
    inline bool destinationTaken(std::span<const std::optional<TilePoint>> slotDestinations, std::size_t slot,
                                 const TilePoint & destination)
    {
        for (std::size_t other = 0; other < slotDestinations.size(); ++other)
        {
            if (other != slot && slotDestinations[other] == destination)
            {
                return true;
            }
        }
        return false;
    }

    // BundleAssignment's isCaptureKind is private to MaximumValueAssignment.
    constexpr bool capturesInPlan(PlanBundleKind kind)
    {
        return kind == PlanBundleKind::Capture || kind == PlanBundleKind::MoveAndCapture;
    }

    // JointPlanStockValuer::planActions keys a live action with a stock row by its destination and captures flag.
    constexpr bool stockKeyUnchanged(bool entersKey, const TilePoint & installedDestination, bool installedCaptures,
                                     const TilePoint & changedDestination, bool changedCaptures)
    {
        return !entersKey || (installedDestination == changedDestination && installedCaptures == changedCaptures);
    }

    struct ChangeFlags
    {
        bool stockKeyUnchanged{false};
        bool chainInteraction{false};
        bool firingTilesShared{false};
        bool serviceInteraction{false};
    };

    constexpr bool exactChange(const ChangeFlags & flags)
    {
        return flags.stockKeyUnchanged && !flags.chainInteraction && !flags.firingTilesShared &&
               !flags.serviceInteraction;
    }

    // The plan reply prices the danger to the actor and the shots our fire removes, so neither term stays.
    constexpr MilliFunds seatedValue(const BundleValuation & valuation)
    {
        return valuation.value().economicValue - valuation.continuation.exposure -
               valuation.continuation.targetContinuationRemoved;
    }

    constexpr MilliFunds changeGain(MilliFunds seatedDelta, const ReplyLoss & installed, const ReplyLoss & changed)
    {
        return seatedDelta - (changed.total() - installed.total());
    }

    // The first largest gain stays, so a tie keeps the earlier offer.
    template <typename Change>
    struct ChangeBucket
    {
        std::int32_t count{0};
        std::int32_t positive{0};
        std::optional<MilliFunds> bestGain;
        Change best{};

        void add(MilliFunds gain, const Change & change)
        {
            ++count;
            positive += gain > 0 ? 1 : 0;
            offer(gain, change);
        }

        void offer(MilliFunds gain, const Change & change)
        {
            if (!bestGain || gain > *bestGain)
            {
                bestGain = gain;
                best = change;
            }
        }

        void offerBest(const ChangeBucket & other)
        {
            if (other.bestGain)
            {
                offer(*other.bestGain, other.best);
            }
        }
    };
}
