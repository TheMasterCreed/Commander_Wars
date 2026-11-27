#pragma once

#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/supportcounterattack.h"

namespace Coordinator
{
    struct CaptureDefenderAvailability
    {
        int firstFreeTick{-1}, remainingFuel{};
        bool mayRelocate{};
        MilliFunds alternativeValue{};
    };

    struct FundedFiringMove
    {
        TilePoint from{INVALID_TILE};
        std::vector<int> forward,back;
        int fuel{};
    };

    struct CaptureGunMotion
    {
        TilePoint origin{INVALID_TILE};
        int fuel{};
        std::vector<int> cells;
    };

    struct CaptureRouteShot
    {
        int gun{}, shotTick{}, weapon{-1}, counterWeapon{-1};
        TilePoint from{INVALID_TILE};
        double targetBefore{}, targetAfter{}, shooterAfter{};
        MilliFunds damageValue{}, counterValue{};
        bool alreadyPriced{};
        FundedFiringMove deployment;
        CaptureGunMotion motion;
    };

    struct CaptureSurvivalState
    {
        double hp{};
        int carry{}, completedTick{-1};
        std::vector<CaptureRouteShot> shots;
    };

    struct CaptureFiringOrigins
    {
        std::vector<int> cost, parent;
    };

    struct CaptureFiringFrontier
    {
        using Step=std::pair<int,int>;
        CaptureFiringOrigins routes;
        int activeCell{-1},activeCost{},nextDirection{};
        std::priority_queue<Step,std::vector<Step>,std::greater<Step>> queue;
    };

    namespace CaptureSurvivalDetail
    {
        struct Gun
        {
            int index{}, weapon{}, counterWeapon{-1};
            TilePoint from{INVALID_TILE};
            double attack{}, counter{}, hp{};
            TerrainDefenseRule cover;
            MilliFunds cost{}, alternative{};
            bool paid{};
            FundedFiringMove deployment;
            CaptureGunMotion motion;
            std::optional<std::array<double,2>> counterSlots;
        };
    }

    struct CaptureRouteDefense
    {
        const CombatTerrain &terrain;
        std::span<const KnownUnit> units;
        std::span<const TilePoint> positions;
        std::span<const double> hp;
        std::span<const std::array<int,2>> load;
        std::span<const CaptureDefenderAvailability> availability;
        std::span<const FundedCaptureGun> funded;
        // Additional agreed traffic on this dated board. A gun ID above the
        // stock-unit range denotes a funded unit. The caller retains its job.
        std::function<bool(int,int,TilePoint,bool)> traffic;
        std::function<bool(int,int)> available;
        mutable std::map<int,int> minimumEntryCost;
        // Marginal value of retiming this particular transfer after this hit.
        std::function<MilliFunds(int,TilePoint,int,int,double,double)> avoidedCaptureIncome;
        // Only an immutable dated calendar may share a range search. Changing
        // any traffic reservation advances this revision before another query.
        std::function<std::uint64_t()> geometryRevision;
        using FireKey=std::tuple<int,int,int,int,int>; // gun, tick, origin, allowance, extra blocked cell
        mutable std::map<FireKey,CaptureFiringOrigins> firingOrigins;
        mutable std::optional<std::uint64_t> cachedRevision;
        using StopKey=std::tuple<int,int,int,bool,double,int,int,int,int>;
        mutable std::map<StopKey,CaptureSurvivalState> openingStops;
        bool memoizeStops{};
        std::function<bool(int,int,TilePoint)> canLeavePost;
        const std::vector<MobilityCostGrid> *movementGrids{};
        std::map<int,int> *sharedMinimumEntryCost{};
        mutable std::map<std::tuple<const MobilityCostGrid*,int,int,int>,std::vector<ReinforcementRoute>> mobileDeployments;
        std::function<bool(int,int)> canLoseGun;
        // A branch may update its own calendar after a real shot moves the
        // shooter or kills it in counterfire. Only then reprice remaining guns.
        std::function<bool(const CaptureRouteShot&)> recordShot;
        std::function<std::optional<std::vector<CaptureSurvivalDetail::Gun>>(int,TilePoint,
            const CaptureSurvivalState&,std::int64_t&)> paidResponders;
        // Set only by a single-shooter existence check on an immutable dated stop.
        int singleOpeningGun{NO_UNIT};
        std::map<int,CaptureFiringFrontier> *singleGunFields{};
        int *singleGunExpansions{};
        bool *singleGunScanned{};
        int *singleGunWork{};
        void beginTrip()
        {
            openingStops.clear();
            memoizeStops=true;
        }
    };

    namespace CaptureSurvivalDetail
    {
        inline std::optional<FundedFiringMove> fundedFiringMove(const CaptureRouteDefense &board,int gun,int tick,TilePoint target,
            const FundedCaptureGun &paid,std::int64_t &budget)
        {
            FundedFiringMove none;
            const auto &unit=*paid.unit;
            if(!unit.mobileFire || !unit.movementGrid || unit.movement<=0 || tick<paid.readyTick+2 ||
               !board.canLeavePost || !board.canLeavePost(gun,tick,paid.destination)) return none;
            const int distance=manhattanDistance(target,paid.destination);
            if(static_cast<std::int64_t>(std::max(0,distance-unit.maxRange))*unit.minimumMoveCost>unit.movement) return none;
            const auto &grid=*unit.movementGrid;const int width=grid.width();
            auto step=[&](int index){return TilePoint{index%width,index/width};};
            const int start=paid.destination.y*width+paid.destination.x;
            const int fuel=paidFuelAtRelease(paid,tick);
            const auto key=std::tuple{&grid,start,unit.movement,fuel};
            auto found=board.mobileDeployments.find(key);
            if(found==board.mobileDeployments.end())
            {
                const auto work=static_cast<std::int64_t>(width)*grid.height();
                if(budget<work) return {};
                budget-=work;
                found=board.mobileDeployments.emplace(key,reinforcementRoutes(grid,paid.destination,unit.movement,1,
                    fuel<0?std::numeric_limits<int>::max():fuel)).first;
            }
            for(const auto &route:found->second)
            {
                const auto at=route.destination;const int range=manhattanDistance(at,target);
                if(range<unit.minRange || range>unit.maxRange || at==paid.destination || at==target) continue;
                if(board.traffic && (!board.traffic(gun,tick,at,false) || !board.traffic(gun,tick+1,at,false) ||
                    !board.traffic(gun,tick+2,paid.destination,false))) continue;
                if(budget<=2*static_cast<std::int64_t>(route.cells.size())) return {};
                budget-=2*static_cast<std::int64_t>(route.cells.size())+1;
                FundedFiringMove move{at};int forward=0,back=0;
                auto previous=paid.destination;
                for(auto it=route.cells.rbegin();it!=route.cells.rend();++it)
                {
                    const auto next=step(*it);
                    if(next==target || (board.traffic && !board.traffic(gun,tick,next,true))) {forward=unit.movement+1;break;}
                    const int cost=grid.cost(next.x,next.y,entryDirection(previous.x,previous.y,next.x,next.y));
                    if(cost<0) {forward=unit.movement+1;break;}
                    forward+=cost;move.forward.push_back(*it);previous=next;
                }
                if(forward>unit.movement) continue;
                for(int j=static_cast<int>(move.forward.size())-2;j>=-1;--j)
                {
                    const int sq=j<0? start : move.forward[j];const auto next=step(sq);
                    if(next==target || (board.traffic && !board.traffic(gun,tick+2,next,true))) {back=unit.movement+1;break;}
                    const int cost=grid.cost(next.x,next.y,entryDirection(previous.x,previous.y,next.x,next.y));
                    if(cost<0) {back=unit.movement+1;break;}
                    back+=cost;move.back.push_back(sq);previous=next;
                }
                if(back<=unit.movement && (fuel<0 || forward+back<=fuel))
                {move.fuel=forward+back;return move;}
            }
            return none;
        }

        inline bool occupiesAfterShot(const CaptureRouteShot &shot,const CaptureSurvivalState &state,int tick)
        {
            if(shot.shotTick==tick) return shot.shooterAfter>0;
            if(shot.shotTick>tick || shot.shooterAfter<=0 || (shot.deployment.from!=INVALID_TILE && tick>=shot.shotTick+2)) return false;
            return std::none_of(state.shots.begin(),state.shots.end(),[&](const auto &next) {
                return next.gun==shot.gun && next.shotTick>shot.shotTick && next.shotTick<=tick && next.from!=shot.from;
            });
        }

        inline std::optional<Gun> stockGun(const CaptureRouteDefense &board, int gun, int actor,
            int tick, TilePoint target, const CaptureSurvivalState &state, std::int64_t &budget)
        {
            const auto &terrain=board.terrain;
            const auto &pawn=terrain.units[gun];
            const auto &unit=board.units[gun];
            const auto &rights=board.availability[gun];
            auto origin=board.positions[gun];
            auto health=board.hp[gun];
            auto rounds=board.load[gun];
            int fuel=rights.remainingFuel;
            for (const auto &prior:state.shots)
                if (prior.gun==gun)
                {
                    origin=prior.from;
                    if(fuel>=0) fuel=std::max(0,fuel-prior.motion.fuel);
                    health=prior.shooterAfter;
                    spendCombatRound(rounds,prior.weapon);
                }
            Gun none{NO_UNIT};
            if (pawn.enemy==terrain.units[actor].enemy || rights.firstFreeTick<0 || rights.firstFreeTick>tick ||
                origin==INVALID_TILE || health<=0 || !(unit.hasWeapons || unit.canFire) ||
                (rounds[0]==0 && rounds[1]==0)) return none;
            const int move=rights.mayRelocate && unit.canMoveAndFire && pawn.grid>=0 ?
                (fuel<0 ? pawn.movement : std::min(pawn.movement,fuel)) : 0;
            const auto weaponAt=[&](TilePoint from) {
                const int d=manhattanDistance(from,target);
                return d>=unit.minRange && d<=unit.maxRange && d>0 ?
                    supportWeapon(terrain,gun,actor,d,rounds) : LoadedCombatWeapon{};
            };
            const auto legal=[&](TilePoint tile,bool transit) {
                if (transit && pawn.ignoresCollision) return true;
                if (tile==target || (board.traffic && !board.traffic(gun,tick,tile,transit))) return false;
                for (const auto &shot:state.shots)
                    if (shot.gun!=gun && shot.from==tile && occupiesAfterShot(shot,state,tick) && !transit) return false;
                // Dated reservations already release a unit's old square when
                // it leaves. A second timeless occupancy check would close it again.
                if (board.traffic) return true;
                for (int i=0;i<static_cast<int>(board.positions.size());++i)
                    if (i!=gun && i!=actor && board.hp[i]>0 && board.positions[i]==tile &&
                        (!transit || (!pawn.ignoresCollision && terrain.units[i].enemy!=pawn.enemy))) return false;
                return true;
            };
            const auto offer=[&](TilePoint from,LoadedCombatWeapon shot) {
                const int d=manhattanDistance(from,target);
                const auto back=d==1 && board.units[actor].minRange<=1 && board.units[actor].maxRange>=1 ?
                    supportWeapon(terrain,actor,gun,d,board.load[actor]) : LoadedCombatWeapon{};
                return Gun{gun,shot.slot,back.slot,from,shot.base,back.base,health,terrain.defense(gun,from),
                           pawn.replacementCost,rights.alternativeValue,false};
            };
            const auto atOrigin=weaponAt(origin);
            if (atOrigin.base>0 && legal(origin,false)) return offer(origin,atOrigin);
            if (move<=0) return none;
            const auto &grid=board.movementGrids ? board.movementGrids->at(pawn.grid) : terrain.grids[pawn.grid];
            const auto shared=terrain.movementCache;
            int minimum=0;
            if(shared && shared->minimumEntryCost.contains(&grid)) minimum=shared->minimumEntryCost.at(&grid);
            else
            {
                auto &minima=board.sharedMinimumEntryCost ? *board.sharedMinimumEntryCost : board.minimumEntryCost;
                auto lowest=minima.find(pawn.grid);
                // Zero is a safe lower bound. A fragmented guard query
                // can resume the actual walk instead of requiring an upfront
                // scan of every tile merely to reject distant firing squares.
                if(lowest==minima.end() && board.singleOpeningGun==NO_UNIT)
                {
                    const auto work=static_cast<std::int64_t>(grid.width())*grid.height()*ENTRY_DIRECTION_COUNT;
                    if(work>static_cast<std::uint64_t>(budget)) return {};
                    budget-=work;
                    lowest=minima.emplace(pawn.grid,minimumTerrainEntryCost(grid)).first;
                }
                if(lowest!=minima.end())minimum=lowest->second;
            }
            if(std::int64_t(std::max(0,manhattanDistance(origin,target)-unit.maxRange))*minimum>move) return none;
            bool hasTarget=false;
            for (int d=std::max(1,unit.minRange);d<=std::min(unit.maxRange,terrain.width+terrain.height);++d)
                if (supportWeapon(terrain,gun,actor,d,rounds).base>0) {hasTarget=true; break;}
            if (!hasTarget) return none;
            using Step=std::pair<int,int>;
            const auto cell=[&](TilePoint p){return p.y*terrain.width+p.x;};
            // The same dated gun often answers several alternate stops. Build
            // its movement field once. Endpoints and weapons are checked anew;
            // the capper's body may also cut an otherwise legal movement path.
            if (board.traffic && board.geometryRevision)
            {
                const auto epoch=board.geometryRevision();
                if (board.cachedRevision!=epoch)
                {
                    board.firingOrigins.clear();
                    board.openingStops.clear();
                    board.cachedRevision=epoch;
                }
                auto field=[&](int denied)->const CaptureFiringOrigins* {
                    const CaptureRouteDefense::FireKey key{gun,tick,cell(origin),move,denied};
                    if (auto hit=board.firingOrigins.find(key);hit!=board.firingOrigins.end()) return &hit->second;
                    CaptureFiringFrontier temporary;
                    auto *const fields=board.singleGunFields;
                    auto &search=fields?(*fields)[denied]:temporary;
                    auto &f=search.routes;
                    auto &queue=search.queue;
                    if(f.cost.empty())
                    {
                        f.cost.assign(terrain.width*terrain.height,-1);
                        f.parent.assign(f.cost.size(),-1);
                        f.cost[cell(origin)]=0;
                        queue.emplace(0,cell(origin));
                    }
                    while (search.activeCell>=0 || !queue.empty())
                    {
                        if(search.activeCell<0)
                        {
                            const auto [cost,square]=queue.top();
                            if(f.cost[square]!=cost){queue.pop();continue;}
                            const int startCost=fields?1:1+ENTRY_DIRECTION_COUNT;
                            if(budget<startCost)return nullptr;
                            budget-=startCost;
                            if(board.singleGunWork)++*board.singleGunWork;
                            queue.pop();
                            search.activeCell=square;search.activeCost=cost;search.nextDirection=0;
                        }
                        const int sq=search.activeCell,spent=search.activeCost;
                        const TilePoint from{sq%terrain.width,sq/terrain.width};
                        while(search.nextDirection<ENTRY_DIRECTION_COUNT)
                        {
                            if(fields)
                            {
                                if(budget<1)return nullptr;
                                --budget;
                                if(board.singleGunWork)++*board.singleGunWork;
                            }
                            const auto dir=ENTRY_DIRECTIONS[search.nextDirection++];
                            const auto d=entryStep(dir);
                            const TilePoint next{from.x+d.dx,from.y+d.dy};
                            const int cost=grid.cost(next.x,next.y,dir);
                            if (cost<0 || cost>move-spent) continue;
                            const int nd=cell(next);
                            if (f.cost[nd]>=0 && f.cost[nd]<=spent+cost) continue;
                            if (!pawn.ignoresCollision && (nd==denied || !board.traffic(gun,tick,next,true))) continue;
                            f.cost[nd]=spent+cost;
                            f.parent[nd]=sq;
                            queue.emplace(spent+cost,nd);
                        }
                        search.activeCell=-1;
                        if(board.singleGunExpansions)++*board.singleGunExpansions;
                    }
                    if(fields)return &fields->at(denied).routes;
                    if (board.firingOrigins.size()>=CombatMovementCache::maxEntries) board.firingOrigins.clear();
                    return &board.firingOrigins.emplace(key,std::move(f)).first->second;
                };
                const auto bestEnd=[&](const CaptureFiringOrigins &f) {
                    Step best{std::numeric_limits<int>::max(),-1};
                    const int radius=std::min(unit.maxRange,terrain.width+terrain.height);
                    const int minR=std::max(1,unit.minRange);
                    const int yMin=std::max(0,target.y-radius),yMax=std::min(terrain.height-1,target.y+radius);
                    for (int y=yMin;y<=yMax;++y)
                    {
                        const int dy=std::abs(y-target.y);
                        const int dxMax=radius-dy;
                        const int xMin=std::max(0,target.x-dxMax),xMax=std::min(terrain.width-1,target.x+dxMax);
                        const int rowOffset=y*terrain.width;
                        for (int x=xMin;x<=xMax;++x)
                        {
                            if (dy+std::abs(x-target.x)<minR) continue;
                            const int sq=rowOffset+x;
                            const int c=f.cost[sq];
                            if (c<0 || (x==origin.x && y==origin.y) || Step{c,sq}>=best) continue;
                            const TilePoint end{x,y};
                            if (weaponAt(end).base>0 && legal(end,false)) best={c,sq};
                        }
                    }
                    return best.second;
                };
                const auto *f=field(-1);
                if (!f) return {};
                int end=bestEnd(*f);
                if (!pawn.ignoresCollision)
                    for (int sq=end;sq>=0;sq=f->parent[sq])
                        if (sq==cell(target))
                        {
                            f=field(sq);
                            if (!f) return {};
                            end=bestEnd(*f);
                            break;
                        }
                if (end<0) return none;
                const TilePoint where{end%terrain.width,end/terrain.width};
                auto result=offer(where,weaponAt(where));
                result.motion={origin,f->cost[end],{}};
                for(int sq=end;sq!=cell(origin);sq=f->parent[sq]) result.motion.cells.push_back(sq);
                std::reverse(result.motion.cells.begin(),result.motion.cells.end());
                return result;
            }
            struct LocalDijkstraScratch
            {
                std::vector<int> cost,parent;
                std::vector<std::uint32_t> stamp;
                std::vector<Step> heap;
                std::uint32_t gen{0};
                bool active{false};
            };
            static thread_local LocalDijkstraScratch scratch;
            LocalDijkstraScratch fallback;
            const bool useScratch=!scratch.active;
            if(useScratch) scratch.active=true;
            struct ReleaseScratch { LocalDijkstraScratch &s; bool owned; ~ReleaseScratch(){ if(owned) s.active=false; } } release{scratch,useScratch};
            auto &st=useScratch?scratch:fallback;
            const std::size_t mapCells=static_cast<std::size_t>(terrain.width)*terrain.height;
            if(st.cost.size()<mapCells)
            {
                st.cost.resize(mapCells);
                st.parent.resize(mapCells);
                st.stamp.assign(mapCells,0);
            }
            if(++st.gen==0)
            {
                std::fill(st.stamp.begin(),st.stamp.end(),0);
                st.gen=1;
            }
            st.heap.clear();
            const int originCell=cell(origin);
            st.stamp[originCell]=st.gen;
            st.cost[originCell]=0;
            st.heap.emplace_back(0,originCell);
            const auto cmp=std::greater<Step>{};
            constexpr int FALLBACK_STEP_WORK=5;
            while (!st.heap.empty())
            {
                if (budget<FALLBACK_STEP_WORK) return {};
                budget-=FALLBACK_STEP_WORK;
                std::pop_heap(st.heap.begin(),st.heap.end(),cmp);
                const auto [spent,at]=st.heap.back();
                st.heap.pop_back();
                if (st.cost[at]!=spent) continue;
                const TilePoint from{at%terrain.width,at/terrain.width};
                if (from!=origin && legal(from,false))
                {
                    const auto shot=weaponAt(from);
                    if (shot.base>0)
                    {
                        auto result=offer(from,shot);result.motion={origin,spent,{}};
                        for(int sq=at;sq!=originCell;sq=st.parent[sq]) result.motion.cells.push_back(sq);
                        std::reverse(result.motion.cells.begin(),result.motion.cells.end());
                        return result;
                    }
                }
                for (auto dir:ENTRY_DIRECTIONS)
                {
                    const auto step=entryStep(dir);
                    const TilePoint next{from.x+step.dx,from.y+step.dy};
                    const int cost=grid.cost(next.x,next.y,dir);
                    if (cost<0 || cost>move-spent || !legal(next,true)) continue;
                    const auto sq=cell(next);
                    const int nextSpent=spent+cost;
                    if (st.stamp[sq]==st.gen && st.cost[sq]<=nextSpent) continue;
                    st.stamp[sq]=st.gen;
                    st.cost[sq]=nextSpent;
                    st.parent[sq]=at;
                    st.heap.emplace_back(nextSpent,sq);
                    std::push_heap(st.heap.begin(),st.heap.end(),cmp);
                }
            }
            return none;
        }
    }

    // A capper moves and captures on its own activation. An enemy shot is legal
    // only on the following half-turn, before the capper's next activation.
    // The caller commits these shots only if it accepts the trip. False means
    // the work budget ran out, never proof of death or of safe passage.
    inline bool quoteCaptureSurvival(const CaptureRouteDefense &board,CaptureSurvivalState &state,
        int actor,TilePoint tile,int ownTick,bool captures,int threshold,int baseRate,
        int originalRoundedHp,std::int64_t &budget)
    {
        using namespace CaptureSurvivalDetail;
        const auto n=board.units.size();
        if (actor<0 || actor>=static_cast<int>(n) || board.terrain.units.size()!=n || board.positions.size()!=n ||
            board.hp.size()!=n || board.load.size()!=n || board.availability.size()!=n || threshold<=0 ||
            ownTick<0 || (ownTick%2)!=int(board.terrain.units[actor].enemy)) return false;
        if (state.hp<=0 || state.completedTick>=0) return true;
        if (captures)
        {
            const int rate=replyCaptureRate(baseRate,originalRoundedHp-ceilHpSteps(state.hp),false);
            state.carry+=std::min(std::max(0,threshold-state.carry),std::max(0,rate));
            if (state.carry>=threshold) {state.completedTick=ownTick; return true;}
        }
        else state.carry=0;
        const auto &terrain=board.terrain;
        const int tick=ownTick+1;
        const int stockCount=static_cast<int>(n);
        const int fundedCount=static_cast<int>(board.funded.size());
        const int single=board.singleOpeningGun;
        if(single!=NO_UNIT && (single<0 || single>=stockCount+fundedCount || board.paidResponders ||
           board.recordShot || !state.shots.empty() || captures || board.memoizeStops))return false;
        const std::int64_t scan=single==NO_UNIT?std::int64_t(stockCount)+fundedCount:1;
        if(!board.singleGunScanned || !*board.singleGunScanned)
        {
            if(budget<scan)return false;
            budget-=scan;
            if(board.singleGunScanned)*board.singleGunScanned=true;
            if(board.singleGunWork)++*board.singleGunWork;
        }
        std::vector<Gun> shots;
        const auto free=[&](int gun) {
            return (!board.available || board.available(gun,tick)) &&
                std::none_of(state.shots.begin(),state.shots.end(),[&](const auto &s){return s.gun==gun && s.shotTick==tick;});
        };
        const int firstStock=single==NO_UNIT?0:std::min(stockCount,single);
        const int lastStock=single==NO_UNIT?stockCount:std::min(stockCount,single+1);
        for (int gun=firstStock;gun<lastStock;++gun)
        {
            if (!free(gun)) continue;
            if (!terrain.defense || (!terrain.weapon && !terrain.weaponSlots))
            {
                const auto &r=board.availability[gun];
                if (terrain.units[gun].enemy!=terrain.units[actor].enemy && r.firstFreeTick>=0 && r.firstFreeTick<=tick &&
                    board.positions[gun]!=INVALID_TILE && board.hp[gun]>0 && (board.units[gun].hasWeapons || board.units[gun].canFire) &&
                    (board.load[gun][0]!=0 || board.load[gun][1]!=0)) return false;
                continue;
            }
            const auto shot=stockGun(board,gun,actor,tick,tile,state,budget);
            if (!shot) return false;
            if (shot->index!=NO_UNIT) shots.push_back(*shot);
        }
        if(board.paidResponders)
        {
            auto extra=board.paidResponders(tick,tile,state,budget);
            if(!extra)return false;
            shots.insert(shots.end(),std::make_move_iterator(extra->begin()),std::make_move_iterator(extra->end()));
        }
        const int firstPaid=single==NO_UNIT?0:std::max(0,single-stockCount);
        const int lastPaid=single==NO_UNIT?fundedCount:std::clamp(single-stockCount+1,0,fundedCount);
        for (int n=firstPaid;!board.paidResponders && n<lastPaid;++n)
        {
            const auto &gun=board.funded[n];
            const int i=static_cast<int>(board.units.size())+n;
            if (!gun.unit || gun.side==int(terrain.units[actor].enemy) || tick<gun.readyTick ||
                tick<=gun.buildTick || !free(i)) continue;
            const auto &u=*gun.unit;
            const bool booked=tick==gun.impactTick && gun.targetUnit==actor && gun.targetTile==tile;
            const int extra=std::count_if(state.shots.begin(),state.shots.end(),[&](const auto &s){return s.gun==i && !s.alreadyPriced;});
            const int reservedRound=gun.targetUnit==NO_UNIT ? 0 : 1;
            if ((!booked && (tick==gun.impactTick || u.guaranteedShots<=reservedRound+extra)) ||
                (board.traffic && !board.traffic(i,tick,gun.destination,false))) continue;
            TilePoint firing=gun.destination;
            FundedFiringMove redeployment;
            int distance=manhattanDistance(firing,tile);
            if(u.cost<=0 || gun.exposure>=u.cost) continue;
            if(distance<u.minRange || distance>u.maxRange || distance==0)
            {
                if(booked || std::any_of(state.shots.begin(),state.shots.end(),[&](const auto &s){
                    return s.gun==i && s.deployment.from!=INVALID_TILE;})) continue;
                auto movement=fundedFiringMove(board,i,tick,tile,gun,budget);
                if(!movement) return false;
                if(movement->from==INVALID_TILE) continue;
                redeployment=std::move(*movement);firing=redeployment.from;distance=manhattanDistance(firing,tile);
            }
            if (actor>=static_cast<int>(u.attack.size())) return false;
            if (u.attack[actor]<=0) continue;
            if (!terrain.defense || gun.destination.x<0 || gun.destination.x>=terrain.width ||
                gun.destination.y<0 || gun.destination.y>=terrain.height) return false;
            double firingHp=paidHpAfterService(gun,tick,u.startingHp-static_cast<int>((std::max<MilliFunds>(0,gun.exposure)*10+u.cost-1)/u.cost));
            if(gun.observedService)
            {
                if(tick<gun.observedService->nextReady)continue;
                const auto rounds=paidLoadAfterService(gun,tick,u.captureLoad);
                if(actor>=static_cast<int>(u.captureCounter.size()) || usableOpeningWeapon(u.captureCounter[actor],rounds)<0)continue;
            }
            for (const auto &prior:state.shots)
                if (prior.gun==i) firingHp=std::min(firingHp,prior.shooterAfter);
            if (firingHp<=0) continue;
            const auto square=firing.y*terrain.width+firing.x;
            if (square>=static_cast<int>(u.routeAt.size())) return false;
            const int route=u.routeAt[square];
            if (route<0 || route>=static_cast<int>(u.defense.size())) return false;
            const bool counters=distance==1 && board.units[actor].minRange<=1 && board.units[actor].maxRange>=1;
            if (counters && actor>=static_cast<int>(u.incoming.size())) return false;
            const double back=counters ? u.incoming[actor] : -1.;
            if (back>0 && (actor>=static_cast<int>(u.incomingSlot.size()) || u.incomingSlot[actor]<0 || u.incomingSlot[actor]>1)) return false;
            shots.push_back({i,-1,back>0 ? u.incomingSlot[actor] : -1,firing,u.attack[actor],back,
                             double(firingHp),u.defense[route],u.cost,0,booked,std::move(redeployment)});
        }
        if (shots.empty()) return true;
        const auto cover=terrain.defense(actor,tile);
        auto counters=board.load[actor];
        for (const auto &prior:state.shots) spendCombatRound(counters,prior.counterWeapon);
        while (!shots.empty() && state.hp>0)
        {
            std::optional<CaptureRouteShot> best;
            MilliFunds bestGain{};
            for (const auto &s:shots)
            {
                if (std::any_of(state.shots.begin(),state.shots.end(),[&](const auto &p) {
                    return p.gun!=s.index && p.from==s.from && occupiesAfterShot(p,state,tick);
                })) continue;
                const auto rawPrimary=terrainReducedDamage(cover,s.attack,ceilHpSteps(s.hp),ceilHpSteps(state.hp));
                const auto hit=engineShotDamage(rawPrimary);
                const double after=engineHpAfterReply(state.hp,hit);
                if (after>=state.hp) continue;
                auto counter=LoadedCombatWeapon{s.counter,s.counterWeapon};
                if(s.counterSlots)
                {
                    counter={};
                    for(int i=0;i<2;++i)if(counters[i]!=0 && (*s.counterSlots)[i]>counter.base)
                        counter={(*s.counterSlots)[i],i};
                }
                if (after>0 && s.index<static_cast<int>(n) && manhattanDistance(s.from,tile)==1 &&
                    board.units[actor].minRange<=1 && board.units[actor].maxRange>=1)
                    counter=supportWeapon(terrain,actor,s.index,1,counters);
                if (after>0 && s.index>=static_cast<int>(n) && counter.base>0 && counters[counter.slot]==0 &&
                    counters[1-counter.slot]!=0) return false;
                const bool canCounter=after>0 && counter.base>0 && counter.slot>=0 && counters[counter.slot]!=0;
                const auto back=canCounter ? engineShotDamage(sameActionCounterDamage(s.cover,counter.base,state.hp,rawPrimary,ceilHpSteps(s.hp))) : 0.;
                const double surviving=engineHpAfterReply(s.hp,back);
                if(surviving<=0 && board.canLoseGun && !board.canLoseGun(s.index,tick)) continue;
                // An earlier spare round cannot weaken the shooter whose later
                // firing task still has a prior claim on this unit's strength.
                if(!board.paidResponders && s.index>=static_cast<int>(n) && tick<board.funded[s.index-n].impactTick && surviving<s.hp) continue;
                if(!board.paidResponders && s.deployment.from!=INVALID_TILE && surviving<=0) continue;
                const auto hurt=combatCapitalLoss(terrain.units[actor].replacementCost,state.hp,after);
                const auto loss=combatCapitalLoss(s.cost,s.hp,surviving);
                const auto income=board.avoidedCaptureIncome ?
                    board.avoidedCaptureIncome(actor,tile,ownTick,state.carry,state.hp,after) : MilliFunds{};
                // Compare the marginal swing once for this hit. Earlier damage
                // already changed both the HP and finish date of the next bid.
                const auto gain=hurt-loss-s.alternative+income;
                if (!s.paid && gain<=0) continue;
                if (best && std::tuple{s.paid,gain,-s.index}<=std::tuple{best->alreadyPriced,bestGain,-best->gun}) continue;
                bestGain=gain;
                best=CaptureRouteShot{s.index,tick,s.weapon,canCounter ? counter.slot : -1,s.from,
                                      state.hp,after,surviving,hurt,loss,s.paid,s.deployment,s.motion};
            }
            if (!best) break;
            state.hp=best->targetAfter;
            spendCombatRound(counters,best->counterWeapon);
            state.shots.push_back(*best);
            std::erase_if(shots,[&](const auto &s){return s.index==best->gun;});
            const bool changed=board.recordShot && board.recordShot(*best);
            if(changed && state.hp>0)
            {
                std::erase_if(shots,[&](const auto &s){return s.index<static_cast<int>(n);});
                for(int gun=0;gun<static_cast<int>(n);++gun)
                {
                    if(!free(gun))continue;
                    const auto next=stockGun(board,gun,actor,tick,tile,state,budget);
                    if(!next)return false;
                    if(next->index!=NO_UNIT)shots.push_back(*next);
                }
                continue;
            }
            if (state.hp>0)
                for (auto &s:shots)
                    if (s.index>=0 && s.index<static_cast<int>(n) && s.from==best->from)
                    {
                        const auto alternate=stockGun(board,s.index,actor,tick,tile,state,budget);
                        if (!alternate) return false;
                        s=*alternate;
                    }
            std::erase_if(shots,[](const auto &s){return s.index==NO_UNIT;});
        }
        return true;
    }

    inline bool advanceCaptureSurvival(const CaptureRouteDefense &board,CaptureSurvivalState &state,
        int actor,TilePoint tile,int ownTick,bool captures,int threshold,int baseRate,int originalRoundedHp,
        std::int64_t &budget)
    {
        if (!board.memoizeStops || !board.geometryRevision || !state.shots.empty() || state.completedTick>=0 || state.hp<=0)
            return quoteCaptureSurvival(board,state,actor,tile,ownTick,captures,threshold,baseRate,originalRoundedHp,budget);
        const auto epoch=board.geometryRevision();
        if (board.cachedRevision!=epoch)
        {
            board.firingOrigins.clear();
            board.openingStops.clear();
            board.cachedRevision=epoch;
        }
        const CaptureRouteDefense::StopKey key{actor,ownTick,tile.y*board.terrain.width+tile.x,captures,state.hp,state.carry,
                                              threshold,baseRate,originalRoundedHp};
        if (auto it=board.openingStops.find(key);it!=board.openingStops.end())
        {
            if (budget<1) return false;
            --budget;
            state=it->second;
            return true;
        }
        if (!quoteCaptureSurvival(board,state,actor,tile,ownTick,captures,threshold,baseRate,originalRoundedHp,budget)) return false;
        if (board.openingStops.size()<CombatMovementCache::maxEntries) board.openingStops.emplace(key,state);
        return true;
    }
}
