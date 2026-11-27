#pragma once
#include "ai/coordinator/townworkercalendarid.h"

#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/stockroutefrontier.h"
#include <atomic>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace Coordinator
{
    struct StockCaptureStep
    {
        int tick;
        TilePoint tile;
        std::vector<int> cells;
        bool operator==(const StockCaptureStep &) const = default;
    };

    struct StockCapturePath
    {
        int entryTick{-1};
        int fuel{};
        std::vector<StockCaptureStep> steps;
        std::set<int> blockedCells;
        bool operator==(const StockCapturePath &) const = default;
    };

    struct StockCaptureUse
    {
        int cell, first, last, side, actor;
        bool stopped;
    };

    class StockCaptureTraffic
    {
    public:
        StockCaptureTraffic(int width,int height,int lastTick)
            : m_base(std::make_shared<CellGrid>(static_cast<std::size_t>(std::max(0,width*height)))),
              m_cellCount(std::max(0,width*height)), m_last(lastTick) {}

        void reserve(int cell,int first,int last,int side,int actor,bool stopped)
        {
            if (cell < 0 || cell >= m_cellCount || first > last || first > m_last) return;
            mutableCell(cell).push_back({cell,first,std::min(last,m_last),side,actor,stopped});
            changedCalendar();
        }

        bool open(int actor,int side,int cell,int tick,bool stop) const
        {
            if(cell<0 || cell>=m_cellCount) return false;
            const auto &uses=cellUses(cell);
            return std::none_of(uses.begin(),uses.end(),[&](const auto &u) {
                return (actor<0 || u.actor!=actor) && u.first<=tick && tick<=u.last &&
                    (u.side!=side || (u.stopped && stop));
            });
        }

        // End this actor's current stay. A separately booked later visit remains valid.
        void vacate(int actor, int cell, int tick)
        {
            if (actor < 0 || cell < 0 || cell >= m_cellCount) return;
            const auto &readonly = cellUses(cell);
            if (std::none_of(readonly.begin(), readonly.end(), [&](const auto &use) {
                return use.actor == actor && use.stopped && use.first <= tick && use.last >= tick;
            }))
            {
                return;
            }
            auto &uses = mutableCell(cell);
            for (auto &use : uses)
            {
                if (use.actor != actor || !use.stopped || use.first > tick || use.last < tick) continue;
                use.last = tick - 1;
            }
            std::erase_if(uses, [](const auto &use) { return use.last < use.first; });
            changedCalendar();
        }

        // A funded unit can accept a replacement job. Erase only its later
        // commitments, preserving its birth and other actors' paid routes.
        void removeFutureUses(int actor, int firstTick)
        {
            if (actor<0) return;
            materialize();
            bool changed=false;
            for (auto &uses:*m_base)
            {
                for (auto &use:uses) if (use.actor==actor && use.last>=firstTick)
                {use.last=firstTick-1;changed=true;}
                std::erase_if(uses,[](const auto &use){return use.last<use.first;});
            }
            if(changed) changedCalendar();
        }

        int cellCount() const { return m_cellCount; }
        int lastTick() const { return m_last; }
        std::uint64_t generation() const { return m_generation; }
        std::uint64_t calendarId() const { return m_calendarId; }
        void removeUsesWithin(int actor,int first,int last)
        {
            if(actor<0 || first>last)return;
            materialize();
            bool changed=false;
            for(auto &uses:*m_base)
            {
                std::vector<StockCaptureUse> tails;
                for(auto &u:uses)if(u.actor==actor && u.last>=first && u.first<=last)
                {
                    if(u.last>last){auto tail=u;tail.first=last+1;tails.push_back(tail);}
                    u.last=first-1;changed=true;
                }
                std::erase_if(uses,[](const auto &u){return u.last<u.first;});
                uses.insert(uses.end(),tails.begin(),tails.end());
            }
            if(changed)changedCalendar();
        }
        const std::vector<StockCaptureUse>& uses(int cell) const
        {
            if(cell<0 || cell>=m_cellCount) return m_base->at(static_cast<std::size_t>(cell));
            return cellUses(cell);
        }

    private:
        friend struct FrozenTownCalendarBindings;
        using CellGrid = std::vector<std::vector<StockCaptureUse>>;
        static constexpr std::size_t OVERRIDE_FLATTEN_THRESHOLD = 16;
        static constexpr unsigned OVERRIDE_MASK_BITS = 63u;

        static constexpr std::uint64_t overrideBit(int cell)
        {
            return std::uint64_t{1} << (static_cast<unsigned>(cell) & OVERRIDE_MASK_BITS);
        }

        const std::vector<StockCaptureUse>& cellUses(int cell) const
        {
            if((m_overrideMask & overrideBit(cell))!=0)
            {
                for(const auto &entry:m_overrides)
                {
                    if(entry.first==cell) return entry.second;
                }
            }
            return (*m_base)[static_cast<std::size_t>(cell)];
        }

        std::vector<StockCaptureUse>& mutableCell(int cell)
        {
            if(m_overrides.empty() && m_base.use_count()==1)
            {
                return (*m_base)[static_cast<std::size_t>(cell)];
            }
            const std::uint64_t bit=overrideBit(cell);
            if((m_overrideMask & bit)!=0)
            {
                for(auto &entry:m_overrides)
                {
                    if(entry.first==cell) return entry.second;
                }
            }
            if(m_overrides.size()>=OVERRIDE_FLATTEN_THRESHOLD)
            {
                materialize();
                return (*m_base)[static_cast<std::size_t>(cell)];
            }
            m_overrideMask|=bit;
            m_overrides.emplace_back(cell,(*m_base)[static_cast<std::size_t>(cell)]);
            return m_overrides.back().second;
        }

        void materialize()
        {
            if(m_base.use_count()>1)
            {
                m_base=std::make_shared<CellGrid>(*m_base);
            }
            for(auto &[cell,uses]:m_overrides)
            {
                (*m_base)[static_cast<std::size_t>(cell)]=std::move(uses);
            }
            m_overrides.clear();
            m_overrideMask=0;
        }

        std::shared_ptr<CellGrid> m_base;
        std::vector<std::pair<int,std::vector<StockCaptureUse>>> m_overrides;
        std::uint64_t m_overrideMask{0};
        int m_cellCount{0};
        int m_last;
        std::uint64_t m_generation{};
        // Copies inherit the exact reservation history. Each actual write
        // gets a distinct identity, including writes to sibling timelines.
        inline static std::atomic<std::uint64_t> nextCalendarId{1};
        std::uint64_t m_calendarId{TownWorkerCalendarId::allocate(nextCalendarId)};
        void changedCalendar()
        {
            ++m_generation;
            m_calendarId=TownWorkerCalendarId::allocate(nextCalendarId);
        }
    };

    // Minimum directional terrain cost back to this target. It is an admissible
    // search bound even if the terrain has zero-cost steps or later blockers.
    inline std::optional<std::vector<int>> reverseStockRouteCosts(const MobilityCostGrid &grid,int goal,std::int64_t &budget)
    {
        const int width=grid.width();
        const int height=grid.height();
        std::vector<int> costs(static_cast<std::size_t>(width)*height,std::numeric_limits<int>::max());
        using Item=std::pair<int,int>;
        struct HeapScratch { std::vector<Item> heap; bool active{false}; };
        static thread_local HeapScratch scratch;
        std::vector<Item> localHeap;
        const bool useScratch=!scratch.active;
        if(useScratch) scratch.active=true;
        struct Release { HeapScratch &s; bool owned; ~Release(){ if(owned) s.active=false; } } release{scratch,useScratch};
        auto &heap=useScratch?scratch.heap:localHeap;
        heap.clear();
        const auto cmp=std::greater<Item>{};
        costs[goal]=0;
        heap.emplace_back(0,goal);
        while(!heap.empty())
        {
            std::pop_heap(heap.begin(),heap.end(),cmp);
            const auto [current,cell]=heap.back();
            heap.pop_back();
            if(costs[cell]!=current) continue;
            if(budget<=0) return {}; --budget;
            const int cx=cell%width,cy=cell/width;
            for(auto dir:ENTRY_DIRECTIONS)
            {
                const auto delta=entryStep(dir);
                const int x=cx-delta.dx,y=cy-delta.dy;
                if(x<0 || y<0 || x>=width || y>=height) continue;
                const int price=grid.cost(cx,cy,dir),from=y*width+x;
                if(price<0 || price>=costs[from]-current) continue;
                costs[from]=current+price;
                heap.emplace_back(costs[from],from);
                std::push_heap(heap.begin(),heap.end(),cmp);
            }
        }
        return costs;
    }

    class StockRouteLowerBounds
    {
    public:
        explicit StockRouteLowerBounds(const StockRouteLowerBounds *prepared=nullptr)
            : m_prepared(prepared) {}

        const std::vector<int> *cached(const MobilityCostGrid &grid,int goal) const
        {
            const auto found=m_fields.find({&grid,goal});
            if (found!=m_fields.end()) return &found->second;
            return m_prepared ? m_prepared->cached(grid,goal) : nullptr;
        }

        // Only geometry shared before squad comparison may define its common
        // opportunity menu. A field privately warmed by this reply is excluded.
        const std::vector<int> *preparedCost(const MobilityCostGrid &grid,int goal) const
        {
            return m_prepared ? m_prepared->cached(grid,goal) : nullptr;
        }

        const std::vector<int> *costs(const MobilityCostGrid &grid,int goal,std::int64_t &budget)
        {
            if (const auto *field=cached(grid,goal)) return field;
            auto computed=reverseStockRouteCosts(grid,goal,budget);
            if (!computed) return nullptr;
            return &m_fields.emplace(std::pair{&grid,goal},std::move(*computed)).first->second;
        }

        // Only for an exactly dated physical move without callbacks or a prescribed first stop.
        // Fuel, activation, collision, grid and the complete dated occupancy all determine its answer.
        using DatedRouteKey=std::tuple<const MobilityCostGrid*,int,int,int,int,int,int,int,int,int,bool>;
        template<class Search>
        std::optional<StockCapturePath> datedRoute(const StockCaptureTraffic &traffic,DatedRouteKey key,
            std::int64_t &budget,Search search)
        {
            auto it=std::find_if(m_calendars.begin(),m_calendars.end(),[&](const auto &item){
                return item.identity==traffic.calendarId();
            });
            if(it!=m_calendars.end())
            {
                if(const auto seen=it->routes.find(key);seen!=it->routes.end())
                {
                    if(budget<=0)return {};
                    --budget;++datedHits;return seen->second;
                }
            }
            const auto before=budget;
            auto result=search();
            if(!result || before-budget<=1)return result;
            // No complete calendar is copied. Bound stored path payloads across
            // all active market slots; at capacity the ordinary search still runs.
            std::size_t bytes=256+result->steps.size()*sizeof(StockCaptureStep)+result->blockedCells.size()*64;
            for(const auto &step:result->steps)bytes+=step.cells.size()*sizeof(int);
            if(bytes>datedStorageLeft)return result;
            if(it==m_calendars.end())
            {
                if(m_calendars.size()>=4)return result;
                m_calendars.push_back({traffic.calendarId(),{}});
                it=std::prev(m_calendars.end());
            }
            it->routes.emplace(key,*result);
            datedStorageLeft-=bytes;
            return result;
        }
        int datedHits{};
        std::size_t datedStorageLeft{64*1024};
    private:
        friend struct FrozenTownGeometry;
        struct DatedCalendar
        {
            std::uint64_t identity;
            std::map<DatedRouteKey,StockCapturePath> routes;
        };
        std::vector<DatedCalendar> m_calendars;
        const StockRouteLowerBounds *m_prepared;
        std::map<std::pair<const MobilityCostGrid*,int>,std::vector<int>> m_fields;
    };

    // A turn can end early. Tiles crossed on the same activation only require
    // legal transit; a stop must also survive occupancy on the opposing activation.
    inline std::optional<StockCapturePath> stockCaptureRoute(const MobilityCostGrid &grid,
        const StockCaptureTraffic &traffic,int actor,int side,TilePoint from,TilePoint to,int movement,int fuel,
        int departureTick,int earliestEntryTick,int latestEntryTick,int holdActivations,std::int64_t &budget,
        TilePoint firstStop=INVALID_TILE,StockRouteLowerBounds *cache=nullptr,
        const std::function<bool(int,TilePoint)> &allowStop={},bool ignoresCollision=false,
        const std::function<void(int,int,bool)> &blocked={})
    {
        StockCapturePath result;
        if(movement<=0 || !grid.onGrid(from.x,from.y) || !grid.onGrid(to.x,to.y) || departureTick>latestEntryTick ||
           departureTick%2!=side || earliestEntryTick%2!=side || latestEntryTick%2!=side || holdActivations<1) return result;
        const int width=grid.width(),count=width*grid.height(),start=from.y*width+from.x,goal=to.y*width+to.x;
        StockRouteLowerBounds temporary;
        auto &fields=cache ? *cache : temporary;
        const auto *lower=fields.costs(grid,goal,budget);
        if (!lower) return {};
        const int unlimited=std::numeric_limits<int>::max();
        if(fuel<0) fuel=unlimited;
        // Try a minimum-cost terrain walk before allocating the time-expanded search.
        // It is conclusive only if it achieves the earliest possible activation AND every
        // crossing, overnight stop and capture action survives the current dated calendar.
        // A rejected walk supplies no bound on the other routes.
        const auto directCost = (*lower)[start];
        if (directCost == unlimited || directCost > fuel) return result;
        const auto extraTurns = std::max<std::int64_t>(0,(static_cast<std::int64_t>(directCost)-1)/movement);
        const auto minArrival = std::max<std::int64_t>(earliestEntryTick,departureTick+2*extraTurns);
        if (minArrival > latestEntryTick) return result;
        if (start != goal)
        {
            int cell = start, spent = 0, tick = departureTick;
            std::vector<StockCaptureStep> steps{{tick,from,{}}};
            while (cell != goal && tick <= minArrival)
            {
                if (budget <= 0) return {};
                --budget;
                int next = -1, cost = 0;
                const TilePoint pos{cell%width,cell/width};
                for (const auto dir : ENTRY_DIRECTIONS)
                {
                    const auto delta = entryStep(dir);
                    const int x = pos.x+delta.dx, y = pos.y+delta.dy;
                    const int price = grid.cost(x,y,dir);
                    if (price <= 0 || price > movement || price > (*lower)[cell]) continue;
                    const int dest = width*y+x;
                    if ((*lower)[dest] == (*lower)[cell]-price) { next=dest; cost=price; break; }
                }
                if (next < 0) break;
                if (cost > movement-spent)
                {
                    tick += 2;
                    if (tick > minArrival) break;
                    spent = 0;
                    steps.push_back({tick,pos,{}});
                }
                steps.back().cells.push_back(next);
                steps.back().tile = {next%width,next/width};
                spent += cost;
                cell = next;
            }
            if (cell == goal && tick <= minArrival)
            {
                const int offset = static_cast<int>(minArrival)-tick;
                for (auto &step : steps) step.tick += offset;
                std::vector<StockCaptureStep> dated;
                for (int wait = departureTick; wait < departureTick+offset; wait += 2)
                    dated.push_back({wait,from,{}});
                dated.insert(dated.end(),std::make_move_iterator(steps.begin()),std::make_move_iterator(steps.end()));
                const auto stopAllowed = [&](int at,TilePoint pos) {
                    const int sq = pos.y*width+pos.x;
                    return traffic.open(actor,side,sq,at,true) && traffic.open(actor,side,sq,at+1,true) &&
                        (!allowStop || allowStop(at,pos));
                };
                bool valid = firstStop==INVALID_TILE || dated.front().tile==firstStop;
                for (const auto &step : dated)
                {
                    if (!valid) break;
                    if (budget <= static_cast<std::int64_t>(step.cells.size())) return {};
                    budget -= 1 + static_cast<std::int64_t>(step.cells.size());
                    valid = stopAllowed(step.tick,step.tile) && (ignoresCollision ||
                        std::all_of(step.cells.begin(),step.cells.end(),[&](int sq) {
                            return traffic.open(actor,side,sq,step.tick,false);
                        }));
                }
                for (int turn=1; valid && turn<holdActivations; ++turn)
                    valid = stopAllowed(static_cast<int>(minArrival)+2*turn,to);
                if (valid)
                {
                    result.entryTick = static_cast<int>(minArrival);
                    result.fuel = directCost;
                    result.steps = std::move(dated);
                    return result;
                }
            }
        }
        using namespace StockRouteDetail;
        const auto search=[&](auto &frontier)->std::optional<StockCapturePath>{
            auto insert=[&](int tick,int cell,int spent,int burned,int parent) {
                const int remaining=lower->at(cell);
                if(remaining==unlimited || remaining>fuel-burned) return;
                const auto excess=std::max<std::int64_t>(0,static_cast<std::int64_t>(remaining)-movement+spent);
                const auto arrival=tick+2*((excess+movement-1)/movement);
                if(arrival>latestEntryTick) return;
                const Node node{tick,cell,spent,burned,parent};
                if(frontier.improve(node))frontier.push(node,static_cast<int>(arrival),remaining);
            };
            auto open=[&](int cell,int tick,bool stop) {
                if((!stop && ignoresCollision) || traffic.open(actor,side,cell,tick,stop)) return true;
                result.blockedCells.insert(cell);
                if(blocked) blocked(cell,tick,stop);
                return false;
            };
            insert(departureTick,start,0,0,-1);
            for(int n=frontier.pop();n>=0;n=frontier.pop())
            {
                const auto v=frontier.nodes[n];
                if(!frontier.current(v))continue;
                if(budget<=0) return {}; --budget;
                const TilePoint here{v.cell%width,v.cell/width};
                const bool stop=(v.tick!=departureTick || firstStop==INVALID_TILE || firstStop==here) &&
                    open(v.cell,v.tick,true) && open(v.cell,v.tick+1,true) && (!allowStop || allowStop(v.tick,here));
                if(stop && v.cell==goal && v.tick>=earliestEntryTick)
                {
                    bool held=true;
                    for(int day=1;day<holdActivations && held;++day)
                    {
                        const int tick=v.tick+2*day;
                        held=open(goal,tick,true) && open(goal,tick+1,true) && (!allowStop || allowStop(tick,to));
                    }
                    if(held)
                    {
                        result.entryTick=v.tick;result.fuel=v.fuel;
                        std::vector<int> chain;
                        for(int at=n;at>=0;at=frontier.nodes[at].parent) chain.push_back(at);
                        for(auto it=chain.rbegin();it!=chain.rend();++it)
                        {
                            const auto &z=frontier.nodes[*it];const TilePoint pos{z.cell%width,z.cell/width};
                            if(result.steps.empty() || result.steps.back().tick!=z.tick) result.steps.push_back({z.tick,pos,{}});
                            auto &step=result.steps.back();
                            if(step.tile!=pos) step.cells.push_back(z.cell);
                            step.tile=pos;
                        }
                        return result;
                    }
                }
                if(stop && v.tick+2<=latestEntryTick) insert(v.tick+2,v.cell,0,v.fuel,n);
                for(auto dir:ENTRY_DIRECTIONS)
                {
                    const auto s=entryStep(dir);const int x=here.x+s.dx,y=here.y+s.dy;
                    const int price=grid.cost(x,y,dir);
                    if(price<0 || price>movement-v.spent || price>fuel-v.fuel) continue;
                    const int dest=y*width+x;
                    if(open(dest,v.tick,false)) insert(v.tick,dest,v.spent+price,v.fuel+price,n);
                }
            }
            return result;
        };
        // Nested callbacks may start their own dated search. Only the outer
        // query owns this thread's bounded scratch space.
        static thread_local BucketFrontier workspace;
        if(workspace.begin(count,movement,departureTick,latestEntryTick))
        {
            EndBucketSearch end{workspace};
            return search(workspace);
        }
        SparseFrontier sparse(count,movement);
        return search(sparse);
    }
}
