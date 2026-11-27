#pragma once

#include "ai/coordinator/townassault.h"
#include "ai/coordinator/relativestockcomparison.h"
#include <QJsonArray>
#include <QJsonObject>

namespace Coordinator
{
    struct TownAssaultLedger
    {
        MilliFunds ownership{}, capitalLoss{}, timing{}, score{};
        std::vector<MilliFunds> flow;
    };

    inline MilliFunds assaultPaydayChange(const TownAssaultLine &line,const PropertyStockColumn &property,int tick)
    {
        if (line.finishTick<0 || tick<=line.finishTick) return 0;
        const auto next=line.side ? OwnerSign::Enemy : OwnerSign::Ours;
        if (tick%2==0)
            return (int(next==OwnerSign::Ours)-int(property.ownerBefore==OwnerSign::Ours))*property.income.oursPerTurn;
        return (int(next==OwnerSign::Enemy)-int(property.ownerBefore==OwnerSign::Enemy))*(-property.income.enemyPerTurn);
    }

    // Quantify the proposed encounter using the current team's actual prior flow.
    // The rounding in the usual ten-turn valuation must be identical to a full reprice.
    template<class T>
    std::optional<TownAssaultLedger> townAssaultLedger(const TownAssaultLine &line,
        const PropertyStockColumn &property,std::span<const MilliFunds> base,int cutoff,T timing)
    {
        TownAssaultLedger delta;
        delta.flow.assign(base.begin(),base.end());
        for (auto [tick,amount]:line.losses)
        {
            if (tick<0 || tick>cutoff) continue;
            if (!RelativeStockDetail::checkedAdd(delta.capitalLoss,amount)) return {};
            if (tick>0 && !RelativeStockDetail::checkedSubtract(delta.flow.at((tick-1)/2),amount)) return {};
        }
        for (int tick=1;tick<=cutoff;++tick)
        {
            const auto gain=assaultPaydayChange(line,property,tick);
            if (!RelativeStockDetail::checkedAdd(delta.ownership,gain) ||
                !RelativeStockDetail::checkedAdd(delta.flow.at((tick-1)/2),gain)) return {};
        }
        const auto adjusted=timing(delta.flow),original=timing(base);
        if (!adjusted || !original) return {};
        delta.timing=*adjusted;
        if (!RelativeStockDetail::checkedSubtract(delta.timing,*original)) return {};
        delta.score=delta.ownership;
        if (!RelativeStockDetail::checkedSubtract(delta.score,delta.capitalLoss) ||
            !RelativeStockDetail::checkedAdd(delta.score,delta.timing)) return {};
        return delta;
    }

    // The only ownership change in this branch is the contested ordinary property.
    // Preserve the original purchase orders and accept a line only if both dated
    // wallets still fund them. The additional income need not be spent.
    template<class Change>
    std::optional<QJsonArray> townPurchasesWithIncome(const QJsonArray &orders,Change incomeAt)
    {
        QJsonArray revised;
        std::array<MilliFunds,2> change{};
        int previous=0;
        for (const auto &value:orders)
        {
            auto order=value.toObject();
            const int now=order["buildHalfTurn"].toInt(),side=int(order["enemy"].toBool());
            for (int tick=previous+1;tick<=now;++tick)
            {
                const int payer=tick%2;
                const auto gain=incomeAt(tick);
                if(!gain || !(payer?RelativeStockDetail::checkedSubtract(change[payer],*gain):
                              RelativeStockDetail::checkedAdd(change[payer],*gain)))return {};
            }
            MilliFunds before=order["treasuryBefore"].toInteger(),after=order["treasuryAfter"].toInteger();
            if (!RelativeStockDetail::checkedAdd(before,change[side]) ||
                !RelativeStockDetail::checkedAdd(after,change[side]) || after<0 || before<order["price"].toInteger()) return {};
            order["treasuryBefore"]=static_cast<qint64>(before);
            order["treasuryAfter"]=static_cast<qint64>(after);
            revised.append(order);
            previous=now;
        }
        return revised;
    }
    inline std::optional<QJsonArray> townAssaultPurchases(const QJsonArray &orders,
        const TownAssaultLine &line,const PropertyStockColumn &property)
    {
        if(line.finishTick<0)return orders;
        return townPurchasesWithIncome(orders,[&](int tick)->std::optional<MilliFunds>{return assaultPaydayChange(line,property,tick);});
    }
}
