#pragma once

#include "ai/coordinator/stockpackingbranch.h"
#include "ai/coordinator/stockpackingprices.h"

namespace Coordinator
{
    inline std::optional<CompatiblePackingResult> compatiblePackingBounds(
        const SequentialInstance &instance, RefinementLedger &ledger,
        const std::vector<std::vector<SequentialDetail::RowOption>> &menus,std::vector<MilliFunds> *prices=nullptr)
    {
        if (instance.rows.size()!=menus.size() || instance.nodeColumns.size()!=instance.capturedNodes.size())
            return std::nullopt;
        const auto start=ledger.stats().spent();
        auto result=pricedStockPacking(menus,instance.capturedNodes,[&](std::int64_t work){
            return ledger.consume(work,RefinementWork::MasterWitness);
        });
        if (!result) return std::nullopt;
        if(prices)*prices=std::move(result->sitePrices);
        CompatiblePackingResult bound{result->lower,result->upper,ledger.stats().spent()-start,std::move(result->witness)};
        if (bound.lower < bound.upper)
        {
            // Whole-site prices can leave an integrality gap. The predecessor's finite
            // search can tighten either endpoint using the same remaining native ledger.
            auto branched=branchStockPacking(instance,ledger,menus);
            if (branched)
            {
                bound.upper=std::min(bound.upper,branched->upper);
                if (branched->lower>bound.lower)
                {
                    bound.lower=branched->lower;
                    bound.witness=std::move(branched->witness);
                }
            }
        }
        bound.work=ledger.stats().spent()-start;
        return bound;
    }
}
