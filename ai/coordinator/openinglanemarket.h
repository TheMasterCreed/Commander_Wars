#pragma once

#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/orderedreplylane.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include <queue>

namespace Coordinator
{
    struct OpeningLaneHandoff
    {
        OpeningLaneState afterService;
        // Supply full native movement points after any paid service. Each body still has its own current fuel.
        std::vector<int> movement;
        bool paidOpeningKnown{},stockServiceKnown{},datedCombatModifiersKnown{};
    };
    struct ProposedOpeningLane
    {
        int reserve{NO_UNIT},guard{NO_UNIT};
        TilePoint firing{INVALID_TILE};
        std::vector<int> screens;
        OpeningLaneResult search;
    };

    // Cache tree geometry only inside a request. Every physical branch has its own occupancy, including all new bodies from F0.
    class OpeningLanePaths
    {
    public:
        struct Field{std::vector<int> cost,parent;};
        OpeningLanePaths(const CombatTerrain &terrain,const OpeningLaneHandoff &opening):m_terrain(terrain),m_opening(opening){}
        Field build(const OpeningLaneState &state,int i,bool ignoreStock,std::int64_t &work) const
        {
            Field result;const auto &t=m_terrain;
            if(i<0 || i>=static_cast<int>(state.size()) || !t.units[i].present || state[i].hp<=0 || t.width<=0 || t.height<=0 || t.width>std::numeric_limits<int>::max()/t.height)return result;
            const auto &pawn=t.units[i];const auto &u=state[i];const int area=t.width*t.height;
            const auto valid=[&](TilePoint p){return p.x>=0 && p.y>=0 && p.x<t.width && p.y<t.height;};
            const std::int64_t prepare=2LL*area+static_cast<std::int64_t>(state.size());
            if(!valid(u.at) || work<prepare){work=0;return result;}work-=prepare;
            result.cost.assign(area,UNREACHABLE);result.parent.assign(area,-1);
            const auto cell=[&](TilePoint p){return p.y*t.width+p.x;};const int start=cell(u.at);result.cost[start]=0;
            if(pawn.grid<0 || pawn.grid>=static_cast<int>(t.grids.size()))return result;
            std::vector<int> occupied(area,NO_UNIT);
            for(int j=0;j<static_cast<int>(state.size());++j)if(state[j].hp>0 && valid(state[j].at))occupied[cell(state[j].at)]=j;
            for(auto [x,y]:t.friendlyBirthSites)if(valid({x,y}))occupied[cell({x,y})]=-2;
            const auto &grid=t.grids[pawn.grid];if(grid.width()!=t.width || grid.height()!=t.height)return {};
            const int limit=u.fuel<0?m_opening.movement[i]:std::min(u.fuel,m_opening.movement[i]);
            using Visit=std::pair<int,int>;std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> queue;
            queue.emplace(0,start);
            while(!queue.empty()){
                if(work<5){work=0;return {};}work-=5;
                const auto [old,s]=queue.top();queue.pop();if(old!=result.cost[s])continue;
                const TilePoint from{s%t.width,s/t.width};
                for(auto direction:ENTRY_DIRECTIONS){const auto delta=entryStep(direction);const TilePoint p{from.x+delta.dx,from.y+delta.dy};
                    if(!valid(p))continue;const auto n=cell(p),w=grid.cost(p.x,p.y,direction),who=occupied[n];
                    if(w<0 || w>limit-old || (!ignoreStock&&!pawn.ignoresCollision&&(who==-2 || (who>=0&&state[who].side!=u.side))))continue;
                    const int next=old+w;if(result.cost[n]>=0 && result.cost[n]<=next)continue;
                    result.cost[n]=next;result.parent[n]=s;queue.emplace(next,n);
                }
            }
            return result;
        }
        int movement(const OpeningLaneState &state,int i,TilePoint to,std::int64_t &work)
        {
            if(to.x<0||to.y<0||to.x>=m_terrain.width||to.y>=m_terrain.height)return UNREACHABLE;
            if(i<0||i>=static_cast<int>(state.size())||state[i].hp<=0)return UNREACHABLE;
            if(state[i].at==to)return 0;
            if(openingLaneOccupant(state,to)>=0 || m_terrain.friendlyBirthSites.contains({to.x,to.y}))return UNREACHABLE;
            if(m_board!=state){m_board=state;m_fields.clear();}
            auto [p,fresh]=m_fields.try_emplace(i);if(fresh)p->second=build(state,i,false,work);
            const auto &v=p->second.cost;return v.empty()?UNREACHABLE:v[to.y*m_terrain.width+to.x];
        }
    private:
        const CombatTerrain &m_terrain;
        const OpeningLaneHandoff &m_opening;
        OpeningLaneState m_board;
        std::map<int,Field> m_fields;
    };

    inline auto openingLaneNativeExchange(const ProductionForecastBoard &budgetBoard)
    {
        const auto real=budgetBoard.contactDamage;
        return [real](const OpeningLaneState &state,const OpeningLaneShot &a,std::int64_t &budget)->std::optional<OpeningLaneExchange>{
            const auto &p=state[a.shooter],&v=state[a.target];const NativeContactPiece x{a.shooter,p.side,{},a.from,p.hp,p.rounds},y{a.target,v.side,{},v.at,v.hp,v.rounds};
            if(!real)return {};const auto q=real(x,y,budget);if(!q)return {};return OpeningLaneExchange{q->attack,q->counter,q->weapon,q->counterWeapon};
        };
    }

    // Caller settles a common pre-E0 roster/bill and validates the cleared line again in the full normal allocator.
    // A positive result adds a candidate. No result establishes the impossibility of another route.
    inline std::optional<ProposedOpeningLane> proposeOpeningLane(
        const CombatTerrain &t,std::span<const KnownUnit> units,const OpeningLaneHandoff &opening,
        const OpeningRecruitTarget &target,std::int64_t &work,bool expandTransitScreens=false)
    {
        const auto &board=opening.afterService;const int n=static_cast<int>(board.size()),reserve=target.shooter;
        if(!opening.paidOpeningKnown || !opening.stockServiceKnown || !opening.datedCombatModifiersKnown ||
           reserve<0 || reserve>=n || units.size()!=board.size() || t.units.size()!=board.size() ||
           opening.movement.size()!=board.size() || !t.production || !t.production->contactDamage || !target.powers ||
           target.factory==INVALID_TILE || target.minRange<1 || target.maxRange<target.minRange ||
           !t.friendlyBirthSites.contains({target.factory.x,target.factory.y}) || board[reserve].hp<=0 || board[reserve].spent)return {};
        if(t.width<=0 || t.height<=0 || t.width>std::numeric_limits<int>::max()/t.height ||
           std::int64_t(t.width)+t.height>std::numeric_limits<int>::max()/2)return {};
        std::set<std::pair<int,int>> occupied=t.friendlyBirthSites;
        for(int i=0;i<n;++i){const auto &p=board[i];if(p.hp<0 || p.hp>100 || p.side!=static_cast<int>(t.units[i].enemy))return {};
            if(p.hp>0 && (!t.units[i].present || p.at.x<0 || p.at.y<0 || p.at.x>=t.width || p.at.y>=t.height || !occupied.emplace(p.at.x,p.at.y).second))return {};
        }
        OpeningLanePaths paths(t,opening);std::map<int,OpeningLanePaths::Field> relaxed;
        const auto field=[&](int i)->const OpeningLanePaths::Field&{auto [it,fresh]=relaxed.try_emplace(i);if(fresh)it->second=paths.build(board,i,true,work);return it->second;};
        const auto cell=[&](TilePoint p){return p.y*t.width+p.x;};
        const auto visitInRange=[&](TilePoint at,int low,int high,auto visit){high=std::min(high,t.width+t.height);for(int y=std::max(0,at.y-high);y<std::min(t.height,at.y+high+1);++y)
            for(int x=std::max(0,at.x-high);x<std::min(t.width,at.x+high+1);++x){if(work<=0)return;--work;const int d=std::abs(x-at.x)+std::abs(y-at.y);if(d>=low&&d<=high)visit(TilePoint{x,y},d);}};
        const auto oracle=openingLaneNativeExchange(*t.production);
        const auto final=[&](const OpeningLaneState &b,int i,TilePoint from,std::int64_t &budget){const auto &u=b[i];const int distance=std::abs(from.x-target.factory.x)+std::abs(from.y-target.factory.y);
            if(u.hp<=0||u.spent||paths.movement(b,i,from,budget)<0)return false;const auto types=target.powers(distance);return types&&usableOpeningWeapon((*types)[0],u.rounds)>=0;};
        std::optional<ProposedOpeningLane> result;const auto &reserveField=field(reserve);if(reserveField.cost.empty())return {};
        visitInRange(target.factory,target.minRange,target.maxRange,[&](TilePoint post,int){
            if(result||reserveField.cost[cell(post)]<0||paths.movement(board,reserve,post,work)>=0||work<=0)return;
            std::vector<int> gates;for(int k=cell(post);k>=0;k=reserveField.parent[k]){const auto who=openingLaneOccupant(board,{k%t.width,k/t.width});if(who>=0&&board[who].side!=board[reserve].side)gates.push_back(who);}
            if(gates.empty())return;
            // One unsampled path or a second gate is still open to other proposals. This first probe prices a single root plus screen-clearing volleys.
            const int guard=gates.front();std::vector<int> screens;std::vector<OpeningLaneShot> options;
            const auto addScreen=[&](int who){
                if(who>=0 && who!=guard && board[who].side!=board[reserve].side &&
                   std::find(screens.begin(),screens.end(),who)==screens.end())screens.push_back(who);
            };
            const auto collect=[&](int victim,bool collectScreens){for(int i=0;i<n;++i){
                if(work<=0)return;if(i==reserve || board[i].side!=board[reserve].side || board[i].hp<=0 || board[i].spent||!units[i].canFire || units[i].minRange<1)continue;
                const auto &reach=field(i);if(reach.cost.empty())continue;
                visitInRange(board[victim].at,units[i].minRange,units[i].maxRange,[&](TilePoint from,int){
                    if(from==board[reserve].at || (!expandTransitScreens && from==post) ||
                       (from!=board[i].at && !units[i].canMoveAndFire) ||
                       reach.cost[cell(from)]<0 || t.friendlyBirthSites.contains({from.x,from.y}))return;
                    OpeningLaneShot a{i,victim,from,0};const auto q=oracle(board,a,work);if(!q||!std::isfinite(q->hit)||q->hit<1||q->hit>10000)return;
                    a.damageHint=static_cast<int>(q->hit);options.push_back(a);if(!collectScreens)return;
                    // A legal assisting shot may require a second gun to clear
                    // a transit tile or firing square before it can be played.
                    // This is nomination only; the replay must pay for every
                    // clearance, actual path, counter and spent firing post.
                    addScreen(openingLaneOccupant(board,from));
                    if(expandTransitScreens && !t.units[i].ignoresCollision)for(int k=cell(from);k>=0;k=reach.parent[k]){
                        if(work<1){work=0;return;}--work;
                        addScreen(openingLaneOccupant(board,{k%t.width,k/t.width}));
                    }
                });
            }};
            collect(guard,true);
            for(const int g:gates)addScreen(g);
            for(std::size_t i=0;i<screens.size() && work>0;++i)collect(screens[i],expandTransitScreens);
            auto move=[&](const OpeningLaneState &b,int i,TilePoint at,std::int64_t &budget){return paths.movement(b,i,at,budget);};
            auto candidate=findOpeningLane(board,reserve,guard,post,screens,options,move,oracle,final,work,96,7);
            if(!candidate.witness.empty())result=ProposedOpeningLane{reserve,guard,post,std::move(screens),std::move(candidate)};
        });return result;
    }
}
