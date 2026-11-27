#pragma once

#include "ai/coordinator/mobilityfield.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace Coordinator
{
    // All identities are indices in the same knowledge snapshot. A vacant square is represented by a dead body or a different live post.
    struct OpeningLanePawn
    {
        TilePoint at{INVALID_TILE};
        int side{}, hp{}, fuel{}; // Native health in hundredths of a full unit, with 100 denoting 10 displayed HP.
        std::array<int,2> rounds{};
        bool spent{};
        friend bool operator==(const OpeningLanePawn&,const OpeningLanePawn&)=default;
    };
    struct OpeningLaneShot
    {
        int shooter{},target{};
        TilePoint from{INVALID_TILE};
        friend bool operator==(const OpeningLaneShot&,const OpeningLaneShot&)=default;
        int damageHint{}; // Search ordering only. The dated native exchange determines every resulting health and ammo value.
    };
    struct OpeningLaneExchange
    {
        double hit{-1},counter{-1};
        int weapon{-1},counterWeapon{-1};
        friend bool operator==(const OpeningLaneExchange&,const OpeningLaneExchange&)=default;
    };
    struct OpeningLaneAction
    {
        OpeningLaneShot shot;
        OpeningLaneExchange native;
        int moveCost{},shooterBefore{},shooterAfter{},targetBefore{},targetAfter{};
        std::array<int,2> startingLoad{},returnLoad{};
        TilePoint origin{INVALID_TILE},targetTile{INVALID_TILE};
        friend bool operator==(const OpeningLaneAction&,const OpeningLaneAction&)=default;
    };
    struct OpeningLaneResult
    {
        std::vector<OpeningLaneAction> witness;
        int visited{},proposals{};
        bool truncated{};
    };
    using OpeningLaneState=std::vector<OpeningLanePawn>;

    inline int openingLaneOccupant(const OpeningLaneState &board,TilePoint tile)
    {
        for(int i=0;i<static_cast<int>(board.size());++i)if(board[i].hp>0 && board[i].at==tile)return i;
        return -1;
    }
    inline bool debitOpeningLaneRound(std::array<int,2> &rounds,int slot)
    {
        if(slot<0 || slot>=2 || rounds[slot]==0)return false;
        if(rounds[slot]>0)--rounds[slot];
        return true;
    }
    // moveCost must use the *current* opposing walls, friendly-transit/empty-stop rules, typed movement and action-day fuel.
    // exchange must query the native two-stock matchup at these positions, current health and loaded slots. It never supplies movement rights.
    template<class MoveCost,class Exchange>
    std::optional<OpeningLaneAction> applyOpeningLaneShot(OpeningLaneState &state,const OpeningLaneShot &shot,
        int protectedShooter,MoveCost &moveCost,Exchange &exchange,std::int64_t &work)
    {
        const int i=shot.shooter,j=shot.target,n=static_cast<int>(state.size());
        if(protectedShooter<0 || protectedShooter>=n || i<0 || j<0 || i>=n || j>=n || i==protectedShooter || state[i].side!=state[protectedShooter].side ||
           state[j].side==state[i].side || state[i].hp<=0 || state[j].hp<=0 || state[i].spent)return {};
        const int occupied=openingLaneOccupant(state,shot.from);
        if(occupied>=0 && occupied!=i)return {};
        const int cost=moveCost(state,i,shot.from,work);
        if(cost<0 || (state[i].fuel>=0 && cost>state[i].fuel))return {};
        const auto q=exchange(state,shot,work);
        if(!q || !std::isfinite(q->hit) || !std::isfinite(q->counter) || q->hit<0 || q->hit>10000 || q->counter>10000)return {};
        auto actor=state[i],victim=state[j];const auto shooterLoad=actor.rounds,targetLoad=victim.rounds;
        if(!debitOpeningLaneRound(actor.rounds,q->weapon))return {};
        if(q->counter>=0 && !debitOpeningLaneRound(victim.rounds,q->counterWeapon))return {};
        actor.at=shot.from;actor.spent=true;
        if(actor.fuel>=0)actor.fuel-=cost;
        actor.hp=std::max(0,actor.hp-static_cast<int>(std::max(0.,q->counter)));
        victim.hp=std::max(0,victim.hp-static_cast<int>(q->hit));
        OpeningLaneAction paid{shot,*q,cost,state[i].hp,actor.hp,state[j].hp,victim.hp,shooterLoad,targetLoad,state[i].at,state[j].at};
        state[i]=actor;state[j]=victim;
        return paid;
    }

    // This bounded frontier nominates legal conditional responses. It cannot prove a wall is impenetrable from a failed search.
    template<class MoveCost,class Exchange,class CanFinish>
    OpeningLaneResult findOpeningLane(const OpeningLaneState &initial,int reserved,int guard,TilePoint finalPost,
        const std::vector<int> &screens,const std::vector<OpeningLaneShot> &options,MoveCost moveCost,Exchange exchange,
        CanFinish canFinish,std::int64_t &work,int width=64,int depth=7)
    {
        OpeningLaneResult result;
        const int n=static_cast<int>(initial.size());
        if(reserved<0 || guard<0 || reserved>=n || guard>=n || width<=0 || depth<=0 || options.empty() ||
           initial[reserved].hp<=0 || initial[reserved].spent || initial[guard].hp<=0 ||
           initial[reserved].side==initial[guard].side ||
           std::any_of(screens.begin(),screens.end(),[&](int i){return i<0 || i>=n || initial[i].hp<=0;}))return result;
        using Rank=std::tuple<bool,int,int,int,int>;
        struct Node{OpeningLaneState board;std::vector<OpeningLaneAction> actions;Rank rank;};
        const auto rank=[&](const OpeningLaneState &board){
            std::vector<std::pair<TilePoint,std::vector<std::pair<int,int>>>> posts;
            for(const auto &a:options){
                if(a.shooter<0 || a.shooter>=n || a.target!=guard || a.shooter==reserved || a.from==finalPost ||
                   a.from==initial[reserved].at || board[a.shooter].spent || board[a.shooter].hp<=0)continue;
                const int occupant=openingLaneOccupant(board,a.from);
                if(occupant>=0 && board[occupant].side==board[reserved].side && board[occupant].spent)continue;
                auto p=std::find_if(posts.begin(),posts.end(),[&](const auto &s){return s.first==a.from;});
                if(p==posts.end())p=posts.emplace(posts.end(),a.from,std::vector<std::pair<int,int>>{});
                p->second.emplace_back(a.damageHint,a.shooter);
            }
            int room=0;
            if(posts.size()==2){
                // Two openings are common around a blocked destination. Assign two different movers when prioritizing alternatives.
                for(const auto &p:posts)for(const auto &[v,i]:p.second)room=std::max(room,v);
                for(const auto &[v,i]:posts[0].second)for(const auto &[w,j]:posts[1].second)if(i!=j)room=std::max(room,v+w);
            }else for(const auto &p:posts){int best=0;for(const auto &[v,i]:p.second)best=std::max(best,v);room+=best;}
            int cleared=0,damage=0;
            for(int i:screens){cleared+=board[i].hp==0;damage+=100*(initial[i].hp-board[i].hp)/initial[i].hp;}
            return Rank{room>=board[guard].hp,-board[guard].hp,cleared,damage,room};
        };
        std::vector<Node> frontier{{initial,{},{}}};
        for(int turn=0;turn<depth && !frontier.empty();++turn){
            std::vector<Node> next;
            for(const auto &node:frontier){
                ++result.visited;
                for(const auto &shot:options){
                    const std::int64_t fee=1+static_cast<std::int64_t>(n);
                    if(work<fee){result.truncated=true;return result;}work-=fee;++result.proposals;
                    if(shot.target!=guard && std::find(screens.begin(),screens.end(),shot.target)==screens.end())continue;
                    auto board=node.board;
                    const auto charged=applyOpeningLaneShot(board,shot,reserved,moveCost,exchange,work);
                    if(!charged)continue;
                    if(board[guard].hp==0 && canFinish(board,reserved,finalPost,work)){
                        result.witness=node.actions;result.witness.push_back(*charged);return result;
                    }
                    const std::int64_t sortFee=1+static_cast<std::int64_t>(options.size())*(options.size()+n)+static_cast<std::int64_t>(width)*n;
                    if(work<sortFee){result.truncated=true;return result;}work-=sortFee;
                    const auto proposedRank=rank(board);
                    if(static_cast<int>(next.size())==width && proposedRank<=next.back().rank){result.truncated=true;continue;}
                    if(std::any_of(next.begin(),next.end(),[&](const Node &old){return old.board==board;}))continue;
                    auto actions=node.actions;actions.push_back(*charged);
                    auto where=std::find_if(next.begin(),next.end(),[&](const Node &v){return proposedRank>v.rank;});
                    next.insert(where,{std::move(board),std::move(actions),proposedRank});
                    if(static_cast<int>(next.size())>width){next.pop_back();result.truncated=true;}
                }
            }
            frontier=std::move(next);
        }
        result.truncated|=!frontier.empty();return result;
    }
}
