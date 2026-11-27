#pragma once

#include "ai/coordinator/productionforecast.h"

namespace Coordinator
{
    struct PaidNeutralOrigin
    {
        TilePoint from;
        int departureTick{},fuel{};
    };

    // `ready` is the first unspent activation, after the caller checks all
    // purchase, gun and screen commitments. The body starts at its stop on
    // the preceding half-turn. Its earlier deployment cannot be replayed.
    inline std::optional<PaidNeutralOrigin> paidNeutralOrigin(const StockCaptureTraffic &traffic,
        int actor,const FundedCaptureGun &g,int ready)
    {
        if(actor<0 || !g.unit || !g.unit->movementGrid || ready<g.buildTick+2 || ready%2!=g.side)return {};
        const auto &unit=*g.unit;const auto &grid=*unit.movementGrid;const int width=grid.width();
        if(!grid.onGrid(g.factory.x,g.factory.y) || !grid.onGrid(g.destination.x,g.destination.y))return {};
        const auto cell=[&](TilePoint p){return p.y*width+p.x;};
        auto booked=[&](int sq,int t,bool stop){
            const auto &uses=traffic.uses(sq);
            return std::any_of(uses.begin(),uses.end(),[&](const auto &u){
                return u.actor==actor && u.side==g.side && u.stopped==stop && u.first<=t && t<=u.last;
            });
        };
        const auto source=[&](TilePoint at,int fuel)->std::optional<PaidNeutralOrigin>{
            const int sq=cell(at),prior=ready-1;
            if(!booked(sq,prior,true) || !traffic.open(actor,g.side,sq,prior,true))return {};
            return PaidNeutralOrigin{at,ready,fuel};
        };
        if(g.observedService && ready>=g.observedService->nextReady)
            return source(g.destination,paidFuelAtRelease(g,ready));
        if(g.observedService && ready>=g.observedService->departure)return {};
        if(ready==g.buildTick+2)return source(g.factory,unit.initialFuel);
        if(g.factory==g.destination)
        {
            if(g.fuelAtPost>=0 || unit.initialFuel<0)return source(g.factory,g.fuelAtPost);
            const auto &uses=traffic.uses(cell(g.factory));
            if(std::none_of(uses.begin(),uses.end(),[&](const auto &s){return s.actor==actor && s.stopped &&
                s.first<=g.buildTick && s.last>=ready-1;}))return {};
            return source(g.factory,unit.initialFuel);
        }
        const int end=cell(g.destination);
        const bool knownPostFuel=g.fuelAtPost>=0 || unit.initialFuel<0;
        // The reached gun post already carries its verified remaining fuel.
        // It can outlive a claim or an assigned future firing activation.
        if(ready>g.readyTick && knownPostFuel && booked(end,ready-1,true))
            return source(g.destination,g.fuelAtPost);
        if(end>=static_cast<int>(unit.routeAt.size()) || unit.routeAt[end]<0 ||
           unit.routeAt[end]>=static_cast<int>(unit.routes.size()))return {};
        const auto &route=unit.routes[unit.routeAt[end]];
        if(route.destination!=g.destination)return {};
        if(ready>g.buildTick+2*route.activations && knownPostFuel)
            return source(g.destination,g.fuelAtPost);
        // The action became free mid-deployment. Debit only its confirmed
        // prefix, using the same directional movement table as the buyer.
        int t=g.buildTick+2,fuel=unit.initialFuel,used=0;
        TilePoint at=g.factory;auto stop=route.overnightStops.rbegin();
        for(auto it=route.cells.rbegin();it!=route.cells.rend() && t<ready;++it)
        {
            const int sq=*it;const TilePoint next{sq%width,sq/width};
            if(!grid.onGrid(next.x,next.y) || manhattanDistance(at,next)!=1 || !booked(sq,t,false))return {};
            const int cost=grid.cost(next.x,next.y,entryDirection(at.x,at.y,next.x,next.y));
            if(cost<0 || cost>unit.movement-used || (fuel>=0 && cost>fuel))return {};
            used+=cost;if(fuel>=0)fuel-=cost;at=next;
            if(stop!=route.overnightStops.rend() && *stop==sq)
            {
                if(!booked(sq,t,true) || !booked(sq,t+1,true))return {};
                ++stop;t+=2;used=0;
            }
        }
        return source(at,fuel);
    }

    // Construct once for an immutable funded portfolio being scored. Each
    // actor/release pair pays its native prefix walk once across all towns;
    // changing dated traffic discards the previously checked origins.
    class PaidNeutralOriginCache
    {
    public:
        PaidNeutralOriginCache(const StockCaptureTraffic &traffic,std::span<const FundedCaptureGun> recruits,int stock)
            : m_traffic(traffic),m_recruits(recruits),m_stock(stock),m_revision(traffic.generation()) {}

        std::optional<PaidNeutralOrigin> get(int index,int ready)
        {
            if(index<0 || index>=static_cast<int>(m_recruits.size()))return {};
            if(m_revision!=m_traffic.generation()){m_origins.clear();m_revision=m_traffic.generation();}
            auto [it,fresh]=m_origins.try_emplace({index,ready});
            if(fresh)it->second=paidNeutralOrigin(m_traffic,m_stock+index,m_recruits[index],ready);
            return it->second;
        }
    private:
        const StockCaptureTraffic &m_traffic;
        std::span<const FundedCaptureGun> m_recruits;
        int m_stock;
        std::uint64_t m_revision;
        std::map<std::pair<int,int>,std::optional<PaidNeutralOrigin>> m_origins;
    };
}
