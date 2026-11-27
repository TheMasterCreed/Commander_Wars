#pragma once

#include "ai/coordinator/townassault.h"
#include "ai/coordinator/paidneutralorigin.h"

namespace Coordinator
{
    struct NeutralRaceEntrant
    {
        int actor,side,departure,ready,earliest,movement,fuel;
        TilePoint from;
        const MobilityCostGrid *grid;
        bool ignoresCollision;
        bool originUnknown{};
        bool conditionalDuty{};
    };

    struct NeutralCaptureOffer
    {
        TownAssaultTarget target;
        int first{};
        std::vector<int> earlierClaimants;
        std::vector<NeutralRaceEntrant> potentialRivals;
    };

    // A fast terrain bound can put an opponent on the wrong side of a wall
    // of real units. Check each earlier rival on the dated calendar before it
    // preempts a proposed entry. An exhausted optional check earns no gain.
    struct NeutralOccupancy
    {
        int actor{NO_UNIT},entry{-1};
        StockCapturePath path;
        bool originUnknown{};
        TilePoint origin{INVALID_TILE};
        int departure{-1},initialFuel{-1};
        bool conditionalDuty{};
    };
    struct NeutralRouteHistory
    {
        struct Entry {int impossibleThrough{-1};std::optional<NeutralOccupancy> arrival;};
        // One immutable calendar. Distinct origins, activation rights and
        // movement/fuel states never donate a route to a different unit.
        std::map<std::array<int,12>,Entry> routes;
    };
    inline std::optional<NeutralOccupancy> earlierNeutralOccupant(const TownAssaultBoard &board,TilePoint tile,int before,
        const std::vector<NeutralRaceEntrant> &rivals,StockRouteLowerBounds &lower,std::int64_t &budget,
        NeutralRouteHistory *history=nullptr,int surveyBefore=-1)
    {
        for(const auto &r:rivals)
        {
            int last=before-1;if(last%2!=r.side) --last;
            if(r.ready>last || r.earliest>last) continue;
            if(r.originUnknown)return NeutralOccupancy{r.actor,-1,{},true};
            NeutralRouteHistory::Entry *proof=nullptr;
            if(history)
            {
                if(budget<=0)return {};--budget;
                proof=&history->routes[{tile.x,tile.y,r.actor,r.side,r.from.x,r.from.y,r.departure,r.ready,r.movement,r.fuel,int(r.ignoresCollision),int(r.conditionalDuty)}];
                if(proof->arrival)
                {
                    if(proof->arrival->entry<=last)return proof->arrival;
                    // stockCaptureRoute returns the earliest feasible landing.
                    // The same witness therefore rules out every earlier date.
                    continue;
                }
                if(proof->impossibleThrough>=last)continue;
            }
            int through=last;
            if(proof && surveyBefore>before)
            {
                through=surveyBefore-1;
                if(through%2!=r.side)--through;
            }
            const auto arrived=[&](int t,StockCapturePath path){
                NeutralOccupancy found{r.actor,t,std::move(path),false,r.from,r.departure,r.fuel,r.conditionalDuty};
                if(proof)proof->arrival=found;return found;
            };
            if(r.from==tile)
            {
                for(int t=r.ready;t<=through;t+=2)
                    if(board.traffic.open(r.actor,r.side,TownAssaultDetail::cell(board.terrain,tile),t,true) &&
                       board.traffic.open(r.actor,r.side,TownAssaultDetail::cell(board.terrain,tile),t+1,true))
                    {
                        auto found=arrived(t,StockCapturePath{t,0,{{t,tile,{}}}});
                        if(t<=last)return found;
                        break;
                    }
            }
            else
            {
                auto path=stockCaptureRoute(*r.grid,board.traffic,r.actor,r.side,r.from,tile,r.movement,r.fuel,
                    r.departure,r.ready,through,1,budget,INVALID_TILE,&lower,{},r.ignoresCollision);
                if(!path)return {};
                if(path->entryTick>=0)
                {
                    auto found=arrived(path->entryTick,std::move(*path));
                    if(found.entry<=last)return found;
                    continue;
                }
            }
            if(proof && !proof->arrival)proof->impossibleThrough=std::max(proof->impossibleThrough,through);
        }
        return NeutralOccupancy{};
    }


    // A denied proposal never takes ownership or preempts another capper.
    // Reopen only that neutral property. A rival's earliest possible arrival
    // bounds the entry window; the actual price still requires movement,
    // survival, held installments and the post-capture bank ledger.
    inline std::vector<NeutralCaptureOffer> neutralStockRebids(const TownAssaultBoard &board,
        const PropertyStockColumn &property,std::span<const int> previous,const StockRouteLowerBounds &lower,
        bool includeUnclaimed=false,int requiredCapper=NO_UNIT,std::array<int,2>* firstBids=nullptr,std::array<int,2>* firstOccupants=nullptr,int desiredTick=-1,bool vacantOwned=false,int afterTitle=2,PaidNeutralOriginCache *origins=nullptr)
    {
        std::vector<NeutralCaptureOffer> result;
        if ((!includeUnclaimed && previous.empty()) || (!vacantOwned && property.ownerBefore!=OwnerSign::Neutral) || board.cutoff<2) return result;
        const int n=static_cast<int>(board.units.size()),last=board.cutoff;
        const int square=TownAssaultDetail::cell(board.terrain,property.tile);
        std::array<int,2> possible{last+1,last+1},newBid=possible;
        std::vector<NeutralRaceEntrant> entrants;
        const auto earliest=[&](const MobilityCostGrid &grid,TilePoint from,int movement,int fuel,int ready,int floor)->int {
            if (ready<0 || ready>last || from==INVALID_TILE || !grid.onGrid(property.tile.x,property.tile.y)) return last+1;
            if (from==property.tile) return ready;
            if (movement<=0 || fuel==0) return last+1;
            const auto *costs=lower.cached(grid,square);
            const auto distance=costs ? std::int64_t((*costs)[TownAssaultDetail::cell(board.terrain,from)]) :
                std::int64_t(manhattanDistance(from,property.tile))*floor;
            if (distance==std::numeric_limits<int>::max() || (fuel>=0 && distance>fuel)) return last+1;
            return static_cast<int>(std::min<std::int64_t>(last+1,ready+2*std::max<std::int64_t>(0,(distance+movement-1)/movement-1)));
        };
        for(int i=0;i<n;++i)
        {
            const auto &pawn=board.terrain.units[i];const auto &unit=board.units[i];const auto &right=board.rights[i];
            const int profile=pawn.grid>=0 ? pawn.grid : pawn.travelGrid;
            if(board.hp[i]<=0 || profile<0 || board.positions[i]==INVALID_TILE) continue;
            const int side=int(pawn.enemy);const auto &grid=board.terrain.grids[profile];
            int floor=0;
            if(const auto *geometry=board.terrain.movementCache.get();geometry && geometry->minimumEntryCost.contains(&grid))
                floor=geometry->minimumEntryCost.at(&grid);
            else if(board.minimumEntryCost.contains(profile)) floor=board.minimumEntryCost.at(profile);
            // A rejected trip did not spend its hypothetical interceptor's
            // activation. Admit its earlier physical placement as a separate
            // opposing commitment, without changing the original capper bids.
            // The displaced response is repriced on that alternative branch.
            if(board.beforeConditionalDuty.size()==board.rights.size() && board.conditionalClaims &&
               board.conditionalClaims->contains(i)){
                const auto &prior=board.beforeConditionalDuty[i];
                if(prior.readyTick>=0){
                    const int starts=std::max(2+side,board.positions[i]==property.tile?prior.readyTick:
                        TownAssaultDetail::firstMoveTick(prior,side));
                    const int arrives=earliest(grid,board.positions[i],pawn.movement,prior.fuel,starts,floor);
                    const int retained=right.readyTick<0?last+1:std::max(2+side,board.positions[i]==property.tile?
                        right.readyTick:TownAssaultDetail::firstMoveTick(right,side));
                    const int retainedArrival=right.readyTick<0?last+1:
                        earliest(grid,board.positions[i],pawn.movement,right.fuel,retained,floor);
                    if(starts<retained && arrives<retainedArrival && arrives<=prior.lastFreeTick && arrives<=last){
                        possible[side]=std::min(possible[side],arrives);
                        entrants.push_back({i,side,starts,starts,arrives,pawn.movement,prior.fuel,board.positions[i],
                            &grid,pawn.ignoresCollision,false,true});
                    }
                }
            }
            if(right.readyTick<0)continue;
            const int ready=std::max(2+side,board.positions[i]==property.tile ? right.readyTick : TownAssaultDetail::firstMoveTick(right,side));
            const int first=earliest(grid,board.positions[i],pawn.movement,right.fuel,ready,floor);
            if(first>right.lastFreeTick) continue;
            possible[side]=std::min(possible[side],first);
            if(first<=last)entrants.push_back({i,side,ready,ready,first,pawn.movement,right.fuel,board.positions[i],&grid,pawn.ignoresCollision});
            if(!unit.canCapture) continue;
            const int rate=replyCaptureRate(unit.captureRate,unit.hpRounded-ceilHpSteps(board.hp[i]),false);
            if(rate<=0 || first+2*(widenedCaptureTurns(0,rate,board.threshold)-1)>std::min(right.lastFreeTick,last)) continue;
            if((requiredCapper==NO_UNIT || pawn.engineUnitId==requiredCapper) &&
               (includeUnclaimed || std::find(previous.begin(),previous.end(),i)==previous.end())) newBid[side]=std::min(newBid[side],first);
        }
        // A real recruit can occupy a neutral tile as soon as its dated
        // duty lets it act, even if it cannot capture itself. Its opportunity may limit either
        // side; a phantom purchase does not become a competing capper.
        for(int i=0;i<static_cast<int>(board.recruits.size());++i)
        {
            const auto &g=board.recruits[i];
            if(!g.unit || !g.unit->movementGrid) continue;
            int ready=g.buildTick+2;
            while(ready<=last && board.recruitFree && !board.recruitFree(i,ready,last)) ready+=2;
            if(ready>last) continue;
            const auto &grid=*g.unit->movementGrid;
            const int fromBirth=earliest(grid,g.factory,g.unit->movement,g.unit->initialFuel,g.buildTick+2,g.unit->minimumMoveCost);
            if(fromBirth>last)continue;
            const auto origin=origins?origins->get(i,ready):paidNeutralOrigin(board.traffic,n+i,g,ready);
            const int first=origin?earliest(grid,origin->from,g.unit->movement,origin->fuel,origin->departureTick,g.unit->minimumMoveCost):
                std::max(ready,fromBirth);
            possible[g.side]=std::min(possible[g.side],first);
            if(first<=last)entrants.push_back({n+i,g.side,ready,ready,first,g.unit->movement,
                origin?origin->fuel:0,origin?origin->from:INVALID_TILE,&grid,false,!origin});
        }
        if(firstBids)*firstBids=newBid;
        if(firstOccupants)*firstOccupants=possible;
        for(int side=0;side<2;++side)
        {
            if(vacantOwned && property.ownerBefore==(side?OwnerSign::Enemy:OwnerSign::Ours))continue;
            const int end=std::min(last,possible[1-side]-1);
            const int first=std::max(newBid[side],afterTitle+int(afterTitle%2!=side));
            const int entry=desiredTick<0?first:desiredTick;
            if(first>entry || entry>last || entry%2!=side)continue;
            const auto offerWindow=[&](int from,int through,bool contested) {
                if(from>through)return;
                NeutralCaptureOffer offer{{property,1-side,NO_UNIT,from,through},from,{previous.begin(),previous.end()},{}};
                if(contested)for(const auto &r:entrants)
                    if(r.side!=side && r.earliest<from)offer.potentialRivals.push_back(r);
                result.push_back(std::move(offer));
            };
            if(desiredTick>=0)offerWindow(entry,entry,entry>end);
            else
            {
                // A lower bound rules out earlier rival arrivals. From that
                // date onward each still-legal start needs a dated challenge.
                // Even this side's best terrain route may open several turns later.
                offerWindow(entry,std::min(last,end),false);
                int next=std::max(entry,end+1);next+=int(next%2!=side);
                for(int tick=next;tick<=last;tick+=2)offerWindow(tick,tick,true);
            }
        }
        return result;
    }
}
