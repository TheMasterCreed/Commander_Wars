#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <vector>

namespace Coordinator
{
    struct PricedStockPacking
    {
        std::int64_t lower{}, upper{};
        std::vector<int> witness;
        // Prices of the best complete dual scan, in the same units as the row values.
        std::vector<std::int64_t> sitePrices;
    };

    // Complete per-actor menus. At most one route per actor and one claimant per site.
    // A nonnegative site price vector gives a ceiling independent of any search convergence.
    template<class Menus, class Captured, class Pay>
    std::optional<PricedStockPacking> pricedStockPacking(const Menus &menus, const Captured &captured,
                                                         Pay pay, int maxRounds = 128)
    {
        using Money = std::int64_t;
        constexpr Money limit = std::numeric_limits<Money>::max();
        const auto maxIndex = static_cast<std::size_t>(std::numeric_limits<int>::max());
        if (menus.size() > maxIndex || captured.size() > maxIndex ||
            !pay(1 + 2 * Money(menus.size()) + Money(captured.size()))) return std::nullopt;
        const int count = static_cast<int>(menus.size()), sites = static_cast<int>(captured.size());
        struct Route {
            Money value{}, net{};
            std::vector<int> sites;
            int original{-1}, maskRank{};
        };
        std::vector<std::vector<Route>> rows(count);
        PricedStockPacking result{0, 0, std::vector<int>(count, -1), std::vector<Money>(sites)};
        Money quantum = 0, tariff = 16, nodeCount = 0;
        const auto add = [](Money &x, Money y) {
            if (y > limit-x) return false;
            x += y;
            return true;
        };
        for (int r = 0; r < count; ++r)
        {
            const auto &menu = menus[r];
            if (menu.size() >= maxIndex || !pay(1 + Money(menu.size()))) return std::nullopt;
            auto &row = rows[r];
            row.reserve(menu.size()+1);
            row.push_back({}); // Idle is always legal.
            std::size_t longest = 0;
            for (int k = 0; k < static_cast<int>(menu.size()); ++k)
            {
                const auto &op = menu[k];
                if (op.nodes.size() > maxIndex ||
                    !pay(1 + Money(op.nodes.size())*(2+std::bit_width(op.nodes.size())))) return std::nullopt;
                std::vector<int> nodes(op.nodes.begin(),op.nodes.end());
                std::sort(nodes.begin(),nodes.end());
                if (std::adjacent_find(nodes.begin(),nodes.end()) != nodes.end()) return std::nullopt;
                for (int n : nodes)
                    if (n < 0 || n >= sites || captured[n]) return std::nullopt;
                if (op.value <= 0) continue;
                longest = std::max(longest,nodes.size());
                if (!add(nodeCount,Money(nodes.size()))) return std::nullopt;
                row.push_back({op.value, 0, std::move(nodes), k, 0});
                quantum = std::gcd(quantum,Money(op.value));
            }
            const Money comparisons = Money(row.size()) * (1 + std::bit_width(row.size()));
            if (longest+2 > static_cast<std::size_t>(limit/comparisons) ||
                !pay(comparisons*Money(longest+2)) || !pay(comparisons)) return std::nullopt;
            // A rank orders even sparse masks with more than 64 sites deterministically.
            std::sort(row.begin(),row.end(),[](const Route &a, const Route &b) {
                if (a.sites == b.sites) return a.original < b.original;
                return std::lexicographical_compare(a.sites.rbegin(),a.sites.rend(),
                                                    b.sites.rbegin(),b.sites.rend());
            });
            for (int i=0; i<static_cast<int>(row.size()); ++i) row[i].maskRank=i;
            std::sort(row.begin(),row.end(),[](const Route &a, const Route &b) {
                if (a.value != b.value) return a.value>b.value;
                return std::tuple{a.sites.size(),a.maskRank} < std::tuple{b.sites.size(),b.maskRank};
            });
            if (!add(result.upper,row.front().value) || !add(tariff,12*Money(row.size()))) return std::nullopt;
        }
        if (!quantum || !count) return result;
        // In our supported arithmetic range every price is <= independentUpper, so site tolls,
        // the full dual and all nonnegative original-payoff sums can be evaluated exactly.
        const Money independentUpper = result.upper;
        if (independentUpper > limit/(1+Money(sites)) || nodeCount > limit/6 ||
            !add(tariff,6*nodeCount) || !add(tariff,12*(Money(sites)+count))) return result;

        std::vector<Money> prices(sites), usage(sites);
        std::vector<unsigned char> taken(sites);
        std::vector<int> choice(count);
        int flat = 0, denominator = 1;
        for (int epoch = 0; epoch < maxRounds; ++epoch)
        {
            // Covers the full sparse row/claim scans, usage/fund totals, and copying a new proof.
            if (!pay(tariff)) return result;
            std::fill(usage.begin(),usage.end(),0);
            Money upper = std::accumulate(prices.begin(),prices.end(),Money{});
            for (int j = 0; j < count; ++j)
            {
                auto &row = rows[(Money(epoch)+j)%count];
                Money top = -limit;
                std::tuple<Money,std::size_t,int> selected{};
                const Route *best = nullptr;
                for (auto &route : row)
                {
                    Money toll=0, congestion=0;
                    for (int n : route.sites) { toll+=prices[n]; congestion+=usage[n]; }
                    route.net=route.value-toll;
                    const auto tie=std::tuple{congestion,route.sites.size(),route.maskRank};
                    if (route.net>top || (route.net==top && tie<selected))
                    {
                        top=route.net; selected=tie; best=&route;
                    }
                }
                upper+=top;
                for (int n : best->sites) ++usage[n];
            }
            // Each integral packing value is a multiple of the gcd of ALL positive route values.
            bool improved=false;
            const Money rounded=upper-upper%quantum;
            if (rounded<result.upper)
            {
                result.upper=rounded;
                result.sitePrices=prices;
                improved=true;
            }
            if (result.lower==result.upper) break;
            std::fill(taken.begin(),taken.end(),0);
            Money total=0;
            for (int j=0; j<count; ++j)
            {
                const int r=static_cast<int>((Money(epoch)+j)%count);
                const auto &row=rows[r];
                int selected=-1;
                for (int k=0; k<static_cast<int>(row.size()); ++k)
                {
                    const auto &route=row[k];
                    if (std::any_of(route.sites.begin(),route.sites.end(),[&](int n){return taken[n];})) continue;
                    const auto priority=[](const Route &entry){return std::tuple{entry.net,entry.value,-entry.maskRank};};
                    if (selected<0 || priority(route)>priority(row[selected])) selected=k;
                }
                choice[r]=selected;
                total+=row[selected].value;
                for (int n : row[selected].sites) taken[n]=1;
            }
            auto publish=[&] {
                if (total<=result.lower) return;
                result.lower=total;
                for (int r=0;r<count;++r) result.witness[r]=rows[r][choice[r]].original;
                improved=true;
            };
            const Money previous=result.lower;
            publish();
            if (epoch%5==0 && (previous<=total || (previous-total)/quantum<=3) && result.lower<result.upper)
            {
                // Replace two routes only when their union is still disjoint from all other actors.
                // Bit marking is over actual site indices and never uses a fixed-size map mask.
                for (int i=0;i<count;++i)
                {
                    for (int k=0;k<i;++k)
                    {
                        const auto &first=rows[i], &second=rows[k];
                        const auto mark=[&](const Route &r,unsigned char v){for (int n : r.sites) taken[n]=v;};
                        const auto blocked=[&](const Route &r){
                            return std::any_of(r.sites.begin(),r.sites.end(),[&](int n){return taken[n];});
                        };
                        Money pair=first[choice[i]].value+second[choice[k]].value;
                        const Money original=pair;
                        int left=choice[i],right=choice[k];
                        if (!pay(12+2*Money(first[left].sites.size()+second[right].sites.size()))) return result;
                        mark(first[left],0); mark(second[right],0);
                        for (int a=0;a<static_cast<int>(first.size());++a)
                        {
                            const auto &x=first[a];
                            if (!pay(3+3*Money(x.sites.size()))) return result;
                            if (x.value+second.front().value<=pair) break;
                            if (blocked(x)) continue;
                            mark(x,1);
                            for (int b=0;b<static_cast<int>(second.size());++b)
                            {
                                const auto &y=second[b];
                                if (!pay(3+Money(y.sites.size()))) return result;
                                if (x.value+y.value<=pair) break;
                                if (!blocked(y)) { pair=x.value+y.value; left=a; right=b; break; }
                            }
                            mark(x,0);
                        }
                        if (!pay(2+Money(first[left].sites.size()+second[right].sites.size()))) return result;
                        mark(first[left],1); mark(second[right],1);
                        total+=pair-original;
                        choice[i]=left; choice[k]=right;
                    }
                }
                publish();
            }
            if (result.lower==result.upper) break;
            // These steps only propose future duals. Any cutoff retains the last completed bounds.
            if (improved) flat=0;
            else if (++flat>=16) { denominator=std::min(16,2*denominator); flat=0; }
            Money norm=0;
            for (int n=0;n<sites;++n)
            {
                auto &g=usage[n];
                g=prices[n] || g>1 ? g-1 : 0;
                if (!add(norm,g*g)) return result;
            }
            if (!norm) break;
            const Money step=std::max({Money{1},quantum/50,(upper-result.lower)/norm/denominator});
            for (int n=0;n<sites;++n)
            {
                const Money g=usage[n];
                if (g<0) prices[n]=std::max<Money>(0,prices[n]-step);
                else if (g>0) prices[n]=step>(independentUpper-prices[n])/g ? independentUpper : prices[n]+step*g;
            }
        }
        return result;
    }
}
