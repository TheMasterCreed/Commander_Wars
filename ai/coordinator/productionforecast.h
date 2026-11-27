#pragma once

#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/openingrepairbill.h"
#include "ai/coordinator/friendlydaystart.h"
#include "ai/coordinator/stockcaptureroute.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/propertystockfield.h"
#include <numeric>
#include <QJsonArray>
#include <QJsonObject>

namespace Coordinator
{
    // An uncommitted target can decline to end its turn in an indirect unit's
    // range. A lane only supplies a forecast shot if this escape test fails or
    // another modeled action actually commits the target to that endpoint.
    inline std::optional<bool> indirectTargetCanEscape(const CombatTerrain &terrain,
        std::span<const TilePoint> positions, std::span<const int> hp, int unit,
        TilePoint gun, int minimum, int maximum, std::int64_t *optionalBudget=nullptr)
    {
        auto pay=[&](std::int64_t amount) {
            if(optionalBudget && *optionalBudget<amount) return false;
            if(!terrain.pay(amount)) return false;
            if(optionalBudget) *optionalBudget-=amount;
            return true;
        };
        const auto &pawn=terrain.units[unit];
        if (pawn.movement<=0) return false;
        if (pawn.grid<0) return true;
        const auto from=positions[unit];
        if(!pay(terrain.width*terrain.height+static_cast<std::int64_t>(positions.size()))) return {};
        std::vector<bool> occupied(terrain.width*terrain.height);
        std::vector<TilePoint> opposing;
        for (int i=0;i<static_cast<int>(positions.size());++i)
            if(i!=unit && positions[i]!=INVALID_TILE && hp[i]>0)
            {
                occupied[positions[i].y*terrain.width+positions[i].x]=true;
                if(terrain.units[i].enemy!=pawn.enemy) opposing.push_back(positions[i]);
            }
        const auto &grid=terrain.grids[pawn.grid];
        auto outside=[&](TilePoint t) {
            const int d=manhattanDistance(t,gun);
            return d<minimum || d>maximum;
        };
        for(const auto dir:ENTRY_DIRECTIONS)
        {
            const auto step=entryStep(dir);
            const TilePoint to{from.x+step.dx,from.y+step.dy};
            const int cost=grid.cost(to.x,to.y,dir);
            if(cost>=0 && cost<=pawn.movement && outside(to) && !occupied[to.y*terrain.width+to.x])
                return true;
        }
        if(!pay(3LL*terrain.width*terrain.height)) return {};
        std::vector<TilePoint> exits;
        for(int y=0;y<terrain.height;++y)
            for(int x=0;x<terrain.width;++x)
            {
                TilePoint t{x,y};
                if(outside(t) && !occupied[y*terrain.width+x]) exits.push_back(t);
            }
        ActivationField field;
        field.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(opposing),from,pawn.movement,1,exits);
        return std::any_of(exits.begin(),exits.end(),[&](TilePoint t){return field.activations(t.x,t.y)==1;});
    }

    struct ReinforcementRoute
    {
        TilePoint destination, firstStop;
        int activations{};
        std::vector<int> cells;
        std::vector<TilePoint> exits;
        std::vector<int> overnightStops;
    };

    // Keep one directional route, including the actual first overnight stop.
    // A route that a reply obstructs can be declined without searching the map again.
    inline std::vector<ReinforcementRoute> reinforcementRoutes(const MobilityCostGrid &grid, TilePoint origin, int mp,
        int horizon = 2, int fuel = std::numeric_limits<int>::max())
    {
        if (mp <= 0 || !grid.onGrid(origin.x, origin.y))
            return {};
        const int width = grid.width(), count = width * grid.height();
        const int from = origin.y * width + origin.x;
        using Label = std::pair<int,int>;
        std::vector<Label> best(count, {horizon+1,0});
        std::vector<int> fuelSpent(count,0);
        std::vector<int> previous(count, -1), overnight(count, from);
        using Item = std::tuple<int,int,int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        best[from] = {1,0};
        heap.emplace(1,0,from);
        while (!heap.empty())
        {
            const auto [turn, spent, cell] = heap.top();
            heap.pop();
            if (best[cell] != Label{turn,spent})
                continue;
            for (const auto dir : ENTRY_DIRECTIONS)
            {
                const auto delta = entryStep(dir);
                const int x = cell % width + delta.dx, y = cell / width + delta.dy;
                const int cost = grid.cost(x, y, dir);
                if (cost < 0 || cost > mp)
                    continue;
                const bool stop = cost > mp - spent;
                const Label next{turn + int(stop), stop ? cost : spent + cost};
                const int to = y * width + x;
                if (next.first > horizon || cost > fuel-fuelSpent[cell] || next >= best[to])
                    continue;
                best[to] = next;
                previous[to] = cell;
                fuelSpent[to]=fuelSpent[cell]+cost;
                overnight[to] = stop && turn==1 ? cell : overnight[cell];
                heap.emplace(next.first, next.second, to);
            }
        }
        std::vector<ReinforcementRoute> routes;
        for (int i = 0; i < count; ++i)
        {
            if (best[i].first > horizon)
                continue;
            const auto stop = best[i].first == 1 ? i : overnight[i];
            ReinforcementRoute route{{i%width,i/width},{stop%width,stop/width},best[i].first,{}};
            for (int j = i; j != from; j = previous[j])
            {
                route.cells.push_back(j);
                if(best[j].first>best[previous[j]].first)
                    route.overnightStops.push_back(previous[j]);
            }
            for (const auto dir : ENTRY_DIRECTIONS)
            {
                const auto step = entryStep(dir);
                const TilePoint next{route.destination.x + step.dx, route.destination.y + step.dy};
                const int cost = grid.cost(next.x, next.y, dir);
                if (cost >= 0 && cost <= mp && cost<=fuel-fuelSpent[i])
                    route.exits.push_back(next);
            }
            routes.push_back(std::move(route));
        }
        std::stable_sort(routes.begin(),routes.end(),[](const auto &a,const auto &b) {
            return std::pair{a.activations,a.cells.size()}<std::pair{b.activations,b.cells.size()};
        });
        return routes;
    }

    struct ProductionChoice
    {
        QString type;
        MilliFunds cost{};
        bool mobileFire{};
        int minRange{}, maxRange{};
        std::vector<ReinforcementRoute> routes;
        std::map<int,int> captureRate;
        std::vector<double> attack, incoming;
        std::vector<int> routeAt;
        std::vector<TerrainDefenseRule> defense;
        int guaranteedShots{1};
        std::vector<int> incomingSlot;
        std::shared_ptr<const MobilityCostGrid> movementGrid;
        int movement{},initialFuel{-1},minimumMoveCost{};
        std::array<int,2> captureLoad{};
        std::vector<std::array<double,2>> captureCounter,againstCapper;
        bool ignoresUnitCollision{};
        double startingHp{10.};
        QString screeningMovementType;
        int screeningMovement{},screeningRange{};
        std::array<QString,2> screeningWeapons;
        bool transportCapacity{},supportsUnits{};
    };
    inline int remainingDeploymentFuel(const ProductionChoice &unit,TilePoint from,const ReinforcementRoute &route,int width)
    {
        int fuel=unit.initialFuel;
        if(fuel<0 || !unit.movementGrid) return fuel;
        for(auto at=route.cells.rbegin();at!=route.cells.rend();++at)
        {
            const TilePoint next{*at%width,*at/width};
            fuel-=std::max(0,unit.movementGrid->cost(next.x,next.y,entryDirection(from.x,from.y,next.x,next.y)));
            from=next;
        }
        return std::max(0,fuel);
    }

    struct PaidObservedService
    {
        int departure{},shot{},returned{},nextReady{};
        double hpAfterFirst{},hpAfterReply{};
        int fuelAfterReturn{};
        std::array<int,2> ammoAfterFirst{},ammoAfterReply{};
    };
    struct FundedCaptureGun
    {
        const ProductionChoice *unit;
        TilePoint factory,destination;
        int side,buildTick,readyTick,impactTick,targetUnit;
        MilliFunds exposure;
        TilePoint targetTile;
        int fuelAtPost{-1};
        // An intercept may never be fired. Keep its independent approach
        // exposure separate from the counter to that particular proposed shot.
        std::optional<MilliFunds> exposureBeforeFiring;
        std::optional<PaidObservedService> observedService;
    };
    inline double paidHpAfterService(const FundedCaptureGun &g,int tick,double before)
    {
        if(!g.observedService || tick<g.observedService->shot)return before;
        return tick==g.observedService->shot?g.observedService->hpAfterFirst:g.observedService->hpAfterReply;
    }
    inline std::array<int,2> paidLoadAfterService(const FundedCaptureGun &g,int tick,std::array<int,2> before)
    {
        if(!g.observedService || tick<g.observedService->shot)return before;
        return tick==g.observedService->shot?g.observedService->ammoAfterFirst:g.observedService->ammoAfterReply;
    }
    inline int paidFuelAtRelease(const FundedCaptureGun &g,int tick)
    {
        return g.observedService && tick>=g.observedService->returned?g.observedService->fuelAfterReturn:g.fuelAtPost;
    }
    struct ProductionSite
    {
        TilePoint tile;
        std::array<std::vector<ProductionChoice>,2> menu;
        std::array<MilliFunds,2> minimumBuildCost{};
    };
    struct FixedProductionPurchase
    {
        TilePoint factory;
        QString type;
        MilliFunds price{};
    };
    struct NativeContactPiece
    {
        int stock{NO_UNIT},side{};
        QString type;
        TilePoint at{INVALID_TILE};
        int hpTenths{};
        std::array<int,2> ammo{};
        friend bool operator<(const NativeContactPiece &a,const NativeContactPiece &b)
        {
            return std::tie(a.stock,a.side,a.type,a.at.x,a.at.y,a.hpTenths,a.ammo)<
                   std::tie(b.stock,b.side,b.type,b.at.x,b.at.y,b.hpTenths,b.ammo);
        }
    };
    struct NativeContactDamage
    {
        double attack{},counter{-1.};
        int weapon{-1},counterWeapon{-1};
    };
    struct ProductionForecastBoard
    {
        std::array<MilliFunds,2> funds{};
        std::array<int,2> availableUnits{};
        std::vector<ProductionSite> sites;
        // Empty and engaged means the team kept its current treasury. An
        // unengaged value allows the provisional portfolio to be proposed.
        mutable std::optional<std::vector<FixedProductionPurchase>> friendlyOpening;
        std::function<std::optional<FriendlyServiceResult>(const ProductionForecastBoard&,const CombatTerrain&,
            const std::set<std::pair<int,int>>&,std::span<const TilePoint>,std::span<const double>,std::span<const int>,
            std::span<const std::array<int,2>>,const OpeningRecruitFight*,MilliFunds,std::int64_t&,QString&)> friendlyDayStart;
        mutable TilePoint resolvedOpeningFireSite{INVALID_TILE};
        // Opening bids which cannot be certified against E0 must not displace
        // suitable purchases at other sites before the portfolio is frozen.
        mutable std::function<bool(const ProductionSite&,const ProductionChoice&)> canProposeOpening;
        // Native, slot-specific base damage for two actual prospective units.
        std::function<std::array<double,2>(const FundedCaptureGun&,const FundedCaptureGun&,int)> paidWeaponSlots;
        // A funded town occupant uses the actual stock type, range and remaining weapon slot.
        std::function<std::optional<NativeContactDamage>(const NativeContactPiece&,const NativeContactPiece&,std::int64_t&)> contactDamage;
        using StockPairPowers=std::array<std::array<double,2>,2>;
        std::function<std::optional<StockPairPowers>(const FundedCaptureGun&,int,int)> paidStockSlots;
        std::set<std::pair<int,int>> matureFactoryCells;
        // Terrain-only one-action upper envelopes are shared by all trial
        // replies on this board. Ignoring traffic makes exposure conservative.
        mutable std::map<std::tuple<int,int,int,int,int,int>,std::vector<bool>> attackEnvelope;
    };

    inline const std::vector<bool> &reinforcementExposure(const CombatTerrain &terrain, const KnownUnit &unit,
                                                          const CombatPawn &pawn, TilePoint from)
    {
        requireTownOwnerState("MUTABLE_REINFORCEMENT_EXPOSURE");
        const int width=terrain.width, height=terrain.height;
        const int mp=unit.canMoveAndFire ? pawn.movement : 0;
        auto key=std::tuple{pawn.grid,from.y*width+from.x,mp,unit.minRange,unit.maxRange,int(pawn.ignoresCollision)};
        auto [it,fresh]=terrain.production->attackEnvelope.try_emplace(key);
        if(!fresh) return it->second;
        auto &covered=it->second;
        covered.assign(width*height,false);
        std::vector<TilePoint> origins{from};
        if (mp>0 && pawn.grid>=0)
        {
            for (const auto &route:reinforcementRoutes(terrain.grids[pawn.grid],from,mp,1))
                origins.push_back(route.destination);
        }
        for(const auto tile:origins)
        {
            const int radius=std::min(unit.maxRange,width+height);
            for(int y=std::max(0,tile.y-radius);y<=std::min(height-1,tile.y+radius);++y)
                for(int x=std::max(0,tile.x-radius);x<=std::min(width-1,tile.x+radius);++x)
                {
                    const int d=manhattanDistance(tile,{x,y});
                    if(d>=unit.minRange && d<=radius) covered[y*width+x]=true;
                }
        }
        return covered;
    }
    struct ProductionTransfer
    {
        int entryTick, completionTick;
        OwnerSign owner;
    };
    struct ProductionProperty
    {
        TilePoint tile;
        PropertyStockColumn column;
        int existingCompletion{-1};
        bool existingEnemy{};
        int existingEntry{-1};
        int capturer{NO_UNIT};
        int rate{}, carry{};
        std::vector<ProductionTransfer> ownershipChanges;
    };
    struct ProductionAccess
    {
        int side{-1};
        int buildDay{-1};
        int vacater{NO_UNIT};
        TilePoint exit{INVALID_TILE};
        int beforeEntry{std::numeric_limits<int>::max()};
    };
    struct ProductionOrder
    {
        int factory, side, targetUnit, property, day, impact;
        TilePoint destination, firstStop;
        const ProductionChoice *unit;
        MilliFunds damage, exposure;
        int arrival;
        const ReinforcementRoute *route;
        const ProductionAccess *access;
        TilePoint reliefExit{INVALID_TILE};
        int deniedUntil{-1};
        int forecastTargetProperty{-1};
        MilliFunds launchExposure{}, waitingExposure{};
        bool killsCapturer{};
        std::optional<MilliFunds> exposureBeforeFiring;
    };

    // Purchases convert available cash into equally priced army capital. Only
    // the extra paydays or an executable shot improve the score. The stock
    // forecast has already assigned every existing unit's planned capture.
    struct ProductionProjection
    {
        MilliFunds net{};
        QJsonArray orders,paidFounderWindows;
        QJsonObject alliedCaptureVacancy;
        std::vector<MilliFunds> flow;
        int availableSites{}, affordableOrders{};
        bool limited{};
        std::vector<StockCaptureUse> traffic;
        std::vector<FundedCaptureGun> fundedGuns;
        // A stock E0 shot can change one already-paid unit without changing
        // another unit of the same type bought on this or a later turn.
        std::shared_ptr<const ProductionChoice> damagedOpening;
        std::optional<OpeningRepairBill> enemyOpeningRepairs, friendlyFirstRepairs;
        struct Departure {int actor,tick; TilePoint end;};
        std::vector<Departure> departures;
    };

    // Use the same ownership dates for the spending ledger and the scored
    // paydays. The payment precedes the incoming player's purchase phase.
    inline OwnerSign productionOwnerAt(const ProductionProperty &property, int tick,
        const ProductionOrder *order=nullptr)
    {
        auto owner=property.column.ownerBefore;
        int latest=-1;
        const int scheduled=2*property.existingCompletion+int(property.existingEnemy);
        const auto claimant=property.existingEnemy ? OwnerSign::Enemy : OwnerSign::Ours;
        auto transfer=[&](int when,OwnerSign next) {
            if(order && order->forecastTargetProperty>=0 && when==scheduled && next==claimant)
                when=order->deniedUntil;
            if(when>=latest && when<tick) { owner=next; latest=when; }
        };
        if(property.ownershipChanges.empty() && property.existingCompletion>=0) transfer(scheduled,claimant);
        for(const auto &change : property.ownershipChanges) transfer(change.completionTick,change.owner);
        if(order && order->property>=0 && order->impact>latest && order->impact<tick)
            owner=order->side ? OwnerSign::Enemy : OwnerSign::Ours;
        return owner;
    }

    // Funding this fixed itinerary may use paydays whose title is already
    // established, even if some later proposed captures are declined. A
    // disputed future capture is never needed to finance a retained troop.
    inline bool certainProductionOwner(const ProductionProperty &p,int tick,OwnerSign side)
    {
        auto owner=p.column.ownerBefore;
        for(const auto &event:p.ownershipChanges)
        {
            if(event.completionTick<=1)
            {
                if(event.completionTick<tick) owner=event.owner;
            }
            else if(event.entryTick<tick && event.owner!=side) return false;
        }
        return owner==side;
    }

    inline MilliFunds productionIncomeChange(const ProductionOrder &order,
        std::span<const ProductionProperty> properties,int tick,int side)
    {
        const int j=order.property>=0 ? order.property : order.forecastTargetProperty;
        if(j<0) return 0;
        const auto &p=properties[j];
        const auto original=productionOwnerAt(p,tick);
        const auto revised=productionOwnerAt(p,tick,&order);
        const auto owner=side ? OwnerSign::Enemy : OwnerSign::Ours;
        const auto rate=side ? p.column.income.enemyPerTurn : p.column.income.oursPerTurn;
        return (int(revised==owner)-int(original==owner))*rate;
    }

    struct FundedPostCommitment
    {
        int side;
        TilePoint factory;
        int buildTick;
        QString type;
        TilePoint post;

        bool matches(const QJsonObject &order) const
        {
            return order["enemy"].toBool()==bool(side) && order["factory"]==QJsonArray{factory.x,factory.y} &&
                   order["type"]==type && order["buildHalfTurn"].toInt(-1)==buildTick;
        }
    };

    struct FundedPostReplay
    {
        const FundedPostCommitment &choice;
        const QJsonObject &original;
    };

    // A changed deployment cannot retroactively choose different purchases,
    // repairs, movement, or activations before the recruit's first order.
    inline bool unchangedFundedPostPrefix(const ProductionProjection &a,const ProductionProjection &b,int first)
    {
        const auto purchases=[&](const ProductionProjection &p){
            QJsonArray events;
            for(const auto &v:p.orders)
            {
                const auto o=v.toObject();
                if(o["buildHalfTurn"].toInt()>=first)continue;
                QJsonObject event;
                for(auto key:{"enemy","type","factory","buildHalfTurn","price","treasuryBefore","treasuryAfter","vacater","vacaterEnd"})
                    event[key]=o[key];
                if(o["firstStepHalfTurn"].toInt()<first)event["firstStop"]=o["firstStop"];
                if(o["impactHalfTurn"].toInt()>=0 && o["impactHalfTurn"].toInt()<first)
                    for(auto key:{"impactHalfTurn","kind","end","target","targetTile","targetDamage","exposure"})
                        event[key]=o[key];
                events.append(event);
            }
            return events;
        };
        const auto occupancy=[&](const ProductionProjection &p){
            std::multiset<std::tuple<int,int,int,int,int,bool>> seen;
            for(const auto &u:p.traffic)
                if(u.first<first)seen.emplace(u.cell,u.first,std::min(first-1,u.last),u.side,u.actor,u.stopped);
            return seen;
        };
        return purchases(a)==purchases(b) && occupancy(a)==occupancy(b);
    }

    struct ProductionReservation
    {
        int cell, first, last, side;
        bool stopped;
        int unassignedPurchase{-1};
        int actor{NO_UNIT};
    };

    inline bool overlappingProductionSteps(const ProductionReservation &a,const ProductionReservation &b)
    {
        return a.cell==b.cell && a.first<=b.last && b.first<=a.last && (a.side!=b.side || (a.stopped && b.stopped));
    }

    // Capture consumes the entrant's action. The square is still occupied
    // during the next opponent activation, including immediately after a
    // completion. A paid lethal interruption ends the original occupancy;
    // any forecast replacement needs the square from its own next activation.
    inline std::vector<ProductionReservation> productionCaptureStops(const ProductionProperty &p,
        int cell,int last,const ProductionOrder *intercept=nullptr)
    {
        std::vector<ProductionReservation> stops;
        auto add=[&](int from,int until,int side) {
            if(from>=0 && from<=until && from<=last) stops.push_back({cell,from,std::min(until,last),side,true});
        };
        auto visit=[&](int first,int done,OwnerSign owner) {
            const int who=int(owner==OwnerSign::Enemy);
            if(intercept && who==int(p.existingEnemy) && done==2*p.existingCompletion+who)
            {
                if(intercept->killsCapturer)
                {
                    add(first,intercept->impact-1,who);
                    add(intercept->impact+1,intercept->deniedUntil+1,who);
                }
                else add(first,intercept->deniedUntil+1,who);
            }
            else add(first,done+1,who);
        };
        if(p.ownershipChanges.empty() && p.existingEntry>=0 && p.existingCompletion>=p.existingEntry)
            visit(2*p.existingEntry+int(p.existingEnemy),2*p.existingCompletion+int(p.existingEnemy),
                  p.existingEnemy ? OwnerSign::Enemy : OwnerSign::Ours);
        for(const auto &event:p.ownershipChanges) visit(event.entryTick,event.completionTick,event.owner);
        return stops;
    }

    // A later recruit may follow the same road or depart the same factory.
    // End squares must be distinct; a friendly unit can pass through its ally.
    inline std::vector<ProductionReservation> productionReservations(const ProductionOrder &o,TilePoint factory,
        int width,int last,bool reserveVacater)
    {
        const auto cell=[&](TilePoint p){return p.y*width+p.x;};
        const auto &route=*o.route;
        const int built=2*o.day+o.side, arrived=2*o.arrival+o.side;
        std::vector<ProductionReservation> path;
        auto add=[&](int square,int first,int end,bool stopped) {
            if(first<=end && first<=last) path.push_back({square,first,std::min(last,end),o.side,stopped});
        };
        // Birth consumes the factory until movement on the next own activation.
        add(cell(factory),built,route.firstStop==factory ? last : built+1,true);
        if(reserveVacater && o.access->exit!=INVALID_TILE)
            add(cell(o.access->exit),2*o.access->buildDay+o.side,last,true);
        int tick=built+2;
        auto endOfDay=route.overnightStops.rbegin();
        for(auto it=route.cells.rbegin();it!=route.cells.rend();++it)
        {
            add(*it,tick,tick,false);
            if(endOfDay!=route.overnightStops.rend() && *it==*endOfDay)
            {
                add(*it,tick,tick+1,true);
                ++endOfDay;
                tick+=2;
            }
        }
        add(cell(route.destination),arrived,o.reliefExit==INVALID_TILE ? last : o.impact+1,true);
        if(o.reliefExit!=INVALID_TILE) add(cell(o.reliefExit),o.impact+2,last,true);
        return path;
    }

    inline std::optional<ProductionProjection> selectFundedProduction(
        const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge, std::span<const ProductionAccess> access,
        std::span<const ProductionProperty> properties, std::span<const TilePoint> positions, std::span<const int> hp,
        std::span<const int> staysThrough, const std::vector<int> &occupied, int points, int horizon, int last,
        TilePoint homeHQ, TilePoint awayHQ, std::span<const int> clearedAfterE1={},
        std::span<const TilePoint> occupiedAfterF0={},std::span<const TilePoint> occupiedAfterE0={},
        bool settleSchedule=false,QJsonObject *failure=nullptr,const OpeningRecruitFight *openingFight=nullptr,
        const OpeningRepairBill *commonEnemyOpeningRepairs=nullptr,const FundedPostReplay *condition=nullptr,const FriendlyServiceResult *friendlyService=nullptr,bool priceCapturedSources=false)
    {
        ProductionProjection result;
        QJsonObject postConsequence;
        auto refuse=[&](const char *reason,TilePoint site,int tick)->std::optional<ProductionProjection> {
            if(failure)
            {
                *failure=QJsonObject{{"phase","FUNDING_PROPOSAL"},{"reason",reason},{"site",QJsonArray{site.x,site.y}},{"tick",tick}};
                if(condition)
                {
                    const auto &q=condition->choice;
                    (*failure)["committedPurchase"]=QJsonObject{{"enemy",bool(q.side)},{"type",q.type},
                        {"factory",QJsonArray{q.factory.x,q.factory.y}},{"buildTick",q.buildTick},{"originalPost",condition->original["firstStop"]},
                        {"post",QJsonArray{q.post.x,q.post.y}}};
                    if(!postConsequence.isEmpty())(*failure)["postConsequence"]=postConsequence;
                }
            }
            return {};
        };
        result.flow.assign(horizon,0);
        if (!terrain.production || horizon < 2)
            return result;
        const auto &board = *terrain.production;
        auto slotsLeft = board.availableUnits;
        const int size = terrain.width * terrain.height;
        const int windows=std::max(1,static_cast<int>(std::count_if(access.begin(),access.end(),[&](const auto &a) {
            return a.side>=0 && a.buildDay>=0 && 2*(a.buildDay+1)+a.side<=last;
        })));
        // A later team gets the same per-site search as the first team. If the turn
        // budget cannot admit that comparison, do not price a shorter shopping list.
        const auto productionAllowance=[&](std::int64_t cap) -> std::optional<std::int64_t> {
            const auto available=terrain.workLeft ? *terrain.workLeft : 0;
            if(terrain.completeProductionMenu)return available;
            return terrain.quoteWorkQuota(available,16,cap);
        };
        const auto quota=productionAllowance(40000+24LL*size);
        if (!quota) return refuse("COMPARISON_WORK",INVALID_TILE,-1);
        const auto optionalWork=*quota;
        auto cell = [&](TilePoint t) { return t.y * terrain.width + t.x; };
        auto occupiedAt=[&](int pos,int day,int side) {
            const int unit=occupied[pos];
            return unit!=NO_UNIT && (2*day+side<=3 ||
                std::find(clearedAfterE1.begin(),clearedAfterE1.end(),unit)==clearedAfterE1.end());
        };
        std::vector<int> propertyAt(size,-1);
        std::array<int,2> fastestReplacement{1,1};
        for(std::size_t i=0;i<knowledge.units().size();++i)
            if(knowledge.units()[i].canCapture && hp[i]>0 && positions[i]!=INVALID_TILE)
            {
                const int side=int(terrain.units[i].enemy);
                fastestReplacement[side]=std::max(fastestReplacement[side],knowledge.units()[i].captureRate);
            }
        for (std::size_t j=0; j<properties.size(); ++j)
            propertyAt[cell(properties[j].tile)] = static_cast<int>(j);
        std::array<std::vector<MilliFunds>,2> balances;
        std::array<MilliFunds,2> possibleFunds=board.funds;
        if(commonEnemyOpeningRepairs)result.enemyOpeningRepairs=*commonEnemyOpeningRepairs;
        MilliFunds openingSpend=0;
        if(friendlyService){
            result.friendlyFirstRepairs=friendlyService->bill;
            if(!board.friendlyOpening)return refuse("F1_SERVICE_MISSING_F0_PORTFOLIO",INVALID_TILE,2);
            for(const auto &buy:*board.friendlyOpening)
                if(!RelativeStockDetail::checkedAdd(openingSpend,buy.price))return refuse("F1_SERVICE_ARITHMETIC",buy.factory,2);
        }
        for(int side=0;side<2;++side)
        {
            balances[side].resize(horizon);
            MilliFunds bank=board.funds[side];
            const auto owner=side ? OwnerSign::Enemy : OwnerSign::Ours;
            for(int day=0;day<horizon;++day)
            {
                const int tick=2*day+side;
                if(tick>0 && tick<=last)
                    for(const auto &p:properties)
                    {
                        const auto rate=side ? p.column.income.enemyPerTurn : p.column.income.oursPerTurn;
                        if(settleSchedule ? certainProductionOwner(p,tick,owner) : productionOwnerAt(p,tick)==owner) bank+=rate;
                        // Menu screening only: a friendly recruit may accelerate
                        // ownership. The actual bank is checked for every order.
                        possibleFunds[side]+=std::max<MilliFunds>(0,rate);
                    }
                if(side==1 && tick==1 && result.enemyOpeningRepairs){
                    const auto gross=bank;
                    if(!settleOpeningRepairDebit(*result.enemyOpeningRepairs,MILLI_FUNDS_PER_FUND,bank))return refuse("OPENING_REPAIR_WALLET_CHANGED",INVALID_TILE,tick);
                    possibleFunds[side]-=gross-bank;
                }
                if(side==0 && tick==2 && last>=2 && result.friendlyFirstRepairs){
                    // balances[] has not yet been debited for the fixed t0 purchases.
                    auto net=bank;const auto before=bank;
                    if(!RelativeStockDetail::checkedSubtract(net,openingSpend) ||
                       !settleOpeningRepairDebit(*result.friendlyFirstRepairs,MILLI_FUNDS_PER_FUND,net) ||
                       !RelativeStockDetail::checkedAdd(net,openingSpend))return refuse("F1_SERVICE_WALLET_CHANGED",INVALID_TILE,2);
                    bank=net;possibleFunds[side]-=before-bank;
                }
                balances[side][day]=bank;
            }
        }
        // This part of the friendly action precedes E0. Reserve every paid
        // newborn even when the enemy denies the recruit's proposed future job.
        int damagedFactory=-1;
        std::map<int,const FixedProductionPurchase*> purchases;
        std::map<int,std::vector<ProductionReservation>> traffic;
        if(board.friendlyOpening)
            for(const auto &purchase:*board.friendlyOpening)
            {
                const auto site=std::find_if(board.sites.begin(),board.sites.end(),[&](const auto &s){return s.tile==purchase.factory;});
                if(site==board.sites.end() || purchase.price<=0 || slotsLeft[0]<=0 ||
                   std::find(occupiedAfterF0.begin(),occupiedAfterF0.end(),purchase.factory)!=occupiedAfterF0.end() ||
                   std::find(occupiedAfterE0.begin(),occupiedAfterE0.end(),purchase.factory)!=occupiedAfterE0.end())
                    return refuse("FIXED_BIRTH_OCCUPIED_OR_ILLEGAL",purchase.factory,0);
                const auto option=std::find_if(site->menu[0].begin(),site->menu[0].end(),[&](const auto &u) {
                    return u.type==purchase.type && u.cost==purchase.price;
                });
                const int index=static_cast<int>(site-board.sites.begin());
                if(option==site->menu[0].end() || !purchases.emplace(index,&purchase).second) return refuse("FIXED_MENU_OR_DUPLICATE",purchase.factory,0);
                if(purchase.factory==board.resolvedOpeningFireSite ||
                   (openingFight && purchase.factory==openingFight->factory))
                {
                    if(openingFight && openingFight->recruitAfter<=0)
                        return refuse("OPENING_CASUALTY_FUTURE_SLOT_UNPRICED",purchase.factory,1);
                    auto changed=std::make_shared<ProductionChoice>(*option);
                    if(openingFight)
                    {
                        auto clips=openingFight->recruitLoad;auto life=openingFight->recruitAfter;
                        if(friendlyService){
                            const int actor=static_cast<int>(terrain.units.size()+(&purchase-board.friendlyOpening->data()));
                            for(const auto &s:friendlyService->subjects)if(s.repair.actor==actor){
                                clips=s.rounds;life=s.repair.hpPercent/10.;changed->initialFuel=s.fuel;break;}
                        }
                        for(int slot=0;slot<2;++slot)
                            if(option->captureLoad[slot]>0 && clips[slot]==0)
                                return refuse("OPENING_DEPLETED_WEAPON_FUTURE_MATCHUPS",purchase.factory,1);
                        changed->startingHp=life;
                        changed->captureLoad=clips;
                        for(const auto rounds:changed->captureLoad)
                            if(rounds>0) changed->guaranteedShots=std::min(changed->guaranteedShots,rounds);
                    }
                    result.damagedOpening=std::move(changed);
                    damagedFactory=index;
                }
                for(auto &bank:balances[0])
                {
                    if(bank<purchase.price) return refuse("FIXED_FUNDS",purchase.factory,0);
                    bank-=purchase.price;
                }
                --slotsLeft[0];
                const int sq=cell(purchase.factory);
                const int property=propertyAt[sq];
                if(property>=0 && productionOwnerAt(properties[property],1)!=OwnerSign::Ours) return refuse("FIXED_OWNER",purchase.factory,0);
                traffic[sq].push_back({sq,0,last,0,true,index});
            }
        if(openingFight && damagedFactory<0) return refuse("MISSING_DAMAGED_OPENING",openingFight->factory,1);
        std::vector<ProductionOrder> offers;
        std::map<std::tuple<int,int,int,int>,std::vector<std::vector<int>>> firingTargets;
        std::map<std::tuple<int,int,int>,std::vector<std::vector<int>>> captureFronts;
        auto apply = [&](const ProductionOrder &o, std::vector<MilliFunds> *flows) {
            const int sign = o.side ? -1 : 1;
            MilliFunds net = 0;
            for (int round = 1; round <= horizon; ++round)
            {
                MilliFunds change = 0;
                if(2*round<=last) change+=productionIncomeChange(o,properties,2*round,0);
                if(2*round-1<=last) change-=productionIncomeChange(o,properties,2*round-1,1);
                if (round == (o.impact+1)/2)
                    change += sign * (o.damage-o.exposure);
                if (flows)
                    (*flows)[round-1] += change;
                net += change;
            }
            return sign*net;
        };
        auto priceInterception=[&](ProductionOrder &o) {
            const auto &p=properties[o.forecastTargetProperty];
            const int k=p.capturer, foe=int(p.existingEnemy);
            const auto &unit=*o.unit;
            const int ready=o.arrival+int(!unit.mobileFire && o.destination!=board.sites[o.factory].tile);
            const int fires=std::max(ready,p.existingEntry+int(o.side<foe));
            o.impact=2*fires+o.side;
            if(o.impact>=2*p.existingCompletion+foe || o.impact>last) return false;
            const auto risk=std::min(unit.cost,o.launchExposure+o.waitingExposure*(1+fires-ready));
            if(risk>=unit.cost) return false;
            o.exposureBeforeFiring=risk;
            const int firingHp=ceilHpSteps(unit.startingHp)-static_cast<int>((risk*10+unit.cost-1)/unit.cost);
            const auto shot=engineShotDamage(terrainReducedDamage(terrain.defense(k,p.tile),unit.attack[k],firingHp,hp[k]));
            const int left=ceilHpSteps(engineHpAfterReply(hp[k],shot)), hit=hp[k]-left;
            if(hit<=0) return false;
            const int spent=fires-p.existingEntry+int(foe<o.side);
            const int carried=std::min(points,p.carry+p.rate*spent);
            const int next=fires+int(foe<o.side);
            const int rate=replyCaptureRate(p.rate,hit,left<=0);
            const int restart=next+captureTurnsFor(0,std::max(p.rate,fastestReplacement[foe]),points)-1;
            const int delayed=rate>0 ? std::min(restart,next+captureTurnsFor(carried,rate,points)-1) : restart;
            o.deniedUntil=2*delayed+foe;
            o.killsCapturer=left<=0;
            // Moving this transfer past an already scheduled recapture would
            // invalidate the later capturer's job. That alternative needs a
            // freshly replayed capture calendar before it can earn a payoff.
            const int old=2*p.existingCompletion+foe;
            for(const auto &change:p.ownershipChanges)
                if(change.completionTick>old && change.entryTick<=o.deniedUntil+1) return false;
            o.damage=bookDamage(terrain.units[k].replacementCost,hit);
            o.exposure=risk;
            if(manhattanDistance(o.destination,p.tile)==1 && left>0 && unit.incoming[k]>0 &&
               knowledge.units()[k].minRange<=1 && knowledge.units()[k].maxRange>=1)
            {
                const auto routeIndex=o.route-unit.routes.data();
                const auto counter=engineShotDamage(terrainReducedDamage(unit.defense[routeIndex],unit.incoming[k],left,firingHp));
                o.exposure=std::min(unit.cost,risk+bookDamage(unit.cost,firingHp-ceilHpSteps(engineHpAfterReply(firingHp,counter))));
            }
            return true;
        };
        for (int window=0; window<static_cast<int>(access.size()); ++window)
        {
            const int f=window%static_cast<int>(board.sites.size());
            const auto &site = board.sites[f];
            const auto &opening=access[window];
            const int side = opening.side, day = opening.buildDay;
            const auto committed=purchases.find(f);
            const bool paidOpening=side==0 && day==0 && committed!=purchases.end();
            if (side < 0 || day < 0 || (!slotsLeft[side] && !paidOpening) || 2*(day+1)+side > last)
                continue;
            ++result.availableSites;
            // Keep the healthy itinerary as the source for a later new recruit.
            // Only the unit already paid for at F0 inherits the actual hit.
            const auto &menu=site.menu[side];
            const auto optionCount=menu.size()+static_cast<std::size_t>(paidOpening && f==damagedFactory);
            for (std::size_t oi=0;oi<optionCount;++oi)
            {
                const auto &unit=oi<menu.size()?menu[oi]:*result.damagedOpening;
                if (unit.cost > possibleFunds[side])
                    continue;
                // During a full paired quote, cover every modeled offer using the
                // real arm budget. A cheap prior visit cannot truncate a later menu.
                std::int64_t quota=terrain.completeProductionMenu ? optionalWork :
                    optionalWork/windows/std::max<std::size_t>(1,optionCount);
                auto pay=[&](std::int64_t amount) {
                    if(amount>quota || !terrain.pay(amount))
                    {
                        result.limited=true;
                        return false;
                    }
                    quota-=amount;
                    return true;
                };
                const auto frontKey=std::tuple{side,unit.minRange,unit.maxRange};
                auto [front,fresh]=captureFronts.try_emplace(frontKey);
                if(fresh)
                {
                    front->second.resize(size);
                    for(int j=0;j<static_cast<int>(properties.size());++j)
                    {
                        const auto &p=properties[j];
                        if(p.existingCompletion<=0 || p.capturer==NO_UNIT || p.rate<=0 ||
                           int(p.existingEnemy)==side || p.tile==homeHQ || p.tile==awayHQ) continue;
                        const int radius=std::min(unit.maxRange,terrain.width+terrain.height);
                        for(int y=std::max(0,p.tile.y-radius);y<=std::min(terrain.height-1,p.tile.y+radius);++y)
                            for(int x=std::max(0,p.tile.x-radius);x<=std::min(terrain.width-1,p.tile.x+radius);++x)
                            {
                                const int d=manhattanDistance(p.tile,{x,y});
                                if(d>=unit.minRange && d<=radius) front->second[y*terrain.width+x].push_back(j);
                            }
                    }
                }
                auto targetsFor = [&](int day) -> const std::vector<std::vector<int>> * {
                    const auto key=std::tuple{side,unit.minRange,unit.maxRange,day};
                    if(auto i=firingTargets.find(key);i!=firingTargets.end()) return &i->second;
                    const int radius=std::min(unit.maxRange,terrain.width+terrain.height);
                    if(!pay(1+static_cast<std::int64_t>(positions.size()))) return nullptr;
                    auto &table=firingTargets[key];
                    table.resize(size);
                    for (int k=0;k<static_cast<int>(positions.size());++k)
                    {
                        if (int(terrain.units[k].enemy)==side || positions[k]==INVALID_TILE || hp[k]<=0 ||
                            (day>1 && staysThrough[k]<day)) continue;
                        const auto t=positions[k];
                        for (int y=std::max(0,t.y-radius);y<=std::min(terrain.height-1,t.y+radius);++y)
                            for (int x=std::max(0,t.x-radius);x<=std::min(terrain.width-1,t.x+radius);++x)
                            {
                                const int dist=manhattanDistance({x,y},t);
                                if (dist>=unit.minRange && dist<=radius) table[y*terrain.width+x].push_back(k);
                            }
                    }
                    return &table;
                };
                // Compare captures and firing positions on the same forecast board. A
                // newborn can first leave or fire on its next own activation.
                std::map<std::pair<int,int>,MilliFunds> exposureCache;
                auto exposureAt = [&](int sq, const TerrainDefenseRule &def) {
                    auto key = std::pair{sq, def.stars};
                    if (auto it = exposureCache.find(key); it != exposureCache.end())
                        return it->second;
                    if(!pay(1+static_cast<std::int64_t>(positions.size()))) return unit.cost;
                    MilliFunds worst = 0;
                    const TilePoint tile{sq%terrain.width,sq/terrain.width};
                    for (int k=0; k<static_cast<int>(positions.size()); ++k)
                    {
                        if (int(terrain.units[k].enemy)==side || positions[k]==INVALID_TILE || hp[k]<=0 ||
                            unit.incoming[k]<=0)
                            continue;
                        const auto &u=knowledge.units()[k];
                        if (!reinforcementExposure(terrain,u,terrain.units[k],positions[k])[sq]) continue;
                        const double damage=engineShotDamage(terrainReducedDamage(def,unit.incoming[k],hp[k],ceilHpSteps(unit.startingHp)));
                        worst=std::max(worst,bookDamage(unit.cost,ceilHpSteps(unit.startingHp)-ceilHpSteps(engineHpAfterReply(unit.startingHp,damage))));
                    }
                    exposureCache.emplace(key,worst);
                    return worst;
                };
                for (std::size_t r=0; r<unit.routes.size(); ++r)
                {
                    if(!pay(1)) break;
                    const auto &route = unit.routes[r];
                    const int sq = cell(route.destination), j = propertyAt[sq];
                    const int arrival = day+route.activations;
                    if(opening.beforeEntry!=std::numeric_limits<int>::max() &&
                       (route.firstStop==site.tile || 2*(day+1)+side>=opening.beforeEntry))
                        continue;
                    // A current occupant is not an all-orders reason to deny a paid
                    // capturer the property next turn: its teammate can move first.
                    // Keep one precise determinant until both commitments are priced.
                    if(result.alliedCaptureVacancy.isEmpty() && paidOpening &&
                       committed->second->type==unit.type && committed->second->price==unit.cost &&
                       j>=0 && arrival==1 && route.destination==route.firstStop && route.destination!=site.tile &&
                       route.destination!=homeHQ && route.destination!=awayHQ &&
                       properties[j].column.ownerBefore!=OwnerSign::Ours && occupiedAt(sq,arrival,side)){
                        const int holder=occupied[sq];
                        const auto rate=unit.captureRate.find(sq);
                        if(holder>=0 && holder<static_cast<int>(knowledge.units().size()) &&
                           !terrain.units[holder].enemy && !knowledge.units()[holder].canCapture &&
                           rate!=unit.captureRate.end() && rate->second>0){
                            const auto installments=(static_cast<std::int64_t>(points)+rate->second-1)/rate->second;
                            const auto firstPayday=2*(installments+1);
                            if(firstPayday<=last)
                                result.alliedCaptureVacancy={{"reason","TEAMMATE_MAY_VACATE_BEFORE_PAID_CAPTURER"},
                                    {"unit",terrain.units[holder].engineUnitId},{"purchased",unit.type},
                                    {"factory",QJsonArray{site.tile.x,site.tile.y}},
                                    {"tile",QJsonArray{route.destination.x,route.destination.y}},
                                    {"requiredVacancyHalfTurn",2},{"firstPossiblePayday",static_cast<qint64>(firstPayday)}};
                        }
                    }
                    // This is one chosen departure before the modeled buyer. The
                    // vacater can choose another legal stop or act again before
                    // its recruit arrives. A useful capture rejected solely by
                    // this stop needs that coordinated choice covered explicitly.
                    if(result.alliedCaptureVacancy.isEmpty() && opening.vacater!=NO_UNIT &&
                       opening.exit!=INVALID_TILE && opening.buildDay>=1 && j>=0 &&
                       (route.destination==opening.exit || route.firstStop==opening.exit) &&
                       route.destination!=homeHQ && route.destination!=awayHQ &&
                       properties[j].column.ownerBefore!=(side?OwnerSign::Enemy:OwnerSign::Ours)){
                        const auto rate=unit.captureRate.find(sq);
                        if(rate!=unit.captureRate.end() && rate->second>0){
                            const auto installments=(static_cast<std::int64_t>(points)+rate->second-1)/rate->second;
                            const auto firstEntry=2LL*arrival+side;
                            const auto firstPay=firstEntry+2*installments;
                            if(firstPay<=last)
                                result.alliedCaptureVacancy={{"reason","ALTERNATE_VACATING_POST_OR_LATER_QUEUE_NEEDS_PRICE"},
                                    {"unit",opening.vacater},{"enemy",bool(side)},{"possibleRecruit",unit.type},
                                    {"factory",QJsonArray{site.tile.x,site.tile.y}},
                                    {"quotedVacatingPost",QJsonArray{opening.exit.x,opening.exit.y}},
                                    {"tile",QJsonArray{route.destination.x,route.destination.y}},
                                    {"potentialBuyHalfTurn",2*day+side},{"requiredVacancyHalfTurn",static_cast<qint64>(firstEntry)},
                                    {"firstPossiblePayday",static_cast<qint64>(firstPay)}};
                        }
                    }
                    if (2*arrival+side > last || route.destination==opening.exit || route.firstStop==opening.exit ||
                        (route.destination!=site.tile && occupiedAt(sq,arrival,side)) ||
                        (route.firstStop!=site.tile && occupiedAt(cell(route.firstStop),day+1,side)))
                        continue;
                    const bool capture = j>=0 &&
                        (properties[j].existingCompletion<0 || int(properties[j].existingEnemy)==side) &&
                        properties[j].column.ownerBefore!=(side ? OwnerSign::Enemy : OwnerSign::Ours) &&
                        route.destination!=homeHQ && route.destination!=awayHQ && unit.captureRate.contains(sq);
                    // Indirect units need an additional stationary activation after deployment.
                    const int shotDay=arrival+int(!unit.mobileFire && route.destination!=site.tile);
                    const auto &futureTargets=front->second[sq];
                    if (!capture && futureTargets.empty() && (shotDay>1 || 2*shotDay+side>last))
                        continue;
                    const std::vector<int> *targets=nullptr;
                    if(shotDay==1 && 2*shotDay+side<=last)
                    {
                        auto table=targetsFor(shotDay);
                        if(table) targets=&(*table)[sq];
                    }
                    if(!capture && futureTargets.empty() && (!targets || targets->empty())) continue;
                    if (!pay(1 + static_cast<std::int64_t>(route.cells.size()) + futureTargets.size()))
                        break;
                    if (std::any_of(route.cells.begin(),route.cells.end(),[&](int pos) {
                        return occupiedAt(pos,day+1,side) && int(terrain.units[occupied[pos]].enemy)!=side; }))
                        continue;
                    if (std::any_of(route.overnightStops.begin(),route.overnightStops.end(),[&](int stop) {
                        return occupiedAt(stop,day+1,side) || (opening.exit!=INVALID_TILE && stop==cell(opening.exit)); }))
                        continue;
                    // Charge one adverse shot for each distinct exposed stage. Do not count
                    // a lethal deployment as usable or book its full-health capture rate.
                    // E0 was played for this opening body in both the hit and decline replies.
                    // Its F1 destination and all later fresh purchases still face their own opponent turn.
                    auto launchRisk=&unit==result.damagedOpening.get()?MilliFunds{0}:exposureAt(cell(site.tile),unit.defense.back());
                    for (int stop:route.overnightStops)
                    {
                        const int ri=unit.routeAt.empty() ? -1 : unit.routeAt[stop];
                        launchRisk=std::min(unit.cost,launchRisk+exposureAt(stop,ri>=0 ? unit.defense[ri] : unit.defense.back()));
                    }
                    if (launchRisk>=unit.cost)
                        continue;
                    const auto risk=std::min(unit.cost,launchRisk+exposureAt(sq,unit.defense[r]));
                    if (capture && risk<unit.cost && exposureAt(sq,unit.defense[r])==0)
                    {
                        const int lost=10-ceilHpSteps(unit.startingHp)+static_cast<int>((risk*10+unit.cost-1)/unit.cost);
                        const int rate=replyCaptureRate(unit.captureRate.at(sq),lost,false);
                        const int finish=rate>0 ? arrival+captureTurnsFor(0,rate,points)-1 : horizon;
                        TilePoint reliefExit = INVALID_TILE;
                        const auto &previous = properties[j];
                        bool compatible = previous.existingCompletion < 0;
                        if (!compatible && finish + 1 <= previous.existingEntry)
                        {
                            // Clear the property before the old claimant arrives. If the
                            // moves fall on one turn, the reinforcement vacates first.
                            auto unprotected = unit.defense[r];
                            unprotected.stars = 0;
                            for (TilePoint to : route.exits)
                            {
                                const int at = cell(to);
                                if (to == opening.exit || to == site.tile || occupiedAt(at,finish+1,side) ||
                                    propertyAt[at] >= 0 || exposureAt(at, unprotected) != 0)
                                    continue;
                                reliefExit = to;
                                compatible = true;
                                break;
                            }
                        }
                        if (compatible && 2*finish+side<last && pay(horizon+3))
                            offers.push_back({f,side,NO_UNIT,j,day,2*finish+side,route.destination,
                                              route.firstStop,&unit,0,risk,arrival,&route,&opening,reliefExit});
                    }
                    // An enemy itinerary commits its named capturer from first entry until
                    // completion. Fire between those activations at the actual forecast tile.
                    // A different full-strength capper may restart on the very next action.
                    for(int j:futureTargets)
                    {
                        const auto &p=properties[j];
                        const int k=p.capturer;
                        if(hp[k]<=0 || unit.attack[k]<=0) continue;
                        ProductionOrder o{f,side,k,-1,day,0,route.destination,route.firstStop,&unit,0,0,
                            arrival,&route,&opening,INVALID_TILE,-1,j,launchRisk,exposureAt(sq,unit.defense[r])};
                        if(priceInterception(o) && pay(horizon+3)) offers.push_back(o);
                    }
                    if (!targets)
                        continue;
                    for (int k:*targets)
                    {
                        if (int(terrain.units[k].enemy)==side || positions[k]==INVALID_TILE || hp[k]<=0 ||
                            unit.attack[k]<=0 || (shotDay>1 && staysThrough[k]<shotDay))
                            continue;
                        const int distance=manhattanDistance(route.destination,positions[k]);
                        if (distance<unit.minRange || distance>unit.maxRange)
                            continue;
                        // When a target's intervening activation has no modeled commitment,
                        // require that it cannot leave the proposed weapon range. Its projected
                        // position is otherwise only a possible, freely avoidable follow-up.
                        if(staysThrough[k]<shotDay)
                        {
                            auto escape=indirectTargetCanEscape(terrain,positions,hp,k,route.destination,
                                                               unit.minRange,unit.maxRange,&quota);
                            if(!escape) { result.limited=true; continue; }
                            if(*escape) continue;
                        }
                        const int firingHp=ceilHpSteps(unit.startingHp)-static_cast<int>((launchRisk*10+unit.cost-1)/unit.cost);
                        const double damage=engineShotDamage(terrainReducedDamage(terrain.defense(k,positions[k]),
                            unit.attack[k],firingHp,hp[k]));
                        const auto destroyed=bookDamage(terrain.units[k].replacementCost,
                            hp[k]-ceilHpSteps(engineHpAfterReply(hp[k],damage)));
                        if (destroyed>risk && pay(horizon+3))
                            offers.push_back({f,side,k,-1,day,2*shotDay+side,route.destination,
                                              route.firstStop,&unit,destroyed,risk,arrival,&route,&opening});
                    }
                }
            }
        }
        // Reuse physical routes, not a fixed price for a factory day. A later
        // recruit must still reach its task and have a positive dated effect.
        // Preserve the base-date bids even when later optional pricing runs out.
        const auto firstOffers=offers.size();
        const auto laterQuota=productionAllowance(optionalWork);
        if (!laterQuota) return refuse("LATER_COMPARISON_WORK",INVALID_TILE,-1);
        auto laterWork=*laterQuota;
        for(int delay=1;delay<horizon;++delay)
        {
            bool affordableWork=true;
            for(std::size_t i=0;i<firstOffers;++i)
            {
                if(offers[i].unit==result.damagedOpening.get()) continue;
                const int cost=horizon+3;
                if(laterWork<cost || !terrain.pay(cost)) { affordableWork=false; break; }
                laterWork-=cost;
                auto o=offers[i];
                o.day+=delay;
                o.arrival+=delay;
                if(2*o.arrival+o.side>last || 2*(o.day+1)+o.side>=o.access->beforeEntry) continue;
                if(o.forecastTargetProperty>=0)
                {
                    if(!priceInterception(o)) continue;
                }
                else
                {
                    o.impact+=2*delay;
                    if(o.impact>last) continue;
                    if(o.property>=0)
                    {
                        const auto &p=properties[o.property];
                        if(p.existingCompletion>=0 && o.impact/2+1>p.existingEntry) continue;
                    }
                    else if(staysThrough[o.targetUnit]<o.impact/2) continue;
                }
                offers.push_back(o);
            }
            if(!affordableWork) { result.limited=true; break; }
        }
        // Reuse the generated paid capture bids. A future unit at a newly
        // captured producer has its own duty and funding requirements. Record
        // a dated financial gap instead of assuming it has no consequence.
        std::vector<int> potentialShop;
        std::map<std::pair<int,int>,const ProductionOrder*> paidFounders;
        if(settleSchedule && last>=4 && board.sites.size()>1){
            potentialShop.assign(properties.size(),-1);
            for(int f=0;f<static_cast<int>(board.sites.size());++f){
                const int property=propertyAt[cell(board.sites[f].tile)];
                if(property>=0)potentialShop[property]=f;
            }
        }
        std::vector<MilliFunds> values;
        for (const auto &o:offers)
        {
            values.push_back(apply(o,nullptr));
            result.affordableOrders+=values.back()>0;
            if(!potentialShop.empty() && o.property>=0 && potentialShop[o.property]>=0 && o.impact+2<=last){
                const int f=potentialShop[o.property];
                const auto &dest=board.sites[f];
                const auto &old=properties[o.property];
                const auto owner=o.side?OwnerSign::Enemy:OwnerSign::Ours;
                const auto paid=purchases.find(o.factory);
                const bool matchesOpening=o.side!=0 || o.day>0 || !board.friendlyOpening ||
                    (paid!=purchases.end() && paid->second->type==o.unit->type && paid->second->price==o.unit->cost);
                if(old.column.ownerBefore!=owner && matchesOpening && dest.minimumBuildCost[o.side]>0 &&
                   possibleFunds[o.side]>=dest.minimumBuildCost[o.side]){
                    auto &best=paidFounders[{f,o.side}];
                    if(!best || o.impact<best->impact)best=&o;
                }
            }
        }
        std::vector<int> order(offers.size());
        std::iota(order.begin(),order.end(),0);
        std::stable_sort(order.begin(),order.end(),[&](int a,int b) {
            const auto &x=offers[a], &y=offers[b];
            return std::tuple{2*x.day+x.side,-values[a],x.impact,x.unit->cost} <
                   std::tuple{2*y.day+y.side,-values[b],y.impact,y.unit->cost};
        });
        // Preserve the economic ordering of unrelated purchases. Only reorder the
        // places already occupied by bids for a property that both teams can reach.
        // An unaffordable or obstructed early bidder will still be skipped below.
        struct Contest
        {
            int teams{};
            std::vector<std::size_t> places;
            std::vector<int> bids;
        };
        std::map<int,Contest> contested;
        for (std::size_t i=0;i<order.size();++i)
        {
            const int bid=order[i];
            const auto &o=offers[bid];
            if(o.property<0 || values[bid]<=0) continue;
            auto &contest=contested[o.property];
            contest.teams|=1<<o.side;
            contest.places.push_back(i);
            contest.bids.push_back(bid);
        }
        for(auto &[property,contest]:contested)
        {
            if(contest.teams!=3) continue;
            std::stable_sort(contest.bids.begin(),contest.bids.end(),[&](int a,int b) {
                const auto &x=offers[a], &y=offers[b];
                return std::tuple{2*x.arrival+x.side,-values[a],x.unit->cost} <
                       std::tuple{2*y.arrival+y.side,-values[b],y.unit->cost};
            });
            for(std::size_t i=0;i<contest.places.size();++i)
                order[contest.places[i]]=contest.bids[i];
        }
        std::set<int> hit, booked;
        std::set<const ProductionAccess*> usedOpenings;
        std::vector<const ProductionOrder*> changedProperties(properties.size(),nullptr);
        std::map<int,std::vector<std::pair<int,OwnerSign>>> usedSites;
        std::set<int> assignedOpening;
        std::vector<QJsonObject> receipts;
        auto tile=[](TilePoint p){return QJsonArray{p.x,p.y};};
        std::vector<std::vector<ProductionReservation>> captureStops;
        for(const auto &p:properties) captureStops.push_back(productionCaptureStops(p,cell(p.tile),last));
        auto needsFirstDeparture=[&](int factory) {
            const int sq=cell(board.sites[factory].tile);
            const int j=propertyAt[sq];
            const bool contested=j>=0 && properties[j].existingEntry>=0 &&
                2*properties[j].existingEntry+int(properties[j].existingEnemy)<=last;
            return (last>=2 && occupiedAt(sq,1,0)) || contested;
        };
        auto conflicts=[&](std::span<const ProductionReservation> path,int originalPurchase,
                           int intercepted=-1,std::span<const ProductionReservation> revised={}) {
            return std::any_of(path.begin(),path.end(),[&](const auto &next) {
                const int site=propertyAt[next.cell];
                if(site>=0)
                {
                    const std::span<const ProductionReservation> existing=
                        site==intercepted ? revised : std::span<const ProductionReservation>(captureStops[site]);
                    if(std::any_of(existing.begin(),existing.end(),
                        [&](const auto &stock){return overlappingProductionSteps(next,stock);})) return true;
                }
                auto found=traffic.find(next.cell);
                return found!=traffic.end() && std::any_of(found->second.begin(),found->second.end(),[&](const auto &prior) {
                    return !(originalPurchase>=0 && prior.unassignedPurchase==originalPurchase) &&
                        overlappingProductionSteps(next,prior);
                });
            });
        };
        auto releasePlaceholder=[&](int factory) {
            auto &birth=traffic[cell(board.sites[factory].tile)];
            std::erase_if(birth,[&](const auto &slot){return slot.unassignedPurchase==factory;});
        };
        // This is an executable movement-and-wait policy for one already
        // funded body. The ordinary native blueprint supplies movement costs
        // and gun load. A threatened new post requires its own combat branch.
        const auto makePostOrder=[&](const ProductionOrder &source,ProductionOrder &out)->const char * {
            const auto &q=condition->choice;
            const auto &unit=*source.unit;
            const auto &factory=board.sites[source.factory].tile;
            if(!unit.movementGrid || unit.movement<=0 || unit.cost<=0 || unit.initialFuel<0 || q.post==source.access->exit)
                return "FUNDED_POST_MOVEMENT_UNSUPPORTED";
            if(source.access->beforeEntry!=std::numeric_limits<int>::max() &&
               (q.post==factory || q.buildTick+2>=source.access->beforeEntry))return "FUNDED_POST_REQUIRED_CLEARANCE_CHANGED";
            const auto found=std::find_if(unit.routes.begin(),unit.routes.end(),[&](const auto &r){
                return r.activations==1 && r.firstStop==q.post && r.destination==q.post && r.overnightStops.empty();});
            if(found==unit.routes.end())return "FUNDED_POST_ROUTE_NOT_WITNESSED";
            const auto &grid=*unit.movementGrid;
            if(!grid.onGrid(q.post.x,q.post.y) || !grid.onGrid(factory.x,factory.y))return "FUNDED_POST_INVALID_GRID";
            int spent=0;
            auto previous=factory;
            if(!terrain.pay(1+static_cast<std::int64_t>(found->cells.size()+positions.size())))return "FUNDED_POST_MOVEMENT_WORK_LIMIT";
            for(auto it=found->cells.rbegin();it!=found->cells.rend();++it)
            {
                if(*it<0 || *it>=size)return "FUNDED_POST_INVALID_ROUTE";
                TilePoint next{*it%terrain.width,*it/terrain.width};
                if(manhattanDistance(previous,next)!=1)return "FUNDED_POST_INVALID_ROUTE";
                const auto cost=grid.cost(next.x,next.y,entryDirection(previous.x,previous.y,next.x,next.y));
                if(cost<0 || cost>unit.movement-spent || (unit.initialFuel>=0 && cost>unit.initialFuel-spent))
                    return "FUNDED_POST_ROUTE_OR_FUEL_CHANGED";
                if(occupiedAt(*it,source.day+1,source.side) &&
                   ((!unit.ignoresUnitCollision && int(terrain.units[occupied[*it]].enemy)!=source.side) || next==q.post))
                {
                    postConsequence={{"unit",terrain.units[occupied[*it]].engineUnitId},{"tile",QJsonArray{next.x,next.y}},
                        {"tick",q.buildTick+2},{"effect","ORIGINAL_REQUIRES_DATED_CLEARANCE"}};
                    return "FUNDED_POST_ORIGINAL_OCCUPANCY_UNRESOLVED";
                }
                previous=next;spent+=cost;
            }
            if(previous!=q.post || (q.post!=factory && occupiedAt(cell(q.post),source.day+1,source.side)))
                return "FUNDED_POST_ORIGINAL_OCCUPANCY_UNRESOLVED";
            for(int i=0;i<int(positions.size());++i)
            {
                if(positions[i]==INVALID_TILE || hp[i]<=0 || int(terrain.units[i].enemy)==source.side)continue;
                if(i>=int(unit.incoming.size()))return "FUNDED_POST_MISSING_EXPOSURE_INPUT";
                if(unit.incoming[i]<=0)continue;
                if(i>=int(knowledge.units().size()))return "FUNDED_POST_MISSING_EXPOSURE_INPUT";
                const auto &reachable=reinforcementExposure(terrain,knowledge.units()[i],terrain.units[i],positions[i]);
                const bool atBirth=&unit!=result.damagedOpening.get() && reachable[cell(factory)];
                if(reachable[cell(q.post)] || atBirth)
                {
                    const auto square=atBirth?factory:q.post;
                    postConsequence={{"unit",terrain.units[i].engineUnitId},{"from",QJsonArray{positions[i].x,positions[i].y}},
                        {"roundedHpOnInput",hp[i]},{"tile",QJsonArray{square.x,square.y}},{"tick",q.buildTick+(atBirth?1:3)},
                        {"effect","SUPPORTED_WEAPON_LANE_REQUIRES_DATED_MOVEMENT_AND_COMBAT"}};
                    return "FUNDED_POST_EXPOSURE_NEEDS_REPLAY";
                }
            }
            out={source.factory,source.side,NO_UNIT,-1,source.day,q.buildTick+2,q.post,q.post,&unit,0,0,
                 source.day+1,&*found,source.access};
            return nullptr;
        };
        std::map<std::pair<int,int>,int> vacaterAt;
        bool placed=false;
        for(int n:order)
        {
            const auto &bid=offers[n];
            std::optional<ProductionOrder> post;
            if(condition && bid.side==condition->choice.side && 2*bid.day+bid.side==condition->choice.buildTick &&
               board.sites[bid.factory].tile==condition->choice.factory)
            {
                const auto &original=condition->original;
                const auto expectedType=bid.forecastTargetProperty>=0?"INTERCEPT_CAPTURE":bid.property>=0?"CAPTURE":"FIRE";
                if(placed || bid.unit->type!=condition->choice.type || bid.unit->cost!=original["price"].toInteger(-1) ||
                   original["end"]!=tile(bid.destination) || original["firstStop"]!=tile(bid.firstStop) ||
                   original["kind"]!=expectedType || original["impactHalfTurn"].toInt(-1)!=bid.impact ||
                   original["target"].toInt(-1)!=(bid.targetUnit==NO_UNIT?NO_UNIT:terrain.units[bid.targetUnit].engineUnitId) ||
                   original["vacater"].toInt(NO_UNIT)!=(bid.day==bid.access->buildDay?bid.access->vacater:NO_UNIT))continue;
                post=bid;
                if(auto reason=makePostOrder(bid,*post))return refuse(reason,condition->choice.factory,condition->choice.buildTick);
            }
            const auto &o=post?*post:bid;
            const MilliFunds orderValue=post?apply(o,nullptr):values[n];
            const auto &opening=*o.access;
            const bool committed=o.side==0 && o.day==0 && board.friendlyOpening.has_value();
            if(committed)
            {
                if(o.factory==damagedFactory && o.unit!=result.damagedOpening.get()) continue;
                const auto paid=purchases.find(o.factory);
                if(paid==purchases.end() || assignedOpening.contains(o.factory) ||
                   o.unit->type!=paid->second->type || o.unit->cost!=paid->second->price) continue;
                if(o.firstStop==paid->second->factory && needsFirstDeparture(o.factory)) continue;
            }
            if ((!post && orderValue<=0) || (!committed && (!slotsLeft[o.side] || balances[o.side][o.day]<o.unit->cost)) ||
                (o.property>=0 && booked.contains(o.property)) ||
                (o.forecastTargetProperty>=0 && booked.contains(o.forecastTargetProperty)) ||
                (o.targetUnit!=NO_UNIT && hit.contains(o.targetUnit)))
                continue;
            if(!board.friendlyOpening && o.side==0 && o.day==0 && board.canProposeOpening &&
               !board.canProposeOpening(board.sites[o.factory],*o.unit)) continue;
            const int shop=propertyAt[cell(board.sites[o.factory].tile)];
            const int atPurchase=2*o.day+o.side+1;
            const auto buyer=o.side ? OwnerSign::Enemy : OwnerSign::Ours;
            bool dependsOnStockCapture=false;
            if(settleSchedule && shop>=0 && !certainProductionOwner(properties[shop],atPurchase,buyer))
            {
                const auto &source=properties[shop];
                dependsOnStockCapture=priceCapturedSources && source.column.ownerBefore!=buyer &&
                    source.capturer>=0 && source.existingEntry>=0 && source.existingCompletion>0 &&
                    int(source.existingEnemy)==o.side && opening.buildDay>source.existingCompletion &&
                    source.existingCompletion<o.day;
                if(!dependsOnStockCapture)continue;
            }
            if(shop>=0 && productionOwnerAt(properties[shop],atPurchase,changedProperties[shop])!=buyer) continue;
            const int modified=o.property>=0 ? o.property : o.forecastTargetProperty;
            if(modified>=0 && usedSites.contains(modified) &&
               std::any_of(usedSites[modified].begin(),usedSites[modified].end(),[&](const auto &buy) {
                   return productionOwnerAt(properties[modified],buy.first,&o)!=buy.second;
               })) continue;
            const auto path=productionReservations(o,board.sites[o.factory].tile,terrain.width,last,!usedOpenings.contains(o.access));
            const int intercepted=o.forecastTargetProperty;
            std::vector<ProductionReservation> revisedStops;
            if(intercepted>=0)
            {
                const int targetCell=cell(properties[intercepted].tile);
                revisedStops=productionCaptureStops(properties[intercepted],targetCell,last,&o);
                auto used=traffic.find(targetCell);
                if(used!=traffic.end() && std::any_of(revisedStops.begin(),revisedStops.end(),[&](const auto &claim) {
                    return std::any_of(used->second.begin(),used->second.end(),[&](const auto &paid) {
                        return paid.unassignedPurchase<0 && overlappingProductionSteps(claim,paid);
                    });
                })) continue;
            }
            if(conflicts(path,committed ? o.factory : -1,intercepted,revisedStops))
            {
                if(post)return refuse("FUNDED_POST_DEPLOYMENT_TRAFFIC_NEEDS_REPLAY",condition->choice.post,condition->choice.buildTick+2);
                continue;
            }
            auto revised=balances;
            bool solvent=true;
            for(int who=0;who<2;++who)
            {
                MilliFunds delta=0;
                for(int day=0;day<horizon;++day)
                {
                    const int payday=2*day+who;
                    if(payday>0 && payday<=last)
                    {
                        const auto addition=productionIncomeChange(o,properties,payday,who);
                        delta+=settleSchedule ? std::min<MilliFunds>(0,addition) : addition;
                    }
                    if(!committed && day==o.day && who==o.side) delta-=o.unit->cost;
                    revised[who][day]+=delta;
                    if(revised[who][day]<0) solvent=false;
                }
            }
            if(!solvent) continue;
            balances=std::move(revised);
            if(committed)
            {
                assignedOpening.insert(o.factory);
                releasePlaceholder(o.factory);
            }
            else --slotsLeft[o.side];
            usedOpenings.insert(o.access);
            if(shop>=0) usedSites[shop].emplace_back(atPurchase,buyer);
            if(modified>=0) changedProperties[modified]=&o;
            if(intercepted>=0) captureStops[intercepted]=std::move(revisedStops);
            const int gun=o.targetUnit==NO_UNIT && !post ? NO_UNIT : static_cast<int>(terrain.units.size()+result.fundedGuns.size());
            for(auto step:path)
            {
                step.actor=gun;
                traffic[step.cell].push_back(step);
            }
            if(opening.vacater!=NO_UNIT && opening.exit!=INVALID_TILE)
            {
                auto it=std::find_if(terrain.units.begin(),terrain.units.end(),[&](const auto &u){return u.engineUnitId==opening.vacater;});
                if(it!=terrain.units.end())
                {
                    const int actor=static_cast<int>(it-terrain.units.begin());
                    const int tick=2*opening.buildDay+o.side;
                    if(vacaterAt.emplace(std::pair{cell(opening.exit),tick},actor).second)
                        result.departures.push_back({actor,tick,opening.exit});
                }
            }
            if(post || o.targetUnit!=NO_UNIT)
            {
                const bool needsSetUp=!o.unit->mobileFire && o.destination!=board.sites[o.factory].tile;
                const int remaining=remainingDeploymentFuel(*o.unit,board.sites[o.factory].tile,*o.route,terrain.width);
                result.fundedGuns.push_back({o.unit,board.sites[o.factory].tile,o.destination,o.side,2*o.day+o.side,
                    2*(o.arrival+int(needsSetUp || post.has_value()))+o.side,post?-1:o.impact,o.targetUnit,o.exposure,
                    post?INVALID_TILE:o.forecastTargetProperty>=0 ? properties[o.forecastTargetProperty].tile : positions[o.targetUnit],remaining,
                    o.exposureBeforeFiring});
            }
            if(o.targetUnit!=NO_UNIT) hit.insert(o.targetUnit);
            if(o.property>=0) booked.insert(o.property);
            if(o.forecastTargetProperty>=0) booked.insert(o.forecastTargetProperty);
            apply(o,&result.flow);
            result.net+=(o.side ? -1 : 1)*orderValue;
            receipts.push_back(QJsonObject{{"enemy",bool(o.side)},{"type",o.unit->type},{"factory",tile(board.sites[o.factory].tile)},
                {"buildDay",o.day},{"firstActionDay",o.day+1},{"firstStop",tile(o.firstStop)},{"end",tile(o.destination)},
                {"impactHalfTurn",o.impact},{"kind",post?"DEPLOY":o.property>=0 ? "CAPTURE" :
                    o.forecastTargetProperty>=0 ? "INTERCEPT_CAPTURE" : "FIRE"},
                {"targetTile",o.forecastTargetProperty<0 ? QJsonArray{} : tile(properties[o.forecastTargetProperty].tile)},
                {"delayedCaptureHalfTurn",o.deniedUntil},
                {"target",o.targetUnit==NO_UNIT ? NO_UNIT : terrain.units[o.targetUnit].engineUnitId},
                {"buildHalfTurn",2*o.day+o.side},{"firstStepHalfTurn",2*(o.day+1)+o.side},
                {"price",static_cast<qint64>(o.unit->cost)},
                {"newArmyCapital",static_cast<qint64>(o.unit->cost)},{"vacater",o.day==opening.buildDay ? opening.vacater : NO_UNIT},
                {"vacaterEnd",tile(o.day==opening.buildDay ? opening.exit : INVALID_TILE)}, {"reliefExit",tile(o.reliefExit)},
                {"priorCaptureDay",o.property < 0 ? -1 : properties[o.property].existingCompletion},
                {"priorArrivalDay",o.property < 0 ? -1 : properties[o.property].existingEntry},
                {"exposure",static_cast<qint64>(o.exposure)},{"targetDamage",static_cast<qint64>(o.damage)},
                {"net",static_cast<qint64>(orderValue)}});
            if(dependsOnStockCapture){
                const auto &source=properties[shop];const int parent=source.capturer;
                receipts.back()["stockFactoryParent"]=QJsonObject{{"actor",parent},{"unit",terrain.units[parent].engineUnitId},
                    {"proposedEntry",2*source.existingEntry+o.side},{"proposedFinish",2*source.existingCompletion+o.side},
                    {"proposedVacancy",2*opening.buildDay+o.side}};
            }
            if(post)placed=true;
        }
        for(const auto &[index,paid]:purchases)
            if(!assignedOpening.contains(index))
            {
                TilePoint stop=paid->factory;
                const auto &menu=board.sites[index].menu[0];
                const auto matched=std::find_if(menu.begin(),menu.end(),[&](const auto &u){return u.type==paid->type && u.cost==paid->price;});
                if(matched==menu.end()) return refuse("FIXED_UNIT_MISSING",paid->factory,0);
                const auto &unit=index==damagedFactory ? *result.damagedOpening : *matched;
                const bool armed=std::any_of(unit.attack.begin(),unit.attack.end(),[](double power){return power>0;});
                const int recruit=armed?static_cast<int>(terrain.units.size()+result.fundedGuns.size()):NO_UNIT;
                int fuel=unit.initialFuel;
                const bool changedIdle=condition && !placed && condition->choice.side==0 && condition->choice.buildTick==0 &&
                    condition->choice.factory==paid->factory && condition->choice.type==paid->type && condition->original["kind"]=="DEPLOY";
                if(changedIdle)
                {
                    ProductionAccess opening{0,0};
                    ProductionOrder move{index,0,NO_UNIT,-1,0,2,stop,stop,&unit,0,0,1,nullptr,&opening};
                    ProductionOrder revised=move;
                    if(!armed)return refuse("FUNDED_POST_UNARMED_PURCHASE",paid->factory,0);
                    if(auto reason=makePostOrder(move,revised))return refuse(reason,paid->factory,0);
                    const auto path=productionReservations(revised,paid->factory,terrain.width,last,false);
                    if(conflicts(path,index))return refuse("FUNDED_POST_DEPLOYMENT_TRAFFIC_NEEDS_REPLAY",condition->choice.post,2);
                    releasePlaceholder(index);
                    for(auto step:path){step.actor=recruit;traffic[step.cell].push_back(step);}
                    fuel=remainingDeploymentFuel(unit,paid->factory,*revised.route,terrain.width);
                    stop=condition->choice.post;placed=true;
                }
                else if(needsFirstDeparture(index))
                {
                    // An idle newborn cannot occupy the factory throughout a
                    // forecast which also sends a unit there. Price one real,
                    // safe F1 relocation. It earns no shot, capture or free unit.
                    if(last<2) return refuse("NO_FIRST_ACTIVATION",paid->factory,2);
                    for(const auto &route:unit.routes)
                    {
                        if(!terrain.pay(1+static_cast<std::int64_t>(route.cells.size()+positions.size()))) return refuse("FIRST_MOVE_WORK",paid->factory,2);
                        if(route.activations!=1 || route.destination==paid->factory) continue;
                        const auto to=route.destination;
                        const int end=cell(to), property=propertyAt[end];
                        if(occupiedAt(end,1,0) ||
                           std::find(occupiedAfterE0.begin(),occupiedAfterE0.end(),to)!=occupiedAfterE0.end() ||
                           (property>=0 && properties[property].existingEntry>=0 &&
                            2*properties[property].existingEntry+int(properties[property].existingEnemy)<=last)) continue;
                        bool safe=true;
                        for(std::size_t k=0;k<positions.size() && safe;++k)
                        {
                            if(!terrain.units[k].enemy) continue;
                            const auto origin=k<occupiedAfterE0.size() ? occupiedAfterE0[k] : positions[k];
                            for(TilePoint from:{origin,positions[k]})
                            {
                                if(from==INVALID_TILE) continue;
                                if(std::find(route.cells.begin(),route.cells.end(),cell(from))!=route.cells.end() ||
                                   (unit.incoming[k]>0 && reinforcementExposure(terrain,knowledge.units()[k],terrain.units[k],from)[end]))
                                { safe=false; break; }
                            }
                        }
                        if(!safe) continue;
                        ProductionAccess opening{0,0};
                        ProductionOrder move{index,0,NO_UNIT,-1,0,2,to,to,&unit,0,0,1,&route,&opening};
                        const auto path=productionReservations(move,paid->factory,terrain.width,last,false);
                        if(conflicts(path,index)) continue;
                        releasePlaceholder(index);
                        for(auto step:path) {step.actor=recruit;traffic[step.cell].push_back(step);}
                        fuel=remainingDeploymentFuel(unit,paid->factory,route,terrain.width);
                        stop=to;
                        break;
                    }
                    // A speculative later stock trip cannot evict a paid unit. If
                    // only that trip wanted the square, let the actual route pass
                    // include the retained body before assigning any income.
                    if(stop==paid->factory && (!settleSchedule || (last>=2 && occupiedAt(cell(paid->factory),1,0))))
                        return refuse("NO_COMPATIBLE_FIRST_STOP",paid->factory,2);
                }
                if(armed)
                {
                    if(stop==paid->factory)
                        for(auto &step:traffic[cell(stop)]) if(step.unassignedPurchase==index) step.actor=recruit;
                    result.fundedGuns.push_back({&unit,paid->factory,stop,0,0,
                        (changedIdle || (!unit.mobileFire && stop!=paid->factory))?4:2,-1,NO_UNIT,0,INVALID_TILE,fuel});
                }
                receipts.push_back(QJsonObject{{"enemy",false},{"type",paid->type},{"factory",tile(paid->factory)},
                    {"buildDay",0},{"firstActionDay",1},{"firstStop",tile(stop)},{"end",tile(stop)},
                    {"relocatedForForecastOccupant",stop!=paid->factory},
                    {"buildHalfTurn",0},{"firstStepHalfTurn",2},{"impactHalfTurn",2},{"kind","DEPLOY"},
                    {"price",static_cast<qint64>(paid->price)},{"newArmyCapital",static_cast<qint64>(paid->price)},
                    {"target",NO_UNIT},{"targetTile",QJsonArray{}},{"vacater",NO_UNIT},{"vacaterEnd",tile(INVALID_TILE)},
                    {"reliefExit",tile(INVALID_TILE)},{"priorCaptureDay",-1},{"priorArrivalDay",-1},{"delayedCaptureHalfTurn",-1},
                    {"targetDamage",0},{"exposure",0},{"net",0}});
            }
        // A contested property can be awarded by arrival rather than purchase
        // order. Report the final, chronologically funded purchase sequence.
        std::stable_sort(receipts.begin(),receipts.end(),[](const QJsonObject &a,const QJsonObject &b) {
            return a["buildHalfTurn"].toInt()<b["buildHalfTurn"].toInt();
        });
        for(auto it=receipts.rbegin();it!=receipts.rend();++it)
        {
            auto &bank=balances[int((*it)["enemy"].toBool())][(*it)["buildDay"].toInt()];
            (*it)["treasuryAfter"]=static_cast<qint64>(bank);
            bank+=(*it)["price"].toInteger();
            (*it)["treasuryBefore"]=static_cast<qint64>(bank);
        }
        for(const auto &[square,uses]:traffic)
            for(const auto &use:uses)
            {
                int actor=use.actor;
                auto real=vacaterAt.find({square,use.first});
                if(real!=vacaterAt.end() && use.stopped && use.last==last) actor=real->second;
                result.traffic.push_back({square,use.first,use.last,use.side,actor,use.stopped});
            }
        for(const auto &receipt:receipts) result.orders.append(receipt);
        for(const auto &[key,founder]:paidFounders){
            const auto [f,side]=key;
            const auto source=board.sites[founder->factory].tile,dest=board.sites[f].tile;
            QJsonObject gap{{"scope","PAID_CAPTURER_FUTURE_PRODUCTION_UNPRICED"},{"side",side},{"site",tile(dest)},
                {"founderType",founder->unit->type},{"founderSource",tile(source)},{"founderBuildTick",2*founder->day+side},
                {"proposedCaptureTick",founder->impact},{"earliestLaterBuyTick",founder->impact+2},
                {"minimumBuildPrice",static_cast<qint64>(board.sites[f].minimumBuildCost[side])}};
            for(const auto &receipt:receipts)if(receipt["kind"]=="CAPTURE" &&
                int(receipt["enemy"].toBool())==side && receipt["end"]==tile(dest)){
                gap["retainedFounder"]=QJsonObject{{"type",receipt["type"]},{"source",receipt["factory"]},
                    {"buildTick",receipt["buildHalfTurn"]},{"captureTick",receipt["impactHalfTurn"]},
                    {"projectedExit",receipt["reliefExit"]}};
                break;
            }
            result.paidFounderWindows.append(gap);
        }
        if(condition && !placed)return refuse("FUNDED_POST_PURCHASE_OR_ACCESS_NOT_RETAINED",condition->choice.factory,condition->choice.buildTick);
        return result;
    }

    inline std::optional<ProductionProjection> projectFundedProduction(
        const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge, std::span<const ProductionAccess> access,
        std::span<const ProductionProperty> properties, std::span<const TilePoint> positions, std::span<const int> hp,
        std::span<const int> staysThrough, const std::vector<int> &occupied, int points, int horizon, int last,
        TilePoint homeHQ, TilePoint awayHQ, std::span<const int> clearedAfterE1={},
        std::span<const TilePoint> occupiedAfterF0={},std::span<const TilePoint> occupiedAfterE0={},
        bool settleSchedule=false,QJsonObject *failure=nullptr,const OpeningRecruitFight *openingFight=nullptr,
        const OpeningRepairBill *commonEnemyOpeningRepairs=nullptr,const FundedPostCommitment *commitment=nullptr,const FriendlyServiceResult *friendlyService=nullptr,bool priceCapturedSources=false)
    {
        const auto run=[&](const FundedPostReplay *replay){return selectFundedProduction(terrain,knowledge,access,properties,
            positions,hp,staysThrough,occupied,points,horizon,last,homeHQ,awayHQ,clearedAfterE1,occupiedAfterF0,occupiedAfterE0,
            settleSchedule,failure,openingFight,commonEnemyOpeningRepairs,replay,friendlyService,priceCapturedSources);};
        auto normal=run(nullptr);
        if(!normal || !commitment)return normal;
        const auto &q=*commitment;
        const auto refuse=[&](const char *reason)->std::optional<ProductionProjection>{
            if(failure)*failure=QJsonObject{{"phase","FUNDING_PROPOSAL"},{"reason",reason},{"site",QJsonArray{q.factory.x,q.factory.y}},
                                          {"tick",q.buildTick},{"post",QJsonArray{q.post.x,q.post.y}}};
            return {};
        };
        if(q.side<0 || q.side>1 || q.buildTick<0 || q.buildTick%2!=q.side ||
           q.buildTick+2>last || q.type.isEmpty())return refuse("FUNDED_POST_PURCHASE_OUTSIDE_CONTINUATION");
        QJsonObject original;
        for(const auto &value:normal->orders)if(q.matches(value.toObject()))
        {
            if(!original.isEmpty())return refuse("FUNDED_POST_AMBIGUOUS_PURCHASE");
            original=value.toObject();
        }
        if(original.isEmpty())return refuse(normal->limited?"FUNDED_POST_PURCHASE_SEARCH_WORK_LIMIT":"FUNDED_POST_ORIGINAL_PURCHASE_ABSENT");
        if(original["firstStepHalfTurn"].toInt(-1)!=q.buildTick+2 || original["firstStop"]!=original["end"] ||
           original["kind"]=="CAPTURE")return refuse("FUNDED_POST_ORIGINAL_ITINERARY_NOT_SINGLE_DEPLOYMENT");
        const auto at=QJsonArray{q.post.x,q.post.y};
        if(original["firstStop"].toArray()!=at)
        {
            const FundedPostReplay replay{q,original};
            auto changed=run(&replay);if(!changed)return {};
            if(!unchangedFundedPostPrefix(*normal,*changed,q.buildTick+2))return refuse("FUNDED_POST_PRECOMMITMENT_PREFIX_CHANGED");
            changed->limited|=normal->limited;
            normal=std::move(changed);
        }
        auto gun=std::find_if(normal->fundedGuns.begin(),normal->fundedGuns.end(),[&](const auto &g){
            return g.side==q.side && g.factory==q.factory && g.buildTick==q.buildTick && g.destination==q.post &&
                   g.unit && g.unit->type==q.type;});
        if(gun==normal->fundedGuns.end())return refuse("FUNDED_POST_UNVERIFIED_GUN_OR_POST");
        for(int i=0;i<normal->orders.size();++i)
        {
            auto o=normal->orders[i].toObject();if(!q.matches(o))continue;
            if(o["end"].toArray()!=at || o["firstStop"].toArray()!=at)return refuse("FUNDED_POST_ORDER_CHANGED");
            o["conditionedFundedPost"]=true;o["originalPost"]=original["firstStop"];
            o["roundsAtBirth"]=QJsonArray{gun->unit->captureLoad[0],gun->unit->captureLoad[1]};
            if(gun->unit->initialFuel>=0 && gun->fuelAtPost>=0)o["deploymentFuelSpent"]=gun->unit->initialFuel-gun->fuelAtPost;
            normal->orders[i]=o;
            return normal;
        }
        return refuse("FUNDED_POST_ORDER_MISSING");
    }
}
