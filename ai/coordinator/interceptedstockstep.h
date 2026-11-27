#pragma once

#include "ai/coordinator/captureapproachsurvival.h"

namespace Coordinator
{
    // A surviving shooter still occupies its post on the claimant's very next
    // activation. A later absence needs its own displacement or casualty witness.
    inline const CaptureRouteShot *crossedInterceptPost(std::span<const CaptureRouteShot> shots,
        const StockCaptureStep &step,int width,bool ignoresCollision)
    {
        for(auto it=shots.rbegin();it!=shots.rend();++it)
        {
            if(it->shotTick<step.tick-1)break;
            if(it->shotTick!=step.tick-1 || it->shooterAfter<=0 || it->from==INVALID_TILE)continue;
            const int sq=it->from.y*width+it->from.x;
            if(step.tile==it->from || (!ignoresCollision && std::find(step.cells.begin(),step.cells.end(),sq)!=step.cells.end()))return &*it;
        }
        return nullptr;
    }

    // Keep the observed prefix, next stop and date. A replacement must fit the
    // original activation's fuel ceiling so every later movement remains funded.
    // A missing answer means work exhausted; false only rejects this repair.
    inline std::optional<bool> repairInterceptedStep(const MobilityCostGrid &grid,const StockCaptureTraffic &traffic,
        std::span<const CaptureRouteShot> shots,int actor,int side,int movement,bool ignoresCollision,
        TilePoint from,StockCaptureStep &step,int &routeFuel,StockRouteLowerBounds &bounds,std::int64_t &work)
    {
        const int width=grid.width();
        if(!grid.onGrid(from.x,from.y) || !grid.onGrid(step.tile.x,step.tile.y) || movement<=0 ||
            step.tick>traffic.lastTick())return false;
        auto at=from;int oldFuel=0;
        for(int sq:step.cells)
        {
            if(work<=0)return {};--work;
            const TilePoint next{sq%width,sq/width};
            if(sq<0 || !grid.onGrid(next.x,next.y) || manhattanDistance(at,next)!=1)return false;
            const int cost=grid.cost(next.x,next.y,entryDirection(at.x,at.y,next.x,next.y));
            if(cost<0 || cost>movement-oldFuel)return false;
            oldFuel+=cost;at=next;
        }
        if(at!=step.tile || routeFuel<oldFuel)return false;
        auto guarded=traffic;
        for(auto it=shots.rbegin();it!=shots.rend();++it)
        {
            if(it->shotTick<step.tick-1)break;
            if(work<=0)return {};--work;
            if(it->shotTick!=step.tick-1 || it->shooterAfter<=0)continue;
            if(!grid.onGrid(it->from.x,it->from.y))return false;
            guarded.reserve(it->from.y*width+it->from.x,step.tick,step.tick,1-side,it->gun,true);
        }
        auto path=stockCaptureRoute(grid,guarded,actor,side,from,step.tile,movement,oldFuel,
            step.tick,step.tick,step.tick,1,work,INVALID_TILE,&bounds,{},ignoresCollision);
        if(!path)return {};
        if(path->entryTick!=step.tick || path->steps.size()!=1 || path->steps.front().tile!=step.tile ||
            path->fuel>oldFuel || crossedInterceptPost(shots,path->steps.front(),width,ignoresCollision))return false;
        routeFuel-=oldFuel-path->fuel;step.cells=std::move(path->steps.front().cells);return true;
    }
}
