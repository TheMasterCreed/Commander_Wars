#pragma once

#include "ai/coordinator/captureapproachsurvival.h"
#include "ai/coordinator/stockcaptureroute.h"
#include "ai/coordinator/townconditionalgun.h"
#include "ai/coordinator/townarrivalceiling.h"

namespace Coordinator
{
    struct TownAssaultRight
    {
        int readyTick{-1}, lastFreeTick{-1}, fuel{-1};
        MilliFunds displacedValue{};
        int mobileFrom{-1};
    };

    struct PaidTownCalendarCache;
    struct TownAssaultBoard
    {
        const CombatTerrain &terrain;
        std::span<const KnownUnit> units;
        const StockCaptureTraffic &traffic;
        std::span<const TilePoint> positions;
        std::span<const double> hp;
        std::span<const std::array<int,2>> load;
        std::span<const TownAssaultRight> rights;
        std::span<const FundedCaptureGun> recruits;
        // Returning INVALID_TILE means there is no proved occupant at this tick.
        std::function<TilePoint(int,int)> at;
        // Supplied only for currently unassigned paid recruits. A true result
        // covers every activation from readyTick through the proposed shot.
        std::function<bool(int,int,int)> recruitFree;
        int threshold{}, horizon{}, cutoff{};
        // The settlement can exclude a collateral victim whose future orders
        // were already priced, just as it excludes a booked town occupant.
        std::function<bool(int,int)> canRemove;
        // Terrain does not change between the mutually exclusive assaults
        // priced on this board. HP, ammunition and movement traffic still do.
        mutable std::map<int,int> minimumEntryCost;
        std::span<const TownAssaultRight> beforeConditionalDuty;
        const std::map<int,std::vector<TownGunClaim>> *conditionalClaims{};
        std::function<bool(int,int)> canLoseReactionGun;
        std::shared_ptr<PaidTownCalendarCache> fundedGuardCache;
        std::function<TownWithdrawalJob(int,TilePoint,int,double,std::int64_t &)> withdrawalJob;
        std::span<const ProductionProperty> postEntryProperties;
    };

    struct TownAssaultTarget
    {
        PropertyStockColumn property;
        int defendingSide{}, occupant{NO_UNIT}, occupiedFrom{}, occupiedThrough{}, requiredCapper{NO_UNIT};
    };

    struct TownAssaultMove
    {
        int actor{NO_UNIT}, side{}, fromTick{}, actionTick{};
        TilePoint origin{INVALID_TILE}, destination{INVALID_TILE};
        StockCapturePath path;
        bool operator==(const TownAssaultMove &) const = default;
    };

    struct TownAssaultHit
    {
        int actor{NO_UNIT}, target{NO_UNIT}, tick{}, weapon{-1}, counterWeapon{-1};
        TilePoint from{INVALID_TILE}, to{INVALID_TILE};
        double actorBefore{}, actorAfter{}, targetBefore{}, targetAfter{};
        MilliFunds targetLoss{}, counterLoss{};
        bool operator==(const TownAssaultHit &) const = default;
    };

    struct TownAssaultLine
    {
        int side{}, blocker{NO_UNIT}, capper{NO_UNIT}, entryTick{-1}, finishTick{-1};
        TilePoint town{INVALID_TILE};
        double capperHp{};
        bool stoppedBeforeEntry{};
        std::vector<TownAssaultMove> moves;
        std::vector<TownAssaultHit> shots;
        // Positive amounts are losses for Coordinated, negative losses for the foe.
        std::vector<std::pair<int,MilliFunds>> losses;
        std::vector<std::pair<int,int>> installments; // own tick, resulting carry
        bool operator==(const TownAssaultLine &) const = default;
    };

    struct TownAssaultAlternatives
    {
        std::optional<TownAssaultLine> assault, response;
        MilliFunds declineValue{}, assaultValue{}, responseValue{};
        std::optional<MilliFunds> bestUnanswered;
        std::map<std::pair<int,int>,TownArrivalCeiling> arrivalsRefuted;
        // Partial values are evidence only and never populate the rank/bound cache.
        std::map<std::pair<int,int>,TownArrivalCeiling> partialArrivalActions;
        bool selectsAssault{}, selectsResponse{}, workLimited{}, unresolved{};
        std::set<int> checkedCappers;
        struct Intention {TownAssaultMove move;int finish;MilliFunds score;};
        std::map<std::pair<int,int>,Intention> intentions;
        QJsonArray focusedAttempts;
        QJsonArray conditionalReactions;
        QJsonArray stagingResponses;
        QJsonArray unpricedPaidArrivals;
        QJsonArray ordinaryCounterChecks;
        bool competingReaction{};
        struct
        {
            int ticks{}, eligibleDates{}, lanes{}, guardedLanes{}, firstHits{}, clears{}, capperRoutes{}, fullyPriced{};
            int dominatedAtArrival{};
        } search;
    };

    // Supplies the same existing horizon-valued material/income comparison
    // for the declined attack, uncountered attack and physical counter.
    using TownAssaultValue=std::function<std::optional<MilliFunds>(const TownAssaultLine&)>;
}
#include "ai/coordinator/townarmedpostentry.h"
namespace Coordinator
{

    struct TownGuardVolleyHistory
    {
        struct Step
        {
            CaptureSurvivalState survival;
            std::map<int,TilePoint> targetAt;
            int lastMove{-1};
            bool stopped{};
        };
        // Key is the last opponent activation actually simulated. A second
        // observation of the same stay restores its wounds and spent rounds.
        std::map<int,Step> onward;
    };

    namespace TownAssaultDetail
    {
        inline int cell(const CombatTerrain &t,TilePoint p) {return p.y*t.width+p.x;}
        inline int firstMoveTick(const TownAssaultRight &right,int side)
        {
            const int first=std::max(right.readyTick,right.mobileFrom);
            return first+int(first%2!=side);
        }

        inline void reserve(const CombatTerrain &t,StockCaptureTraffic &traffic,const TownAssaultMove &m,int until)
        {
            int leaves=until+1;
            for (const auto &s:m.path.steps)
                if (s.tile!=m.origin) {leaves=s.tick;break;}
            traffic.vacate(m.actor,cell(t,m.origin),leaves);
            for (const auto &s:m.path.steps)
            {
                for (int sq:s.cells) traffic.reserve(sq,s.tick,s.tick,m.side,m.actor,false);
                traffic.reserve(cell(t,s.tile),s.tick,s.tick+1,m.side,m.actor,true);
            }
            traffic.reserve(cell(t,m.destination),m.actionTick,until,m.side,m.actor,true);
        }

        // A moved indirect needs an earlier completed move and a stationary
        // firing activation. A direct weapon may fire on its arrival.
        inline std::optional<TownAssaultMove> move(const TownAssaultBoard &b,const StockCaptureTraffic &traffic,
            int actor,int side,const MobilityCostGrid &grid,int mp,int fuel,bool ignoresCollision,
            TilePoint origin,TilePoint to,int ready,int tick,bool setup,StockRouteLowerBounds &lower,
            std::int64_t &budget,bool reuseStockRoute=false)
        {
            TownAssaultMove m{actor,side,ready,tick,origin,INVALID_TILE,{}};
            const int arrival=tick-2*int(setup && origin!=to);
            if (ready>arrival || ready<0 || tick>b.cutoff || tick%2!=side || ready%2!=side)
                return m;
            if (origin==to)
            {
                for (int dt=ready;dt<=tick;dt+=2)
                {
                    if (!traffic.open(actor,side,cell(b.terrain,to),dt,true) ||
                        !traffic.open(actor,side,cell(b.terrain,to),std::min(dt+1,b.cutoff),true)) return m;
                    m.path.steps.push_back({dt,to,{}});
                }
                m.path.entryTick=arrival;
            }
            else
            {
                const int hold=1+int(setup);
                const auto search=[&]{return stockCaptureRoute(grid,traffic,actor,side,origin,to,mp,fuel,ready,
                    arrival,arrival,hold,budget,INVALID_TILE,&lower,{},ignoresCollision);};
                const StockRouteLowerBounds::DatedRouteKey key{&grid,actor,side,cell(b.terrain,origin),
                    cell(b.terrain,to),mp,fuel,ready,arrival,hold,ignoresCollision};
                auto p=reuseStockRoute?lower.datedRoute(traffic,key,budget,search):search();
                if (!p) return {};
                m.path=std::move(*p);
                if (m.path.entryTick!=arrival) return m;
                if (setup) m.path.steps.push_back({tick,to,{}});
            }
            m.destination=to;
            return m;
        }

        struct State
        {
            std::vector<double> hp;
            std::vector<std::array<int,2>> load;
        };

        inline std::optional<TownAssaultHit> hit(const TownAssaultBoard &b,State &s,int gun,int target,
            TilePoint from,TilePoint to,int tick)
        {
            const auto &t=b.terrain;
            const auto &u=b.units[gun];
            const int d=manhattanDistance(from,to);
            if (s.hp[gun]<=0 || s.hp[target]<=0 || d<=0 || d<u.minRange || d>u.maxRange) return {};
            const auto w=supportWeapon(t,gun,target,d,s.load[gun]);
            if(w.base<=0) return {};
            const auto gunBefore=s.hp[gun],before=s.hp[target];
            const auto rawPrimary=terrainReducedDamage(t.defense(target,to),w.base,
                ceilHpSteps(gunBefore),ceilHpSteps(before));
            const auto damage=engineShotDamage(rawPrimary);
            if (damage<=0) return {};
            const auto after=engineHpAfterReply(before,damage);
            const auto back=after>0 && d==1 && b.units[target].minRange<=1 && b.units[target].maxRange>=1 ?
                supportWeapon(t,target,gun,d,s.load[target]) : LoadedCombatWeapon{};
            const auto counter=back.base>0 ? engineShotDamage(sameActionCounterDamage(t.defense(gun,from),back.base,
                before,rawPrimary,ceilHpSteps(gunBefore))) : 0.;
            const auto gunAfter=engineHpAfterReply(gunBefore,counter);
            spendCombatRound(s.load[gun],w.slot);
            if(back.base>0)spendCombatRound(s.load[target],back.slot);
            s.hp[gun]=gunAfter;s.hp[target]=after;
            return TownAssaultHit{gun,target,tick,w.slot,back.base>0 ? back.slot:-1,from,to,gunBefore,gunAfter,before,after,
                combatCapitalLoss(t.units[target].replacementCost,before,after),
                combatCapitalLoss(t.units[gun].replacementCost,gunBefore,gunAfter)};
        }

        inline void recordHit(TownAssaultLine &line,const TownAssaultHit &shot,int gunSide)
        {
            line.shots.push_back(shot);
            line.losses.emplace_back(shot.tick,(gunSide ? 1 : -1)*(shot.targetLoss-shot.counterLoss));
        }

        inline void charge(TownAssaultLine &line,const TownAssaultBoard &b,const TownAssaultMove &m)
        {
            if (m.actor<static_cast<int>(b.rights.size()) && b.rights[m.actor].displacedValue>0)
                line.losses.emplace_back(m.fromTick,(m.side?-1:1)*b.rights[m.actor].displacedValue);
            line.moves.push_back(m);
        }

        inline bool clock(TownAssaultLine &line,const TownAssaultBoard &b,const StockCaptureTraffic &traffic,
            double health,int carry=0)
        {
            line.installments.clear();line.finishTick=-1;line.capperHp=health;
            const auto &u=b.units[line.capper];
            const auto rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(health),health<=0);
            if (rate<=0) return false;
            for(int tick=line.entryTick;tick<=b.cutoff;tick+=2)
            {
                if(!traffic.open(line.capper,line.side,cell(b.terrain,line.town),tick,true)) return false;
                carry+=std::min(rate,std::max(0,b.threshold-carry));
                line.installments.emplace_back(tick,carry);
                if(carry>=b.threshold){line.finishTick=tick;return true;}
                if (!traffic.open(line.capper,line.side,cell(b.terrain,line.town),std::min(b.cutoff,tick+1),true)) return false;
            }
            return false;
        }

        inline std::vector<TilePoint> firingSquares(const CombatTerrain &t,TilePoint target,TilePoint origin,int lo,int hi)
        {
            std::vector<TilePoint> result;
            const int range=std::min(hi,t.width+t.height);
            for(int y=std::max(0,target.y-range);y<=std::min(t.height-1,target.y+range);++y)
                for(int x=std::max(0,target.x-range);x<=std::min(t.width-1,target.x+range);++x)
                {
                    TilePoint sq{x,y};
                    const int d=manhattanDistance(sq,target);
                    if(d>=std::max(1,lo) && d<=range) result.push_back(sq);
                }
            std::stable_sort(result.begin(),result.end(),[&](auto a,auto b){
                return std::tuple{manhattanDistance(origin,a),a.y,a.x}<std::tuple{manhattanDistance(origin,b),b.y,b.x};
            });
            return result;
        }

        inline std::vector<CaptureDefenderAvailability> defenseRights(const TownAssaultBoard &b,int side)
        {
            std::vector<CaptureDefenderAvailability> out(b.units.size());
            for(int i=0;i<static_cast<int>(out.size());++i)
                if(int(b.terrain.units[i].enemy)==side && b.rights[i].readyTick>=0)
                    out[i]={b.rights[i].readyTick,b.rights[i].fuel,b.rights[i].lastFreeTick>=b.cutoff,b.rights[i].displacedValue};
            return out;
        }

        inline bool openForGun(const TownAssaultBoard &b,const StockCaptureTraffic &traffic,
            int gun,int tick,TilePoint at,bool transit)
        {
            const int n=static_cast<int>(b.units.size());
            const int side=gun<n ? int(b.terrain.units[gun].enemy):b.recruits[gun-n].side;
            if(gun<n && b.rights[gun].mobileFrom>tick && at!=b.positions[gun])return false;
            return traffic.open(gun,side,cell(b.terrain,at),tick,!transit);
        }

        inline bool freeGun(const TownAssaultBoard &b,int gun,int tick)
        {
            const int n=static_cast<int>(b.units.size());
            if(gun<n)
            {
                const auto &r=b.rights[gun];
                return r.readyTick>=0 && tick>=r.readyTick && tick<=r.lastFreeTick &&
                    (!b.at || b.at(gun,tick)==b.positions[gun]);
            }
            const auto &p=b.recruits[gun-n];
            return p.targetUnit==NO_UNIT && b.recruitFree && b.recruitFree(gun-n,tick,tick);
        }

        struct GuardStopProbe
        {
            int nextGun{};
            int expansions{},paidWork{};
            bool retainFields{},scannedGun{};
            std::optional<bool> quiet;
            QJsonArray firstShot;
            std::map<int,CaptureFiringFrontier> fields;
        };

        // The initial milestone certifies a quiet approach. If a staging
        // square is already under a profitable, legal shot, that different
        // combat line requires its own forecast; never assume the escort
        // arrives on its original HP.
        inline std::optional<bool> quietApproach(const TownAssaultBoard &b,const StockCaptureTraffic &traffic,
            const TownAssaultMove &proposed,int strike,std::int64_t &budget,QJsonArray *observed=nullptr,
            const std::function<MilliFunds(int,TilePoint,int,int,double,double)> &captureDelay={},
            bool *otherFiringChoice=nullptr,GuardStopProbe *probe=nullptr)
        {
            if(probe && (otherFiringChoice || captureDelay || proposed.path.steps.size()!=1))return {};
            if(proposed.fromTick>=strike) return true;
            const auto rights=defenseRights(b,1-proposed.side);
            CaptureRouteDefense d{b.terrain,b.units,b.positions,b.hp,b.load,rights,b.recruits,
                [&](int gun,int tick,TilePoint t,bool transit){return openForGun(b,traffic,gun,tick,t,transit);},
                [&](int gun,int tick){return freeGun(b,gun,tick);}};
            d.geometryRevision=[&]{return traffic.generation();};
            d.sharedMinimumEntryCost=&b.minimumEntryCost;
            d.avoidedCaptureIncome=captureDelay;
            d.canLoseGun=[&](int gun,int tick){
                return gun>=static_cast<int>(b.units.size()) || !b.canRemove || b.canRemove(gun,tick);
            };
            for(const auto &s:proposed.path.steps)
            {
                if(s.tick>=strike) break;
                CaptureSurvivalState state{b.hp[proposed.actor],0};
                if(probe)
                {
                    if(probe->quiet.has_value())
                    {
                        if(budget<1)return {};
                        --budget;
                        if(observed)for(const auto &shot:probe->firstShot)observed->append(shot);
                        return probe->quiet;
                    }
                    // This precheck only establishes whether any profitable first
                    // shot exists. The later guard exchange replays the entire volley.
                    const int limit=static_cast<int>(b.units.size()+b.recruits.size());
                    for(;probe->nextGun<limit;++probe->nextGun)
                    {
                        d.singleOpeningGun=probe->nextGun;
                        d.singleGunFields=probe->retainFields?&probe->fields:nullptr;
                        d.singleGunExpansions=probe->retainFields?&probe->expansions:nullptr;
                        const bool resumableStock=probe->retainFields && probe->nextGun<static_cast<int>(b.units.size());
                        d.singleGunScanned=resumableStock?&probe->scannedGun:nullptr;
                        d.singleGunWork=resumableStock?&probe->paidWork:nullptr;
                        if(!advanceCaptureSurvival(d,state,proposed.actor,s.tile,s.tick,false,b.threshold,0,
                            b.units[proposed.actor].hpRounded,budget))return {};
                        probe->fields.clear();probe->scannedGun=false;
                        if(!state.shots.empty())break;
                    }
                    probe->quiet=state.shots.empty();
                }
                else if(!advanceCaptureSurvival(d,state,proposed.actor,s.tile,s.tick,false,b.threshold,0,
                    b.units[proposed.actor].hpRounded,budget)) return {};
                if(!state.shots.empty())
                {
                    if(otherFiringChoice)
                    {
                        *otherFiringChoice=state.shots.size()>1;
                        if(!*otherFiringChoice)
                        {
                            // Start again from the untouched position. A
                            // different mobile lane or gun can change the
                            // capper's wounds, counter and later capture.
                            const auto &first=state.shots.front();
                            CaptureRouteDefense alternate{b.terrain,b.units,b.positions,b.hp,b.load,rights,b.recruits,
                                [&](int gun,int tick,TilePoint tile,bool transit){
                                    if(gun==first.gun && tick==first.shotTick && !transit && tile==first.from)return false;
                                    return openForGun(b,traffic,gun,tick,tile,transit);
                                },[&](int gun,int tick){return freeGun(b,gun,tick);}};
                            alternate.sharedMinimumEntryCost=&b.minimumEntryCost;
                            alternate.avoidedCaptureIncome=d.avoidedCaptureIncome;alternate.canLoseGun=d.canLoseGun;
                            CaptureSurvivalState fresh{b.hp[proposed.actor],0};
                            if(!advanceCaptureSurvival(alternate,fresh,proposed.actor,s.tile,s.tick,false,b.threshold,0,
                                b.units[proposed.actor].hpRounded,budget))return {};
                            *otherFiringChoice=!fresh.shots.empty();
                        }
                    }
                    auto *receipt=probe?&probe->firstShot:observed;
                    if(receipt)for(const auto &hit:state.shots)
                    {
                        QJsonObject fact{{"gun",hit.gun<static_cast<int>(b.units.size())?b.terrain.units[hit.gun].engineUnitId:NO_UNIT},
                            {"tick",hit.shotTick},{"from",QJsonArray{hit.from.x,hit.from.y}},{"targetTile",QJsonArray{s.tile.x,s.tile.y}},
                            {"hpBefore",hit.targetBefore},{"hpAfter",hit.targetAfter},{"shooterHpAfter",hit.shooterAfter},
                            {"damage",static_cast<qint64>(hit.damageValue)},{"counter",static_cast<qint64>(hit.counterValue)}};
                        const int paid=hit.gun-static_cast<int>(b.units.size());
                        if(paid>=0 && paid<static_cast<int>(b.recruits.size()) && b.recruits[paid].unit)
                        {
                            const auto &g=b.recruits[paid];fact["birthSite"]=QJsonArray{g.factory.x,g.factory.y};
                            fact["buildTick"]=g.buildTick;fact["recruit"]=g.unit->type;
                        }
                        receipt->append(fact);
                    }
                    if(probe && observed)for(const auto &shot:probe->firstShot)observed->append(shot);
                    return false;
                }
            }
            return true;
        }

        // Retain the exact stops reached before a route was interrupted.
        // A guard can move even when it cannot move and fire on the same day.
        inline bool settleInterruptedTownMove(const CombatTerrain &terrain,const CombatPawn &pawn,
                                              int lastMove,TownAssaultMove &move)
        {
            const int profile=pawn.grid>=0?pawn.grid:pawn.travelGrid;
            if(profile<0 || profile>=static_cast<int>(terrain.grids.size()))return false;
            const auto &grid=terrain.grids[profile];
            if(grid.width()!=terrain.width || grid.height()!=terrain.height || !grid.onGrid(move.origin.x,move.origin.y))return false;
            auto &steps=move.path.steps;
            const auto end=std::find_if(steps.begin(),steps.end(),[&](const auto &s){return s.tick>lastMove;});
            if(end==steps.begin() || std::prev(end)->tick!=lastMove)return false;
            TilePoint prev=move.origin;
            int fuel=0;
            for(auto it=steps.begin();it!=end;++it)
            {
                for(int sq:it->cells)
                {
                    if(sq<0 || sq>=terrain.width*terrain.height)return false;
                    const TilePoint at{sq%terrain.width,sq/terrain.width};
                    if(manhattanDistance(prev,at)!=1)return false;
                    const int cost=grid.cost(at.x,at.y,entryDirection(prev.x,prev.y,at.x,at.y));
                    if(cost<0 || fuel>std::numeric_limits<int>::max()-cost)return false;
                    fuel+=cost;prev=at;
                }
                if(prev!=it->tile)return false;
            }
            steps.erase(end,steps.end());
            move.destination=prev;move.actionTick=move.path.entryTick=lastMove;move.path.fuel=fuel;
            return true;
        }

        inline std::optional<TownAssaultLine> counterLine(const TownAssaultBoard &b,const TownAssaultLine &attack,
            const State &after,std::span<const TilePoint> positions,const StockCaptureTraffic &traffic,
            std::span<const FundedCaptureGun> paid,const TownAssaultValue &value,
            const TownAssaultMove *deployment,StockRouteLowerBounds &lower,std::int64_t &budget,
            const TownAssaultMove *approach=nullptr,int fireFrom=0,int lastGuardReply=-1,TownGuardVolleyHistory *guardHistory=nullptr)
        {
            auto line=attack;
            line.installments.clear();line.finishTick=-1;
            int cap=line.capper,n=static_cast<int>(b.units.size());
            auto rights=defenseRights(b,1-line.side);
            // Stationary surviving guns leave the incoming reservations intact.
            std::optional<StockCaptureTraffic> alteredTraffic;
            const auto replyTraffic=[&]() -> const StockCaptureTraffic & {
                return alteredTraffic ? *alteredTraffic : traffic;
            };
            std::vector<TilePoint> posts(positions.begin(),positions.end());
            for(const auto &g:paid)posts.push_back(g.destination);
            CaptureRouteDefense d{b.terrain,b.units,positions,after.hp,after.load,rights,paid,
                [&](int gun,int tick,TilePoint tile,bool transit){
                    if(gun>=n && !transit && tile!=paid[gun-n].destination) return false;
                    return openForGun(b,replyTraffic(),gun,tick,tile,transit);
                },
                [&](int gun,int tick){return freeGun(b,gun,tick);}};
            d.geometryRevision=[&]{return replyTraffic().generation();};
            d.sharedMinimumEntryCost=&b.minimumEntryCost;
            d.canLoseGun=[&](int gun,int tick){return gun>=n || !b.canRemove || b.canRemove(gun,tick);};
            d.recordShot=[&](const CaptureRouteShot &shot){
                auto &at=posts[shot.gun];
                if(at==shot.from && shot.shooterAfter>0)return false;
                if(!alteredTraffic)alteredTraffic.emplace(traffic);
                if(at!=INVALID_TILE)alteredTraffic->vacate(shot.gun,cell(b.terrain,at),shot.shotTick);
                at=shot.shooterAfter>0 ? shot.from:INVALID_TILE;
                if(at!=INVALID_TILE)alteredTraffic->reserve(cell(b.terrain,at),shot.shotTick,b.cutoff,1-line.side,shot.gun,true);
                return true;
            };
            d.avoidedCaptureIncome=[&](int,TilePoint at,int ownTick,int carry,double before,double wounded){
                if(lastGuardReply>=0)return MilliFunds{};
                const auto &u=b.units[cap];
                auto cash=[&](double health){
                    auto projected=line;
                    const int rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(health),health<=0);
                    projected.finishTick=-1;
                    if(rate>0)
                    {
                        const int next=std::max(ownTick+2,attack.entryTick);
                        const int held=at==line.town && ownTick>=attack.entryTick?carry:0;
                        const auto due=next+2*(widenedCaptureTurns(held,rate,b.threshold)-1);
                        if(due<=b.cutoff) projected.finishTick=static_cast<int>(due);
                    }
                    return value(projected);
                };
                auto a=cash(before),c=cash(wounded);
                return a && c ? std::max<MilliFunds>(0,(line.side ? 1 : -1)*(*c-*a)) : MilliFunds{};
            };
            CaptureSurvivalState capState{after.hp[cap],0};
            std::map<int,TilePoint> targetAt;
            int lastMove=-1,simulated=-1;
            if(guardHistory && lastGuardReply>=0)
            {
                auto it=guardHistory->onward.upper_bound(lastGuardReply);
                if(it!=guardHistory->onward.begin())
                {
                    if(budget<=0)return {};--budget;--it;
                    simulated=it->first;capState=it->second.survival;targetAt=it->second.targetAt;
                    lastMove=it->second.lastMove;line.stoppedBeforeEntry=it->second.stopped;
                    for(const auto &shot:capState.shots)d.recordShot(shot);
                }
            }
            if(approach)
            {
                const auto &pawn=b.terrain.units[cap];
                for(const auto &step:approach->path.steps)
                {
                    if(line.stoppedBeforeEntry)break;
                    if(step.tick+1<=simulated)continue;
                    if(lastGuardReply>=0 && step.tick+1>lastGuardReply)break;
                    if(!replyTraffic().open(cap,line.side,cell(b.terrain,step.tile),step.tick,true) ||
                       !replyTraffic().open(cap,line.side,cell(b.terrain,step.tile),step.tick+1,true) ||
                       (!pawn.ignoresCollision && std::any_of(step.cells.begin(),step.cells.end(),[&](int sq){
                           return !replyTraffic().open(cap,line.side,sq,step.tick,false);
                       }))) {line.stoppedBeforeEntry=true;break;}
                    lastMove=step.tick;
                    if(lastGuardReply<0 && step.tick>=line.entryTick)break;
                    targetAt[step.tick+1]=step.tile;
                    if(step.tick+1<fireFrom)continue;
                    if(!advanceCaptureSurvival(d,capState,cap,step.tile,step.tick,false,b.threshold,b.units[cap].captureRate,
                        b.units[cap].hpRounded,budget))return {};
                    line.stoppedBeforeEntry=capState.hp<=0;
                    if(guardHistory && lastGuardReply>=0)
                        guardHistory->onward.insert_or_assign(step.tick+1,TownGuardVolleyHistory::Step{capState,targetAt,lastMove,line.stoppedBeforeEntry});
                    if(line.stoppedBeforeEntry)break;
                }
                if(line.stoppedBeforeEntry)
                {
                    auto mover=std::find_if(line.moves.begin(),line.moves.end(),[&](const auto &m){return m.actor==cap;});
                    if(mover==line.moves.end() || lastMove<0)return {};
                    if(!settleInterruptedTownMove(b.terrain,pawn,lastMove,*mover))return {};
                }
            }
            for(int tick=line.entryTick;lastGuardReply<0 && !line.stoppedBeforeEntry && tick<=b.cutoff && capState.hp>0 && capState.completedTick<0;tick+=2)
            {
                if(!replyTraffic().open(cap,line.side,cell(b.terrain,line.town),tick,true)) break;
                targetAt[tick+1]=line.town;
                if(!advanceCaptureSurvival(d,capState,cap,line.town,tick,true,b.threshold,b.units[cap].captureRate,
                    b.units[cap].hpRounded,budget)) return {};
                line.installments.emplace_back(tick,capState.carry);
            }
            line.finishTick=capState.completedTick;line.capperHp=capState.hp;
            std::map<int,double> gunHP;
            std::vector<TilePoint> lastPosition(positions.begin(),positions.end());
            bool usedDeployment=false;
            for(const auto &s:capState.shots)
            {
                if (s.alreadyPriced) return {}; // paid jobs are excluded by this milestone
                double hp{};
                if(gunHP.contains(s.gun)) hp=gunHP.at(s.gun);
                else if(s.gun<n) hp=after.hp[s.gun];
                else
                {
                    const auto &g=paid[s.gun-n];
                    hp=paidHpAfterService(g,s.shotTick,g.unit->startingHp-static_cast<int>((std::max<MilliFunds>(0,g.exposure)*10+g.unit->cost-1)/g.unit->cost));
                }
                gunHP[s.gun]=s.shooterAfter;
                if(s.gun<n && s.from!=lastPosition[s.gun])
                {
                    const auto &pawn=b.terrain.units[s.gun];
                    if(pawn.grid<0 || !b.units[s.gun].canMoveAndFire || s.motion.origin!=lastPosition[s.gun] || s.motion.cells.empty())return {};
                    TownAssaultMove moved{s.gun,1-line.side,s.shotTick,s.shotTick,s.motion.origin,s.from,
                        {s.shotTick,s.motion.fuel,{{s.shotTick,s.from,s.motion.cells}}}};
                    line.moves.push_back(std::move(moved));lastPosition[s.gun]=s.from;
                }
                if(s.shooterAfter<=0 && s.gun<n)
                    lastPosition[s.gun]=INVALID_TILE;
                recordHit(line,{s.gun,cap,s.shotTick,s.weapon,s.counterWeapon,s.from,targetAt.at(s.shotTick),hp,s.shooterAfter,
                                s.targetBefore,s.targetAfter,s.damageValue,s.counterValue},1-line.side);
                if(s.gun<n && b.rights[s.gun].displacedValue>0)
                    line.losses.emplace_back(s.shotTick,(line.side ? 1 : -1)*b.rights[s.gun].displacedValue);
                if(deployment && s.gun==deployment->actor && s.from==deployment->destination)
                    usedDeployment=true;
            }
            if(usedDeployment) line.moves.push_back(*deployment);
            return line;
        }

        inline bool cachedTravelPossible(const TownAssaultBoard &board,int actor,TilePoint end,int tick,
            bool setup,const StockRouteLowerBounds &lower);

        inline std::optional<TownAssaultMove> stockMove(const TownAssaultBoard &b,const StockCaptureTraffic &traffic,
            int actor,TilePoint end,int tick,bool setup,StockRouteLowerBounds &lower,std::int64_t &budget)
        {
            const auto &p=b.terrain.units[actor];
            const auto &r=b.rights[actor];
            TownAssaultMove none;
            const int routeGrid=p.grid>=0 ? p.grid : p.travelGrid;
            if(r.readyTick<0 || r.readyTick>tick || r.lastFreeTick<tick || routeGrid<0 ||
                (end!=b.positions[actor] && r.lastFreeTick<b.cutoff)) return none;
            if(!cachedTravelPossible(b,actor,end,tick,setup,lower))return none;
            if(end!=b.positions[actor])
                for(int at=tick+1;at<=b.cutoff;++at)
                    if(!traffic.open(actor,p.enemy,cell(b.terrain,end),at,true)) return none;
            const int ready=end==b.positions[actor] ? r.readyTick:firstMoveTick(r,int(p.enemy));
            return move(b,traffic,actor,p.enemy,b.terrain.grids[routeGrid],p.movement,r.fuel,p.ignoresCollision,
                        b.positions[actor],end,ready,tick,setup,lower,budget,true);
        }

        inline bool better(MilliFunds a,MilliFunds b,int side){return side ? a<b : a>b;}

        struct Lane
        {
            int actor, side, tick;
            TilePoint end;
            int blocker;
            double baseDamage, projectedDamage;
            int travelDistance;
        };
        struct AttackWorld
        {
            State damage;
            StockCaptureTraffic traffic;
            std::vector<TilePoint> positions;
            TownAssaultLine line;
            std::set<int> used;
        };

        inline void commitShot(const TownAssaultBoard &b,AttackWorld &w,const TownAssaultMove &route,const TownAssaultHit &shot)
        {
            w.used.insert(route.actor);
            charge(w.line,b,route);recordHit(w.line,shot,route.side);
            reserve(b.terrain,w.traffic,route,b.cutoff);
            w.positions[route.actor]=shot.actorAfter>0?route.destination:INVALID_TILE;
            if(shot.actorAfter<=0)w.traffic.vacate(route.actor,cell(b.terrain,route.destination),route.actionTick);
            if(shot.targetAfter<=0)
            {
                w.traffic.vacate(shot.target,cell(b.terrain,shot.to),route.actionTick);
                w.positions[shot.target]=INVALID_TILE;
            }
        }

        inline std::optional<bool> takeLane(const TownAssaultBoard &b,AttackWorld &w,const Lane &lane,
            int victim,TilePoint town,int otherGun,StockRouteLowerBounds &lower,std::int64_t &budget)
        {
            const int i=lane.actor,tick=lane.tick,side=lane.side;
            if(w.used.contains(i) || w.damage.hp[victim]<=0)return false;
            if(lane.blocker!=NO_UNIT && w.damage.hp[lane.blocker]>0 && w.positions[lane.blocker]==lane.end)
            {
                const auto block=lane.end;
                const int guard=lane.blocker;
                if(int(b.terrain.units[guard].enemy)==side || (b.canRemove && !b.canRemove(guard,tick)))return false;
                std::optional<TownAssaultMove> best;
                MilliFunds bestNet{};
                for(int j=0;j<static_cast<int>(b.units.size());++j)
                {
                    const auto &u=b.units[j];
                    if(j==i || j==otherGun || w.used.contains(j) || int(b.terrain.units[j].enemy)!=side ||
                       w.damage.hp[j]<=0 || b.rights[j].readyTick<0 || b.rights[j].readyTick>tick ||
                       !(u.canFire || u.hasWeapons) || (w.damage.load[j][0]==0 && w.damage.load[j][1]==0))continue;
                    for(auto end:firingSquares(b.terrain,block,b.positions[j],u.minRange,u.maxRange))
                    {
                        const auto weapon=supportWeapon(b.terrain,j,guard,manhattanDistance(end,block),w.damage.load[j]);
                        if(weapon.base<=0)continue;
                        const auto wound=engineShotDamage(terrainReducedDamage(b.terrain.defense(guard,block),weapon.base,
                            ceilHpSteps(w.damage.hp[j]),ceilHpSteps(w.damage.hp[guard])));
                        if(engineHpAfterReply(w.damage.hp[guard],wound)>0)continue;
                        auto moved=stockMove(b,w.traffic,j,end,tick,!u.canMoveAndFire,lower,budget);
                        if(!moved)return {};
                        if(moved->destination==INVALID_TILE)continue;
                        auto trial=w.damage;
                        auto shot=hit(b,trial,j,guard,end,block,tick);
                        if(!shot || shot->targetAfter>0 || (shot->actorAfter<=0 && b.canRemove && !b.canRemove(j,tick)))continue;
                        auto quiet=quietApproach(b,w.traffic,*moved,tick,budget);
                        if(!quiet)return {};
                        if(!*quiet)continue;
                        auto net=shot->targetLoss-shot->counterLoss-b.rights[j].displacedValue;
                        if(!best || net>bestNet){best=std::move(*moved);bestNet=net;}
                        break;
                    }
                }
                if(!best)return false;
                auto opening=hit(b,w.damage,best->actor,guard,best->destination,block,tick);
                if(!opening || opening->targetAfter>0)return false;
                commitShot(b,w,*best,*opening);
            }
            auto actual=stockMove(b,w.traffic,i,lane.end,tick,!b.units[i].canMoveAndFire,lower,budget);
            if(!actual)return {};
            if(actual->destination==INVALID_TILE)return false;
            const auto quiet=quietApproach(b,w.traffic,*actual,tick,budget);
            if(!quiet)return {};
            if(!*quiet)return false;
            auto assault=hit(b,w.damage,i,victim,actual->destination,town,tick);
            if(!assault || (assault->actorAfter<=0 && b.canRemove && !b.canRemove(i,tick)))return false;
            commitShot(b,w,*actual,*assault);
            return true;
        }

        // The victim can be removed and a screen can fall during the strike.
        // Use terrain without occupancy only to disprove impossible arrivals.
        // Accepted lanes and capture walks still replay their full calendars.
        inline std::optional<bool> withinTravelBudget(const TownAssaultBoard &b,int actor,TilePoint end,
            int tick,bool setup,StockRouteLowerBounds &lower,std::int64_t &budget)
        {
            const auto &pawn=b.terrain.units[actor];
            const auto &right=b.rights[actor];
            const int index=pawn.grid>=0 ? pawn.grid:pawn.travelGrid;
            if(index<0 || right.readyTick>tick || b.positions[actor]==INVALID_TILE)return false;
            if(end==b.positions[actor])return true;
            if(!cachedTravelPossible(b,actor,end,tick,setup,lower))return false;
            const int ready=firstMoveTick(right,int(pawn.enemy));
            const int activations=1+(tick-ready)/2-int(setup);
            if(activations<=0 || pawn.movement<=0 || right.fuel==0)return false;
            const auto &grid=b.terrain.grids[index];
            const auto *field=lower.costs(grid,cell(b.terrain,end),budget);
            if(!field)return {};
            const auto cost=(*field)[cell(b.terrain,b.positions[actor])];
            return cost!=std::numeric_limits<int>::max() &&
                   cost<=static_cast<std::int64_t>(activations)*pawn.movement &&
                   (right.fuel<0 || cost<=right.fuel);
        }

        inline bool cachedTravelPossible(const TownAssaultBoard &b,int actor,TilePoint end,int tick,
            bool setup,const StockRouteLowerBounds &lower)
        {
            const auto &pawn=b.terrain.units[actor];const auto &right=b.rights[actor];
            const int index=pawn.grid>=0 ? pawn.grid:pawn.travelGrid;
            if(index<0 || b.positions[actor]==INVALID_TILE || right.readyTick>tick)return false;
            if(end==b.positions[actor])return true;
            const int ready=firstMoveTick(right,int(pawn.enemy));
            if(ready>tick)return false;
            const int turns=1+(tick-ready)/2-int(setup);
            if(turns<=0 || pawn.movement<=0 || right.fuel==0 || right.lastFreeTick<b.cutoff)return false;
            const auto capacity=std::min<std::int64_t>(std::int64_t(turns)*pawn.movement,
                right.fuel>=0 ? right.fuel:std::numeric_limits<int>::max());
            const auto &grid=b.terrain.grids[index];
            if(const auto *field=lower.cached(grid,cell(b.terrain,end)))
            {
                const int cost=(*field)[cell(b.terrain,b.positions[actor])];
                return cost!=std::numeric_limits<int>::max() && cost<=capacity;
            }
            // The same immutable map bound was already paid for before these
            // competing teams were quoted. Its zero-cost directions remain valid.
            if(const auto *shared=b.terrain.movementCache.get();shared && shared->minimumEntryCost.contains(&grid))
                return std::int64_t(manhattanDistance(b.positions[actor],end))*shared->minimumEntryCost.at(&grid)<=capacity;
            if(auto it=b.minimumEntryCost.find(index);it!=b.minimumEntryCost.end())
                return std::int64_t(manhattanDistance(b.positions[actor],end))*it->second<=capacity;
            return true;
        }
    }

    // An interrupted option never commits part of a shot. A continuation
    // belongs to one target on one immutable squad, reply, wallet and traffic
    // calendar. It preserves only the outcome before the unfinished option.
    struct EmptyTownCursor
    {
        std::size_t capper{}, blockedCapper{}, origin{}, afterClear{};
        int screen{}, gun{};
        std::vector<int> blocked, crossing;
        std::optional<TownAssaultDetail::AttackWorld> cleared;
        bool triedAssumedArrival{};
    };

    struct TownAssaultProgress
    {
        TownAssaultAlternatives settled;
        MilliFunds bestWorst{};
        std::int64_t unfinishedAllowance{};
        int tick{-1},firstLane{-1},secondLane{-1};
        bool complete{};
        std::optional<EmptyTownCursor> empty;
        int emptySteps{};
        std::vector<int> cappers;
        std::vector<TownAssaultDetail::Lane> lanes;
        std::optional<TownAssaultDetail::AttackWorld> afterFirst;
    };

    inline std::optional<TownAssaultBoard> restoreSameTargetGuns(const TownAssaultBoard &b,int cap,int departure,
        std::vector<TownAssaultRight> &rights,std::vector<int> &restored)
    {
        const int n=static_cast<int>(b.units.size());
        std::optional<TownAssaultBoard> adjusted;
            if(b.conditionalClaims && b.beforeConditionalDuty.size()==b.rights.size())
                for(const auto &[gun,claims]:*b.conditionalClaims)
                {
                    if(gun<0 || gun>=n || b.terrain.units[gun].enemy==b.terrain.units[cap].enemy)continue;
                    const auto &r=b.beforeConditionalDuty[gun];
                    if(r.readyTick<0 || (b.rights[gun].readyTick>=0 && b.rights[gun].readyTick<=r.readyTick))continue;
                    if(std::none_of(claims.begin(),claims.end(),[&](const auto &d){
                        return d.claimant==cap && d.firstShot>departure && d.through>=r.readyTick;
                    }))continue;
                    if(!adjusted){adjusted.emplace(b);rights.assign(b.rights.begin(),b.rights.end());}
                    rights[gun]=r;restored.push_back(gun);
                }
            if(adjusted)
            {
                adjusted->rights=rights;
                adjusted->canRemove=[&b,&restored](int gun,int at){
                    if(std::find(restored.begin(),restored.end(),gun)!=restored.end() && b.canLoseReactionGun)
                        return b.canLoseReactionGun(gun,at);
                    return !b.canRemove || b.canRemove(gun,at);
                };
            }
        return adjusted;
    }

    // Before looking for a route, grant the named soldier a full-health
    // arrival. A withhold-until-entry policy is independent of its route.
    // Only an actual, affordable defender volley can certify this bound.
    inline std::optional<TownArrivalCeiling> boundAssumedTownArrival(const TownAssaultBoard &b,
        const TownAssaultTarget &target,int cap,int when,StockRouteLowerBounds &lower,
        const TownAssaultValue &value,MilliFunds decline,std::int64_t &work,
        std::optional<TownArrivalCeiling> *partial=nullptr)
    {
        using namespace TownAssaultDetail;
        const int n=static_cast<int>(b.units.size()),side=1-target.defendingSide;
        const auto town=target.property.tile;
        if(target.occupant!=NO_UNIT || target.requiredCapper!=b.terrain.units[cap].engineUnitId ||
           target.occupiedFrom!=when || target.occupiedThrough!=when || b.hp[cap]!=10. ||
           b.positions[cap]==INVALID_TILE || b.rights[cap].displacedValue!=0 || b.rights[cap].lastFreeTick<b.cutoff ||
           (b.positions[cap]==town && b.units[cap].capturePoints>0))return {};
        // An adjacent defender may be worth killing even without taking the
        // town. Keep the full clearing search when an independent gun could
        // engage it. Occupied towns always use the normal assault search.
        for(auto dir:ENTRY_DIRECTIONS)
        {
            const auto d=entryStep(dir);const TilePoint p{town.x+d.dx,town.y+d.dy};
            if(p.x<0 || p.y<0 || p.x>=b.terrain.width || p.y>=b.terrain.height)continue;
            for(const auto &use:b.traffic.uses(cell(b.terrain,p)))
            {
                if(work<=0)return {};
                --work;
                if(!use.stopped || use.side==side || use.first>when || use.last<when)continue;
                if(use.actor<0 || use.actor>=n)return {};
                for(int gun=0;gun<n;++gun)
                {
                    if(work<=0)return {};
                    --work;
                    const auto &u=b.units[gun];
                    if(gun==cap || b.hp[gun]<=0 || int(b.terrain.units[gun].enemy)!=side ||
                       !(u.canFire || u.hasWeapons) || b.rights[gun].readyTick<0 ||
                       b.rights[gun].readyTick>when || b.rights[gun].lastFreeTick<when)continue;
                    for(int dist=std::max(1,u.minRange);dist<=std::min(b.terrain.width+b.terrain.height,u.maxRange);++dist)
                        if(supportWeapon(b.terrain,gun,use.actor,dist,b.load[gun]).base>0)return {};
                }
                // A paid escort may have an independently valuable shot.
                for(const auto &paid:b.recruits)
                {
                    if(work<=0)return {};
                    --work;
                    if(paid.side==side && paid.readyTick<=when &&
                       (!paid.unit || use.actor>=static_cast<int>(paid.unit->attack.size()) || paid.unit->attack[use.actor]>0))return {};
                }
            }
        }
        TownAssaultLine landed;landed.town=town;landed.side=side;landed.capper=cap;landed.entryTick=when;
        // Keep the capper's previous reservation in place as an additional
        // obstruction. No defender can rely on a guessed pre-entry vacancy.
        auto traffic=b.traffic;
        traffic.reserve(cell(b.terrain,town),when,b.cutoff,side,cap,true);
        landed.moves.push_back({cap,side,when,when,town,town,{when,0,{{when,town,{}}}}});
        if(!clock(landed,b,traffic,10.) || landed.finishTick==when)return {};
        auto positions=std::vector<TilePoint>(b.positions.begin(),b.positions.end());positions[cap]=town;
        State state{{b.hp.begin(),b.hp.end()},{b.load.begin(),b.load.end()}};
        const auto certify=[&](TownAssaultLine &reply)->std::optional<TownArrivalCeiling>{
            auto proof=townArrivalCeiling(b,landed,reply,value,work,false);
            if(!proof || better(proof->value,decline,side))return {};
            for(int i=0;i<proof->defenders.size();++i)
            {
                const auto &s=reply.shots[i];auto step=proof->defenders[i].toObject();
                step["from"]=QJsonArray{s.from.x,s.from.y};step["hpBefore"]=s.targetBefore;step["hpAfter"]=s.targetAfter;
                step["gunHpAfter"]=s.actorAfter;step["weapon"]=s.weapon;step["counterWeapon"]=s.counterWeapon;
                for(const auto &m:reply.moves)if(m.actor==s.actor && m.actionTick==s.tick)
                {step["movedFrom"]=QJsonArray{m.origin.x,m.origin.y};step["fuel"]=m.path.fuel;}
                proof->defenders[i]=step;
            }
            std::erase_if(reply.moves,[&](const TownAssaultMove &m){return m.actor==cap;});
            proof->assumedArrival=true;proof->witness=std::make_shared<TownAssaultLine>(std::move(reply));
            if(!proof->complete())
            {
                if(partial && !*partial)*partial=std::move(*proof);
                return {};
            }
            return proof;
        };
        // A single stationary lethal shot needs no search for other guns
        // or further activation. Use the same native damage and held round.
        for(int gun=0;gun<n;++gun)
        {
            if(work<=0)return {};
            --work;
            const auto pos=b.positions[gun];
            if(int(b.terrain.units[gun].enemy)==side || b.hp[gun]<=0 || pos==INVALID_TILE ||
               !(b.units[gun].canFire || b.units[gun].hasWeapons) || !freeGun(b,gun,when+1) ||
               !traffic.open(gun,1-side,cell(b.terrain,pos),when+1,true))continue;
            State clash=state;
            auto shot=hit(b,clash,gun,cap,pos,town,when+1);
            if(!shot || shot->targetAfter>0)continue;
            auto reply=landed;
            reply.capperHp=0;reply.finishTick=-1;reply.installments.resize(1);recordHit(reply,*shot,1-side);
            if(b.rights[gun].displacedValue>0)reply.losses.emplace_back(when+1,(side?1:-1)*b.rights[gun].displacedValue);
            if(auto proof=certify(reply))return proof;
        }
        // Try firing without moving first. The defender needs one executable
        // policy. All other defenders may withhold their shot.
        for(bool mobile:{false,true})
        {
            if(work<n)return {};
            work-=n;
            std::vector<TownAssaultRight> rights(b.rights.begin(),b.rights.end());
            bool armed=false;
            for(int gun=0;gun<n;++gun)
            {
                const auto &u=b.units[gun];
                if(int(b.terrain.units[gun].enemy)==side || b.hp[gun]<=0 || !(u.canFire || u.hasWeapons) ||
                   b.positions[gun]==INVALID_TILE || rights[gun].readyTick<0 || rights[gun].lastFreeTick<when+1)
                {rights[gun].readyTick=-1;continue;}
                const int dist=manhattanDistance(b.positions[gun],town);
                if(!mobile && (dist<std::max(1,u.minRange) || dist>u.maxRange ||
                    supportWeapon(b.terrain,gun,cap,dist,b.load[gun]).base<=0))
                {rights[gun].readyTick=-1;continue;}
                if(!mobile)rights[gun].mobileFrom=b.cutoff+1;
                armed=true;
            }
            if(!armed)continue;
            auto replyBoard=b;replyBoard.rights=rights;
            auto counter=counterLine(replyBoard,landed,state,positions,traffic,{},value,nullptr,lower,work);
            if(!counter)return {};
            // This is a defensive contingency only. The hypothetical soldier
            // has no proved approach, scheduled movement, income or ownership.
            if(auto proof=certify(*counter))return proof;
        }
        return {};
    }

    inline bool unexaminedPaidTownEntrant(const TownAssaultBoard &b,int index,const TownAssaultTarget &target,
        int tick,const StockRouteLowerBounds &lower)
    {
        const auto &g=b.recruits[index];const int side=1-target.defendingSide;
        if(g.side!=side || g.buildTick+2>tick || (b.recruitFree && !b.recruitFree(index,tick,tick)))return false;
        if(!g.unit)return true;
        const auto town=target.property.tile;const int sq=TownAssaultDetail::cell(b.terrain,town);
        const auto rate=g.unit->captureRate.find(sq);
        if(rate==g.unit->captureRate.end() || rate->second<=0)return false;
        if(tick+2*(widenedCaptureTurns(0,rate->second,b.threshold)-1)>b.cutoff)return false;
        if(g.factory==town)return true;
        const auto *grid=g.unit->movementGrid.get();
        if(!grid || !grid->onGrid(g.factory.x,g.factory.y))return true;
        const auto *dist=lower.cached(*grid,sq);
        const auto minCost=dist?(*dist)[TownAssaultDetail::cell(b.terrain,g.factory)]:
            std::int64_t(manhattanDistance(g.factory,town))*g.unit->minimumMoveCost;
        // Optimistic continuous travel from birth; an unreachable recruit
        // cannot enter. A potentially reachable one has no stock-unit proof.
        return minCost!=std::numeric_limits<int>::max() && minCost<=std::int64_t((tick-g.buildTick)/2)*g.unit->movement;
    }

    // The preferred gun can owe a shot to another capper. The already
    // priced ordinary response is a different policy with its own rounds,
    // movement and return fire. Check that policy on its actual board.
    inline bool retainOrdinaryTownCounter(TownAssaultAlternatives &result,const TownAssaultBoard &board,
        const TownAssaultLine &attack,const TownAssaultLine &ordinary,const TownAssaultValue &value,
        MilliFunds decline,std::int64_t &work)
    {
        const auto k=std::pair{attack.capper,attack.entryTick};
        if(result.arrivalsRefuted.contains(k))return true;
        if(attack.capper<0 || attack.moves.size()!=1 || !attack.shots.empty() || !attack.losses.empty() ||
           ordinary.shots.empty())return false;
        QString status;
        QJsonArray guns;
        for(const auto &shot:ordinary.shots)
        {
            if(work<=0){status="VERIFICATION_WORK_LIMIT";break;}--work;
            if(shot.actor<0 || shot.actor>=static_cast<int>(board.units.size())){status="FUNDED_OR_UNKNOWN_GUN";break;}
            guns.append(QJsonArray{board.terrain.units[shot.actor].engineUnitId,shot.tick});
            if(shot.tick<=attack.entryTick || shot.to!=attack.town){status="SHOT_ON_APPROACH";break;}
            auto position=board.positions[shot.actor];
            for(const auto &m:ordinary.moves)
            {
                if(work<=0){status="VERIFICATION_WORK_LIMIT";break;}--work;
                if(m.actor==shot.actor && m.actionTick<=shot.tick)position=m.destination;
            }
            if(!status.isEmpty())break;
            if(position!=shot.from){status="NO_RETAINED_FIRING_POSITION";break;}
            if(board.conditionalClaims)
                if(const auto it=board.conditionalClaims->find(shot.actor);it!=board.conditionalClaims->end())
                    for(const auto &debt:it->second)
                    {
                        if(work<=0){status="VERIFICATION_WORK_LIMIT";break;}--work;
                        if(debt.claimant!=attack.capper && debt.through>=shot.tick){status="OTHER_STOCK_GUN_DUTY";break;}
                    }
            if(!status.isEmpty())break;
        }
        auto outcome=[&](const QString &reason){
            if(result.ordinaryCounterChecks.size()<8)result.ordinaryCounterChecks.append(QJsonObject{
                {"capper",board.terrain.units[attack.capper].engineUnitId},{"entryTick",attack.entryTick},
                {"status",reason},{"shots",guns}});
        };
        if(!status.isEmpty()){outcome(status);return false;}
        auto upper=townArrivalCeiling(board,attack,ordinary,value,work);
        if(!upper || TownAssaultDetail::better(upper->value,decline,attack.side))
        {outcome(!upper?"ARRIVAL_OR_WALLET_NOT_CERTIFIED":"PROFITABLE_CONTINUATION_OR_WITHDRAWAL");return false;}
        upper->witness=std::make_shared<TownAssaultLine>(ordinary);
        if(!upper->complete())
        {
            result.partialArrivalActions.insert_or_assign(k,std::move(*upper));
            outcome("UNEXAMINED_POST_ENTRY_ACTION");return false;
        }upper->ordinaryAfterConditional=true;
        outcome("ORDINARY_VOLLEY_INCLUDES_SAFE_WITHDRAWAL");
        result.arrivalsRefuted.emplace(k,std::move(*upper));return true;
    }

    inline TownAssaultAlternatives priceTownAssault(const TownAssaultBoard &board,
        const TownAssaultTarget &target,StockRouteLowerBounds &lower,const TownAssaultValue &value,
        std::int64_t &budget,TownAssaultProgress *progress=nullptr,bool boundNamedArrival=false,
        bool boundEachStockArrival=false,bool recheckEarlyCounter=false)
    {
        using namespace TownAssaultDetail;
        const auto &b=board;
        if(progress && progress->complete)return progress->settled;
        TownAssaultAlternatives result=progress?progress->settled:TownAssaultAlternatives{};
        const int n=static_cast<int>(b.units.size()),victim=target.occupant,side=1-target.defendingSide;
        const auto town=target.property.tile;
        const bool emptyTown=victim==NO_UNIT;
        TownAssaultLine declined;
        declined.side=side;declined.blocker=victim;declined.town=town;
        const auto decline=value(declined);
        if(!decline || b.terrain.units.size()!=b.units.size() || b.positions.size()!=b.units.size() ||
           b.hp.size()!=b.units.size() || b.load.size()!=b.units.size() || b.rights.size()!=b.units.size() ||
           target.defendingSide<0 || target.defendingSide>1 || town.x<0 || town.y<0 || town.x>=b.terrain.width ||
           town.y>=b.terrain.height || !b.at || b.threshold<=0 ||
           (!emptyTown && (victim<0 || victim>=n || b.hp[victim]<=0 || int(b.terrain.units[victim].enemy)!=target.defendingSide)) ||
           !b.terrain.defense ||
           (!b.terrain.weapon && !b.terrain.weaponSlots)) {result.unresolved=true;return result;}
        result.declineValue=*decline;
        MilliFunds bestWorst=progress?progress->bestWorst:0;
        const int resumedTick=progress?progress->tick:-1;
        const int resumedLane=progress?progress->firstLane:-1;
        const int resumedSecond=progress?progress->secondLane:-1;
        const AttackWorld *retryWorld=nullptr;
        auto checkpoint=[&](int tick,int first,int second,const AttackWorld *world=nullptr){
            if(!progress)return;
            progress->settled=result;progress->bestWorst=bestWorst;
            progress->tick=tick;progress->firstLane=first;progress->secondLane=second;
            progress->unfinishedAllowance=budget;retryWorld=world;
        };
        auto spent=[&](){
            if(progress && retryWorld)progress->afterFirst=*retryWorld;
            result.workLimited=true;result.unresolved=true;return result;
        };
        auto consider=[&](TownAssaultLine attack,const State &damage,const StockCaptureTraffic &occupied,
                          std::span<const TilePoint> positions,const TownAssaultBoard &reaction,
                          const std::vector<int> &restored,const TownAssaultMove &capMove,const QJsonArray &firstShots,
                          bool differentStagingGun)->bool {
            const auto base=value(attack);
            if(!base){result.unresolved=true;return true;}
            if(!result.bestUnanswered || better(*base,*result.bestUnanswered,side))result.bestUnanswered=*base;
            auto defended=counterLine(reaction,attack,damage,positions,occupied,b.recruits,value,nullptr,lower,budget);
            if(!defended) return false;
            auto response=value(*defended);
            if(!response){result.unresolved=true;return true;}
            if(emptyTown && better(*response,*base,1-side))
            {
                auto ceiling=townArrivalCeiling(b,attack,*defended,value,budget);
                std::optional<TownAssaultLine> stockCounter;
                const bool boughtGun=std::any_of(defended->shots.begin(),defended->shots.end(),
                    [&](const auto &shot){return shot.actor>=n;});
                if(!ceiling && boughtGun && target.requiredCapper==b.terrain.units[attack.capper].engineUnitId &&
                   target.property.ownerBefore==(side?OwnerSign::Ours:OwnerSign::Enemy) &&
                   target.occupiedFrom==target.occupiedThrough && attack.moves.size()==1 && attack.shots.empty() &&
                   attack.losses.empty() && !better(*response,*decline,side))
                {
                    // The buyer's gun can withhold its shot. Try a different legal defense
                    // on the actual route, with the ordinary guns' original dated rights.
                    // Keep funded occupancy, cash flow and commitments on their calendar.
                    auto work=budget/3;
                    const auto offered=work;
                    stockCounter=counterLine(b,attack,damage,positions,occupied,{},value,nullptr,lower,work);
                    if(stockCounter)
                    {
                        auto alternate=townArrivalCeiling(b,attack,*stockCounter,value,work);
                        if(alternate && alternate->complete() && !better(alternate->value,*decline,side))
                            ceiling=std::move(alternate);
                    }
                    budget-=offered-work;
                }
                if(ceiling && !ceiling->complete())
                {
                    ceiling->witness=std::make_shared<TownAssaultLine>(*defended);
                    result.partialArrivalActions.insert_or_assign({attack.capper,attack.entryTick},std::move(*ceiling));
                }
                else if(ceiling && !better(ceiling->value,*decline,side))
                {
                    const auto key=std::pair{attack.capper,attack.entryTick};
                    ceiling->witness=std::make_shared<TownAssaultLine>(stockCounter?*stockCounter:*defended);
                    result.arrivalsRefuted.insert_or_assign(key,std::move(*ceiling));
                    if(attack.entryTick>capMove.fromTick+2)
                    {
                        // This defender spends no action before entry. A
                        // different unassisted overnight route cannot dodge
                        // its on-town volley or improve on safely withdrawing.
                        ++result.search.dominatedAtArrival;
                        if(!result.assault || better(*response,bestWorst,side))
                        {
                            bestWorst=*response;result.assaultValue=*base;result.responseValue=*response;
                            result.selectsAssault=false;result.selectsResponse=true;
                            result.assault=std::move(attack);result.response=std::move(*defended);
                        }
                        return true;
                    }
                }
            }
            if(!better(*response,*base,1-side)){defended=attack;response=base;}
            std::optional<TownAssaultLine> ordinaryRepulse;
            std::optional<MilliFunds> ordinaryScore;
            bool otherStageChoices=false;
            if(!firstShots.isEmpty())
            {
                const auto start=capMove.fromTick+1;
                // With several wardens or several staging activations, other
                // subsets may matter. A funded reply already worse than
                // declining resolves this specific attack even then.
                otherStageChoices=differentStagingGun || attack.entryTick>capMove.fromTick+2;
                QJsonArray policies;
                policies.append(QJsonObject{{"policy","DECLINE_STAGING_SHOT"},{"score",static_cast<qint64>(*response)},
                    {"finish",defended->finishTick},{"capperHp",defended->capperHp}});
                const auto priceStage=[&](const TownAssaultBoard &defenders,bool ordinary)->bool {
                    auto line=counterLine(defenders,attack,damage,positions,occupied,b.recruits,value,nullptr,lower,budget,&capMove,start);
                    if(!line)return false;
                    const auto amount=value(*line);
                    if(!amount){result.unresolved=true;return true;}
                    QJsonArray hits;
                    auto clips=damage.load;
                    for(const auto &s:line->shots)if(s.target==attack.capper)
                    {
                        QJsonObject hit{{"gun",s.actor<n?b.terrain.units[s.actor].engineUnitId:NO_UNIT},{"tick",s.tick},
                            {"from",QJsonArray{s.from.x,s.from.y}},{"targetTile",QJsonArray{s.to.x,s.to.y}},
                            {"weapon",s.weapon},{"counterWeapon",s.counterWeapon},{"hpBefore",s.targetBefore},{"hpAfter",s.targetAfter},
                            {"gunBefore",s.actorBefore},{"gunAfter",s.actorAfter},{"damage",static_cast<qint64>(s.targetLoss)},
                            {"counter",static_cast<qint64>(s.counterLoss)}};
                        if(s.actor<n)
                        {
                            auto &rounds=clips[s.actor];hit["gunAmmoBefore"]=QJsonArray{rounds[0],rounds[1]};
                            spendCombatRound(rounds,s.weapon);hit["gunAmmoAfter"]=QJsonArray{rounds[0],rounds[1]};
                        }
                        else {const auto &g=b.recruits[s.actor-n];hit["birthSite"]=QJsonArray{g.factory.x,g.factory.y};hit["buildTick"]=g.buildTick;}
                        auto &counter=clips[s.target];hit["targetAmmoBefore"]=QJsonArray{counter[0],counter[1]};
                        spendCombatRound(counter,s.counterWeapon);hit["targetAmmoAfter"]=QJsonArray{counter[0],counter[1]};
                        for(const auto &move:line->moves)if(move.actor==s.actor && move.actionTick==s.tick)
                        {
                            QJsonArray path;
                            for(const auto &step:move.path.steps)for(int sq:step.cells)path.append(QJsonArray{sq%b.terrain.width,sq/b.terrain.width});
                            hit["movedFrom"]=QJsonArray{move.origin.x,move.origin.y};hit["movementFuel"]=move.path.fuel;hit["movementCells"]=path;
                        }
                        hits.append(hit);
                    }
                    policies.append(QJsonObject{{"policy",ordinary?"AVAILABLE_GUNS":"INCLUDING_CONDITIONAL_GUNS"},
                        {"score",static_cast<qint64>(*amount)},{"finish",line->finishTick},{"capperHp",line->capperHp},
                        {"stoppedBeforeEntry",line->stoppedBeforeEntry},{"shots",hits}});
                    if(line->stoppedBeforeEntry && line->capperHp>0){result.unresolved=true;return true;}
                    if(ordinary){ordinaryScore=amount;ordinaryRepulse=*line;}
                    if(better(*amount,*response,1-side)){response=amount;defended=std::move(line);}
                    return true;
                };
                if(!priceStage(b,true) || (!restored.empty() && !priceStage(reaction,false)))return false;
                if(result.stagingResponses.size()<12 || target.requiredCapper!=NO_UNIT)
                    result.stagingResponses.append(QJsonObject{{"capper",b.terrain.units[attack.capper].engineUnitId},{"entryTick",attack.entryTick},
                    {"firstStop",QJsonArray{capMove.path.steps.front().tile.x,capMove.path.steps.front().tile.y}},
                    {"declineScore",static_cast<qint64>(*decline)},{"unansweredScore",static_cast<qint64>(*base)},
                    {"additionalStagingChoices",otherStageChoices},{"policies",policies}});
            }
            // A defending paid recruit may reposition after the town assault
            // is revealed. Earlier movements must already be in the supplied
            // calendar, regardless of whether the attacker goes through.
            for(int k=0;k<static_cast<int>(b.recruits.size());++k)
            {
                const auto &p=b.recruits[k];
                if(p.side==side || !p.unit || !p.unit->movementGrid || p.targetUnit!=NO_UNIT ||
                   p.unit->attack.size()<=static_cast<std::size_t>(attack.capper) ||
                   p.unit->attack[attack.capper]<=0 || !b.recruitFree) continue;
                const int start=std::max(p.readyTick,attack.entryTick+1);
                if(start>=attack.finishTick || !b.recruitFree(k,start,b.cutoff)) continue;
                for(int tick=start;tick<attack.finishTick;tick+=2)
                {
                    bool found=false;
                    for(const auto end:firingSquares(b.terrain,town,p.destination,p.unit->minRange,p.unit->maxRange))
                    {
                        if(end==p.destination) continue;
                        auto route=move(b,occupied,n+k,p.side,*p.unit->movementGrid,p.unit->movement,paidFuelAtRelease(p,start),
                            false,p.destination,end,start,tick,!p.unit->mobileFire,lower,budget);
                        if(!route) return false;
                        if(route->destination==INVALID_TILE) continue;
                        bool futureFree=true;
                        for(int t=tick+1;t<=b.cutoff && futureFree;++t)
                            futureFree=occupied.open(n+k,p.side,cell(b.terrain,end),t,true);
                        if(!futureFree) continue;
                        // A paid reserve which crosses an unpriced hostile
                        // weapon envelope may not claim an unharmed arrival.
                        // Permit moving and shooting on a single activation.
                        bool exposed=false;
                        for(const auto &step:route->path.steps)
                        {
                            if(step.tick>=tick) break;
                            for(int e=0;e<n;++e)
                            {
                                if(int(b.terrain.units[e].enemy)!=side || damage.hp[e]<=0 ||
                                   p.unit->incoming.size()<=static_cast<std::size_t>(e) || p.unit->incoming[e]<=0 ||
                                   !freeGun(b,e,step.tick+1))continue;
                                const auto &gun=b.units[e];const auto &pawn=b.terrain.units[e];
                                if((damage.load[e][0]==0 && damage.load[e][1]==0) || positions[e]==INVALID_TILE)continue;
                                const int distance=manhattanDistance(positions[e],step.tile);
                                if(distance>=gun.minRange && distance<=gun.maxRange){exposed=true;break;}
                                if(gun.canMoveAndFire && pawn.grid>=0 && pawn.movement>0)
                                {
                                    const int allowance=b.rights[e].fuel<0?pawn.movement:std::min(pawn.movement,b.rights[e].fuel);
                                    const auto work=2LL*b.terrain.width*b.terrain.height;
                                    if(budget<work)return false;budget-=work;
                                    const auto reach=DistanceField::build(b.terrain.grids[pawn.grid],{positions[e]},FieldExpansion::FromSources,allowance);
                                    for(auto sq:firingSquares(b.terrain,step.tile,positions[e],gun.minRange,gun.maxRange))
                                        if(reach.reachable(sq.x,sq.y,allowance)){exposed=true;break;}
                                    if(exposed)break;
                                }
                            }
                            if(exposed) break;
                        }
                        if(exposed){result.unresolved=true;continue;}
                        auto trialTraffic=occupied;
                        reserve(b.terrain,trialTraffic,*route,b.cutoff);
                        auto guns=std::vector<FundedCaptureGun>(b.recruits.begin(),b.recruits.end());
                        guns[k].destination=end;guns[k].readyTick=tick;
                        auto reply=counterLine(reaction,attack,damage,positions,trialTraffic,guns,value,&*route,lower,budget);
                        if(!reply) return false;
                        if(reply->moves.size()<=attack.moves.size()) continue;
                        const auto amount=value(*reply);
                        if(!amount){result.unresolved=true;continue;}
                        if(better(*amount,*response,p.side)) {response=amount;defended=std::move(*reply);}
                        found=true;break;
                    }
                    if(found) break;
                }
            }
            if(!restored.empty() && better(*response,*base,1-side))
            {
                QJsonArray shots,otherClaims;
                for(int gun:restored)
                {
                    int first=-1;
                    for(const auto &s:defended->shots) if(s.actor==gun && s.target==attack.capper &&
                       (b.rights[gun].readyTick<0 || s.tick<b.rights[gun].readyTick))
                    {
                        if(first<0)first=s.tick;
                        shots.append(QJsonObject{{"gun",b.terrain.units[gun].engineUnitId},{"tick",s.tick},
                            {"from",QJsonArray{s.from.x,s.from.y}},{"gunBefore",s.actorBefore},{"gunAfter",s.actorAfter},
                            {"hpBefore",s.targetBefore},{"hpAfter",s.targetAfter},{"damage",static_cast<qint64>(s.targetLoss)},
                            {"counter",static_cast<qint64>(s.counterLoss)}});
                    }
                    if(first<0)continue;
                    for(const auto &d:b.conditionalClaims->at(gun))
                    {
                        if(d.through<first || (d.claimant==attack.capper && d.firstShot>capMove.fromTick))continue;
                        otherClaims.append(QJsonObject{{"gun",b.terrain.units[gun].engineUnitId},{"claimant",b.terrain.units[d.claimant].engineUnitId},
                            {"target",QJsonArray{d.target.x,d.target.y}},{"firstShot",d.firstShot},{"through",d.through},{"incomplete",d.incomplete}});
                    }
                }
                if(!shots.isEmpty())
                {
                    if(result.conditionalReactions.size()<12 || target.requiredCapper!=NO_UNIT)
                        result.conditionalReactions.append(QJsonObject{{"capper",b.terrain.units[attack.capper].engineUnitId},
                            {"entryTick",attack.entryTick},{"unchallengedFinish",attack.finishTick},{"challengedFinish",defended->finishTick},
                            {"declineScore",static_cast<qint64>(*decline)},{"unchallengedScore",static_cast<qint64>(*base)},
                            {"responseScore",static_cast<qint64>(*response)},{"shots",shots},{"otherClaims",otherClaims},
                            {"status",otherClaims.isEmpty()?"REPRICED_SAME_CAPPER":"REQUIRES_REPRICING_OTHER_CLAIM"}});
                    if(!otherClaims.isEmpty())
                    {
                        if(!ordinaryScore || better(*ordinaryScore,*decline,side))
                        {
                            auto abstain=counterLine(b,attack,damage,positions,occupied,b.recruits,value,nullptr,lower,budget);
                            if(!abstain)return false;
                            const auto priced=value(*abstain);
                            if(priced && (!ordinaryScore || better(*priced,*ordinaryScore,1-side)))
                            {ordinaryRepulse=std::move(abstain);ordinaryScore=priced;}
                        }
                        if(ordinaryRepulse && !better(*ordinaryScore,*decline,side))
                        {
                            defended=std::move(ordinaryRepulse);response=ordinaryScore;
                        }
                        else {result.unresolved=true;result.competingReaction=true;return true;}
                    }
                }
            }
            if(otherStageChoices && better(*response,*decline,side))
            {result.unresolved=true;return true;}
            if(recheckEarlyCounter && emptyTown && !restored.empty() && !better(*response,*decline,side) &&
               target.requiredCapper==b.terrain.units[attack.capper].engineUnitId && target.occupiedFrom==target.occupiedThrough)
                retainOrdinaryTownCounter(result,b,attack,*defended,value,*decline,budget);
            // A non-stock defender, or intervening staging damage, may
            // prevent validating the simple full-health ceiling entirely.
            // Keep that executable response and the unexamined choice separate.
            if(emptyTown && attack.shots.empty() && !defended->stoppedBeforeEntry && !defended->shots.empty())
            {
                const auto key=std::pair{attack.capper,attack.entryTick};
                TownWithdrawalJob extra;
                const auto [next,hp]=unexaminedCapperAction(b,attack,*defended,budget,extra);
                bool native=b.terrain.units[attack.capper].needsNativeTownExchange;
                for(const auto &s:defended->shots)if(s.actor>=0 && s.actor<n)
                    native|=b.terrain.units[s.actor].needsNativeTownExchange;
                if((next>=0 || native) && !result.arrivalsRefuted.contains(key))
                {
                    auto existing=result.partialArrivalActions.find(key);
                    if(existing==result.partialArrivalActions.end() || existing->second.assumedArrival)
                    {
                        TownArrivalCeiling partial;partial.value=*response;partial.withdrawalCoverage=false;
                        partial.unexaminedActionTick=next;partial.hpAtUnexaminedAction=hp;partial.nativeExchangeRequired=native;partial.alternative=extra;
                        partial.witness=std::make_shared<TownAssaultLine>(*defended);
                        result.partialArrivalActions.insert_or_assign(key,std::move(partial));
                    }
                }
            }
            ++result.search.fullyPriced;
            if(better(*response,*decline,side))
            {
                const auto &actual=better(*response,*base,1-side)?*defended:attack;
                const auto m=std::find_if(actual.moves.begin(),actual.moves.end(),[&](const auto &v){return v.actor==actual.capper;});
                const auto key=std::pair{actual.capper,actual.entryTick};auto prev=result.intentions.find(key);
                if(m!=actual.moves.end() && (prev==result.intentions.end() || better(*response,prev->second.score,side)))
                    result.intentions.insert_or_assign(key,TownAssaultAlternatives::Intention{*m,actual.finishTick,*response});
            }
            if(!result.assault || better(*response,bestWorst,side))
            {
                bestWorst=*response;result.assaultValue=*base;result.responseValue=*response;
                result.selectsResponse=better(*response,*base,1-side);
                result.selectsAssault=better(bestWorst,*decline,side);
                result.assault=std::move(attack);result.response=std::move(*defended);
            }
            return true;
        };
        const auto attackWorld=[&](){
            return AttackWorld{{{b.hp.begin(),b.hp.end()},{b.load.begin(),b.load.end()}},b.traffic,
                {b.positions.begin(),b.positions.end()},declined,{}};
        };
        const auto priceEntry=[&](AttackWorld &world,int cap,int tick,const TownAssaultMove &m,bool &exposed)->bool {
            QJsonObject attempt;
            if(target.requiredCapper!=NO_UNIT)
            {
                QJsonArray stops;for(const auto &s:m.path.steps){QJsonArray cells;for(int x:s.cells)cells.append(QJsonArray{x%b.terrain.width,x/b.terrain.width});
                    stops.append(QJsonObject{{"tick",s.tick},{"tile",QJsonArray{s.tile.x,s.tile.y}},{"cells",cells}});}
                attempt={{"unit",b.terrain.units[cap].engineUnitId},{"entryTick",tick},{"from",QJsonArray{m.origin.x,m.origin.y}},{"stops",stops}};
            }
            // An original capper with a new route is still the same target.
            // Restore its conditional opponent only in this reaction, after
            // the attacking orders have been chosen. Unrelated initiatives
            // and genuinely committed jobs keep the original allocation.
            std::vector<int> restored;
            std::vector<TownAssaultRight> rights;
            auto adjusted=restoreSameTargetGuns(b,cap,m.fromTick,rights,restored);
            const auto &reaction=adjusted?*adjusted:b;
            QJsonArray firstShots;
            const bool cleanOpening=world.line.moves.empty() && world.line.shots.empty();
            const auto captureDelay=[&](int,TilePoint,int,int,double before,double after)->MilliFunds{
                auto worth=[&](double health){
                    auto line=world.line;line.capper=cap;line.entryTick=tick;line.finishTick=-1;
                    const auto &u=b.units[cap];const int rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(health),health<=0);
                    if(rate>0){const auto end=tick+2*(widenedCaptureTurns(0,rate,b.threshold)-1);if(end<=b.cutoff)line.finishTick=static_cast<int>(end);}
                    return value(line);
                };
                const auto a=worth(before),z=worth(after);
                return a && z?std::max<MilliFunds>(0,(side?1:-1)*(*z-*a)):0;
            };
            bool differentStagingGun=false;
            const auto quiet=quietApproach(reaction,world.traffic,m,tick,budget,&firstShots,captureDelay,
                cleanOpening?&differentStagingGun:nullptr);
            if(!attempt.isEmpty()){attempt["approachStatus"]=!quiet?"WORK_LIMITED":*quiet?"REACHED_ENTRY":
                cleanOpening?"STAGING_RESPONSE_REQUIRED":"EXPOSED_APPROACH_UNPRICED";
                if(!firstShots.isEmpty())attempt["approachShots"]=firstShots;result.focusedAttempts.append(attempt);}
            if(!quiet)return false;
            exposed=!*quiet && cleanOpening;
            if(!*quiet && !cleanOpening){result.unresolved=true;return true;}
            ++result.search.capperRoutes;
            auto reserved=world.traffic;
            reserve(b.terrain,reserved,m,b.cutoff);
            auto line=world.line;line.capper=cap;line.entryTick=tick;charge(line,b,m);
            if(!clock(line,b,reserved,world.damage.hp[cap]))return true;
            world.positions[cap]=town;
            const auto resolved=consider(std::move(line),world.damage,reserved,world.positions,reaction,restored,m,firstShots,differentStagingGun);
            world.positions[cap]=b.positions[cap];
            return resolved;
        };
        const auto tryEntry=[&](AttackWorld &world,int cap,int tick,const TownAssaultMove &m)->bool {
            bool exposed=false;
            if(!priceEntry(world,cap,tick,m,exposed))return false;
            const auto bounded=result.arrivalsRefuted.find({cap,tick});
            if(!exposed || (world.line.moves.empty() && world.line.shots.empty() &&
                bounded!=result.arrivalsRefuted.end() && (tick>m.fromTick+2 || bounded->second.ordinaryAfterConditional)))return true;
            if(tick!=m.fromTick+2 || m.path.steps.size()!=2)
            {
                // Combinations of earlier overnight squares on longer trips
                // might avoid the first shot. Keep that choice unpriced.
                result.unresolved=true;return true;
            }
            // At two activations combat can differ only at the intermediate
            // overnight stop. Exhaust the distinct legal stops, or expose
            // the genuine work limit. Fuel and traffic remain dated.
            const auto &pawn=b.terrain.units[cap];
            std::set<int> seen{cell(b.terrain,m.path.steps.front().tile)};
            for(;;)
            {
                const auto allowed=[&](int t,TilePoint at){return t!=m.fromTick || !seen.contains(cell(b.terrain,at));};
                auto route=stockCaptureRoute(b.terrain.grids[pawn.grid],world.traffic,cap,side,m.origin,town,
                    pawn.movement,b.rights[cap].fuel,m.fromTick,tick,tick,1,budget,INVALID_TILE,&lower,allowed,pawn.ignoresCollision);
                if(!route)return false;
                if(route->entryTick!=tick || route->steps.empty())return true;
                if(!seen.insert(cell(b.terrain,route->steps.front().tile)).second)return false;
                TownAssaultMove other{cap,side,m.fromTick,tick,m.origin,town,std::move(*route)};
                if(!priceEntry(world,cap,tick,other,exposed))return false;
            }
        };
        int first=std::max(0,target.occupiedFrom);
        if(first%2!=side)++first;
        for(int tick=std::max(first,resumedTick);tick<=std::min(b.cutoff,target.occupiedThrough);tick+=2)
        {
            const bool resumeLanes=tick==resumedTick && resumedLane>=0;
            std::vector<int> cappers;
            const bool resumeEmpty=emptyTown && tick==resumedTick && progress && progress->empty;
            if(resumeLanes || resumeEmpty)cappers=progress->cappers;
            else
            {
                if(progress)progress->empty.reset();
                checkpoint(tick,-1,-1);
                ++result.search.ticks;
                if(!emptyTown && (b.at(victim,tick)!=town || (b.canRemove && !b.canRemove(victim,tick))))continue;
                for(int cap=0;cap<n;++cap)
                {
                    if(target.requiredCapper!=NO_UNIT && target.requiredCapper!=b.terrain.units[cap].engineUnitId) continue;
                    // The capper can act on its own square. Every other actor's dated
                    // stop still blocks its entry, including stops on the following reply.
                    if(emptyTown && !b.traffic.open(cap,side,cell(b.terrain,town),tick,true)) continue;
                    const auto &unit=b.units[cap];const auto &right=b.rights[cap];
                    if(int(b.terrain.units[cap].enemy)!=side || b.hp[cap]<=0 || !unit.canCapture ||
                       right.readyTick<0 || right.readyTick>tick || right.lastFreeTick<tick)continue;
                    const int rate=replyCaptureRate(unit.captureRate,unit.hpRounded-ceilHpSteps(b.hp[cap]),false);
                    const int last=std::min(b.cutoff,right.lastFreeTick);
                    if(rate<=0 || 2*(widenedCaptureTurns(0,rate,b.threshold)-1)>last-tick)continue;
                    result.checkedCappers.insert(cap);
                    const auto possible=withinTravelBudget(b,cap,town,tick,false,lower,budget);
                    if(!possible)return spent();
                    if(*possible)cappers.push_back(cap);
                }
                if(emptyTown && boundEachStockArrival && target.requiredCapper==NO_UNIT &&
                   target.occupiedFrom==target.occupiedThrough)
                    for(int k=0;k<static_cast<int>(b.recruits.size());++k)
                    {
                        if(budget<=0)return spent();
                        --budget;
                        if(!unexaminedPaidTownEntrant(b,k,target,tick,lower))continue;
                        const auto &g=b.recruits[k];result.unresolved=true;
                        result.unpricedPaidArrivals.append(QJsonObject{{"factory",QJsonArray{g.factory.x,g.factory.y}},
                            {"buildTick",g.buildTick},{"entryTick",tick}});
                    }
                if(cappers.empty())continue;
                ++result.search.eligibleDates;
            }
            if(emptyTown)
            {
                auto world=attackWorld();
                EmptyTownCursor local;
                if(progress && !resumeEmpty)
                {
                    progress->cappers=cappers;
                    progress->empty.emplace();
                }
                auto &cursor=progress?*progress->empty:local;
                auto advanced=[&]{if(progress)++progress->emptySteps;};
                if(!cursor.triedAssumedArrival && target.occupiedFrom==target.occupiedThrough &&
                   (target.requiredCapper!=NO_UNIT?boundNamedArrival:boundEachStockArrival))
                {
                    cursor.triedAssumedArrival=true;
                    auto pool=budget/3;const auto precheck=pool;
                    for(std::size_t i=0;i<cappers.size();++i)
                    {
                        const int cap=cappers[i];
                        auto person=target;person.requiredCapper=b.terrain.units[cap].engineUnitId;
                        auto allowance=pool/static_cast<std::int64_t>(cappers.size()-i);
                        if(allowance<2)continue;
                        const auto offered=allowance;
                        std::optional<TownArrivalCeiling> partial;
                        auto proof=boundAssumedTownArrival(b,person,cap,tick,lower,value,*decline,allowance,&partial);
                        if(partial)result.partialArrivalActions.try_emplace({cap,tick},std::move(*partial));
                        pool-=offered-allowance;
                        if(proof)
                        {
                            result.focusedAttempts.append(QJsonObject{{"unit",b.terrain.units[cap].engineUnitId},{"entryTick",tick},
                                {"approachStatus","ASSUMED_ARRIVAL_REFUTED_BEFORE_ROUTE"},{"actualApproachProved",false},
                                {"onTownDefenders",proof->defenders},{"withdrawals",proof->withdrawals},
                                {"attackerCeiling",static_cast<qint64>(proof->value)}});
                            result.arrivalsRefuted.insert_or_assign({cap,tick},std::move(*proof));
                            ++result.search.dominatedAtArrival;
                        }
                    }
                    budget-=precheck-pool;
                    advanced();
                }
                for(;cursor.capper<cappers.size();++cursor.capper)
                {
                    checkpoint(tick,-1,-1);
                    const int cap=cappers[cursor.capper];
                    const auto refuted=result.arrivalsRefuted.find({cap,tick});
                    if(refuted!=result.arrivalsRefuted.end() && refuted->second.assumedArrival){advanced();continue;}
                    auto route=stockMove(b,world.traffic,cap,town,tick,false,lower,budget);
                    if(!route)return spent();
                    if(route->destination==INVALID_TILE)
                    {
                        cursor.blocked.push_back(cap);
                        if(target.requiredCapper!=NO_UNIT)
                        {
                            QJsonArray cells;for(int sq:route->path.blockedCells)cells.append(QJsonArray{sq%b.terrain.width,sq/b.terrain.width});
                            result.focusedAttempts.append(QJsonObject{{"unit",b.terrain.units[cap].engineUnitId},{"entryTick",tick},
                                {"approachStatus","NO_COMPATIBLE_DATED_ROUTE_OR_HOLD"},{"blockedCells",cells}});
                        }
                    }
                    else if(!tryEntry(world,cap,tick,*route))return spent();
                    advanced();
                }
                if(cursor.blocked.empty())continue;
                for(;cursor.screen<n;++cursor.screen)
                {
                    const int screen=cursor.screen;
                    const auto post=b.positions[screen];
                    if(b.hp[screen]<=0 || int(b.terrain.units[screen].enemy)!=target.defendingSide ||
                       post==INVALID_TILE || manhattanDistance(post,town)!=1 || b.at(screen,tick)!=post ||
                       (b.canRemove && !b.canRemove(screen,tick)))continue;
                    const int screenCell=cell(b.terrain,post);
                    auto vacated=b.traffic;
                    vacated.vacate(screen,screenCell,tick);
                    // A hypothetical removal only nominates the screen to
                    // attack. Nothing earns credit until the actual gun and
                    // capper both replay successfully in the cleared world.
                    const auto crossesScreen=[&](const TownAssaultMove &m){
                        return std::any_of(m.path.steps.begin(),m.path.steps.end(),[&](const auto &s){
                            return s.tick==tick && std::find(s.cells.begin(),s.cells.end(),screenCell)!=s.cells.end();});
                    };
                    for(;cursor.blockedCapper<cursor.blocked.size();++cursor.blockedCapper)
                    {
                        checkpoint(tick,-1,-1);
                        const int cap=cursor.blocked[cursor.blockedCapper];
                        auto route=stockMove(b,vacated,cap,town,tick,false,lower,budget);
                        if(!route)return spent();
                        if(route->destination!=INVALID_TILE && crossesScreen(*route))cursor.crossing.push_back(cap);
                        advanced();
                    }
                    for(;!cursor.crossing.empty() && cursor.gun<n;++cursor.gun)
                    {
                        const int gun=cursor.gun;
                        const auto &u=b.units[gun];
                        if(int(b.terrain.units[gun].enemy)!=side || b.hp[gun]<=0 || b.rights[gun].readyTick<0 ||
                           b.rights[gun].readyTick>tick || b.rights[gun].lastFreeTick<tick ||
                           !(u.canFire || u.hasWeapons))continue;
                        const auto ends=firingSquares(b.terrain,post,b.positions[gun],u.minRange,u.maxRange);
                        for(;cursor.origin<ends.size();++cursor.origin)
                        {
                            const auto end=ends[cursor.origin];
                            if(!cachedTravelPossible(b,gun,end,tick,!u.canMoveAndFire,lower))continue;
                            const auto weapon=supportWeapon(b.terrain,gun,screen,manhattanDistance(end,post),b.load[gun]);
                            if(weapon.base<=0)continue;
                            const auto damage=engineShotDamage(terrainReducedDamage(b.terrain.defense(screen,post),
                                weapon.base,ceilHpSteps(b.hp[gun]),ceilHpSteps(b.hp[screen])));
                            if(engineHpAfterReply(b.hp[screen],damage)>0)continue;
                            if(!cursor.cleared)
                            {
                                checkpoint(tick,-1,-1);
                                ++result.search.lanes;++result.search.guardedLanes;
                                auto opened=attackWorld();
                                Lane firing{gun,side,tick,end,NO_UNIT,weapon.base,damage,manhattanDistance(end,b.positions[gun])};
                                const auto used=takeLane(b,opened,firing,screen,post,NO_UNIT,lower,budget);
                                if(!used)return spent();
                                advanced();
                                if(!*used || opened.damage.hp[screen]>0)continue;
                                ++result.search.firstHits;++result.search.clears;
                                cursor.cleared=std::move(opened);
                            }
                            for(;cursor.afterClear<cursor.crossing.size();++cursor.afterClear)
                            {
                                const int cap=cursor.crossing[cursor.afterClear];
                                // priceEntry temporarily updates this position. A stopped
                                // reply must leave the clearing shot's dated board intact.
                                auto opened=*cursor.cleared;
                                if(opened.used.contains(cap))continue;
                                checkpoint(tick,-1,-1);
                                auto route=stockMove(b,opened.traffic,cap,town,tick,false,lower,budget);
                                if(!route)return spent();
                                if(route->destination!=INVALID_TILE && crossesScreen(*route) &&
                                   !tryEntry(opened,cap,tick,*route))return spent();
                                advanced();
                            }
                            cursor.afterClear=0;
                            cursor.cleared.reset();
                        }
                        cursor.origin=0;
                    }
                    cursor.blockedCapper=0;
                    cursor.crossing.clear();
                    cursor.gun=0;
                }
                continue;
            }
            std::vector<Lane> lanes;
            if(resumeLanes)lanes=progress->lanes;
            else
            {
                for(int i=0;i<n;++i)
                {
                    const auto &u=b.units[i];
                    if(int(b.terrain.units[i].enemy)!=side || b.hp[i]<=0 || b.rights[i].readyTick<0 ||
                       b.rights[i].readyTick>tick || b.rights[i].lastFreeTick<tick ||
                       !(u.canFire || u.hasWeapons) || (b.load[i][0]==0 && b.load[i][1]==0)) continue;
                    for(auto end:firingSquares(b.terrain,town,b.positions[i],u.minRange,u.maxRange))
                    {
                        if(!cachedTravelPossible(b,i,end,tick,!u.canMoveAndFire,lower))continue;
                        const int d=manhattanDistance(end,town);
                        const auto weapon=supportWeapon(b.terrain,i,victim,d,b.load[i]);
                        if(weapon.base<=0)continue;
                        const auto wound=engineShotDamage(terrainReducedDamage(b.terrain.defense(victim,town),weapon.base,
                            ceilHpSteps(b.hp[i]),ceilHpSteps(b.hp[victim])));
                        if(wound<=0)continue;
                        int guard=NO_UNIT;
                        if(!b.traffic.open(i,side,cell(b.terrain,end),tick,true))
                        {
                            for(const auto &use:b.traffic.uses(cell(b.terrain,end)))
                                if(use.stopped && use.actor!=i && use.first<=tick && tick<=use.last && use.actor>=0 &&
                                   use.actor<n && b.at(use.actor,tick)==end && b.hp[use.actor]>0) {guard=use.actor;break;}
                            if(guard==NO_UNIT)continue;
                        }
                        // Preserve all firing origins. Prove a route at the moment
                        // this hit is tried, after any preceding shot has occurred.
                        if(budget<=0)return spent();
                        --budget;
                        lanes.push_back({i,side,tick,end,guard,weapon.base,wound,manhattanDistance(b.positions[i],end)});
                        ++result.search.lanes;
                        if(guard!=NO_UNIT)++result.search.guardedLanes;
                    }
                }
                std::stable_sort(lanes.begin(),lanes.end(),[](const Lane &a,const Lane &c){
                    return std::tuple{a.blocker!=NO_UNIT,a.travelDistance,-a.projectedDamage}<
                           std::tuple{c.blocker!=NO_UNIT,c.travelDistance,-c.projectedDamage};
                });
                if(progress){progress->lanes=lanes;progress->cappers=cappers;}
            }
            for(int laneIndex=resumeLanes?resumedLane:0;laneIndex<static_cast<int>(lanes.size());++laneIndex)
            {
                const auto &firstGun=lanes[laneIndex];
                const bool resumeSecond=resumeLanes && laneIndex==resumedLane && resumedSecond>=0;
                if(!resumeSecond)checkpoint(tick,laneIndex,-1);
                if(firstGun.blocker!=NO_UNIT && int(b.terrain.units[firstGun.blocker].enemy)==side)continue;
                const auto afterFirst=engineHpAfterReply(b.hp[victim],firstGun.projectedDamage);
                // One or two shots strike the town in this milestone. Before
                // spending route searches, reject pairs that cannot clear it
                // even if the firing positions are all physically available.
                const auto finishes=[&](const Lane &lane,double hp,double gunHp){
                    const auto damage=engineShotDamage(terrainReducedDamage(b.terrain.defense(victim,town),lane.baseDamage,
                        ceilHpSteps(gunHp),ceilHpSteps(hp)));
                    return engineHpAfterReply(hp,damage)<=0;
                };
                if(afterFirst>0 && std::none_of(lanes.begin(),lanes.end(),[&](const auto &lane){
                    return lane.actor!=firstGun.actor && finishes(lane,afterFirst,b.hp[lane.actor]);
                }))continue;
                auto firstWorld=resumeSecond?*progress->afterFirst:attackWorld();
                if(!resumeSecond)
                {
                    const auto firstOk=takeLane(b,firstWorld,firstGun,victim,town,NO_UNIT,lower,budget);
                    if(!firstOk)return spent();
                    if(!*firstOk)continue;
                    ++result.search.firstHits;
                }
                const int secondCount=firstWorld.damage.hp[victim]<=0 ? 1 : static_cast<int>(lanes.size());
                for(int secondSlot=resumeSecond?resumedSecond:0;secondSlot<secondCount;++secondSlot)
                {
                    checkpoint(tick,laneIndex,secondSlot,&firstWorld);
                    if(budget<1)return spent();
                    --budget;
                    auto world=firstWorld;
                    if(world.damage.hp[victim]>0)
                    {
                        const auto &candidate=lanes[secondSlot];
                        if(world.used.contains(candidate.actor) ||
                           !finishes(candidate,world.damage.hp[victim],world.damage.hp[candidate.actor]))continue;
                        const auto secondOk=takeLane(b,world,candidate,victim,town,firstGun.actor,lower,budget);
                        if(!secondOk)return spent();
                        if(!*secondOk || world.damage.hp[victim]>0)continue;
                    }
                    ++result.search.clears;
                    for(int cap:cappers)
                    {
                        if(world.used.contains(cap)) continue;
                        auto m=stockMove(b,world.traffic,cap,town,tick,false,lower,budget);
                        if(!m)return spent();
                        if(m->destination==INVALID_TILE)continue;
                        if(!tryEntry(world,cap,tick,*m))return spent();
                    }
                }
            }
        }
        for(const auto &[key,partial]:result.partialArrivalActions)
            if(!partial.assumedArrival && !result.arrivalsRefuted.contains(key))result.unresolved=true;
        if(progress)
        {
            progress->complete=true;progress->settled=result;progress->bestWorst=bestWorst;
            progress->tick=progress->firstLane=progress->secondLane=-1;
            progress->unfinishedAllowance=0;progress->empty.reset();progress->lanes.clear();progress->cappers.clear();progress->afterFirst.reset();
        }
        return result;
    }
}
