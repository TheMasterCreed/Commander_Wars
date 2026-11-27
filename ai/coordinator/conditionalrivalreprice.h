#pragma once
#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/townassaultledger.h"
#include "ai/coordinator/stockneutralrebid.h"
#include "ai/coordinator/screenfireprofile.h"

namespace Coordinator
{
    inline QJsonArray conditionalPoint(TilePoint p){return {p.x,p.y};}
    inline TilePoint conditionalTile(const QJsonValue &v)
    {
        const auto a=v.toArray();
        return a.size()==2?TilePoint{a[0].toInt(-1),a[1].toInt(-1)}:INVALID_TILE;
    }
    inline QJsonObject conditionalOpeningHolder(const TownAssaultBoard &b,const ScreenOpening &start,
        const TownAssaultTarget &target,const StockCaptureSettlement &settled)
    {
        const auto n=b.units.size();const auto spot=target.property.tile;
        if(target.requiredCapper==NO_UNIT || start.positions.size()!=n || start.openingSteps.size()!=n ||
           start.openingCarry.size()!=n)return {};
        for(int i=0;i<static_cast<int>(n);++i){
            const auto &p=b.terrain.units[i];
            if(start.positions[i]!=spot || start.openingSteps[i]<=0 || start.openingCarry[i]<=0 ||
               p.engineUnitId==target.requiredCapper || int(p.enemy)==target.defendingSide)continue;
            const auto released=[&](const auto &claims){
                const auto job=claims.find(i);
                return job!=claims.end() && job->second.readyTick>=0 && job->second.readyTick<=target.occupiedFrom &&
                    job->second.tile!=spot;
            };
            if(released(settled.townAfterCapture) || released(settled.townAfterFactoryExit))continue;
            QJsonObject r{{"status","LIVE_CAPPER_NEEDS_A_DATED_VACANCY_BEFORE_EITHER_ENTRANT"},
                {"unit",p.engineUnitId},{"tile",conditionalPoint(spot)},{"atHalfTurn",2},
                {"hpSteps",start.openingSteps[i]},{"captureCarry",start.openingCarry[i]},
                {"namedReplacement",target.requiredCapper},{"requestedEntry",target.occupiedFrom}};
            const auto rate=replyCaptureRate(b.units[i].captureRate,b.units[i].hpRounded-start.openingSteps[i],false);
            if(rate>0 && start.openingCarry[i]<b.threshold){
                const auto actions=widenedCaptureTurns(start.openingCarry[i],rate,b.threshold);
                r["ifHolderCapturesUnhurt"]=QJsonObject{{"atHalfTurn",2+int(p.enemy)},
                    {"carryAfter",std::min(b.threshold,start.openingCarry[i]+rate)},{"rate",rate},
                    {"earliestCompletionAtThisHp",2+int(p.enemy)+2*(actions-1)}};
            }
            if(start.openingExactHp.size()==n && start.openingExactHp[i]>=0)r["hp"]=start.openingExactHp[i];
            if(start.openingFuel.size()==n && start.openingFuel[i]>=0 && !p.enemy && p.grid>=0){
                QJsonArray exits;const auto &grid=b.terrain.grids[p.grid];
                for(const auto d:ENTRY_DIRECTIONS){
                    const auto step=entryStep(d);const TilePoint to{spot.x+step.dx,spot.y+step.dy};
                    if(!grid.onGrid(to.x,to.y))continue;
                    const auto cost=grid.cost(to.x,to.y,d);if(cost<=0 || cost>p.movement || cost>start.openingFuel[i])continue;
                    bool occupied=false;
                    for(std::size_t j=0;j<n;++j)if(start.openingSteps[j]>0 && start.positions[j]==to){occupied=true;break;}
                    if(!occupied)exits.append(QJsonObject{{"atHalfTurn",2},{"to",conditionalPoint(to)},{"fuel",cost}});
                }
                r["oneStepVacancyCandidatesBeforeOtherF1Actions"]=exits;
                r["departureRequiresJointReplay"]=true;
            }
            return r;
        }
        return {};
    }

    // The original declined trip is not booked. Each entry below means "if
    // this particular claimant takes its recorded route". Reuse no damage
    // value after a gun moves, misses the activation or takes counterfire.
    // The alternate later firing post is a named policy, not a bound on an
    // unexamined opposing response or on another property.
    inline QJsonObject repriceClaimedVolley(const TownAssaultBoard &b,const QJsonObject &trip,
        const NeutralOccupancy &rival,TilePoint south,int release,int claimant,
        const ScreenOpening &screen,StockRouteLowerBounds &dist,std::int64_t &work,bool &exhausted,bool guardLost=false)
    {
        using namespace TownAssaultDetail;
        const int n=static_cast<int>(b.units.size()),side=int(b.terrain.units[rival.actor].enemy);
        const auto shots=trip["shots"].toArray();
        const auto starts=trip["stops"].toArray();
        QJsonObject row{{"claimant",b.terrain.units[claimant].engineUnitId},{"target",trip["tile"]},
            {"priorStatus",trip["status"]},{"priorCompletion",trip["completionTick"]},{"hadBookedTitle",false}};
        const auto refuse=[&](const QString &why){row["status"]=why;return row;};
        if(shots.empty() || starts.empty() || !b.terrain.defense || !b.terrain.weaponSlots ||
           b.conditionalClaims==nullptr || b.beforeConditionalDuty.size()!=b.rights.size() ||
           conditionalTile(trip["from"])!=b.positions[claimant])return refuse("UNBOUND_NORTHERN_ROUTE_OR_EXCHANGE");
        if(work<n*3LL+starts.size()*4LL+shots.size()*8LL){exhausted=true;return refuse("WORK_LIMIT");}
        work-=n*3LL+starts.size()*4LL+shots.size()*8LL;
        for(const auto &s:shots)if(s.toObject()["alreadyPriced"].toBool())
            return refuse("REQUIRES_RECONCILIATION_WITH_BOOKED_MATERIAL");
        State health{{b.hp.begin(),b.hp.end()},{b.load.begin(),b.load.end()}};
        if(guardLost)health.hp[rival.actor]=0.;
        auto traffic=b.traffic;
        TownAssaultMove arrival{rival.actor,side,rival.departure,rival.entry,rival.origin,south,rival.path};
        // Release is an optional subsequent own activation, never a second
        // use of the southern arrival or a deletion of the northern job.
        reserve(b.terrain,traffic,arrival,std::min(b.cutoff,release-1));
        const auto actorAt=[&](int id){
            for(int i=0;i<n;++i)if(b.terrain.units[i].engineUnitId==id)return i;
            return -1;
        };
        std::vector<TilePoint> pos(b.positions.begin(),b.positions.end());
        std::vector<int> ready(n,-1),mobile(n,-1),fuel(n,-1),last(n,b.cutoff);
        std::set<int> initialized;
        for(int i=0;i<n;++i){
            const auto &r=b.beforeConditionalDuty[i];
            if(r.readyTick>=0){ready[i]=r.readyTick;mobile[i]=std::max(r.readyTick,r.mobileFrom);fuel[i]=r.fuel;last[i]=r.lastFreeTick;}
        }
        pos[rival.actor]=south;ready[rival.actor]=mobile[rival.actor]=release;
        fuel[rival.actor]=rival.initialFuel<0?-1:rival.initialFuel-rival.path.fuel;
        if(rival.initialFuel>=0 && fuel[rival.actor]<0)return refuse("INAFFORDABLE_EARLY_POST");
        std::map<int,TilePoint> targetStops;
        auto at=b.positions[claimant];const int attacking=int(b.terrain.units[claimant].enemy);
        const auto &pawn=b.terrain.units[claimant];
        const int gp=pawn.grid>=0?pawn.grid:pawn.travelGrid;
        if(gp<0)return refuse("CLAIMANT_MOVEMENT_UNBOUND");
        const auto &grid=b.terrain.grids[gp];
        int targetFuel=b.beforeConditionalDuty[claimant].fuel;
        if(targetFuel<0 && screen.openingFuel.size()==b.units.size() && screen.openingFuel[claimant]>=0)
            targetFuel=screen.openingFuel[claimant];
        // This actor was uncommitted after F0/E0. A rejected trip's snapshot
        // may have an unavailable right. Use only a bound physical fuel value.
        if(targetFuel<0 && b.units[claimant].fuel>=0)return refuse("CLAIMANT_FUEL_UNBOUND");
        const int startTick=starts.first().toObject()["tick"].toInt(-1);
        int previousTick=startTick-2;
        for(const auto &v:starts){
            const auto s=v.toObject();const int tick=s["tick"].toInt(-1);const auto end=conditionalTile(s["tile"]);
            if(tick!=previousTick+2 || tick%2!=attacking || !grid.onGrid(end.x,end.y))return refuse("UNBOUND_CLAIMANT_DATES");
            const auto cells=s["cells"].toArray();const auto origin=at;StockCapturePath path{tick,0,{}};
            std::vector<int> walked;
            for(const auto &sq:cells){
                if(work<=0){exhausted=true;return refuse("WORK_LIMIT");}--work;
                const auto nxt=conditionalTile(sq);int cost=-1;
                if(grid.onGrid(nxt.x,nxt.y))for(const auto d:ENTRY_DIRECTIONS){
                    const auto step=entryStep(d);
                    if(nxt==TilePoint{at.x+step.dx,at.y+step.dy}){cost=grid.cost(nxt.x,nxt.y,d);break;}
                }
                if(cost<=0 || !traffic.open(claimant,attacking,cell(b.terrain,nxt),tick,false))
                    return refuse("NAMED_CLAIMANT_CANNOT_TRANSIT_THE_COMBINED_CALENDAR");
                path.fuel+=cost;walked.push_back(cell(b.terrain,nxt));at=nxt;
            }
            if(at!=end || path.fuel>pawn.movement || (targetFuel>=0 && path.fuel>targetFuel) ||
               !traffic.open(claimant,attacking,cell(b.terrain,end),tick,true) ||
               !traffic.open(claimant,attacking,cell(b.terrain,end),tick+1,true))
                return refuse("NAMED_CLAIMANT_CANNOT_STOP_ON_THE_COMBINED_CALENDAR");
            if(targetFuel>=0)targetFuel-=path.fuel;
            path.steps.push_back({tick,end,std::move(walked)});
            reserve(b.terrain,traffic,TownAssaultMove{claimant,attacking,tick,tick,origin,end,path},tick+1);
            targetStops[tick]=end;previousTick=tick;
        }
        QJsonArray results;MilliFunds net=0;int priorShot=-1;
        for(const auto &v:shots){
            const auto s=v.toObject();
            if(health.hp[claimant]<=0)break;
            const int tick=s["tick"].toInt(-1),gun=actorAt(s["gun"].toInt(-1));const auto target=conditionalTile(s["targetTile"]);
            if(gun<0 || tick<priorShot || tick%2!=side || !targetStops.contains(tick-1) || targetStops[tick-1]!=target)
                return refuse("ADDITIONAL_TARGET_POST_OR_FIRE_ORDER_UNBOUND");
            priorShot=tick;

            QJsonObject fire{{"gun",b.terrain.units[gun].engineUnitId},{"tick",tick},{"targetTile",conditionalPoint(target)}};
            if(gun!=rival.actor && initialized.insert(gun).second && s["proposalGunSource"].isObject()){
                const auto origin=s["proposalGunSource"].toObject();
                const auto post=conditionalTile(origin["post"]);const auto rounds=origin["ammo"].toArray();
                const auto linked=b.conditionalClaims->find(gun);
                const bool sameClaim=linked!=b.conditionalClaims->end() && std::any_of(linked->second.begin(),linked->second.end(),[&](const auto &c){
                    return c.claimant==claimant && c.target==conditionalTile(trip["tile"]) && c.firstShot<=tick && c.through>=tick;
                });
                if(!sameClaim || post==INVALID_TILE || rounds.size()!=2 || origin["firstFree"].toInt(-1)<0 ||
                   !origin["hp"].isDouble() || origin["hp"].toDouble()<=0. || origin["fuel"].toInt(-2)<-1)
                    return refuse("UNBOUND_NAMED_SHOOTER_SOURCE");
                // Reopen the stock claim only together with its original
                // contributors. Do not carry forward the later market's
                // hypothetical exclusions *and* that earlier rejected shot.
                fire["proposalGunSource"]=origin;fire["laterMarketReady"]=ready[gun];fire["laterMarketMobile"]=mobile[gun];
                fire["laterMarketHp"]=health.hp[gun];fire["laterMarketPost"]=conditionalPoint(pos[gun]);
                QJsonArray competing;
                for(const auto &d:linked->second){
                    if(d.claimant==claimant && d.target==conditionalTile(trip["tile"]) &&
                       d.firstShot<=tick && d.through>=tick)continue;
                    if(d.claimant<0 || d.claimant>=n)return refuse("UNBOUND_SUBSTITUTE_COMPETING_CLAIM");
                    competing.append(QJsonObject{{"claimant",b.terrain.units[d.claimant].engineUnitId},
                        {"tile",conditionalPoint(d.target)},{"firstShot",d.firstShot},{"through",d.through},{"entry",d.entry}});
                }
                fire["otherConditionalClaims"]=competing;
                fire["otherSourceChoicesRequireRepricing"]=!competing.isEmpty() || ready[gun]<0 || ready[gun]>tick ||
                    mobile[gun]>tick || pos[gun]!=post || std::abs(health.hp[gun]-origin["hp"].toDouble())>.001 ||
                    health.load[gun][0]!=rounds[0].toInt() || health.load[gun][1]!=rounds[1].toInt() ||
                    fuel[gun]!=origin["fuel"].toInt() || last[gun]<tick || origin["foregoneOpportunity"].toInteger()!=0;
                pos[gun]=post;ready[gun]=origin["firstFree"].toInt();mobile[gun]=origin["mayRelocate"].toBool()?ready[gun]:b.cutoff+1;
                last[gun]=b.cutoff;fuel[gun]=origin["fuel"].toInt();health.hp[gun]=origin["hp"].toDouble();
                health.load[gun]={rounds[0].toInt(),rounds[1].toInt()};
            }
            if(ready[gun]<0)return refuse("UNBOUND_GUN_ACTIVATION");
            if(health.hp[gun]<=0 || ready[gun]>tick || last[gun]<tick){
                fire["status"]="NO_ACTIVATION_AT_THIS_DATE";results.append(fire);continue;
            }
            const auto &g=b.terrain.units[gun];const auto &u=b.units[gun];const int index=g.grid>=0?g.grid:g.travelGrid;
            if(index<0)return refuse("UNBOUND_GUN_MOVEMENT");
            const auto &gunGrid=b.terrain.grids[index];const auto quoted=conditionalTile(s["from"]);
            std::vector<TilePoint> posts;
            if(gunGrid.onGrid(quoted.x,quoted.y))posts.push_back(quoted);
            // An early guard can return to a different legal firing position.
            // Other named shooters follow their own previously quoted posts.
            if(gun==rival.actor)for(int y=std::max(0,target.y-u.maxRange);y<=std::min(b.terrain.height-1,target.y+u.maxRange);++y)
                for(int x=std::max(0,target.x-u.maxRange);x<=std::min(b.terrain.width-1,target.x+u.maxRange);++x){
                    const TilePoint post{x,y};const int distance=manhattanDistance(post,target);
                    if(distance>=u.minRange && distance<=u.maxRange && post!=quoted)posts.push_back(post);
                }
            std::optional<TownAssaultMove> selected;
            for(auto post:posts){
                auto moved=move(b,traffic,gun,side,gunGrid,g.movement,fuel[gun],g.ignoresCollision,
                    pos[gun],post,post==pos[gun]?ready[gun]:std::max(ready[gun],mobile[gun]),tick,!u.canMoveAndFire,dist,work,true);
                if(!moved){exhausted=true;return refuse("WORK_LIMIT");}
                if(moved->destination==INVALID_TILE)continue;
                auto hypothetical=health;
                const auto priced=hit(b,hypothetical,gun,claimant,post,target,tick);
                if(!priced)continue;
                selected=std::move(moved);break;
            }
            if(!selected){
                fire["status"]="NO_COMPATIBLE_NAMED_SHOT_ON_RETAINED_CALENDAR";results.append(fire);continue;
            }
            const auto hitObject=hit(b,health,gun,claimant,selected->destination,target,tick);
            if(!hitObject)return refuse("NONREPRODUCIBLE_COMBAT_QUOTE");
            const auto &h=*hitObject;
            if(!RelativeStockDetail::checkedAdd(net,h.targetLoss) || !RelativeStockDetail::checkedSubtract(net,h.counterLoss))
                return refuse("ARITHMETIC");
            reserve(b.terrain,traffic,*selected,b.cutoff);
            if(fuel[gun]>=0)fuel[gun]-=selected->path.fuel;
            pos[gun]=selected->destination;ready[gun]=mobile[gun]=tick+2;
            QJsonArray motion;for(const auto &step:selected->path.steps)motion.append(QJsonArray{step.tick,step.tile.x,step.tile.y});
            fire["status"]="REPRICED_NAMED_SHOT";fire["posts"]=motion;fire["from"]=conditionalPoint(pos[gun]);
            fire["shooterHpAfter"]=h.actorAfter;fire["targetHpBefore"]=h.targetBefore;fire["targetHpAfter"]=h.targetAfter;
            fire["remainingFuel"]=fuel[gun];fire["remainingAmmo"]=QJsonArray{health.load[gun][0],health.load[gun][1]};
            fire["targetCapital"]=static_cast<qint64>(h.targetLoss);fire["counterCapital"]=static_cast<qint64>(h.counterLoss);
            results.append(fire);
        }
        row["status"]="NAMED_NORTHERN_EXCHANGE_REPRICED";row["shots"]=results;
        row["claimantHpAfter"]=health.hp[claimant];row["claimantFuelAfterRecordedStops"]=targetFuel;
        row["conditionalMaterialForDefender"]=static_cast<qint64>(net);
        if(health.hp[claimant]>0 && trip["proposedEntryTick"].isDouble() && conditionalTile(trip["from"])!=conditionalTile(trip["tile"])){
            const int rate=replyCaptureRate(b.units[claimant].captureRate,
                b.units[claimant].hpRounded-ceilHpSteps(health.hp[claimant]),false);
            if(rate>0){
                const int enter=trip["proposedEntryTick"].toInt();
                row["ifClaimantArrivesAtOriginalDateUnchanged"]=QJsonObject{{"entry",enter},{"rateAtSurvivingHp",rate},
                    {"firstUninterruptedFinish",enter+2*(widenedCaptureTurns(0,rate,b.threshold)-1)},
                    {"remainingTravelAndDefenderResponsesUnpriced",true}};
            }
        }
        row["continuation"]=health.hp[claimant]<=0?"NAMED_ACTOR_DEAD_IF_IT_ATTEMPTS":"SURVIVING_CAPPER_ROUTE_AND_RESPONSES_OPEN";
        return row;
    }

    template<class Timing> QJsonObject repriceConditionalRival(const TownAssaultBoard &b,const StockCaptureSettlement &settled,
        const TownAssaultTarget &target,const NeutralOccupancy &rival,const ScreenOpening &screen,std::span<const MilliFunds> cash,
        int cutoff,Timing timing,StockRouteLowerBounds &lower,std::int64_t &work,bool &exhausted)
    {
        QJsonObject out{{"scope","ONE_EARLY_GUARD_AND_EACH_CONDITIONAL_ORIGINAL_ROUTE"},
            {"wholeTeamRank",false},{"firstGuardHalfTurn",rival.entry},{"fuelToGuard",rival.path.fuel}};
        if(!rival.conditionalDuty || rival.actor<0 || rival.actor>=static_cast<int>(b.units.size()) ||
           !b.conditionalClaims || !b.conditionalClaims->contains(rival.actor)){out["status"]="UNBOUND_CONDITIONAL_GUN";return out;}
        const auto &claims=b.conditionalClaims->at(rival.actor);int lastClaim=-1;QJsonArray duties,calendars;
        for(const auto &claim:claims){
            if(claim.claimant<0 || claim.claimant>=static_cast<int>(b.units.size())){out["status"]="UNBOUND_CONDITIONAL_CLAIM";return out;}
            lastClaim=std::max(lastClaim,claim.through);
            duties.append(QJsonObject{{"claimant",b.terrain.units[claim.claimant].engineUnitId},{"tile",conditionalPoint(claim.target)},
                {"firstShot",claim.firstShot},{"through",claim.through},{"entry",claim.entry},{"incomplete",claim.incomplete}});
        }
        out["claims"]=duties;
        if(screen.positions.size()==b.units.size() && rival.departure==2+int(b.terrain.units[rival.actor].enemy) &&
           screen.positions[rival.actor]!=rival.origin){
            out["status"]="EARLY_RIVAL_ORIGIN_REQUIRES_A_DATED_MOVE";
            out["openingPosition"]=conditionalPoint(screen.positions[rival.actor]);out["proposedOrigin"]=conditionalPoint(rival.origin);
            return out;
        }
        const auto occupant=conditionalOpeningHolder(b,screen,target,settled);
        if(!occupant.isEmpty())out["requiredBeforeSouthernPlacement"]=occupant;
        int cap=-1;for(int i=0;i<static_cast<int>(b.units.size());++i)if(b.terrain.units[i].engineUnitId==target.requiredCapper)cap=i;
        if(cap<0){out["status"]="UNBOUND_NAMED_SOUTHERN_CAPPER";return out;}
        const int rate=replyCaptureRate(b.units[cap].captureRate,b.units[cap].hpRounded-ceilHpSteps(b.hp[cap]),false);
        if(rate<=0){out["status"]="UNBOUND_TARGET_CAPTURE_RATE";return out;}
        const int installments=widenedCaptureTurns(0,rate,b.threshold),side=1-target.defendingSide;
        const int lastRelease=std::min(cutoff+1,std::max(rival.entry+2,lastClaim+2));
        for(int release=rival.entry+2;release<=lastRelease;release+=2){
            if(work<=0){exhausted=true;break;}--work;
            const int enter=std::max(target.occupiedFrom,release+1);
            const int finish=enter+2*(installments-1);
            TownAssaultLine earns;earns.side=side;earns.town=target.property.tile;earns.finishTick=finish<=cutoff?finish:-1;
            auto book=townAssaultLedger(earns,target.property,cash,cutoff,timing);
            if(!book){out["status"]="ARITHMETIC";return out;}
            QJsonObject line{{"guardReleases",release},{"firstPossibleEntryIfPostClears",enter},
                {"firstPossibleCompletionAtQuotedHp",finish},{"datedTitleOnlyIfUninterrupted",static_cast<qint64>(book->score)}};
            QJsonArray displaced;
            for(const auto &claim:claims){
                bool found=false;
                for(const auto &v:settled.proofs){
                    const auto proof=v.toObject();
                    if(proof["unit"].toInt(-1)!=b.terrain.units[claim.claimant].engineUnitId ||
                       conditionalTile(proof["tile"])!=claim.target || proof["proposedEntryTick"].toInt(-1)!=claim.entry ||
                       proof["completionTick"].toInt(-2)!=-1 || proof["shots"].toArray().isEmpty())continue;
                    found=true;
                    auto survives=repriceClaimedVolley(b,proof,rival,target.property.tile,release,claim.claimant,screen,lower,work,exhausted);
                    if(!exhausted){
                        survives["ifSouthGuardLostBeforeRelease"]=repriceClaimedVolley(b,proof,rival,target.property.tile,
                            release,claim.claimant,screen,lower,work,exhausted,true);
                        survives["guardRemovalIsConditional"]=true;
                    }
                    displaced.append(survives);
                    if(exhausted)break;
                }
                if(!found)displaced.append(QJsonObject{{"status","UNSETTLED_NORTHERN_ROUTE_NOT_IDENTIFIED"},
                    {"claimant",b.terrain.units[claim.claimant].engineUnitId}});
                if(exhausted)break;
            }
            line["ifOtherClaimantsAttempt"]=displaced;calendars.append(line);
            if(exhausted)break;
        }
        out["datedChoices"]=calendars;
        out["upstreamCondition"]="EARLY_GUARD_SURVIVES_WITH_OBSERVED_HP_AMMO_AND_FUEL";
        out["status"]=exhausted?"WORK_LIMIT":"CONDITIONAL_POLICIES_REPRICED_OTHER_BRANCHES_OPEN";
        return out;
    }
}
