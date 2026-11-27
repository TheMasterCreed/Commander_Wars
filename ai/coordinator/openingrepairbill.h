#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Coordinator
{
    // Inputs are independently bound to native start-of-turn service eligibility and the dated post-F0 state.
    // Native money units (one fund). Only whole, ordinary service prices belong here; other scripts need their own quote.
    struct OrdinaryOpeningRepair
    {
        int actor{-1},hpPercent{},healingSteps{};
        std::int64_t fundsPerStep{};
        friend bool operator==(const OrdinaryOpeningRepair&,const OrdinaryOpeningRepair&)=default;
    };
    struct PricedOpeningRepair
    {
        int actor{-1},before{},after{};
        std::int64_t debit{};
    };
    struct OpeningRepairBill
    {
        std::int64_t before{},after{};
        std::vector<PricedOpeningRepair> rows;
    };
    // Both the provisional allocation and final ownership replay call this once at E0 after its income, before the first enemy purchase.
    // Each reconstructs a fresh bank. A changed earlier title or a repeated debit makes the pre-bill value fail to match.
    inline bool settleOpeningRepairDebit(const OpeningRepairBill &bill,std::int64_t scale,std::int64_t &bank)
    {
        if(scale<=0 || bill.before<0 || bill.after<0 || bill.after>bill.before || bill.before>std::numeric_limits<std::int64_t>::max()/scale ||
           bank!=bill.before*scale)return false;
        std::int64_t charged=0;
        for(const auto &r:bill.rows){if(r.debit<0 || r.debit>bill.before-charged)return false;charged+=r.debit;}
        if(bill.before-charged!=bill.after)return false;
        bank=bill.after*scale;return true;
    }

    // The native map randomizes the visitation order. If every body can obtain all its service, that unknown order cannot change the bill.
    // Scarce funds are safe with one claimant; two or more competing claims require an authentic visit order, or remain unpriced.
    inline std::optional<OpeningRepairBill> ordinaryOpeningRepairs(std::int64_t afterIncome,
        const std::vector<OrdinaryOpeningRepair> &eligible,bool knownNativeOrder=false)
    {
        if(afterIncome<0)return {};
        OpeningRepairBill bill{afterIncome,afterIncome,{}};std::int64_t total=0;int charged=0;
        for(const auto &u:eligible){
            if(u.actor<0 || u.hpPercent<=0 || u.hpPercent>100 || u.healingSteps<0 || u.fundsPerStep<0 ||
               std::any_of(bill.rows.begin(),bill.rows.end(),[&](const auto &p){return p.actor==u.actor;}))return {};
            const int rounded=(u.hpPercent+9)/10;
            const int maximum=std::min(u.healingSteps,10-rounded);
            if(maximum && u.fundsPerStep>(std::numeric_limits<std::int64_t>::max()-total)/maximum)return {};
            const auto price=u.fundsPerStep*maximum;
            total+=price;charged+=price>0;
            bill.rows.push_back({u.actor,u.hpPercent,10*(rounded+maximum),price});
        }
        if(total>afterIncome && charged>1 && !knownNativeOrder)return {};
        for(std::size_t i=0;i<eligible.size();++i){
            auto &p=bill.rows[i];const auto unitPrice=eligible[i].fundsPerStep;
            if(p.debit>bill.after){
                const auto steps=bill.after/unitPrice;
                p.after=10*((p.before+9)/10+static_cast<int>(steps));p.debit=steps*unitPrice;
            }
            bill.after-=p.debit;
        }
        return bill;
    }
}
