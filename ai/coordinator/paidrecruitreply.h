#pragma once

#include "ai/coordinator/captureapproachsurvival.h"

namespace Coordinator
{
    struct PaidDuelPositions
    {
        std::vector<std::vector<StockCaptureUse>> uses;
        void read(const StockCaptureTraffic &traffic,int cells,int stock,int funded)
        {
            if(!uses.empty())return;
            uses.resize(funded);
            for(int sq=0;sq<cells;++sq)for(const auto &use:traffic.uses(sq))
                if(use.actor>=stock && use.actor-stock<funded)uses[use.actor-stock].push_back(use);
        }
        TilePoint at(int gun,int tick,int width)const
        {
            int since=-1,cell=-1;bool conflicting=false;
            for(const auto &visit:uses[gun])if(visit.stopped && visit.first<=tick && tick<=visit.last)
            {
                if(visit.first>since){since=visit.first;cell=visit.cell;conflicting=false;}
                else if(visit.first==since && cell!=visit.cell)conflicting=true;
            }
            return cell<0 || conflicting ? INVALID_TILE:TilePoint{cell%width,cell/width};
        }
    };

    struct PaidDuelWalk {bool legal{};int fuel{};std::vector<int> cells;};
    // A single actual movement activation using the recruit's directional terrain
    // table, fuel and dated bodies. Equal-cost paths need no special map rule.
    template<class Open>
    std::optional<PaidDuelWalk> paidDuelWalk(const MobilityCostGrid &grid,TilePoint from,TilePoint end,int movement,
        int fuel,int tick,Open open,std::int64_t &budget)
    {
        PaidDuelWalk none;
        if(!grid.onGrid(from.x,from.y) || !grid.onGrid(end.x,end.y) || !open(end,tick,true) || !open(end,tick+1,true))return none;
        if(from==end)return PaidDuelWalk{true};
        const int width=grid.width(),size=width*grid.height(),start=from.y*width+from.x,finish=end.y*width+end.x;
        const int limit=fuel<0?movement:std::min(movement,fuel);
        std::vector<int> costs(size,-1),parents(size,-1);
        using Node=std::pair<int,int>;std::priority_queue<Node,std::vector<Node>,std::greater<Node>> todo;
        costs[start]=0;todo.emplace(0,start);
        while(!todo.empty())
        {
            const auto [cost,sq]=todo.top();todo.pop();if(costs[sq]!=cost)continue;
            if(budget<5)return {};budget-=5;
            if(sq==finish)
            {
                PaidDuelWalk walk{true,cost};
                for(int n=sq;n!=start;n=parents[n])walk.cells.push_back(n);
                std::reverse(walk.cells.begin(),walk.cells.end());return walk;
            }
            const TilePoint p{sq%width,sq/width};
            for(auto dir:ENTRY_DIRECTIONS)
            {
                const auto delta=entryStep(dir);const TilePoint q{p.x+delta.dx,p.y+delta.dy};
                const int step=grid.cost(q.x,q.y,dir);
                if(step<0 || step>limit-cost || !open(q,tick,false))continue;
                const int next=q.y*width+q.x;
                if(costs[next]>=0 && costs[next]<=cost+step)continue;
                costs[next]=cost+step;parents[next]=sq;todo.emplace(cost+step,next);
            }
        }
        return none;
    }

    struct PaidDuelGun
    {
        CaptureSurvivalDetail::Gun gun;
        bool proved{},unknown{};
        QString reason;
        TilePoint start{INVALID_TILE},committed{INVALID_TILE},resume{INVALID_TILE};
        int availableFuel{},replacedFuel{};
        std::array<int,2> ammo{};
    };
    struct PaidDuelSpent
    {
        std::array<int,2> rounds{};
        int fuel{},returnTick{-1};
        double hp{-1};
    };

    inline std::optional<int> replacedDuelTravel(const FundedCaptureGun &gun,int tick,TilePoint start,TilePoint post,
        TilePoint next,int width)
    {
        if(start==post && (next==INVALID_TILE || next==post))return 0;
        const auto &unit=*gun.unit;const int sq=gun.destination.y*width+gun.destination.x;
        if(!unit.movementGrid || sq<0 || sq>=static_cast<int>(unit.routeAt.size()) ||
           unit.routeAt[sq]<0 || unit.routeAt[sq]>=static_cast<int>(unit.routes.size()))return {};
        const auto &route=unit.routes[unit.routeAt[sq]];
        auto from=gun.factory;int t=gun.buildTick+2,used=0;auto stop=route.overnightStops.rbegin();
        TilePoint previous=from,current=from,resume=from;
        for(auto it=route.cells.rbegin();it!=route.cells.rend();++it)
        {
            TilePoint to{*it%width,*it/width};if(manhattanDistance(to,from)!=1)return {};
            const int cost=unit.movementGrid->cost(to.x,to.y,entryDirection(from.x,from.y,to.x,to.y));
            if(cost<0)return {};
            if(t==tick || t==tick+2)used+=cost;
            from=to;
            if(t<tick)previous=to;if(t<=tick)current=to;if(t<=tick+2)resume=to;
            if(stop!=route.overnightStops.rend() && *stop==*it){++stop;t+=2;}
        }
        if(previous!=start || current!=post || (next!=INVALID_TILE && resume!=next))return {};
        return used;
    }

    inline std::optional<PaidDuelGun> paidDuelShot(const CombatTerrain &terrain,
        const StockCaptureTraffic &traffic,const PaidDuelPositions &posts,std::span<const FundedCaptureGun> purchases,
        int stock,int capper,int foe,TilePoint capperAt,int tick,int last,const StockCapturePath &trip,
        const PaidDuelSpent &spent,bool otherCommitment,std::int64_t &budget,const std::function<bool(int,int,int)> &screen={})
    {
        PaidDuelGun result;
        const auto &g=purchases[foe],&c=purchases[capper];const auto &u=*g.unit;
        const int width=terrain.width,actor=stock+foe,body=stock+capper;
        result.start=posts.at(foe,tick-1,width);result.committed=posts.at(foe,tick,width);
        result.resume=tick+2<=last?posts.at(foe,tick+2,width):INVALID_TILE;
        result.ammo=paidLoadAfterService(g,tick,u.captureLoad);
        for(int slot=0;slot<2;++slot)if(result.ammo[slot]>=0) result.ammo[slot]=std::max(0,result.ammo[slot]-spent.rounds[slot]);
        auto unknown=[&](QString s){result.unknown=true;result.reason=std::move(s);return result;};
        if(spent.hp==0){result.reason="SHOOTER_DESTROYED";return result;}
        if(result.start==INVALID_TILE || result.committed==INVALID_TILE)return unknown("MISSING_PAID_ROUTE");
        const auto credit=replacedDuelTravel(g,tick,result.start,result.committed,result.resume,width);
        result.replacedFuel=credit.value_or(0);
        const int fuelAtRelease=paidFuelAtRelease(g,tick);
        result.availableFuel=fuelAtRelease<0?-1:std::max(0,fuelAtRelease+result.replacedFuel-spent.fuel);
        if(g.observedService && tick<g.observedService->nextReady){result.reason="BOOKED_OBSERVED_STOCK_RESPONSE";return result;}
        if(result.ammo[0]==0 && result.ammo[1]==0){result.reason="NO_ROUNDS";return result;}
        const auto capperPosition=[&](int time){
            auto p=c.factory;for(const auto &s:trip.steps)if(s.tick<=time)p=s.tile;return p;
        };
        const auto open=[&](TilePoint p,int t,bool stop){
            if(p.x<0 || p.y<0 || p.x>=terrain.width || p.y>=terrain.height)return false;
            if(!stop && u.ignoresUnitCollision)return true;
            if(p==capperPosition(t))return false;
            const auto &uses=traffic.uses(p.y*width+p.x);
            return std::none_of(uses.begin(),uses.end(),[&](const auto &v){
                return v.actor!=actor && v.actor!=body && v.first<=t && t<=v.last && (v.side!=g.side || (v.stopped && stop));
            });
        };
        const auto *quote=terrain.production && terrain.production->paidWeaponSlots ? &terrain.production->paidWeaponSlots:nullptr;
        bool neededScreen=false;
        const auto admit=[&](TilePoint at,PaidDuelWalk outgoing,PaidDuelWalk returning,bool loan)->bool {
            const int range=manhattanDistance(at,capperAt),sq=at.y*width+at.x;
            if(range<u.minRange || range>u.maxRange || range<=0)return false;
            if(!quote){result.unknown=true;result.reason="MISSING_NATIVE_DUEL";return true;}
            const auto weapons=(*quote)(g,c,range);
            int chosen=-1;double damage=0;
            for(int i=0;i<2;++i)if(result.ammo[i]!=0 && weapons[i]>damage){chosen=i;damage=weapons[i];}
            if(chosen<0){result.reason="NO_DAMAGING_WEAPON";return false;}
            if(loan && screen)for(const auto &old:posts.uses[foe])
                for(int time=std::max(tick,old.first);time<=std::min(tick+2,old.last);++time)
                {
                    if(!screen(foe,old.cell,time))continue;
                    const auto stop=time==tick+2?result.resume:at;
                    bool present=stop.y*width+stop.x==old.cell;
                    if(!old.stopped && time!=tick+1)
                    {
                        const auto &cells=time==tick?outgoing.cells:returning.cells;
                        present|=std::find(cells.begin(),cells.end(),old.cell)!=cells.end();
                    }
                    if(!present){neededScreen=true;return false;}
                }
            if(sq<0 || sq>=static_cast<int>(u.routeAt.size()) || u.routeAt[sq]<0 || u.routeAt[sq]>=static_cast<int>(u.defense.size()))
            {result.unknown=true;result.reason="UNKNOWN_DEFENDER_COVER";return true;}
            if(otherCommitment || spent.returnTick==tick || (g.targetUnit!=NO_UNIT && (tick==g.impactTick ||
                (loan && tick<g.impactTick && tick+2>=g.impactTick))) ||
               (g.targetUnit!=NO_UNIT && tick<g.impactTick && result.ammo[chosen]==1))
            {result.unknown=true;result.reason="COMPETING_PAID_ACTIVATION_OR_ROUND";return true;}
            const auto exposure=g.exposureBeforeFiring.value_or(g.exposure);
            if(u.cost<=0 || exposure>=u.cost){result.unknown=true;result.reason="UNPROVED_SHOOTER_HP";return true;}
            const double hp=spent.hp>=0?spent.hp:paidHpAfterService(g,tick,u.startingHp-(std::max<MilliFunds>(0,exposure)*10+u.cost-1)/u.cost);
            std::array<double,2> counters{-1.,-1.};
            if(range==1 && c.unit->minRange<=1 && c.unit->maxRange>=1)counters=(*quote)(c,g,1);
            result.gun={stock+1+foe,chosen,-1,at,damage,0.,hp,u.defense[u.routeAt[sq]],u.cost,0,false};
            result.gun.counterSlots=counters;
            if(loan)result.gun.deployment={at,std::move(outgoing.cells),std::move(returning.cells),outgoing.fuel+returning.fuel};
            result.proved=true;result.reason=loan?"ROUTED_SHOT_AND_RETURN":"SHOT_ON_COMMITTED_ROUTE";return true;
        };
        if((u.mobileFire || result.start==result.committed) && open(result.committed,tick,true) &&
           admit(result.committed,{true},{true},false))return result;
        if(u.mobileFire && !u.movementGrid && result.committed!=capperAt)return unknown("MISSING_NATIVE_MOVEMENT");
        if(!u.mobileFire || u.movement<=0)
        {if(result.reason.isEmpty())result.reason="NO_SPARE_MOBILE_SHOT";return result;}
        bool reachableShot=false;
        const int radius=std::min(u.maxRange,terrain.width+terrain.height);
        for(int y=std::max(0,capperAt.y-radius);y<=std::min(terrain.height-1,capperAt.y+radius);++y)
            for(int x=std::max(0,capperAt.x-radius);x<=std::min(terrain.width-1,capperAt.x+radius);++x)
            {
                const TilePoint at{x,y};const int d=manhattanDistance(at,capperAt);
                if(d<u.minRange || d>u.maxRange || d<=0 || at==result.committed)continue;
                if(std::int64_t(manhattanDistance(result.start,at))*u.minimumMoveCost>u.movement)continue;
                auto outbound=paidDuelWalk(*u.movementGrid,result.start,at,u.movement,result.availableFuel,tick,open,budget);
                if(!outbound)return {};if(!outbound->legal)continue;
                if(!quote)return unknown("MISSING_NATIVE_DUEL");
                const auto weapons=(*quote)(g,c,d);
                if(!((result.ammo[0]!=0 && weapons[0]>0) || (result.ammo[1]!=0 && weapons[1]>0)))continue;
                reachableShot=true;
                if(tick+2>last || result.resume==INVALID_TILE)continue;
                const int remaining=result.availableFuel<0?-1:result.availableFuel-outbound->fuel;
                auto back=paidDuelWalk(*u.movementGrid,at,result.resume,u.movement,remaining,tick+2,open,budget);
                if(!back)return {};if(!back->legal)continue;
                if(admit(at,std::move(*outbound),std::move(*back),true))return result;
            }
        if(neededScreen)return unknown("PAID_SHOT_CHANGES_EXISTING_SCREEN");
        if(reachableShot && !result.proved)return unknown("UNPRICED_PAID_POST_ALTERNATIVE");
        if(!credit && result.availableFuel>=0 && result.availableFuel<2*u.movement)return unknown("UNVERIFIED_ORIGINAL_FUEL");
        if(result.reason.isEmpty())result.reason="NO_LEGAL_FIRING_SQUARE";
        return result;
    }
}
