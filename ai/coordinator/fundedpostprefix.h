#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace Coordinator
{
    struct FundedPostPrefix
    {
        QString digest;
        QJsonObject components,open;
        bool complete() const {return !digest.isEmpty() && open.isEmpty();}
        QJsonObject record() const
        {return {{"digest",digest},{"components",components},{"complete",complete()},{"open",open}};}
    };

    // The same frozen native opening and capture-race branch are prerequisites
    // at the caller. These are the realized choices on that branch before the
    // recruit's first activation. Rejected paths are counterfactual.
    template<class Race>
    FundedPostPrefix measureFundedPostPrefix(const Race &race,int first,const CombatTerrain &terrain,
        const StockCaptureSettlement &settled,const ProductionProjection &paid,
        std::span<const ProductionProperty> titles,const QJsonArray &originals,const QJsonArray &openingOrders,
        std::span<const std::pair<int,MilliFunds>> losses,std::span<const MilliFunds> cash,
        std::int64_t &work)
    {
        FundedPostPrefix result;
        const auto xy=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const auto missing=[&](const char *reason,const QJsonObject &source=QJsonObject{}){
            if(!result.open.isEmpty())return;
            result.open={{"reason",reason}};
            for(const auto &key:{"tile","factory","unit","capper","entryTick","proposedEntryTick","departureTick","status","firstPossible"})
                if(source.contains(key))result.open[key]=source[key];
        };
        const auto charge=[&](qint64 cost){
            if(cost<0 || work<cost){missing("PREDEPLOYMENT_HISTORY_WORK_LIMIT");return false;}
            work-=cost;return true;
        };
        const auto save=[&](const QString &part,const QJsonArray &events){
            const auto raw=QJsonDocument(events).toJson(QJsonDocument::Compact);
            if(!charge(1+raw.size()/64))return false;
            result.components[part]=QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex());
            return true;
        };
        if(first<=0 || first>2*std::int64_t(cash.size()) || originals.size()!=int(terrain.units.size()) || !settled.townTraffic)
        {missing("MISSING_PREDEPLOYMENT_ROSTER_OR_TRAFFIC");return result;}
        const auto &calendar=*settled.townTraffic;
        if(calendar.lastTick()<first-1 || calendar.cellCount()!=terrain.width*terrain.height)
        {missing("INCOMPLETE_PREDEPLOYMENT_TRAFFIC");return result;}
        if(!save("originalActors",originals))return result;
        // These alternatives belong to the common F0/E0 branch. Keeping a
        // later booked act here can refuse an equivalence conservatively.
        if(!save("originalOrders",{openingOrders,race.guardAction,race.holds,race.withdrawals,race.returnFire,
            race.combinedFollowup,race.enemyCaptureActions,race.homeCaptureResponse,race.approachRecord,race.boxContinuation}))return result;
        QJsonArray traffic;
        using Use=std::tuple<int,int,int,int,int,bool>;
        std::vector<Use> uses;
        std::map<std::pair<int,int>,int> stops;
        for(int cell=0;cell<calendar.cellCount();++cell)
        {
            const auto &bookings=calendar.uses(cell);if(!charge(1+bookings.size()))return result;
            for(const auto &u:bookings)
            {
                // This stock calendar is built from post-E0 positions. The
                // native opening above supplies the two actual earlier phases.
                const int a=std::max(2,u.first),b=std::min(u.last,first-1);
                if(a>b)continue;
                uses.emplace_back(cell,a,b,u.side,u.actor,u.stopped);
                if(u.stopped && u.actor>=0)
                {
                    if(!charge(b-a+1))return result;
                    for(int tick=a;tick<=b;++tick)
                    {
                        auto [it,inserted]=stops.emplace(std::pair{u.actor,tick},cell);
                        if(!inserted && it->second!=cell)
                            missing("CONTRADICTORY_RETAINED_STOPS_BEFORE_POST",{{"unit",u.actor<int(terrain.units.size())?
                                terrain.units[u.actor].engineUnitId:NO_UNIT},{"tile",xy({cell%terrain.width,cell/terrain.width})},
                                {"entryTick",tick}});
                    }
                }
            }
        }
        std::sort(uses.begin(),uses.end());
        for(const auto &[cell,a,b,side,actor,stopped]:uses)traffic.append(QJsonArray{cell,a,b,side,actor,stopped});
        if(!save("retainedOccupancyAndTransit",traffic))return result;
        const auto dates=[&](const QJsonArray &rows,const QString &kind,const QJsonValue &actor,QJsonArray &book){
            if(!charge(1+rows.size()))return;
            for(const auto &value:rows)
            {
                const auto r=value.toObject();const int tick=r["tick"].toInt(-1);
                if(tick<0){missing("UNDATED_EARLY_ACTION",r);continue;}
                if(tick<first)book.append(QJsonArray{kind,actor,r});
            }
        };
        QJsonArray movements,shots,instalments,ownership,material,buyers;
        for(const auto &v:race.routeProofs)
        {
            if(!charge(1))return result;
            const auto leg=v.toObject();const auto status=leg.value("status").toString();
            if(status!="WITNESSED")
            {
                const int departure=leg["departureTick"].toInt(-1);
                if(departure<first && (status.startsWith("DEFERRED") || status.startsWith("UNSUPPORTED") || status.isEmpty()))
                    missing("UNRESOLVED_STOCK_DEPARTURE_BEFORE_POST",leg);
                continue;
            }
            const auto actor=leg["unit"];
            if(leg["departureTick"].toInt(-1)<first && leg["stops"].toArray().isEmpty())
                missing("EARLY_STOCK_PATH_NOT_RECORDED",leg);
            dates(leg["stops"].toArray(),"stock",actor,movements);
            const auto from=leg["from"].toArray();
            TilePoint previous{from.size()==2?from[0].toInt(-1):-1,from.size()==2?from[1].toInt(-1):-1};
            const auto pawn=std::find_if(terrain.units.begin(),terrain.units.end(),[&](const auto &p){return p.engineUnitId==actor.toInt(-1);});
            for(const auto &v:leg["stops"].toArray())
            {
                const auto step=v.toObject();const int tick=step["tick"].toInt(-1);if(tick<0 || tick>=first)continue;
                const auto cells=step["cells"].toArray();if(!charge(1+cells.size()))return result;
                int spent=0;
                if(pawn==terrain.units.end() || pawn->grid<0 || pawn->grid>=int(terrain.grids.size()))
                    {missing("EARLY_STOCK_MOVEMENT_GRID_MISSING",leg);break;}
                const auto &grid=terrain.grids[pawn->grid];
                for(const auto &v:cells)
                {
                    const auto point=v.toArray();
                    if(point.size()!=2){missing("EARLY_STOCK_TRANSIT_MISSING",leg);break;}
                    TilePoint next{point[0].toInt(-1),point[1].toInt(-1)};
                    if(!grid.onGrid(previous.x,previous.y) || !grid.onGrid(next.x,next.y) || manhattanDistance(previous,next)!=1)
                        {missing("EARLY_STOCK_ROUTE_NOT_CONTIGUOUS",leg);break;}
                    const int cost=grid.cost(next.x,next.y,entryDirection(previous.x,previous.y,next.x,next.y));
                    if(cost<0 || cost>std::numeric_limits<int>::max()-spent){missing("EARLY_STOCK_FUEL_UNVERIFIED",leg);break;}
                    spent+=cost;previous=next;
                }
                if(step["tile"].toArray()!=xy(previous))missing("EARLY_STOCK_ENDPOINT_UNVERIFIED",leg);
                movements.append(QJsonArray{"stockFuel",actor,tick,spent});
            }
            dates(leg["shots"].toArray(),"stock",actor,shots);
            dates(leg["captureInstallments"].toArray(),"stock",actor,instalments);
        }
        // A selected town initiative can affect HP or a post even without
        // completing a title. Its capper's title certificate alone is insufficient.
        for(const auto &v:race.townEncounters)
        {
            if(!charge(1))return result;
            const auto row=v.toObject();if(!row["appliedToSchedule"].toBool())continue;
            const auto line=row.contains("jointReleaseLine")?row["jointReleaseLine"].toObject():
                row[row["responseChosen"].toBool()?"response":"assault"].toObject();
            if(line.isEmpty()){missing("UNBOUND_TOWN_INITIATIVE",row);continue;}
            for(const auto &m:line["moves"].toArray())
            {
                const auto move=m.toObject();
                dates(move["route"].toArray(),"town",move["actor"],movements);
                if(move["route"].toArray().isEmpty() && move["actionTick"].toInt(-1)<first)
                    movements.append(QJsonArray{"town",move["actor"],move});
            }
            dates(line["shots"].toArray(),"town",line["capper"],shots);
            for(const auto &s:line["installments"].toArray())
            {
                const auto a=s.toArray();if(a.size()!=2 || a[0].toInt(-1)<0){missing("UNDATED_TOWN_INSTALLMENT",line);continue;}
                if(a[0].toInt()<first)instalments.append(QJsonArray{"town",line["capper"],line["tile"],a});
            }
            const int finish=line["finishTick"].toInt(-1);
            if(finish>=0 && finish<first)ownership.append(QJsonArray{"town",line["tile"],finish,line["enemy"]});
        }
        for(const auto &col:titles)
        {
            if(!charge(1+col.ownershipChanges.size()))return result;
            ownership.append(QJsonArray{"titleAtStart",xy(col.tile),int(col.column.ownerBefore),
                static_cast<qint64>(col.column.income.oursPerTurn),static_cast<qint64>(col.column.income.enemyPerTurn)});
            if(col.ownershipChanges.empty() && col.existingCompletion>=0 && 2*col.existingCompletion+int(col.existingEnemy)<first)
                ownership.append(QJsonArray{"stock",xy(col.tile),2*col.existingEntry+int(col.existingEnemy),
                    2*col.existingCompletion+int(col.existingEnemy),int(col.existingEnemy?OwnerSign::Enemy:OwnerSign::Ours)});
            for(const auto &event:col.ownershipChanges)if(event.completionTick<first)
                ownership.append(QJsonArray{"stock",xy(col.tile),event.entryTick,event.completionTick,int(event.owner)});
        }
        for(const auto &[tick,loss]:losses)if(tick>=0 && tick<first)material.append(QJsonArray{tick,static_cast<qint64>(loss)});
        for(int r=0;2*(r+1)<first;++r)material.append(QJsonArray{"completedRound",r,static_cast<qint64>(cash[r])});
        for(const auto &v:race.productionOrders)
        {
            if(!charge(1))return result;
            const auto o=v.toObject();const int built=o["buildHalfTurn"].toInt(-1);
            if(built<0){missing("UNDATED_PURCHASE",o);continue;}
            if(built>=first)continue;
            if(!o["treasuryBefore"].isDouble() || !o["treasuryAfter"].isDouble())missing("UNBOUND_PREDEPLOYMENT_WALLET",o);
            QJsonObject receipt;
            for(const auto &key:{"factory","enemy","type","price","buildHalfTurn","treasuryBefore","treasuryAfter","vacater","vacaterEnd"})
                receipt[key]=o[key];
            const auto born=QJsonArray{o["enemy"],o["factory"],built,o["type"]};
            const int departed=o["firstStepHalfTurn"].toInt(-1),impact=o["impactHalfTurn"].toInt(-1);
            if(departed<0)missing("PURCHASE_HAS_NO_DATED_FIRST_ACTION",o);
            else if(departed<first)receipt["firstStop"]=o["firstStop"];
            if(impact>=0 && impact<first)
                for(const auto &key:{"kind","end","impactHalfTurn","target","targetTile","targetDamage","exposure"})
                    receipt[key]=o[key];
            if(o["kind"]=="CAPTURE" && impact>=0 && impact<first)
                ownership.append(QJsonArray{"paid",o["end"],impact,o["enemy"]});
            const auto capture=o["paidCaptureProof"].toObject();
            dates(capture["stops"].toArray(),"paid",born,movements);
            dates(capture["shots"].toArray(),"paid",born,shots);
            dates(capture["captureInstallments"].toArray(),"paid",born,instalments);
            dates(o["otherPricedShots"].toArray(),"paidSupport",born,shots);
            dates(o["paidCaptureDefenseUses"].toArray(),"paidCounter",born,shots);
            if(departed>=0 && departed<first && capture.isEmpty() && o["factory"]!=o["end"])
            {
                // A normal production receipt identifies a canonical native
                // itinerary. Bind its ordered prefix, including movement costs.
                const auto id=o["factory"].toArray();
                const auto site=terrain.production?std::find_if(terrain.production->sites.begin(),terrain.production->sites.end(),
                    [&](const auto &s){return xy(s.tile)==id;}):std::vector<ProductionSite>::const_iterator{};
                if(!terrain.production || site==terrain.production->sites.end()){missing("EARLY_PAID_SITE_UNBOUND",o);continue;}
                const auto &menu=site->menu[int(o["enemy"].toBool())];
                const auto type=std::find_if(menu.begin(),menu.end(),[&](const auto &u){return u.type==o["type"].toString() &&
                    u.cost==o["price"].toInteger(-1);});
                if(type==menu.end() || !type->movementGrid){missing("EARLY_PAID_MOVEMENT_GRID_UNBOUND",o);continue;}
                const ReinforcementRoute *route=nullptr;
                for(const auto &candidate:type->routes)
                {
                    if(!charge(1))return result;
                    if(xy(candidate.firstStop)!=o["firstStop"].toArray() || xy(candidate.destination)!=o["end"].toArray())continue;
                    if(route){missing("AMBIGUOUS_EARLY_PAID_ROUTE",o);break;}
                    route=&candidate;
                }
                if(!route){missing("EARLY_PAID_ROUTE_UNBOUND",o);continue;}
                int tick=departed;auto last=site->tile;auto stop=route->overnightStops.rbegin();
                for(auto c=route->cells.rbegin();c!=route->cells.rend() && tick<first;++c)
                {
                    if(!charge(1))return result;
                    TilePoint next{*c%terrain.width,*c/terrain.width};
                    if(!type->movementGrid->onGrid(next.x,next.y) || manhattanDistance(last,next)!=1)
                        {missing("EARLY_PAID_ROUTE_NOT_CONTIGUOUS",o);break;}
                    const int cost=type->movementGrid->cost(next.x,next.y,entryDirection(last.x,last.y,next.x,next.y));
                    if(cost<0){missing("EARLY_PAID_FUEL_UNVERIFIED",o);break;}
                    movements.append(QJsonArray{"paidFuel",born,tick,xy(next),cost});
                    last=next;
                    if(stop!=route->overnightStops.rend() && *stop==*c){++stop;tick+=2;}
                }
            }
            buyers.append(receipt);
        }
        if(paid.enemyOpeningRepairs && first>1)
        {
            const auto &bill=*paid.enemyOpeningRepairs;QJsonArray rows;
            for(const auto &r:bill.rows)rows.append(QJsonArray{r.actor,r.before,r.after,static_cast<qint64>(r.debit)});
            buyers.append(QJsonArray{"openingRepairs",static_cast<qint64>(bill.before),static_cast<qint64>(bill.after),rows});
        }
        for(const auto &g:paid.fundedGuns)if(g.buildTick<first)
        {
            if(!g.unit){missing("UNKNOWN_PREDEPLOYMENT_PURCHASE_TYPE");continue;}
            QJsonArray body{g.side,xy(g.factory),g.buildTick,g.unit->type,g.unit->startingHp,g.unit->initialFuel,
                QJsonArray{g.unit->captureLoad[0],g.unit->captureLoad[1]}};
            if(g.buildTick+2<first)body.append(QJsonArray{xy(g.destination),g.fuelAtPost});
            if(g.observedService && g.observedService->shot<first)
            {
                const auto &r=*g.observedService;
                body.append(QJsonArray{r.shot,r.hpAfterFirst,r.hpAfterReply,QJsonArray{r.ammoAfterFirst[0],r.ammoAfterFirst[1]},
                    QJsonArray{r.ammoAfterReply[0],r.ammoAfterReply[1]},r.returned,r.fuelAfterReturn});
            }
            buyers.append(body);
        }
        if(!save("originalAndPaidMoves",movements) || !save("exchanges",shots) || !save("captureProgress",instalments) ||
           !save("titlesAndRates",ownership) || !save("materialAndPayments",material) || !save("purchases",buyers))return result;
        // A late entry may require leaving an earlier post. Without a dated
        // departure bound, an unfinished hearing cannot establish the prefix.
        for(const auto &v:race.townCoverage)
        {
            if(!charge(1))return result;
            const auto row=v.toObject();if(first>2 && !row["complete"].toBool())missing("UNVERIFIED_TOWN_DEPARTURE_BEFORE_POST",row);
        }
        if(race.productionLimited)missing("EARLIER_PURCHASE_MENU_MAY_CHANGE");
        if(!race.observedStockComplete)missing("EARLY_STOCK_REPLY_OR_POST_UNVERIFIED");
        if(!race.paidCaptureUnsupported.isEmpty() || !race.paidCaptureWorkGap.isEmpty())missing("EARLIER_PAID_CAPTURE_DEPARTURE_UNVERIFIED");
        if(!race.townConflicts.isEmpty() || !race.unconfirmedFactoryReleases.isEmpty())missing("EARLIER_OCCUPANCY_OR_RELEASE_UNVERIFIED");
        const auto raw=QJsonDocument(QJsonArray{first,result.components}).toJson(QJsonDocument::Compact);
        result.digest=QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex());
        return result;
    }
}
