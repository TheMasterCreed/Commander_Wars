#pragma once

#include "ai/coordinator/captureapproachsurvival.h"
#include "ai/coordinator/stockcaptureroute.h"

namespace Coordinator
{
    struct FundedInterceptPath
    {
        TilePoint firing{INVALID_TILE}, observed{INVALID_TILE};
        int observation{-1},departure{-1},shot{-1},back{-1},fuel{};
        std::vector<StockCaptureStep> steps;
    };

    // A bounded two-activation reaction to a separately proved stock trip.
    // Terminating the opposing trip before it reaches the proposed firing
    // square cancels the shot and its payoff. The caller supplies the certified
    // calendar, remaining action rights and proof that each overnight stop is
    // safe. An empty optional means the work limit interrupted the proof.
    inline std::optional<FundedInterceptPath> stageFundedIntercept(const FundedCaptureGun &gun,
        int defender,int capper,TilePoint capperOrigin,const StockCapturePath &trip,TilePoint property,
        int fireTick,int last,const StockCaptureTraffic &calendar,StockRouteLowerBounds &lower,
        const std::function<bool(int,int)> &available,
        const std::function<std::optional<bool>(TilePoint,int)> &safe,std::int64_t &budget,int firstRedeployment=-1)
    {
        FundedInterceptPath none;
        if(!gun.unit || !gun.unit->movementGrid || gun.targetUnit!=NO_UNIT || !safe || !available ||
           trip.entryTick<0 || trip.steps.empty() || trip.steps.back().tile!=property ||
           fireTick<=trip.entryTick || fireTick>last-2 || fireTick%2!=gun.side ||
           defender<0 || capper<0 || gun.destination==INVALID_TILE)return none;
        const auto &unit=*gun.unit;
        if(unit.movement<=0 || unit.guaranteedShots<=0 || unit.attack.size()<=static_cast<std::size_t>(capper) ||
           unit.attack[capper]<=0 || unit.maxRange<1)return none;
        const int departure=fireTick-(unit.mobileFire?2:4),arrival=fireTick-(unit.mobileFire?0:2);
        const int ready=firstRedeployment<0?gun.readyTick+2:std::max(gun.readyTick,firstRedeployment);
        if(departure<ready || !available(departure,fireTick+2))return none;
        int seenTick=-1;
        TilePoint seen=INVALID_TILE,previous=capperOrigin;
        for(const auto &s:trip.steps)
        {
            if(s.tick>=departure)break;
            if(s.tile!=previous || s.tick==trip.entryTick){seenTick=s.tick;seen=s.tile;}
            previous=s.tile;
        }
        if(seenTick<0)return none;
        const auto &grid=*unit.movementGrid;
        const int width=grid.width(),height=grid.height(),range=std::min(unit.maxRange,width+height);
        const auto cell=[&](TilePoint p){return p.y*width+p.x;};
        auto traffic=calendar;
        traffic.vacate(capper,cell(capperOrigin),trip.steps.front().tick);
        for(const auto &s:trip.steps)
        {
            for(int sq:s.cells)traffic.reserve(sq,s.tick,s.tick,1-gun.side,capper,false);
            traffic.reserve(cell(s.tile),s.tick,s.tick+1,1-gun.side,capper,true);
        }
        traffic.reserve(cell(property),trip.entryTick,last,1-gun.side,capper,true);
        std::vector<TilePoint> squares;
        for(int y=std::max(0,property.y-range);y<=std::min(height-1,property.y+range);++y)
            for(int x=std::max(0,property.x-range);x<=std::min(width-1,property.x+range);++x)
            {
                TilePoint at{x,y};const int distance=manhattanDistance(property,at);
                if(distance>=std::max(1,unit.minRange) && distance<=range && at!=gun.destination)squares.push_back(at);
            }
        std::stable_sort(squares.begin(),squares.end(),[&](TilePoint a,TilePoint b){
            return manhattanDistance(a,gun.destination)<manhattanDistance(b,gun.destination);
        });
        std::set<std::pair<int,int>> unavailable;
        const auto admissible=[&](int tick,TilePoint at){return !unavailable.contains({tick,cell(at)});};
        for(const auto end:squares)for(;;)
        {
            auto outward=stockCaptureRoute(grid,traffic,defender,gun.side,gun.destination,end,unit.movement,
                gun.fuelAtPost,departure,arrival,arrival,1+int(!unit.mobileFire),budget,INVALID_TILE,&lower,admissible,unit.ignoresUnitCollision);
            if(!outward)return {};
            if(outward->entryTick!=arrival || outward->steps.size()!=2)break;
            const auto &first=outward->steps.front();
            if(first.tile==gun.destination)break;
            auto witness=FundedInterceptPath{end,seen,seenTick,departure,fireTick,-1,outward->fuel,outward->steps};
            if(!unit.mobileFire)witness.steps.push_back({fireTick,end,{}});
            const int remaining=gun.fuelAtPost<0?-1:gun.fuelAtPost-outward->fuel;
            auto reverse=stockCaptureRoute(grid,traffic,defender,gun.side,end,gun.destination,unit.movement,remaining,
                fireTick+2,fireTick+2,std::min(last,fireTick+4),1,budget,INVALID_TILE,&lower,admissible,unit.ignoresUnitCollision);
            if(!reverse)return {};
            if(reverse->entryTick<0 || !available(departure,reverse->entryTick))break;
            witness.back=reverse->entryTick;witness.fuel+=reverse->fuel;
            witness.steps.insert(witness.steps.end(),reverse->steps.begin(),reverse->steps.end());
            bool survived=true;
            for(const auto &step:witness.steps)
            {
                if(step.tick==witness.back)break;
                const auto safety=safe(step.tile,step.tick+1);
                if(!safety)return {};
                if(!*safety)
                {
                    if(!unavailable.emplace(step.tick,cell(step.tile)).second)return none;
                    survived=false;break;
                }
            }
            if(survived)return witness;
        }
        return none;
    }
}
