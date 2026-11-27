#pragma once

#include "ai/coordinator/townassault.h"
#include <bit>
#include <memory>

namespace Coordinator
{
    struct TownGuardSuffix
    {
        std::shared_ptr<const int> world;
        std::vector<std::int64_t> identity;
        TownAssaultProgress search;
        int resets{},resumes{},completeReads{},retainedFirstShots{};
    };
    struct TownRivalSuffixes {TownGuardSuffix held,afterFire;};

    // The token owns one immutable terrain, stock/paid calendar, economy,
    // CO, obligation and opponent-policy snapshot. Only the guard's dated
    // route and the observed volley are allowed to modify that starting board.
    // Store their exact inputs, not a hash or simply the guard's endpoint.
    inline TownAssaultAlternatives priceGuardSuffix(const TownAssaultBoard &board,const TownAssaultTarget &late,
        StockRouteLowerBounds &lower,const TownAssaultValue &value,std::int64_t &work,
        const std::shared_ptr<const int> &world,TownGuardSuffix *saved,const TownAssaultMove &guard,
        const TownAssaultLine *exchange,int reveal,std::int64_t &credit,bool &advanced)
    {
        const auto uncached=[&]{return priceTownAssault(board,late,lower,value,work);};
        if(!world || !saved)return uncached();
        const auto moveCost=[](const TownAssaultMove &m){
            std::int64_t cost=1;
            for(const auto &st:m.path.steps)cost+=1+st.cells.size();
            return cost;
        };
        std::int64_t lookup=1+moveCost(guard);
        if(exchange)
        {
            lookup+=exchange->shots.size()+exchange->losses.size();
            for(const auto &m:exchange->moves)lookup+=moveCost(m);
        }
        if(work<lookup)return uncached();
        work-=lookup;
        std::vector<std::int64_t> key{late.property.tile.x,late.property.tile.y,late.defendingSide,late.occupant,
            late.occupiedFrom,late.occupiedThrough,late.requiredCapper,int(late.property.ownerBefore),
            late.property.income.oursPerTurn,late.property.income.enemyPerTurn,reveal,
            static_cast<std::int64_t>(board.traffic.generation()),int(exchange!=nullptr)};
        std::set<int> changed{guard.actor};
        const auto addMove=[&](const TownAssaultMove &m){
            changed.insert(m.actor);
            key.insert(key.end(),{m.actor,m.side,m.fromTick,m.actionTick,m.origin.x,m.origin.y,m.destination.x,m.destination.y,
                m.path.fuel,m.path.entryTick,static_cast<std::int64_t>(m.path.steps.size())});
            for(const auto &s:m.path.steps)
            {
                key.insert(key.end(),{s.tick,s.tile.x,s.tile.y,static_cast<std::int64_t>(s.cells.size())});
                key.insert(key.end(),s.cells.begin(),s.cells.end());
            }
        };
        addMove(guard);
        if(exchange)
        {
            key.push_back(exchange->moves.size());for(const auto &m:exchange->moves)addMove(m);
            key.push_back(exchange->shots.size());
            for(const auto &s:exchange->shots)
            {
                changed.insert(s.actor);changed.insert(s.target);
                key.insert(key.end(),{s.actor,s.target,s.tick,s.weapon,s.counterWeapon,s.from.x,s.from.y,s.to.x,s.to.y,
                    std::bit_cast<std::int64_t>(s.actorBefore),std::bit_cast<std::int64_t>(s.actorAfter),
                    std::bit_cast<std::int64_t>(s.targetBefore),std::bit_cast<std::int64_t>(s.targetAfter),s.targetLoss,s.counterLoss});
            }
            key.push_back(exchange->losses.size());
            for(auto [tick,loss]:exchange->losses)key.insert(key.end(),{tick,loss});
        }
        for(int actor:changed)
        {
            if(actor<0 || actor>=static_cast<int>(board.units.size()))return uncached();
            const auto &r=board.rights[actor];
            key.insert(key.end(),{actor,board.positions[actor].x,board.positions[actor].y,std::bit_cast<std::int64_t>(board.hp[actor]),
                board.load[actor][0],board.load[actor][1],r.readyTick,r.lastFreeTick,r.fuel,r.mobileFrom,r.displacedValue});
        }
        if(saved->world!=world || saved->identity!=key)
        {
            if(saved->world)++saved->resets;
            saved->world=world;saved->identity=std::move(key);saved->search=TownAssaultProgress{};
        }
        auto &cursor=saved->search;const bool done=cursor.complete;
        if(done)++saved->completeReads;
        const auto before=std::tuple{cursor.tick,cursor.firstLane,cursor.secondLane,cursor.emptySteps};
        if(!done && cursor.tick>=0)
        {
            ++saved->resumes;
            if(cursor.secondLane>=0 && cursor.afterFirst)++saved->retainedFirstShots;
        }
        const auto offered=work;
        auto answer=priceTownAssault(board,late,lower,value,work,&cursor);
        if(!done && (cursor.complete || before!=std::tuple{cursor.tick,cursor.firstLane,cursor.secondLane,cursor.emptySteps}))
        {
            advanced=true;
            const auto completedWork=offered-(cursor.complete?work:cursor.unfinishedAllowance);
            credit+=std::max<std::int64_t>(0,completedWork-lookup);
        }
        return answer;
    }
}
