#pragma once

#include "ai/coordinator/combatforecast.h"

namespace Coordinator
{
    struct WoundedCaptureWindow
    {
        int firstActivation{}, activations{}, rateUpper{}, carried{};
        std::int64_t pointsUpper{};
    };

    // Conditional only on this original unit not gaining health, capture points
    // or extra actions. Grant arrival on the next activation without movement.
    inline std::optional<WoundedCaptureWindow> woundedCaptureWindow(bool ordinaryRate,double hp,int carried,
        int threshold,int firstActivation,int side,int last)
    {
        if(!ordinaryRate || !std::isfinite(hp) || hp<=0 || hp>=10 || carried<0 || carried>=threshold ||
           threshold!=20 || firstActivation<0 || last<firstActivation-1 || side<0 || side>1 || firstActivation%2!=side)return {};
        const int count=firstActivation>last?0:1+(last-firstActivation)/2;
        const int rate=ceilHpSteps(hp);
        const auto maximum=std::int64_t(carried)+std::int64_t(count)*rate;
        if(maximum>=threshold)return {};
        return WoundedCaptureWindow{firstActivation,count,rate,carried,maximum};
    }
}
