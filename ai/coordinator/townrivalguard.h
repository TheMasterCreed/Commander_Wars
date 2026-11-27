#pragma once

#include "ai/coordinator/stockneutralrebid.h"
#include "ai/coordinator/townearlytitle.h"
#include "ai/coordinator/townguardsuffix.h"
#include "ai/coordinator/townpaidparticipant.h"
#include "ai/coordinator/townretainedwalk.h"
#include "ai/coordinator/townscreensurvival.h"
#include "ai/coordinator/townguardbookedshot.h"

namespace Coordinator
{
    inline std::vector<int> rivalPrefixKey(const CombatTerrain &terrain,const NeutralOccupancy &rival)
    {
        std::vector<int> key{rival.actor,rival.entry,rival.path.fuel,rival.origin.x,rival.origin.y,rival.departure,rival.initialFuel,int(rival.conditionalDuty)};
        for(const auto &s:rival.path.steps)
        {
            key.push_back(s.tick);key.push_back(s.tile.y*terrain.width+s.tile.x);key.push_back(static_cast<int>(s.cells.size()));
            key.insert(key.end(),s.cells.begin(),s.cells.end());
        }
        return key;
    }

    // This cache belongs to one immutable market board. Departure controls
    // which prior gun duties are released for this guard. Neither a paid
    // stock-shaped temporary board nor a changed reply shares these stops.
    struct TownGuardStops
    {
        using Key=std::array<int,5>;
        std::map<Key,TownAssaultDetail::GuardStopProbe> entries;
        int activeFields{};
        BookedGuardIndex booked;
        std::shared_ptr<const int> world;
        std::uint64_t generation{};
        TownAssaultDetail::GuardStopProbe *find(const std::shared_ptr<const int> &scope,std::uint64_t revision,
            int actor,int departure,const StockCaptureStep &step,int area)
        {
            if(!scope)return nullptr;
            if(world!=scope || generation!=revision)
            {
                entries.clear();activeFields=0;world=scope;generation=revision;
            }
            const Key key{actor,departure,step.tick,step.tile.x,step.tile.y};
            auto it=entries.find(key);
            if(it==entries.end())
            {
                if(entries.size()>=512)return nullptr;
                it=entries.try_emplace(key).first;
            }
            // Each in-progress gun may own two direction fields and their queues.
            // Larger markets still use the ordinary probe if the cache is full.
            const auto maximum=1024*1024/(2048+std::size_t(area)*160);
            auto &probe=it->second;
            probe.retainFields=!probe.fields.empty() || std::size_t(activeFields)<maximum;
            return &probe;
        }
    };

    struct TownRivalPrefix
    {
        TownAssaultProgress capture;
        BookedGuardCursor booked;
        std::optional<TownAssaultAlternatives> latestCapture;
        int capturePass{-1},quietThrough{-1},underFire{-1},holdThrough{-1},holdBlocked{-1};
        QJsonArray firstShots;
        TownGuardVolleyHistory fire;
    };
    struct TownRivalOpenPolicy
    {
        TownAssaultProgress progress;
        std::optional<TownAssaultAlternatives> quote;
        std::optional<TownWalkReview> retainedWalk;
        std::shared_ptr<const int> walkWorld;
        std::uint64_t walkCalendar{};
        bool openUsesRetainedCut{};
        int pass{-1};
    };

    // A rival's earlier arrival gives the defender another choice. Recreate
    // the two boards from the same dated resources before comparing the late
    // challenge. A guard line which changes an earlier commitment still needs
    // that commitment repriced before it can earn the squad a better rank.
    struct TownRivalGuardComparison
    {
        TownAssaultAlternatives open;
        std::optional<TownAssaultAlternatives> guarded,earlierCapture;
        std::optional<TownEarlyTitleResponse> afterEarlyTitle;
        std::optional<TownAssaultLine> guardExchange;
        std::optional<TownAssaultAlternatives> afterGuardExchange;
        QString exchangeStatus;
        QJsonArray competingGuardGunDuties;
        QJsonArray approachShots;
        QJsonObject retainedWalk,bookedApproachShot;
        QString guardStatus;
        int actor{NO_UNIT},arrives{-1},departure{-1},remainingFuel{-1};
        bool workLimited{},openRefutes{},dominatedOnRetainedBoard{},paidGuardUnnecessaryForThisAttack{};
        std::set<int> excludedUnits;
        bool reusedCapture{},earlyTitlePossible{},reusedOpen{},openEvaluated{};
        bool guardProbeAdvanced{},suffixAdvanced{};
        int guardGunsChecked{-1},guardRouteExpansions{},guardPendingTick{-1};
        TilePoint guardPendingTile{INVALID_TILE};
        std::int64_t suffixCredit{};
        int heldResumes{},observedResumes{};
        int verifiedGuardThrough{-1},reusedFireThrough{-1};
    };

    // Bound only the named late attacking choice on the retained board.
    // The paid rival may decline to move, in which case it supplies no new
    // combat target. Its independent earlier jobs are not removed by this proof.
    inline bool retainedLateAttackNeedsNoPaidGuard(const TownAssaultBoard &b,const TownAssaultTarget &target,
        const NeutralOccupancy &rival,const TownAssaultAlternatives &open)
    {
        const int n=static_cast<int>(b.units.size()),k=rival.actor-n,tick=target.occupiedFrom;
        const int side=1-target.defendingSide;
        if(k<0 || k>=static_cast<int>(b.recruits.size()) || !b.recruits[k].unit ||
           rival.originUnknown || rival.entry<0 || rival.entry>=tick || rival.departure<0 ||
           rival.origin==INVALID_TILE || rival.path.steps.empty() || rival.path.steps.front().tick!=rival.departure ||
           rival.path.steps.back().tick!=rival.entry || rival.path.steps.back().tile!=target.property.tile ||
           b.recruits[k].side!=target.defendingSide || !b.recruits[k].unit->captureRate.empty() ||
           target.occupant!=NO_UNIT || target.occupiedThrough!=tick || tick<0 || tick>b.cutoff ||
           tick%2!=side || !b.recruitFree || !b.recruitFree(k,rival.departure,b.cutoff) ||
           open.workLimited || open.unresolved || open.selectsAssault ||
           (open.bestUnanswered && TownAssaultDetail::better(*open.bestUnanswered,open.declineValue,side)))return false;
        const auto origin=paidNeutralOrigin(b.traffic,rival.actor,b.recruits[k],rival.departure);
        if(!origin || origin->from!=rival.origin || origin->fuel!=rival.initialFuel)return false;
        // A late material-only line can clear a nearby gun even when the
        // capper cannot finish in time. The open title search need not have
        // tested that gun once it rejected the capture clock.
        const auto town=target.property.tile;
        for(auto delta:std::array<TilePoint,5>{{{0,0},{0,1},{0,-1},{1,0},{-1,0}}})
        {
            const TilePoint tile{town.x+delta.x,town.y+delta.y};
            if(tile.x<0 || tile.y<0 || tile.x>=b.terrain.width || tile.y>=b.terrain.height)continue;
            for(const auto &use:b.traffic.uses(TownAssaultDetail::cell(b.terrain,tile)))
                if(use.side!=side && use.stopped && use.first<=tick && tick<=use.last)return false;
        }
        // Give every eligible stock capper its maximum ordinary capture rate.
        // A title transfer alone has value only when it changes a payday.
        bool hasCapper=false;
        for(int i=0;i<n;++i)
        {
            const auto &u=b.units[i];const auto &p=b.terrain.units[i];
            if(!p.present || int(p.enemy)!=side || b.hp[i]<=0 || !u.canCapture ||
               (target.requiredCapper!=NO_UNIT && target.requiredCapper!=p.engineUnitId))continue;
            hasCapper=true;
            const int rate=u.captureRate+std::max(0,10-u.hpRounded);
            if(rate<=0)return false;
            const bool alreadyThere=b.positions[i]==target.property.tile || p.tile==target.property.tile ||
                (b.at && tick>0 && b.at(i,tick-1)==target.property.tile);
            const int carry=alreadyThere?std::max(0,b.threshold-1):0;
            const auto finished=tick+2*std::max<std::int64_t>(0,widenedCaptureTurns(carry,rate,b.threshold)-1);
            // The last action can precede the final payday by a half-turn.
            const int lastPayday=2*b.horizon;
            if(finished>=lastPayday)continue;
            TownAssaultLine earliest;earliest.side=side;earliest.finishTick=static_cast<int>(finished);
            for(int t=tick+1;t<=lastPayday;++t)
            {
                if(assaultPaydayChange(earliest,target.property,t)!=0)return false;
            }
        }
        return hasCapper;
    }

    inline TownRivalGuardComparison compareTownRivalGuard(const TownAssaultBoard &board,
        const TownAssaultTarget &target,const NeutralOccupancy &rival,StockRouteLowerBounds &bounds,
        const TownAssaultValue &value,std::int64_t &budget,TownRivalPrefix *prefix=nullptr,
        int pass=0,std::int64_t *prefixFunds=nullptr,TownRivalOpenPolicy *openPolicy=nullptr,std::int64_t *openFunds=nullptr,
        const std::function<TownEarlyTitleResponse(const TownAssaultAlternatives&,std::int64_t&)> &retake={},
        const std::shared_ptr<const int> &world={},TownRivalSuffixes *suffixes=nullptr,bool physicalPaidRival=false,
        const TownWalkOpening *opening=nullptr,TownGuardStops *sharedStops=nullptr)
    {
        using namespace TownAssaultDetail;
        TownRivalGuardComparison out;
        out.actor=rival.actor;out.arrives=rival.entry;
        // Only the physical impossibility of *reaching* the proposed date on
        // an empty native map is independent of what other duties the rival
        // takes. A skipped actor or a rejected attack is no such certificate.
        const int attackSide=1-target.defendingSide;
        bool hasCapper=false,canArrive=false;
        const int tick=target.occupiedFrom;
        out.earlyTitlePossible=rival.actor>=0 && rival.actor<static_cast<int>(board.units.size()) &&
            !rival.originUnknown && rival.entry>=0 && rival.entry<tick && board.units[rival.actor].canCapture &&
            target.property.ownerBefore==OwnerSign::Neutral;
        const int paidIndex=rival.actor-static_cast<int>(board.units.size());
        const bool paidCapper=paidIndex>=0 && paidIndex<static_cast<int>(board.recruits.size()) &&
            board.recruits[paidIndex].unit && board.recruits[paidIndex].unit->captureRate.contains(cell(board.terrain,target.property.tile)) &&
            target.property.ownerBefore==OwnerSign::Neutral && rival.entry>=0 && rival.entry<tick;
        out.earlyTitlePossible|=paidCapper;
        if(!paidCapper && !physicalPaidRival && target.requiredCapper!=NO_UNIT && tick==target.occupiedThrough && tick%2==attackSide)
        {
            for(int i=0;i<static_cast<int>(board.units.size());++i)
            {
                const auto &p=board.terrain.units[i];
                const auto &r=board.rights[i];
                if(int(p.enemy)!=attackSide || (target.requiredCapper!=NO_UNIT && target.requiredCapper!=p.engineUnitId) ||
                   board.hp[i]<=0 || !board.units[i].canCapture || board.positions[i]==INVALID_TILE)continue;
                if(budget<=0){canArrive=true;break;}
                --budget;hasCapper=true;
                // An actor already promised elsewhere is a different economic
                // option. Leave it for the comparison with that job withdrawn.
                if(r.readyTick<0 || r.readyTick>tick || r.lastFreeTick<tick){canArrive=true;break;}
                if(board.positions[i]==target.property.tile || p.tile==target.property.tile){canArrive=true;break;}
                const int index=p.grid>=0?p.grid:p.travelGrid;
                if(index<0){canArrive=true;break;}
                const auto &grid=board.terrain.grids[index];
                const auto *field=bounds.costs(grid,cell(board.terrain,target.property.tile),budget);
                if(!field || !grid.onGrid(p.tile.x,p.tile.y)){canArrive=true;break;}
                const auto cost=std::min((*field)[cell(board.terrain,board.positions[i])],(*field)[cell(board.terrain,p.tile)]);
                const auto maxCost=static_cast<long long>(1+(tick-attackSide)/2)*p.movement;
                if(cost!=std::numeric_limits<int>::max() && cost<=maxCost)
                {canArrive=true;break;}
                out.excludedUnits.insert(p.engineUnitId);
            }
            if(hasCapper && !canArrive)
            {
                TownAssaultLine kept;kept.side=attackSide;kept.town=target.property.tile;
                if(const auto cash=value(kept)){out.open.declineValue=*cash;out.openEvaluated=true;}else out.open.unresolved=true;
                out.openRefutes=true;out.guardStatus="NAMED_STOCK_CAPPER_OUT_OF_RANGE_WITHOUT_GUARD";return out;
            }
        }
        bool walkBlocked=false,closedWhileRetained=false;
        if(opening && !physicalPaidRival && !paidCapper && !out.earlyTitlePossible && target.requiredCapper!=NO_UNIT &&
           tick==target.occupiedThrough && target.property.ownerBefore==(attackSide?OwnerSign::Ours:OwnerSign::Enemy))
        {
            auto &pool=openFunds?*openFunds:budget;
            TownWalkReview local;
            const auto *review=&local;
            if(openPolicy && world && openPolicy->walkWorld==world && openPolicy->walkCalendar==board.traffic.calendarId() &&
               openPolicy->retainedWalk && openPolicy->retainedWalk->arrival==tick &&
               openPolicy->retainedWalk->target==target.property.tile &&
               board.terrain.units[openPolicy->retainedWalk->actor].engineUnitId==target.requiredCapper && pool>0)
            {--pool;review=&*openPolicy->retainedWalk;}
            else if(pool>0)
            {
                auto offer=pool/3;const auto start=offer;
                local=retainedTownWalk(board,target,*opening,bounds,offer);pool-=start-offer;
                if(openPolicy && world && local.inspected && (local.finished || !local.cuts.empty()))
                {
                    openPolicy->retainedWalk=local;openPolicy->walkWorld=world;
                    openPolicy->walkCalendar=board.traffic.calendarId();
                }
            }
            if(!review->cuts.empty())
            {
                out.retainedWalk=review->record(board.terrain);
                walkBlocked=review->rulesCertified;
                if(noOtherAttackOnTownWalk(board,*review,pool))
                {
                    TownAssaultLine kept;kept.side=attackSide;kept.town=target.property.tile;
                    if(const auto cash=value(kept))
                    {
                        out.open.declineValue=*cash;out.openEvaluated=true;
                        out.open.checkedCappers.insert(review->actor);out.dominatedOnRetainedBoard=true;
                        out.guardStatus="NAMED_UNARMED_WALK_BLOCKED_ON_RETAINED_BOARD";return out;
                    }
                }
                if(!board.terrain.screenFire && board.terrain.screenDemand && opening->screen.orders &&
                   review->rulesCertified && review->departure==2+review->side &&
                   opening->screen.positions.size()==board.units.size() &&
                   opening->screen.positions[review->actor]==review->origin)
                {
                    for(const auto &cut:review->cuts)if(opening->screen.positions[cut.holder]==cut.post)
                    {
                        board.terrain.screenDemand->request(opening->screen.diagnostic,target.requiredCapper,
                            board.terrain.units[cut.holder].engineUnitId,cut.tick);
                        break;
                    }
                }
                if(board.terrain.screenFire && opening->screen.orders)
                {
                    QJsonArray checks;
                    for(const auto &cut:review->cuts)
                    {
                        auto proof=priceRetainedScreen(board,*review,cut,opening->screen,bounds,pool);
                        checks.append(proof.record(board));
                        if(proof.survives && proof.openingJobsCompatible){closedWhileRetained=true;break;}
                    }
                    out.retainedWalk["ordinaryFireAgainstRetainedPosts"]=checks;
                }
            }
        }
        const bool captureAvailable=out.earlyTitlePossible && paidIndex<0;
        if(walkBlocked && openPolicy && !openPolicy->openUsesRetainedCut)
        {
            openPolicy->quote.reset();openPolicy->progress={};openPolicy->pass=-1;
            openPolicy->openUsesRetainedCut=true;
        }
        if(closedWhileRetained)
        {
            // Every route for this particular entry crosses a post which ordinary
            // fire cannot remove in time. Early rival occupations, attacking
            // the screen for material, and displacing its existing job remain
            // independent choices. This branch supplies no economic ceiling.
            TownAssaultLine kept;kept.side=attackSide;kept.town=target.property.tile;
            if(const auto price=value(kept))out.open.declineValue=*price;
            out.open.unresolved=true;
            out.open.focusedAttempts.append(QJsonObject{{"unit",target.requiredCapper},{"entryTick",tick},
                {"approachStatus","NAMED_TITLE_DENIED_WHILE_SCREEN_RETAINED"},{"remainingOptions","MATERIAL_OR_POST_RELEASE"}});
        }
        else if(openPolicy && openPolicy->quote && (openPolicy->progress.complete || openPolicy->pass==pass))
        {
            if(budget<=0){out.workLimited=true;out.guardStatus="OPEN_RESPONSE_LOOKUP_LIMIT";return out;}
            --budget;out.open=*openPolicy->quote;out.reusedOpen=true;
        }
        else
        {
            auto &pool=openFunds?*openFunds:budget;
            auto openWork=openFunds?pool:pool/(captureAvailable && !prefixFunds?3:2);
            const auto offered=openWork;
            // On a town the defender already owns, a named challenge needs an
            // actual approach to compare with retaining the early guard. On a
            // neutral town the rival can earn an independent earlier title.
            const auto defendingOwner=target.defendingSide?OwnerSign::Enemy:OwnerSign::Ours;
            const bool boundNamedLanding=!walkBlocked && target.property.ownerBefore!=defendingOwner;
            out.open=priceTownAssault(board,target,bounds,value,openWork,openPolicy?&openPolicy->progress:nullptr,boundNamedLanding,true);
            pool-=offered-openWork;
            if(walkBlocked)out.open.unresolved=true; // Other actors may clear, carry, or claim the screen's activation.
            if(openPolicy){openPolicy->quote=out.open;openPolicy->pass=pass;}
        }
        out.openEvaluated=true;out.workLimited=out.open.workLimited;
        bool anyExpected=false,allExamined=true,allArrivalsRefuted=true;
        for(int i=0;i<static_cast<int>(board.units.size());++i)
        {
            const auto &p=board.terrain.units[i];
            if(!p.present || int(p.enemy)!=attackSide || !board.units[i].canCapture || board.hp[i]<=0 ||
                (target.requiredCapper!=NO_UNIT && p.engineUnitId!=target.requiredCapper))continue;
            anyExpected=true;
            if(!out.open.arrivalsRefuted.contains({i,tick}))allArrivalsRefuted=false;
            if(!out.open.checkedCappers.contains(i)){allExamined=false;break;}
        }
        const bool assumedArrival=std::any_of(out.open.arrivalsRefuted.begin(),out.open.arrivalsRefuted.end(),
            [](const auto &item){return item.second.assumedArrival;});
        // Even if this late unassisted capture loses, the earlier rival may
        // capture, be attacked en route, or be cleared from the town.
        if(!physicalPaidRival && !paidCapper && !assumedArrival && !out.open.workLimited && !out.open.unresolved && anyExpected && allExamined &&
           (!out.open.bestUnanswered || !better(*out.open.bestUnanswered,out.open.declineValue,attackSide) ||
            (allArrivalsRefuted && tick==target.occupiedThrough && !out.open.selectsAssault)))
        {
            // On the branch where the rival declines the early move, even
            // withholding all optional return fire gives the attacker no
            // improvement over declining. Other defensive choices cannot
            // increase that guaranteed gain. Keep this exact board condition
            // until the joint portfolio is known.
            out.dominatedOnRetainedBoard=true;
            out.guardStatus=allArrivalsRefuted?"ON_TOWN_REPLY_COVERS_UNASSISTED_ARRIVALS":"NO_GAIN_IF_RIVAL_DECLINES_EARLY_OCCUPATION";
            return out;
        }
        if(retainedLateAttackNeedsNoPaidGuard(board,target,rival,out.open))
        {
            out.paidGuardUnnecessaryForThisAttack=true;
            out.guardStatus="RETAINED_OPEN_BOARD_BOUNDS_NAMED_ATTACK_WITHOUT_PAID_GUARD";
            return out;
        }
        if(paidIndex>=0 && !physicalPaidRival)
        {
            TownRivalGuardComparison priced;
            const auto reason=priceWithPaidTownGuard(board,target,rival,budget,[&](const TownAssaultBoard &asStock,
                const NeutralOccupancy &moved,const PaidTownIdentity &identity){
                TownRivalOpenPolicy sameOpen;sameOpen.quote=out.open;sameOpen.progress.complete=true;
                StockRouteLowerBounds geometry;
                const auto cash=[&](const TownAssaultLine &line){
                    auto named=line;identity.restore(named);return value(named);
                };
                // The previously checked original board remains one alternative.
                // This stock-shaped copy now prices only the funded rival's actual
                // occupation and early title, with its real cost, weapons and fuel.
                priced=compareTownRivalGuard(asStock,target,moved,geometry,cash,budget,nullptr,pass,nullptr,&sameOpen,
                    nullptr,{}, {},nullptr,true);
                if(priced.guarded)identity.restore(*priced.guarded);
                if(priced.earlierCapture)identity.restore(*priced.earlierCapture);
                if(priced.guardExchange)identity.restore(*priced.guardExchange);
                if(priced.afterGuardExchange)identity.restore(*priced.afterGuardExchange);
                priced.approachShots=identity.restoreEvidence(priced.approachShots).toArray();
                priced.competingGuardGunDuties=identity.restoreEvidence(priced.competingGuardGunDuties).toArray();
            });
            if(!reason.isEmpty())
            {
                out.guardStatus=reason;out.workLimited|=reason=="PAID_GUARD_WORK_LIMIT";return out;
            }
            priced.actor=rival.actor;priced.open=out.open;priced.openEvaluated=out.openEvaluated;
            priced.retainedWalk=out.retainedWalk;
            priced.reusedOpen=out.reusedOpen;priced.workLimited|=out.workLimited;
            if(priced.guardStatus=="GUARDED_CHALLENGE_PRICED")
                priced.guardStatus="FUNDED_GUARDED_CHALLENGE_PRICED";
            return priced;
        }
        if(captureAvailable)
        {
            auto captureTarget=target;
            captureTarget.defendingSide=attackSide;
            captureTarget.occupant=NO_UNIT;
            captureTarget.occupiedFrom=captureTarget.occupiedThrough=rival.entry;
            captureTarget.requiredCapper=board.terrain.units[rival.actor].engineUnitId;
            if(prefix && prefix->latestCapture && (prefix->capture.complete || prefix->capturePass==pass))
            {
                if(budget<=0){out.workLimited=true;out.guardStatus="EARLY_CAPTURE_LOOKUP_LIMIT";return out;}
                --budget;out.earlierCapture=prefix->latestCapture;out.reusedCapture=true;
            }
            else
            {
                auto &pool=prefixFunds?*prefixFunds:budget;
                auto captureWork=pool/2;const auto allowance=captureWork;
                out.earlierCapture=priceTownAssault(board,captureTarget,bounds,value,captureWork,
                    prefix?&prefix->capture:nullptr,false,false,true);
                pool-=allowance-captureWork;
                if(prefix){prefix->latestCapture=out.earlierCapture;prefix->capturePass=pass;}
            }
            out.workLimited|=out.earlierCapture->workLimited;
            if(retake && !out.earlierCapture->workLimited && !out.earlierCapture->unresolved && out.earlierCapture->assault)
            {
                const auto &plan=out.earlierCapture->selectsResponse && out.earlierCapture->response ?
                    *out.earlierCapture->response:*out.earlierCapture->assault;
                if(plan.finishTick>=0 && plan.finishTick<tick)
                {
                    auto lateWork=budget/3;const auto offeredLate=lateWork;
                    out.afterEarlyTitle=retake(*out.earlierCapture,lateWork);budget-=offeredLate-lateWork;
                    out.workLimited|=out.afterEarlyTitle->status=="POST_TITLE_HOLD_WORK_LIMIT" ||
                        (out.afterEarlyTitle->late && out.afterEarlyTitle->late->workLimited);
                }
            }
        }
        if(rival.originUnknown || rival.actor<0 || rival.actor>=static_cast<int>(board.units.size()) ||
           rival.path.steps.empty() || rival.entry<0 || rival.entry>=tick)
        {out.guardStatus="GUARD_ORIGIN_OR_MATCHUP_UNSUPPORTED";return out;}
        const auto &r=board.rights[rival.actor];
        if(r.readyTick<0 || r.lastFreeTick<tick || (r.fuel>=0 && rival.path.fuel>r.fuel))
        {out.guardStatus="GUARD_ALREADY_COMMITTED";return out;}
        out.departure=rival.path.steps.front().tick;
        out.remainingFuel=r.fuel<0?-1:r.fuel-rival.path.fuel;
        const auto town=target.property.tile;
        TownAssaultMove move{rival.actor,target.defendingSide,out.departure,rival.entry,board.positions[rival.actor],town,rival.path};
        auto approach=move;
        for(int stage=rival.entry+2;stage<tick;stage+=2)approach.path.steps.push_back({stage,town,{}});
        auto &common=prefixFunds?*prefixFunds:budget;
        const auto intercept=sharedStops && prefix && world && !physicalPaidRival ?
            sharedStops->booked.query(world,board.traffic.generation(),board.recruits,rival.actor,target.defendingSide,
                approach.path.steps,tick,prefix->booked,common,out.guardProbeAdvanced) :
            bookedShotAgainstGuard(board.recruits,rival.actor,target.defendingSide,approach.path.steps,tick,budget);
        if(!intercept){out.workLimited=true;out.guardStatus="BOOKED_GUARD_SHOT_CHECK_LIMIT";return out;}
        if(*intercept!=NO_UNIT)
        {
            const auto &gun=board.recruits[*intercept];
            out.bookedApproachShot=QJsonObject{{"birthSite",QJsonArray{gun.factory.x,gun.factory.y}},{"buildTick",gun.buildTick},
                {"type",gun.unit?gun.unit->type:QString{}},{"shotTick",gun.impactTick},
                {"guardStop",QJsonArray{gun.targetTile.x,gun.targetTile.y}}};
            out.guardStatus="BOOKED_GUARD_INTERCEPT_NEEDS_SETTLEMENT";
            return out;
        }
        std::vector<TownAssaultRight> reactionRights;std::vector<int> restored;
        auto adjusted=restoreSameTargetGuns(board,rival.actor,out.departure,reactionRights,restored);
        const auto &reaction=adjusted?*adjusted:board;
        bool exposed=false;
        if(prefix)
        {
            if(prefix->underFire>=0 && prefix->underFire<tick)
            {out.approachShots=prefix->firstShots;exposed=true;}
            else for(const auto &s:approach.path.steps)
            {
                if(s.tick>=tick || s.tick<=prefix->quietThrough)continue;
                auto step=move;step.fromTick=s.tick;step.path.steps={s};
                auto *probe=sharedStops?sharedStops->find(world,board.traffic.generation(),rival.actor,out.departure,s,
                    board.terrain.width*board.terrain.height):nullptr;
                const int before=probe?probe->nextGun:0;
                const int spent=probe?probe->paidWork:0;
                const bool hadFields=probe && !probe->fields.empty();
                const auto quiet=quietApproach(reaction,board.traffic,step,s.tick+2,common,&out.approachShots,{},nullptr,probe);
                if(probe)
                {
                    sharedStops->activeFields+=int(!probe->fields.empty())-int(hadFields);
                    out.guardProbeAdvanced|=probe->nextGun>before || probe->paidWork>spent;
                    out.guardGunsChecked=probe->nextGun;
                    out.guardRouteExpansions=probe->expansions;
                }
                if(!quiet)
                {
                    out.verifiedGuardThrough=prefix->quietThrough;
                    out.guardPendingTick=s.tick;out.guardPendingTile=s.tile;
                    out.workLimited=true;out.guardStatus="GUARD_APPROACH_WORK_LIMIT";return out;
                }
                if(!*quiet){prefix->underFire=s.tick;prefix->firstShots=out.approachShots;exposed=true;break;}
                prefix->quietThrough=s.tick;
            }
            out.verifiedGuardThrough=prefix->quietThrough;
        }
        else
        {
            const auto quiet=quietApproach(reaction,board.traffic,approach,tick,budget,&out.approachShots);
            if(!quiet){out.workLimited=true;out.guardStatus="GUARD_APPROACH_WORK_LIMIT";return out;}
            exposed=!*quiet;
        }
        // Both abstaining from early fire and spending a dated gun on the
        // guard are real options for the side that will challenge the town.
        // Keep the guard on the town in this response. All his earlier travel
        // is paid and every prior occupant/reservation must still fit.
        if(prefix && prefix->holdBlocked>=0){out.guardStatus="GUARD_WOULD_DISPLACE_A_RESERVED_BODY";return out;}
        for(int t=prefix?std::max(rival.entry,prefix->holdThrough+1):rival.entry;t<=board.cutoff;++t)
        {
            if(common<=0){out.workLimited=true;out.guardStatus="GUARD_HOLD_WORK_LIMIT";return out;}
            --common;
            if(!board.traffic.open(rival.actor,target.defendingSide,cell(board.terrain,town),t,true))
            {if(prefix)prefix->holdBlocked=t;out.guardStatus="GUARD_WOULD_DISPLACE_A_RESERVED_BODY";return out;}
            if(prefix)prefix->holdThrough=t;
        }
        auto traffic=board.traffic;
        reserve(board.terrain,traffic,move,board.cutoff);
        std::vector<TilePoint> positions(board.positions.begin(),board.positions.end());
        positions[rival.actor]=town;
        std::vector<TownAssaultRight> rights(board.rights.begin(),board.rights.end());
        rights[rival.actor].readyTick=std::max(rival.entry+2,r.readyTick);
        rights[rival.actor].mobileFrom=std::max(rights[rival.actor].readyTick,r.mobileFrom);
        rights[rival.actor].fuel=out.remainingFuel;
        auto at=[&](int a,int t){
            if(a!=rival.actor || t<out.departure)return board.at(a,t);
            auto p=move.origin;
            for(const auto &s:move.path.steps)if(s.tick<=t)p=s.tile;
            return p;
        };
        TownAssaultBoard held{board.terrain,board.units,traffic,positions,board.hp,board.load,rights,board.recruits,
            at,board.recruitFree,board.threshold,board.horizon,board.cutoff,board.canRemove,board.minimumEntryCost,
            board.beforeConditionalDuty,board.conditionalClaims,board.canLoseReactionGun};
        auto guardedTarget=target;
        guardedTarget.occupant=rival.actor;
        auto holdWork=exposed?budget/2:budget;
        const auto holdOffered=holdWork;
        out.guarded=priceGuardSuffix(held,guardedTarget,bounds,value,holdWork,world,suffixes?&suffixes->held:nullptr,
            move,nullptr,rival.entry,out.suffixCredit,out.suffixAdvanced);
        if(suffixes)out.heldResumes=suffixes->held.resumes;
        budget-=holdOffered-holdWork;
        out.workLimited|=out.guarded->workLimited;
        out.guardStatus=out.guarded->workLimited?"GUARDED_CHALLENGE_WORK_LIMIT":
            out.guarded->unresolved?"GUARDED_CHALLENGE_UNRESOLVED":"GUARDED_CHALLENGE_PRICED";
        if(!exposed)return out;

        auto during=board.traffic;
        reserve(board.terrain,during,move,board.cutoff);
        TownAssaultLine guardOrder;
        guardOrder.side=target.defendingSide;guardOrder.town=town;
        guardOrder.capper=rival.actor;guardOrder.entryTick=rival.entry;
        charge(guardOrder,board,approach);
        State health{{board.hp.begin(),board.hp.end()},{board.load.begin(),board.load.end()}};
        if(prefix)
        {
            const auto prior=prefix->fire.onward.upper_bound(tick-1);
            if(prior!=prefix->fire.onward.begin())out.reusedFireThrough=std::prev(prior)->first;
        }
        out.guardExchange=counterLine(reaction,guardOrder,health,board.positions,during,board.recruits,value,nullptr,bounds,
            common,&approach,0,tick-1,prefix?&prefix->fire:nullptr);
        if(!out.guardExchange)
        {
            out.workLimited|=common<=0;
            out.exchangeStatus=common<=0?"EARLY_FIRE_WORK_LIMIT":"EARLY_FIRE_RESOURCES_UNSUPPORTED";
            return out;
        }
        auto &exchange=*out.guardExchange;
        if(exchange.shots.empty()){out.exchangeStatus="NO_WITNESSED_EARLY_SHOT";return out;}
        if(board.conditionalClaims)for(const auto &shot:exchange.shots)
        {
            if(std::find(restored.begin(),restored.end(),shot.actor)==restored.end())continue;
            for(const auto &d:board.conditionalClaims->at(shot.actor))
                if(d.claimant!=rival.actor && shot.tick<=d.through)
                    out.competingGuardGunDuties.append(QJsonObject{{"gun",board.terrain.units[shot.actor].engineUnitId},
                        {"claimant",board.terrain.units[d.claimant].engineUnitId},{"through",d.through},
                        {"target",QJsonArray{d.target.x,d.target.y}}});
        }
        const int n=static_cast<int>(board.units.size());
        auto after=board.traffic;
        auto afterPositions=std::vector<TilePoint>(board.positions.begin(),board.positions.end());
        auto afterRights=std::vector<TownAssaultRight>(board.rights.begin(),board.rights.end());
        for(const auto &motion:exchange.moves)
        {
            if(motion.actor<0 || motion.actor>=n){out.exchangeStatus="PAID_MOVE_REQUIRES_REPRICING";return out;}
            reserve(board.terrain,after,motion,board.cutoff);
            afterPositions[motion.actor]=motion.destination;
            auto &fuel=afterRights[motion.actor].fuel;
            if(fuel>=0 && motion.path.fuel>fuel){out.exchangeStatus="INSUFFICIENT_MOVEMENT_FUEL";return out;}
            if(fuel>=0)fuel-=motion.path.fuel;
        }
        for(const auto &shot:exchange.shots)
        {
            if(shot.actor<0 || shot.actor>=n){out.exchangeStatus="PAID_GUN_REQUIRES_DATED_RELEASE";return out;}
            health.hp[shot.actor]=shot.actorAfter;health.hp[shot.target]=shot.targetAfter;
            spendCombatRound(health.load[shot.actor],shot.weapon);
            spendCombatRound(health.load[shot.target],shot.counterWeapon);
            for(const int participant:{shot.actor,shot.target})if(health.hp[participant]<=0)
            {
                after.removeFutureUses(participant,shot.tick);
                afterPositions[participant]=INVALID_TILE;
                afterRights[participant].readyTick=-1;
            }
        }
        // This is a constructive post-observation response. Other available
        // units waited until the branch's last occupied guard stop or shot.
        // A more profitable plan with earlier staging can be tried separately.
        int reveal=exchange.shots.back().tick;
        for(const auto &motion:exchange.moves)for(const auto &st:motion.path.steps)
            if(st.tick<tick)reveal=std::max(reveal,st.tick);
        for(int i=0;i<n;++i)
        {
            auto &r=afterRights[i];
            if(r.readyTick<0 || afterPositions[i]==INVALID_TILE)continue;
            const int side=int(board.terrain.units[i].enemy);
            const bool acted=std::any_of(exchange.shots.begin(),exchange.shots.end(),[&](const auto &s){
                    return s.actor==i && s.tick==reveal;}) ||
                std::any_of(exchange.moves.begin(),exchange.moves.end(),[&](const auto &m){
                    return m.actor==i && std::any_of(m.path.steps.begin(),m.path.steps.end(),[&](const auto &s){return s.tick==reveal;});});
            const int next=reveal+int(reveal%2!=side)+2*int(reveal%2==side && acted);
            r.readyTick=std::max(r.readyTick,next);r.mobileFrom=std::max(r.mobileFrom,r.readyTick);
        }
        const auto afterAt=[&](int actor,int atTick) {
            if(actor<0 || actor>=n || afterPositions[actor]==INVALID_TILE)return INVALID_TILE;
            const auto p=afterPositions[actor];
            const auto &uses=after.uses(cell(board.terrain,p));
            for(const auto &u:uses)if(u.actor==actor && u.stopped && u.first<=atTick && atTick<=u.last)return p;
            return INVALID_TILE;
        };
        TownAssaultBoard observed{board.terrain,board.units,after,afterPositions,health.hp,health.load,afterRights,
            board.recruits,afterAt,board.recruitFree,board.threshold,board.horizon,board.cutoff,board.canRemove,board.minimumEntryCost};
        // Conditional guns from before this volley cannot be revived with their
        // old HP, ammunition or firing post in this particular continuation.
        auto observedTarget=target;
        if(health.hp[rival.actor]>0)
        {
            if(afterPositions[rival.actor]!=town){out.exchangeStatus="GUARD_STOPPED_BEFORE_POST";return out;}
            observedTarget.occupant=rival.actor;
        }
        const auto combined=[&](const TownAssaultLine &late) {
            auto whole=late;
            whole.losses.insert(whole.losses.begin(),exchange.losses.begin(),exchange.losses.end());
            return value(whole);
        };
        out.afterGuardExchange=priceGuardSuffix(observed,observedTarget,bounds,combined,budget,world,suffixes?&suffixes->afterFire:nullptr,
            move,&exchange,reveal,out.suffixCredit,out.suffixAdvanced);
        if(suffixes)out.observedResumes=suffixes->afterFire.resumes;
        out.workLimited|=out.afterGuardExchange->workLimited;
        out.exchangeStatus=!out.competingGuardGunDuties.isEmpty()?"REPRICE_OTHER_GUN_DUTIES":
            out.afterGuardExchange->workLimited?"AFTER_FIRE_WORK_LIMIT":
            out.afterGuardExchange->unresolved?"AFTER_FIRE_PARTIAL":"POST_OBSERVATION_LINE_PRICED";
        return out;
    }
}
