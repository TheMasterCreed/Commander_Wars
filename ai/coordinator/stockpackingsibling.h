#pragma once

#include "ai/coordinator/stockpackingbranch.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Coordinator
{
    struct StockSiblingWitness
    {
        MilliFunds value{};
        // Indices into the candidate's own complete native menus. -1 means idle.
        std::vector<int> witness;
        std::int64_t work{};
    };

    // Find one jointly feasible candidate allocation above a certified threshold.
    // A sibling's site prices only order possible jobs to try. The restricted problem's
    // upper bound has no bearing on the complete candidate and is never returned.
    inline std::optional<StockSiblingWitness> stockSiblingWitness(
        const SequentialInstance &instance,const std::vector<std::vector<SequentialDetail::RowOption>> &menus,
        std::span<const MilliFunds> sitePrices,MilliFunds requiredLower,RefinementLedger &ledger)
    {
        using Money=MilliFunds;
        constexpr auto max=std::numeric_limits<Money>::max();
        constexpr auto maxIndex=static_cast<std::size_t>(std::numeric_limits<int>::max());
        const auto start=ledger.stats().spent();
        const auto charge=[&](std::int64_t n){return ledger.consume(n,RefinementWork::MasterWitness);};
        const auto rows=menus.size(),sites=sitePrices.size();
        if(rows==0 || rows>maxIndex || sites==0 || sites>maxIndex || requiredLower<=0 ||
           instance.rows.size()!=rows || instance.nodeColumns.size()!=sites || instance.capturedNodes.size()!=sites ||
           !charge(1+std::int64_t(rows)+std::int64_t(sites))) return {};
        bool useful=false;
        for(const Money p:sitePrices)
        {
            if(p<0)return {};
            useful|=p!=0;
        }
        if(!useful)return {};
        struct Job { Money net; int original; };
        std::vector<std::vector<SequentialDetail::RowOption>> shortlist(rows);
        std::vector<std::vector<int>> originals(rows);
        // More friendly actors create more chances for their preferred sites to collide.
        // Width affects discovery alone. The ordinary full-menu evaluator handles misses.
        const auto width=2*rows;
        for(std::size_t i=0;i<rows;++i)
        {
            const auto &menu=menus[i];
            if(menu.size()>maxIndex)return {};
            const auto kept=std::min(width,menu.size());
            const auto factor=4+2*std::bit_width(kept);
            if(!charge(1+std::int64_t(menu.size())*std::int64_t(factor)))return {};
            std::vector<Job> jobs;
            jobs.reserve(menu.size());
            for(int k=0;k<static_cast<int>(menu.size());++k)
            {
                const auto &route=menu[k];
                if(route.value<=0)continue;
                if(route.nodes.size()>sites || !charge(2+2*std::int64_t(route.nodes.size())))return {};
                Money net=route.value;
                for(int node:route.nodes)
                {
                    if(node<0 || std::size_t(node)>=sites || instance.capturedNodes[node] ||
                       net<std::numeric_limits<Money>::min()+sitePrices[node])return {};
                    net-=sitePrices[node];
                }
                jobs.push_back({net,k});
            }
            const auto count=std::min(width,jobs.size());
            std::partial_sort(jobs.begin(),jobs.begin()+count,jobs.end(),[](const Job &a,const Job &b){
                return a.net!=b.net ? a.net>b.net : a.original<b.original;
            });
            originals[i].reserve(count);
            shortlist[i].reserve(count);
            for(std::size_t j=0;j<count;++j)
            {
                const auto k=jobs[j].original;
                const auto &source=menu[k];
                if(!charge(2+std::int64_t(source.nodes.size())))return {};
                originals[i].push_back(k);
                SequentialDetail::RowOption option;
                option.value=source.value;
                option.nodes=source.nodes;
                shortlist[i].push_back(std::move(option));
            }
        }
        // Keep enough budget to validate any completed branch witness even if its
        // search consumes every other available unit.
        const Money verifyCost=1+std::int64_t(sites)+std::int64_t(rows)*(2+std::int64_t(sites));
        if(ledger.balance()<=verifyCost)return {};
        std::optional<CompatiblePackingResult> small;
        {
            RefinementLedger::WorkLimit slice(ledger,ledger.balance()-verifyCost);
            small=branchStockPacking(instance,ledger,shortlist);
        }
        if(!small || small->lower<requiredLower || small->witness.size()!=rows || !charge(verifyCost))return {};
        StockSiblingWitness proof;
        proof.witness.resize(rows,-1);
        std::vector<bool> used(sites,false);
        for(std::size_t r=0;r<rows;++r)
        {
            const int index=small->witness[r];
            if(index==-1)continue;
            if(index<0 || std::size_t(index)>=originals[r].size())return {};
            const int k=originals[r][index];
            const auto &source=menus[r][k];
            if(source.value<=0 || source.value>max-proof.value)return {};
            for(int n:source.nodes)
            {
                if(n<0 || std::size_t(n)>=sites || instance.capturedNodes[n] || used[n])return {};
                used[n]=true;
            }
            proof.witness[r]=k;
            proof.value+=source.value;
        }
        if(proof.value!=small->lower || proof.value<requiredLower)return {};
        proof.work=ledger.stats().spent()-start;
        return proof;
    }
}
