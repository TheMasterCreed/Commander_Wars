#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/relativestockcomparison.h"

namespace Coordinator
{
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

    namespace AssignmentValuationDetail
    {
        inline bool nonnegativeRoundedProductFits(
            MilliFunds value, std::int32_t multiplier, std::int32_t denominator)
        {
            if (value < 0 || multiplier < 0 || denominator <= 0)
            {
                return false;
            }
            const MilliFunds maximum = std::numeric_limits<MilliFunds>::max();
            return multiplier == 0 || value <= (maximum - denominator / 2) / multiplier;
        }

        inline bool staticValueFits(const BundleValuation & valuation)
        {
            if (!valuation.valid)
            {
                return false;
            }
            MilliFunds ledger = 0;
            const EconomicDelta & economic = valuation.ledger;
            for (const MilliFunds term : {economic.income, economic.friendlyCapital,
                 economic.enemyCapital, economic.actionCost, economic.resourceCost,
                 economic.scriptedCapital, economic.revaluation})
            {
                if (!RelativeStockDetail::checkedAdd(ledger, term))
                {
                    return false;
                }
            }
            MilliFunds continuation = 0;
            const ContinuationDelta & future = valuation.continuation;
            for (const MilliFunds term : {future.repositioning, future.exposure,
                 future.targetContinuationRemoved, future.propertyContinuation,
                 future.partnerContinuation})
            {
                if (!RelativeStockDetail::checkedAdd(continuation, term))
                {
                    return false;
                }
            }
            return RelativeStockDetail::checkedAdd(ledger, continuation);
        }

        inline bool fireAdjustmentFits(
            const FireFacts & fire, MilliFunds staticValue, std::int32_t granted)
        {
            const std::int32_t dealt = std::min(fire.damageSteps, fire.targetHpSteps);
            MilliFunds staticCredit = fire.targetBestShotBefore;
            const MilliFunds survivingShot =
                dealt >= fire.targetHpSteps ? 0 : fire.targetBestShotAfter;
            if (!RelativeStockDetail::checkedSubtract(staticCredit, survivingShot) ||
                !nonnegativeRoundedProductFits(fire.targetReplacementCost, dealt, UNIT_HP_STEPS) ||
                !nonnegativeRoundedProductFits(fire.targetReplacementCost, granted, UNIT_HP_STEPS))
            {
                return false;
            }
            MilliFunds adjustment = staticValue;
            if (!RelativeStockDetail::checkedSubtract(
                    adjustment, bookDamage(fire.targetReplacementCost, dealt)) ||
                !RelativeStockDetail::checkedAdd(
                    adjustment, bookDamage(fire.targetReplacementCost, granted)))
            {
                return false;
            }
            if (staticCredit < 0)
            {
                return true;
            }
            return RelativeStockDetail::checkedSubtract(adjustment, staticCredit) &&
                   nonnegativeRoundedProductFits(staticCredit, granted, dealt) &&
                   RelativeStockDetail::checkedAdd(
                       adjustment, proRatedCredit(staticCredit, granted, dealt));
        }
    }

    inline std::optional<MilliFunds> checkedSeatedCandidateValue(
        const CandidateBundle & candidate, std::int32_t grantedDamage)
    {
        if (!AssignmentValuationDetail::staticValueFits(candidate.valuation))
        {
            return std::nullopt;
        }
        const MilliFunds staticValue = candidate.valuation.value().economicValue;
        const auto & components = candidate.bundle.components;
        if (components.size() != 1 || components.front().kind != ComponentKind::Fire)
        {
            return grantedDamage == 0 ? std::optional<MilliFunds>{staticValue} : std::nullopt;
        }
        const FireFacts & fire = components.front().fire;
        if (!isWellFormedFire(fire) || grantedDamage < 0 ||
            grantedDamage > std::min(fire.damageSteps, fire.targetHpSteps))
        {
            return std::nullopt;
        }
        if (grantedDamage != std::min(fire.damageSteps, fire.targetHpSteps) &&
            !AssignmentValuationDetail::fireAdjustmentFits(fire, staticValue, grantedDamage))
        {
            return std::nullopt;
        }
        return grantedFireValue(fire, staticValue, grantedDamage);
    }
}
