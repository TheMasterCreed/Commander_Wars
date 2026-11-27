#pragma once

#include "ai/coordinator/stockgeometryfork.h"
#include "ai/coordinator/townmarketfork.h"
#include "ai/coordinator/townworkeridentity.h"
#include "ai/coordinator/townmarketresultcheck.h"
#include "ai/coordinator/townframecheck.h"
#include "ai/coordinator/townworkermarket.h"
#include <QCborValue>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace Coordinator
{
    struct TownMarketPrefetchHint
    {
        std::int64_t townEffort{1};
        std::int64_t armBudget{-1};
        std::optional<std::int64_t> privateTownQuoteCap;
        std::size_t requiredChallengeCount{};
        bool hasHint{};
    };

    // Predict only another execution of the last observed operation. Completion
    // in a worker gives this object no authority over plans, native orders or work.
    class TownMarketPrefetch final : public TownMarketOwnerHooks
    {
        static constexpr std::int64_t TOWN_CAP_AREA_WEIGHT=4;
        static constexpr int TOWN_QUOTA_DIVISOR=16;
        static constexpr std::size_t MAX_PREFETCH_PACKETS=12;
        using Clock=std::chrono::steady_clock;
        using GridMap=FrozenTownGeometry::GridMap;
        struct Key {
            QByteArray raw,originalRecords;
            std::vector<std::array<std::int64_t,8>> required;
            std::vector<int> branch;
            bool ownerUpper{},diagnostic{},ordered{},completeProduction{},hasScreenOrders{};
            std::int64_t comparisonWork{},offered{};
            bool operator==(const Key &) const=default;
        };
        struct Packet {
            std::shared_ptr<const int> token;
            std::shared_ptr<const TownMarketFrame::Discovery> discovery;
            std::uint64_t expectedRevision{};
            std::size_t requiredChallengeCount{};
            Key key;
            std::unique_ptr<FrozenTownMarketRequest> input;
            FrozenTownGeometry initial;
        };
        struct Answer {
            TownWorkerResult quote;
            TownMarketFrame frame;
            int completed{},resumed{};
            std::uint64_t allocatedCalendars{};
        };
        struct Ticket {
            std::shared_ptr<Packet> packet;
            std::future<Answer> done;
        };
        class Pool {
            std::mutex lock;
            std::condition_variable wake;
            std::deque<std::packaged_task<Answer()>> todo;
            bool closing{};
            std::vector<std::jthread> workers;
            void loop() {
                for(;;) {
                    std::packaged_task<Answer()> task;
                    {
                        std::unique_lock guard(lock);
                        wake.wait(guard,[&]{return closing || !todo.empty();});
                        if(closing && todo.empty())return;
                        task=std::move(todo.front());todo.pop_front();
                    }
                    task();
                }
            }
            void stop() {
                {std::lock_guard guard(lock);closing=true;}
                wake.notify_all();
            }
        public:
            explicit Pool(int count) {
                try{for(int i=0;i<count;++i)workers.emplace_back([this]{loop();});}
                catch(...){stop();throw;}
            }
            ~Pool(){stop();}
            std::future<Answer> submit(std::packaged_task<Answer()> task) {
                auto result=task.get_future();
                {std::lock_guard guard(lock);todo.push_back(std::move(task));}
                wake.notify_one();
                return result;
            }
        };

        bool configured=qgetenv("AMBER_MARKET_PREFETCH")!="0" &&
            !qEnvironmentVariableIsSet("COW_COORDINATED_AUDIT_TOWN_REPORTS") &&
            !qEnvironmentVariableIsSet("COW_COORDINATED_EAGER_TOWN_REPORTS");
        bool recording{};
        bool shadow=std::getenv("AMBER_MARKET_PREFETCH_SHADOW")!=nullptr;
        struct Audit {
            Answer answer;
            int completedBefore,resumedBefore;
            std::uint64_t firstCalendar;
        };
        std::optional<Audit> audit;
        int lastTrial{-1};
        std::size_t activeRequiredChallengeCount{};
        TownMarketHistory *currentHistory{};
        std::unique_ptr<Pool> pool;
        std::vector<std::shared_ptr<Packet>> previous,current;
        std::vector<Ticket> tickets;
        std::map<QString,std::int64_t> outcomes;
        std::uint64_t memoryMisses{};
        std::chrono::nanoseconds snapshotTime{},waitTime{},deepCheckTime{};

        static Key keyOf(const TownMarketOwnerView &v) {
            Key out;
            out.originalRecords=v.originalRecords;
            out.offered=v.offered;out.comparisonWork=v.terrain.comparisonWork;
            out.ownerUpper=v.recordOwnerUpper;out.diagnostic=v.screen.diagnostic;
            out.ordered=v.terrain.orderedOpening;out.completeProduction=v.terrain.completeProductionMenu;
            out.hasScreenOrders=v.screen.orders!=nullptr;
            out.branch.assign(v.branch.begin(),v.branch.end());
            for(const auto &q:v.required)
                out.required.push_back({q.tile.x,q.tile.y,q.attackingSide,q.capperUnit,q.entryTick,q.friendlyExit.x,q.friendlyExit.y,q.decisionWork});
            // The ordinary physical frame intentionally strips display-only route
            // fields. Its immutable inputs plus the complete original records
            // below must both agree before we can return a publishable response.
            QJsonArray extra{v.orders,v.settled.proofs,v.settled.paidCaptureChecks,v.production.orders,
                v.production.paidFounderWindows,v.production.alliedCaptureVacancy};
            if(v.screen.orders)extra.append(*v.screen.orders);
            QJsonArray defenders;
            for(const auto &u:v.terrain.observedDefenders)defenders.append(QJsonArray{
                u.knowledgeIndex,u.engineUnitId,u.tile.x,u.tile.y,qint64(u.cost),u.stockRow,QString::number(quintptr(u.facts.get()),16)});
            extra.append(defenders);
            out.raw=QCborValue::fromJsonValue(extra).toCbor();
            return out;
        }
        static GridMap identityGrids(const FrozenTownMarketRequest &r) {
            GridMap map;
            for(const auto &[source,dest]:r.gridMap)map.emplace(dest,dest);
            return map;
        }
        static bool currentGridsMatch(const CombatTerrain &t,const FrozenTownMarketRequest &r) {
            if(t.grids.size()!=r.terrain.grids.size())return false;
            for(std::size_t i=0;i<t.grids.size();++i) {
                const auto &a=t.grids[i],&b=r.terrain.grids[i];
                const auto stored=r.gridMap.find(&a);
                if(stored==r.gridMap.end() || stored->second!=&b || a.width()!=b.width() || a.height()!=b.height())return false;
                for(int y=0;y<a.height();++y)for(int x=0;x<a.width();++x)
                    for(auto dir:ENTRY_DIRECTIONS)if(a.cost(x,y,dir)!=b.cost(x,y,dir))return false;
            }
            return true;
        }
        static TownMarketFrame *liveFrame(TownMarketHistory &h,const std::shared_ptr<const int> &token) {
            for(auto *s:{&h.reference,&h.challenger})if(*s)
                for(auto &r:(*s)->frames)if(r.frame.world==token)return &r.frame;
            return nullptr;
        }
        void rememberInput(const TownMarketOwnerView &v) {
            if(!recording || currentHistory!=&v.history || !v.settled.townTraffic || v.focus || !v.discovery ||
               !FrozenTownCalendarBindings::canFork())return;
            auto at=std::ranges::find(current,v.frame.world,&Packet::token);
            // Bounded physical retention only. Every omitted job takes the
            // same ordinary path and receives its full original work grant.
            if(at==current.end() && current.size()>=MAX_PREFETCH_PACKETS)return;
            auto start=Clock::now();
            auto meter=qScopeGuard([&]{snapshotTime+=Clock::now()-start;});
            auto r=freezeTownMarketForWorker(v.terrain,v.units,v.production,v.orders,v.settled,v.properties,v.positions,
                v.hp,v.fuel,v.ammo,v.freeDay,v.casualty,v.guard,v.home,v.away,v.horizon,v.cutoff,v.threshold,v.cash,
                v.focus,v.required,v.branch,v.recordOwnerUpper,v.screen);
            if(!r){++outcomes["capture-unavailable"];return;}
            auto lower=FrozenTownGeometry::copy(v.lower,r->gridMap);
            if(!lower){++outcomes["unowned-geometry"];return;}
            auto packet=std::make_shared<Packet>();
            packet->token=v.frame.world;packet->expectedRevision=v.frame.physicalRevision;
            packet->requiredChallengeCount=activeRequiredChallengeCount;
            packet->discovery=v.discovery;packet->key=keyOf(v);packet->initial=std::move(*lower);
            FrozenTownCalendarBindings::markIncoming(*r->settled.townTraffic);
            packet->input=std::move(r);
            if(at==current.end())current.push_back(std::move(packet));else *at=std::move(packet);
            ++outcomes["captured"];
        }
        void remember(const TownMarketOwnerView &v) noexcept {
            try{rememberInput(v);}
            catch(const std::bad_alloc&){++memoryMisses;}
        }
        void endPair() {
            // Do not let a later pair modify any input that still has a reader.
            for(auto &ticket:tickets)if(ticket.done.valid())ticket.done.wait();
            tickets.clear();currentHistory=nullptr;
            previous=std::move(current);current.clear();recording=false;
        }
        void dispatch(std::shared_ptr<Packet> p,TownMarketHistory &h,const CombatTerrain &terrain,
                      const TownMarketPrefetchHint &hint) {
            auto *real=liveFrame(h,p->token);
            if(!real || real->physicalRevision!=p->expectedRevision){++outcomes["frame-advanced"];return;}
            if(hint.hasHint) {
                if(p->requiredChallengeCount!=hint.requiredChallengeCount){++outcomes["skipped-required-changed"];return;}
                const auto area=static_cast<std::int64_t>(terrain.width)*terrain.height;
                const auto unitCount=static_cast<std::int64_t>(p->input->hp.size());
                auto townCap=p->input->horizon*(TOWN_CAP_AREA_WEIGHT*area+unitCount*unitCount);
                if(hint.townEffort>1 && terrain.comparisonWork>0) {
                    const auto maximum=terrain.comparisonWork/TOWN_QUOTA_DIVISOR;
                    townCap=townCap>maximum/hint.townEffort?maximum:townCap*hint.townEffort;
                }
                if(hint.privateTownQuoteCap)townCap=*hint.privateTownQuoteCap;
                const auto quotaRemaining=hint.armBudget>=0?hint.armBudget:p->key.offered*TOWN_QUOTA_DIVISOR;
                const auto predictedOffered=terrain.quoteWorkOffer(quotaRemaining,TOWN_QUOTA_DIVISOR,townCap);
                if(hint.armBudget>=0 && hint.armBudget<predictedOffered){++outcomes["skipped-budget-floor"];return;}
                if(predictedOffered!=p->key.offered || terrain.comparisonWork!=p->key.comparisonWork) {
                    const auto oldPriorityWork=p->key.offered/2;
                    const auto newPriorityWork=predictedOffered/2;
                    p->input->terrain.comparisonWork=terrain.comparisonWork;
                    p->key.comparisonWork=terrain.comparisonWork;
                    p->key.offered=predictedOffered;
                    if(oldPriorityWork>0 && newPriorityWork!=oldPriorityWork) {
                        for(std::size_t i=0;i<p->input->required.size();++i) {
                            if(p->input->required[i].decisionWork==oldPriorityWork) {
                                p->input->required[i].decisionWork=newPriorityWork;
                                if(i<p->key.required.size())p->key.required[i][7]=newPriorityWork;
                            }
                        }
                    }
                }
            }
            if(!refreshFrozenTownFacts(*p->input,terrain)){++outcomes["rules-unavailable"];return;}
            auto privateFrame=forkOwnedTownFrame(*real);
            if(!privateFrame){++outcomes["external-cursor"];return;}
            auto remap=identityGrids(*p->input);
            auto lower=FrozenTownGeometry::copy(*p->initial.chain.back(),remap);
            if(!lower){++outcomes["private-geometry"];return;}
            auto task=[p,frame=std::move(*privateFrame),geometry=std::move(*lower),remap=std::move(remap)]() mutable {
                std::uint64_t allocations=0;
                TownWorkerCalendarId::Scope localIds(allocations);
                TownMarketHistory scratch;
                scratch.enabled=true;
                DetachedTownFrame detached{&frame,p->discovery};
                auto result=runFrozenTownMarket(*p->input,*geometry.chain.back(),scratch,detached,p->key.offered);
                if(result.price && (result.screenRequest || scratch.visits || scratch.aliasCompletedReads || scratch.priorityEvictions ||
                    !p->initial.sameInput(*geometry.chain.back(),remap))) {
                    result.price.reset();result.gap="EXTERNAL_MARKET_STATE_CHANGED";
                }
                return Answer{std::move(result),std::move(frame),scratch.completedReads,scratch.resumedReads,allocations};
            };
            if(!pool)pool=std::make_unique<Pool>(std::max(1,std::min(4,QThread::idealThreadCount()-1)));
            tickets.push_back({p,pool->submit(std::packaged_task<Answer()>(std::move(task)))});
            ++outcomes["dispatched"];
        }
    public:
        bool enabled() const{return configured;}
        bool tracking(const TownMarketHistory &h) const override{return recording && currentHistory==&h;}
        auto enterPair(int trial,TownMarketHistory &h,const CombatTerrain &terrain,
                       const TownMarketPrefetchHint &hint={}) {
            if(configured) {
                currentHistory=&h;
                activeRequiredChallengeCount=hint.hasHint?hint.requiredChallengeCount:0;
                recording=trial==lastTrial;
                if(recording)try{
                    tickets.reserve(previous.size());
                    for(auto &p:previous)dispatch(p,h,terrain,hint);
                }catch(const std::bad_alloc&){
                    ++memoryMisses;
                }catch(const std::system_error&){
                    // Already submitted jobs remain owned. Other requests follow
                    // the ordinary path, even if the OS cannot start a worker.
                }
                lastTrial=trial;
            }
            return qScopeGuard([this]{if(configured)endPair();});
        }
        ~TownMarketPrefetch() override {
            if(!configured)return;
            if(const char *path=std::getenv("AMBER_PREFETCH_REPORT"))if(auto file=std::fopen(path,"ab"))try{
                const auto closeFile=qScopeGuard([&]{std::fclose(file);});
                QJsonObject fields;
                for(auto &[name,value]:outcomes)fields[name]=qint64(value);
                fields["optionalMemoryMisses"]=qint64(memoryMisses);
                fields["deepFrameNanos"]=qint64(deepCheckTime.count());fields["snapshotNanos"]=qint64(snapshotTime.count());fields["waitNanos"]=qint64(waitTime.count());
                auto line=QJsonDocument(fields).toJson(QJsonDocument::Compact);
                std::fwrite(line.constData(),1,line.size(),file);std::fputc('\n',file);
            }catch(const std::bad_alloc&){}
        }
        void completed(const TownMarketOwnerView &v,std::int64_t remaining) override {
            if(audit){
                auto &a=audit->answer;
                if(remaining!=a.quote.remaining || v.history.completedReads-audit->completedBefore!=a.completed ||
                    v.history.resumedReads-audit->resumedBefore!=a.resumed || v.frame.retainedBytes!=a.frame.retainedBytes ||
                    v.frame.opportunities.size()!=a.frame.opportunities.size())++outcomes["audit-work-or-frame-mismatch"];
                if(FrozenTownCalendarBindings::liveCount()-audit->firstCalendar!=a.allocatedCalendars)++outcomes["audit-calendar-mismatch"];
                if(std::getenv("AMBER_PREFETCH_DEEP_FRAME")){
                    const auto started=Clock::now();
                    const auto measure=qScopeGuard([&]{deepCheckTime+=Clock::now()-started;});
                    if(!v.settled.townTraffic || FrozenTownCalendarBindings::liveCount()-audit->firstCalendar!=a.allocatedCalendars)
                        ++outcomes["audit-deep-unrebindable"];
                    else try{
                        // The ordinary arm has already allocated these real IDs.
                        // Convert the private fork only, without reserving again.
                        FrozenTownCalendarBindings::rebind(a.frame,audit->firstCalendar,v.settled.townTraffic->calendarId());
                        a.frame.physicalRevision=v.frame.physicalRevision;
                        const auto check=TownFrameCheck::compareRebasedTownFrames(v.frame,a.frame);
                        if(check.reason.isEmpty())++outcomes["audit-deep-frames-equal"];
                        else {
                            ++outcomes["audit-deep-frames-different"];
                            if(outcomes["audit-deep-frames-different"]<=4){
                                QString detail=QStringLiteral("audit-deep-")+check.reason;
                                for(int i:check.path){detail+=QLatin1Char('/');detail+=QString::number(i);}
                                ++outcomes[detail];
                            }
                        }
                    }catch(const std::bad_alloc&){++outcomes["audit-deep-unverified-memory"];}
                }
            }
            remember(v);
        }
        void published(const PricedTownEncounter &actual,std::int64_t remaining) override {
            if(!audit)return;
            const auto &q=audit->answer.quote;
            auto field=differenceInTownResult(actual,*q.price);
            if(remaining!=q.remaining)field="remaining";
            if(field.isEmpty())++outcomes["audit-matched"];
            else ++outcomes["audit-mismatch-"+field];
            audit.reset();
        }
        std::optional<PricedTownEncounter> consume(TownMarketOwnerView &v,std::int64_t &remaining) override {
            try{return consumeReady(v,remaining);}
            catch(const std::bad_alloc&){++memoryMisses;return {};}
        }
        std::optional<PricedTownEncounter> consumeReady(TownMarketOwnerView &v,std::int64_t &remaining) {
            if(currentHistory!=&v.history)return {};
            auto found=std::ranges::find_if(tickets,[&](const Ticket &t){return t.done.valid() && t.packet->token==v.frame.world;});
            if(found==tickets.end())return {};
            auto p=found->packet;
            if(v.previousRevision!=p->expectedRevision || remaining!=p->key.offered || !v.settled.townTraffic ||
               !v.discovery || *v.discovery!=*p->discovery || keyOf(v)!=p->key){++outcomes["request-miss"];
                if(shadow){
                    if(v.previousRevision!=p->expectedRevision)++outcomes["request-revision"];
                    else if(remaining!=p->key.offered)++outcomes["request-grant"];
                    else if(!v.settled.townTraffic || !v.discovery || *v.discovery!=*p->discovery)++outcomes["request-discovery"];
                    else ++outcomes["request-input"];
                }
                return {};}
            const auto actualCalendar=v.settled.townTraffic->calendarId();
            if(!currentGridsMatch(v.terrain,*p->input) || !p->initial.sameInput(v.lower,p->input->gridMap) ||
               p->initial.containsCalendar(actualCalendar) || FrozenTownCalendarBindings::contains(v.frame,actualCalendar)){
                ++outcomes["geometry-or-calendar-miss"];return {};
            }
            std::optional<Answer> result;
            auto start=Clock::now();
            try{result.emplace(found->done.get());}
            catch(const std::exception&){++outcomes["worker-exception"];return {};}
            waitTime+=Clock::now()-start;
            auto &a=*result;
            if(!a.quote.price){++outcomes["worker-gap-"+a.quote.gap];return {};}
            if(a.quote.remaining<0 || a.quote.remaining>remaining || !validateFrozenTownReads(*p->input,v.terrain)){
                ++outcomes["rule-or-work-miss"];return {};
            }
            if(!FrozenTownCalendarBindings::canRebind(a.frame,a.allocatedCalendars)){++outcomes["unknown-worker-calendar"];return {};}
            if(shadow){
                audit.emplace(Audit{std::move(a),v.history.completedReads,v.history.resumedReads,FrozenTownCalendarBindings::liveCount()});
                return {};
            }
            // All mutations of canonical state occur here, at the ordinary
            // serial call site, after every eligibility check has succeeded.
            auto &used=outcomes["used"];
            static_assert(std::is_nothrow_move_assignable_v<TownMarketFrame>);
            static_assert(std::is_nothrow_move_constructible_v<PricedTownEncounter>);
            auto firstCalendar=FrozenTownCalendarBindings::reserve(a.allocatedCalendars);
            if(!firstCalendar){++outcomes["calendar-space"];return {};}
            FrozenTownCalendarBindings::rebind(a.frame,*firstCalendar,actualCalendar);
            a.frame.physicalRevision=v.frame.physicalRevision;
            v.frame=std::move(a.frame);
            remaining=a.quote.remaining;
            v.history.completedReads+=a.completed;v.history.resumedReads+=a.resumed;
            ++used;
            remember(v);
            return std::move(a.quote.price);
        }
    };
}
