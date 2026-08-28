#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/ownershipschedule.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/terminalvalue.h"

namespace Coordinator
{
    // engine 0 means unlimited, the builder converts at the boundary; 0 here is the final turn
    constexpr std::int32_t NO_TURN_LIMIT = -1;
    // scaffolding until a derived relevance estimate exists, not game wisdom
    constexpr std::int32_t DEFAULT_ECONOMIC_RELEVANCE_HORIZON = 10;

    inline constexpr ReplacementCostPolicy DEFAULT_CAPITAL_POLICY{};

    struct ActorFacts
    {
        MilliFunds replacementCost{0};
        std::int32_t hpSteps{UNIT_HP_STEPS};
    };

    // best shot = book damage on the best legal next activation
    struct FireFacts
    {
        std::int32_t targetUnitId{NO_UNIT};
        MilliFunds targetReplacementCost{0};
        std::int32_t targetHpSteps{0};
        std::int32_t damageSteps{0};
        std::int32_t counterSteps{0};
        MilliFunds targetBestShotBefore{0};
        MilliFunds targetBestShotAfter{0};
    };

    struct CaptureFacts
    {
        PropertyIncome income{};
        OwnerSign ownerBefore{OwnerSign::Neutral};
        // explicit so the player mirror stays an identity
        OwnerSign ownerAfter{OwnerSign::Ours};
        std::int32_t turnsUntilOwned{NO_CAPTURE_TURNS};
    };

    // resupplied ammo is unpriced
    struct ServiceFacts
    {
        std::int32_t partnerUnitId{NO_UNIT};
        MilliFunds partnerReplacementCost{0};
        std::int32_t partnerHpStepsBefore{0};
        std::int32_t partnerHpStepsAfter{0};
        // zero net when it equals bookValue(after) - bookValue(before), the builder prices it so
        MilliFunds repairCost{0};
        MilliFunds partnerBestShotBefore{0};
        MilliFunds partnerBestShotAfter{0};
    };

    // only the payload matching kind is read
    struct BundleComponent
    {
        ComponentKind kind{ComponentKind::Fire};
        FireFacts fire{};
        CaptureFacts capture{};
        ServiceFacts service{};
    };

    constexpr BundleComponent fireComponent(const FireFacts & facts)
    {
        BundleComponent component;
        component.kind = ComponentKind::Fire;
        component.fire = facts;
        return component;
    }

    constexpr BundleComponent captureComponent(const CaptureFacts & facts)
    {
        BundleComponent component;
        component.kind = ComponentKind::Capture;
        component.capture = facts;
        return component;
    }

    constexpr BundleComponent serviceComponent(const ServiceFacts & facts)
    {
        BundleComponent component;
        component.kind = ComponentKind::Service;
        component.service = facts;
        return component;
    }

    // no components and origin == destination is a wait
    struct ActionBundle
    {
        std::int32_t unitId{NO_UNIT};
        TilePoint origin{INVALID_TILE};
        TilePoint destination{INVALID_TILE};
        std::vector<TilePoint> path;
        std::vector<BundleComponent> components;
    };

    // span legality is the builder's contract
    struct PositionFacts
    {
        // origin spans are the do nothing counterfactual, current tile and current hp
        std::span<const MilliFunds> actorNextShotsAtOrigin;
        // destination spans resolve after the bundle, empty when the actor died
        std::span<const MilliFunds> actorNextShotsAtDestination;
        std::span<const MilliFunds> enemyShotsOnActorAtOrigin;
        std::span<const MilliFunds> enemyShotsOnActorAtDestination;
    };

    struct ValuationContext
    {
        std::int32_t horizonTurns{DEFAULT_ECONOMIC_RELEVANCE_HORIZON};
    };

    constexpr std::int32_t resolveHorizon(std::int32_t remainingTurnLimit)
    {
        if (remainingTurnLimit >= 0)
        {
            return remainingTurnLimit;
        }
        return DEFAULT_ECONOMIC_RELEVANCE_HORIZON;
    }

    // best single shot is the one bundle lower bound until the assignment supplies D_enemy deltas
    constexpr MilliFunds bestShot(std::span<const MilliFunds> shots)
    {
        MilliFunds best = 0;
        for (const MilliFunds shot : shots)
        {
            if (shot > best)
            {
                best = shot;
            }
        }
        return best;
    }

    struct ContinuationDelta
    {
        MilliFunds repositioning{0};
        MilliFunds exposure{0};
        MilliFunds targetContinuationRemoved{0};
        MilliFunds propertyContinuation{0};
        MilliFunds partnerContinuation{0};

        constexpr MilliFunds total() const
        {
            return repositioning + exposure + targetContinuationRemoved + propertyContinuation + partnerContinuation;
        }

        friend constexpr bool operator==(const ContinuationDelta &, const ContinuationDelta &) = default;
    };

    struct BundleValuation
    {
        EconomicDelta ledger{};
        ContinuationDelta continuation{};
        // callers must check it, value() of an invalid result is a well ordered zero
        bool valid{false};

        constexpr TerminalValue value() const
        {
            return TerminalValue{TerminalClass::Unresolved, ledger.total() + continuation.total()};
        }
    };

    constexpr PlanBundleKind planBundleKindOf(const ActionBundle & bundle)
    {
        const bool moves = bundle.origin != bundle.destination;
        if (bundle.components.size() > 1)
        {
            return PlanBundleKind::Compound;
        }
        if (bundle.components.empty())
        {
            if (moves)
            {
                return PlanBundleKind::Move;
            }
            return PlanBundleKind::Wait;
        }
        switch (bundle.components.front().kind)
        {
            case ComponentKind::Fire:
                if (moves)
                {
                    return PlanBundleKind::MoveAndFire;
                }
                return PlanBundleKind::Fire;
            case ComponentKind::Capture:
                if (moves)
                {
                    return PlanBundleKind::MoveAndCapture;
                }
                return PlanBundleKind::Capture;
            case ComponentKind::Service:
                break;
        }
        if (moves)
        {
            return PlanBundleKind::MoveAndService;
        }
        return PlanBundleKind::Service;
    }

    // a dead target cannot be shot at
    constexpr bool isWellFormedFire(const FireFacts & facts)
    {
        return facts.targetUnitId != NO_UNIT && facts.targetReplacementCost >= 0 &&
               isLiveHpStepCount(facts.targetHpSteps) &&
               isHpStepCount(facts.damageSteps) && isHpStepCount(facts.counterSteps);
    }

    constexpr bool isWellFormedCapture(const CaptureFacts & facts)
    {
        return facts.turnsUntilOwned >= 0 || facts.turnsUntilOwned == NO_CAPTURE_TURNS;
    }

    // a destroyed partner cannot be revived by a repair
    constexpr bool isWellFormedService(const ServiceFacts & facts)
    {
        return facts.partnerUnitId != NO_UNIT && facts.partnerReplacementCost >= 0 && facts.repairCost >= 0 &&
               isLiveHpStepCount(facts.partnerHpStepsBefore) && isLiveHpStepCount(facts.partnerHpStepsAfter) &&
               facts.partnerHpStepsAfter >= facts.partnerHpStepsBefore;
    }

    constexpr bool isWellFormedComponent(const BundleComponent & component)
    {
        switch (component.kind)
        {
            case ComponentKind::Fire:
                return isWellFormedFire(component.fire);
            case ComponentKind::Capture:
                return isWellFormedCapture(component.capture);
            case ComponentKind::Service:
                break;
        }
        return isWellFormedService(component.service);
    }

    // owner time lives in continuation, never in the ledger
    constexpr MilliFunds capturePropertyContinuation(const CaptureFacts & facts, std::int32_t horizonTurns)
    {
        if (facts.turnsUntilOwned == NO_CAPTURE_TURNS || facts.turnsUntilOwned >= horizonTurns)
        {
            return 0;
        }
        return incomeSwing(facts.income, horizonTurns - facts.turnsUntilOwned, facts.ownerBefore, facts.ownerAfter);
    }

    // shared hp capacity across fire components
    struct TargetChain
    {
        std::int32_t unitId{NO_UNIT};
        std::int32_t remainingHpSteps{0};
        MilliFunds bestShot{0};
    };

    constexpr std::size_t findTargetChain(const std::vector<TargetChain> & chains, std::int32_t unitId)
    {
        for (std::size_t slot = 0; slot < chains.size(); ++slot)
        {
            if (chains[slot].unitId == unitId)
            {
                return slot;
            }
        }
        return chains.size();
    }

    constexpr BundleValuation valueBundle(const ActionBundle & bundle,
                                          const ActorFacts & actor,
                                          const PositionFacts & position,
                                          const ValuationContext & context,
                                          const CapitalPolicy & policy = DEFAULT_CAPITAL_POLICY)
    {
        BundleValuation result;
        if (bundle.unitId == NO_UNIT || !isLiveHpStepCount(actor.hpSteps) || actor.replacementCost < 0 ||
            context.horizonTurns < 0)
        {
            return result;
        }
        for (const BundleComponent & component : bundle.components)
        {
            if (!isWellFormedComponent(component))
            {
                return result;
            }
        }
        std::int32_t remainingActorHpSteps = actor.hpSteps;
        std::vector<TargetChain> targets;
        std::vector<std::int32_t> servicedPartners;
        // consistency is checked on every component, booking stops once the actor is dead
        for (const BundleComponent & component : bundle.components)
        {
            const bool actorAlive = remainingActorHpSteps > 0;
            switch (component.kind)
            {
                case ComponentKind::Fire:
                {
                    const FireFacts & fire = component.fire;
                    const std::size_t slot = findTargetChain(targets, fire.targetUnitId);
                    if (slot == targets.size())
                    {
                        targets.push_back(TargetChain{fire.targetUnitId, fire.targetHpSteps, fire.targetBestShotBefore});
                    }
                    else if (fire.targetHpSteps != targets[slot].remainingHpSteps ||
                             fire.targetBestShotBefore != targets[slot].bestShot)
                    {
                        return BundleValuation{};
                    }
                    if (!actorAlive)
                    {
                        break;
                    }
                    const std::int32_t dealt = std::min(fire.damageSteps, targets[slot].remainingHpSteps);
                    const bool killsTarget = dealt >= targets[slot].remainingHpSteps;
                    std::int32_t taken = 0;
                    MilliFunds survivingShot = 0;
                    if (!killsTarget)
                    {
                        taken = std::min(fire.counterSteps, remainingActorHpSteps);
                        survivingShot = fire.targetBestShotAfter;
                    }
                    remainingActorHpSteps -= taken;
                    targets[slot].remainingHpSteps -= dealt;
                    targets[slot].bestShot = survivingShot;
                    result.ledger.enemyCapital += policy.bookValue(fire.targetReplacementCost, dealt);
                    result.ledger.friendlyCapital -= policy.bookValue(actor.replacementCost, taken);
                    result.continuation.targetContinuationRemoved += fire.targetBestShotBefore - survivingShot;
                    break;
                }
                case ComponentKind::Capture:
                {
                    if (!actorAlive)
                    {
                        break;
                    }
                    result.continuation.propertyContinuation += capturePropertyContinuation(component.capture, context.horizonTurns);
                    break;
                }
                case ComponentKind::Service:
                {
                    const ServiceFacts & service = component.service;
                    if (std::find(servicedPartners.begin(), servicedPartners.end(), service.partnerUnitId) != servicedPartners.end())
                    {
                        return BundleValuation{};
                    }
                    servicedPartners.push_back(service.partnerUnitId);
                    const bool onSelf = service.partnerUnitId == bundle.unitId;
                    // a self repair must start from the hp the earlier components left
                    if (onSelf && service.partnerHpStepsBefore != remainingActorHpSteps)
                    {
                        return BundleValuation{};
                    }
                    if (!actorAlive)
                    {
                        break;
                    }
                    if (onSelf)
                    {
                        remainingActorHpSteps = service.partnerHpStepsAfter;
                    }
                    result.ledger.friendlyCapital += policy.bookValue(service.partnerReplacementCost, service.partnerHpStepsAfter) -
                                                     policy.bookValue(service.partnerReplacementCost, service.partnerHpStepsBefore);
                    result.ledger.actionCost -= service.repairCost;
                    result.continuation.partnerContinuation += service.partnerBestShotAfter - service.partnerBestShotBefore;
                    break;
                }
            }
        }
        MilliFunds actorShotAtDestination = 0;
        MilliFunds enemyShotAtDestination = 0;
        // a dead actor holds no post, so its destination spans are ignored rather than rejected
        if (remainingActorHpSteps > 0)
        {
            actorShotAtDestination = bestShot(position.actorNextShotsAtDestination);
            enemyShotAtDestination = bestShot(position.enemyShotsOnActorAtDestination);
        }
        result.continuation.repositioning = actorShotAtDestination - bestShot(position.actorNextShotsAtOrigin);
        // an enemy gain at the destination lowers our value
        result.continuation.exposure = -(enemyShotAtDestination - bestShot(position.enemyShotsOnActorAtOrigin));
        result.valid = true;
        return result;
    }
}
