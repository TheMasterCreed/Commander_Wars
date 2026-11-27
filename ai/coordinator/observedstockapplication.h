#pragma once
#include "ai/coordinator/fundedinterceptadmission.h"
#include "ai/coordinator/observedstockengagement.h"
#include "ai/coordinator/observedstockscreen.h"

namespace Coordinator
{
    struct ObservedStockBid
    {
        int capper{NO_UNIT},funded{NO_UNIT};
        FundedInterceptPath route;
        QJsonObject original;
        ObservedStockLine answer;
        MilliFunds beforeSortie{},continueDelta{},attackDelta{};
        QJsonArray diversions;
    };

    // The accepted source title, resources and jobs are validated on the
    // current calendar before any replacement is published. A changed owner
    // can never fund its own now-lost factory or duplicate the original title.
    inline bool applyObservedStockBid(const CombatTerrain &terrain,int cutoff,const ObservedStockBid &bid,
        StockCaptureSettlement &settled,ProductionProjection &production,std::vector<ProductionProperty> &properties,
        QJsonArray &claims,QJsonArray &proofs,QJsonArray &orders,std::vector<MilliFunds> &cash,
        MilliFunds &ownership,MilliFunds &capitalLoss,StockRouteLowerBounds *lower=nullptr,std::int64_t *work=nullptr)
    {
        const auto &a=bid.answer;const auto &trip=bid.route;
        const int n=static_cast<int>(terrain.units.size());
        if(!settled.townTraffic || bid.capper<0 || bid.capper>=n || bid.funded<0 ||
            bid.funded>=static_cast<int>(production.fundedGuns.size()) || trip.observation>=trip.departure ||
            trip.shot<=trip.departure || trip.back<=trip.shot || trip.steps.empty() ||
            a.completion<0 || a.completion>cutoff || a.capperHp<=0 || a.reserveHp<=0)return false;
        auto &gun=production.fundedGuns[bid.funded];
        auto released=settled.townAfterCapture.find(bid.capper);
        if(!gun.unit || !gun.unit->movementGrid || released==settled.townAfterCapture.end() ||
           gun.observedService || trip.steps.back().tile!=gun.destination)return false;
        const auto &cap=terrain.units[bid.capper];const auto dest=released->second.tile;
        const auto pt=[](TilePoint t){return QJsonArray{t.x,t.y};};
        if(bid.original["unit"].toInt(NO_UNIT)!=cap.engineUnitId || bid.original["tile"]!=pt(dest) ||
           bid.original["status"]!="WITNESSED" || !bid.original["source"].toString().isEmpty() ||
           !bid.original["shots"].toArray().isEmpty())return false;
        const int old=bid.original["actualCompletionTick"].toInt(-1);
        if(old<=trip.shot || a.completion<=old || released->second.readyTick!=old+2)return false;
        auto market=std::find_if(properties.begin(),properties.end(),[&](const auto &p){return p.tile==dest;});
        if(market==properties.end())return false;
        const auto owner=cap.enemy?OwnerSign::Enemy:OwnerSign::Ours;
        const int ownIndex=static_cast<int>(std::count_if(market->ownershipChanges.begin(),market->ownershipChanges.end(),
            [&](const auto &change){return change.completionTick==old && change.owner==owner;}));
        if(ownIndex!=1 || std::any_of(market->ownershipChanges.begin(),market->ownershipChanges.end(),
            [&](const auto &change){return change.completionTick>old;}))return false;
        int claim=-1,proof=-1,receipt=-1;
        for(int i=0;i<claims.size();++i){
            const auto o=claims[i].toObject();
            if(o["unit"].toInt(NO_UNIT)!=cap.engineUnitId || o["tile"]!=pt(dest) ||
               2*o["completionDayOffset"].toInt(-1)+int(cap.enemy)!=old)continue;
            if(claim>=0)return false;claim=i;
        }
        for(int i=0;i<proofs.size();++i)if(proofs[i].toObject()==bid.original){if(proof>=0)return false;proof=i;}
        auto revised=a.price.revisedPurchases;
        if(revised.size()!=orders.size() || a.price.change.flow.size()!=cash.size())return false;
        for(int i=0;i<revised.size();++i){
            const auto o=revised[i].toObject(),previous=orders[i].toObject();
            if(o["factory"]!=previous["factory"] || o["type"]!=previous["type"] ||
               o["price"]!=previous["price"] || o["buildHalfTurn"]!=previous["buildHalfTurn"] ||
               o["enemy"]!=previous["enemy"])return false;
            const int bought=o["buildHalfTurn"].toInt();
            if(o["factory"]==pt(dest) && bought>old && bought<=a.completion && o["enemy"].toBool()==cap.enemy)return false;
            if(o["factory"]==pt(gun.factory) && bought==gun.buildTick && int(o["enemy"].toBool())==gun.side)
            {
                if(o["kind"]!="DEPLOY" || o["type"]!=gun.unit->type || receipt>=0)return false;
                receipt=i;
            }
        }
        if(claim<0 || proof<0 || receipt<0)return false;
        auto stock=ownership,material=capitalLoss;
        if(!RelativeStockDetail::checkedAdd(stock,a.price.change.ownership) ||
            !RelativeStockDetail::checkedAdd(material,a.price.change.capitalLoss)) return false;
        auto calendar=*settled.townTraffic;
        const int paid=n+bid.funded;
        calendar.removeUsesWithin(paid,trip.departure,trip.back);
        const auto &grid=*gun.unit->movementGrid;
        const auto sq=[&](TilePoint p){return p.y*terrain.width+p.x;};
        auto at=gun.destination;int fuel=gun.fuelAtPost,previous=trip.departure-2;
        for(const auto &step:trip.steps)
        {
            if(step.tick!=previous+2 || step.tick>trip.back)return false;
            for(const auto cell:step.cells){
                const TilePoint next{cell%terrain.width,cell/terrain.width};
                if(!grid.onGrid(next.x,next.y) || std::abs(next.x-at.x)+std::abs(next.y-at.y)!=1 ||
                   (!gun.unit->ignoresUnitCollision && !calendar.open(paid,gun.side,cell,step.tick,false)))return false;
                const int cost=grid.cost(next.x,next.y,entryDirection(at.x,at.y,next.x,next.y));
                if(cost<0 || (fuel>=0 && cost>fuel))return false;
                if(fuel>=0)fuel-=cost;
                calendar.reserve(cell,step.tick,step.tick,gun.side,paid,false);at=next;
            }
            if(at!=step.tile || !calendar.open(paid,gun.side,sq(at),step.tick,true) ||
               !calendar.open(paid,gun.side,sq(at),step.tick+1,true))return false;
            calendar.reserve(sq(at),step.tick,step.tick+1,gun.side,paid,true);previous=step.tick;
        }
        if(previous!=trip.back || at!=gun.destination || (gun.fuelAtPost>=0 && gun.fuelAtPost-fuel!=trip.fuel))return false;
        if(!settled.townProtectedPosts.empty() || released->second.protectsPost)
        {
            if(!lower || !work || (released->second.protectsPost && settled.townProtectedPosts.empty()))return false;
            const std::array<ChangedScreenActor,2> subjects{{{bid.capper,bid.original["entryTick"].toInt(trip.shot-1)},
                {n+bid.funded,trip.departure}}};
            const auto screens=reviewObservedScreens(terrain,settled,subjects,&calendar,*lower,*work);
            if(!screens.complete || !screens.preserved)return false;
        }
        auto updated=bid.original;
        auto installments=updated["captureInstallments"].toArray();QJsonArray prefix;
        for(const auto &v:installments)if(v.toObject()["tick"].toInt()<trip.shot)prefix.append(v);
        for(const auto &v:a.installments)prefix.append(v);
        if(prefix.isEmpty() || prefix.last().toObject()["tick"]!=a.completion)return false;
        QJsonArray walk;
        for(const auto &step:trip.steps)walk.append(QJsonObject{{"tick",step.tick},{"end",pt(step.tile)}});
        QJsonObject report{{"fundedAt",pt(gun.factory)},{"fromPost",pt(gun.destination)},
            {"observationTick",trip.observation},{"observedTile",pt(trip.observed)},{"departureTick",trip.departure},
            {"shotTick",trip.shot},{"from",pt(trip.firing)},{"returnTick",trip.back},{"datedStops",walk},
            {"originalFinish",old},{"revisedFinish",a.completion},{"stockAfterFirst",a.capperAfterFirst},
            {"paidAfterFirst",a.reserveAfterFirst},{"stockAfter",a.capperHp},{"paidAfter",a.reserveHp},
            {"stockAmmoAfter",QJsonArray{a.capperAmmo[0],a.capperAmmo[1]}},
            {"paidAmmoAfter",QJsonArray{a.reserveAmmo[0],a.reserveAmmo[1]}},{"paidFuelAfter",fuel},
            {"continueChange",static_cast<qint64>(bid.continueDelta)},{"stationaryAttackChange",static_cast<qint64>(bid.attackDelta)},
            {"abandonChange",static_cast<qint64>(bid.beforeSortie)},{"competingCaptures",bid.diversions},
            {"choice",a.action==ObservedCapperAction::Capture?"CONTINUE_CAPTURE":"ATTACK_RESERVE"}};
        updated["actualCompletionTick"]=a.completion;updated["completionTick"]=a.completion;updated["hpAfter"]=a.capperHp;
        updated["captureInstallments"]=prefix;updated["observedFundedResponse"]=report;
        auto title=claims[claim].toObject();title["completionDayOffset"]=a.completion/2;
        auto signedIncome=title["signedIncome"].toInteger();
        if(!RelativeStockDetail::checkedAdd(signedIncome,a.price.change.ownership))return false;
        title["signedIncome"]=signedIncome;title["observedFundedResponse"]=report;
        auto buy=revised[receipt].toObject();
        buy["observedStockSupport"]=report;buy["reserveReadyTick"]=trip.back+2;revised[receipt]=buy;
        for(auto &change:market->ownershipChanges)if(change.completionTick==old && change.owner==owner)change.completionTick=a.completion;
        if(market->capturer==bid.capper && market->existingCompletion>=0 &&
           2*market->existingCompletion+int(market->existingEnemy)==old)
            market->existingCompletion=a.completion/2;
        released->second.readyTick=a.completion+2;released->second.hp=a.capperHp;released->second.ammo=a.capperAmmo;
        gun.observedService=PaidObservedService{trip.departure,trip.shot,trip.back,trip.back+2,
            a.reserveAfterFirst,a.reserveHp,fuel,a.reserveAmmoAfterFirst,a.reserveAmmo};
        settled.townTraffic=std::move(calendar);
        claims[claim]=title;proofs[proof]=updated;orders=std::move(revised);cash=a.price.change.flow;
        ownership=stock;capitalLoss=material;return true;
    }
}
