#pragma once
#include "ai/coordinator/townassault.h"

namespace Coordinator
{
    struct TownWalkOpening
    {
        std::span<const TilePoint> positions;
        std::span<const int> firstFreeDay;
        std::span<const int> casualtyDay;
        ScreenOpening screen;
    };

    struct TownWalkCut
    {
        int holder{},tick{};
        TilePoint post;
    };

    struct TownWalkReview
    {
        int actor{NO_UNIT},side{},departure{},arrival{},movement{};
        TilePoint origin{},target{};
        std::vector<TownWalkCut> cuts;
        std::optional<StockCapturePath> emptyRoute;
        bool inspected{},finished{},rulesCertified{};

        QJsonObject record(const CombatTerrain &terrain) const
        {
            const auto point=[](TilePoint at){return QJsonArray{at.x,at.y};};
            QJsonArray entries,walk;
            for(const auto &c:cuts)entries.append(QJsonObject{{"unit",terrain.units[c.holder].engineUnitId},
                {"tile",point(c.post)},{"requiredOpposingTick",c.tick}});
            if(emptyRoute)for(const auto &s:emptyRoute->steps)
                walk.append(QJsonObject{{"tick",s.tick},{"stop",point(s.tile)}});
            return {{"unit",terrain.units[actor].engineUnitId},{"from",point(origin)},{"requested",point(target)},
                {"earliestOpeningActivation",departure},{"requestedEntry",arrival},{"untrimmedMovement",movement},
                {"fuelRelaxed",true},{"fixedThroughRequestedEntry",rulesCertified},{"emptyBoardWalk",walk},
                {"singleRetainedCuts",entries},{"cutScanComplete",finished},
                {"appliesAfterOptionalGuardMoves",false},{"scope","NAMED_UNASSISTED_WALK_ON_RETAINED_POSTS"}};
        }
    };

    // Every single post capable of blocking *all* walks necessarily intersects
    // this empty-board witness. Test those crossings independently. An ally who
    // chooses to guard later, an unrelated funded unit or a collateral kill
    // cannot be silently introduced into the movement conclusion.
    inline TownWalkReview retainedTownWalk(const TownAssaultBoard &b,const TownAssaultTarget &target,
        const TownWalkOpening &opening,StockRouteLowerBounds &lower,std::int64_t &work)
    {
        TownWalkReview out;
        const int n=static_cast<int>(b.units.size()),side=1-target.defendingSide,tick=target.occupiedFrom;
        if(target.requiredCapper==NO_UNIT || target.occupiedThrough!=tick || tick<0 || tick>b.cutoff || tick%2!=side ||
           target.occupant!=NO_UNIT || target.property.ownerBefore!=(side?OwnerSign::Ours:OwnerSign::Enemy) ||
           n!=static_cast<int>(b.terrain.units.size()) || b.hp.size()!=b.units.size() || b.positions.size()!=b.units.size() ||
           n!=static_cast<int>(opening.positions.size()) || opening.firstFreeDay.size()!=opening.positions.size() ||
           opening.casualtyDay.size()!=opening.positions.size() ||
           b.traffic.cellCount()!=b.terrain.width*b.terrain.height || !b.at)return out;
        const auto cell=[&](TilePoint p){return p.y*b.terrain.width+p.x;};
        const auto found=std::find_if(b.terrain.units.begin(),b.terrain.units.end(),[&](const auto &p){return p.engineUnitId==target.requiredCapper;});
        if(found==b.terrain.units.end())return out;
        const int i=static_cast<int>(found-b.terrain.units.begin()),grid=found->grid>=0?found->grid:found->travelGrid;
        if(i>=n || !found->present || int(found->enemy)!=side || b.hp[i]<=0 || !b.units[i].canCapture ||
           b.units[i].carrier!=KnownUnit::NO_CARRIER || found->movementBeforeFuel<=0 || grid<0 ||
           grid>=static_cast<int>(b.terrain.grids.size()) || opening.firstFreeDay[i]<0 ||
           opening.firstFreeDay[i]>(tick-side)/2)return out;
        const int first=2*opening.firstFreeDay[i]+side;
        const auto origin=opening.positions[i],destination=target.property.tile;
        const auto &terrain=b.terrain.grids[grid];
        if(first>tick || origin!=b.positions[i] || !terrain.onGrid(origin.x,origin.y) ||
           !terrain.onGrid(destination.x,destination.y) || origin==destination)return out;
        out.actor=i;out.side=side;out.origin=origin;out.target=destination;
        out.departure=first;out.arrival=tick;out.movement=found->movementBeforeFuel;
        const auto route=[&](const StockCaptureTraffic &traffic){
            return stockCaptureRoute(terrain,traffic,i,side,origin,destination,out.movement,-1,first,tick,tick,1,
                work,INVALID_TILE,&lower,{},found->ignoresCollision);
        };
        const auto prepare=b.traffic.cellCount();
        if(work<prepare)return out;
        work-=prepare;
        StockCaptureTraffic empty(b.terrain.width,b.terrain.height,b.cutoff);
        const auto plain=route(empty);
        if(!plain)return out;
        out.inspected=true;
        if(plain->entryTick!=tick){out.finished=true;return out;}
        out.emptyRoute=*plain;
        std::set<std::tuple<int,int,int>> examined;
        for(const auto &s:plain->steps)
        {
            auto cells=s.cells;
            if(cells.empty() || cells.back()!=cell(s.tile))cells.push_back(cell(s.tile));
            for(int sq:cells)
            {
                if(work<=0)return out;
                --work;
                for(const auto &use:b.traffic.uses(sq))
                {
                    if(work<=0)return out;
                    --work;
                    const int h=use.actor;
                    if(!use.stopped || h<0 || h>=n || use.side==side || use.first>s.tick || use.last<s.tick ||
                       !b.terrain.units[h].present || b.hp[h]<=0 ||
                       (opening.casualtyDay[h]>=0 && opening.casualtyDay[h]<=s.tick/2) ||
                       int(b.terrain.units[h].enemy)==side)continue;
                    const TilePoint p{sq%b.terrain.width,sq/b.terrain.width};
                    if(opening.positions[h]!=p || b.positions[h]!=p || b.at(h,s.tick)!=p ||
                       !examined.emplace(sq,s.tick,h).second)continue;
                    if(work<prepare)return out;
                    work-=prepare;
                    StockCaptureTraffic one(b.terrain.width,b.terrain.height,b.cutoff);
                    one.reserve(sq,s.tick,s.tick,use.side,h,true);
                    const auto changed=route(one);
                    if(!changed)return out;
                    if(changed->entryTick<0)
                    {
                        if(out.cuts.empty() && b.terrain.certifyRetainedWalkProfile)
                            out.rulesCertified=b.terrain.certifyRetainedWalkProfile(i,tick,work);
                        out.cuts.push_back({h,s.tick,p});
                    }
                }
            }
        }
        out.finished=true;return out;
    }

    inline bool noOtherAttackOnTownWalk(const TownAssaultBoard &b,const TownWalkReview &review,std::int64_t &work)
    {
        if(review.cuts.empty() || b.units[review.actor].hasWeapons || b.units[review.actor].canFire)return false;
        const auto freeToStay=[&](const TownWalkCut &cut){
            if(b.rights[cut.holder].displacedValue!=0)return false;
            if(!b.conditionalClaims)return true;
            const auto it=b.conditionalClaims->find(cut.holder);
            return it==b.conditionalClaims->end() || std::none_of(it->second.begin(),it->second.end(),
                [&](const auto &use){return use.firstShot<=cut.tick;});
        };
        // With other actors, an earlier allied shot or a transport may remove
        // the obstruction. Preserve that independent initiative for pricing.
        for(int i=0;i<static_cast<int>(b.units.size());++i)
            if(i!=review.actor && int(b.terrain.units[i].enemy)==review.side &&
               (b.terrain.units[i].present || b.units[i].carrier!=KnownUnit::NO_CARRIER))return false;
        if(std::any_of(b.recruits.begin(),b.recruits.end(),[&](const auto &g){
            return g.side==review.side && g.buildTick+2<=review.arrival;}))return false;
        if(review.rulesCertified)return std::any_of(review.cuts.begin(),review.cuts.end(),freeToStay);
        // If later seasons/modifiers enlarge range or open a terrain lane, the
        // frozen movement cut alone cannot close the full choice. Grant any
        // square on the map within one activation and recheck the single post.
        const auto cells=b.traffic.cellCount();
        if(work<cells)return false;
        work-=cells;
        MobilityCostGrid relaxed(b.terrain.width,b.terrain.height);
        for(int y=0;y<b.terrain.height;++y)for(int x=0;x<b.terrain.width;++x)relaxed.setTileCost(x,y,1);
        StockRouteLowerBounds fresh;
        for(const auto &cut:review.cuts)
        {
            if(!freeToStay(cut))continue;
            if(work<cells)return false;
            work-=cells;
            StockCaptureTraffic one(b.terrain.width,b.terrain.height,b.cutoff);
            one.reserve(cut.post.y*b.terrain.width+cut.post.x,cut.tick,cut.tick,1-review.side,cut.holder,true);
            const auto route=stockCaptureRoute(relaxed,one,review.actor,review.side,review.origin,review.target,cells,-1,
                review.departure,review.arrival,review.arrival,1,work,INVALID_TILE,&fresh,{},b.terrain.units[review.actor].ignoresCollision);
            if(!route)return false;
            if(route->entryTick<0)return true;
        }
        return false;
    }
}
