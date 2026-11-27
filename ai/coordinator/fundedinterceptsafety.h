#pragma once

#include "ai/coordinator/fundedintercept.h"

namespace Coordinator
{
    // Consider the actual dated positions of opposing weapons against a
    // supported reserve. A reachable but unpriced enemy-on-recruit matchup
    // withholds the reserve. Never let its nominal original post or the
    // opposing capper serve as a second speculative physical blocker.
    inline std::optional<bool> quietFundedStop(const CombatTerrain &t,std::span<const KnownUnit> units,
        std::span<const double> hp,std::span<const std::array<int,2>> ammo,
        std::span<const FundedCaptureGun> funding,const StockCaptureTraffic &traffic,
        int loan,int capper,const StockCapturePath &route,TilePoint capperOrigin,int divergence,
        TilePoint stop,int tick,std::int64_t &budget,int pricedStationaryReply=-1)
    {
        const int n=static_cast<int>(units.size()),size=t.width*t.height;
        if(loan<0 || loan>=static_cast<int>(funding.size()) || !funding[loan].unit || capper<0 || capper>=n ||
           t.units.size()!=units.size() || hp.size()!=units.size() || ammo.size()!=units.size() || tick<1)return false;
        const auto &reserve=funding[loan];int side=1-reserve.side;
        if(tick%2!=side)return false;
        auto pay=[&](int amount){if(budget<amount)return false;budget-=amount;return true;};
        if(!pay(size+static_cast<int>(n+funding.size())))return {};
        std::vector<std::vector<TilePoint>> starts(n+funding.size());
        for(int sq=0;sq<size;++sq)
            for(const auto &use:traffic.uses(sq))
                if(use.stopped && use.side==side && use.actor>=0 && use.actor<static_cast<int>(starts.size()) &&
                   use.first<=tick-1 && tick-1<=use.last)
                {
                    const TilePoint from{sq%t.width,sq/t.width};auto &locations=starts[use.actor];
                    if(std::find(locations.begin(),locations.end(),from)==locations.end())locations.push_back(from);
                }
        TilePoint before=capperOrigin;
        // A freely diverging capturer may use each subsequent activation.
        // For that one actor use a deliberately optimistic terrain-only reach
        // from the divergence. A failed safety check is an unpriced branch.
        const bool canDivert=tick>=divergence;
        const int fromTime=std::min(tick,divergence);
        for(const auto &s:route.steps)if(s.tick<fromTime)before=s.tile;
        starts[capper]={before};
        if(pricedStationaryReply>=0 && tick>pricedStationaryReply)starts[capper]={route.steps.back().tile};
        const auto cell=[&](TilePoint v){return v.y*t.width+v.x;};
        const auto open=[&](int actor,int sq,bool end) {
            return std::none_of(traffic.uses(sq).begin(),traffic.uses(sq).end(),[&](const auto &use) {
                return use.actor!=actor && use.actor!=n+loan && use.actor!=capper && use.first<=tick && tick<=use.last &&
                       (use.side!=side || (end && use.stopped));
            });
        };
        for(int actor=0;actor<static_cast<int>(starts.size());++actor)
        {
            int lo=0,hi=0,move=0;bool through=false;const MobilityCostGrid *grid=nullptr;
            if(actor<n)
            {
                // The exact in-place cap/attack alternatives on this activation
                // are priced separately. The exit and diversion are compared at
                // the common observed fork, without crediting this gun a ghost hit.
                if(actor==capper && tick==pricedStationaryReply)continue;
                const auto &pawn=t.units[actor];const auto &u=units[actor];
                if(int(pawn.enemy)!=side || hp[actor]<=0 || !(u.canFire || u.hasWeapons) ||
                   (ammo[actor][0]==0 && ammo[actor][1]==0))continue;
                if(actor>=static_cast<int>(reserve.unit->incoming.size()))return false;
                if(reserve.unit->incoming[actor]<=0)continue;
                lo=u.minRange;hi=u.maxRange;
                int tr=pawn.grid>=0?pawn.grid:pawn.travelGrid;
                if(tr>=0){grid=&t.grids[tr];through=pawn.ignoresCollision;}
                if(u.canMoveAndFire)move=pawn.movement;
                if(actor==capper && canDivert && (pricedStationaryReply<0 || tick<pricedStationaryReply))
                    move+=pawn.movement*((tick-divergence)/2);
            }
            else
            {
                const auto &birth=funding[actor-n];
                if(birth.side!=side || !birth.unit || tick<=birth.buildTick || birth.unit->guaranteedShots<=0)continue;
                lo=birth.unit->minRange;hi=birth.unit->maxRange;
                grid=birth.unit->movementGrid.get();move=birth.unit->mobileFire ? birth.unit->movement:0;
            }
            if(hi<lo || hi<=0)continue;
            if(starts[actor].empty())return false;
            for(const auto start:starts[actor])
            {
                if(start==INVALID_TILE)return false;
                const int d=manhattanDistance(start,stop);
                if(d>=std::max(1,lo) && d<=hi)return false;
                if(!move)continue;
                if(!grid)return false;
                std::vector<int> cost(size,std::numeric_limits<int>::max());
                using Node=std::pair<int,int>;
                std::priority_queue<Node,std::vector<Node>,std::greater<Node>> fringe;
                cost[cell(start)]=0;fringe.emplace(0,cell(start));
                while(!fringe.empty())
                {
                    auto [spent,sq]=fringe.top();fringe.pop();
                    if(cost[sq]!=spent)continue;
                    if(!pay(5))return {};
                    const TilePoint from{sq%t.width,sq/t.width};
                    const int range=manhattanDistance(from,stop);
                    const bool unbound=actor==capper && canDivert && tick>divergence &&
                        (pricedStationaryReply<0 || tick<pricedStationaryReply);
                    if(range>=std::max(1,lo) && range<=hi && (unbound || open(actor,sq,true)))return false;
                    for(auto dir:ENTRY_DIRECTIONS)
                    {
                        const auto step=entryStep(dir);const TilePoint next{from.x+step.dx,from.y+step.dy};
                        if(!grid->onGrid(next.x,next.y) || next==stop)continue;
                        const int to=cell(next),price=grid->cost(next.x,next.y,dir);
                        if(price<0 || price>move-spent || spent+price>=cost[to] ||
                           (!unbound && !through && !open(actor,to,false)))continue;
                        cost[to]=spent+price;fringe.emplace(cost[to],to);
                    }
                }
            }
        }
        return true;
    }
}
