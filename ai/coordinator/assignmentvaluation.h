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

        inline std::optional<MilliFunds> checkedLedgerTotal(const EconomicDelta & economic)
        {
            MilliFunds ledger = 0;
            for (const MilliFunds term : {economic.income, economic.friendlyCapital,
                 economic.enemyCapital, economic.actionCost, economic.resourceCost,
                 economic.scriptedCapital, economic.revaluation})
            {
                if (!RelativeStockDetail::checkedAdd(ledger, term))
                {
                    return std::nullopt;
                }
            }
            return ledger;
        }

        inline bool staticValueFits(const BundleValuation & valuation)
        {
            if (!valuation.valid)
            {
                return false;
            }
            const std::optional<MilliFunds> checkedLedger = checkedLedgerTotal(valuation.ledger);
            if (!checkedLedger)
            {
                return false;
            }
            MilliFunds ledger = *checkedLedger;
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

        inline std::optional<MilliFunds> checkedCapitalAdjustment(
            const FireFacts & fire, MilliFunds staticValue, std::int32_t granted)
        {
            const std::int32_t dealt = std::min(fire.damageSteps, fire.targetHpSteps);
            MilliFunds adjustment = staticValue;
            if (!nonnegativeRoundedProductFits(fire.targetReplacementCost, dealt, UNIT_HP_STEPS) ||
                !nonnegativeRoundedProductFits(fire.targetReplacementCost, granted, UNIT_HP_STEPS) ||
                !RelativeStockDetail::checkedSubtract(
                    adjustment, bookDamage(fire.targetReplacementCost, dealt)) ||
                !RelativeStockDetail::checkedAdd(
                    adjustment, bookDamage(fire.targetReplacementCost, granted)))
            {
                return std::nullopt;
            }
            return adjustment;
        }

        inline bool fireAdjustmentFits(
            const FireFacts & fire, MilliFunds staticValue, std::int32_t granted)
        {
            const std::int32_t dealt = std::min(fire.damageSteps, fire.targetHpSteps);
            MilliFunds staticCredit = fire.targetBestShotBefore;
            const MilliFunds survivingShot =
                dealt >= fire.targetHpSteps ? 0 : fire.targetBestShotAfter;
            const std::optional<MilliFunds> capital = checkedCapitalAdjustment(fire, staticValue, granted);
            if (!RelativeStockDetail::checkedSubtract(staticCredit, survivingShot) || !capital)
            {
                return false;
            }
            MilliFunds adjustment = *capital;
            if (staticCredit < 0)
            {
                return true;
            }
            return RelativeStockDetail::checkedSubtract(adjustment, staticCredit) &&
                   nonnegativeRoundedProductFits(staticCredit, granted, dealt) &&
                   RelativeStockDetail::checkedAdd(
                       adjustment, proRatedCredit(staticCredit, granted, dealt));
        }

        enum class SeatedValueMode : std::int8_t
        {
            Legacy,
            // Leaves out the enemy-turn prices: exposure, target continuation and the destination best shot.
            Acting,
        };

        inline std::optional<MilliFunds> actingStaticValue(const CandidateBundle & candidate)
        {
            const ContinuationDelta & future = candidate.valuation.continuation;
            std::optional<MilliFunds> value =
                candidate.valuation.valid ? checkedLedgerTotal(candidate.valuation.ledger) : std::nullopt;
            if (!value || !RelativeStockDetail::checkedAdd(*value, future.propertyContinuation) ||
                !RelativeStockDetail::checkedAdd(*value, future.partnerContinuation) ||
                !RelativeStockDetail::checkedSubtract(*value, bestShot(candidate.actorNextShotsAtOrigin)))
            {
                return std::nullopt;
            }
            return value;
        }

        inline std::optional<MilliFunds> seatedValue(
            const CandidateBundle & candidate, std::int32_t grantedDamage, SeatedValueMode mode)
        {
            const bool acting = mode == SeatedValueMode::Acting;
            std::optional<MilliFunds> staticValue;
            if (acting)
            {
                staticValue = actingStaticValue(candidate);
            }
            else if (staticValueFits(candidate.valuation))
            {
                staticValue = candidate.valuation.value().economicValue;
            }
            if (!staticValue)
            {
                return std::nullopt;
            }
            const auto & components = candidate.bundle.components;
            if (components.size() != 1 || components.front().kind != ComponentKind::Fire)
            {
                return grantedDamage == 0 ? staticValue : std::nullopt;
            }
            const FireFacts & fire = components.front().fire;
            const std::int32_t dealt = std::min(fire.damageSteps, fire.targetHpSteps);
            if (!isWellFormedFire(fire) || grantedDamage < 0 || grantedDamage > dealt)
            {
                return std::nullopt;
            }
            if (grantedDamage == dealt)
            {
                return staticValue;
            }
            if (acting)
            {
                return checkedCapitalAdjustment(fire, *staticValue, grantedDamage);
            }
            if (!fireAdjustmentFits(fire, *staticValue, grantedDamage))
            {
                return std::nullopt;
            }
            return grantedFireValue(fire, *staticValue, grantedDamage);
        }
    }

    inline std::optional<MilliFunds> checkedSeatedCandidateValue(
        const CandidateBundle & candidate, std::int32_t grantedDamage)
    {
        return AssignmentValuationDetail::seatedValue(
            candidate, grantedDamage, AssignmentValuationDetail::SeatedValueMode::Legacy);
    }

    // The seated value without the enemy-turn prices a plan-level value supplies once.
    inline std::optional<MilliFunds> checkedActingSeatedCandidateValue(
        const CandidateBundle & candidate, std::int32_t grantedDamage)
    {
        return AssignmentValuationDetail::seatedValue(
            candidate, grantedDamage, AssignmentValuationDetail::SeatedValueMode::Acting);
    }
}
