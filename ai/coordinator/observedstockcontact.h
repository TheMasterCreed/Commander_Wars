#pragma once

#include "ai/coordinator/observedstockengagement.h"
#include "ai/coordinator/paidrecruitreply.h"
#include "ai/coordinator/observedstockscreen.h"

namespace Coordinator
{
    struct StockContactPiece
    {
        int actor{NO_UNIT},side{};
        TilePoint post{INVALID_TILE};
        double hp{};
        std::array<int,2> ammo{};
        MilliFunds price{};
    };
    struct StockContactReply
    {
        bool staged{};
        int capTick{-1},paidFuel{};
        StockContactPiece enemy,stock,paid;
        std::vector<TownAssaultHit> hits;
        std::vector<StockCaptureStep> paidWalk,recapture;
        std::vector<std::pair<int,MilliFunds>> losses;
        QJsonArray screenChecks;
        bool preservesScreens{true};
        int removedStockVictimTick{-1};
        std::optional<FundedInterceptionPrice> result,abandon;
    };
    inline std::optional<TownAssaultHit> checkedStockContactHit(const CombatTerrain &terrain,
        std::span<const KnownUnit> actors,const FundedCaptureGun &reserve,int paid,StockContactPiece &a,StockContactPiece &v,int tick,
        std::int64_t &work)
    {
        const int n=static_cast<int>(actors.size()),d=manhattanDistance(a.post,v.post);
        if(a.hp<=0 || v.hp<=0 || d!=1 || a.side==v.side || !terrain.defense || !terrain.weaponSlots ||
           !reserve.unit || a.price<=0 || v.price<=0)return {};
        const auto &g=*reserve.unit;
        if(terrain.production)
        {
            if(!terrain.production->contactDamage)return {};
            const auto ref=[&](const StockContactPiece &who){
                return NativeContactPiece{who.actor==paid?NO_UNIT:who.actor,who.side,who.actor==paid?g.type:QString{},
                    who.post,static_cast<int>(std::lround(who.hp*10)),who.ammo};
            };
            const auto q=terrain.production->contactDamage(ref(a),ref(v),work);
            if(!q || q->attack<0 || q->weapon<0 || q->weapon>1 || a.ammo[q->weapon]==0 || (q->counter>=0 &&
               (q->counterWeapon<0 || q->counterWeapon>1 || v.ammo[q->counterWeapon]==0)))return {};
            TownAssaultHit h{a.actor,v.actor,tick,q->weapon,q->counter>=0?q->counterWeapon:-1,
                a.post,v.post,a.hp,a.hp,v.hp,v.hp,0,0};
            v.hp=engineHpAfterReply(v.hp,engineShotDamage(q->attack));spendCombatRound(a.ammo,q->weapon);
            if(q->counter>=0)
            {
                a.hp=engineHpAfterReply(a.hp,engineShotDamage(q->counter));spendCombatRound(v.ammo,q->counterWeapon);
            }
            h.actorAfter=a.hp;h.targetAfter=v.hp;h.targetLoss=combatCapitalLoss(v.price,h.targetBefore,v.hp);
            h.counterLoss=combatCapitalLoss(a.price,h.actorBefore,a.hp);return h;
        }
        const auto power=[&](const StockContactPiece &p,const StockContactPiece &target)->std::optional<std::array<double,2>>{
            if(p.actor<0 || target.actor<0)return {};
            if(p.actor!=paid && p.actor>=n)return {};
            if(target.actor!=paid && target.actor>=n)return {};
            const int lo=p.actor==paid?g.minRange:actors[p.actor].minRange,hi=p.actor==paid?g.maxRange:actors[p.actor].maxRange;
            if(d<lo || d>hi)return std::array<double,2>{-1,-1};
            if(p.actor!=paid && target.actor!=paid)return terrain.weaponSlots(p.actor,target.actor,d);
            const int stock=p.actor==paid?target.actor:p.actor;
            if(terrain.production && terrain.production->paidStockSlots)
            {
                const auto raw=terrain.production->paidStockSlots(reserve,stock,d);if(!raw)return {};
                return (*raw)[int(p.actor==paid)];
            }
            const auto &table=p.actor==paid?g.captureCounter:g.againstCapper;
            if(stock>=static_cast<int>(table.size()) || d!=lo)return {};
            return table[stock];
        };
        const auto cover=[&](const StockContactPiece &p)->std::optional<TerrainDefenseRule>{
            if(p.actor!=paid)return terrain.defense(p.actor,p.post);
            const int sq=p.post.y*terrain.width+p.post.x;
            if(sq<0 || sq>=static_cast<int>(g.routeAt.size()) || g.routeAt[sq]<0 ||
               g.routeAt[sq]>=static_cast<int>(g.defense.size()))return {};
            return g.defense[g.routeAt[sq]];
        };
        const auto forward=power(a,v),back=power(v,a);const auto ca=cover(a),cv=cover(v);
        if(!forward || !back || !ca || !cv)return {};
        const auto hit=usableOpeningWeapon(*forward,a.ammo),counter=usableOpeningWeapon(*back,v.ammo);
        if(hit<0)return {};
        auto h=TownAssaultHit{a.actor,v.actor,tick,hit,-1,a.post,v.post,a.hp,a.hp,v.hp,v.hp,0,0};
        v.hp=engineHpAfterReply(v.hp,engineShotDamage(terrainReducedDamage(*cv,(*forward)[hit],ceilHpSteps(a.hp),ceilHpSteps(v.hp))));
        spendCombatRound(a.ammo,hit);
        if(v.hp>0 && counter>=0)
        {
            h.counterWeapon=counter;
            a.hp=engineHpAfterReply(a.hp,engineShotDamage(terrainReducedDamage(*ca,(*back)[counter],ceilHpSteps(v.hp),ceilHpSteps(a.hp))));
            spendCombatRound(v.ammo,counter);
        }
        h.actorAfter=a.hp;h.targetAfter=v.hp;
        h.targetLoss=combatCapitalLoss(v.price,h.targetBefore,v.hp);h.counterLoss=combatCapitalLoss(a.price,h.actorBefore,a.hp);
        return h;
    }

    inline std::optional<TilePoint> uniqueStockPost(const StockCaptureTraffic &traffic,int actor,int tick,int width,std::int64_t &work)
    {
        int latest=-1,post=-1;
        bool conflict=false;
        for(int sq=0;sq<traffic.cellCount();++sq)
        {
            if(work<=0)return {};--work;
            for(const auto &s:traffic.uses(sq))if(s.actor==actor && s.stopped && s.first<=tick && tick<=s.last)
            {
                if(s.first>latest){post=sq;latest=s.first;conflict=false;}
                else if(s.first==latest && post!=sq)conflict=true;
            }
        }
        return post<0 || conflict?INVALID_TILE:TilePoint{post%width,post/width};
    }

    // Compare the attack, the stock victim's in-place return, and each
    // physically available ordering with the reserve. Both sides start from
    // the same observed stock position. Later unassigned units are separate
    // alternatives and get no implied pass or booked damage.
    template<class Timing>
    std::optional<std::vector<StockContactReply>> stockContactResponses(const CombatTerrain &t,
        std::span<const KnownUnit> units,const StockCaptureTraffic &original,int tick,int cutoff,int cap,int victim,int paid,
        StockContactPiece enemy,StockContactPiece stock,StockContactPiece bought,const FundedCaptureGun &g,
        const FundedCaptureFork &fork,TilePoint firing,const PaidDuelWalk &enemyMove,const FundedInterceptPath &loan,
        bool staged,const PropertyStockColumn &property,int finish,int threshold,std::span<const MilliFunds> cash,
        const QJsonArray &orders,StockRouteLowerBounds &lower,Timing timing,std::int64_t &work,
        const StockCaptureSettlement *settled=nullptr,bool allScreensProved=true)
    {
        std::vector<StockContactReply> replies;
        if(enemy.actor!=cap || stock.actor!=victim || bought.actor!=paid || !g.unit || tick!=fork.tick ||
           tick+1>cutoff || fork.calendar!=&original || fork.revision!=original.generation())return replies;
        const auto &p=t.units[cap];const auto &u=units[cap];const auto &piece=*g.unit;
        const int enemyGrid=p.grid>=0?p.grid:p.travelGrid;
        if(enemyGrid<0 || !piece.movementGrid || loan.steps.empty())return replies;
        const auto cell=[&](TilePoint v){return v.y*t.width+v.x;};
        StockCaptureTraffic calendar=original;
        calendar.removeFutureUses(cap,tick);calendar.removeFutureUses(paid,staged?loan.departure:tick+1);
        if(staged)bought.post=loan.steps.front().tile;
        if(staged)
        {
            for(const auto sq:loan.steps.front().cells)calendar.reserve(sq,loan.departure,loan.departure,bought.side,paid,false);
            calendar.reserve(cell(bought.post),loan.departure,tick,bought.side,paid,true);
        }
        if(!calendar.open(cap,enemy.side,cell(firing),tick,true) || !calendar.open(cap,enemy.side,cell(firing),tick+1,true))return replies;
        for(int sq:enemyMove.cells)if(!p.ignoresCollision && !calendar.open(cap,enemy.side,sq,tick,false))return replies;
        enemy.post=firing;
        auto shot=checkedStockContactHit(t,units,g,paid,enemy,stock,tick,work);
        if(!shot)return replies;
        for(int sq:enemyMove.cells)calendar.reserve(sq,tick,tick,enemy.side,cap,false);
        if(enemy.hp>0)calendar.reserve(cell(firing),tick,cutoff,enemy.side,cap,true);
        if(stock.hp<=0)calendar.removeFutureUses(victim,tick);
        const auto open=[&](TilePoint at,int when,bool stop){
            return (!stop && piece.ignoresUnitCollision) || calendar.open(paid,bought.side,cell(at),when,stop);
        };
        int fuel=g.fuelAtPost,stageTravel=0;
        if(staged)
        {
            auto prev=g.destination;
            for(int sq:loan.steps.front().cells)
            {
                TilePoint next{sq%t.width,sq/t.width};
                const int cost=piece.movementGrid->cost(next.x,next.y,entryDirection(prev.x,prev.y,next.x,next.y));
                if(cost<0 || (fuel>=0 && cost>fuel))return replies;
                if(fuel>=0)fuel-=cost;
                stageTravel+=cost;prev=next;
            }
        }
        auto price=[&](const std::vector<int> &order,const PaidDuelWalk &walk,TilePoint end)->bool{
            if(work<=0)return false;--work;
            StockContactReply q;q.staged=staged;q.enemy=enemy;q.stock=stock;q.paid=bought;q.hits.push_back(*shot);
            q.removedStockVictimTick=stock.hp<=0?tick:-1;
            if(walk.legal)q.paid.post=end;
            auto traffic=calendar;
            q.paidFuel=walk.fuel+stageTravel;
            if(walk.legal)
            {
                q.paidWalk.push_back({tick+1,end,walk.cells});
                for(int sq:walk.cells)traffic.reserve(sq,tick+1,tick+1,bought.side,paid,false);
            }
            for(int actor:order)
            {
                if(q.enemy.hp<=0)break;
                auto &source=actor==paid?q.paid:q.stock;
                if(source.hp<=0)return true;
                auto hit=checkedStockContactHit(t,units,g,paid,source,q.enemy,tick+1,work);if(!hit)return work>0;
                q.hits.push_back(*hit);
                if(source.hp<=0){traffic.removeFutureUses(actor,tick+1);if(actor==victim)q.removedStockVictimTick=tick+1;}
            }
            if(walk.legal && std::none_of(q.hits.begin(),q.hits.end(),[&](const auto &hit){return hit.actor==paid;}))return true;
            if(q.enemy.hp<=0)traffic.removeFutureUses(cap,tick+1);
            for(const auto &event:q.hits)
            {
                const int side=event.actor==cap?enemy.side:stock.side;
                q.losses.emplace_back(event.tick,(side?1:-1)*(event.targetLoss-event.counterLoss));
            }
            if(q.paid.hp>0)
            {
                const int first=tick+1+2*int(walk.legal);
                if(q.paid.post==g.destination)traffic.reserve(cell(q.paid.post),tick+1,cutoff,q.paid.side,paid,true);
                else
                {
                    traffic.reserve(cell(q.paid.post),tick+1,first-1,q.paid.side,paid,true);
                    const int lastOwn=cutoff-int(cutoff%2!=q.paid.side);
                    auto back=stockCaptureRoute(*piece.movementGrid,traffic,paid,q.paid.side,q.paid.post,g.destination,
                        piece.movement,fuel<0?-1:fuel-walk.fuel,first,first,lastOwn,1,work,INVALID_TILE,&lower,{},piece.ignoresUnitCollision);
                    if(!back)return false;
                    if(back->entryTick<0)return true;
                    q.paidFuel+=back->fuel;
                    for(const auto &s:back->steps)
                    {
                        for(int x:s.cells)traffic.reserve(x,s.tick,s.tick,q.paid.side,paid,false);
                        traffic.reserve(cell(s.tile),s.tick,s.tick+1,q.paid.side,paid,true);q.paidWalk.push_back(s);
                    }
                    traffic.reserve(cell(g.destination),back->entryTick,cutoff,q.paid.side,paid,true);
                }
            }
            if(q.enemy.hp>0)
            {
                traffic.removeFutureUses(cap,tick+2);
                const int rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(q.enemy.hp),false);
                if(rate<=0)return true;
                const int installments=widenedCaptureTurns(0,rate,threshold);
                const int lastEntry=cutoff-int(cutoff%2!=enemy.side)-2*(installments-1);
                if(lastEntry>=tick+2)
                {
                    const auto recapture=stockCaptureRoute(t.grids[enemyGrid],traffic,cap,enemy.side,firing,property.tile,p.movement,
                        fork.fuel<0?-1:fork.fuel-enemyMove.fuel,tick+2,tick+2,lastEntry,installments,work,INVALID_TILE,&lower,{},p.ignoresCollision);
                    if(!recapture)return false;
                    if(recapture->entryTick>=0)
                    {
                        q.capTick=recapture->entryTick+2*(installments-1);q.recapture=recapture->steps;
                        for(const auto &stop:q.recapture)
                        {
                            for(int sq:stop.cells)traffic.reserve(sq,stop.tick,stop.tick,enemy.side,cap,false);
                            traffic.reserve(cell(stop.tile),stop.tick,stop.tick+1,enemy.side,cap,true);
                        }
                        traffic.reserve(cell(property.tile),recapture->entryTick,cutoff,enemy.side,cap,true);
                    }
                }
            }
            if(settled && (!allScreensProved || q.stock.hp<=0))
            {
                std::vector<ChangedScreenActor> affected{{cap,tick},{paid,staged?loan.departure:tick+1}};
                if(q.removedStockVictimTick>=0)affected.push_back({victim,q.removedStockVictimTick});
                auto screen=reviewObservedScreens(t,*settled,affected,&traffic,lower,work);
                q.screenChecks=std::move(screen.checks);q.preservesScreens=screen.complete && screen.preserved;
            }
            q.result=replaceProvedCapture(enemy.side,finish,q.capTick,property,cash,orders,q.losses,cutoff,timing);
            q.abandon=q.capTick<0?q.result:replaceProvedCapture(enemy.side,finish,-1,property,cash,orders,q.losses,cutoff,timing);
            if(q.result && q.abandon)replies.push_back(std::move(q));
            return true;
        };
        if(!price({}, {},bought.post) || !price({victim}, {},bought.post))return {};
        for(const auto end:TownAssaultDetail::firingSquares(t,firing,bought.post,piece.minRange,piece.maxRange))
        {
            if(manhattanDistance(end,firing)!=1 || (end!=bought.post && !piece.mobileFire))continue;
            const auto walk=paidDuelWalk(*piece.movementGrid,bought.post,end,piece.movement,fuel,tick+1,open,work);
            if(!walk)return {};
            if(!walk->legal)continue;
            if(!price({paid},*walk,end) || !price({paid,victim},*walk,end) || !price({victim,paid},*walk,end))return {};
        }
        return replies;
    }

    template<class Timing>
    std::optional<QJsonArray> observedContactAlternatives(const CombatTerrain &t,std::span<const KnownUnit> units,
        std::span<const double> hp,std::span<const std::array<int,2>> ammo,std::span<const int> lastShot,
        std::span<const int> lastHurt,const StockCaptureSettlement &settled,const ProductionProjection &production,
        const StockCaptureTraffic &original,const FundedCaptureFork &fork,int cap,int reserve,
        const FundedInterceptPath &loan,const PropertyStockColumn &property,int nominal,int threshold,int cutoff,
        const QJsonArray &orders,std::span<const MilliFunds> cash,StockRouteLowerBounds &lower,Timing timing,
        std::span<const int> targets,std::int64_t &work,bool allScreensProved=true)
    {
        QJsonArray result;
        const int n=static_cast<int>(units.size()),paid=n+reserve,tick=fork.tick;
        const auto &p=t.units[cap];const auto &u=units[cap];const auto &g=production.fundedGuns[reserve];
        const int profile=p.grid>=0?p.grid:p.travelGrid;
        if(profile<0 || !g.unit || g.exposure!=0 || g.observedService || tick<3 || tick+1!=loan.shot ||
           u.minRange>1 || u.maxRange<1 || !t.weaponSlots || hp.size()!=units.size() || ammo.size()!=units.size() ||
           lastShot.size()!=units.size() || lastHurt.size()!=units.size())return result;
        const int side=int(p.enemy);
        auto relaxed=original;relaxed.removeFutureUses(cap,tick);relaxed.removeFutureUses(paid,loan.departure);
        const auto cell=[&](TilePoint z){return z.y*t.width+z.x;};
        const auto xy=[](TilePoint z){return QJsonArray{z.x,z.y};};
        const auto piece=[&](int actor,TilePoint post,double health,std::array<int,2> rounds){
            return StockContactPiece{actor,int(t.units[actor].enemy),post,health,rounds,t.units[actor].replacementCost};
        };
        const StockContactPiece enemy=piece(cap,fork.tile,fork.hp,fork.rounds);
        const StockContactPiece bought{paid,g.side,g.destination,g.unit->startingHp,g.unit->captureLoad,g.unit->cost};
        for(int victim:targets)
        {
            if(victim<0 || victim>=n || hp[victim]<=0 || int(t.units[victim].enemy)==side || settled.townBusyStock.contains(victim) ||
               2*lastShot[victim]+int(t.units[victim].enemy)>=tick || lastHurt[victim]>=tick ||
               (settled.townGunDutyThrough.contains(victim) && settled.townGunDutyThrough.at(victim)>=tick))continue;
            const bool assigned=std::any_of(production.fundedGuns.begin(),production.fundedGuns.end(),[&](const auto &gun){
                return gun.targetUnit==victim && gun.impactTick>=tick;});
            if(assigned)continue;
            const auto post=uniqueStockPost(original,victim,tick-1,t.width,work);if(!post)return {};
            if(*post==INVALID_TILE)continue;
            const auto following=uniqueStockPost(original,victim,tick+1,t.width,work);if(!following)return {};
            if(*following!=*post)continue;
            const auto stock=piece(victim,*post,hp[victim],ammo[victim]);
            for(const auto fire:TownAssaultDetail::firingSquares(t,*post,fork.tile,1,1))
            {
                if(fire!=fork.tile && !u.canMoveAndFire)continue;
                const auto open=[&](TilePoint z,int when,bool stop){
                    return (!stop && p.ignoresCollision) || relaxed.open(cap,side,cell(z),when,stop);
                };
                const auto route=paidDuelWalk(t.grids[profile],fork.tile,fire,p.movement,fork.fuel,tick,open,work);
                if(!route)return {};
                if(!route->legal)continue;
                QJsonObject row{{"enemy",t.units[cap].engineUnitId},{"victim",t.units[victim].engineUnitId},
                    {"tick",tick},{"observedEnemy",xy(fork.tile)},{"enemyFiringSquare",xy(fire)},{"stockVictimTile",xy(*post)},
                    {"damageSource",t.production?"NATIVE_CURRENT_FIELDS_HP_AMMO":"CONTROL_TERRAIN_PROJECTION"},
                    {"mapWideCombatConditions","retained saved area effects, owners and weather"},
                    {"enemyMovementFuel",route->fuel},{"paidBirth",xy(g.factory)},{"conditionalOnOrdinaryShot",true}};
                std::array<std::optional<MilliFunds>,2> best;
                for(bool stage:{false,true})
                {
                    auto all=stockContactResponses(t,units,original,tick,cutoff,cap,victim,paid,enemy,stock,bought,g,fork,
                        fire,*route,loan,stage,property,nominal,threshold,cash,orders,lower,timing,work,&settled,allScreensProved);
                    if(!all)return {};
                    QJsonArray quotes;
                    for(const auto &r:*all)
                    {
                        QJsonArray hits;
                        for(const auto &s:r.hits)hits.append(QJsonObject{{"actor",s.actor==paid?NO_UNIT:t.units[s.actor].engineUnitId},
                            {"actorIsPaid",s.actor==paid},{"target",t.units[s.target].engineUnitId},{"tick",s.tick},{"from",xy(s.from)},{"to",xy(s.to)},
                            {"weapon",s.weapon},{"counterWeapon",s.counterWeapon},{"shooterAfter",s.actorAfter},{"targetAfter",s.targetAfter}});
                        const auto steps=[&](const std::vector<StockCaptureStep> &path){
                            QJsonArray out;for(const auto &s:path)
                            {
                                QJsonArray crossed;for(int sq:s.cells)crossed.append(xy({sq%t.width,sq/t.width}));
                                out.append(QJsonObject{{"tick",s.tick},{"stop",xy(s.tile)},{"cells",crossed}});
                            }
                            return out;
                        };
                        const auto load=[](const auto &q){return QJsonArray{q[0],q[1]};};
                        const auto capture=r.result->change.score,abandon=r.abandon->change.score;
                        const auto enemyPrice=TownAssaultDetail::better(abandon,capture,side)?abandon:capture;
                        if(r.preservesScreens && (!best[stage] || TownAssaultDetail::better(enemyPrice,*best[stage],g.side)))best[stage]=enemyPrice;
                        quotes.append(QJsonObject{{"conditionalTransfer",r.capTick},{"conditionalDelta",static_cast<qint64>(r.result->change.score)},
                            {"preservesEarlierStockScreens",r.preservesScreens},{"actualScreenChecks",r.screenChecks},{"stockScreenVictim",t.units[victim].engineUnitId},
                            {"stockScreenHpAfter",r.stock.hp},{"removedStockVictimTick",r.removedStockVictimTick},
                            {"ifSurvivingCapperDeclines",static_cast<qint64>(abandon)},{"capOrDeclinePrice",static_cast<qint64>(enemyPrice)},
                            {"newHealth",QJsonArray{r.enemy.hp,r.stock.hp,r.paid.hp}},{"ammoAfter",QJsonArray{load(r.enemy.ammo),load(r.stock.ammo),load(r.paid.ammo)}},
                            {"paidActionPost",xy(r.paid.post)},{"paidRouteFuel",r.paidFuel},{"paidFiringAndReturn",steps(r.paidWalk)},
                            {"unopposedRecapture",steps(r.recapture)},{"hits",hits}});
                    }
                    row[stage?"whenReserveStaged":"whenReserveKeptPost"]=quotes;
                }
                if(best[0] && best[1])
                {
                    row["bestRetainedDefenseAfterCapOrDecline"]=static_cast<qint64>(*best[0]);
                    row["bestStagedDefenseAfterCapOrDecline"]=static_cast<qint64>(*best[1]);
                    row["otherFutureStockActionsAndReplacementsOpen"]=true;
                }
                if(!row["whenReserveStaged"].toArray().isEmpty() || !row["whenReserveKeptPost"].toArray().isEmpty())result.append(row);
            }
        }
        return result;
    }
}
