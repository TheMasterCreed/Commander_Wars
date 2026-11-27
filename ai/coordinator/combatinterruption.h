#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/realcombatcapital.h"

namespace Coordinator
{
    struct CaptureInterruptionEstimate
    {
        int turnsBefore{}, turnsAfter{};
        double targetAfter{};
        MilliFunds exchange{};
    };

    // A screening measurement only. A physical full-team reply decides which
    // order actually wins after routes, income and all other units are priced.
    inline std::optional<CaptureInterruptionEstimate> measureCaptureInterruption(const CombatTerrain &terrain,
        const KnownUnit &target, const CandidateBundle &first, const CandidateBundle &second,
        int captureThreshold, int horizon, bool secondFires = true)
    {
        if (!terrain.ownFire || !first.replyFacts || !second.replyFacts) return {};
        const auto &a = first.bundle, &b = second.bundle;
        const auto &opening = a.components.front().fire, &follow = b.components.front().fire;
        if (a.unitId == b.unitId || opening.targetUnitId != follow.targetUnitId ||
            a.destination == b.destination || opening.isolated.damage < 0 || opening.targetRealHp <= 0)
            return {};
        const auto afterFirst = engineHpAfterReply(opening.targetRealHp,engineShotDamage(opening.isolated.damage));
        if (afterFirst <= 0 && secondFires) return {};
        auto rounds = opening.targetAmmo;
        const int counter = opening.isolated.counterWeapon;
        if (opening.isolated.counter >= 0 && counter >= 0 && counter < 2 && rounds[counter] > 0)
            --rounds[counter];
        const auto hpA = first.replyFacts->actorRealHp, hpB = second.replyFacts->actorRealHp;
        double afterBoth = afterFirst, secondAfter = hpB;
        if (secondFires)
        {
            const auto reply = terrain.ownFire(b.unitId,b.destination,follow.targetUnitId,hpB,afterFirst,rounds);
            if (reply.damage < 0) return {};
            afterBoth = engineHpAfterReply(afterFirst,engineShotDamage(reply.damage));
            secondAfter = engineHpAfterReply(hpB,engineShotDamage(reply.counter));
        }
        const auto finish = [&](double real) {
            const auto steps = ceilHpSteps(real);
            const auto rate = replyCaptureRate(target.captureRate,target.hpRounded-steps,steps <= 0);
            const int points = std::max(0,captureThreshold-target.capturePoints);
            return rate > 0 ? std::min(horizon+1,points/rate + int(points%rate != 0)) : horizon+1;
        };
        MilliFunds exchange = combatCapitalLoss(opening.targetReplacementCost,opening.targetRealHp,afterBoth);
        exchange -= combatCapitalLoss(first.actor.replacementCost,hpA,engineHpAfterReply(hpA,engineShotDamage(opening.isolated.counter)));
        exchange -= combatCapitalLoss(second.actor.replacementCost,hpB,secondAfter);
        return CaptureInterruptionEstimate{finish(opening.targetRealHp),finish(afterBoth),afterBoth,exchange};
    }
}
