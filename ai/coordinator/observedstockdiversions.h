#pragma once
#include "ai/coordinator/fundedstockfork.h"

namespace Coordinator
{
    struct StockDiversionBound
    {
        bool complete{true},terminalPossible{};
        MilliFunds greatestIncome{};
        QJsonArray firstClaims;
    };

    // Bound the capper's different property choices from its actual fork, using
    // the original dated bodies and remaining fuel. Ignoring hostile damage and
    // allowing full capture strength favors the diverging actor. If the bound
    // can reverse the defender's decision the line stays unresolved.
    template<class Timing>
    StockDiversionBound boundObservedDiversions(const CombatTerrain &terrain,const KnownUnit &unit,int actor,
        const FundedCaptureFork &fork,const StockCaptureTraffic &traffic,std::span<const ProductionProperty> properties,
        TilePoint original,int oldEntry,int oldFinish,TilePoint home,TilePoint away,int threshold,int cutoff,StockRouteLowerBounds &lower,
        std::span<const MilliFunds> baseline,Timing timing,std::int64_t &budget)
    {
        StockDiversionBound result;
        const auto &pawn=terrain.units[actor];
        const int profile=pawn.grid<0?pawn.travelGrid:pawn.grid;
        if(profile<0 || fork.hp<=0 || pawn.movement<=0 || threshold<=0 || !fork.calendar ||
           fork.calendar!=&traffic || fork.revision!=traffic.generation()){result.complete=false;return result;}
        const int side=int(pawn.enemy);
        const int rate=unit.captureRate+std::max(0,10-unit.hpRounded);
        if(rate<=0)return result;
        const int installments=widenedCaptureTurns(0,rate,threshold);
        const auto &grid=terrain.grids[profile];auto frozen=traffic;frozen.removeUsesWithin(actor,fork.tick,cutoff);
        const auto before=timing(baseline);
        if(!before){result.complete=false;return result;}
        const auto cell=[&](TilePoint p){return p.y*terrain.width+p.x;};
        std::set<int> acquired;
        std::vector<ProductionProperty> alternateProperties(properties.begin(),properties.end());
        for(auto &p:alternateProperties)if(p.tile==original)
        {
            const auto expected=side?OwnerSign::Enemy:OwnerSign::Ours;
            std::erase_if(p.ownershipChanges,[&](const auto &e){return e.owner==expected && e.completionTick==oldFinish;});
            if(p.existingCompletion>=0 && 2*p.existingCompletion+int(p.existingEnemy)==oldFinish)
                p.existingCompletion=-1;
        }
        const auto explore=[&](auto &&self,TilePoint from,int ready,int fuel,const std::vector<MilliFunds> &flow,
                               MilliFunds earnings,bool first)->void{
            if(!result.complete || result.terminalPossible || ready>cutoff)return;
            for(const auto &p:alternateProperties)
            {
                if(acquired.contains(cell(p.tile)))continue;
                if(budget<=0){result.complete=false;return;}--budget;
                const bool terminal=p.tile==(side?home:away);
                const auto own=side?OwnerSign::Enemy:OwnerSign::Ours;
                if(productionOwnerAt(p,ready+1)==own &&
                   std::none_of(p.ownershipChanges.begin(),p.ownershipChanges.end(),[&](const auto &o){
                       return o.completionTick>=ready && o.owner!=own;}))continue;
                const int latest=cutoff-2*(installments-1);
                if(ready>latest)continue;
                // Waiting out the visible reserve and then coming back is a genuine rival.
                const int earliest=first && p.tile==original?std::max(ready,oldEntry+2):ready;
                if(earliest>latest)continue;
                // An earlier detour need not dominate a later route with more fuel.
                // Check every attainable entry date and relax finite fuel in this upper
                // bound only; no trip or transfer is bought on the strength of that relaxation.
                for(int date=earliest;date<=latest;date+=2)
                {
                    const auto route=stockCaptureRoute(grid,frozen,actor,side,from,p.tile,pawn.movement,-1,
                        ready,date,date,installments,budget,INVALID_TILE,&lower,{},pawn.ignoresCollision);
                    if(!route){result.complete=false;return;}
                    if(route->entryTick<0)continue;
                    const int close=route->entryTick+2*(installments-1);
                    if(productionOwnerAt(p,route->entryTick+1)==own || productionOwnerAt(p,close+1)==own)continue;
                    if(terminal){result.terminalPossible=true;return;}
                    auto next=flow;auto total=earnings;
                    for(int tick=close+1;tick<=cutoff;++tick)
                    {
                        const auto owner=productionOwnerAt(p,tick);
                        const auto who=tick%2?OwnerSign::Enemy:OwnerSign::Ours;
                        const auto money=who==OwnerSign::Ours?p.column.income.oursPerTurn:p.column.income.enemyPerTurn;
                        const auto gain=(int(own==who)-int(owner==who))*(who==OwnerSign::Ours?money:-money);
                        if(!RelativeStockDetail::checkedAdd(next[(tick-1)/2],gain) ||
                           !RelativeStockDetail::checkedAdd(total,gain)){result.complete=false;return;}
                    }
                    const auto time=timing(next);if(!time){result.complete=false;return;}
                    auto value=total;
                    if(!RelativeStockDetail::checkedAdd(value,*time)||!RelativeStockDetail::checkedSubtract(value,*before))
                    {result.complete=false;return;}
                    result.greatestIncome=std::max(result.greatestIncome,side?-value:value);
                    if(first)result.firstClaims.append(QJsonObject{{"tile",QJsonArray{p.tile.x,p.tile.y}},{"earliest",close},
                        {"unopposedIncomeUpper",static_cast<qint64>(side?-value:value)}});
                    acquired.insert(cell(p.tile));
                    self(self,p.tile,close+2,-1,next,total,false);
                    acquired.erase(cell(p.tile));
                }
            }
        };
        const std::vector<MilliFunds> initial(baseline.begin(),baseline.end());
        explore(explore,fork.tile,fork.tick,fork.fuel,initial,0,true);
        return result;
    }
}
