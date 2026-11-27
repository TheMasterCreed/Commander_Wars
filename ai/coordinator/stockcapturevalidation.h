#pragma once

#include "ai/coordinator/paidcaptureroute.h"
#include "ai/coordinator/woundedcapturewindow.h"
#include "ai/coordinator/stockentrycut.h"
#include "ai/coordinator/townconditionalgun.h"

namespace Coordinator
{
    struct StockCaptureTrip
    {
        int actor,side;
        TilePoint from,target;
        int departure,entry,completion,rate,carry;
        PropertyStockColumn property;
        // First activation on the retained board, when the caller can establish it.
        int retainedReadyTick{-1};
    };

    struct StockCaptureRelease
    {
        TilePoint tile{INVALID_TILE};
        int readyTick{-1},fuel{};
        double hp{};
        std::array<int,2> ammo{};
        bool protectsPost{};
    };

    struct StockPostDependency
    {
        int claimant,holder,actionTick,occupiedTick;
        TilePoint target;
        int claim{-1};
        bool stopping{true};
    };
    struct StockScreenClaim
    {
        int actor,side,grid,movement,departure,latest,lastTick,hold;
        TilePoint from,target;
        bool ignoresCollision;
        std::set<int> posts;
    };
    inline int stockScreenMovementUpper(int actualMovement,int currentFuel)
    {
        // The native getter already trims movement to fuel. If that cap
        // may be active, a resupplied original could walk farther than
        // this turn's observed range. Relax the per-activation limit too.
        return currentFuel>=0 && currentFuel<=actualMovement?std::numeric_limits<int>::max():actualMovement;
    }

    // The complete recorded path and exchange live in proofs[proof]. This
    // observation is conditional on executing that particular rejected trip.
    struct StockAttemptCasualty
    {
        int actor{},side{},requested{},death{},proof{-1};
        TilePoint target{INVALID_TILE},post{INVALID_TILE};
    };

    struct StockCaptureSettlement
    {
        struct Closure {int entry{-1},completion{-1},departure{-1},rate{};TilePoint from{INVALID_TILE};bool replanned{};};
        std::vector<Closure> closures;
        std::vector<StockEntryObligation> entryObligations;
        std::vector<std::pair<int,MilliFunds>> losses;
        std::map<std::pair<int,int>,CaptureRouteShot> paidHits;
        std::map<int,QJsonArray> fundedFollowups;
        QJsonArray proofs,paidCaptureChecks;
        std::vector<StockAttemptCasualty> attemptedCasualties;
        QJsonObject paidCaptureWorkGap,paidCaptureUnsupported;
        std::vector<PaidCaptureCandidate> paidCaptures;
        std::optional<StockCaptureTraffic> townTraffic;
        std::set<int> townBusyStock,townBusyPaid,townProtectedPosts;
        std::map<int,StockCaptureRelease> townAfterCapture, townAfterFactoryExit;
        std::map<int,std::vector<StockPostDependency>> townPostDependencies;
        std::vector<StockScreenClaim> screenClaims;
        std::map<int,int> townGunDutyThrough;
        std::map<int,std::vector<TownGunClaim>> townGunClaims;
        std::set<int> townUncertainGunDuty;
        int attempts{},deferred{};
        std::int64_t routeWork{},defenseWork{};
        bool dependentSearchIncomplete{},interceptRouteIncomplete{};
    };

    // A proposal which fails or gets cut by the horizon never moves the body.
    // Store the final state only when the full earned title has been accepted.
    inline void recordStockCaptureRelease(StockCaptureSettlement &settled,int actor,TilePoint destination,
        const CaptureSurvivalState &state,int threshold,int remainingFuel,std::array<int,2> &rounds)
    {
        if(state.completedTick<0 || state.hp<=0 || state.carry<threshold || destination==INVALID_TILE) return;
        for(const auto &shot:state.shots) spendCombatRound(rounds,shot.counterWeapon);
        settled.townAfterCapture[actor]={destination,state.completedTick+2,remainingFuel,state.hp,rounds};
    }

    inline bool stockCapturePostCanDepart(const StockCaptureTraffic &traffic,int actor,int side,int cell,
        const StockCaptureRelease &release,int last,bool otherDuty)
    {
        if(otherDuty || release.readyTick<0 || release.readyTick>last || release.hp<=0 || release.tile==INVALID_TILE ||
           release.readyTick%2!=side) return false;
        const auto &uses=traffic.uses(cell);
        return std::any_of(uses.begin(),uses.end(),[&](const auto &u){
            return u.actor==actor && u.side==side && u.stopped && u.first<release.readyTick && u.last>=last;
        });
    }

    inline TilePoint retainedOriginalPostAt(const StockCaptureTraffic &traffic,int width,int actor,int tick,
        TilePoint original,TilePoint released,const StockCaptureRelease *exit)
    {
        const auto tile=exit && tick<exit->readyTick-2 ? original : released;
        if(tile==INVALID_TILE)return INVALID_TILE;
        const auto &uses=traffic.uses(tile.y*width+tile.x);
        const bool present=std::any_of(uses.begin(),uses.end(),[&](const auto &u){return u.actor==actor && u.stopped && u.first<=tick && tick<=u.last;});
        return present?tile:INVALID_TILE;
    }

    inline void findPaidCaptures(const PaidCaptureDefenseView &view,const ProductionProjection &production,
        const std::vector<PropertyStockColumn> &columns,const std::set<int> &protectedPosts,
        std::span<const int> booked,StockRouteLowerBounds &bounds,int last,int horizon,int threshold,
        StockCaptureSettlement &settled,std::int64_t &budget)
    {
        struct Bid {int gun,property,earliestFinish;MilliFunds opportunity;};
        std::vector<Bid> bids;
        const int width=view.terrain.width,n=static_cast<int>(view.units.size());
        const auto cell=[&](TilePoint t){return t.y*width+t.x;};
        const auto tile=[](TilePoint t){return QJsonArray{t.x,t.y};};
        const auto &funded=production.fundedGuns;
        std::map<int,const ReinforcementRoute*> committedStarts;
        for(int i=0;i<static_cast<int>(funded.size());++i)
        {
            const auto &g=funded[i];const auto &u=*g.unit;
            if(u.captureRate.empty() || !u.movementGrid || u.movement<=0 || settled.fundedFollowups.contains(i) ||
               settled.paidHits.contains({i,g.impactTick})) continue;
            // A separately guaranteed immediate FIRE is already valuable. The
            // backfill is only for a paid deployment whose claimed job vanished.
            bool free=false;
            for(const auto &v:production.orders)
            {
                const auto o=v.toObject();if(o["factory"].toArray()!=tile(g.factory) || o["buildHalfTurn"].toInt()!=g.buildTick) continue;
                free=o["kind"]=="INTERCEPT_CAPTURE" || o["kind"]=="DEPLOY";
                if(free && o["conditionedFundedPost"].toBool())
                {
                    const auto first=std::find_if(u.routes.begin(),u.routes.end(),[&](const auto &r){
                        return r.activations==1 && r.destination==g.destination && r.firstStop==g.destination;});
                    if(first==u.routes.end() || g.observedService || g.exposure>0 || g.fuelAtPost<0 || u.initialFuel<g.fuelAtPost)
                    {
                        free=false;
                        if(settled.paidCaptureUnsupported.isEmpty())settled.paidCaptureUnsupported={{"reason","CONDITIONED_POST_CAPTURE_PREFIX_UNPRICED"},
                            {"factory",tile(g.factory)},{"birthTick",g.buildTick},{"firstStop",tile(g.destination)}};
                    }
                    else committedStarts.emplace(i,&*first);
                }
                break;
            }
            if(!free) continue;
            bool screen=false;
            for(const auto &r:production.traffic)
                if(r.actor==n+i && r.stopped && r.last>=g.buildTick+2 && protectedPosts.contains(r.cell)) {screen=true;break;}
            if(screen) continue;
            for(int j=0;j<static_cast<int>(columns.size());++j)
            {
                const auto &col=columns[j];const int sq=cell(col.tile);
                if(sq<0 || sq>=static_cast<int>(u.routeAt.size()) || booked[sq]>=0 || col.ownerBefore==(g.side?OwnerSign::Enemy:OwnerSign::Ours) || !u.captureRate.contains(sq) ||
                   u.routeAt[sq]<0 || std::find_if(production.orders.begin(),production.orders.end(),[&](const auto &v){
                       const auto o=v.toObject();return o["kind"]=="CAPTURE" && o["end"].toArray()==tile(col.tile);})!=production.orders.end()) continue;
                const auto &r=u.routes[u.routeAt[sq]];
                const int finish=g.buildTick+2*(r.activations+static_cast<int>(widenedCaptureTurns(0,u.captureRate.at(sq),threshold))-1);
                if(finish>=last) continue;
                MilliFunds gain=0;
                for(int t=finish+1;t<=last;++t) if(t%2==g.side || col.ownerBefore==(g.side?OwnerSign::Ours:OwnerSign::Enemy))
                    gain+=(t%2?col.income.enemyPerTurn:col.income.oursPerTurn)*(horizon-(t-1)/2);
                if(gain>0) bids.push_back({i,j,finish,gain});
            }
        }
        std::stable_sort(bids.begin(),bids.end(),[](const auto &a,const auto &b){return std::tuple{-a.opportunity,a.earliestFinish,a.gun}<std::tuple{-b.opportunity,b.earliestFinish,b.gun};});
        std::array<std::optional<PaidCaptureCandidate>,2> best;
        std::set<std::pair<int,int>> tried;
        for(const auto &bid:bids)
        {
            const auto &g=funded[bid.gun];const auto &u=*g.unit;const auto &col=columns[bid.property];const int sq=cell(col.tile);
            const auto gap=[&](QJsonObject &out,const QString &reason){
                if(out.isEmpty())out={{"reason",reason},{"factory",tile(g.factory)},{"birthTick",g.buildTick},{"target",tile(col.tile)}};
            };
            if(best[g.side] && best[g.side]->utility>=bid.opportunity) continue;
            if(!tried.emplace(bid.gun,bid.property).second) continue;
            if(budget<=0){gap(settled.paidCaptureWorkGap,"PAID_CAPTURE_WORK_LIMIT");break;}
            const int hold=static_cast<int>(widenedCaptureTurns(0,u.captureRate.at(sq),threshold)),latest=2*(horizon-hold)+g.side;
            const auto committed=committedStarts.find(bid.gun);
            const bool locked=committed!=committedStarts.end();
            const auto from=locked?g.destination:g.factory;
            const int first=locked?std::max(g.readyTick,g.buildTick+4):g.buildTick+2;
            auto path=stockCaptureRoute(*u.movementGrid,view.traffic,n+bid.gun,g.side,from,col.tile,u.movement,
                locked?g.fuelAtPost:u.initialFuel,first,first,latest,hold,budget,INVALID_TILE,&bounds);
            if(!path){gap(settled.paidCaptureWorkGap,"PAID_CAPTURE_ROUTE_WORK_LIMIT");break;}
            if(path->entryTick<0) continue;
            if(locked)
            {
                // Replay the actual first movement-and-wait, including enemy
                // fire after it, before allowing this body to seek the title.
                const auto &route=*committed->second;
                std::vector<StockCaptureStep> prefix{{g.buildTick+2,from,{route.cells.rbegin(),route.cells.rend()}}};
                for(int tick=g.buildTick+4;tick<first;tick+=2)prefix.push_back({tick,from,{}});
                path->steps.insert(path->steps.begin(),prefix.begin(),prefix.end());
                path->fuel+=u.initialFuel-g.fuelAtPost;
            }
            bool clear=true;
            for(int t=path->entryTick;t<=last;++t) if(!view.traffic.open(n+bid.gun,g.side,sq,t,true)){clear=false;break;}
            if(!clear) continue;
            auto job=provePaidCapture(view,bid.gun,*path,col.tile,col,threshold,last,budget);
            if(!job){gap(settled.paidCaptureWorkGap,"PAID_CAPTURE_DEFENSE_WORK_LIMIT");break;}
            if(job->status=="DEFERRED_PAID_DUEL_WORK")gap(settled.paidCaptureWorkGap,job->status);
            else if(job->finish<0 && job->status!="BOOKED_STOCK_SUPPORT")gap(settled.paidCaptureUnsupported,job->status);
            if(settled.paidCaptureChecks.size()<12)
            {
                QJsonObject check{{"factory",tile(g.factory)},{"birthTick",g.buildTick},{"target",tile(col.tile)},
                    {"arrivalTick",path->entryTick},{"finishTick",job->finish},{"status",job->status}};
                if(!job->defenderResources.isEmpty())check["defenderResources"]=job->defenderResources;
                if(!job->paidDuelChecks.isEmpty())
                {
                    check["paidDuelChecks"]=job->paidDuelChecks;
                    QJsonArray stops;
                    for(const auto &step:path->steps)stops.append(QJsonArray{step.tick,step.tile.x,step.tile.y});
                    check["paidStops"]=stops;
                }
                settled.paidCaptureChecks.append(check);
            }
            if(job->finish<0) continue;
            bool contested=false;
            for(const auto &v:settled.proofs)
            {
                const auto q=v.toObject();const auto reason=q["status"].toString();
                if(q["enemy"].toBool()!=bool(g.side) && q["tile"].toArray()==tile(col.tile) &&
                   q["proposedEntryTick"].toInt()<=job->finish && (reason=="DEFERRED_WORK_LIMIT" || reason=="DEFERRED_DEPENDENT_LEG" || reason=="DEFERRED_INTERCEPT_BLOCKS_ROUTE"))
                    {contested=true;break;}
            }
            if(contested) continue;
            for(int t=job->finish+1;t<=last;++t) if(t%2==g.side || col.ownerBefore==(g.side?OwnerSign::Ours:OwnerSign::Enemy))
                job->utility+=(t%2?col.income.enemyPerTurn:col.income.oursPerTurn)*(horizon-(t-1)/2);
            for(const auto &hit:job->survivor.shots) job->utility-=(hit.damageValue-hit.counterValue)*(horizon-(hit.shotTick-1)/2);
            if(job->utility>0 && (!best[g.side] || job->utility>best[g.side]->utility)) best[g.side]=std::move(*job);
        }
        if(best[0] && best[1])
        {
            // Do not sell the same property, shooter, route cell or extra
            // payday to both sides. Leave an unresolved race unpriced.
            bool conflict=best[0]->property==best[1]->property;
            for(const auto &a:best[0]->path.steps) for(const auto &b:best[1]->path.steps)
                if(std::abs(a.tick-b.tick)<=1 && (a.tile==b.tile ||
                   std::find(a.cells.begin(),a.cells.end(),cell(b.tile))!=a.cells.end() ||
                   std::find(b.cells.begin(),b.cells.end(),cell(a.tile))!=b.cells.end())) conflict=true;
            for(int side=0;side<2;++side)
                for(const auto &step:best[side]->path.steps)
                    if(step.tick>=best[1-side]->entry && (step.tile==best[1-side]->property ||
                       std::find(step.cells.begin(),step.cells.end(),cell(best[1-side]->property))!=step.cells.end())) conflict=true;
            // Each counter changes the defender's health. Never use the same
            // defender or firing space to certify independent new tasks twice.
            for(const auto &a:best[0]->survivor.shots) for(const auto &b:best[1]->survivor.shots)
                if(a.gun==b.gun || (a.shotTick==b.shotTick && a.from==b.from)) conflict=true;
            for(int side=0;side<2;++side)for(const auto &s:best[side]->survivor.shots)
                if(s.gun==n+best[1-side]->gun)conflict=true;
            if(conflict)
            {
                if(settled.paidCaptureUnsupported.isEmpty())settled.paidCaptureUnsupported={{"reason","CONFLICTING_PAID_CAPTURE_JOBS"}};
                return;
            }
        }
        for(auto &job:best) if(job) settled.paidCaptures.push_back(std::move(*job));
    }

    // Certify one proposed economic schedule. An idle unit keeps its actual
    // square. A failed earlier leg cannot move the origin of a later capture.
    // A purchased unit retains its real deployment even if no capper comes.
    using StockDutyTrip=std::array<int,4>; // Engine unit ID, target x/y and original entry activation.
    struct StockDutyAllowance
    {
        std::set<StockDutyTrip> requested;
        std::int64_t remaining{};
        std::map<StockDutyTrip,std::int64_t> used,pairedSpent;
        std::map<StockDutyTrip,StockDutyTrip> pairedCharges;
    };

    inline StockCaptureSettlement settleStockCaptureTrips(const CombatTerrain &terrain,std::span<const KnownUnit> units,
        std::span<const StockCaptureTrip> trips,const ProductionProjection &production,
        std::span<const TilePoint> positions,std::span<const double> real,std::span<const int> casualtyDay,
        std::span<const int> fuel,std::span<const std::array<int,2>> load,std::span<const int> lastShot,
        std::span<const MilliFunds> alternative,int reservedGuard,int horizon,int cutoff,int threshold,
        StockRouteLowerBounds &lower,std::int64_t &budget,const std::vector<PropertyStockColumn> &captureSites,
        std::span<const TilePoint> excludedCaptureSites,std::int64_t &paidWork,StockDutyAllowance *duty=nullptr)
    {
        StockCaptureSettlement result;
        result.closures.resize(trips.size());
        const int n=static_cast<int>(positions.size()),width=terrain.width,last=std::min(2*horizon-1,cutoff);
        const auto cell=[&](TilePoint t){return t.y*width+t.x;};
        const auto tile=[](TilePoint t){return QJsonArray{t.x,t.y};};
        StockCaptureTraffic traffic(width,terrain.height,last);
        std::set<std::tuple<int,int,int,int>> dependentBirths;
        for(const auto &item:production.orders){
            const auto o=item.toObject();const auto parent=o["stockFactoryParent"].toObject();
            if(parent.isEmpty())continue;
            const auto xy=o["factory"].toArray();
            dependentBirths.emplace(parent["actor"].toInt(-1),int(o["enemy"].toBool()),
                xy[1].toInt()*width+xy[0].toInt(),o["buildHalfTurn"].toInt(-1));
        }
        std::vector<int> startDeparture(n,last+1),departureCount(n,0);
        std::vector<TilePoint> requiredStop(n,INVALID_TILE),location(positions.begin(),positions.end());
        std::vector<bool> hasJob(n);
        for(const auto &leg:trips) hasJob[leg.actor]=true;
        for(const auto &v:production.departures)
        {
            if(v.actor<0 || v.actor>=n)continue;
            ++departureCount[v.actor];
            if(v.tick<startDeparture[v.actor]) {startDeparture[v.actor]=v.tick;requiredStop[v.actor]=v.end;}
        }
        for(const auto &use:production.traffic)
            traffic.reserve(use.cell,use.first,use.last,use.side,use.actor,use.stopped);
        std::vector<CaptureDefenderAvailability> rights(n);
        for(int i=0;i<n;++i)
        {
            if(positions[i]==INVALID_TILE) continue;
            const auto &u=terrain.units[i];
            const int end=casualtyDay[i]<0 ? last : 2*casualtyDay[i];
            traffic.reserve(cell(positions[i]),0,std::min(end,startDeparture[i]-1),u.enemy,i,true);
            if(real[i]<=0 || casualtyDay[i]>=0 || i==reservedGuard || hasJob[i] || startDeparture[i]<=last) continue;
            rights[i]={2*std::max(lastShot[i]+1,alternative[i]>0 ? 2 : 1)+int(u.enemy),
                       fuel[i]<0 ? u.movement : fuel[i],true,alternative[i]};
        }
        std::set<int> spentGuns,protectedPosts;
        const StockCapturePath *currentPath=nullptr;
        int currentSide=-1;
        auto freeSquare=[&](int gun,int tick,TilePoint t,bool transit) {
            const int side=gun<n ? int(terrain.units[gun].enemy) : production.fundedGuns[gun-n].side;
            if(!traffic.open(gun,side,cell(t),tick,!transit)) return false;
            if(currentPath && currentSide!=side && gun>=n)
            {
                TilePoint occupied=INVALID_TILE;
                for(const auto &step:currentPath->steps) if(step.tick<=tick) occupied=step.tile;
                if(occupied==t) return false;
            }
            if(!transit && gun<n && t!=positions[gun])
                for(int at=tick+1;at<=last;++at) if(!traffic.open(gun,side,cell(t),at,true)) return false;
            return true;
        };
        auto gunAvailable=[&](int gun,int tick){return (gun>=n && production.fundedGuns[gun-n].impactTick==tick) || !spentGuns.contains(gun);};
        std::vector<std::array<int,2>> availableLoad(load.begin(),load.end());
        CaptureRouteDefense defense{terrain,units,positions,real,availableLoad,rights,production.fundedGuns,freeSquare,gunAvailable};
        defense.geometryRevision=[&]{return traffic.generation();};
        defense.canLeavePost=[&](int,int tick,TilePoint post) {
            return tick+2<=last && !protectedPosts.contains(cell(post));
        };
        std::vector<int> fuelLeft(fuel.begin(),fuel.end()),nextFree(n);
        std::vector<double> hp(real.begin(),real.end());
        std::vector<bool> declinedPredecessor(n),replannedChain(n),unfinishedPredecessor(n);
        std::array<bool,2> armed{};
        for(int i=0;i<n;++i) if(real[i]>0 && positions[i]!=INVALID_TILE)
            armed[int(terrain.units[i].enemy)] |= units[i].canFire || units[i].hasWeapons;
        for(const auto &g:production.fundedGuns) if(g.unit && g.unit->maxRange>0)armed[g.side]=true;
        // A purchased capper may meet a stock unit before, between or after
        // its accepted jobs. A rejected route never changes these resources.
        struct ResourceEvent {int tick;double hpAfter;int fuelUsed,weapon;bool fired;};
        const bool pricePaid=std::any_of(production.fundedGuns.begin(),production.fundedGuns.end(),
            [](const auto &g){return g.unit && !g.unit->captureRate.empty();});
        std::vector<std::vector<ResourceEvent>> history(pricePaid?n:0);
        const auto perDuty=duty && !duty->requested.empty()?
            duty->remaining/static_cast<std::int64_t>(duty->requested.size()):0;
        std::set<std::tuple<int,int,int>> traveled;
        std::vector<int> targets(width*terrain.height,-1);
        for(int j=0;j<static_cast<int>(trips.size());++j)
        {
            auto leg=trips[j];const auto &pawn=terrain.units[leg.actor];const auto &known=units[leg.actor];
            const StockDutyTrip key{pawn.engineUnitId,leg.target.x,leg.target.y,leg.entry};
            const bool matched=duty && duty->pairedCharges.contains(key);
            const StockDutyTrip charge=matched?duty->pairedCharges.at(key):key;
            const bool requested=matched || (duty && duty->requested.contains(key));
            const auto extra=requested?perDuty-duty->used[charge]:0;
            QJsonObject event{{"unit",pawn.engineUnitId},{"enemy",bool(leg.side)},{"from",tile(leg.from)},{"tile",tile(leg.target)},
                              {"departureTick",leg.departure},{"proposedEntryTick",leg.entry},{"proposedCompletionTick",leg.completion}};
            const bool replan=replannedChain[leg.actor] || (declinedPredecessor[leg.actor] && leg.from!=location[leg.actor]);
            std::optional<StockAttemptCasualty> casualty;
            auto decline=[&](const QString &reason) {event["status"]=reason;event["entryTick"]=-1;event["completionTick"]=-1;
                declinedPredecessor[leg.actor]=true;
                if(reason=="DEFERRED_WORK_LIMIT" || reason=="DEFERRED_DEPENDENT_LEG" || reason=="DEFERRED_INTERCEPT_BLOCKS_ROUTE")unfinishedPredecessor[leg.actor]=true;
                if(replan)result.dependentSearchIncomplete=true;
                if(casualty && reason=="DECLINED_EXPOSED_APPROACH")
                {
                    casualty->proof=static_cast<int>(result.proofs.size());result.attemptedCasualties.push_back(*casualty);
                }
                result.proofs.append(event);++result.deferred;};
            if(replan)
            {
                const int actor=leg.actor;
                auto post=location[actor];
                event["proposedFrom"]=event["from"];event["proposedDepartureTick"]=leg.departure;
                event["retainedFrom"]=tile(post);event["replannedAfterDecline"]=true;
                replannedChain[actor]=true;
                // A feasible competing itinerary does not exhaust a work-limited
                // predecessor or discharge the defensive commitment it exposed.
                result.dependentSearchIncomplete|=unfinishedPredecessor[actor];
                // The retained first stop for a purchase may be consumed once, by
                // the route's first activation. Never jump forward to the paid exit.
                const int ready=nextFree[actor]>0?nextFree[actor]:leg.retainedReadyTick;
                if(post==INVALID_TILE || pawn.grid<0 || !known.canCapture || hp[actor]<=0 || leg.retainedReadyTick<0 ||
                   ready<2+leg.side || ready%2!=leg.side || casualtyDay[actor]>=0 || actor==reservedGuard ||
                   lastShot[actor]>=1 || spentGuns.contains(actor) || result.townGunDutyThrough.contains(actor) ||
                   protectedPosts.contains(cell(post)) || departureCount[actor]>1 ||
                   (requiredStop[actor]!=INVALID_TILE && ready!=startDeparture[actor]) ||
                   std::any_of(production.fundedGuns.begin(),production.fundedGuns.end(),[&](const auto &g){
                       return g.targetUnit==actor && g.impactTick>=ready; }))
                {
                    event["retainedReplanReason"]="ACTIVATION_OR_RETAINED_SERVICE_UNVERIFIED";
                    decline("DEFERRED_DEPENDENT_LEG");continue;
                }
                if(leg.target==post || targets[cell(leg.target)]!=-1 ||
                   std::find(excludedCaptureSites.begin(),excludedCaptureSites.end(),leg.target)!=excludedCaptureSites.end())
                {
                    event["retainedReplanReason"]="EXISTING_CARRY_OR_OWNERSHIP_NEEDS_REPRICE";
                    decline("DEFERRED_DEPENDENT_LEG");continue;
                }
                // The stock ledger stops at title transfer. It has no reply
                // on that last capture activation, nor a fresh dated roster
                // for a gun already used by a different accepted route.
                const bool armedAfterTitle=nextFree[actor]>0 && armed[1-leg.side];
                if((nextFree[actor]==0 && leg.side && armed[0]) || armedAfterTitle ||
                   std::any_of(spentGuns.begin(),spentGuns.end(),[&](int g){
                    return (g<n?int(terrain.units[g].enemy):production.fundedGuns[g-n].side)!=leg.side;
                }))
                {
                    event["retainedReplanReason"]="PRIOR_COMBAT_STATE_NEEDS_REPLAY";
                    decline("DEFERRED_DEPENDENT_LEG");continue;
                }
                const auto original=std::find_if(captureSites.begin(),captureSites.end(),[&](const auto &col){return col.tile==leg.target;});
                if(original==captureSites.end() || original->ownerBefore==(leg.side?OwnerSign::Enemy:OwnerSign::Ours))
                {
                    event["retainedReplanReason"]="TITLE_DEPENDS_ON_ANOTHER_TRANSFER";
                    decline("DEFERRED_DEPENDENT_LEG");continue;
                }
                leg.from=post;leg.departure=ready;leg.entry=ready;leg.carry=0;leg.property=*original;
                leg.rate=replyCaptureRate(known.captureRate,known.hpRounded-ceilHpSteps(hp[actor]),false);
                if(leg.rate<=0){decline("DEFERRED_DEPENDENT_LEG");continue;}
                event["from"]=tile(post);event["departureTick"]=ready;event["hpAtDeparture"]=hp[actor];
                event["fuelAtDeparture"]=fuelLeft[actor];event["ammoAtDeparture"]=QJsonArray{availableLoad[actor][0],availableLoad[actor][1]};
            }
            if(leg.from!=location[leg.actor] || leg.entry>last || pawn.grid<0 || hp[leg.actor]<=0) {decline("DEFERRED_DEPENDENT_LEG");continue;}
            if(targets[cell(leg.target)]==leg.side) {decline("PREVIOUSLY_CAPTURED");continue;}
            int start=std::max(leg.departure,nextFree[leg.actor]);
            if(start>last) {decline("OUTSIDE_HORIZON");continue;}
            TilePoint first=INVALID_TILE;
            if(requiredStop[leg.actor]!=INVALID_TILE)
            {
                if(start==startDeparture[leg.actor]) first=requiredStop[leg.actor];
                else if(start>startDeparture[leg.actor]) {decline("ALREADY_DEPLOYED_ELSEWHERE");continue;}
            }
            const int hold=static_cast<int>(widenedCaptureTurns(leg.carry,leg.rate,threshold));
            const int latest=2*(horizon-hold)+leg.side;
            defense.beginTrip();
            int fastest=0;
            for(int i=0;i<n;++i) if(int(terrain.units[i].enemy)==leg.side && positions[i]!=INVALID_TILE && real[i]>0)
                fastest=std::max(fastest,units[i].captureRate);
            bool ordinaryRate=false;
            int woundedTitleTerms=0;
            defense.avoidedCaptureIncome=[&](int actor,TilePoint at,int ownTick,int carry,double before,double after)->MilliFunds {
                const int arrival=replan && currentPath?currentPath->entryTick:leg.entry;
                const bool capturing=at==leg.target && ownTick>=arrival;
                const int next=capturing ? ownTick+2 : std::max(ownTick+2,arrival);
                const int oldRate=replyCaptureRate(known.captureRate,known.hpRounded-ceilHpSteps(before),false);
                const int newRate=replyCaptureRate(known.captureRate,known.hpRounded-ceilHpSteps(after),after<=0);
                if(actor!=leg.actor || oldRate<=0 || (after<=0 && !capturing)) return 0;
                auto clock=[&](int carried,int rate) {
                    return rate<=0 ? 2*horizon+leg.side : static_cast<int>(std::min<std::int64_t>(2*horizon+leg.side,
                        next+2*(widenedCaptureTurns(carried,rate,threshold)-1)));
                };
                const int carried=capturing?carry:0;
                // Evaluating the original capper's opportunity damage: a further hit cannot deny
                // another payday if this already-wounded unit cannot finish in the no-repair line.
                const int carryUpper=at==leg.target?carry:0;
                if(woundedCaptureWindow(ordinaryRate,before,carryUpper,threshold,ownTick+2,leg.side,last))
                {++woundedTitleTerms;return 0;}
                const int first=clock(carried,oldRate);
                const auto lostTitle=woundedCaptureWindow(ordinaryRate,after,carryUpper,threshold,ownTick+2,leg.side,last);
                if(lostTitle)++woundedTitleTerms;
                const int originalAfter=lostTitle?2*horizon+leg.side:clock(carried,newRate);
                const int until=std::min(originalAfter,clock(0,fastest));
                const OwnerSign defender=leg.side ? OwnerSign::Ours : OwnerSign::Enemy;
                MilliFunds gain=0;
                for(int tick=first+1;tick<=until && tick<=last;++tick)
                {
                    if(tick%2==leg.side || leg.property.ownerBefore==defender)
                        gain+=tick%2 ? leg.property.income.enemyPerTurn : leg.property.income.oursPerTurn;
                }
                return gain;
            };
            const auto initialWork=budget;
            const std::int64_t normal=budget/std::max<std::size_t>(1,trips.size()-j);
            const auto named=matched?std::max<std::int64_t>(0,perDuty-duty->pairedSpent[charge]):0;
            const auto baseLeg=matched?std::min(normal,named):normal;
            std::int64_t legBudget=matched?named:baseLeg+extra;
            const auto initialLegBudget=legBudget;
            ordinaryRate=terrain.ordinaryCaptureInstallments && known.captureRate==known.hpRounded &&
                terrain.certifyRetainedWalkProfile && terrain.certifyRetainedWalkProfile(leg.actor,last,legBudget);
            event["stockBaseAllowance"]=static_cast<qint64>(baseLeg);
            if(matched)event["matchedStockReplayAllowance"]=static_cast<qint64>(named);
            event["stockDutyAllowance"]=static_cast<qint64>(matched?named-baseLeg:extra);
            std::set<std::pair<int,int>> excluded;
            bool accepted=false, limited=false,interceptBlocked=false;
            std::set<int> possibleGuardPosts;
            std::map<int,int> possibleGunDuty;
            std::map<int,int> firstConditionalShot;
            std::map<int,int> conditionalEntry;
            std::set<std::tuple<int,int,int,int,bool>> neededPosts; // square, holder, claimant activation, physical use, stop

            const auto recordCut=[&](const StockEntryCut &cut){
                QJsonArray blockers,guards,impassable;
                for(const auto &post:cut.posts)
                {
                    const auto pt=tile({post.cell%width,post.cell/width});
                    blockers.append(pt);possibleGuardPosts.insert(post.cell);
                    QJsonObject guard{{"tile",pt},{"firstTick",post.from},{"lastTick",post.through}};
                    if(post.actor<n) guard["unit"]=terrain.units[post.actor].engineUnitId;
                    else
                    {
                        const auto &g=production.fundedGuns.at(post.actor-n);
                        guard["recruit"]=g.unit->type;guard["birthSite"]=tile(g.factory);guard["birthTick"]=g.buildTick;
                    }
                    guards.append(guard);
                    for(int t=post.from;t<=post.through;++t)
                        neededPosts.emplace(post.cell,post.actor,std::max(0,t-int(t%2!=leg.side)),t,post.cell==cell(leg.target));
                }
                for(int sq:cut.impassable)impassable.append(tile({sq%width,sq/width}));
                result.entryObligations.push_back({leg.actor,leg.target,traffic.generation(),cut.posts});
                event["blockedTiles"]=blockers;
                event["entryCut"]=QJsonObject{{"earliest",cut.earliest},{"last",cut.last},
                    {"guards",guards},{"impassable",impassable}};
                if(!cut.interior.empty())
                {
                    QJsonArray pocket;
                    for(const int sq:cut.interior)pocket.append(tile({sq%width,sq/width}));
                    auto detail=event["entryCut"].toObject();
                    detail["targetRegion"]=pocket;
                    detail["earliestActivationGranted"]=cut.release;
                    detail["claimantOrigin"]=tile(leg.from);
                    detail["scope"]="ORIGINAL_UNASSISTED_STOCK_ROUTE_ON_RETAINED_PORTFOLIO";
                    event["entryCut"]=detail;
                }
            };

            currentPath=nullptr;currentSide=leg.side;
            // First try the proposed line, then one different overnight approach
            // if the first was shot. This is a bounded set of real alternatives.
            for(int alternativePath=0;alternativePath<2 && !accepted;++alternativePath)
            {
                currentPath=nullptr;
                const auto allowed=[&](int t,TilePoint at){return !excluded.contains({t,cell(at)});};
                if(alternativePath==0)
                {
                    // A sufficient final-step cut may prove denial without promising
                    // every unit the larger route search would happen to encounter.
                    const auto allowance=legBudget/8;
                    std::int64_t cutWork=allowance;
                    const auto cut=stockEntryCut(terrain.grids[pawn.grid],traffic,lower,leg.actor,leg.side,
                        leg.from,leg.target,pawn.movement,pawn.ignoresCollision,last,cutWork);
                    legBudget-=allowance-cutWork;result.routeWork+=allowance-cutWork;
                    if(cut){recordCut(*cut);++result.attempts;break;}
                }
                auto before=legBudget;
                std::set<std::tuple<int,int,bool>> blockedAt;
                auto noteBlock=[&](int sq,int tick,bool stop){blockedAt.emplace(sq,tick,stop);};
                auto path=stockCaptureRoute(terrain.grids[pawn.grid],traffic,leg.actor,leg.side,leg.from,leg.target,pawn.movement,
                    fuelLeft[leg.actor],start,std::max(leg.entry,start),latest,hold,legBudget,first,&lower,allowed,pawn.ignoresCollision,noteBlock);
                result.routeWork+=before-legBudget;++result.attempts;
                if(!path) {limited=true;break;}
                if(path->entryTick<0)
                {
                    if(!blockedAt.empty() && terrain.certifyRetainedWalkProfile)
                    {
                        // The original walk has already failed. A small share
                        // may prove that a strict subset of its encountered
                        // blockers still suffices, including behind an entrance.
                        const auto offer=legBudget/4;
                        auto left=offer;
                        // Even a different first order cannot leave this particular
                        // origin before the first activation. A chained origin is
                        // available after its actually settled prior capture.
                        const auto prior=result.townAfterCapture.find(leg.actor);
                        const int release=leg.from==positions[leg.actor]?leg.side:
                            prior!=result.townAfterCapture.end() && prior->second.tile==leg.from?prior->second.readyTick:-1;
                        std::optional<StockEntryCut> cut;
                        if(release>=0 && pawn.movementBeforeFuel>0 && terrain.certifyRetainedWalkProfile(leg.actor,last,left))
                            cut=stockPocketEntryCut(terrain.grids[pawn.grid],traffic,lower,leg.actor,leg.side,leg.from,
                                leg.target,pawn.movementBeforeFuel,pawn.ignoresCollision,release,last,left);
                        legBudget-=offer-left;result.routeWork+=offer-left;
                        if(cut){recordCut(*cut);break;}
                    }
                    QJsonArray blockers;
                    for(int sq:path->blockedCells) {blockers.append(tile({sq%width,sq/width}));possibleGuardPosts.insert(sq);}
                    event["blockedTiles"]=blockers;
                    for(const auto &[sq,tick,stop]:blockedAt)
                        for(const auto &u:traffic.uses(sq))
                            if(u.actor!=leg.actor && u.first<=tick && tick<=u.last && (u.side!=leg.side || (u.stopped && stop)))
                                neededPosts.emplace(sq,u.actor,tick-int(tick%2!=leg.side),tick,stop);
                    break;
                }
                currentPath=&*path;
                defense.beginTrip();
                QJsonArray stops,installments;
                CaptureSurvivalState state{hp[leg.actor],leg.carry};
                bool injuryRecorded=false;
                std::map<int,int> stopped;
                auto survive=[&](TilePoint pos,int tick,bool capture) {
                    stopped[tick]=cell(pos);const int carried=state.carry;const double beforeHp=state.hp;
                    const auto prior=legBudget;
                    const bool priced=advanceCaptureSurvival(defense,state,leg.actor,pos,tick,capture,threshold,known.captureRate,known.hpRounded,legBudget);
                    result.defenseWork+=prior-legBudget;
                    if(capture) installments.append(QJsonObject{{"tick",tick},{"hpBeforeReply",beforeHp},{"carryBefore",carried},{"carryAfter",state.carry}});
                    if(priced && !injuryRecorded && !state.shots.empty() && state.shots.back().shotTick==tick+1 && state.hp<beforeHp && state.completedTick<0)
                    {
                        const int carriedOnTarget=pos==leg.target?state.carry:0;
                        const auto window=woundedCaptureWindow(ordinaryRate,state.hp,carriedOnTarget,threshold,tick+2,leg.side,last);
                        if(window)
                        {
                            QJsonArray shots;
                            for(const auto &shot:state.shots)
                            {
                                QJsonObject hit{{"tick",shot.shotTick},{"post",tile(shot.from)},{"hpBefore",shot.targetBefore},
                                    {"hpAfter",shot.targetAfter},{"shooterHpAfter",shot.shooterAfter},{"weapon",shot.weapon}};
                                if(shot.gun<n)hit["unit"]=terrain.units[shot.gun].engineUnitId;
                                else {const auto &g=production.fundedGuns[shot.gun-n];hit["birth"]=tile(g.factory);hit["birthTick"]=g.buildTick;hit["type"]=g.unit->type;}
                                shots.append(hit);
                            }
                            auto windows=event["woundedCaptureWindows"].toArray();
                            windows.append(QJsonObject{{"through",last},{"firstActivation",window->firstActivation},
                                {"activationsGranted",window->activations},{"rateUpper",window->rateUpper},{"carry",window->carried},
                                {"pointsUpper",static_cast<qint64>(window->pointsUpper)},{"threshold",threshold},{"hp",state.hp},
                                {"post",tile(pos)},{"stops",stops},{"shots",shots},{"installments",installments},
                                {"unassistedTitleLower",0},{"unassistedTitleUpper",0},{"otherMaterial","UNRESOLVED"},
                                {"otherBranches",QJsonArray{"EARLIER_ORDERS","REPAIR_OR_JOIN","ADDITIONAL_ACTION_OR_CARRY","REPLACEMENT_CAPPER"}}});
                            event["woundedCaptureWindows"]=windows;injuryRecorded=true;
                        }
                    }
                    
                    return priced;
                };
                TilePoint previousStop=leg.from;
                bool incompatibleStep=false;
                for(auto &step:path->steps)
                {
                    if(const auto *hit=crossedInterceptPost(state.shots,step,width,pawn.ignoresCollision))
                    {
                        const auto original=step.cells;const auto beforeRepair=legBudget;
                        const auto repaired=repairInterceptedStep(terrain.grids[pawn.grid],traffic,state.shots,leg.actor,
                            leg.side,pawn.movement,pawn.ignoresCollision,previousStop,step,path->fuel,lower,legBudget);
                        result.routeWork+=beforeRepair-legBudget;
                        QJsonArray oldPath,newPath;
                        for(int sq:original)oldPath.append(tile({sq%width,sq/width}));
                        if(repaired && *repaired)for(int sq:step.cells)newPath.append(tile({sq%width,sq/width}));
                        QJsonObject info{{"moveTick",step.tick},{"from",tile(previousStop)},{"to",tile(step.tile)},
                            {"shooterPost",tile(hit->from)},{"shotTick",hit->shotTick},{"shooterHp",hit->shooterAfter},
                            {"originalCells",oldPath},{"replacementCells",newPath},{"status",!repaired?"WORK_LIMITED":*repaired?"SAME_STOP_REROUTED":"NO_SAME_STOP_REPAIR"}};
                        if(hit->gun>=0 && hit->gun<n)info["unit"]=terrain.units[hit->gun].engineUnitId;
                        auto reviews=event["interceptTransitChecks"].toArray();reviews.append(info);event["interceptTransitChecks"]=reviews;
                        if(!repaired || !*repaired)
                        {
                            limited=!repaired;incompatibleStep=true;interceptBlocked=true;casualty.reset();
                            result.interceptRouteIncomplete=true;
                            if(!limited)excluded.emplace(step.tick-2,cell(previousStop));
                            break;
                        }
                    }
                    QJsonArray crossed;for(int sq:step.cells) crossed.append(tile({sq%width,sq/width}));
                    stops.append(QJsonObject{{"tick",step.tick},{"tile",tile(step.tile)},{"cells",crossed}});
                    if(!survive(step.tile,step.tick,step.tick==path->entryTick)) {limited=true;break;}
                    if(state.hp<=0) break;
                    previousStop=step.tile;
                }
                if(incompatibleStep){if(limited)break;continue;}
                for(int tick=path->entryTick+2;!limited && state.hp>0 && state.completedTick<0 && tick<=last;tick+=2)
                {
                    if(!traffic.open(leg.actor,leg.side,cell(leg.target),tick,true) ||
                       !traffic.open(leg.actor,leg.side,cell(leg.target),tick+1,true)) break;
                    if(!survive(leg.target,tick,true)) limited=true;
                }
                // This capper can decline only while these actual guns still
                // have the option to take their dated shots. An accepted
                // alternative below installs its own combat history instead.
                for(const auto &shot:state.shots)
                {
                    const int through=shot.shotTick+(shot.deployment.from==INVALID_TILE?0:2);
                    possibleGunDuty[shot.gun]=std::max(possibleGunDuty[shot.gun],through);
                    auto [first,added]=firstConditionalShot.emplace(shot.gun,shot.shotTick);
                    if(added || shot.shotTick<first->second){first->second=shot.shotTick;conditionalEntry[shot.gun]=path->entryTick;}
                }
                if(limited)
                {
                    // A second attempt may meet the meter after a different
                    // approach was fully exchanged. Its stops cannot describe
                    // the shots or installments recorded for that earlier path.
                    event["unfinishedRoute"]=QJsonObject{{"attempt",alternativePath},{"attemptedStops",stops}};
                    break;
                }
                QJsonArray shots;
                std::map<int,std::array<int,2>> firedClips;
                auto capperClips=availableLoad[leg.actor];
                for(const auto &shot:state.shots)
                {
                    QJsonObject detail{{"gun",shot.gun<n ? terrain.units[shot.gun].engineUnitId : -1},{"tick",shot.shotTick},{"from",tile(shot.from)},
                        {"hpBefore",shot.targetBefore},{"hpAfter",shot.targetAfter},{"shooterHpAfter",shot.shooterAfter},
                        {"targetCapital",static_cast<qint64>(shot.damageValue)},{"counterCapital",static_cast<qint64>(shot.counterValue)},
                        {"newSignedLoss",static_cast<qint64>(shot.alreadyPriced?0:(leg.side?-1:1)*(shot.damageValue-shot.counterValue))},{"alreadyPriced",shot.alreadyPriced}};
                    if(shot.gun>=0 && shot.gun<n && !shot.alreadyPriced){
                        const auto &r=rights[shot.gun];const auto &clips=availableLoad[shot.gun];
                        detail["proposalGunSource"]=QJsonObject{{"post",tile(positions[shot.gun])},
                            {"hp",real[shot.gun]},{"ammo",QJsonArray{clips[0],clips[1]}},
                            {"firstFree",r.firstFreeTick},{"mayRelocate",r.mayRelocate},
                            {"fuel",fuel[shot.gun]},{"foregoneOpportunity",static_cast<qint64>(r.alternativeValue)}};
                    }
                    detail["weapon"]=shot.weapon;detail["counterWeapon"]=shot.counterWeapon;
                    detail["capperAmmoBefore"]=QJsonArray{capperClips[0],capperClips[1]};
                    spendCombatRound(capperClips,shot.counterWeapon);
                    detail["capperAmmoAfter"]=QJsonArray{capperClips[0],capperClips[1]};
                    if(shot.gun<n)
                    {
                        auto &clip=firedClips.try_emplace(shot.gun,availableLoad[shot.gun]).first->second;
                        detail["shooterAmmoBefore"]=QJsonArray{clip[0],clip[1]};
                        spendCombatRound(clip,shot.weapon);
                        detail["shooterAmmoAfter"]=QJsonArray{clip[0],clip[1]};
                    }
                    else detail["shooterAmmoStatus"]="FUNDED_WEAPON_SLOT_UNRESOLVED";
                    const auto target=stopped.find(shot.shotTick-1);
                    if(target!=stopped.end())detail["targetTile"]=tile({target->second%width,target->second/width});
                    if(shot.motion.origin!=INVALID_TILE)
                    {
                        QJsonArray path;for(int sq:shot.motion.cells) path.append(tile({sq%width,sq/width}));
                        detail["movedFrom"]=tile(shot.motion.origin);detail["movementFuel"]=shot.motion.fuel;detail["movementCells"]=path;
                    }
                    if(shot.gun>=n)
                    {
                        const auto &recruit=production.fundedGuns[shot.gun-n];detail["recruit"]=recruit.unit->type;
                        detail["birthSite"]=tile(recruit.factory);detail["birthTick"]=recruit.buildTick;
                        const int post=cell(recruit.destination);
                        possibleGuardPosts.insert(post);
                        neededPosts.emplace(post,shot.gun,shot.shotTick-1,shot.shotTick,true);
                        if(shot.deployment.from!=INVALID_TILE) neededPosts.emplace(post,shot.gun,shot.shotTick-1,shot.shotTick+2,true);
                    }
                    if(shot.deployment.from!=INVALID_TILE)
                    {
                        QJsonArray out,back;
                        for(int square:shot.deployment.forward) out.append(tile({square%width,square/width}));
                        for(int square:shot.deployment.back) back.append(tile({square%width,square/width}));
                        detail["outbound"]=out;detail["returnPath"]=back;detail["returnTick"]=shot.shotTick+2;
                        detail["movementFuel"]=shot.deployment.fuel;
                    }
                    shots.append(detail);
                }
                event["routeAttempt"]=alternativePath;event["stops"]=stops;
                event["shots"]=shots;event["captureInstallments"]=installments;
                casualty.reset();
                if(state.hp==0 && state.completedTick<0 && !state.shots.empty())
                {
                    const auto &hit=state.shots.back();const auto site=stopped.find(hit.shotTick-1);
                    if(hit.targetBefore>0 && hit.targetAfter==0 && site!=stopped.end())
                        casualty=StockAttemptCasualty{leg.actor,leg.side,leg.entry,hit.shotTick,-1,leg.target,
                            {site->second%width,site->second/width}};
                }
                if(state.hp<=0 || state.completedTick<0 || state.completedTick>last)
                {
                    // A later detour can have a different arrival. Keep the
                    // original witnessed entry and its dated defensive shots
                    // when the squad comparison specifically challenges it.
                    if(requested && !installments.isEmpty() && installments.first().toObject()["tick"].toInt(-1)==path->entryTick)
                    {
                        auto rejected=event["rejectedCaptureAttempts"].toArray();
                        rejected.append(QJsonObject{{"actualEntry",path->entryTick},{"attempt",alternativePath},
                            {"stops",stops},{"shots",shots},{"installments",installments},{"hpAfter",state.hp},
                            {"captureCompleted",false}});
                        event["rejectedCaptureAttempts"]=rejected;
                    }
                    if(state.shots.empty()) break;
                    const int firstHit=state.shots.front().shotTick-1;
                    if(!stopped.contains(firstHit) || !excluded.emplace(firstHit,stopped[firstHit]).second) break;
                    continue;
                }
                // The capper holds its property after completion until a later
                // certified activation. It cannot displace an already paid stop.
                int occupiedAfter=path->entryTick;
                for(;occupiedAfter<=last;++occupiedAfter)
                    if(!traffic.open(leg.actor,leg.side,cell(leg.target),occupiedAfter,true))break;
                if(occupiedAfter<=last){
                    const bool proposedExit=occupiedAfter>state.completedTick &&
                        dependentBirths.contains({leg.actor,leg.side,cell(leg.target),occupiedAfter}) &&
                        std::any_of(trips.begin()+j+1,trips.end(),[&](const StockCaptureTrip &next){
                            return next.actor==leg.actor && next.side==leg.side && next.from==leg.target &&
                                next.target!=leg.target && next.departure>state.completedTick && next.departure<=occupiedAfter;
                        });
                    if(!proposedExit)break;
                    // Keep the capper's full stay in traffic until the retained
                    // outward trip actually vacates it. The complete funded
                    // schedule checks that resulting stop before the purchase.
                    event["conditionalBirthTick"]=occupiedAfter;
                }
                if(replan && std::any_of(state.shots.begin(),state.shots.end(),[&](const auto &shot){
                    return result.townGunDutyThrough.contains(shot.gun) || protectedPosts.contains(cell(shot.from));
                }))
                {
                    event["retainedReplanReason"]="COMPETING_GUN_OR_SCREEN_NEEDS_REPRICE";
                    break;
                }
                event["status"]="WITNESSED";event["entryTick"]=path->entryTick;event["completionTick"]=state.completedTick;
                event["actualCompletionTick"]=state.completedTick;event["finalCarry"]=state.carry;event["captureThreshold"]=threshold;
                event["fuelSpent"]=path->fuel;event["hpAfter"]=state.hp;
                result.closures[j]={path->entryTick,state.completedTick,start,leg.rate,leg.from,replan};
                declinedPredecessor[leg.actor]=false;
                traffic.vacate(leg.actor,cell(location[leg.actor]),start);
                if(first!=INVALID_TILE) traffic.vacate(leg.actor,cell(first),start+2);
                for(const auto &step:path->steps)
                {
                    for(int sq:step.cells)traffic.reserve(sq,step.tick,step.tick,leg.side,leg.actor,false);
                    traffic.reserve(cell(step.tile),step.tick,step.tick+1,leg.side,leg.actor,true);
                }
                traffic.reserve(cell(leg.target),path->entryTick,last,leg.side,leg.actor,true);
                if(pricePaid)
                {
                    const auto &grid=terrain.grids[pawn.grid];
                    auto from=leg.from;
                    for(const auto &step:path->steps)
                    {
                        if(!production.departures.empty())traveled.emplace(leg.actor,step.tick,cell(step.tile));
                        int consumed=0;
                        for(int sq:step.cells)
                        {
                            const TilePoint to{sq%width,sq/width};
                            consumed+=grid.cost(to.x,to.y,entryDirection(from.x,from.y,to.x,to.y));from=to;
                        }
                        if(consumed)history[leg.actor].push_back({step.tick,-1,consumed,-1,false});
                    }
                    for(const auto &hit:state.shots)
                    {
                        history[leg.actor].push_back({hit.shotTick,hit.targetAfter,0,hit.counterWeapon,false});
                        if(hit.gun<n)history[hit.gun].push_back({hit.shotTick,hit.shooterAfter,hit.motion.fuel,hit.weapon,true});
                    }
                }
                if(fuelLeft[leg.actor]>=0) fuelLeft[leg.actor]-=path->fuel;
                hp[leg.actor]=state.hp;location[leg.actor]=leg.target;requiredStop[leg.actor]=INVALID_TILE;
                nextFree[leg.actor]=state.completedTick+2;targets[cell(leg.target)]=leg.side;
                recordStockCaptureRelease(result,leg.actor,leg.target,state,threshold,fuelLeft[leg.actor],availableLoad[leg.actor]);
                for(std::size_t shotNumber=0;shotNumber<state.shots.size();++shotNumber)
                {
                    const auto &shot=state.shots[shotNumber];
                    if(shot.alreadyPriced) {result.paidHits[{shot.gun-n,shot.shotTick}]=shot;continue;}
                    spentGuns.insert(shot.gun);
                    if(shot.gun<n && shot.shooterAfter<=0 && shot.motion.origin==INVALID_TILE)
                        traffic.vacate(shot.gun,cell(shot.from),shot.shotTick);
                    if(shot.motion.origin!=INVALID_TILE)
                    {
                        traffic.vacate(shot.gun,cell(shot.motion.origin),shot.shotTick);
                        for(int transit:shot.motion.cells) traffic.reserve(transit,shot.shotTick,shot.shotTick,1-leg.side,shot.gun,false);
                    }
                    if(shot.gun>=n)
                    {
                        auto detail=shots[static_cast<int>(shotNumber)].toObject();detail["target"]=pawn.engineUnitId;
                        result.fundedFollowups[shot.gun-n].append(detail);
                    }
                    if(shot.deployment.from!=INVALID_TILE)
                    {
                        const auto anchor=production.fundedGuns[shot.gun-n].destination;
                        traffic.vacate(shot.gun,cell(anchor),shot.shotTick);
                        for(int sq:shot.deployment.forward) traffic.reserve(sq,shot.shotTick,shot.shotTick,1-leg.side,shot.gun,false);
                        traffic.reserve(cell(shot.from),shot.shotTick,shot.shotTick+1,1-leg.side,shot.gun,true);
                        for(int sq:shot.deployment.back) traffic.reserve(sq,shot.shotTick+2,shot.shotTick+2,1-leg.side,shot.gun,false);
                        traffic.reserve(cell(anchor),shot.shotTick+2,last,1-leg.side,shot.gun,true);
                    }
                    else if(shot.shooterAfter>0) traffic.reserve(cell(shot.from),shot.shotTick,last,1-leg.side,shot.gun,true);
                    result.losses.emplace_back(shot.shotTick,(leg.side?-1:1)*(shot.damageValue-shot.counterValue));
                }
                accepted=true;
            }
            const auto consumed=initialLegBudget-legBudget;
            budget=initialWork-std::min(baseLeg,consumed);
            if(extra>0)
            {
                const auto spentExtra=std::max<std::int64_t>(0,consumed-baseLeg);
                duty->remaining-=spentExtra;duty->used[charge]+=spentExtra;
                if(matched)duty->pairedSpent[charge]+=consumed;
            }
            if(woundedTitleTerms)event["woundedTitleTermsPruned"]=woundedTitleTerms;
            event["stockWorkSpent"]=static_cast<qint64>(consumed);
            if(accepted)result.proofs.append(event);
            currentPath=nullptr;
            if(!accepted)
            {
                protectedPosts.insert(possibleGuardPosts.begin(),possibleGuardPosts.end());
                for(const auto &[gun,through]:possibleGunDuty)
                {
                    const int end=limited ? last : through;
                    auto &promised=result.townGunDutyThrough[gun];promised=std::max(promised,end);
                    result.townGunClaims[gun].push_back({leg.actor,leg.target,firstConditionalShot.at(gun),end,limited,
                        replan?conditionalEntry.at(gun):leg.entry});
                    auto defended=event["conditionalGunDuties"].toArray();
                    defended.append(QJsonObject{{"unit",gun<n?terrain.units[gun].engineUnitId:NO_UNIT},
                        {"gunIndex",gun},{"firstShot",firstConditionalShot.at(gun)},{"lastObservedDuty",through},
                        {"retainedThrough",end},{"routeSearchIncomplete",limited}});
                    event["conditionalGunDuties"]=defended;
                    if(limited) result.townUncertainGunDuty.insert(gun);
                }
                int claim=-1;
                if(!possibleGuardPosts.empty())
                {
                    claim=static_cast<int>(result.screenClaims.size());
                    // An entry alone is sufficient to reopen the question. A
                    // stationary body must fail even a single installment with
                    // unlimited fuel on every remaining own activation.
                    const int deadline=last-int(last%2!=leg.side);
                    const int movementUpper=stockScreenMovementUpper(pawn.movement,known.fuel);
                    result.screenClaims.push_back({leg.actor,leg.side,pawn.grid,movementUpper,start,deadline,last,1,
                        leg.from,leg.target,pawn.ignoresCollision,possibleGuardPosts});
                }
                for(const auto &[sq,holder,act,tick,stop]:neededPosts)
                    result.townPostDependencies[sq].push_back({leg.actor,holder,act,tick,leg.target,claim,stop});
                // Incomplete search cannot delimit every date protected by a
                // known screen. Its holder must remain until repriced.
                if(limited) for(int sq:possibleGuardPosts)
                    result.townPostDependencies[sq].push_back({leg.actor,NO_UNIT,-1,-1,leg.target,claim,true});
            }
            if(!accepted) decline(limited?"DEFERRED_WORK_LIMIT":interceptBlocked?"DEFERRED_INTERCEPT_BLOCKS_ROUTE":excluded.empty()?"DECLINED_ON_ORDERED_CALENDAR":"DECLINED_EXPOSED_APPROACH");
        }
        if(pricePaid)
        {
            for(TilePoint pt:excludedCaptureSites) if(pt!=INVALID_TILE) targets[cell(pt)]=0;
            std::vector<std::vector<std::pair<int,int>>> busy(n);
            for(int i=0;i<static_cast<int>(trips.size());++i)
                if(result.closures[i].completion>=0) busy[trips[i].actor].emplace_back(result.closures[i].departure,result.closures[i].completion);
            std::vector<std::vector<std::pair<int,TilePoint>>> post(n,std::vector<std::pair<int,TilePoint>>(last+2,{-1,INVALID_TILE}));
            for(int sq=0;sq<width*terrain.height;++sq) for(const auto &r:traffic.uses(sq))
                if(r.actor>=0 && r.actor<n && r.stopped)
                    for(int t=std::max(0,r.first);t<=std::min(last,r.last);++t)
                        if(r.first>=post[r.actor][t].first) post[r.actor][t]={r.first,{sq%width,sq/width}};
            auto at=[&](int actor,int tick) {
                if(positions[actor]==INVALID_TILE || (casualtyDay[actor]>=0 && 2*casualtyDay[actor]<tick)) return INVALID_TILE;
                const auto &dated=post[actor][std::min(tick,last)];return dated.first<0 ? positions[actor] : dated.second;
            };
            // A retained purchase may still require its factory exit even if
            // the original capturer's later job was rejected. Reprove that
            // one action on the settled traffic and debit it exactly once.
            std::vector<int> unknownFuelFrom(n,last+3);
            for(const auto &departure:production.departures)
            {
                const int actor=departure.actor,t=departure.tick;
                if(t<0 || t>last || actor<0 || actor>=n || departure.end==INVALID_TILE ||
                   traveled.contains({actor,t,cell(departure.end)}))continue;
                const auto &pawn=terrain.units[actor];
                const int grid=pawn.grid>=0?pawn.grid:pawn.travelGrid;
                auto before=t?at(actor,t-1):positions[actor];int remaining=fuel[actor];
                if(remaining>=0)for(const auto &event:history[actor])if(event.tick<t)remaining-=event.fuelUsed;
                if(grid>=0 && before!=INVALID_TILE && paidWork>0 && (remaining>=0 || fuel[actor]<0))
                {
                    auto path=stockCaptureRoute(terrain.grids[grid],traffic,actor,pawn.enemy,before,departure.end,pawn.movement,
                        remaining,t,t,t,1,paidWork,INVALID_TILE,&lower,{},pawn.ignoresCollision);
                    if(path && path->entryTick==t)
                    {
                        history[actor].push_back({t,-1,path->fuel,-1,false});continue;
                    }
                }
                unknownFuelFrom[actor]=std::min(unknownFuelFrom[actor],t+1);
            }
            std::vector<std::vector<PaidStockResources>> dated(n);
            for(int i=0;i<n;++i)
            {
                auto &events=history[i];
                std::stable_sort(events.begin(),events.end(),[](const auto &a,const auto &b){return a.tick<b.tick;});
                PaidStockResources state{real[i],fuel[i],load[i],false};
                auto event=events.begin();
                for(int t=0;t<=last+2;++t)
                {
                    while(event!=events.end() && event->tick<t)
                    {
                        if(event->hpAfter>=0)state.hp=event->hpAfter;
                        if(state.fuel>=0)state.fuel=std::max(0,state.fuel-event->fuelUsed);
                        spendCombatRound(state.ammo,event->weapon);
                        ++event;
                    }
                    if(at(i,t)==INVALID_TILE)state.hp=0;
                    dated[i].push_back(state);
                }
            }
            const auto gunDue=[&](int i,int t){
                const auto p=result.townGunDutyThrough.find(i);return p!=result.townGunDutyThrough.end() && p->second>=t;
            };
            const auto screenDue=[&](int i,int t){
                const auto tile=at(i,t);
                if(tile==INVALID_TILE || !protectedPosts.contains(cell(tile)))return false;
                const auto d=result.townPostDependencies.find(cell(tile));
                return d==result.townPostDependencies.end() || std::any_of(d->second.begin(),d->second.end(),[&](const auto &dep){
                    return (dep.holder==NO_UNIT || dep.holder==i) && (dep.occupiedTick<0 || dep.occupiedTick>=t);
                });
            };
            auto free=[&](int actor,int tick) {
                if(actor>=n || tick>last || dated[actor][tick].hp<=0 || actor==reservedGuard ||
                   (2*lastShot[actor]+int(terrain.units[actor].enemy)>=tick)) return false;
                for(const auto &d:production.departures)if(d.actor==actor && d.tick==tick)return false;
                for(const auto &e:history[actor])if(e.tick==tick && e.fired)return false;
                for(auto [start,finish]:busy[actor]) if(start<=tick && tick<=finish) return false;
                return true;
            };
            auto future=[&](int actor,int tick) {
                for(auto [start,finish]:busy[actor]) if(finish>tick) return true;
                for(const auto &e:history[actor])if(e.tick>tick)return true;
                const auto departure=std::any_of(production.departures.begin(),production.departures.end(),[&](const auto &d){
                    return d.actor==actor && d.tick>tick && d.tick<=last;});
                return actor==reservedGuard || gunDue(actor,tick) || screenDue(actor,tick) || departure ||
                    (casualtyDay[actor]>=0 && 2*casualtyDay[actor]>=tick);
            };
            auto usedPaid=[&](int gun,int tick){return result.paidHits.contains({gun,tick});};
            auto ready=rights;
            for(int i=0;i<n;++i) if(positions[i]!=INVALID_TILE && real[i]>0 && i!=reservedGuard)
                ready[i]={2*std::max(lastShot[i]+1,alternative[i]>0 ? 2:1)+int(terrain.units[i].enemy),
                          fuel[i],true,alternative[i]};
            PaidCaptureDefenseView view{terrain,units,traffic,positions,real,load,ready,production.fundedGuns,at,free,future,usedPaid,&defense.minimumEntryCost};
            view.stockResources=[&](int actor,int t){
                auto state=dated[actor][std::clamp(t,0,last+2)];
                state.mayRelocate=ready[actor].mayRelocate;
                if(t>=unknownFuelFrom[actor]){state.fuel=-1;state.fuelKnown=false;}
                return state;
            };
            view.conditionalGun=gunDue;
            PaidDuelPositions fundedPositions;view.fundedPositions=&fundedPositions;
            view.fundedScreen=[&](int gun,int sq,int t){
                if(!protectedPosts.contains(sq))return false;
                const auto dependency=result.townPostDependencies.find(sq);
                return dependency==result.townPostDependencies.end() || std::any_of(dependency->second.begin(),dependency->second.end(),
                    [&](const auto &d){return (d.holder==NO_UNIT || d.holder==n+gun) && (d.occupiedTick<0 || d.occupiedTick==t);});
            };
            view.fundedCommitment=[&](int gun,int t){
                if(result.fundedFollowups.contains(gun) || gunDue(n+gun,t))return true;
                return std::any_of(result.paidHits.begin(),result.paidHits.end(),[&](const auto &s){
                    return s.first.first==gun && s.first.second<=t;
                });
            };
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            QJsonArray originalRangeChecks;
            if(qEnvironmentVariableIntValue("COW_AI_ARENA_TEST")==1 && qEnvironmentVariableIntValue("COW_COMBAT_REPRICE_ORDERS")==1 &&
               qEnvironmentVariableIntValue("COW_PAID_RECRUIT_TRACE")==1)
            {
                auto audit=view;auto snapshot=result;auto auditWork=paidWork;auto privateBounds=lower;
                auto minima=defense.minimumEntryCost;PaidDuelPositions privatePosts;
                audit.gridMinima=&minima;audit.fundedPositions=&privatePosts;audit.originalRangeAudit=true;
                findPaidCaptures(audit,production,captureSites,protectedPosts,targets,privateBounds,last,horizon,threshold,snapshot,auditWork);
                originalRangeChecks=snapshot.paidCaptureChecks;
            }
#endif
            findPaidCaptures(view,production,captureSites,protectedPosts,targets,lower,last,horizon,threshold,result,paidWork);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            if(!originalRangeChecks.isEmpty())
                result.paidCaptureChecks.append(QJsonObject{{"audit","ORIGINAL_RANGE_RECHECK"},{"checks",originalRangeChecks}});
#endif
        }
        for (std::size_t j=0;j<trips.size();++j)
            if (result.closures[j].entry>=0) result.townBusyStock.insert(trips[j].actor);
        // A proved departure spends one activation. Preserve the original purchase and offer the body only after the actual exit.
        for (const auto &move:production.departures)
        {
            const int i=move.actor,at=move.tick;
            if(i<0 || i>=n)continue;
            const bool otherWork=result.townBusyStock.contains(i) || spentGuns.contains(i) || i==reservedGuard || casualtyDay[i]>=0;
            result.townBusyStock.insert(i);
            if(otherWork || startDeparture[i]!=at || at<=0 || at>last-2 || real[i]<=0 || positions[i]==INVALID_TILE ||
               departureCount[i]!=1)continue;
            const auto &pawn=terrain.units[i];const int side=int(pawn.enemy),grid=pawn.grid>=0?pawn.grid:pawn.travelGrid;
            if(at%2!=side || 2*lastShot[i]+side>=at || grid<0 || grid>=static_cast<int>(terrain.grids.size()) ||
               terrain.grids[grid].width()!=width || terrain.grids[grid].height()!=terrain.height ||
               manhattanDistance(positions[i],move.end)!=1 || move.end.x<0 || move.end.y<0 || move.end.x>=width || move.end.y>=terrain.height)continue;
            const int tax=terrain.grids[grid].cost(move.end.x,move.end.y,entryDirection(positions[i].x,positions[i].y,move.end.x,move.end.y));
            if(tax<0 || tax>pawn.movement || (fuel[i]>=0 && tax>fuel[i]))continue;
            const auto &old=traffic.uses(cell(positions[i]));
            if(std::none_of(old.begin(),old.end(),[&](const auto &u){return u.actor==i && u.side==side && u.stopped && u.first<=at-1 && u.last==at-1;}) ||
               !traffic.open(i,side,cell(move.end),at,true))continue;
            const auto &post=traffic.uses(cell(move.end));
            if(std::none_of(post.begin(),post.end(),[&](const auto &u){return u.actor==i && u.side==side && u.stopped && u.first==at && u.last>=last;}))continue;
            StockCaptureRelease released{move.end,at+2,fuel[i]<0?fuel[i]:fuel[i]-tax,real[i],load[i],protectedPosts.contains(cell(move.end))};
            const bool paidReaction=std::any_of(result.paidCaptures.begin(),result.paidCaptures.end(),[&](const auto &job){
                return std::any_of(job.survivor.shots.begin(),job.survivor.shots.end(),[&](const auto &shot){return shot.gun==i;});
            });
            if(stockCapturePostCanDepart(traffic,i,side,cell(move.end),released,last,paidReaction))result.townAfterFactoryExit.emplace(i,released);
        }
        for (const auto gun:spentGuns)
        {
            if (gun<n) result.townBusyStock.insert(gun);
            else result.townBusyPaid.insert(gun-n);
        }
        for (const auto &[key,shot]:result.paidHits) result.townBusyPaid.insert(key.first);
        for (const auto &[gun,follows]:result.fundedFollowups) result.townBusyPaid.insert(gun);
        // A repurposed capper cannot abandon another booked task, a required
        // screen or a later casualty priced in the original calendar. An
        // earlier accepted capture remains owned when the body leaves it.
        for(auto &[unit,r]:result.townAfterCapture) r.protectsPost=protectedPosts.contains(cell(r.tile));
        std::erase_if(result.townAfterCapture,[&](const auto &entry) {
            const auto &[actor,r]=entry;
            const auto &pawn=terrain.units[actor];const int post=cell(r.tile);
            const bool laterDeparture=std::any_of(production.departures.begin(),production.departures.end(),[&](const auto &d){
                return d.actor==actor && d.tick>=r.readyTick;
            });
            const bool obligation=actor==reservedGuard || casualtyDay[actor]>=0 || spentGuns.contains(actor) ||
                laterDeparture || 2*lastShot[actor]+int(pawn.enemy)>=r.readyTick;
            return !stockCapturePostCanDepart(traffic,actor,int(pawn.enemy),post,r,last,obligation);
        });
        result.townProtectedPosts=std::move(protectedPosts);
        result.townTraffic.emplace(std::move(traffic));
        return result;
    }
    inline bool installPaidCaptureOrder(ProductionProjection &portfolio,const PaidCaptureCandidate &job,
        const CombatTerrain &terrain,int threshold)
    {
        if(job.finish<0 || job.path.steps.empty() || job.survivor.hp<=0 || job.survivor.carry<threshold) return false;
        const auto &g=portfolio.fundedGuns[job.gun];const auto square=[](TilePoint t){return QJsonArray{t.x,t.y};};
        QJsonArray route,shots;
        for(const auto &step:job.path.steps)
        {
            QJsonArray crossed;for(int cell:step.cells) crossed.append(square({cell%terrain.width,cell/terrain.width}));
            route.append(QJsonObject{{"tick",step.tick},{"tile",square(step.tile)},{"cells",crossed}});
        }
        for(const auto &s:job.survivor.shots)
        {
            QJsonArray moved;for(int sq:s.motion.cells) moved.append(square({sq%terrain.width,sq/terrain.width}));
            const int n=static_cast<int>(terrain.units.size());
            QJsonObject witness{{"movedFrom",square(s.motion.origin)},{"movementFuel",s.motion.fuel},{"movementCells",moved},
                {"gun",s.gun<n?terrain.units[s.gun].engineUnitId:NO_UNIT},{"from",square(s.from)},{"tick",s.shotTick},
                {"hpBefore",s.targetBefore},{"hpAfter",s.targetAfter},{"shooterHpAfter",s.shooterAfter},
                {"weapon",s.weapon},{"counterWeapon",s.counterWeapon},{"damage",static_cast<qint64>(s.damageValue)},{"counter",static_cast<qint64>(s.counterValue)}};
            if(s.gun>=n)
            {
                if(s.gun-n>=static_cast<int>(portfolio.fundedGuns.size()))return false;
                const auto &foe=portfolio.fundedGuns[s.gun-n];witness["birthSite"]=square(foe.factory);witness["birthTick"]=foe.buildTick;
                if(!foe.unit)return false;
                witness["recruit"]=foe.unit->type;witness["enemy"]=bool(foe.side);
                const auto present=std::any_of(portfolio.orders.begin(),portfolio.orders.end(),[&](const auto &v){
                    const auto order=v.toObject();
                    return order["factory"]==witness["birthSite"] && order["buildHalfTurn"]==witness["birthTick"] &&
                           order["kind"]!="CAPTURE" && order["paidCaptureDefenseUses"].toArray().isEmpty();
                });
                if(!present)return false;
                bool matched=false;
                for(const auto &v:job.paidDuelUses)
                {
                    const auto check=v.toObject();if(check["index"]!=s.gun-n || check["tick"]!=s.shotTick)continue;
                    matched=true;
                    witness["loan"]=check["loan"];
                    witness["committedStop"]=check["committedStop"];witness["returnStop"]=check["nextStop"];
                    witness["outbound"]=check["out"];witness["returnPath"]=check["back"];break;
                }
                if(!matched)return false;
            }
            shots.append(witness);
        }
        for(int i=0;i<portfolio.orders.size();++i)
        {
            auto o=portfolio.orders[i].toObject();
            if(o["factory"].toArray()!=square(g.factory) || o["buildHalfTurn"].toInt()!=g.buildTick) continue;
            if(!o["paidCaptureDefenseUses"].toArray().isEmpty())return false;
            if(o["conditionedFundedPost"].toBool() && (job.path.steps.front().tick!=g.buildTick+2 ||
               square(job.path.steps.front().tile)!=o["firstStop"].toArray() || job.entry<=g.buildTick+2))return false;
            o["kind"]="CAPTURE";o["firstStop"]=square(job.path.steps.front().tile);o["end"]=square(job.property);
            o["impactHalfTurn"]=job.finish;o["arrivalHalfTurn"]=job.entry;o["target"]=-1;o["targetTile"]=QJsonArray{};
            o["reliefExit"]=square(INVALID_TILE);o["delayedCaptureHalfTurn"]=-1;o["net"]=0;o["exposure"]=0;o["targetDamage"]=0;
            o.remove("retainedDeployment");o.remove("relocatedForForecastOccupant");
            o["paidCaptureProof"]=QJsonObject{{"status","WITNESSED"},{"stops",route},{"fuelSpent",job.path.fuel},
                {"shots",shots},{"pursuitChecks",job.pursuitChecks},{"defenderResources",job.defenderResources},{"paidDuelChecks",job.paidDuelChecks},
                {"captureInstallments",job.installments},{"entryTick",job.entry},{"actualCompletionTick",job.finish},
                {"finalCarry",job.survivor.carry},{"captureThreshold",threshold},{"hpAfter",job.survivor.hp}};
            portfolio.orders[i]=o;
            for(const auto &v:shots)
            {
                const auto s=v.toObject();if(!s.contains("birthSite"))continue;
                for(int j=0;j<portfolio.orders.size();++j)
                {
                    auto defender=portfolio.orders[j].toObject();
                    if(defender["factory"]!=s["birthSite"] || defender["buildHalfTurn"]!=s["birthTick"])continue;
                    auto duties=defender["paidCaptureDefenseUses"].toArray();duties.append(s);defender["paidCaptureDefenseUses"]=duties;
                    portfolio.orders[j]=defender;break;
                }
            }
            return true;
        }
        return false;
    }

    inline void releaseCanceledIntercept(const CombatTerrain &terrain,ProductionProjection &p,int index,
        const StockCaptureSettlement &settled,QJsonObject &order)
    {
        auto &gun=p.fundedGuns[index];
        const int n=static_cast<int>(terrain.units.size());
        if(gun.targetUnit<0 || gun.targetUnit>=n || settled.townBusyPaid.contains(index) ||
           !order["paidCaptureDefenseUses"].toArray().isEmpty())return;
        const int oldImpact=gun.impactTick,enemy=terrain.units[gun.targetUnit].engineUnitId;
        int duty=-1;
        if(auto d=settled.townGunDutyThrough.find(n+index);d!=settled.townGunDutyThrough.end())duty=d->second;
        bool known=false,unfinished=false;
        const auto target=QJsonArray{gun.targetTile.x,gun.targetTile.y};
        for(const auto &v:settled.proofs)
        {
            const auto proof=v.toObject();
            if(proof["unit"].toInt(NO_UNIT)!=enemy || proof["tile"].toArray()!=target)continue;
            known=true;
            unfinished|=proof["status"].toString().startsWith("DEFERRED");
        }
        // A search limit is not a proved cancellation before the old date.
        // A priced deterrent also retains its last required availability.
        if(!known || unfinished)duty=std::max(duty,oldImpact);
        const int first=duty+1;
        gun.readyTick=std::max(gun.readyTick,first+int(first%2!=gun.side));
        gun.exposure=gun.exposureBeforeFiring.value_or(gun.exposure);
        gun.targetUnit=NO_UNIT;gun.targetTile=INVALID_TILE;gun.impactTick=-1;
        order["canceledInterceptTarget"]=enemy;order["canceledInterceptTile"]=target;
        order["canceledInterceptTick"]=oldImpact;order["reserveReadyTick"]=gun.readyTick;
        order["reserveExposure"]=static_cast<qint64>(gun.exposure);
    }

    inline bool settleFundedOrders(const CombatTerrain &terrain,ProductionProjection &p,
        std::span<const ProductionProperty> properties,const StockCaptureSettlement &settled,
        int last,int horizon,QJsonObject *failure=nullptr)
    {
        if(!terrain.production) return p.orders.empty();
        auto &board=*terrain.production;
        auto point=[](const QJsonArray &xy){return TilePoint{xy[0].toInt(),xy[1].toInt()};};
        p.flow.assign(horizon,0);p.net=0;
        std::vector<const ProductionProperty*> buildings(terrain.width*terrain.height,nullptr);
        for(const auto &col:properties) buildings[col.tile.y*terrain.width+col.tile.x]=&col;
        struct NewTitle {int tick;OwnerSign owner;};
        std::map<int,NewTitle> boughtCaptures;
        auto title=[&](const ProductionProperty &col,int tick) {
            const auto base=productionOwnerAt(col,tick);const int square=col.tile.y*terrain.width+col.tile.x;
            auto own=boughtCaptures.find(square);
            if(own==boughtCaptures.end() || own->second.tick>=tick) return base;
            for(const auto &stock:col.ownershipChanges)
                if(stock.completionTick>own->second.tick && stock.completionTick<tick) return base;
            return own->second.owner;
        };
        const auto noTile=QJsonArray{-1,-1};
        QJsonArray orders;
        for(const auto &item:p.orders)
        {
            auto o=item.toObject();
            const int bought=o["buildHalfTurn"].toInt(),impact=o["impactHalfTurn"].toInt(),side=o["enemy"].toBool();
            if(bought>last) continue;
            const auto own=side?OwnerSign::Enemy:OwnerSign::Ours;
            auto kind=o["kind"].toString();
            MilliFunds material=o["targetDamage"].toInteger()-o["exposure"].toInteger();
            MilliFunds net=0;
            if(kind=="INTERCEPT_CAPTURE")
            {
                bool hit=false;
                for(const auto &[key,shot]:settled.paidHits)
                {
                    const auto &gun=p.fundedGuns[key.first];
                    if(impact!=key.second || gun.buildTick!=bought || gun.factory!=point(o["factory"].toArray())) continue;
                    hit=true;material=shot.damageValue-shot.counterValue;
                    o["targetDamage"]=static_cast<qint64>(shot.damageValue);o["exposure"]=static_cast<qint64>(shot.counterValue);
                    // The stock cash calendar already uses the observed post-shot finish.
                    for(std::size_t i=0;i<settled.proofs.size();++i)
                    {
                        const auto ref=settled.proofs[static_cast<int>(i)].toObject();
                        if(ref["status"]=="WITNESSED" && ref["unit"]==o["target"] && ref["tile"]==o["targetTile"])
                            o["delayedCaptureHalfTurn"]=ref["actualCompletionTick"];
                    }
                    break;
                }
                if(!hit)
                {
                    kind="DEPLOY";
                    const auto site=point(o["factory"].toArray());
                    for(int i=0;i<static_cast<int>(p.fundedGuns.size());++i)
                    {
                        const auto &gun=p.fundedGuns[i];
                        if(gun.side==side && gun.buildTick==bought && gun.factory==site && gun.impactTick==impact)
                        {releaseCanceledIntercept(terrain,p,i,settled,o);break;}
                    }
                }
            }
            if(kind=="CAPTURE" && impact<=last)
            {
                const auto dest=point(o["end"].toArray());const int sq=dest.y*terrain.width+dest.x;
                if(!buildings[sq] || title(*buildings[sq],impact+1)==own) kind="DEPLOY";
                else
                {
                    const auto &col=*buildings[sq];
                    boughtCaptures[sq]={impact,own};
                    for(const auto &wound:o["paidCaptureProof"].toObject()["shots"].toArray())
                    {
                        const auto shot=wound.toObject();const int tick=shot["tick"].toInt();
                        const auto loss=shot["damage"].toInteger()-shot["counter"].toInteger();
                        if(tick>0 && tick<=last) {p.flow[(tick-1)/2]+=(side?1:-1)*loss;net-=loss;}
                    }
                    for(int tick=impact+1;tick<=last;++tick)
                    {
                        const auto before=productionOwnerAt(col,tick),after=title(col,tick);
                        const int who=tick%2;const auto paying=who ? OwnerSign::Enemy : OwnerSign::Ours;
                        const auto change=(int(after==paying)-int(before==paying))*(who ? -col.column.income.enemyPerTurn : col.column.income.oursPerTurn);
                        p.flow[(tick-1)/2]+=change;net+=side ? -change : change;
                    }
                }
            }
            if(impact>last || kind=="DEPLOY")
            {
                kind="DEPLOY";material=0;o["retainedDeployment"]=true;o["target"]=-1;o["targetTile"]=QJsonArray{};
                o["targetDamage"]=0;o["exposure"]=0;o["delayedCaptureHalfTurn"]=-1;
            }
            for(const auto &[gunIndex,extra]:settled.fundedFollowups)
            {
                const auto &gun=p.fundedGuns[gunIndex];
                if(gun.buildTick==bought && gun.factory==point(o["factory"].toArray())) o["otherPricedShots"]=extra;
            }
            if(impact>0 && impact<=last) p.flow[(impact-1)/2]+=(side?-1:1)*material;
            net+=material;p.net+=(side?-1:1)*net;o["kind"]=kind;o["net"]=static_cast<qint64>(net);orders.append(o);
        }
        // Use exactly the same completed ownership events in the payment and
        // the scored receipts. Keep every committed body and charge its price.
        auto banks=board.funds;int prior=0;
        for(int i=0;i<orders.size();++i)
        {
            auto o=orders[i].toObject();const int tick=o["buildHalfTurn"].toInt(),who=tick%2;
            for(int payday=prior+1;payday<=tick;++payday)
            {
                const int s=payday%2;const auto owner=s ? OwnerSign::Enemy : OwnerSign::Ours;
                for(const auto &col:properties) if(title(col,payday)==owner)
                    banks[s]+=s?col.column.income.enemyPerTurn:col.column.income.oursPerTurn;
                if(payday==2 && p.friendlyFirstRepairs && !settleOpeningRepairDebit(*p.friendlyFirstRepairs,MILLI_FUNDS_PER_FUND,banks[0])){
                    if(failure)*failure={{"phase","FINAL_FUNDS"},{"reason","F1_SERVICE_WALLET_CHANGED"},{"tick",payday}};
                    return false;
                }
                if(payday==1 && p.enemyOpeningRepairs && !settleOpeningRepairDebit(*p.enemyOpeningRepairs,MILLI_FUNDS_PER_FUND,banks[s])){
                    if(failure)*failure=QJsonObject{{"phase","FINAL_FUNDS"},{"reason","OPENING_REPAIR_WALLET_CHANGED"},{"tick",payday},{"funds",static_cast<qint64>(banks[s])}};
                    return false;
                }
            }
            prior=tick;const auto price=o["price"].toInteger();const auto fac=point(o["factory"].toArray());
            const auto *shop=buildings[fac.y*terrain.width+fac.x];
            if(!shop || title(*shop,tick+1)!=(who?OwnerSign::Enemy:OwnerSign::Ours) || banks[who]<price)
            {
                if(failure) *failure=QJsonObject{{"phase","FINAL_FUNDS"},{"reason",!shop?"MISSING_SITE":
                    title(*shop,tick+1)!=(who?OwnerSign::Enemy:OwnerSign::Ours)?"OWNER_CHANGED":"INSUFFICIENT_FUNDS"},
                    {"side",who},{"tick",tick},{"site",QJsonArray{fac.x,fac.y}},{"price",price},{"funds",static_cast<qint64>(banks[who])}};
                return false;
            }
            o["treasuryBefore"]=static_cast<qint64>(banks[who]);banks[who]-=price;o["treasuryAfter"]=static_cast<qint64>(banks[who]);orders[i]=o;
        }
        if(prior<1 && p.enemyOpeningRepairs){
            for(const auto &col:properties)if(title(col,1)==OwnerSign::Enemy)banks[1]+=col.column.income.enemyPerTurn;
            if(!settleOpeningRepairDebit(*p.enemyOpeningRepairs,MILLI_FUNDS_PER_FUND,banks[1])){
                if(failure)*failure=QJsonObject{{"phase","FINAL_FUNDS"},{"reason","OPENING_REPAIR_WALLET_CHANGED"},{"tick",1}};
                return false;
            }
        }
        if(last>=2 && prior<2 && p.friendlyFirstRepairs){
            for(const auto &col:properties)if(title(col,2)==OwnerSign::Ours)banks[0]+=col.column.income.oursPerTurn;
            if(!settleOpeningRepairDebit(*p.friendlyFirstRepairs,MILLI_FUNDS_PER_FUND,banks[0])){
                if(failure)*failure={{"phase","FINAL_FUNDS"},{"reason","F1_SERVICE_WALLET_CHANGED"},{"tick",2}};
                return false;
            }
        }
        p.orders=std::move(orders);return true;
    }

}
