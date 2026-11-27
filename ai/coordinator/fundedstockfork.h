#pragma once

#include "ai/coordinator/fundedintercept.h"
#include "ai/coordinator/fundedinterceptvalue.h"

namespace Coordinator
{
    struct FundedCaptureFork
    {
        const StockCaptureTraffic *calendar{};
        std::uint64_t revision{};
        int unit{NO_UNIT},tick{-1},fuel{},carry{};
        TilePoint tile{INVALID_TILE};
        double hp{};
        std::array<int,2> rounds{};
        friend bool operator==(const FundedCaptureFork&,const FundedCaptureFork&)=default;
    };
    struct FundedStockWitness
    {
        const StockCaptureTraffic &calendar;
        const MobilityCostGrid &grid;
        double startHP;
        int startFuel,startCarry;
        std::array<int,2> startAmmo;
    };
    inline std::optional<StockCapturePath> stockEncounterPath(const QJsonObject &proof,int width,int height)
    {
        if(proof["status"]!="WITNESSED" || !proof["source"].toString().isEmpty())return {};
        StockCapturePath path;
        path.entryTick=proof["entryTick"].toInt(-1);
        const auto tile=[&](const QJsonValue &v) {
            const auto a=v.toArray();
            if(a.size()!=2)return INVALID_TILE;
            TilePoint p{a[0].toInt(-1),a[1].toInt(-1)};
            return p.x>=0 && p.x<width && p.y>=0 && p.y<height ? p:INVALID_TILE;
        };
        for(const auto &v:proof["stops"].toArray())
        {
            const auto s=v.toObject();StockCaptureStep step{s["tick"].toInt(-1),tile(s["tile"]),{}};
            if(step.tile==INVALID_TILE || step.tick<0 || (!path.steps.empty() && path.steps.back().tick+2!=step.tick))return {};
            for(const auto &c:s["cells"].toArray())
            {
                const auto sq=tile(c);
                if(sq==INVALID_TILE)return {};
                step.cells.push_back(sq.y*width+sq.x);
            }
            path.steps.push_back(std::move(step));
        }
        if(path.steps.empty() || path.steps.back().tick!=path.entryTick ||
           path.steps.front().tick!=proof["departureTick"].toInt(-1))return {};
        return path;
    }

    inline std::optional<FundedCaptureFork> replayCaptureFork(const QJsonObject &proof,const StockCapturePath &path,
        int divergence,const FundedStockWitness &facts)
    {
        const auto origin=proof["from"].toArray();
        if(origin.size()!=2 || facts.startHP<=0 || facts.startFuel<-1)return {};
        FundedCaptureFork state{&facts.calendar,facts.calendar.generation(),proof["unit"].toInt(NO_UNIT),
            divergence,facts.startFuel,facts.startCarry,{origin[0].toInt(-1),origin[1].toInt(-1)},facts.startHP,facts.startAmmo};
        for(const auto &s:path.steps)
        {
            if(s.tick>=divergence)break;
            for(const int sq:s.cells)
            {
                const TilePoint next{sq%facts.grid.width(),sq/facts.grid.width()};
                if(manhattanDistance(state.tile,next)!=1)return {};
                const int cost=facts.grid.cost(next.x,next.y,entryDirection(state.tile.x,state.tile.y,next.x,next.y));
                if(cost<0 || (state.fuel>=0 && cost>state.fuel))return {};
                if(state.fuel>=0)state.fuel-=cost;
                state.tile=next;state.carry=0;
            }
            if(s.tile!=state.tile)return {};
        }
        for(const auto &v:proof["captureInstallments"].toArray())
        {
            const auto q=v.toObject();
            if(q["tick"].toInt(-1)<divergence)state.carry=q["carryAfter"].toInt();
        }
        for(const auto &v:proof["shots"].toArray())
        {
            const auto s=v.toObject();if(s["tick"].toInt(-1)>=divergence)continue;
            if(std::abs(s["hpBefore"].toDouble()-state.hp)>1e-8 || !s.contains("counterWeapon"))return {};
            const auto weapon=s["counterWeapon"].toInt();if(weapon>=0 && (weapon>1 || state.rounds[weapon]==0))return {};
            spendCombatRound(state.rounds,weapon);state.hp=s["hpAfter"].toDouble();
            if(state.hp<=0)return {};
        }
        return state;
    }

}
