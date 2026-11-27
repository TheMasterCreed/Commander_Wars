#pragma once

#include "ai/coordinator/interceptedstockstep.h"
#include "ai/coordinator/paidrecruitreply.h"

namespace Coordinator
{
    struct PaidStockResources
    {
        double hp{};
        int fuel{};
        std::array<int,2> ammo{};
        bool mayRelocate{};
        bool fuelKnown{true};
    };

    struct PaidCaptureDefenseView
    {
        const CombatTerrain &terrain;
        std::span<const KnownUnit> units;
        const StockCaptureTraffic &traffic;
        std::span<const TilePoint> positions;
        std::span<const double> hp;
        std::span<const std::array<int,2>> ammo;
        std::span<const CaptureDefenderAvailability> rights;
        std::span<const FundedCaptureGun> funded;
        // The timeline includes completed moves and actions from the prior
        // supported tasks, plus any real clearing volley for an assault.
        std::function<TilePoint(int,int)> at;
        std::function<bool(int,int)> available;
        std::function<bool(int,int)> futureAction;
        std::function<bool(int,int)> fundedBusy;
        std::map<int,int> *gridMinima{};
        // State immediately before the named activation on the accepted stock calendar.
        std::function<PaidStockResources(int,int)> stockResources;
        std::function<bool(int,int)> conditionalGun;
        PaidDuelPositions *fundedPositions{};
        std::function<bool(int,int)> fundedCommitment;
        std::function<bool(int,int,int)> fundedScreen;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        bool originalRangeAudit{};
#endif
    };

    struct PaidCaptureCandidate
    {
        int gun{-1},entry{-1},finish{-1};
        TilePoint property{INVALID_TILE};
        StockCapturePath path;
        CaptureSurvivalState survivor;
        QJsonArray installments,pursuitChecks,defenderResources,paidDuelChecks,paidDuelUses;
        QString status;
        MilliFunds utility{};
    };

    // Evaluate the actual purchased type as the target of the same loaded
    // gun/HP/counter model as an existing infantry. Sites for which the mod
    // has not supplied a complete matchup remain unpriced.
    inline std::optional<PaidCaptureCandidate> provePaidCapture(const PaidCaptureDefenseView &view,
        int index,const StockCapturePath &path,TilePoint property,const PropertyStockColumn &col,
        int threshold,int last,std::int64_t &budget)
    {
        PaidCaptureCandidate out;out.gun=index;out.property=property;out.entry=path.entryTick;out.path=path;
        if(index<0 || index>=static_cast<int>(view.funded.size()) || !view.funded[index].unit || !view.terrain.defense)
        {out.status="INCOMPLETE_MATCHUP";return out;}
        const auto &gun=view.funded[index];const auto &u=*gun.unit;const int side=gun.side;
        const int start=path.steps.empty()?path.entryTick:path.steps.front().tick;
        if(gun.observedService && start<gun.observedService->nextReady){out.status="BOOKED_STOCK_SUPPORT";return out;}
        const auto starting=paidHpAfterService(gun,start,u.startingHp);
        const auto load=paidLoadAfterService(gun,start,u.captureLoad);
        const int n=static_cast<int>(view.units.size()),width=view.terrain.width;
        const int square=property.y*width+property.x;
        if(path.entryTick<0 || !u.captureRate.contains(square) || u.againstCapper.size()!=view.units.size() ||
           u.captureCounter.size()!=view.units.size()) {out.status="INCOMPLETE_MATCHUP";return out;}
        CombatTerrain shadow;shadow.movementCache=view.terrain.movementCache;shadow.width=width;shadow.height=view.terrain.height;shadow.units=view.terrain.units;
        shadow.units.push_back({.tile=gun.factory,.present=true,.enemy=bool(side),.movement=u.movement,
                                .replacementCost=u.cost,.realHp=starting,.ammoByWeapon=load});
        std::vector<KnownUnit> actors(view.units.begin(),view.units.end());
        KnownUnit body{};body.unitId=u.type;body.hpRounded=ceilHpSteps(starting);body.canCapture=true;
        body.captureRate=replyCaptureRate(u.captureRate.at(square),10-body.hpRounded,false);
        body.minRange=u.minRange;body.maxRange=u.maxRange;body.canMoveAndFire=u.mobileFire;
        actors.push_back(body);
        auto position=std::vector<TilePoint>(view.positions.begin(),view.positions.end());position.push_back(gun.factory);
        auto health=std::vector<double>(view.hp.begin(),view.hp.end());health.push_back(starting);
        auto ammo=std::vector<std::array<int,2>>(view.ammo.begin(),view.ammo.end());ammo.push_back(load);
        auto rights=std::vector<CaptureDefenderAvailability>(view.rights.begin(),view.rights.end());rights.push_back({});
        shadow.weaponSlots=[&](int shooter,int target,int distance)->std::array<double,2> {
            if(distance<actors[shooter].minRange || distance>actors[shooter].maxRange) return {-1.,-1.};
            if(target==n && shooter<n) return u.againstCapper[shooter];
            if(shooter==n && target<n) return u.captureCounter[target];
            return {-1.,-1.};
        };
        shadow.defense=[&](int actor,TilePoint at) {
            if(actor!=n) return view.terrain.defense(actor,at);
            const int ri=u.routeAt[at.y*width+at.x];return u.defense[ri];
        };
        const auto &timeline=view.traffic;
        std::optional<StockCaptureTraffic> changedTraffic;
        const auto calendar=[&]() -> const StockCaptureTraffic& {return changedTraffic?*changedTraffic:timeline;};
        const auto cell=[&](TilePoint t){return t.y*width+t.x;};
        const auto realGun=[&](int shooter){return shooter==n ? n+index : shooter;};
        auto physical=[&](int shooter,int tick,TilePoint t,bool transit) {
            const int actor=realGun(shooter),team=shooter==n ? side : int(shadow.units[shooter].enemy);
            const auto &uses=calendar().uses(cell(t));
            // The paid body is following this route. Its former deployment no
            // longer blocks an opposing gun; its real current tile is excluded
            // by the common stockGun movement and firing check.
            const auto open=[&](int when,bool stop) {
                return std::none_of(uses.begin(),uses.end(),[&](const auto &use) {
                    return use.actor!=actor && use.actor!=n+index && use.first<=when && when<=use.last &&
                        (use.side!=team || (use.stopped && stop));
                });
            };
            if(!open(tick,!transit)) return false;
            if(!transit && shooter<n && t!=position[shooter])
                for(int k=tick+1;k<=last;++k) if(!open(k,true)) return false;
            return true;
        };
        CaptureRouteDefense defense{shadow,actors,position,health,ammo,rights,view.funded,physical,view.available};
        defense.movementGrids=&view.terrain.grids; defense.geometryRevision=[&]{return calendar().generation();};
        if(view.gridMinima) defense.sharedMinimumEntryCost=view.gridMinima;
        defense.beginTrip();
        const int rate=body.captureRate;
        defense.avoidedCaptureIncome=[&](int,TilePoint at,int tick,int carried,double before,double after)->MilliFunds {
            if(before<=0) return 0;
            const int first=std::max(path.entryTick,tick);
            if(at!=property || tick<path.entryTick) carried=0;
            const int beforeRate=replyCaptureRate(rate,body.hpRounded-ceilHpSteps(before),false),afterRate=replyCaptureRate(rate,body.hpRounded-ceilHpSteps(after),false);
            if(beforeRate<=0) return 0;
            const auto beforeDone=first+2*std::max<std::int64_t>(0,widenedCaptureTurns(carried,beforeRate,threshold)-int(tick<path.entryTick));
            // A killed capper misses at least its next proven payday. Another
            // uncommitted unit could replace it in later rounds.
            const auto afterDone=after<=0 || afterRate<=0 ? beforeDone+2 :
                first+2*std::max<std::int64_t>(0,widenedCaptureTurns(carried,afterRate,threshold)-int(tick<path.entryTick));
            MilliFunds value=0;
            for(int k=static_cast<int>(beforeDone)+1;k<=last && k<=afterDone;++k)
                if(k%2==side || col.ownerBefore==(side ? OwnerSign::Ours : OwnerSign::Enemy))
                    value+=k%2 ? col.income.enemyPerTurn : col.income.oursPerTurn;
            return value;
        };
        std::set<int> reportedPursuits;
        struct ExtraUse {double wounds{};int fuel{};std::array<int,2> rounds{};TilePoint post{INVALID_TILE};};
        std::vector<ExtraUse> extra(n);
        PaidDuelPositions privatePositions;auto &fundedPosts=view.fundedPositions?*view.fundedPositions:privatePositions;
        std::vector<PaidDuelSpent> duels(view.funded.size());
        std::map<int,PaidDuelGun> offers;
        const auto xy=[](TilePoint p){return QJsonArray{p.x,p.y};};
        defense.paidResponders=[&](int replyTick,TilePoint target,const CaptureSurvivalState &state,std::int64_t &work)
            ->std::optional<std::vector<CaptureSurvivalDetail::Gun>>{
            std::vector<CaptureSurvivalDetail::Gun> eligible;offers.clear();
            for(int i=0;i<static_cast<int>(view.funded.size());++i)
            {
                const auto &other=view.funded[i];
                if(!other.unit || other.side==side || replyTick<other.buildTick+2)continue;
                const auto &foe=*other.unit;const bool transit=replyTick<other.readyTick;
                const int distance=manhattanDistance(transit?other.factory:other.destination,target);
                const bool oldAlarm=transit
                    ? std::int64_t(std::max(0,distance-foe.maxRange))*foe.minimumMoveCost<=std::int64_t((replyTick-other.buildTick)/2)*foe.movement
                    : ((distance>=foe.minRange && distance<=foe.maxRange) || (foe.mobileFire && foe.movement>0 &&
                       std::int64_t(std::max(0,distance-foe.maxRange))*foe.minimumMoveCost<=foe.movement));
                if(!oldAlarm)continue;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                if(view.originalRangeAudit && view.fundedBusy && view.fundedBusy(i,replyTick))continue;
#endif
                fundedPosts.read(timeline,width*view.terrain.height,n,static_cast<int>(view.funded.size()));
                auto shot=paidDuelShot(view.terrain,calendar(),fundedPosts,view.funded,n,index,i,target,replyTick,last,path,duels[i],
                    (view.fundedBusy && view.fundedBusy(i,replyTick)) || (view.fundedCommitment && view.fundedCommitment(i,replyTick)),work,view.fundedScreen);
                QJsonObject evidence{{"capperTick",replyTick-1},{"capperStop",xy(target)},{"tick",replyTick},{"index",i},
                    {"type",foe.type},{"factory",xy(other.factory)},{"birthTick",other.buildTick},{"post",xy(other.destination)},
                    {"ready",other.readyTick},{"originalShot",other.impactTick},{"originalTarget",other.targetUnit>=0 && other.targetUnit<n?
                        view.terrain.units[other.targetUnit].engineUnitId:NO_UNIT},{"originalTargetTile",xy(other.targetTile)},
                    {"capperType",u.type},{"capperHp",state.hp},
                    {"oldAlarm",transit?"UNPRICED_RECRUIT_IN_TRANSIT":"UNPRICED_RECRUIT_REPLY"}};
                if(!shot){out.status="DEFERRED_PAID_DUEL_WORK";evidence["result"]=out.status;
                    if(out.paidDuelChecks.size()<12)out.paidDuelChecks.append(evidence);return eligible;}
                evidence["start"]=xy(shot->start);evidence["committedStop"]=xy(shot->committed);
                evidence["nextStop"]=xy(shot->resume);evidence["result"]=shot->reason;
                evidence["availableFuel"]=shot->availableFuel;evidence["replacedTravelFuel"]=shot->replacedFuel;
                if(shot->proved)
                {
                    QJsonArray there,back;for(int c:shot->gun.deployment.forward)there.append(xy({c%width,c/width}));
                    for(int c:shot->gun.deployment.back)back.append(xy({c%width,c/width}));
                    evidence["from"]=xy(shot->gun.from);evidence["weapon"]=shot->gun.weapon;evidence["baseDamage"]=shot->gun.attack;
                    evidence["hp"]=shot->gun.hp;evidence["out"]=there;evidence["back"]=back;
                    const auto &offer=shot->gun;
                    const auto rawPrimary=terrainReducedDamage(shadow.defense(n,target),offer.attack,
                        ceilHpSteps(offer.hp),ceilHpSteps(state.hp));
                    const auto damage=engineShotDamage(rawPrimary);
                    const auto left=engineHpAfterReply(state.hp,damage);double backPower=0;
                    for(int slot=0;slot<2;++slot)if(ammo[n][slot]!=0 && offer.counterSlots)
                        backPower=std::max(backPower,(*offer.counterSlots)[slot]);
                    const auto counter=left>0 && backPower>0?engineShotDamage(sameActionCounterDamage(offer.cover,backPower,
                        state.hp,rawPrimary,ceilHpSteps(offer.hp))):0.;
                    evidence["isolatedTargetAfter"]=left;evidence["isolatedShooterAfter"]=engineHpAfterReply(offer.hp,counter);
                    evidence["ammo"]=QJsonArray{shot->ammo[0],shot->ammo[1]};
                    eligible.push_back(shot->gun);offers.emplace(i,std::move(*shot));
                }
                if(out.paidDuelChecks.size()<12)out.paidDuelChecks.append(evidence);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                if(view.originalRangeAudit){out.status=transit?"UNPRICED_RECRUIT_IN_TRANSIT":"UNPRICED_RECRUIT_REPLY";return eligible;}
#endif
                if(shot->unknown){out.status=shot->reason;return eligible;}
            }
            return eligible;
        };
        const auto resources=[&](int i,int when){
            return view.stockResources?view.stockResources(i,when):
                PaidStockResources{view.hp[i],view.rights[i].remainingFuel,view.ammo[i],view.rights[i].mayRelocate};
        };
        out.survivor.hp=starting;
        auto advance=[&](TilePoint at,int tick,bool capture) {
            const int targetSquare=cell(at);
            if(targetSquare<0 || targetSquare>=static_cast<int>(u.routeAt.size()) || u.routeAt[targetSquare]<0 || u.routeAt[targetSquare]>=static_cast<int>(u.defense.size()))
            {out.status="UNKNOWN_TERRAIN_DEFENSE";return true;}
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            auto normalOffers=decltype(defense.paidResponders){};
            if(view.originalRangeAudit)
            {
                const bool closes=capture && out.survivor.carry+replyCaptureRate(rate,body.hpRounded-ceilHpSteps(out.survivor.hp),false)>=threshold;
                if(!closes)
                {
                    // A separate evidence-only budget traces the old first warning
                    // without changing which later candidates the original pass reached.
                    std::int64_t auditWork=std::max<std::int64_t>(100000,8LL*width*view.terrain.height);
                    defense.paidResponders(tick+1,at,out.survivor,auditWork);
                    if(!out.status.isEmpty())return true;
                }
                normalOffers=std::move(defense.paidResponders);defense.funded={};
            }
#endif
            for(int i=0;i<n;++i)
            {
                auto base=resources(i,tick+1);
                health[i]=std::max(0.,base.hp-extra[i].wounds);
                ammo[i]=base.ammo;
                for(int slot=0;slot<2;++slot)if(ammo[i][slot]>=0)ammo[i][slot]=std::max(0,ammo[i][slot]-extra[i].rounds[slot]);
                rights[i].remainingFuel=base.fuel<0?-1:std::max(0,base.fuel-extra[i].fuel);
                rights[i].mayRelocate=base.mayRelocate;
                position[i]=extra[i].post!=INVALID_TILE?extra[i].post:view.at?view.at(i,tick+1):view.positions[i];
                if(health[i]<=0)position[i]=INVALID_TILE;
            }
            position[n]=at;
            const int prior=out.survivor.carry;const double initial=out.survivor.hp;
            const int former=static_cast<int>(out.survivor.shots.size());
            // The oracle handles the volley on this activation. Earlier hypothetical
            // shots are already debited above, independently of intervening real stock jobs.
            CaptureSurvivalState activation{out.survivor.hp,out.survivor.carry,out.survivor.completedTick};
            const bool priced=advanceCaptureSurvival(defense,activation,n,at,tick,capture,threshold,rate,body.hpRounded,budget);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            if(view.originalRangeAudit){defense.paidResponders=std::move(normalOffers);defense.funded=view.funded;}
#endif
            if(!priced)return false;
            if(!out.status.isEmpty())return true;
            out.survivor.hp=activation.hp;out.survivor.carry=activation.carry;out.survivor.completedTick=activation.completedTick;
            for(const auto &s:activation.shots)
            {
                const int i=s.gun;
                if(i>n)
                {
                    const int k=i-n-1;const auto &offer=offers.at(k);const auto &g=view.funded[k];
                    auto &spent=duels[k];++spent.rounds[s.weapon];spent.hp=s.shooterAfter;
                    if(s.deployment.from!=INVALID_TILE)spent.fuel+=s.deployment.fuel-offer.replacedFuel;
                    spendCombatRound(ammo[n],s.counterWeapon);
                    auto receipt=s;receipt.gun=n+k;out.survivor.shots.push_back(receipt);
                    QJsonArray outward,returning;
                    for(int c:s.deployment.forward)outward.append(xy({c%width,c/width}));
                    for(int c:s.deployment.back)returning.append(xy({c%width,c/width}));
                    out.paidDuelUses.append(QJsonObject{{"index",k},{"tick",s.shotTick},{"committedStop",xy(offer.committed)},
                        {"nextStop",xy(offer.resume)},{"loan",s.deployment.from!=INVALID_TILE},{"out",outward},{"back",returning}});
                    for(int j=out.paidDuelChecks.size();j-->0;)
                    {
                        auto v=out.paidDuelChecks[j].toObject();if(v["index"]!=k || v["tick"]!=s.shotTick)continue;
                        v["chosen"]=true;v["targetBefore"]=s.targetBefore;v["targetAfter"]=s.targetAfter;
                        v["shooterAfter"]=s.shooterAfter;v["counterWeapon"]=s.counterWeapon;
                        v["targetLoss"]=static_cast<qint64>(s.damageValue);v["counterLoss"]=static_cast<qint64>(s.counterValue);
                        out.paidDuelChecks[j]=v;break;
                    }
                    if((g.targetUnit!=NO_UNIT && s.shotTick<g.impactTick && s.shooterAfter<offer.gun.hp) ||
                       (s.deployment.from!=INVALID_TILE && s.shooterAfter<=0))
                    {out.status="PAID_COUNTER_CHANGES_PROMISED_JOB";return true;}
                    if(s.shooterAfter<=0 && view.fundedScreen)for(const auto &use:fundedPosts.uses[k])
                        for(int t=std::max(s.shotTick,use.first);t<=std::min(last,use.last);++t)
                            if(view.fundedScreen(k,use.cell,t)){out.status="PAID_COUNTER_REMOVES_REQUIRED_SCREEN";return true;}
                    if(s.deployment.from!=INVALID_TILE)
                    {
                        for(const auto &step:path.steps)if(step.tick==s.shotTick+1 && (step.tile==s.from ||
                            (!u.ignoresUnitCollision && std::find(step.cells.begin(),step.cells.end(),cell(s.from))!=step.cells.end())))
                        {out.status="PAID_DEFENDER_BLOCKS_CONTINUATION";return true;}
                        spent.returnTick=s.shotTick+2;
                        if(!changedTraffic)changedTraffic=timeline;
                        const int physicalGun=n+k;changedTraffic->removeUsesWithin(physicalGun,s.shotTick,s.shotTick+2);
                        for(int c:s.deployment.forward)changedTraffic->reserve(c,s.shotTick,s.shotTick,g.side,physicalGun,false);
                        changedTraffic->reserve(cell(s.from),s.shotTick,s.shotTick+1,g.side,physicalGun,true);
                        for(int c:s.deployment.back)changedTraffic->reserve(c,s.shotTick+2,s.shotTick+2,g.side,physicalGun,false);
                        changedTraffic->reserve(cell(offer.resume),s.shotTick+2,s.shotTick+3,g.side,physicalGun,true);
                    }
                    if(s.shooterAfter<=0)
                    {
                        if(!changedTraffic)changedTraffic=timeline;
                        changedTraffic->removeFutureUses(n+k,s.shotTick);
                    }
                    continue;
                }
                auto &use=extra[i];
                if(s.weapon>=0)++use.rounds[s.weapon];
                use.fuel+=s.motion.fuel;
                use.wounds+=std::max(0.,health[i]-s.shooterAfter);
                spendCombatRound(ammo[n],s.counterWeapon);
                out.survivor.shots.push_back(s);
                const auto end=resources(i,last+2);
                out.defenderResources.append(QJsonObject{{"gun",shadow.units[i].engineUnitId},{"tick",s.shotTick},
                    {"hpBefore",health[i]},{"ammoBefore",QJsonArray{ammo[i][0],ammo[i][1]}},{"weapon",s.weapon},
                    {"fuelBefore",rights[i].remainingFuel},{"fuelSpent",s.motion.fuel},{"hpAfter",s.shooterAfter}});
                if(s.motion.fuel>0 && !resources(i,s.shotTick).fuelKnown)
                {out.status="UNPRICED_FACTORY_DEPARTURE";return true;}
                if((view.conditionalGun && view.conditionalGun(i,s.shotTick)) ||
                   (s.weapon>=0 && end.ammo[s.weapon]>=0 && end.ammo[s.weapon]<use.rounds[s.weapon]) ||
                   (end.fuel>=0 && end.fuel<use.fuel) ||
                   ((s.shooterAfter<health[i] || s.motion.origin!=INVALID_TILE) &&
                    view.futureAction && view.futureAction(i,s.shotTick)))
                {out.status="DEFENDER_CHANGES_EARLIER_PLAN";return true;}
                if(s.motion.origin!=INVALID_TILE || s.shooterAfter<=0)
                {
                    if(!changedTraffic)changedTraffic=timeline;
                    changedTraffic->vacate(i,cell(position[i]),s.shotTick);
                    if(s.shooterAfter>0)
                    {
                        use.post=s.from;
                        changedTraffic->reserve(cell(s.from),s.shotTick,last,int(shadow.units[i].enemy),i,true);
                    }
                }
            }
            if(out.survivor.completedTick<0 && out.survivor.hp>0)
                for(int z=former-1;z>=0;--z)
                {
                    const auto &old=out.survivor.shots[z];const int i=old.gun;
                    if(i>=n || reportedPursuits.contains(i) || std::any_of(out.survivor.shots.begin()+z+1,out.survivor.shots.end(),
                        [&](const auto &later){return later.gun==i;})) continue;
                    reportedPursuits.insert(i);
                    auto rounds=ammo[i];int remaining=rights[i].remainingFuel;
                    for(const auto &hit:activation.shots) if(hit.gun==i)
                    {spendCombatRound(rounds,hit.weapon);if(remaining>=0) remaining-=hit.motion.fuel;}
                    const int allowance=rights[i].mayRelocate && actors[i].canMoveAndFire ?
                        (remaining<0 ? shadow.units[i].movement : std::min(shadow.units[i].movement,remaining)) : 0;
                    const auto &fields=defense.firingOrigins;auto field=fields.find({i,tick+1,cell(old.from),allowance,cell(at)});
                    if(field==fields.end()) field=fields.find({i,tick+1,cell(old.from),allowance,-1});
                    QJsonArray lanes;
                    if(actors[i].minRange==1 && actors[i].maxRange==1) for(auto dir:ENTRY_DIRECTIONS)
                    {
                        const auto d=entryStep(dir);TilePoint to{at.x+d.dx,at.y+d.dy};
                        if(to.x<0 || to.y<0 || to.x>=shadow.width || to.y>=shadow.height) continue;
                        QJsonArray holders;
                        for(const auto &r:timeline.uses(cell(to)))
                            if(r.actor!=i && r.actor!=n+index && r.last>=tick+1 && r.stopped)
                                holders.append(QJsonObject{{"actor",r.actor>=0&&r.actor<n?shadow.units[r.actor].engineUnitId:-1},
                                    {"start",r.first},{"end",r.last},{"side",r.side}});
                        lanes.append(QJsonObject{{"tile",QJsonArray{to.x,to.y}},{"cost",field==fields.end()?-1:field->second.cost[cell(to)]},
                            {"stopCompatible",physical(i,tick+1,to,false)},{"holders",holders}});
                    }
                    out.pursuitChecks.append(QJsonObject{{"gun",shadow.units[i].engineUnitId},{"tick",tick+1},
                        {"previousPost",QJsonArray{old.from.x,old.from.y}},{"originalDatedPost",QJsonArray{position[i].x,position[i].y}},
                        {"fuel",remaining},{"movement",allowance},{"ammo",QJsonArray{rounds[0],rounds[1]}},
                        {"free",!view.available || view.available(i,tick+1)},{"differentJob",static_cast<qint64>(rights[i].alternativeValue)},
                        {"target",QJsonArray{at.x,at.y}},{"adjacentLanes",lanes}});
                }
            if(capture) out.installments.append(QJsonObject{{"tick",tick},{"hpBeforeReply",initial},{"carryBefore",prior},{"carryAfter",out.survivor.carry}});
            return true;
        };
        for(const auto &step:path.steps)
        {
            if(crossedInterceptPost(out.survivor.shots,step,width,u.ignoresUnitCollision))
            {out.status="INTERCEPT_POST_BLOCKS_PAID_ROUTE";return out;}
            if(!advance(step.tile,step.tick,step.tick==path.entryTick)) return {};
            if(!out.status.isEmpty() || out.survivor.hp<=0) break;
        }
        for(int t=path.entryTick+2;out.status.isEmpty() && out.survivor.hp>0 && out.survivor.completedTick<0 && t<=last;t+=2)
        {
            if(!timeline.open(n+index,side,square,t,true) || !timeline.open(n+index,side,square,t+1,true))
            {out.status="HOLD_CONFLICT";break;}
            if(!advance(property,t,true)) return {};
        }
        if(out.status.isEmpty())
        {
            if(out.survivor.hp<=0) out.status="LETHAL_REPLY";
            else if(out.survivor.completedTick<0) out.status="OUTSIDE_HORIZON";
            else
            {
                for(const auto &s:out.survivor.shots)
                    for(const auto &step:path.steps) if(s.gun<n && s.shooterAfter>0 && step.tick>s.shotTick &&
                        std::none_of(out.survivor.shots.begin(),out.survivor.shots.end(),[&](const auto &later){
                            return later.gun==s.gun && (later.from!=s.from || later.shooterAfter<=0) &&
                                s.shotTick<later.shotTick && later.shotTick<step.tick;
                        }) && (step.tile==s.from ||
                        std::find(step.cells.begin(),step.cells.end(),cell(s.from))!=step.cells.end()))
                    {out.status="DEFENDER_BLOCKS_CONTINUATION";return out;}
                out.status="WITNESSED";out.finish=out.survivor.completedTick;
            }
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if(view.originalRangeAudit && out.paidDuelChecks.isEmpty() && !out.status.isEmpty() && out.finish<0)
        {
            // A newly proved earlier stock conflict can precede the old range
            // alarm. Trace the old hypothetical occupancies without crediting
            // a future shot or replacing the earlier rejecting reason.
            const auto preceding=out.status;
            auto trace=[&](TilePoint tile,int own){
                out.status.clear();std::int64_t auditWork=100000;
                defense.paidResponders(own+1,tile,out.survivor,auditWork);
                if(out.paidDuelChecks.isEmpty())return false;
                auto row=out.paidDuelChecks[0].toObject();row["unreachedAfter"]=preceding;
                row.remove("capperHp");row.remove("isolatedTargetAfter");row.remove("isolatedShooterAfter");
                out.paidDuelChecks[0]=row;return true;
            };
            bool found=false;
            for(const auto &step:path.steps)if(trace(step.tile,step.tick)){found=true;break;}
            for(int tick=path.entryTick+2;!found && tick<last;tick+=2)found=trace(property,tick);
            out.status=preceding;
        }
#endif
        return out;
    }
}
