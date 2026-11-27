#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/fundedintercept.h"
#include "ai/coordinator/observedstockengagement.h"

namespace Coordinator
{
    struct ChangedScreenActor {int actor,first;};
    struct ObservedScreenReview
    {
        bool complete{true},preserved{true};
        QJsonArray checks;
    };

    // Movement-screen evidence records the original actors, release, movement
    // and map, not just the square at which one failed route was blocked.
    // Removing the affected bodies for the whole interval gives every other
    // claimant at least as much access as a real loan, hit, return or death.
    // A possible path under this relaxation still needs the actual branch.
    inline ObservedScreenReview reviewObservedScreens(const CombatTerrain &terrain,const StockCaptureSettlement &settled,
        std::span<const ChangedScreenActor> changed,const StockCaptureTraffic *specific,
        StockRouteLowerBounds &lower,std::int64_t &work)
    {
        ObservedScreenReview out;
        if(!settled.townTraffic){out.complete=false;out.preserved=false;return out;}
        if(settled.townProtectedPosts.empty())return out;
        const auto &original=*settled.townTraffic;
        if(!specific)
        {
            const auto effort=(static_cast<std::int64_t>(changed.size())+1)*original.cellCount();
            if(work<effort){work=0;out.complete=false;out.preserved=false;return out;}
            work-=effort;
        }
        std::optional<StockCaptureTraffic> changedTraffic;
        const auto &actual=[&]() -> const StockCaptureTraffic&{
            if(specific)return *specific;
            changedTraffic.emplace(original);
            for(const auto &c:changed)changedTraffic->removeFutureUses(c.actor,c.first);
            return *changedTraffic;
        }();
        const auto pt=[](TilePoint p){return QJsonArray{p.x,p.y};};
        std::set<int> affected;
        const auto spend=[&](){if(work<=0){out.complete=false;out.preserved=false;return false;}--work;return true;};
        for(int sq:settled.townProtectedPosts)
        {
            if(!spend())return out;
            for(const auto &use:original.uses(sq))
            {
                if(!spend())return out;
                const auto c=std::find_if(changed.begin(),changed.end(),[&](const auto &item){
                    return item.actor==use.actor && item.first<=use.last;});
                if(c==changed.end())continue;
                bool found=false;
                for(int i=0;i<static_cast<int>(settled.screenClaims.size());++i)
                {
                    const auto &claim=settled.screenClaims[i];
                    if(!claim.posts.contains(sq))continue;
                    found=true;
                    if(use.side==claim.side && !use.stopped)continue; // Same-side transit is legal.
                    auto dates=settled.townPostDependencies.find(sq);
                    bool recorded=false,needs=false;
                    if(dates!=settled.townPostDependencies.end())for(const auto &d:dates->second)
                    {
                        if(!spend())return out;
                        if(d.claim!=i)continue;
                        recorded=true;
                        if(d.holder!=NO_UNIT && d.holder!=use.actor)continue;
                        const int first=std::max({use.first,c->first,claim.departure});
                        const int last=std::min(use.last,claim.lastTick);
                        const int lo=d.occupiedTick<0?first:std::max(first,d.occupiedTick);
                        const int hi=d.occupiedTick<0?last:std::min(last,d.occupiedTick);
                        for(int at=lo;at<=hi;++at)
                        {
                            if(!spend())return out;
                            // A same-side stop only forbids another stop. A
                            // hostile blocker can forbid transit as well.
                            const bool stop=d.occupiedTick<0?use.side==claim.side:d.stopping;
                            if(actual.open(claim.actor,claim.side,sq,at,stop)){needs=true;break;}
                        }
                    }
                    if(needs || !recorded)affected.insert(i);
                }
                if(!found)
                {
                    out.complete=false;out.preserved=false;
                    out.checks.append(QJsonObject{{"status","UNKNOWN_PRIOR_SCREEN_CLAIM"},{"tile",pt({sq%terrain.width,sq/terrain.width})}});
                    return out;
                }
            }
        }
        for(int index:affected)
        {
            const auto &claim=settled.screenClaims[index];const int i=claim.actor;
            auto row=QJsonObject{{"from",pt(claim.from)},{"property",pt(claim.target)},{"first",claim.departure},
                {"latestEntry",claim.latest},{"holdActivations",claim.hold},{"unlimitedFuelUpper",true},
                {"perActivationUpper",claim.movement==std::numeric_limits<int>::max()?QJsonValue("UNLIMITED_IF_RESUPPLIED"):QJsonValue(claim.movement)},
                {"scope","original on-foot movement on the retained nonparticipant calendar"},
                {"calendar",specific?"ACTUAL_CHANGED_STOPS":"CUMULATIVE_REMOVAL"}};
            if(i<0 || i>=static_cast<int>(terrain.units.size()) || claim.grid<0 || claim.grid>=static_cast<int>(terrain.grids.size()) ||
               std::any_of(changed.begin(),changed.end(),[&](const auto &a){return a.actor==i;}))
            {row["status"]="CLAIMANT_STATE_REQUIRES_REPRICING";out.complete=false;out.preserved=false;out.checks.append(row);continue;}
            row["unit"]=terrain.units[i].engineUnitId;
            const auto route=stockCaptureRoute(terrain.grids[claim.grid],actual,i,claim.side,claim.from,claim.target,claim.movement,-1,
                claim.departure,claim.departure,claim.latest,claim.hold,work,INVALID_TILE,&lower,{},claim.ignoresCollision);
            if(!route)
            {row["status"]="SCREEN_RECHECK_WORK_LIMIT";out.complete=false;out.preserved=false;out.checks.append(row);return out;}
            if(route->entryTick<0)row["status"]="ORIGINAL_CAPTURE_STILL_UNREACHABLE";
            else
            {
                out.preserved=false;row["status"]=specific?"NEW_PHYSICAL_ROUTE_REQUIRES_VALUATION":"POSSIBLE_ROUTE_RECHECK_ACTUAL_STOPS";
                row["earliestPhysicalStop"]=route->entryTick;
                QJsonArray path;for(const auto &s:route->steps)path.append(QJsonObject{{"tick",s.tick},{"at",pt(s.tile)}});
                row["stops"]=path;
            }
            out.checks.append(row);
        }
        return out;
    }

    inline QJsonObject reviewPreservedCapPost(const CombatTerrain &terrain,const StockCaptureSettlement &settled,
        std::span<const ChangedScreenActor> subjects,const FundedInterceptPath &loan,const ObservedStockLine &fight,
        int cap,int paid,int side,int cutoff,StockRouteLowerBounds &lower,std::int64_t &work)
    {
        auto changed=*settled.townTraffic;changed.removeFutureUses(paid,loan.departure);
        const int death=fight.reserveAfterFirst<=0?loan.shot:fight.reserveHp<=0?loan.shot+1:cutoff+1;
        const auto cell=[&](TilePoint p){return p.y*terrain.width+p.x;};
        for(const auto &s:loan.steps)
        {
            if(s.tick>death)break;
            for(int cross:s.cells)changed.reserve(cross,s.tick,s.tick,side,paid,false);
            changed.reserve(cell(s.tile),s.tick,std::min(s.tick+1,death-1),side,paid,true);
        }
        if(death>cutoff)changed.reserve(cell(loan.steps.back().tile),loan.back,cutoff,side,paid,true);
        if(fight.capperHp<=0)changed.removeFutureUses(cap,fight.capperAfterFirst<=0?loan.shot:loan.shot+1);
        const auto check=reviewObservedScreens(terrain,settled,subjects,&changed,lower,work);
        return {{"complete",check.complete},{"preserved",check.preserved},{"checks",check.checks}};
    }
}
