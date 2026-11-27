#pragma once
#include <QPoint>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <vector>

namespace Coordinator
{
    struct OpeningCapturePath
    {
        std::vector<QPoint> cells;
        std::int64_t cost{};
    };
    // Unlimited movement is the optimistic hostile search. Fixed friendly
    // occupants still block a capture path; clearing one with the sole unit
    // consumes that unit's activation. A second unit or a refresh invalidates
    // this premise and the caller must leave the family open.
    template<class Cost>
    std::optional<OpeningCapturePath> singleActivationPath(int width,int height,QPoint start,QPoint goal,
        const std::set<std::pair<int,int>> &occupied,Cost edge)
    {
        if(width<=0 || height<=0 || std::int64_t(width)*height>std::numeric_limits<int>::max())return {};
        const auto inside=[&](QPoint p){return p.x()>=0 && p.x()<width && p.y()>=0 && p.y()<height;};
        if(!inside(start) || !inside(goal) || occupied.contains({goal.x(),goal.y()}))return {};
        const auto key=[&](QPoint p){return p.x()+p.y()*width;};
        const auto origin=key(start),target=key(goal);const auto count=std::size_t(width)*height;
        using Distance=std::int64_t;
        std::vector<Distance> best(count,std::numeric_limits<Distance>::max());
        std::vector<int> previous(count,-1);
        using Node=std::pair<Distance,int>;
        std::priority_queue<Node,std::vector<Node>,std::greater<Node>> frontier;
        frontier.emplace(0,origin);best[origin]=0;
        while(!frontier.empty()){
            const auto [paid,at]=frontier.top();frontier.pop();
            if(paid!=best[at])continue;
            if(at==target){
                OpeningCapturePath walk;walk.cost=paid;
                for(int j=at;j!=-1;j=previous[j])walk.cells.push_back({j%width,j/width});
                std::reverse(walk.cells.begin(),walk.cells.end());return walk;
            }
            const QPoint p{at%width,at/width};
            for(const QPoint delta:{QPoint{1,0},QPoint{-1,0},QPoint{0,1},QPoint{0,-1}}){
                const auto next=p+delta;
                if(!inside(next) || occupied.contains({next.x(),next.y()}))continue;
                const int step=edge(p,next);
                if(step<0)continue;
                const auto n=key(next);
                if(best[n]<=paid+step)continue;
                best[n]=paid+step;previous[n]=at;frontier.emplace(best[n],n);
            }
        }
        return {};
    }
}
