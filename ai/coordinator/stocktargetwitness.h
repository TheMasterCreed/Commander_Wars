#pragma once

#include "ai/coordinator/stockpackingsibling.h"
#include <bit>
#include <cstdint>

namespace Coordinator
{
    // Find a disjoint set of original menu entries that reaches a specified
    // certified threshold. The price bound only prunes this witness search;
    // exhausting its grant, or failing to find a witness, returns no result.
    inline std::optional<StockSiblingWitness> stockTargetWitness(
        const SequentialInstance &instance,const std::vector<std::vector<SequentialDetail::RowOption>> &menus,
        std::span<const MilliFunds> prices,MilliFunds required,RefinementLedger &ledger)
    {
        using Money=MilliFunds;
        using Mask=std::uint64_t;
        constexpr Money maximum=std::numeric_limits<Money>::max();
        const int sites=int(prices.size()),count=int(menus.size());
        if(sites<=0 || sites>64 || count<=0 || count>64 || required<=0 ||
           prices.size()!=instance.capturedNodes.size() || menus.size()!=instance.rows.size())return {};
        const auto start=ledger.stats().spent();
        const auto pay=[&](std::int64_t cost){return ledger.consume(cost,RefinementWork::MasterWitness);};
        if(!pay(1+sites+count))return {};
        Money upper=0;Mask priced=0,captured=0;
        for(int j=0;j<sites;++j){
            if(prices[j]<0 || prices[j]>maximum-upper)return {};
            upper+=prices[j];
            if(prices[j])priced|=Mask{1}<<j;
            if(instance.capturedNodes[j])captured|=Mask{1}<<j;
        }
        if(!priced)return {};
        struct Option{int original;Mask mask;Money value,loss;};
        std::vector<std::vector<Option>> rows(count);
        std::vector<Money> tops(count);
        for(int i=0;i<count;++i){
            auto &row=rows[i];row.push_back({-1,0,0,0});
            if(menus[i].size()>std::size_t(std::numeric_limits<int>::max()))return {};
            for(int k=0;k<int(menus[i].size());++k){
                const auto &option=menus[i][k];
                if(!pay(3+std::int64_t(option.nodes.size())) || option.value<0)return {};
                Mask mask=0;Money toll=0;
                for(const int j:option.nodes){
                    if(j<0 || j>=sites || (mask&(Mask{1}<<j)))return {};
                    mask|=Mask{1}<<j;toll+=prices[j];
                }
                if(!mask || (mask&captured))continue;
                const Money net=option.value-toll;
                tops[i]=std::max(tops[i],net);row.push_back({k,mask,option.value,net});
            }
            if(tops[i]>maximum-upper)return {};
            upper+=tops[i];
        }
        if(upper<required)return {};
        const Money gap=upper-required;
        for(int i=0;i<count;++i){
            auto &row=rows[i];
            if(!pay(3LL*row.size()))return {};
            std::erase_if(row,[&](const Option &o){return o.loss<tops[i]-gap;});
            for(auto &o:row)o.loss=tops[i]-o.loss;
            if(row.empty() || !pay(std::int64_t(row.size())*(2+std::bit_width(row.size()))))return {};
            std::sort(row.begin(),row.end(),[](const Option &a,const Option &b){
                if(a.loss!=b.loss)return a.loss<b.loss;
                if(a.value!=b.value)return a.value>b.value;
                return a.mask<b.mask;
            });
        }
        std::vector<int> choice(count,-1);
        bool limited=false;
        const auto search=[&](auto &&self,Mask todo,Mask used,Money spent)->bool{
            if(!pay(1+std::popcount(todo))){limited=true;return false;}
            Mask covered=used;
            int selected=-1,smallest=std::numeric_limits<int>::max();
            Money minimum=0,firstSelected=0;
            for(Mask bits=todo;bits;bits&=bits-1){
                const int i=std::countr_zero(bits);int n=0;Money lowest=0;
                for(const auto &o:rows[i]){
                    if(!pay(2)){limited=true;return false;}
                    if(o.loss>gap-spent)break;
                    if(o.mask&used)continue;
                    if(!n)lowest=o.loss;
                    ++n;covered|=o.mask;
                }
                if(!n || lowest>gap-spent-minimum)return false;
                minimum+=lowest;
                if(n<smallest){selected=i;smallest=n;firstSelected=lowest;}
            }
            for(Mask bits=priced&~covered;bits;bits&=bits-1){
                if(!pay(1)){limited=true;return false;}
                const auto cost=prices[std::countr_zero(bits)];
                if(cost>gap-spent-minimum)return false;
                minimum+=cost;
            }
            if(!todo)return true;
            for(const auto &o:rows[selected]){
                if(!pay(1)){limited=true;return false;}
                if(o.loss>gap-spent || o.loss-firstSelected>gap-spent-minimum)break;
                if(o.mask&used)continue;
                choice[selected]=o.original;
                if(self(self,todo&~(Mask{1}<<selected),used|o.mask,spent+o.loss))return true;
                if(limited)return false;
            }
            return false;
        };
        const Mask all=count==64?~Mask{0}:(Mask{1}<<count)-1;
        if(!search(search,all,0,0))return {};
        Money sum=0;Mask used=captured;
        for(int i=0;i<count;++i){
            if(choice[i]<0)continue;
            const auto &o=menus[i][choice[i]];
            if(!pay(2+std::int64_t(o.nodes.size())) || o.value<0 || o.value>maximum-sum)return {};
            for(const int j:o.nodes){
                const auto bit=Mask{1}<<j;if(used&bit)return {};used|=bit;
            }
            sum+=o.value;
        }
        if(sum<required)return {};
        return StockSiblingWitness{sum,std::move(choice),ledger.stats().spent()-start};
    }
}
