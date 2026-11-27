#pragma once
#include "ai/coordinator/observedstockapplication.h"
#include "ai/coordinator/observedstockdiversions.h"
#include "ai/coordinator/observedstockfireoptions.h"
#include "ai/coordinator/observedstockcontact.h"
#include "ai/coordinator/fundedinterceptsafety.h"

namespace Coordinator
{
    struct ObservedStockMarket
    {
        std::optional<ObservedStockBid> selected;
        QJsonArray evidence;
        bool complete{true};
    };

    template<class Timing>
    ObservedStockMarket priceObservedStockMarket(const CombatTerrain &t,std::span<const KnownUnit> units,
        std::span<const double> hp,std::span<const std::array<int,2>> ammo,std::span<const int> lastShot,
        std::span<const int> lastHurt,const StockCaptureSettlement &settled,
        const ProductionProjection &production,const QJsonArray &orders,std::span<const ProductionProperty> properties,
        const QJsonArray &proofs,std::span<const MilliFunds> cash,TilePoint home,TilePoint away,int last,
        StockRouteLowerBounds &lower,Timing timing,std::int64_t &budget)
    {
        ObservedStockMarket result;
        if(!settled.townTraffic || units.empty() || production.fundedGuns.empty())return result;
        const auto &calendar=*settled.townTraffic;
        const int n=static_cast<int>(units.size());
        const auto pt=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const auto coord=[](QJsonValue v){const auto a=v.toArray();return TilePoint{a.at(0).toInt(-1),a.at(1).toInt(-1)};};
        const bool requiresPaidCalendar=std::any_of(orders.begin(),orders.end(),[](const auto &v){
            const auto o=v.toObject();
            return o["paidCaptureProof"].toObject()["status"]=="WITNESSED" || !o["paidCaptureDefenseUses"].toArray().isEmpty();
        });
        MilliFunds bestGain{};bool competingBookings=false;
        for(const auto &entry:proofs)
        {
            const auto proof=entry.toObject();
            if(proof["status"]!="WITNESSED" || !proof["source"].toString().isEmpty() ||
               !proof["shots"].toArray().isEmpty() || proof["tile"].toArray().size()!=2)continue;
            const int original=proof["actualCompletionTick"].toInt(-1),first=proof["entryTick"].toInt(-1);
            const auto town=coord(proof["tile"]);
            if(first<0 || original<=first || original>=last || town==home || town==away)continue;
            const auto who=std::find_if(t.units.begin(),t.units.end(),[&](const auto &stock){return stock.engineUnitId==proof["unit"].toInt(NO_UNIT);});
            if(who==t.units.end() || who->grid<0 || !who->capper)continue;
            const int actor=static_cast<int>(who-t.units.begin()),side=int(who->enemy);
            const auto itinerary=stockEncounterPath(proof,t.width,t.height);
            if(!itinerary) continue;
            const auto origin=coord(proof["from"]);
            const int cutoff=proof["captureThreshold"].toInt();
            auto installments=proof["captureInstallments"].toArray();
            if(installments.isEmpty())continue;
            for(int k=0;k<static_cast<int>(production.fundedGuns.size());++k)
            {
                const auto &g=production.fundedGuns[k];
                if(!g.unit || !g.unit->mobileFire || g.observedService || g.side==side || g.targetUnit!=NO_UNIT ||
                   first+1<g.readyTick || g.unit->minRange>1 || g.unit->maxRange<1 ||
                   manhattanDistance(g.destination,town)>2*g.unit->movement+1 ||
                   actor>=static_cast<int>(g.unit->captureCounter.size()) ||
                   usableOpeningWeapon(g.unit->captureCounter[actor],g.unit->captureLoad)<0)continue;
                if(budget<static_cast<std::int64_t>(1+proofs.size()+orders.size())){result.complete=false;return result;}
                budget-=1+proofs.size()+orders.size();
                QString reason;
                const auto window=fundedResponseWindow(t,production,orders,settled,properties,actor,k,proof,last,reason);
                if(!window)
                {
                    if(result.evidence.size()<24)result.evidence.append(QJsonObject{{"status",reason},
                        {"unit",who->engineUnitId},{"property",pt(town)},{"paidAt",pt(g.factory)},{"paidPost",pt(g.destination)}});
                    continue;
                }
                if(requiresPaidCalendar)
                {
                    result.evidence.append(QJsonObject{{"status","REQUIRES_FINAL_FUNDED_CAPTURE_CALENDAR"},
                        {"unit",who->engineUnitId},{"property",pt(town)},{"paidAt",pt(g.factory)}});
                    result.complete=false;continue;
                }
                const int shot=first+1,forkTick=shot-1;
                const auto active=[&](int lo,int hi){return lo>=window->firstRedeploy && hi<=last;};
                const auto safe=[&](TilePoint at,int tick){
                    return quietFundedStop(t,units,hp,ammo,production.fundedGuns,calendar,k,actor,*itinerary,
                        origin,forkTick,at,tick,budget,shot+1);
                };
                QJsonObject row{{"unit",who->engineUnitId},{"property",pt(town)},{"paidAt",pt(g.factory)},
                    {"paidPost",pt(g.destination)},{"originalFinish",original},{"bookedPurchases",orders.size()}};
                const std::array<ChangedScreenActor,2> affected{{{actor,forkTick},{n+k,window->firstRedeploy}}};
                const auto offeredScreen=budget/4;auto screenWork=offeredScreen;
                auto screen=reviewObservedScreens(t,settled,affected,nullptr,lower,screenWork);
                budget-=offeredScreen-screenWork;
                row["cumulativeScreenChecks"]=screen.checks;
                row["cumulativeScreensPreserved"]=screen.complete && screen.preserved;
                auto route=stageFundedIntercept(g,n+k,actor,origin,*itinerary,town,shot,last,calendar,lower,
                                                active,safe,budget,window->firstRedeploy);
                if(!route){row["status"]="REDEPLOYMENT_WORK_LIMIT";result.evidence.append(row);result.complete=false;return result;}
                if(route->firing==INVALID_TILE){row["status"]="NO_CERTIFIED_RETURNING_ROUTE";result.evidence.append(row);continue;}
                row["stageTick"]=route->departure;row["shotTick"]=route->shot;row["shotFrom"]=pt(route->firing);
                row["returnTick"]=route->back;row["observedAt"]=pt(route->observed);row["observedTick"]=route->observation;
                const FundedStockWitness facts{calendar,t.grids[who->grid],window->initialHp,window->initialFuel,
                    window->initialCarry,window->initialAmmo};
                auto fork=replayCaptureFork(proof,*itinerary,forkTick,facts);
                if(!fork){row["status"]="UNRESOLVED_FORK_RESOURCES";result.evidence.append(row);continue;}
                const auto &cap=units[actor];
                const double paidHP=g.unit->startingHp-static_cast<double>((std::max<MilliFunds>(0,g.exposure)*10+g.unit->cost-1)/g.unit->cost);
                const auto duel=[&](bool a,double ah,double vh,const auto &ar,const auto &vr){
                    return observedReserveVolley(t,actor,town,g,route->firing,a,ah,vh,ar,vr,&budget);
                };
                int carry=-1;
                for(const auto &v:installments)if(v.toObject()["tick"].toInt()<shot)carry=v.toObject()["carryAfter"].toInt();
                const auto price=[&](ObservedCapperAction action){
                    return quoteObservedStockLine(action,g.side,shot,original,last,window->property,carry,cutoff,cap,
                        window->initialHp,window->initialAmmo,who->replacementCost,paidHP,g.unit->captureLoad,g.unit->cost,
                        cash,orders,duel,timing);
                };
                const auto continues=price(ObservedCapperAction::Capture);
                if(!continues){row["status"]="UNPRICED_FUNDED_CONTINUATION";result.evidence.append(row);result.complete=false;continue;}
                const auto shoots=price(ObservedCapperAction::AttackReserve);
                const bool hasReply=actor<static_cast<int>(g.unit->againstCapper.size()) && continues->capperHp>0 &&
                    continues->reserveHp>0 && usableOpeningWeapon(g.unit->againstCapper[actor],continues->capperAmmo)>=0;
                if(hasReply && !shoots){row["status"]="UNPRICED_STATIONARY_ATTACK";result.evidence.append(row);result.complete=false;continue;}
                const auto declines=replaceProvedCapture(side,original,-1,window->property,cash,orders,{},last,timing);
                if(!declines){row["status"]="UNPRICED_ABANDONED_WALLET";result.evidence.append(row);result.complete=false;continue;}
                row["continueDelta"]=static_cast<qint64>(continues->price.change.score);
                row["continueFinish"]=continues->completion;row["paidHpAfter"]=continues->reserveAfterFirst;
                if(shoots){row["attackDelta"]=static_cast<qint64>(shoots->price.change.score);row["attackFinish"]=shoots->completion;}
                row["abandonDelta"]=static_cast<qint64>(declines->change.score);
                const auto defend=[&](MilliFunds v){return g.side?-v:v;};
                const auto &chosen=shoots && defend(shoots->price.change.score)<defend(continues->price.change.score)?*shoots:*continues;
                const auto gain=defend(chosen.price.change.score);
                if(gain<=0){row["status"]="PREFER_EXISTING_ORDERS";result.evidence.append(row);continue;}
                // Opening a reserve is voluntary. If the target changes course before the
                // shot, it never suffers the hit and the reserve can withdraw from staging.
                const auto &stage=route->steps.front();
                int used=0;auto prior=g.destination;
                for(int c:stage.cells){const TilePoint at{c%t.width,c/t.width};used+=g.unit->movementGrid->cost(at.x,at.y,
                    entryDirection(prior.x,prior.y,at.x,at.y));prior=at;}
                auto retreat=stockCaptureRoute(*g.unit->movementGrid,calendar,n+k,g.side,stage.tile,g.destination,
                    g.unit->movement,g.fuelAtPost<0?-1:g.fuelAtPost-used,shot,shot,shot,1,budget,INVALID_TILE,&lower,{},g.unit->ignoresUnitCollision);
                if(!retreat || retreat->entryTick!=shot){row["status"]="UNRESOLVED_RETURN_ON_REFUSAL";result.evidence.append(row);result.complete=false;continue;}
                const auto otherFire=screenStockDiversionFire(t,units,hp,production.fundedGuns,calendar,*fork,
                    actor,k,*route,*retreat,last,budget);
                row["divertedFireOpportunities"]=otherFire.alternatives;
                if(!otherFire.immediateStockContacts.empty())
                {
                    const auto replied=observedContactAlternatives(t,units,hp,ammo,lastShot,lastHurt,settled,production,calendar,
                        *fork,actor,k,*route,window->property,original,cutoff,last,orders,cash,lower,timing,
                        otherFire.immediateStockContacts,budget,screen.complete && screen.preserved);
                    if(replied)row["counterAtObservedStockShot"]=*replied;
                    else {row["status"]="OBSERVED_ORDINARY_ATTACK_WORK_LIMIT";result.evidence.append(row);result.complete=false;continue;}
                }
                if(!screen.preserved || !screen.complete)
                {
                    // Even when the stronger removal bound fails, check this
                    // supported position before claiming a stock title has reopened.
                    row["specificCaptureScreenChecks"]=reviewPreservedCapPost(t,settled,affected,*route,*continues,
                        actor,n+k,g.side,last,lower,budget);
                    row["status"]=screen.complete?"ALTERNATE_SCREEN_REQUIRES_REPRICING":"SCREEN_RECHECK_UNRESOLVED";
                    result.evidence.append(row);result.complete=false;continue;
                }
                if(otherFire.otherTargetPossible || !otherFire.complete)
                {
                    row["status"]=otherFire.otherTargetPossible?"COMPETING_ORDINARY_ATTACK_REQUIRES_WITNESS":
                        "DIVERTED_GUN_OR_TARGET_STATE_UNRESOLVED";
                    result.evidence.append(row);result.complete=false;continue;
                }
                auto rivals=boundObservedDiversions(t,cap,actor,*fork,calendar,properties,town,first,original,home,away,
                    cutoff,last,lower,cash,timing,budget);
                row["alternativeFirstTransfers"]=rivals.firstClaims;
                row["divertedIncomeUpper"]=static_cast<qint64>(rivals.greatestIncome);
                // The most costly remaining pursuit would destroy the reserve on its
                // first possible attack. A future conditional exchange is not cash.
                MilliFunds pursuitBound=0;
                if(otherFire.earliestReserve>=0)
                {
                    auto adverse=std::vector<MilliFunds>(cash.begin(),cash.end());
                    const auto lost=g.unit->cost;
                    auto &round=adverse.at((otherFire.earliestReserve-1)/2);
                    auto adjusted=RelativeStockDetail::checkedAdd(round,g.side?lost:-lost)?timing(adverse):std::nullopt;
                    const auto initial=timing(cash);
                    if(!adjusted || !initial || !RelativeStockDetail::checkedSubtract(*adjusted,*initial))
                    {row["status"]="UNRESOLVED_PURSUIT_RANGE";result.evidence.append(row);result.complete=false;continue;}
                    pursuitBound=lost;
                    if(!RelativeStockDetail::checkedSubtract(pursuitBound,defend(*adjusted)))
                    {row["status"]="UNRESOLVED_PURSUIT_RANGE";result.evidence.append(row);result.complete=false;continue;}
                }
                row["unearnedPursuitUpper"]=static_cast<qint64>(pursuitBound);
                row["earliestPotentialPursuit"]=otherFire.earliestReserve;
                if(!rivals.complete || rivals.terminalPossible ||
                   defend(declines->change.score)-rivals.greatestIncome-pursuitBound<gain)
                {
                    row["status"]=!rivals.complete?"DIVERSION_WORK_LIMIT":rivals.terminalPossible?
                        "UNRESOLVED_ALTERNATE_HQ":"COMPETING_CAPTURE_REQUIRES_WITNESS";
                    result.evidence.append(row);result.complete=false;continue;
                }
                if(chosen.reserveHp<=0 || chosen.completion<0){row["status"]="UNRESOLVED_CASUALTY_OR_OPEN_TITLE";result.evidence.append(row);result.complete=false;continue;}
                if(result.selected && result.selected->capper!=actor)competingBookings=true;
                row["status"]="CERTIFIED_OBSERVED_SUPPORT";result.evidence.append(row);
                if(gain<=bestGain)continue;
                result.selected=ObservedStockBid{actor,k,*route,proof,chosen,declines->change.score,
                    continues->price.change.score,shoots?shoots->price.change.score:0,rivals.firstClaims};
                bestGain=gain;
            }
        }
        if(competingBookings){result.selected.reset();result.complete=false;
            result.evidence.append(QJsonObject{{"status","SIMULTANEOUS_STOCK_SUPPORT_UNRESOLVED"}});}
        return result;
    }
}
