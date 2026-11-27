#pragma once

#include "ai/coordinator/orderedopeningreplay.h"
#include "ai/coordinator/openingrepairbill.h"
#include <array>
#include <optional>

namespace Coordinator
{
    enum class OrderedOpeningChoice { Ordinary, ClearAndDecline, ClearAndFire };

    template<class Rank> struct OtherOpeningReplies
    {
        bool complete{};
        std::optional<Rank> lowestKnown;
    };

    template<class Rank> struct OrderedOpeningComparison
    {
        std::array<std::optional<Rank>,3> alternatives;
        std::optional<Rank> lowestKnown;
        // A bounded lane nomination proves the entered reply. Only the entire caller's closed menu can award a worst rank.
        std::optional<Rank> closedWorst;
    };

    // The callback must use the normal economic reply allocator and the entire same paid opening in all three arms.
    // ClearAndDecline releases the reserved gun to its normal stock/capture options *after* the last accepted prefix action.
    // ClearAndFire reserves it until its paid target shot is legally resolved, then gives ordinary jobs to every other unspent unit.
    template<class Rank,class Quote>
    OrderedOpeningComparison<Rank> compareOrderedOpening(const OpeningRepairBill &shared,
        const std::vector<OpeningLaneAction> &witness,const OtherOpeningReplies<Rank> &others,Quote quote)
    {
        OrderedOpeningComparison<Rank> result;
        const std::span<const OpeningLaneAction> prefix(witness);
        result.alternatives[0]=quote(OrderedOpeningChoice::Ordinary,std::span<const OpeningLaneAction>{},shared);
        if(!prefix.empty()){
            result.alternatives[1]=quote(OrderedOpeningChoice::ClearAndDecline,prefix,shared);
            result.alternatives[2]=quote(OrderedOpeningChoice::ClearAndFire,prefix,shared);
        }
        result.lowestKnown=others.lowestKnown;
        bool priced=true;
        for(const auto &a:result.alternatives){
            if(!a){priced=false;continue;}
            if(!result.lowestKnown || *a<*result.lowestKnown)result.lowestKnown=*a;
        }
        if(priced && others.complete)result.closedWorst=result.lowestKnown;
        return result;
    }
}
