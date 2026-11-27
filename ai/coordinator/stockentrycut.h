#pragma once

#include "ai/coordinator/stockcaptureroute.h"

namespace Coordinator
{
    struct StockEntryBarrier
    {
        int cell{},actor{},from{},through{};
    };

    struct StockEntryCut
    {
        int earliest{},last{};
        std::vector<StockEntryBarrier> posts;
        std::vector<int> impassable;
        std::vector<int> interior;
        int release{-1};
    };

    struct StockEntryObligation
    {
        int claimant;
        TilePoint target;
        std::uint64_t generation;
        std::vector<StockEntryBarrier> posts;
    };

    struct StockEntryReview
    {
        enum class Status { Retained, Changed, WorkLimited };
        Status status{Status::Retained};
        int actor{NO_UNIT},cell{-1},tick{-1};
    };

    inline StockEntryReview recheckStockEntry(const StockCaptureTraffic &traffic,
        const StockEntryObligation &proof,std::int64_t &work)
    {
        if(proof.posts.empty())return {};
        int first=proof.posts.front().from,last=proof.posts.front().through;
        for(const auto &p:proof.posts){first=std::min(first,p.from);last=std::max(last,p.through);}
        for(int tick=first;tick<=last;++tick)for(const auto &post:proof.posts)
        {
            if(tick<post.from || tick>post.through)continue;
            bool present=false;
            for(const auto &use:traffic.uses(post.cell))
            {
                if(work<=0)return {StockEntryReview::Status::WorkLimited,post.actor,post.cell,tick};
                --work;
                if(!use.stopped || use.last<tick || use.first>tick)continue;
                if(use.actor!=post.actor)return {StockEntryReview::Status::Changed,post.actor,post.cell,tick};
                present=true;
            }
            if(!present)return {StockEntryReview::Status::Changed,post.actor,post.cell,tick};
        }
        return {};
    }

    // Prove denial using only the final step into a target. Give the attacker
    // its original movement each turn, unlimited fuel and its earliest own
    // activation. All other positions may disappear without opening this cut.
    inline std::optional<StockEntryCut> stockEntryCut(const MobilityCostGrid &grid,const StockCaptureTraffic &traffic,
        const StockRouteLowerBounds &bounds,int claimant,int side,TilePoint from,TilePoint target,
        int movement,bool ignoresCollision,int last,std::int64_t &work)
    {
        if (from==target || movement<=0 || !grid.onGrid(from.x,from.y) || !grid.onGrid(target.x,target.y)) return {};
        if (side<0 || side>1 || last<side) return {};
        last-=int(last%2!=side);
        const int width=grid.width(),goal=target.y*width+target.x;
        const auto *field=bounds.preparedCost(grid,goal);
        if (!field) return {};
        const int distance=(*field)[from.y*width+from.x];
        if (distance==std::numeric_limits<int>::max()) return {};
        const auto earliest=[&](int cost) {return side+2LL*(std::max(0,cost-1)/movement);};
        const auto first=earliest(distance);
        if (first>last) return {};
        StockEntryCut cut{static_cast<int>(first),last,{},{}};
        const auto visit=[&] {if(work<=0)return false;--work;return true;};
        const auto barrier=[&](int cell,int firstTick,bool transit)->std::optional<StockEntryBarrier> {
            for (const auto &u:traffic.uses(cell))
            {
                if (!visit()) return {};
                if (!u.stopped || u.actor<0 || u.actor==claimant || u.first>firstTick || u.last<last) continue;
                if (transit && (ignoresCollision || u.side==side)) continue;
                return StockEntryBarrier{cell,u.actor,firstTick,last};
            }
            return {};
        };
        // A continuously occupied destination prevents a capture even for
        // troops which can pass through enemies.
        if (const auto stop=barrier(goal,std::max(0,cut.earliest-1),false))
        {
            cut.posts.push_back(*stop);
            return cut;
        }
        for (const auto dir:ENTRY_DIRECTIONS)
        {
            if (!visit()) return {};
            const auto delta=entryStep(dir);
            const TilePoint before{target.x-delta.dx,target.y-delta.dy};
            if (!grid.onGrid(before.x,before.y)) continue;
            const int edge=grid.cost(target.x,target.y,dir),sq=before.y*width+before.x;
            bool passable=before==from;
            if (edge>=0 && edge<=movement && !passable)
                for (const auto ingress:ENTRY_DIRECTIONS)
                {
                    if (!visit()) return {};
                    const auto step=entryStep(ingress);
                    if (!grid.onGrid(before.x-step.dx,before.y-step.dy)) continue;
                    const auto cost=grid.cost(before.x,before.y,ingress);
                    if (cost>=0 && cost<=movement) {passable=true;break;}
                }
            if (edge<0 || edge>movement || !passable) {cut.impassable.push_back(sq);continue;}
            // If the attacker could have reached the gate before the guard,
            // even the plan to put the guard there would need to be reproved.
            // A cheaper way to a gate followed by its final edge would also
            // be a cheaper way to the target, giving a lower bound per gate.
            if (before==from) return {};
            const int firstGate=static_cast<int>(earliest(std::max(0,distance-edge)));
            const auto post=barrier(sq,std::max(0,firstGate-1),true);
            if (!post) return {};
            cut.posts.push_back(*post);
        }
        return cut;
    }

    // After a failed ordinary route, work backward from the property. An
    // unguarded last step can itself have no entrance. Only solid opposing
    // posts on the boundary of the resulting target-side region are needed.
    inline std::optional<StockEntryCut> stockPocketEntryCut(const MobilityCostGrid &grid,
        const StockCaptureTraffic &traffic,const StockRouteLowerBounds &bounds,int claimant,int side,
        TilePoint from,TilePoint target,int movement,bool ignoresCollision,int release,int last,std::int64_t &work)
    {
        if(from==target || movement<=0 || ignoresCollision || side<0 || side>1 ||
           release<0 || release%2!=side || release>last || !grid.onGrid(from.x,from.y) ||
           !grid.onGrid(target.x,target.y) || std::int64_t(grid.width())*grid.height()!=traffic.cellCount() ||
           last>traffic.lastTick())return {};
        last-=int(last%2!=side);
        const int width=grid.width(),goal=target.y*width+target.x,origin=from.y*width+from.x;
        const auto *field=bounds.preparedCost(grid,goal);
        if(!field || (*field)[origin]==std::numeric_limits<int>::max())return {};
        const int distance=(*field)[origin];
        const auto firstAt=[&](int suffix){
            const auto lower=std::max(0,distance-suffix);
            return release+2LL*(std::max(0,lower-1)/movement);
        };
        if(firstAt(0)>last)return {};
        StockEntryCut cut{static_cast<int>(firstAt(0)),last,{},{}};
        std::vector<std::pair<int,int>> open{{goal,0}};
        std::set<int> inside{goal},barriers,impassable;
        for(std::size_t pos=0;pos<open.size();++pos)
        {
            const auto [cell,suffix]=open[pos];
            const TilePoint current{cell%width,cell/width};
            for(const auto dir:ENTRY_DIRECTIONS)
            {
                if(work<=0)return {};
                --work;
                const auto delta=entryStep(dir);
                const TilePoint p{current.x-delta.dx,current.y-delta.dy};
                if(!grid.onGrid(p.x,p.y))continue;
                const int next=p.y*width+p.x,edge=grid.cost(current.x,current.y,dir);
                if(edge<0 || edge>movement || inside.contains(next) || barriers.contains(next) || impassable.contains(next))continue;
                if(next==origin)return {};
                bool canEnter=false;
                for(const auto ingress:ENTRY_DIRECTIONS)
                {
                    if(work<=0)return {};
                    --work;
                    const auto deltaIn=entryStep(ingress);
                    if(!grid.onGrid(p.x-deltaIn.dx,p.y-deltaIn.dy))continue;
                    const int price=grid.cost(p.x,p.y,ingress);
                    if(price>=0 && price<=movement){canEnter=true;break;}
                }
                if(!canEnter){impassable.insert(next);continue;}
                // 'suffix + edge' is an actual directed terrain walk to the
                // property. Subtracting it from the immutable shortest cost
                // can only make a claimant arrive earlier than it really can.
                const int path=static_cast<int>(std::min<std::int64_t>(distance,std::int64_t(suffix)+edge));
                const int needed=std::max(0,static_cast<int>(firstAt(path))-1);
                for(const auto &post:traffic.uses(next))
                {
                    if(work<=0)return {};
                    --work;
                    if(!post.stopped || post.actor<0 || post.actor==claimant || post.side==side ||
                       post.first>needed || post.last<last)continue;
                    if(std::any_of(cut.posts.begin(),cut.posts.end(),[&](const auto &old){return old.actor==post.actor;}))return {};
                    cut.posts.push_back({next,post.actor,needed,last});
                    barriers.insert(next);break;
                }
                if(!barriers.contains(next)){inside.insert(next);open.emplace_back(next,path);}
            }
        }
        if(cut.posts.empty())return {};
        cut.impassable.assign(impassable.begin(),impassable.end());
        cut.interior.assign(inside.begin(),inside.end());
        cut.release=release;
        return cut;
    }
}
