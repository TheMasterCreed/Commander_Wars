#pragma once

#include "ai/coordinator/relativestockcomparison.h"

namespace Coordinator
{
    // Receipts from capturable properties only. Pay before acting. A title
    // earns at most one side's native per-turn rate on that side's payday.
    struct OpeningIncomeBounds
    {
        MilliFunds firstEnemy{},firstFriendly{};
        MilliFunds nominalLow{},nominalHigh{},weightedLow{},weightedHigh{};
        int horizon{};
    };

    inline std::optional<OpeningIncomeBounds> boundOpeningIncome(MilliFunds firstEnemy,MilliFunds firstFriendly,
        MilliFunds allEnemy,MilliFunds allFriendly,int horizon)
    {
        if(horizon<=0 || horizon>64 || firstEnemy<0 || firstFriendly<0 ||
           allEnemy<firstEnemy || allFriendly<firstFriendly)return {};
        OpeningIncomeBounds bounds{firstEnemy,firstFriendly,0,0,0,0,horizon};
        MilliFunds low=0,high=0,weightedLow=0,weightedHigh=0;
        const auto cap=std::numeric_limits<MilliFunds>::max()/horizon;
        for(int tick=1;tick<=2*horizon;++tick)
        {
            // E0 and F1 pay before acting. From the following activation on,
            // grant all possible ownership changes, regardless of acting side.
            MilliFunds lo=tick%2?-allEnemy:0,hi=tick%2?0:allFriendly;
            if(tick==1)lo=hi=-firstEnemy;
            else if(tick==2)lo=hi=firstFriendly;
            const int weight=horizon-(tick-1)/2;
            if(lo < -cap || lo>cap || hi < -cap || hi>cap ||
               !RelativeStockDetail::checkedAdd(low,lo) || !RelativeStockDetail::checkedAdd(high,hi) ||
               !RelativeStockDetail::checkedAdd(weightedLow,lo*weight) || !RelativeStockDetail::checkedAdd(weightedHigh,hi*weight))return {};
            // Any prefix (including zero) can be shortened by a terminal.
            bounds.nominalLow=std::min(bounds.nominalLow,low);bounds.nominalHigh=std::max(bounds.nominalHigh,high);
            bounds.weightedLow=std::min(bounds.weightedLow,weightedLow);bounds.weightedHigh=std::max(bounds.weightedHigh,weightedHigh);
        }
        return bounds;
    }
}
