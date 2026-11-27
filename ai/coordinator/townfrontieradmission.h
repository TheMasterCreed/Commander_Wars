#pragma once

#include "ai/coordinator/townassaultledger.h"
#include "ai/coordinator/townopportunitymenu.h"

namespace Coordinator
{
    // A single additional transfer on an unchanged title stream. Earlier
    // completion is deliberately granted; other actions, damage, funds and
    // production are not certified by this number.
    template<class Timing>
    QJsonValue frontierTitleOnly(const PropertyStockColumn &column,int side,int finish,
        std::span<const MilliFunds> cash,int last,Timing timing)
    {
        if(finish<0 || finish>last)return {};
        TownAssaultLine title;title.side=side;title.town=column.tile;title.finishTick=finish;
        const auto ledger=townAssaultLedger(title,column,cash,last,timing);
        return ledger?QJsonValue(static_cast<qint64>(ledger->score)):QJsonValue{};
    }

    template<class Timing>
    QJsonObject hearTownProperty(const TownAssaultBoard &b,const TownOpportunity &slot,const PropertyStockColumn &c,
        const std::set<std::array<int,5>> &covered,StockRouteLowerBounds &bound,
        std::span<const MilliFunds> cash,int last,Timing timing,std::int64_t &work)
    {
        const auto before=work;
        QJsonArray witnesses,alreadyCovered;bool limited=false;int calls=0;
        // Forward every existing answer for this tile, including dates beyond
        // our first admission. The peer may discover one of those dates first.
        for(const auto &key:covered)
        {
            if(work<1)return {{"status","EXISTING_SCOPE_WORK_LIMITED"},{"workLimited",true}};
            --work;
            if(key[0]==slot.tile.x && key[1]==slot.tile.y && key[2]==slot.attacker)
                alreadyCovered.append(QJsonArray{key[3],key[4]});
        }
        int start=std::max(2+slot.attacker,slot.firstPossible);start+=int(start%2!=slot.attacker);
        std::vector<int> candidates;
        for(int i=0;i<static_cast<int>(b.units.size());++i)
        {
            if(work<1){limited=true;break;}--work;
            if(b.terrain.units[i].present && int(b.terrain.units[i].enemy)==slot.attacker && b.hp[i]>0 &&
               b.units[i].canCapture && b.rights[i].readyTick>=0 && b.rights[i].lastFreeTick>=start)candidates.push_back(i);
        }
        for(int tick=start;tick<=b.cutoff && witnesses.isEmpty() && work>0;tick+=2)
        {
            if(covered.contains({slot.tile.x,slot.tile.y,slot.attacker,NO_UNIT,tick}))
                continue;
            for(std::size_t k=0;k<candidates.size();++k)
            {
                if(work<=0){limited=true;break;}--work;
                const int i=candidates[k];const auto &u=b.units[i];const auto &r=b.rights[i];
                const int id=b.terrain.units[i].engineUnitId;
                if(covered.contains({slot.tile.x,slot.tile.y,slot.attacker,id,tick}))
                    continue;
                if(r.readyTick>tick || r.lastFreeTick<tick || !b.traffic.open(i,slot.attacker,
                    TownAssaultDetail::cell(b.terrain,slot.tile),tick,true))continue;
                const int rate=replyCaptureRate(u.captureRate,u.hpRounded-ceilHpSteps(b.hp[i]),false);
                if(rate<=0)continue;
                const auto finish=std::int64_t(tick)+2*(widenedCaptureTurns(0,rate,b.threshold)-1);
                // A stationary early actor may still own carry from a prior
                // activation. The retained motion board does not authenticate
                // its survival through every intermediate capture action.
                const bool priorCarryPossible=b.positions[i]==slot.tile && TilePoint{u.x,u.y}==slot.tile && u.capturePoints>0;
                const bool freshFits=finish<=r.lastFreeTick && finish<=b.cutoff;
                if(!freshFits && !priorCarryPossible)continue;
                const auto offered=work/static_cast<std::int64_t>(candidates.size()-k);
                if(offered<=0){limited=true;continue;}
                auto local=offered;++calls;
                auto route=TownAssaultDetail::stockMove(b,b.traffic,i,slot.tile,tick,false,bound,local);
                work-=offered-local;
                if(!route){limited=true;continue;}
                if(route->destination!=slot.tile || route->path.steps.empty() || route->path.entryTick!=tick)continue;
                const auto extra=std::int64_t(4*last+2*route->path.steps.size()+8);
                if(work<extra){limited=true;continue;}work-=extra;
                QJsonArray stops;
                for(const auto &s:route->path.steps)stops.append(QJsonArray{s.tick,s.tile.x,s.tile.y});
                witnesses.append(QJsonObject{{"capper",b.terrain.units[i].engineUnitId},{"entryTick",tick},
                    {"tile",QJsonArray{slot.tile.x,slot.tile.y}},{"attackingSide",slot.attacker},{"stops",stops},
                    {"fuelSpent",route->path.fuel},{"remainingFuel",r.fuel<0?-1:r.fuel-route->path.fuel},{"finishIfStartingFresh",freshFits?QJsonValue(static_cast<int>(finish)):QJsonValue(QJsonValue::Null)},
                    {"priorCarryStatus",priorCarryPossible?"SURVIVAL_UNCONFIRMED":"STARTING_FRESH"},
                    {"optimisticTitleOnly",frontierTitleOnly(c,slot.attacker,tick,cash,last,timing)},
                    {"titleIfStartingFresh",freshFits?frontierTitleOnly(c,slot.attacker,static_cast<int>(finish),cash,last,timing):QJsonValue(QJsonValue::Null)}});
            }
        }
        return {{"status",witnesses.isEmpty()?"NO_ROUTE_WITNESSED":"ROUTE_WITNESSED"},
            {"scope","AVAILABLE_ORIGINALS_ON_RETAINED_CALENDAR_ONLY"},{"workLimited",limited || work==0},
            {"routeQueries",calls},{"used",static_cast<qint64>(before-work)},{"retainedOutcomesAlreadyCovered",alreadyCovered},{"witnesses",witnesses}};
    }
}
