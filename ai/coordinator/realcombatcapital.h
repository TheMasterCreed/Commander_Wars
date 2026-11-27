#pragma once

#include "ai/coordinator/economicledger.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Coordinator
{
    // Unit::getUnitValue values a living unit at its whole-fund cost times its
    // real HP / 10, truncating to funds. The before/after difference telescopes
    // across any number of hits, including chip and overkill.
    inline MilliFunds liveCombatCapital(MilliFunds cost, double hp)
    {
        if (hp <= 0 || cost <= 0) return 0;
        constexpr auto scale = MILLI_FUNDS_PER_FUND;
        if (cost % scale == 0 && cost / scale <= std::numeric_limits<int>::max())
            return static_cast<MilliFunds>((cost / scale) * hp / UNIT_HP_STEPS) * scale;
        // Synthetic/fixed-point prices may be larger than engine prices. Split
        // the product to preserve the whole-fund truncation without overflow.
        const auto hundredths = std::clamp<MilliFunds>(std::llround(hp * 100),0,UNIT_HP_STEPS * 100);
        constexpr auto denominator = scale * UNIT_HP_STEPS * 100;
        return ((cost / denominator) * hundredths + (cost % denominator) * hundredths / denominator) * scale;
    }

    inline MilliFunds combatCapitalLoss(MilliFunds cost, double before, double after)
    {
        return liveCombatCapital(cost, before) - liveCombatCapital(cost, after);
    }
}
