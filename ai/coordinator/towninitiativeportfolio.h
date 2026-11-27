#pragma once

#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    struct AppliedTownEncounter
    {
        TownAssaultLine line;
        OwnerSign priorOwner{OwnerSign::Neutral};
        QJsonObject captureProof;
        MilliFunds ownership{};
    };

    struct PricedTownInitiative
    {
        AppliedTownEncounter chosen;
        PropertyStockColumn property;
        TownAssaultLedger ledger;
        QJsonArray purchases;
        int evidence{-1};
    };

    inline std::set<int> townParticipants(const TownAssaultLine &line)
    {
        std::set<int> actors;
        if(line.capper>=0)actors.insert(line.capper);
        if(line.blocker>=0)actors.insert(line.blocker);
        for(const auto &move:line.moves)actors.insert(move.actor);
        for(const auto &shot:line.shots){actors.insert(shot.actor);actors.insert(shot.target);}
        return actors;
    }

    inline QJsonObject incompatibleTownInitiatives(const TownAssaultBoard &board,const TownAssaultLine &a,
        const TownAssaultLine &b,std::int64_t &budget)
    {
        const auto tile=[](TilePoint t){return QJsonArray{t.x,t.y};};
        const int n=static_cast<int>(board.units.size());
        const auto id=[&](int i){return i>=0 && i<n?board.terrain.units[i].engineUnitId:NO_UNIT;};
        auto conflict=[&](QString why,int actor,int other,int tick,TilePoint where) {
            return QJsonObject{{"status",why},{"actor",id(actor)},{"other",id(other)},{"actorIndex",actor},{"otherIndex",other},
                {"tick",tick},{"tile",tile(where)},{"firstTown",tile(a.town)},{"secondTown",tile(b.town)}};
        };
        if(a.town==b.town)return conflict("SAME_TITLE",a.capper,b.capper,std::min(a.entryTick,b.entryTick),a.town);
        const auto left=townParticipants(a),right=townParticipants(b);
        for(int x:left)if(right.contains(x))return conflict("SHARED_COMBATANT_OR_CAPPER",x,x,-1,INVALID_TILE);
        const auto cell=[&](TilePoint t){return TownAssaultDetail::cell(board.terrain,t);};
        for(int direction=0;direction<2;++direction)
        {
            const auto &one=direction?b:a,&two=direction?a:b;
            StockCaptureTraffic extra(board.terrain.width,board.terrain.height,board.cutoff);
            for(const auto &m:one.moves)TownAssaultDetail::reserve(board.terrain,extra,m,board.cutoff);
            for(const auto &m:two.moves)
            {
                for(const auto &step:m.path.steps)
                {
                    if(budget<1+static_cast<std::int64_t>(step.cells.size()))return conflict("UNPRICED_JOINT_WORK",m.actor,two.capper,step.tick,step.tile);
                    budget-=1+step.cells.size();
                    for(int sq:step.cells)if(!extra.open(m.actor,m.side,sq,step.tick,false))return conflict("CROSSING_ROUTES",m.actor,one.capper,step.tick,{sq%board.terrain.width,sq/board.terrain.width});
                    if(!extra.open(m.actor,m.side,cell(step.tile),step.tick,true) || !extra.open(m.actor,m.side,cell(step.tile),step.tick+1,true))
                        return conflict("SHARED_OVERNIGHT_STOP",m.actor,one.capper,step.tick,step.tile);
                }
                for(int t=m.actionTick;t<=board.cutoff;++t) if(!extra.open(m.actor,m.side,cell(m.destination),t,true))
                    return conflict("CONFLICTING_FINAL_POST",m.actor,one.capper,t,m.destination);
            }
        }
        auto original=[&](int actor,int tick)->TilePoint {
            if(actor<n)return board.positions[actor];
            const auto &g=board.recruits[actor-n];
            if(tick<g.buildTick)return INVALID_TILE;
            return tick>=g.readyTick?g.destination:g.factory;
        };
        auto where=[&](const TownAssaultLine &line,int actor,int tick) {
            TilePoint pos=original(actor,tick);
            for(const auto &m:line.moves) if(m.actor==actor)
                for(const auto &s:m.path.steps)if(s.tick<=tick)pos=s.tile;
            for(const auto &s:line.shots)if(s.tick<=tick && ((s.target==actor && s.targetAfter<=0) ||
                (s.actor==actor && s.actorAfter<=0)))return INVALID_TILE;
            return pos;
        };
        auto team=[&](int i){return i<n?int(board.terrain.units[i].enemy):board.recruits[i-n].side;};
        auto potential=[&](int gun,int victim,int tick,TilePoint from,TilePoint target) {
            int lo=1,hi=0,travel=0,floor=0;
            if(gun<n)
            {
                const auto &pawn=board.terrain.units[gun];const auto &unit=board.units[gun];
                if(!unit.canFire || (board.load[gun][0]==0 && board.load[gun][1]==0))return false;
                lo=unit.minRange;hi=unit.maxRange;
                if(pawn.grid>=0 && unit.canMoveAndFire)
                {
                    travel=pawn.movement;const auto &grid=board.terrain.grids[pawn.grid];
                    if(auto *mc=board.terrain.movementCache.get();mc && mc->minimumEntryCost.contains(&grid))floor=mc->minimumEntryCost.at(&grid);
                }
                if(victim<n && supportWeapon(board.terrain,gun,victim,lo,board.load[gun]).base<=0)return false;
            }
            else
            {
                const auto &g=board.recruits[gun-n];if(!g.unit || tick<g.readyTick)return false;
                lo=g.unit->minRange;hi=g.unit->maxRange;travel=g.unit->mobileFire?g.unit->movement:0;floor=g.unit->minimumMoveCost;
                if(victim<n && (victim>=static_cast<int>(g.unit->attack.size()) || g.unit->attack[victim]<=0))return false;
            }
            const int d=manhattanDistance(from,target);
            return (lo<=d && d<=hi) || (travel>0 && std::int64_t(std::max(0,d-hi))*floor<=travel);
        };
        const auto end=[&](const TownAssaultLine &l){int t=std::max(l.entryTick,l.finishTick);for(const auto &s:l.shots)t=std::max(t,s.tick);return t;};
        const int until=std::max(end(a),end(b));
        for(int t=2;t<=until;++t)for(int x:left)for(int y:right)
        {
            if(budget<=0)return conflict("UNPRICED_JOINT_WORK",x,y,t,INVALID_TILE);--budget;
            if(team(x)==team(y))continue;
            const auto px=where(a,x,t),py=where(b,y,t);
            if(px==INVALID_TILE || py==INVALID_TILE || (px==original(x,t) && py==original(y,t)))continue;
            const bool lx=team(x)==t%2;
            if(potential(lx?x:y,lx?y:x,t,lx?px:py,lx?py:px))
                return conflict("UNPRICED_NEW_CROSSFIRE",lx?x:y,lx?y:x,t,lx?py:px);
        }
        return {};
    }
}
