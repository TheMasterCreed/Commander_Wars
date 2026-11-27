#pragma once
#include "ai/coordinator/stockcaptureroute.h"
#include <memory>

namespace Coordinator
{
    // A candidate job must refer to its own grid addresses. Comparing addresses
    // from two boards directly would turn original cache hits into cold searches.
    struct FrozenTownGeometry
    {
        using GridMap=std::map<const MobilityCostGrid *,const MobilityCostGrid *>;
        std::vector<std::unique_ptr<StockRouteLowerBounds>> chain;

        bool containsCalendar(std::uint64_t id) const
        {
            for(const auto &node:chain)
                for(const auto &cal:node->m_calendars)if(cal.identity==id)return true;
            return false;
        }

        static std::optional<FrozenTownGeometry> copy(const StockRouteLowerBounds &from,const GridMap &grids)
        {
            std::vector<const StockRouteLowerBounds *> sources;
            for(auto *node=&from;node;node=node->m_prepared) {
                if(std::find(sources.begin(),sources.end(),node)!=sources.end())return {};
                sources.push_back(node);
            }
            FrozenTownGeometry result;
            for(auto it=sources.rbegin();it!=sources.rend();++it) {
                const auto &src=**it;
                auto node=std::make_unique<StockRouteLowerBounds>(result.chain.empty()?nullptr:result.chain.back().get());
                node->datedHits=src.datedHits;
                node->datedStorageLeft=src.datedStorageLeft;
                for(const auto &[key,cost]:src.m_fields) {
                    auto target=grids.find(key.first);
                    if(target==grids.end() || !target->second)return {};
                    if(!node->m_fields.emplace(std::pair{target->second,key.second},cost).second)return {};
                }
                for(const auto &cal:src.m_calendars) {
                    auto &next=node->m_calendars.emplace_back();
                    next.identity=cal.identity;
                    for(const auto &[key,path]:cal.routes) {
                        auto remapped=key;
                        auto target=grids.find(std::get<0>(key));
                        if(target==grids.end() || !target->second)return {};
                        std::get<0>(remapped)=target->second;
                        if(!next.routes.emplace(remapped,path).second)return {};
                    }
                }
                result.chain.push_back(std::move(node));
            }
            return result;
        }

        // Exact comparison following the original-to-frozen mapping. In particular
        // a grid snapshot from a temporary promoted guard is never dereferenced.
        bool sameInput(const StockRouteLowerBounds &source,const GridMap &grids) const
        {
            const auto *real=&source;
            for(auto it=chain.rbegin();it!=chain.rend();++it) {
                const auto &frozen=**it;
                if(!real || real->datedHits!=frozen.datedHits || real->datedStorageLeft!=frozen.datedStorageLeft ||
                   real->m_fields.size()!=frozen.m_fields.size() || real->m_calendars.size()!=frozen.m_calendars.size())return false;
                for(const auto &[key,value]:real->m_fields) {
                    auto grid=grids.find(key.first);
                    if(grid==grids.end())return false;
                    auto field=frozen.m_fields.find({grid->second,key.second});
                    if(field==frozen.m_fields.end() || field->second!=value)return false;
                }
                for(std::size_t i=0;i<real->m_calendars.size();++i) {
                    const auto &a=real->m_calendars[i],&b=frozen.m_calendars[i];
                    if(a.identity!=b.identity || a.routes.size()!=b.routes.size())return false;
                    for(const auto &[key,route]:a.routes) {
                        auto remapped=key;
                        auto grid=grids.find(std::get<0>(key));
                        if(grid==grids.end())return false;
                        std::get<0>(remapped)=grid->second;
                        auto cached=b.routes.find(remapped);
                        if(cached==b.routes.end() || cached->second!=route)return false;
                    }
                }
                real=real->m_prepared;
            }
            return !real;
        }
    };
}
