#pragma once

#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    struct FundedInterceptionPrice
    {
        TownAssaultLedger change;
        QJsonArray revisedPurchases;
    };

    // The original receipt already owns the post-capture paydays. Replace
    // that transfer by the newly proved date (or complete refusal) and apply
    // only new shot losses. Replaying the old cash flow is the zero control.
    template<class Timing>
    std::optional<FundedInterceptionPrice> replaceProvedCapture(int side,int originalFinish,int revisedFinish,
        const PropertyStockColumn &property,std::span<const MilliFunds> baseline,const QJsonArray &purchases,
        std::span<const std::pair<int,MilliFunds>> newLosses,int cutoff,Timing timing)
    {
        if((side!=0 && side!=1) || originalFinish<0 || originalFinish>cutoff ||
           (revisedFinish>=0 && (revisedFinish<originalFinish || revisedFinish%2!=side)) ||
           cutoff<0 || (cutoff+1)/2>static_cast<int>(baseline.size()))return {};
        FundedInterceptionPrice result;
        auto &d=result.change;d.flow.assign(baseline.begin(),baseline.end());
        TownAssaultLine original,revised;original.side=side;revised.side=side;
        original.finishTick=originalFinish;revised.finishTick=revisedFinish;
        std::vector<MilliFunds> paydays(cutoff+1);
        for(int tick=1;tick<=cutoff;++tick)
        {
            auto money=assaultPaydayChange(revised,property,tick);
            if(!RelativeStockDetail::checkedSubtract(money,assaultPaydayChange(original,property,tick)) ||
               !RelativeStockDetail::checkedAdd(d.flow[(tick-1)/2],money) ||
               !RelativeStockDetail::checkedAdd(d.ownership,money))return {};
            paydays[tick]=money;
        }
        for(const auto &[tick,loss]:newLosses)
        {
            if(tick<1 || tick>cutoff)return {};
            if(!RelativeStockDetail::checkedAdd(d.capitalLoss,loss) ||
               !RelativeStockDetail::checkedSubtract(d.flow[(tick-1)/2],loss))return {};
        }
        auto adjusted=timing(d.flow),before=timing(baseline);
        if(!adjusted || !before)return {};
        d.timing=*adjusted;
        if(!RelativeStockDetail::checkedSubtract(d.timing,*before))return {};
        d.score=d.ownership;
        if(!RelativeStockDetail::checkedSubtract(d.score,d.capitalLoss) ||
           !RelativeStockDetail::checkedAdd(d.score,d.timing))return {};
        int previous=0;std::array<MilliFunds,2> change{};
        for(const auto &v:purchases)
        {
            auto o=v.toObject();const int tick=o["buildHalfTurn"].toInt(),buyer=o["enemy"].toBool();
            if(tick<previous || tick>cutoff)return {};
            for(int t=previous+1;t<=tick;++t)
                if(t%2 ? !RelativeStockDetail::checkedSubtract(change[1],paydays[t]) :
                           !RelativeStockDetail::checkedAdd(change[0],paydays[t]))return {};
            auto cashBefore=o["treasuryBefore"].toInteger(),cashAfter=o["treasuryAfter"].toInteger();
            if(!RelativeStockDetail::checkedAdd(cashBefore,change[buyer]) ||
               !RelativeStockDetail::checkedAdd(cashAfter,change[buyer]) ||
               cashAfter<0 || cashBefore<o["price"].toInteger())return {};
            o["treasuryBefore"]=cashBefore;o["treasuryAfter"]=cashAfter;
            result.revisedPurchases.append(o);previous=tick;
        }
        return result;
    }
}
