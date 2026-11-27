#pragma once

#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/stockcaptureroute.h"

namespace Coordinator
{
    // A paid intercept in the retained ledger cannot be omitted or paid for a
    // second time when its original target uses the same post on the same date.
    // The ordinary optional volley does not settle that pre-existing exchange.
    inline std::optional<int> bookedShotAgainstGuard(std::span<const FundedCaptureGun> paid,
        int guard,int guardSide,std::span<const StockCaptureStep> stops,int reveal,std::int64_t &work)
    {
        if(stops.empty())return NO_UNIT;
        for(int i=0;i<static_cast<int>(paid.size());++i)
        {
            if(work<1)return {};
            --work;
            const auto &g=paid[i];
            if(g.targetUnit!=guard || g.side==guardSide || g.impactTick<=stops.front().tick || g.impactTick>reveal)continue;
            // Match the actual stopping square of the guard's prior activation.
            // A transit square or a shot at another target does not qualify.
            const int activation=g.impactTick-1;
            for(const auto &s:stops)
            {
                if(work<1)return {};
                --work;
                if(s.tick>activation)break;
                if(s.tick==activation && s.tile==g.targetTile)return i;
            }
        }
        return NO_UNIT;
    }

    struct BookedGuardCursor
    {
        std::shared_ptr<const int> indexEpoch;
        struct Stop {int tick;TilePoint tile;int first;};
        std::vector<Stop> checked;
        int guard{NO_UNIT},side{-1};
    };

    // A market frame owns one value-identified production portfolio and dated traffic calendar.
    // All target/path dates share this scan. Temporary paid-to-stock views use the direct quote instead.
    struct BookedGuardIndex
    {
        std::shared_ptr<const int> world,epoch;
        std::uint64_t generation{};
        std::size_t count{},next{};
        std::map<std::array<int,5>,int> firstAt;

        std::optional<int> query(const std::shared_ptr<const int> &immutableWorld,std::uint64_t trafficRevision,
            std::span<const FundedCaptureGun> paid,int guard,int side,std::span<const StockCaptureStep> route,int reveal,
            BookedGuardCursor &cursor,std::int64_t &work,bool &advanced)
        {
            if(route.empty())return NO_UNIT;
            if(!immutableWorld || guard<0 || side<0 || side>1 || route.front().tick<0 ||
               !std::is_sorted(route.begin(),route.end(),[](const auto &a,const auto &b){return a.tick<b.tick;}))return bookedShotAgainstGuard(paid,guard,side,route,reveal,work);
            if(world!=immutableWorld || generation!=trafficRevision || count!=paid.size()){
                world=immutableWorld;generation=trafficRevision;count=paid.size();next=0;firstAt.clear();epoch=std::make_shared<const int>(0);
            }
            while(next<count){
                if(work<1)return {};
                --work;const auto index=next++;advanced=true;const auto &g=paid[index];
                if(g.targetUnit!=NO_UNIT && g.impactTick>=0)
                    for(int guardSide=0;guardSide<2;++guardSide)if(guardSide!=g.side)
                        firstAt.try_emplace(std::array{g.targetUnit,guardSide,g.impactTick,g.targetTile.x,g.targetTile.y},static_cast<int>(index));
            }
            if(cursor.indexEpoch!=epoch || cursor.guard!=guard || cursor.side!=side){
                cursor={};cursor.indexEpoch=epoch;cursor.guard=guard;cursor.side=side;
            }
            if(firstAt.empty())return NO_UNIT;
            const auto end=std::lower_bound(route.begin(),route.end(),reveal,[](const auto &s,int tick){return s.tick<tick;});
            const auto eligible=static_cast<std::size_t>(end-route.begin());
            std::size_t common=0;
            while(common<eligible && common<cursor.checked.size() && route[common].tick==cursor.checked[common].tick &&
                  route[common].tile==cursor.checked[common].tile)++common;
            if(common==eligible){
                if(work<1)return {};
                --work;return common?cursor.checked[common-1].first:NO_UNIT;
            }
            cursor.checked.resize(common);
            int first=common?cursor.checked.back().first:NO_UNIT;
            for(std::size_t i=common;i<eligible;++i){
                if(work<1)return {};
                --work;const auto &s=route[i];advanced=true;
                const auto at=firstAt.find({guard,side,s.tick+1,s.tile.x,s.tile.y});
                if(at!=firstAt.end() && (first==NO_UNIT || at->second<first))first=at->second;
                cursor.checked.push_back({s.tick,s.tile,first});
            }
            return first;
        }
    };

}
