#pragma once

#include "ai/coordinator/fundedstockfork.h"

namespace Coordinator
{
    struct StockDiversionFire
    {
        bool complete{true},otherTargetPossible{};
        int earliestReserve{-1};
        std::vector<int> immediateStockContacts;
        QJsonArray alternatives;
    };

    // A property-only diversion bound cannot exclude an ordinary attack.
    // Use the retained dated bodies and an optimistic moving/firing envelope
    // for the whole remainder. An admitted target is an unresolved choice,
    // never damage inflicted by either unit.
    inline StockDiversionFire screenStockDiversionFire(const CombatTerrain &t,std::span<const KnownUnit> units,
        std::span<const double> hp,std::span<const FundedCaptureGun> funds,const StockCaptureTraffic &calendar,
        const FundedCaptureFork &fork,int shooter,int reserve,const FundedInterceptPath &deployed,
        const StockCapturePath &retreat,int cutoff,std::int64_t &budget)
    {
        StockDiversionFire out;
        const int n=static_cast<int>(units.size()),size=t.width*t.height,count=n+static_cast<int>(funds.size());
        if(shooter<0 || shooter>=n || reserve<0 || n+reserve>=count || !funds[reserve].unit ||
           n!=static_cast<int>(t.units.size()) || n!=static_cast<int>(hp.size()) || size<=0 || fork.tick<0 ||
           fork.calendar!=&calendar || fork.revision!=calendar.generation() || fork.unit!=t.units[shooter].engineUnitId)
        {out.complete=false;return out;}
        const auto &cap=units[shooter];const auto &pawn=t.units[shooter];const int side=int(pawn.enemy);
        if(!(cap.canFire || cap.hasWeapons) || cap.maxRange<=0 || cap.maxRange<cap.minRange)return out;
        const int grid=pawn.grid>=0?pawn.grid:pawn.travelGrid;
        if(grid<0 || grid>=static_cast<int>(t.grids.size()) || t.grids[grid].width()!=t.width ||
           t.grids[grid].height()!=t.height || !t.grids[grid].onGrid(fork.tile.x,fork.tile.y) ||
           !t.weaponSlots || budget<4LL*size){out.complete=false;return out;}
        budget-=4LL*size;
        const auto distance=DistanceField::build(t.grids[grid],{fork.tile},FieldExpansion::FromSources);
        std::vector<std::vector<const StockCaptureUse*>> bodies(count);
        for(int cell=0;cell<size;++cell)for(const auto &use:calendar.uses(cell))
        {
            if(budget<=0){out.complete=false;return out;}--budget;
            if(use.stopped && use.side!=side && use.actor>=0 && use.actor<count && use.last>=fork.tick-1)
                bodies[use.actor].push_back(&use);
        }
        auto envelope=[&](int victim,TilePoint at,int tick)->std::optional<bool>{
            // Ignore traffic, fuel and future damage only on this adverse bound.
            // Later replenishment may make a currently empty weapon usable.
            const auto &load=tick==fork.tick?fork.rounds:std::array<int,2>{-1,-1};
            const auto mp=std::int64_t(pawn.movement)*((tick-fork.tick)/2+int(cap.canMoveAndFire));
            for(int d=std::max(1,cap.minRange);d<=cap.maxRange;++d)
            {
                std::array<double,2> base;
                if(victim<n)base=t.weaponSlots(shooter,victim,d);
                else
                {
                    const auto &p=funds[victim-n];
                    if(!p.unit || shooter>=static_cast<int>(p.unit->againstCapper.size()))return {};
                    base=p.unit->againstCapper[shooter];
                }
                if((base[0]<=0 || load[0]==0) && (base[1]<=0 || load[1]==0))continue;
                for(int dx=-d;dx<=d;++dx)for(int sign:{-1,1})
                {
                    if(budget<=0)return {};--budget;
                    const TilePoint origin{at.x+dx,at.y+sign*(d-std::abs(dx))};
                    if(!t.grids[grid].onGrid(origin.x,origin.y))continue;
                    const int reach=distance.distance(origin.x,origin.y);
                    if(reach>=0 && reach<=mp)return true;
                }
            }
            return false;
        };
        const auto &guard=funds[reserve];
        auto paidPost=[&](int tick,bool abandoned){
            TilePoint tile=guard.destination;
            for(const auto &s:deployed.steps)if(s.tick<tick && (!abandoned || s.tick<deployed.shot))tile=s.tile;
            if(abandoned)for(const auto &s:retreat.steps)if(s.tick<tick)tile=s.tile;
            return tile;
        };
        for(int actor=0;actor<count;++actor)
        {
            if(actor<n && (!t.units[actor].present || int(t.units[actor].enemy)==side || hp[actor]<=0))continue;
            if(actor>=n && (!funds[actor-n].unit || funds[actor-n].side==side))continue;
            for(int tick=fork.tick;tick<=cutoff;tick+=2)
            {
                if(budget<=0){out.complete=false;return out;}--budget;
                if(actor>=n && funds[actor-n].buildTick>=tick)continue;
                std::vector<TilePoint> here;
                if(actor==n+reserve)here={paidPost(tick,false),paidPost(tick,true)};
                else for(const auto *use:bodies[actor])
                {
                    if(budget<=0){out.complete=false;return out;}--budget;
                    if(use->first<=tick-1 && tick-1<=use->last)here.push_back({use->cell%t.width,use->cell/t.width});
                }
                // Missing dated occupancy may represent an earlier wound or
                // an unrecorded movement. It supplies no proof of safety.
                if(here.empty()){out.complete=false;break;}
                bool possible=false;
                for(const auto at:here)
                {
                    const auto allowed=envelope(actor,at,tick);
                    if(!allowed){out.complete=false;return out;}
                    if(!*allowed)continue;
                    QJsonObject receipt{{"tick",tick},{"targetStop",QJsonArray{at.x,at.y}},{"paid",actor>=n},
                        {"possibleUnderRelaxedTravel",true}};
                    if(actor<n)receipt["unit"]=t.units[actor].engineUnitId;
                    else
                    {
                        const auto &g=funds[actor-n];receipt["birth"]=QJsonArray{g.factory.x,g.factory.y};
                        receipt["buildTick"]=g.buildTick;receipt["type"]=g.unit->type;
                    }
                    if(out.alternatives.size()<12)out.alternatives.append(receipt);
                    if(actor==n+reserve)out.earliestReserve=tick;else out.otherTargetPossible=true;
                    if(actor<n && tick==fork.tick)out.immediateStockContacts.push_back(actor);
                    possible=true;break;
                }
                if(possible)break;
            }
        }
        return out;
    }
}
