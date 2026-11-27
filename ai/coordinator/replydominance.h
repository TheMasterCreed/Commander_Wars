#pragma once

#include <QString>
#include <algorithm>
#include <array>
#include <map>

namespace Coordinator
{
    // The worst reply already tied. With identical legal menus every measured
    // reply participates in Pareto dominance. Across changing menus, require a
    // strict improvement on a common fixed family to keep preferences acyclic.
    template<class Rank>
    bool canImproveReplyOutcomes(const std::map<QString,Rank> &priced,
                                 const std::map<QString,Rank> &prior,
                                 bool complete, bool sameMenuPossible)
    {
        constexpr std::array common{"ECONOMIC","INTERRUPT_CAPTURE","ARMOR_AND_INFANTRY"};
        std::size_t matched=0, commonSeen=0;
        bool better=false, betterCommon=false;
        for (const auto &[name,rank] : priced)
        {
            const auto old=prior.find(name);
            if (old==prior.end()) { sameMenuPossible=false; continue; }
            ++matched;
            if (rank<old->second) return false;
            const bool core=std::find(common.begin(),common.end(),name)!=common.end();
            commonSeen+=core;
            if (rank>old->second) {better=true;betterCommon|=core;}
        }
        if (sameMenuPossible && matched==prior.size()) return better;
        if (complete) return commonSeen==common.size() && betterCommon;
        // A yet-unseen named reply can be the only strict gain on an identical menu.
        return sameMenuPossible || commonSeen<common.size() || betterCommon;
    }
}
