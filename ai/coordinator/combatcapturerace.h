#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/datedformulaledger.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/headquartersdefense.h"
#include "ai/coordinator/followupdecision.h"
#include "ai/coordinator/captureapproach.h"
#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/captureproductiondependency.h"
#include "ai/coordinator/townassaultintegration.h"
#include "ai/coordinator/fundedpostchoice.h"
#include "ai/coordinator/fundedpostprefix.h"
#include "ai/coordinator/towncomparisonevidence.h"
#include "ai/coordinator/towndecisionfrontier.h"
#include "ai/coordinator/observedstockmarket.h"
#include "ai/coordinator/openingincomebounds.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replypassevaluation.h"
#include "ai/coordinator/f1movementcertificate.h"
#include <QJsonArray>
#include <QJsonObject>

namespace Coordinator
{
    struct CaptureRaceResult
    {
        MilliFunds stock{};
        std::optional<OpeningIncomeBounds> openingIncome;
        bool hasOptionalHold{};
        QJsonArray withdrawals;
        MilliFunds foregoneShot{};
        QJsonObject friendlyService;
        QJsonArray observedF1Stock;
        QString friendlyServiceOpen;
        MilliFunds timingAdjustment{};
        QJsonArray roundCashFlow;
        int terminalTick{-1}, terminalSide{0};
        MilliFunds holdingLoss{}, ownedIncome{};
        QJsonArray holds, returnFire, enemyCaptureActions;
        MilliFunds replacedNextShot{}, returnValue{}, survivorNextShotAdjustment{};
        QJsonArray survivorNextShots;
        QJsonArray claims,routeProofs,paidCaptureChecks,observedStockResponses;
        QJsonObject paidCaptureWorkGap,paidCaptureUnsupported;
        bool observedStockComplete{true};
        QJsonArray townEncounters,townSearch,townCoverage,townConflicts;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        QJsonObject townFocusAudit;
#endif
        int townEncounterBids{};
        std::int64_t townEncounterWork{};
        int routePricingPasses{},localRouteBids{},deferredRouteAssignments{};
        QJsonArray stockDutyReview,settledStockIntentions,fundedPostResponses;
        std::map<int,FundedPostPrefix> fundedPostPrefixes;
        std::int64_t routeTraversalWork{},routeDefenseWork{},fundedProjectionWork{};
        QJsonArray contestedHeadquarters, contestedPropertyCaptures;
        std::optional<CaptureApproach> approach, replacementApproach;
        QJsonObject approachRecord;
        MilliFunds productionValue{};
        int availableProductionSites{}, affordableProductionOrders{};
        bool productionLimited{};
        QJsonArray productionOrders;
        QJsonArray unconfirmedFactoryReleases;
        QJsonObject alliedCaptureVacancy;
        bool guardCovered{};
        QJsonObject guardAction, boxContinuation, combinedFollowup, followupDecision, homeCaptureResponse;
        MilliFunds replacedCapperNextShot{}, replacedHomeCapperNextShot{};
    };

    struct CaptureTimingAfterHit { int rate{}, finish{}; };

    inline CaptureTimingAfterHit approachCaptureClock(const KnownUnit &capper,double health,
        int entry,int threshold,int horizon)
    {
        const int steps=ceilHpSteps(health);
        const int rate=replyCaptureRate(capper.captureRate,capper.hpRounded-steps,steps<=0);
        return {rate,rate<=0 ? horizon : static_cast<int>(std::min<std::int64_t>(horizon,
            entry+widenedCaptureTurns(0,rate,threshold)-1))};
    }

    inline std::optional<CaptureTimingAfterHit> delayedBookedCapture(int threshold,int carry,int rate,
        int finish,int horizon,int economicHp,int damagedHp)
    {
        if (carry<0 || carry>=threshold || rate<=0 || finish<1 || finish>=horizon ||
            damagedHp<=0 || damagedHp>=economicHp) return {};
        const int wounded=replyCaptureRate(rate,economicHp-damagedHp,false);
        const auto originalTurns=widenedCaptureTurns(carry,rate,threshold);
        const auto after=wounded>0 ? std::min<std::int64_t>(horizon,
            finish+widenedCaptureTurns(carry,wounded,threshold)-originalTurns) : horizon;
        if (after<=finish) return {};
        return CaptureTimingAfterHit{wounded,static_cast<int>(after)};
    }

    inline constexpr bool winsOnNextCapture(int carry,int nextRate,int threshold)
    {
        return nextRate>0 && static_cast<std::int64_t>(carry)+nextRate>=threshold;
    }

    // One comparison shares the map, movement profiles and ordered property columns across
    // all replies. Cache only distances with the exact same hostile occupancy and time limit.
    struct CaptureRaceRouteCache
    {
        using Key = std::tuple<int, int, int, int, int, std::vector<int>>;
        std::map<Key, std::vector<int>> routes;
        using ExitKey = std::tuple<int,int,int,int,int,int,std::vector<int>,std::vector<int>>;
        std::map<ExitKey,TilePoint> factoryExits;
        ApproachStepCache approachSteps;
        StockRouteLowerBounds preparedPropertyBounds;
        std::vector<TownQuoteFocus> requiredTownChallenges;
        TownMarketHistory townMarkets;
        std::shared_ptr<F1OpportunityShelf> f1OpportunityShelf;
        QJsonObject *completedF1Queries{};
        std::int64_t townEffort{1},stockRouteEffort{1},paidRouteEffort{1};
        std::set<StockDutyTrip> requestedDutyTrips,seenDutyTrips;
        StockChallengeMenu matchedStockChallenges;
        QJsonObject fundedPostQuestions;
        std::optional<FundedPostCommitment> fundedPostCommitment;
        std::optional<TownDecisionFrontier> decisionFrontier;
        std::optional<TownIntention> priorityFor(const QString &policy) const
        {return decisionFrontier && decisionFrontier->policies.contains(policy)?std::optional{decisionFrontier->intention}:std::nullopt;}
        QString stockComparisonPolicy;
        const std::set<TownIntention> &stockChallengesFor(const QString &policy) const
        {
            static const std::set<TownIntention> none;
            const auto it=matchedStockChallenges.find(policy);
            return it==matchedStockChallenges.end()?none:it->second;
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::optional<std::int64_t> privateDutyAllowance;
#endif
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::optional<TownQuoteFocus> townAuditFocus;
        std::optional<std::int64_t> privateTownQuoteCap;
#endif
        QJsonObject lastFailure;
        static constexpr std::size_t maxEntries = 8192;
        int hits{}, misses{};
    };

    inline std::int64_t prepareStockPropertyBounds(const CombatTerrain &terrain,std::span<const TilePoint> goals,
        StockRouteLowerBounds &prepared,std::int64_t &budget)
    {
        const std::int64_t cells=static_cast<std::int64_t>(terrain.width)*terrain.height;
        if (cells<=0) return 0;
        std::set<int> profiles;
        for (const auto &unit:terrain.units)
            if (unit.present && unit.capper && unit.grid>=0) profiles.insert(unit.grid);
        // Only immutable geometry is shared between comparisons. Limit retained distance
        // arrays on large maps. Other endpoints are computed inside each independent quote.
        constexpr std::int64_t maxStoredCells=8*1024*1024/sizeof(int);
        std::int64_t remainingCells=std::min(maxStoredCells,budget/16);
        const auto initial=budget;
        for (int index:profiles)
        {
            const auto &grid=terrain.grids[index];
            for (auto goal:goals)
            {
                if (!grid.onGrid(goal.x,goal.y)) continue;
                if (remainingCells<cells) return initial-budget;
                const int sq=goal.y*terrain.width+goal.x;
                if (prepared.cached(grid,sq)) continue;
                remainingCells-=cells;
                if (!prepared.costs(grid,sq,budget)) return initial-budget;
            }
        }
        return initial-budget;
    }

    // Both the first opposing capture and the later calendar query the same ordered
    // property targets, using the same movement rules and living transit blockers.
    template<class Build>
    const std::vector<int> *captureTargetDistances(CaptureRaceRouteCache *cache, CaptureRaceRouteCache::Key key,
        const CombatTerrain &terrain, std::int64_t &budget, std::vector<int> &scratch, Build build)
    {
        if (cache)
        {
            if (const auto found=cache->routes.find(key); found!=cache->routes.end())
            {
                ++cache->hits;
                return &found->second;
            }
            ++cache->misses;
        }
        const auto work=2LL*terrain.width*terrain.height;
        if (budget<work) return nullptr;
        budget-=work;
        build(scratch);
        if (cache && cache->routes.size()<CaptureRaceRouteCache::maxEntries)
            return &cache->routes.emplace(std::move(key),std::move(scratch)).first->second;
        return &scratch;
    }

    // A funded build on the next activation needs the actual first stop on the
    // holder's capture trip. A distant arrival date alone cannot clear a factory.
    // No value means the search ran out of work, distinct from a proven absent exit.
    inline std::optional<TilePoint> bookedFactoryExit(const CombatTerrain &terrain, int actor, TilePoint from, TilePoint target,
        int activations, int fuel, std::span<const TilePoint> positions, std::vector<int> plannedStops,
        CaptureRaceRouteCache *cache, std::int64_t &budget)
    {
        const auto &pawn=terrain.units[actor];
        if (pawn.grid<0 || pawn.movement<=0 || activations<1 || fuel<=0 || from==target)
            return INVALID_TILE;
        const auto cell=[&](TilePoint t){return t.y*terrain.width+t.x;};
        std::vector<int> hostile;
        for (int other=0;other<static_cast<int>(positions.size());++other)
        {
            if (positions[other]==INVALID_TILE || other==actor) continue;
            const int pos=cell(positions[other]);
            plannedStops.push_back(pos);
            if (!pawn.ignoresCollision && terrain.units[other].enemy!=pawn.enemy)
                hostile.push_back(pos);
        }
        std::sort(plannedStops.begin(),plannedStops.end());
        plannedStops.erase(std::unique(plannedStops.begin(),plannedStops.end()),plannedStops.end());
        CaptureRaceRouteCache::ExitKey key{pawn.grid,cell(from),cell(target),pawn.movement,
                                           activations,fuel,std::move(hostile),std::move(plannedStops)};
        if (cache)
            if (auto found=cache->factoryExits.find(key); found!=cache->factoryExits.end())
                return found->second;
        const auto work=4LL*terrain.width*terrain.height;
        if (work>budget) return {};
        budget-=work;
        std::vector<TilePoint> blocked;
        for (int pos:std::get<6>(key)) blocked.push_back({pos%terrain.width,pos/terrain.width});
        std::vector<char> cannotStop(terrain.width*terrain.height);
        for (int pos:std::get<7>(key)) cannotStop[pos]=true;
        const auto grid=terrain.grids[pawn.grid].withBlockedTiles(blocked);
        const std::int64_t maxTravel=std::min<std::int64_t>(fuel,static_cast<std::int64_t>(activations)*pawn.movement);
        const auto distance=DistanceField::build(grid,{target},FieldExpansion::ToTargets,
            static_cast<std::int32_t>(std::min<std::int64_t>(maxTravel,std::numeric_limits<std::int32_t>::max())));
        // A unit can end a turn before exhausting its movement. This matters
        // when its last movement point would otherwise strand it on an ally.
        using State=std::tuple<int,int,int>; // turn, cell, movement spent this turn
        using Step=std::tuple<int,State,int,int>; // remaining cost, state, fuel used, first stop
        std::priority_queue<Step,std::vector<Step>,std::greater<Step>> queue;
        std::map<State,int> leastFuel;
        auto enqueue=[&](int day,int pos,int spent,int used,int first) {
            const int remaining=distance.distance(pos%terrain.width,pos/terrain.width);
            if (remaining<0 || remaining>fuel-used ||
                remaining>static_cast<std::int64_t>(activations-day+1)*pawn.movement-spent) return;
            const State state{day,pos,spent};
            auto [it,fresh]=leastFuel.emplace(state,used);
            if (!fresh && it->second<=used) return;
            it->second=used;
            queue.emplace(remaining,state,used,first);
        };
        enqueue(1,cell(from),0,0,-1);
        TilePoint exit=INVALID_TILE;
        while (!queue.empty())
        {
            if (budget<5) return {};
            budget-=5;
            const auto [remaining,state,used,first]=queue.top();
            queue.pop();
            if (used!=leastFuel.at(state)) continue;
            const auto [day,pos,spent]=state;
            if (pos==cell(target) && !cannotStop[pos])
            {
                const int end=day==1 ? pos : first;
                exit={end%terrain.width,end/terrain.width};
                break;
            }
            if (day<activations && pos!=cell(from) && !cannotStop[pos] && (day==1 || spent>0))
                enqueue(day+1,pos,0,used,day==1 ? pos : first);
            for (auto dir:ENTRY_DIRECTIONS)
            {
                const auto delta=entryStep(dir);
                const TilePoint to{pos%terrain.width+delta.dx,pos/terrain.width+delta.dy};
                const int cost=grid.cost(to.x,to.y,dir);
                if (cost>=0 && cost<=pawn.movement-spent && cost<=fuel-used)
                    enqueue(day,cell(to),spent+cost,used+cost,first);
            }
        }
        if (cache && cache->factoryExits.size()<CaptureRaceRouteCache::maxEntries)
            cache->factoryExits.emplace(std::move(key),exit);
        return exit;
    }

    // The horizon contains enemy paydays E0..E(H-1) and friendly paydays F1..FH.
    // Each player collects income before acting. The original friendly action is
    // F0.
    constexpr MilliFunds captureRaceSwing(const PropertyStockColumn &column, bool enemy, int day, int horizon)
    {
        const int future = horizon - day;
        if (!enemy)
            return incomeSwing(column.income, future, column.ownerBefore, OwnerSign::Ours);
        const auto lost = column.ownerBefore == OwnerSign::Ours ? column.income.oursPerTurn * future : 0;
        return -lost - column.income.enemyPerTurn * std::max(0, future - 1);
    }

    // A friendly row can shoot from its projected position on F1 and start its trip on F2.
    // Prefer the earlier capture on an exact tie. A last-payday shot can also stand alone.
    constexpr bool shootBeforeCapture(MilliFunds now, MilliFunds later, MilliFunds shot)
    {
        return shot > 0 && now - later < shot;
    }

    inline std::optional<MilliFunds> survivorShotAtFriendlyAction(const SurvivorWeaponFacts &weapon,
        const std::array<int, 2> &remainingAmmo, const CombatTerrain &terrain, std::span<const TilePoint> positions,
        std::span<const int> health, int unit, int movementSpent, std::int64_t &budget)
    {
        const auto work = weapon.work(terrain.width, terrain.height) + static_cast<std::int64_t>(positions.size());
        if (work > budget)
            return {};
        budget -= work;
        SurvivorPosition state;
        state.ammo = remainingAmmo;
        for (int i = 0; i < static_cast<int>(positions.size()); ++i)
        {
            if (terrain.units[i].enemy)
                state.enemy[i] = {positions[i], health[i]};
            else if (health[i] > 0)
                state.friendly[i] = positions[i];
        }
        return survivorShotValue(weapon, state, positions[unit], movementSpent, health[unit]);
    }

    // Deterministic policy: first arrival claims an unreserved property. Rows may
    // travel again only after finishing their capture. Opposing transit blockers
    // remain fixed at the post-reply cut, and later recaptures are outside the
    // model.
    inline std::optional<CaptureRaceResult> projectCaptureRaceAttempt(
        bool proposalOnly,
        const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge, const MasterAssignmentProblem &problem,
        const PropertyStockField &field, const ReplyPassEvaluation &plan, std::int64_t &budget,
        TilePoint homeHQ = INVALID_TILE, TilePoint enemyHQ = INVALID_TILE, bool stayForCapture = false,
        int reservedGuard = NO_UNIT, bool takeCombinedFollowup = false, bool punishCombinedFollowup = false,
        CaptureRaceRouteCache *routeCache = nullptr, bool interruptHomeCapture = false,
        ApproachBranch branch=ApproachBranch::Observe, const CaptureApproach *approach=nullptr,
        const CaptureApproach *priorDecline=nullptr, const RequestedFollowup *requestedFollowup=nullptr,bool priceCapturedSources=false,bool *selectedNewSource=nullptr)
    {
        if(routeCache) routeCache->lastFailure={};
        CaptureRaceResult result;
        bool requestedAnswerPlayed=false;
        if(requestedFollowup && requestedFollowup->answer && (!takeCombinedFollowup || !punishCombinedFollowup))return {};
        QJsonArray f1Quotes,f1Fires;
        bool f1Quoted=false;
        const auto refuseWork=[&](const char *phase,std::int64_t required){
            if(routeCache)routeCache->lastFailure=QJsonObject{{"phase",QString::fromLatin1(phase)},
                {"reason","CAPTURE_RACE_WORK_LIMIT"},{"requiredAtRefusal",static_cast<qint64>(required)},
                {"availableAtRefusal",static_cast<qint64>(budget)}};
            if(routeCache && !result.followupDecision.isEmpty())routeCache->lastFailure["followupDecision"]=result.followupDecision;
            if(routeCache && f1Quoted && !proposalOnly){
                QJsonObject prefix{{"scope","CONDITIONAL_F1_SHOT_OPPORTUNITIES"},{"tick",2},{"policy",routeCache->stockComparisonPolicy},
                    {"hold",stayForCapture},{"guard",reservedGuard},{"followup",takeCombinedFollowup},{"punish",punishCombinedFollowup},
                    {"defendHome",interruptHomeCapture},{"approachBranch",int(branch)},{"shots",f1Quotes},{"priorReturnFire",f1Fires},
                    {"subsequentRefusal",QString::fromLatin1(phase)},{"fullRank",false},{"reusablePrice",false}};
                if(!result.friendlyServiceOpen.isEmpty())prefix["serviceGap"]=result.friendlyServiceOpen;
                if(!result.friendlyService.isEmpty())prefix["service"]=result.friendlyService;
                routeCache->lastFailure["completedF1"]=prefix;
            }
            return std::nullopt;
        };
        const int horizon = field.horizonTurns();
        struct Transfer
        {
            PropertyStockColumn previous;
            bool enemy;
            int day;
            MilliFunds shot;
            int departure;
            int unit, entry;
            int rate{}, carry{};
            TilePoint from{INVALID_TILE};
        };
        std::vector<Transfer> transfers;
        std::vector<PropertyStockColumn> columns;
        std::vector<TilePoint> targets;
        for (const auto &building : knowledge.buildings())
        {
            const TilePoint tile{building.x, building.y};
            const int slot = field.columnSlotAt(tile);
            if (slot == NO_STOCK_COLUMN)
                continue;
            columns.push_back(mirroredColumn(field.enemyColumn(slot)));
            const auto &income = columns.back().income;
            const auto limit = std::numeric_limits<MilliFunds>::max() / (2LL * std::max(1, horizon) + 2);
            if (income.oursPerTurn < 0 || income.enemyPerTurn < 0 || income.oursPerTurn > limit ||
                income.enemyPerTurn > limit)
                return {};
            targets.push_back(tile);
        }
        const auto initial = columns;
        std::vector<bool> unstarted(terrain.units.size()), withdrew(terrain.units.size());
        std::vector<int> hp, carry;
        std::vector<const ReplyFacts *> holdFacts(terrain.units.size());
        std::vector<int> finishDay(terrain.units.size(), -1);
        std::vector<std::pair<int, MilliFunds>> losses;
        if(terrain.orderedOpening && terrain.orderedOpening->materialLessCash)
            losses.emplace_back(1,terrain.orderedOpening->materialLessCash);
        std::vector<MilliFunds> nextShot(terrain.units.size());
        std::vector<bool> fired(terrain.units.size());
        for (const auto &shot : plan.shots)
            fired.at(shot.shooterIndex) = true;
        if(plan.openingRecruitFight) fired.at(plan.openingRecruitFight->shooter)=true;
        for (int i = 0; i < static_cast<int>(terrain.units.size()); ++i)
        {
            const auto &unit = knowledge.units()[i];
            const auto found = plan.enemyReplyHp.find(i);
            hp.push_back(found == plan.enemyReplyHp.end() ? unit.hpRounded : found->second);
            carry.push_back(plan.combatLocations[i] == terrain.units[i].tile ? unit.capturePoints : 0);
        }
        for(const auto &defender:terrain.observedDefenders){
            const auto it=plan.realAfterReply.find(defender.knowledgeIndex);
            if(it==plan.realAfterReply.end())return {};
            const auto i=defender.knowledgeIndex;
            hp[i]=ceilHpSteps(it->second);
            if(hp[i]<=0)carry[i]=0;
        }
        for (const auto &actor : plan.actors)
        {
            const auto &entry = problem.actors()[actor.actorSlot];
            const int i = entry.knowledgeUnitIndex;
            hp[i] = actor.stepsAfterReply;
            nextShot[i] = actor.terms.nextShotAfterReply;
            const auto &action = plan.replay.plan.action(actor.actionIndex);
            const auto *component = singleComponentOf(entry.candidates[actor.candidateIndex].source.bundle);
            if (!component || component->kind != ComponentKind::Capture)
            {
                const auto *facts = entry.candidates[actor.candidateIndex].source.replyFacts.get();
                if (hp[i] > 0 && terrain.units[i].capper &&
                    terrain.contestableSites.contains({action.destination.x, action.destination.y}) && facts &&
                    !facts->tile->shooters.empty())
                {
                    unstarted[i] = true;
                    holdFacts[i] = facts;
                }
                continue;
            }
            auto found = std::find(targets.begin(), targets.end(), action.destination);
            if (found == targets.end())
                continue;
            auto &column = columns[found - targets.begin()];
            if (field.actionCompletesCapture({actor.stockRow, action.destination, true}))
            {
                if (!RelativeStockDetail::checkedAdd(result.stock, captureRaceSwing(column, false, 0, horizon)))
                    return {};
                transfers.push_back({column, false, 0, 0, 0, i, 0});
                column.ownerBefore = OwnerSign::Ours;
                carry[i] = 0;
            }
            else
            {
                carry[i] += knowledge.units()[i].captureRate;
                holdFacts[i] = entry.candidates[actor.candidateIndex].source.replyFacts.get();
            }
        }
        struct FriendlyStockTurn { int unit; int projectedF0Movement; bool observed; };
        std::vector<FriendlyStockTurn> friendlyActors;
        std::vector<int> f0Movement(hp.size(),-1);
        for(const auto &a:plan.actors){
            const auto &entry=problem.actors()[a.actorSlot];const int i=entry.knowledgeUnitIndex;
            const int spent=std::max(0,entry.candidates[a.candidateIndex].source.movementCost);
            friendlyActors.push_back({i,spent,false});f0Movement[i]=spent;
        }
        for(int i=0;i<int(hp.size());++i){
            const auto &u=knowledge.units()[i];const auto &pawn=terrain.units[i];
            if(f0Movement[i]>=0 || !u.hasMoved || u.ownerId!=knowledge.observerId() ||
               u.carrier!=KnownUnit::NO_CARRIER || !pawn.present)continue;
            // Observed fuel and ammunition are already after the real opening
            // action. No movement, attack, capture increment or opportunity is
            // rebooked at F0. The unit next acts after E0 and F1 day-start.
            friendlyActors.push_back({i,0,true});f0Movement[i]=0;
        }
        // A complete F0 owns the historical firing order. A midturn save
        // has resources and destinations but no authenticated earlier order.
        // Use one deterministic F1 ordering for that observed cut.
        if (friendlyActors.size() != plan.actors.size())
            std::stable_sort(friendlyActors.begin(), friendlyActors.end(), [&](const auto &a, const auto &b) {
                return terrain.units[a.unit].engineUnitId < terrain.units[b.unit].engineUnitId;
            });
        for (const auto &observed : terrain.observedDefenders) {
            const int i = observed.knowledgeIndex;
            if (f0Movement[i] < 0 || hp[i] <= 0 || plan.combatLocations[i] == INVALID_TILE ||
                !terrain.units[i].capper || !terrain.contestableSites.contains({observed.tile.x, observed.tile.y})) continue;
            if (!observed.facts || !observed.facts->tile) {
                result.friendlyServiceOpen = "OBSERVED_F1_CAPTURE_FACTS_UNAVAILABLE";
                continue;
            }
            if (carry[i] > 0 || !observed.facts->tile->shooters.empty()) {
                holdFacts[i] = observed.facts.get();
                // Its F0 capture increment is already reflected in the native carry.
                unstarted[i] = carry[i] == 0;
            }
        }
        MilliFunds firstEnemyIncome=0;
        const auto openingBoundWork=2*static_cast<std::int64_t>(columns.size())+2LL*horizon;
        bool openingIncomeMeasured=budget>=openingBoundWork;
        if(openingIncomeMeasured)
        {
            budget-=openingBoundWork;
            for(const auto &c:columns)if(c.ownerBefore==OwnerSign::Enemy)
                openingIncomeMeasured&=RelativeStockDetail::checkedAdd(firstEnemyIncome,c.income.enemyPerTurn);
        }
        // A committed capturer acts at F1 before E1. A hostile already in firing range can
        // fire once each subsequent turn while it survives and retains ammunition.
        // All simultaneous captures share that shooter's action, HP and ammunition.
        auto futureHp = hp, progress = carry;
        std::vector<double> real(hp.begin(), hp.end());
        for(const auto &a:friendlyActors)if(a.observed){
            const auto &pawn=terrain.units[a.unit];
            if(plan.realAfterReply.contains(a.unit))continue;
            if(pawn.realHp>=0 && ceilHpSteps(pawn.realHp)==hp[a.unit])real[a.unit]=pawn.realHp;
            else if(hp[a.unit]>0)result.friendlyServiceOpen="OBSERVED_STOCK_HP_UNVERIFIED";
        }
        for (const auto &[index, health] : plan.realAfterReply)
            real[index] = health;
        std::vector<int> lastShot(hp.size(), -1),lastTargetTick(hp.size(),-1);
        std::vector<int> spentShots(hp.size());
        for (const auto &actor : plan.actors)
        {
            const auto &entry = problem.actors()[actor.actorSlot];
            const auto *part = singleComponentOf(entry.candidates[actor.candidateIndex].source.bundle);
            if (part && part->kind == ComponentKind::Fire)
            {
                ++spentShots[entry.knowledgeUnitIndex];
                if (!isTerrainTargetId(part->fire.targetUnitId) &&
                    part->fire.targetUnitId >= 0 &&
                    part->fire.targetUnitId < static_cast<int>(spentShots.size()) &&
                    (actor.ownShot.target != NO_UNIT ? actor.ownShot.report.counter >= 0 :
                     actor.stepsAfterOwnActions < actor.dayStartSteps))
                    ++spentShots[part->fire.targetUnitId];
            }
        }
        // Submitted targets carry their original actor slot in older reply
        // records. Spent observers have no F0 order and supply a knowledge ID.
        const auto targetIndex=[&](const ReplyPassShot &shot) {
            if(shot.target>=plan.actors.size())return shot.targetKnowledge;
            const int slot=plan.actors[shot.target].actorSlot;
            if(slot<0 || slot>=int(problem.actors().size()))return NO_UNIT;
            const int index=problem.actors()[slot].knowledgeUnitIndex;
            return shot.targetKnowledge==NO_UNIT || shot.targetKnowledge==index ? index : NO_UNIT;
        };
        for (const auto &shot : plan.shots)
        {
            const int victim=targetIndex(shot);
            if(victim<0 || victim>=int(terrain.units.size()) ||
               shot.shooterIndex<0 || shot.shooterIndex>=int(terrain.units.size()) ||
               (shot.targetEngineUnitId!=NO_UNIT && shot.targetEngineUnitId!=terrain.units[victim].engineUnitId))
                return {};
            ++spentShots[shot.shooterIndex];
            if(shot.counterWeapon>=0 || shot.counterCapital>0)
                ++spentShots[victim];
        }
        if(plan.openingRecruitFight) ++spentShots[plan.openingRecruitFight->shooter];
        std::vector<int> ammo;
        for (int i = 0; i < static_cast<int>(hp.size()); ++i)
            ammo.push_back(std::max(0, terrain.units[i].availableShots - spentShots[i]));
        auto positions = plan.combatLocations;
        // Spend E0 before constructing any F1 firing route. An enemy that fired cannot capture;
        // a capper that enters a property now occupies it when the friendly return shot is chosen.
        std::vector<bool> capturedNow(hp.size()), blockedExit(hp.size());
        for (const auto &order : plan.encirclement.blockOrders)
            blockedExit[order.unitIndex] = true;
        std::vector<TilePoint> enemyBlockers;
        std::map<std::pair<int, int>, int> occupied;
        for (const auto &site:terrain.friendlyBirthSites)
        {
            enemyBlockers.push_back({site.first,site.second});
            occupied.emplace(site,NO_UNIT);
        }
        for (int i = 0; i < static_cast<int>(positions.size()); ++i)
            if (positions[i] != INVALID_TILE)
            {
                occupied[{positions[i].x, positions[i].y}] = i;
                if (!terrain.units[i].enemy)
                    enemyBlockers.push_back(positions[i]);
            }
        for (int i = 0; i < static_cast<int>(positions.size()); ++i)
        {
            const auto &pawn = terrain.units[i];
            const auto &unit = knowledge.units()[i];
            if (!pawn.enemy || !pawn.capper || fired[i] || blockedExit[i] || i == plan.enemyWaiting || positions[i] == INVALID_TILE || pawn.grid < 0)
                continue;
            const int rate = replyCaptureRate(unit.captureRate, unit.hpRounded - hp[i], hp[i] <= 0);
            if (rate <= 0)
                continue;
            const auto query = static_cast<std::int64_t>(targets.size() + (routeCache ? enemyBlockers.size() : 0));
            if (budget<query) return {};
            budget-=query;
            std::vector<int> blockedCells, scratch;
            if (!pawn.ignoresCollision)
                for (TilePoint blocked : enemyBlockers) blockedCells.push_back(blocked.y*terrain.width+blocked.x);
            const auto from=positions[i];
            auto distances=captureTargetDistances(routeCache,{pawn.grid,from.y*terrain.width+from.x,pawn.movement,
                1,int(pawn.ignoresCollision),std::move(blockedCells)},terrain,budget,scratch,[&](auto &out) {
                    ActivationField reach;
                    const auto &grid=terrain.grids[pawn.grid];
                    reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(enemyBlockers),from,pawn.movement,1,targets);
                    for (const auto &target : targets) out.push_back(reach.activations(target.x,target.y));
                });
            if (!distances) return {};
            int best = -1;
            std::tuple<bool, MilliFunds, int> bestKey{};
            for (int j = 0; j < static_cast<int>(columns.size()); ++j)
            {
                const auto &col = columns[j];
                const auto tile = col.tile;
                const auto occ = occupied.find({tile.x, tile.y});
                if (col.ownerBefore == OwnerSign::Enemy || (occ != occupied.end() && occ->second != i) ||
                    (*distances)[j] != 1)
                    continue;
                const int carried = tile == positions[i] ? carry[i] : 0;
                const auto turns = widenedCaptureTurns(carried, rate, field.capturePointsToCapture());
                if (turns <= 0 || turns > horizon)
                    continue;
                const auto benefit = -captureRaceSwing(col, true, static_cast<int>(turns - 1), horizon);
                const auto key = std::tuple{tile == homeHQ, benefit, carried};
                if ((benefit > 0 || tile == homeHQ) && (best < 0 || key > bestKey))
                {
                    best = j;
                    bestKey = key;
                }
            }
            if (best < 0)
                continue;
            auto &col = columns[best];
            auto tile = col.tile;
            occupied.erase({positions[i].x, positions[i].y});
            occupied[{tile.x, tile.y}] = i;
            carry[i] = (positions[i] == tile ? carry[i] : 0) + rate;
            positions[i] = tile;
            capturedNow[i] = true;
            bool complete = carry[i] >= field.capturePointsToCapture();
            result.enemyCaptureActions.append(QJsonObject{{"unit", pawn.engineUnitId},
                                                          {"tile", QJsonArray{tile.x, tile.y}},
                                                          {"points", carry[i]},
                                                          {"complete", complete}});
            if (complete)
            {
                if (!RelativeStockDetail::checkedAdd(result.stock, captureRaceSwing(col, true, 0, horizon)))
                    return {};
                transfers.push_back({col, true, 0, 0, 0, i, 0});
                col.ownerBefore = OwnerSign::Enemy;
                carry[i] = 0;
                if (tile == homeHQ)
                {
                    result.terminalSide = -1;
                    result.terminalTick = 1;
                    break;
                }
            }
        }
        MilliFunds firstFriendlyIncome=0,allEnemyIncome=0,allFriendlyIncome=0;
        if(openingIncomeMeasured)for(const auto &c:columns)
        {
            openingIncomeMeasured&=RelativeStockDetail::checkedAdd(allEnemyIncome,c.income.enemyPerTurn);
            openingIncomeMeasured&=RelativeStockDetail::checkedAdd(allFriendlyIncome,c.income.oursPerTurn);
            if(c.ownerBefore==OwnerSign::Ours)openingIncomeMeasured&=RelativeStockDetail::checkedAdd(firstFriendlyIncome,c.income.oursPerTurn);
        }
        if(openingIncomeMeasured)result.openingIncome=boundOpeningIncome(firstEnemyIncome,firstFriendlyIncome,
            allEnemyIncome,allFriendlyIncome,horizon);
        // The E0 capture orders have already occupied their endpoints. A defender may return only
        // on its own activation, by a legal route to the still-vacant HQ, or stay on the HQ it holds.
        const auto productionPositionsAfterE0 = positions;
        int supportActor = NO_UNIT;
        std::vector<std::array<int,2>> supportLoad;
        const auto spendAt = [&](int actor, int target, int distance) {
            if (!supportLoad.empty())
            {
                const auto gun = supportWeapon(terrain, actor, target, distance, supportLoad[actor]);
                spendCombatRound(supportLoad[actor], gun.slot);
            }
        };
        // This same ammunition history is used on both sides of the comparison.
        // If initialized before the vehicle pass, F1 orders advance it in place.
        auto prepareWeaponLoads = [&] {
            if (!supportLoad.empty()) return;
            // Recover each weapon's ammunition from the orders that actually occurred before
            // F1. A last primary round does not remove an unlimited secondary, or vice versa.
            if (terrain.weaponSlots)
            {
                if(terrain.orderedOpening) for(const auto &pawn:terrain.orderedOpening->afterService) supportLoad.push_back(pawn.rounds);
                else for (const auto &pawn : terrain.units) supportLoad.push_back(pawn.ammoByWeapon);
                if(!terrain.orderedOpening)for (const auto &actor : plan.actors)
                {
                    const auto &own = actor.ownShot;
                    if (own.target == NO_UNIT) continue;
                    const int i = problem.actors()[actor.actorSlot].knowledgeUnitIndex;
                    spendCombatRound(supportLoad[i], own.report.weapon);
                    if (own.report.counter >= 0) spendCombatRound(supportLoad[own.target], own.report.counterWeapon);
                }
                if(plan.openingRecruitFight)
                    spendCombatRound(supportLoad[plan.openingRecruitFight->shooter],plan.openingRecruitFight->weapon);
                std::size_t prefixStep=0;
                for (const auto &shot : plan.shots)
                {
                    if(prefixStep<plan.servicedStockShots.size()){
                        const auto &charged=plan.servicedStockShots[prefixStep++];
                        spendCombatRound(supportLoad[charged.shot.shooter],charged.native.weapon);
                        if(charged.native.counter>=0)spendCombatRound(supportLoad[charged.shot.target],charged.native.counterWeapon);
                        continue;
                    }
                    if(terrain.weaponSlots || shot.target>=plan.actors.size()){
                        const int t=targetIndex(shot);
                        const auto targetTile=shot.target<plan.actors.size()?
                            plan.replay.plan.action(plan.actors[shot.target].actionIndex).destination:terrain.units.at(t).tile;
                        const int dist=manhattanDistance(shot.firingTile,targetTile);
                        spendAt(shot.shooterIndex,t,dist);
                        spendCombatRound(supportLoad.at(t),shot.counterWeapon);
                        continue;
                    }
                    const auto &actor = plan.actors[shot.target];
                    const auto &entry = problem.actors()[actor.actorSlot];
                    const int a = shot.shooterIndex, t = entry.knowledgeUnitIndex;
                    const auto &choice = entry.candidates[actor.candidateIndex].source;
                    const int dist = manhattanDistance(shot.firingTile, choice.bundle.destination);
                    spendAt(a,t,dist);
                    if (shot.hpStepsAfter > 0 && dist == 1 && choice.replyFacts)
                    {
                        const auto &facts = *choice.replyFacts;
                        for (std::size_t j=0;j<facts.tile->shooters.size();++j)
                            if (facts.tile->shooters[j].shooterIndex==a && facts.tile->shooters[j].direct &&
                                facts.shots[j].counterRawBase>=0) { spendAt(t,a,dist); break; }
                    }
                }
                std::map<int,int> byId;
                for (int j=0; j<static_cast<int>(terrain.units.size()); ++j) byId[terrain.units[j].engineUnitId]=j;
                for (const auto &item : result.returnFire)
                {
                    const auto shot=item.toObject();
                    const int a=byId.at(shot["unit"].toInt()), t=byId.at(shot["target"].toInt());
                    const auto from=shot["from"].toArray(), to=shot["targetTile"].toArray();
                    const int dist=std::abs(from[0].toInt()-to[0].toInt())+std::abs(from[1].toInt()-to[1].toInt());
                    spendAt(a,t,dist);
                    if (dist==1 && shot["hpAfter"].toInt()>0) spendAt(t,a,dist);
                }
            }
            else
                for (int rounds:ammo) supportLoad.push_back({rounds,0});
        };

        std::optional<FriendlyServiceResult> friendlyService;
        std::vector<int> servicedFuel(hp.size(),-1);
        std::vector<bool> servicedQuote(hp.size());
        const auto remainingOwnFuel=[&](int unit,int f0Spent){
            if(servicedFuel[unit]>=0)return servicedFuel[unit];
            const auto fuel=knowledge.units()[unit].fuel;
            return fuel<0 ? -1 : std::max(0,fuel-f0Spent);
        };
        if(terrain.production && terrain.production->friendlyDayStart && horizon>0 && (result.terminalTick<0 || result.terminalTick>=2)){
            const auto &board=*terrain.production;
            if(board.friendlyOpening && openingIncomeMeasured && terrain.weaponSlots){
                auto bank=board.funds[0];bool valid=RelativeStockDetail::checkedAdd(bank,firstFriendlyIncome);
                for(const auto &purchase:*board.friendlyOpening)valid&=RelativeStockDetail::checkedSubtract(bank,purchase.price);
                if(!valid)return {};
                std::vector<int> fuel(hp.size());
                for(int i=0;i<static_cast<int>(fuel.size());++i)fuel[i]=knowledge.units()[i].fuel;
                for(const auto &a:plan.actors){
                    const auto &entry=problem.actors()[a.actorSlot];
                    fuel[entry.knowledgeUnitIndex]=remainingOwnFuel(entry.knowledgeUnitIndex,entry.candidates[a.candidateIndex].source.movementCost);
                }
                std::set<std::pair<int,int>> own;
                for(const auto &building:knowledge.buildings())if(building.ownerId==knowledge.observerId())own.emplace(building.x,building.y);
                for(const auto &c:columns)if(c.ownerBefore==OwnerSign::Ours)own.emplace(c.tile.x,c.tile.y);else own.erase({c.tile.x,c.tile.y});
                prepareWeaponLoads();
                friendlyService=board.friendlyDayStart(board,terrain,own,positions,real,fuel,supportLoad,
                    plan.openingRecruitFight?&*plan.openingRecruitFight:nullptr,bank,budget,result.friendlyServiceOpen);
                if(friendlyService){
                    QJsonArray serviced;
                    for(std::size_t j=0;j<friendlyService->subjects.size();++j){
                        const auto &s=friendlyService->subjects[j];const auto &bill=friendlyService->bill.rows[j];
                        const int i=bill.actor;QJsonObject event{{"hpBefore",bill.before/10.},{"hpAfter",bill.after/10.},
                            {"debit",static_cast<qint64>(bill.debit)},{"fuelAfter",s.fuel},{"roundsAfter",QJsonArray{s.rounds[0],s.rounds[1]}}};
                        if(i<static_cast<int>(hp.size())){
                            const bool fuelChangesReach=fuel[i]>=0 && fuel[i]<terrain.units[i].movementBeforeFuel && s.fuel>fuel[i];
                            servicedQuote[i]=(hp[i]!=ceilHpSteps(bill.after/10.) || supportLoad[i]!=s.rounds || fuelChangesReach);
                            if(fuelChangesReach && terrain.units[i].movement<terrain.units[i].movementBeforeFuel)
                                result.friendlyServiceOpen="F1_RESTORED_MOVEMENT_GRID_REQUIRES_REPRICE";
                            real[i]=bill.after/10.;hp[i]=futureHp[i]=ceilHpSteps(real[i]);
                            servicedFuel[i]=s.fuel;supportLoad[i]=s.rounds;
                            event["unit"]=terrain.units[i].engineUnitId;event["tile"]=QJsonArray{positions[i].x,positions[i].y};
                        }else{
                            const auto &b=board.friendlyOpening->at(i-static_cast<int>(hp.size()));
                            event["type"]=b.type;event["birth"]=QJsonArray{b.factory.x,b.factory.y};
                        }
                        serviced.append(event);
                    }
                    result.friendlyService={{"tick",2},{"before",static_cast<qint64>(friendlyService->bill.before)},
                        {"after",static_cast<qint64>(friendlyService->bill.after)},{"subjects",serviced},
                        {"materialGain",static_cast<qint64>(friendlyService->materialGain)},
                        {"materialLessCash",static_cast<qint64>(friendlyService->materialLessCash)}};
                    // 'losses' enters both the nominal ledger and its dated flow once.
                    losses.emplace_back(2,-friendlyService->materialLessCash);
                }
            }else result.friendlyServiceOpen="FRIENDLY_F1_WALLET_ROSTER_OR_AMMUNITION_UNVERIFIED";
        }
        std::vector<double> screenHp;
        if(terrain.screenFire)
        {
            screenHp.reserve(real.size());
            for(int i=0;i<static_cast<int>(real.size());++i)
            {
                const auto exact=plan.realAfterReply.find(i);
                // A missing decimal receipt cannot give the holder extra HP.
                // A surviving attacker, on the other hand, always receives full
                // strength and unlimited ammunition in the damage bound.
                const auto lower=friendlyService && servicedFuel[i]>=0?real[i]:exact!=plan.realAfterReply.end()?exact->second:
                    std::max(0.,std::min(terrain.units[i].realHp,static_cast<double>(hp[i]-1)));
                screenHp.push_back(lower);
            }
        }
        const auto marketResourceWork=5LL*static_cast<std::int64_t>(hp.size())+plan.actors.size()+plan.realAfterReply.size();
        if(marketResourceWork>static_cast<std::uint64_t>(std::max<std::int64_t>(0,budget)))return {};
        budget-=static_cast<std::int64_t>(marketResourceWork);
        const auto marketCarry=carry,marketSteps=hp;
        std::vector<double> marketLife(hp.size(),-1.);
        std::vector<int> marketFuel(hp.size(),-2);
        for(const auto &[actor,life]:plan.realAfterReply)marketLife.at(actor)=life;
        for(std::size_t i=0;i<hp.size();++i)if(servicedFuel[i]>=0)marketLife[i]=real[i];
        for(const auto &actor:friendlyActors) {
            const int i = actor.unit;
            marketFuel[i] = remainingOwnFuel(i,actor.projectedF0Movement);
            if (!actor.observed) continue;
            if (!terrain.pay(1)) return {};
            marketLife[i] = real[i];
            const auto &pawn = terrain.units[i];
            const bool alive = positions[i] != INVALID_TILE && futureHp[i] > 0;
            const auto rounds = supportLoad.empty() ? pawn.ammoByWeapon : supportLoad[i];
            result.observedF1Stock.append(QJsonObject{{"unit",pawn.engineUnitId},{"tick",2},
                {"observedAt",QJsonArray{pawn.tile.x,pawn.tile.y}},{"observedHp",pawn.realHp},
                {"tile",QJsonArray{positions[i].x,positions[i].y}},{"alive",alive},{"hp",real[i]},
                {"fuel",marketFuel[i]},{"rounds",QJsonArray{rounds[0],rounds[1]}},
                {"capturePoints",carry[i]},{"reservedGuard",i==reservedGuard},
                {"serviceBound",bool(friendlyService)},{"replayedOpeningActions",0}});
            if (alive && horizon>0 && result.terminalTick!=1 && !friendlyService && result.friendlyServiceOpen.isEmpty())
                result.friendlyServiceOpen="OBSERVED_F1_DAY_START_UNVERIFIED";
        }
        const ScreenOpening screen{productionPositionsAfterE0,screenHp,nullptr,false,marketCarry,marketSteps,marketFuel,marketLife};
        auto friendlyBirthsOnF1 = terrain.friendlyBirthSites;
        if (plan.openingRecruitFight && plan.openingRecruitFight->recruitAfter <= 0) {
            const auto tile = plan.openingRecruitFight->factory;
            friendlyBirthsOnF1.erase({tile.x,tile.y});
        }
        bool guardMoved = false;
        if (result.terminalTick != 1 && reservedGuard != NO_UNIT && horizon > 0)
        {
            const int i = reservedGuard;
            const auto &pawn = terrain.units[i];
            const TilePoint from = positions[i];
            bool covered = futureHp[i] > 0 && from != INVALID_TILE && from == homeHQ;
            if (!covered && futureHp[i] > 0 && from != INVALID_TILE && pawn.grid >= 0 &&
                std::find(positions.begin(), positions.end(), homeHQ) == positions.end())
            {
                const auto work = 2LL * terrain.width * terrain.height + static_cast<std::int64_t>(positions.size());
                if (budget < work)
                    return {};
                budget -= work;
                std::vector<TilePoint> enemies;
                for (size_t j = 0; j < positions.size(); ++j)
                    if (terrain.units[j].enemy && positions[j] != INVALID_TILE)
                        enemies.push_back(positions[j]);
                const auto &grid = terrain.grids[pawn.grid];
                ActivationField returnPath;
                returnPath.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(enemies), from,
                                 pawn.movement, 1, {homeHQ});
                guardMoved = covered = returnPath.activations(homeHQ.x, homeHQ.y) == 1;
            }
            result.guardAction = QJsonObject{{"unit", pawn.engineUnitId}, {"reservedOnF1", covered}, {"covered", covered},
                {"from", QJsonArray{from.x, from.y}}, {"to", QJsonArray{homeHQ.x, homeHQ.y}},
                {"returned", guardMoved}, {"clearableByStationaryVolley", false},
                {"abandonedPoints", guardMoved ? carry[i] : 0}};
            result.guardCovered = covered;
            if (covered)
            {
                positions[i] = homeHQ;
                holdFacts[i] = nullptr;
                carry[i] = progress[i] = 0;
                // Capture-capable units use the abstract F1 shot opportunity. A committed guard
                // cannot collect that and a separate capture/guard order in the same continuation.
                if (pawn.capper && nextShot[i] > 0)
                    losses.emplace_back(2, nextShot[i]);
            }
        }
        int boxedUnit = plan.encirclement.sealed ? plan.encirclement.targetKnowledge : NO_UNIT;
        bool boxedUnitFired = false;
        if(terrain.orderedOpening || friendlyService){
            if(!terrain.weaponSlots)return {};
            prepareWeaponLoads();
            for(std::size_t i=0;i<supportLoad.size();++i){
                const auto rounds=supportLoad[i];
                if(rounds[0]==0&&rounds[1]==0)ammo[i]=0;
                else{ammo[i]=std::numeric_limits<int>::max();for(int r:rounds)if(r>0)ammo[i]=std::min(ammo[i],r);}
            }
        }
        int homeCapturer = NO_UNIT;
        if (result.terminalTick!=1 && horizon>1 && homeHQ!=INVALID_TILE)
            for (int i=0;i<static_cast<int>(positions.size());++i)
            {
                const auto &unit=knowledge.units()[i];
                if (positions[i]!=homeHQ || !terrain.units[i].enemy || !unit.canCapture || futureHp[i]<=0 || carry[i]<=0)
                    continue;
                const int rate=replyCaptureRate(unit.captureRate,unit.hpRounded-futureHp[i],false);
                if (winsOnNextCapture(carry[i],rate,field.capturePointsToCapture()))
                {
                    homeCapturer=i;
                    result.homeCaptureResponse=QJsonObject{{"urgent",true},{"focused",interruptHomeCapture},
                        {"target",terrain.units[i].engineUnitId},{"tile",QJsonArray{homeHQ.x,homeHQ.y}},
                        {"pointsAfterE0",carry[i]},{"threshold",field.capturePointsToCapture()},
                        {"hpBeforeF1",real[i]},{"nextRateBefore",rate}};
                    prepareWeaponLoads();
                }
                break;
            }
        const auto homeRate = [&] {
            if (homeCapturer==NO_UNIT || futureHp[homeCapturer]<=0) return 0;
            const auto &unit=knowledge.units()[homeCapturer];
            return replyCaptureRate(unit.captureRate,unit.hpRounded-futureHp[homeCapturer],false);
        };
        const auto winsNextCapture = [&] {
            const int rate=homeRate();
            return homeCapturer!=NO_UNIT && winsOnNextCapture(carry[homeCapturer],rate,field.capturePointsToCapture());
        };
        std::set<int> homeCapperShots;
        std::vector<int> vehicleOnTarget(hp.size(), NO_UNIT);
        if (result.terminalTick != 1 && terrain.weapon && terrain.defense)
        {
            // E0 shooters and cappers have committed to known endpoints for the F1 response.
            // Every vehicle gets one action. A vehicle cannot fire through an occupied route,
            // claim a second enemy kill, or collect its old day-start best-shot estimate as well.
            auto fireActors=friendlyActors;
            if(interruptHomeCapture && homeCapturer!=NO_UNIT)
                std::stable_partition(fireActors.begin(),fireActors.end(),[&](const auto &a){return !terrain.units[a.unit].capper;});
            for(const auto &actor:fireActors)
            {
                const int i=actor.unit;
                const auto &u = knowledge.units()[i];
                const auto &pawn = terrain.units[i];
                const bool emergency=interruptHomeCapture && homeCapturer!=NO_UNIT && winsNextCapture();
                if (pawn.capper && !emergency)
                    continue;
                if (!pawn.capper && !RelativeStockDetail::checkedAdd(result.replacedNextShot, nextShot[i]))
                    return {};
                if ((i == reservedGuard && guardMoved) || !(u.canFire || (friendlyService && pawn.hasNativeWeapon)) || positions[i] == INVALID_TILE || futureHp[i] <= 0 ||
                    (supportLoad.empty() ? ammo[i]<=0 : (supportLoad[i][0]==0 && supportLoad[i][1]==0)))
                    continue;
                const int remainingFuel=remainingOwnFuel(i,actor.projectedF0Movement);
                const int movement=(actor.observed || homeCapturer!=NO_UNIT) && remainingFuel>=0
                    ? std::min(pawn.movement,remainingFuel) : pawn.movement;
                const auto powerAt=[&](int shooter,int victim,int range) {
                    return supportLoad.empty() ? terrain.weapon(shooter,victim,range)
                                               : supportWeapon(terrain,shooter,victim,range,supportLoad[shooter]).base;
                };
                std::vector<TilePoint> blockers, tiles;
                std::set<std::pair<int, int>> endpoints;
                for (int j = 0; j < static_cast<int>(positions.size()); ++j)
                    if (j != i && positions[j] != INVALID_TILE && futureHp[j] > 0)
                    {
                        endpoints.emplace(positions[j].x, positions[j].y);
                        if (terrain.units[j].enemy)
                            blockers.push_back(positions[j]);
                    }
                endpoints.insert(friendlyBirthsOnF1.begin(),friendlyBirthsOnF1.end());
                const bool mobile = u.canMoveAndFire && pawn.grid >= 0 && positions[i] != homeHQ;
                if (mobile)
                {
                    for (int y = std::max(0, positions[i].y - movement);
                         y <= std::min(terrain.height - 1, positions[i].y + movement); ++y)
                        for (int x = std::max(0, positions[i].x - movement);
                             x <= std::min(terrain.width - 1, positions[i].x + movement); ++x)
                            if (!endpoints.contains({x, y}))
                                tiles.push_back({x, y});
                }
                else
                    tiles.push_back(positions[i]);
                std::vector<int> possibleTargets;
                const int radius = mobile ? movement : 0;
                for (int a = 0; a < static_cast<int>(fired.size()); ++a)
                {
                    if ((pawn.capper && a!=homeCapturer) || !terrain.units[a].enemy || futureHp[a] <= 0 || positions[a] == INVALID_TILE ||
                        !(fired[a] || capturedNow[a] || blockedExit[a] || a == plan.enemyWaiting || a==homeCapturer || !u.canMoveAndFire))
                        continue;
                    // Every generated endpoint is within this square. A target outside
                    // its weapon range cannot take part in the endpoint-by-target loop.
                    const int dx=std::max(0,std::abs(positions[a].x-positions[i].x)-radius);
                    const int dy=std::max(0,std::abs(positions[a].y-positions[i].y)-radius);
                    if (dx+dy<=u.maxRange)
                        possibleTargets.push_back(a);
                }
                const auto scan = static_cast<std::int64_t>(positions.size()+tiles.size());
                if (scan>budget) return {};
                budget-=scan;
                if (possibleTargets.empty()) continue;
                const auto cost = (mobile ? 2LL*terrain.width*terrain.height : 0) +
                    static_cast<std::int64_t>(tiles.size()) * static_cast<std::int64_t>(possibleTargets.size());
                if (cost > budget)
                    return {};
                budget -= cost;
                ActivationField reach;
                if (mobile)
                {
                    const auto &grid = terrain.grids[pawn.grid];
                    reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(blockers), positions[i],
                                movement, 1, tiles);
                }
                MilliFunds best = 0;
                int target = NO_UNIT;
                TilePoint end = positions[i];
                double toHp = 0, fromHp = 0;
                bool counterFired = false;
                for (int a : possibleTargets)
                        for (auto tile : tiles)
                        {
                            if (mobile && reach.activations(tile.x, tile.y) != 1)
                                continue;
                            const int dist = std::abs(tile.x - positions[a].x) + std::abs(tile.y - positions[a].y);
                            if (dist < u.minRange || dist > u.maxRange)
                                continue;
                            if (!u.canMoveAndFire && !fired[a] && !capturedNow[a] && !blockedExit[a] &&
                                a != plan.enemyWaiting)
                            {
                                auto escape=indirectTargetCanEscape(terrain,positions,futureHp,a,tile,u.minRange,u.maxRange);
                                if (!escape) return {};
                                if (*escape) continue;
                            }
                            const double power = powerAt(i, a, dist);
                            if (power <= 0)
                                continue;
                            const auto rawPrimary=terrainReducedDamage(terrain.defense(a,positions[a]),power,
                                futureHp[i],futureHp[a]);
                            const auto damage=engineShotDamage(rawPrimary);
                            const double remaining = engineHpAfterReply(real[a], damage);
                            const int after = ceilHpSteps(remaining);
                            const auto counter =
                                dist == 1 && (supportLoad.empty() ? ammo[a]>0 : (supportLoad[a][0]!=0 || supportLoad[a][1]!=0)) && after > 0 ? powerAt(a, i, dist) : -1.;
                            const double back =
                                counter > 0 ? engineShotDamage(sameActionCounterDamage(terrain.defense(i,tile),counter,
                                    real[a],rawPrimary,futureHp[i]))
                                            : 0.;
                            const double surviving = engineHpAfterReply(real[i], back);
                            const auto net = terrain.ownFire ? combatCapitalLoss(terrain.units[a].replacementCost, real[a], remaining) -
                                             combatCapitalLoss(pawn.replacementCost, real[i], surviving) :
                                             bookDamage(terrain.units[a].replacementCost, futureHp[a] - after) -
                                             bookDamage(pawn.replacementCost, futureHp[i] - ceilHpSteps(surviving));
                            const bool interrupts=emergency && a==homeCapturer && remaining<real[a];
                            const bool alreadyInterrupts=emergency && target==homeCapturer;
                            if (pawn.capper && !interrupts) continue;
                            if (interrupts!=alreadyInterrupts)
                            {
                                if (!interrupts) continue;
                            }
                            else if (net<best || (net==best && (net==0 || tile!=positions[i]))) continue;
                            best = net;
                            target = a;
                            end = tile;
                            toHp = remaining;
                            fromHp = surviving;
                            counterFired = counter>0;
                        }
                if (target == NO_UNIT)
                    continue;
                if (i == reservedGuard && result.guardCovered && horizon > 1 && fromHp < real[i])
                {
                    if (fromHp <= 0)
                    {
                        result.guardAction["declinedUnsafeShot"] = true;
                        continue;
                    }
                    auto wait = headquartersGuardVolley(terrain, knowledge, positions, futureHp, real, ammo, i, homeHQ);
                    if (!wait)
                        return {};
                    if (!wait->cleared)
                    {
                        if (!terrain.pay(3LL * positions.size()))
                            return {};
                        auto trialHp = futureHp, trialAmmo = ammo;
                        auto trialReal = real;
                        trialReal[i] = fromHp;
                        trialHp[i] = ceilHpSteps(fromHp);
                        trialReal[target] = toHp;
                        trialHp[target] = ceilHpSteps(toHp);
                        --trialAmmo[target];
                        --trialAmmo[i];
                        auto fire = headquartersGuardVolley(terrain, knowledge, positions, trialHp, trialReal,
                                                            trialAmmo, i, homeHQ);
                        if (!fire)
                            return {};
                        if (fire->cleared)
                        {
                            result.guardAction["declinedUnsafeShot"] = true;
                            continue;
                        }
                    }
                }
                if (boxedUnit == i)
                    boxedUnitFired = true;
                result.returnFire.append(
                    QJsonObject{{"unit", pawn.engineUnitId},
                                {"target", terrain.units[target].engineUnitId},
                                {"from", QJsonArray{end.x, end.y}},
                                {"targetTile", QJsonArray{positions[target].x, positions[target].y}},
                                {"hpBefore", futureHp[target]},
                                {"hpAfter", ceilHpSteps(toHp)},
                                {"value", static_cast<qint64>(best)}});
                const auto from=positions[i];
                if (pawn.capper)
                {
                    homeCapperShots.insert(i);
                    if(routeCache && routeCache->completedF1Queries && !actor.observed){
                        const auto &last=result.returnFire.last().toObject();
                        QJsonObject quote{{"unit",pawn.engineUnitId},{"afterReply",qint64(nextShot[i])},{"atFriendlyAction",qint64(0)},
                            {"afterPriorFriendlyFire",result.returnFire.size()-1},{"replacement","SPENT_F1_FIRE"},{"fire",last}};
                        QJsonObject scoped{{"quote",quote},{"source","REALIZED_F1_FIRE_REPLACES_NEXT_SHOT"},{"policy",routeCache->stockComparisonPolicy},
                            {"f1ReturnFire",result.returnFire},{"scope","COND_F1_SHOT_ONLY"},{"hold",stayForCapture},{"guard",reservedGuard},
                            {"followup",takeCombinedFollowup},{"punish",punishCombinedFollowup},{"defendHome",interruptHomeCapture},{"approachBranch",int(branch)}};
                        const auto key=QString::fromLatin1(F1OpportunityShelf::hash(QJsonDocument(scoped).toJson(QJsonDocument::Compact)));
                        scoped["query"]=key;if(routeCache->completedF1Queries->size()<128)(*routeCache->completedF1Queries)[key]=scoped;
                    }
                    if (!RelativeStockDetail::checkedAdd(result.replacedHomeCapperNextShot,nextShot[i])) return {};
                    nextShot[i]=0;
                    holdFacts[i]=nullptr;
                    if (end!=from) carry[i]=progress[i]=0;
                    lastShot[i]=1;
                }
                else if (toHp < real[target])
                    vehicleOnTarget[target] = i;
                if (!RelativeStockDetail::checkedAdd(result.returnValue, best))
                    return {};
                const int distance=manhattanDistance(end,positions[target]);
                spendAt(i,target,distance);
                if (counterFired)
                {
                    spendAt(target,i,distance);
                    ammo[target]=std::max(0,ammo[target]-1);
                }
                ammo[i]=std::max(0,ammo[i]-1);
                lastTargetTick[target]=2;
                real[target] = toHp;
                futureHp[target] = ceilHpSteps(toHp);
                real[i] = fromHp;
                futureHp[i] = ceilHpSteps(fromHp);
                positions[i] = futureHp[i] > 0 ? end : INVALID_TILE;
                if (futureHp[target] == 0)
                    positions[target] = INVALID_TILE;
            }
        }
        // A rescued capper's earlier opportunity was quoted before enemy captures and friendly vehicle fire.
        // Replace that amount once, then pass the same quote to its capture/withdraw decision.
        const bool changedTargets = positions != plan.combatLocations || futureHp != hp;
        std::vector<bool> repricedOnF1(hp.size());
        std::vector<MilliFunds> pendingShotAdjustments(hp.size());
        auto quoteActors=friendlyActors;
        if(routeCache && routeCache->completedF1Queries)
            std::stable_partition(quoteActors.begin(),quoteActors.end(),[](const auto &a){return !a.observed;});
        // Physical FIRE has already finished. These independent opportunities
        // read the same F1 board; apply their adjustments in the original order.
        for(const auto &actor:quoteActors){
            const int i=actor.unit;
            if(!terrain.units[i].capper || !(knowledge.units()[i].canFire || (friendlyService && terrain.units[i].hasNativeWeapon)) || homeCapperShots.contains(i) ||
               (result.guardCovered && i==reservedGuard))continue;
            // Once original and purchased pieces share an ordinary opening,
            // all surviving cappers face the same dated board. Even an unhurt
            // shooter can lose a shot when its target moves or a vehicle kills it.
            const bool physicalOpening=terrain.production && terrain.production->friendlyDayStart;
            if(!physicalOpening && !servicedQuote[i])continue;
            if(result.terminalTick==1 || horizon<1 || futureHp[i]<=0 || positions[i]==INVALID_TILE)
            {
                if(!RelativeStockDetail::checkedSubtract(pendingShotAdjustments[i],nextShot[i]))return {};
                nextShot[i]=0;repricedOnF1[i]=true;
                continue;
            }
            if(!terrain.weaponSlots){result.friendlyServiceOpen="F1_CAPPER_SHOT_NATIVE_WEAPONS_UNAVAILABLE";continue;}
            const auto &pawn=terrain.units[i];const auto &u=knowledge.units()[i];SurvivorWeaponFacts weapon;
            if(u.canMoveAndFire && pawn.movementBeforeFuel>0 && pawn.travelGrid<0)
            {result.friendlyServiceOpen="F1_CAPPER_SHOT_MOVEMENT_GRID_UNAVAILABLE";continue;}
            prepareWeaponLoads();
            const int spent=actor.projectedF0Movement;
            weapon.movement=pawn.movementBeforeFuel;weapon.fuel=remainingOwnFuel(i,spent);weapon.ignoresCollision=pawn.ignoresCollision;
            if(u.canMoveAndFire && pawn.travelGrid>=0)weapon.grid=terrain.grids[pawn.travelGrid];
            SurvivorPosition state;state.ammo=supportLoad[i];
            for(int j=0;j<static_cast<int>(positions.size());++j){
                if(!terrain.units[j].enemy){if(futureHp[j]>0)state.friendly[j]=positions[j];continue;}
                state.enemy[j]={positions[j],futureHp[j]};if(positions[j]==INVALID_TILE || futureHp[j]<=0)continue;
                SurvivorTarget target{j,positions[j],futureHp[j],terrain.units[j].replacementCost,{}};
                const auto limit=std::min(u.maxRange,terrain.width+terrain.height-2);
                for(int range=0;range<=limit;++range){if(!terrain.pay(1))return {};
                    target.baseByRange.push_back(terrain.weaponSlots(i,j,range));}
                weapon.targets.push_back(std::move(target));
            }
            int paid=-1;
            for(auto [x,y]:friendlyBirthsOnF1) state.friendly[paid--]={x,y};
            const auto work=weapon.work(terrain.width,terrain.height);
            std::optional<SurvivorShotChoice> opportunity;std::optional<MilliFunds> revised;
            QByteArray query;QString source;
            if(!actor.observed && routeCache && routeCache->f1OpportunityShelf){
                const auto *grid=weapon.grid?&terrain.grids.at(pawn.travelGrid):nullptr;
                revised=routeCache->f1OpportunityShelf->price(weapon,state,positions[i],futureHp[i],grid,work,budget,opportunity,query,source);
            }else if(budget<work){
                F1MovementQuery query(weapon,state,positions[i]);QJsonArray cut;
                revised=query.priceSparse(futureHp[i],budget,opportunity,cut);
                if(!revised)return refuseWork("F1_SURVIVOR_SHOT",work);
            }else{
                budget-=work;revised=survivorShotValue(weapon,state,positions[i],0,futureHp[i],&opportunity);
            }
            if(!revised || !RelativeStockDetail::checkedAdd(pendingShotAdjustments[i],*revised-nextShot[i]))return {};
            QJsonObject quote{{"unit",pawn.engineUnitId},{"afterReply",qint64(nextShot[i])},{"atFriendlyAction",qint64(*revised)},
                {"friendlyF1Service",bool(servicedQuote[i])},{"tick",2},{"afterPriorFriendlyFire",result.returnFire.size()}};
            if(opportunity)
            {
                const auto &shot=*opportunity;const auto target=positions[shot.target];
                quote["target"]=terrain.units[shot.target].engineUnitId;quote["targetAt"]=QJsonArray{target.x,target.y};
                quote["targetHp"]=real[shot.target];quote["from"]=QJsonArray{shot.from.x,shot.from.y};quote["weapon"]=shot.weapon;
            }
            if(routeCache && routeCache->completedF1Queries && !query.isEmpty()){
                QJsonObject scoped{{"quote",quote},{"source",source},{"query",QString::fromLatin1(query)},
                    {"policy",routeCache->stockComparisonPolicy},{"f1ReturnFire",result.returnFire},
                    {"scope","COND_F1_SHOT_ONLY"},{"hold",stayForCapture},{"guard",reservedGuard},{"followup",takeCombinedFollowup},
                    {"punish",punishCombinedFollowup},{"defendHome",interruptHomeCapture},{"approachBranch",int(branch)}};
                QJsonArray approaches;
                for(const auto *a:{approach,priorDecline})if(a)
                    approaches.append(QJsonArray{a->actor,a->shooter,a->origin.x,a->origin.y,a->stage.x,a->stage.y,a->target.x,a->target.y,
                        a->gun.x,a->gun.y,a->firstCost,a->rate,a->finish});
                scoped["approaches"]=approaches;
                if(const auto &post=routeCache->fundedPostCommitment;post)scoped["fundedPost"]=QJsonArray{
                    post->side,post->factory.x,post->factory.y,post->buildTick,post->type,post->post.x,post->post.y};
                QJsonArray adverse;
                for(const auto &hit:plan.shots)adverse.append(QJsonArray{terrain.units.at(hit.shooterIndex).engineUnitId,
                    hit.targetEngineUnitId,hit.firingTile.x,hit.firingTile.y,hit.realBefore,hit.realAfter,hit.counterWeapon});
                scoped["orderedE0Fire"]=adverse;
                const auto key=QString::fromLatin1(F1OpportunityShelf::hash(QJsonDocument(scoped).toJson(QJsonDocument::Compact)));
                if(routeCache->completedF1Queries->size()<128)(*routeCache->completedF1Queries)[key]=scoped;
            }
            result.survivorNextShots.append(quote);
            nextShot[i]=*revised;repricedOnF1[i]=true;
        }
        for(const auto &actor:friendlyActors)
            if(!RelativeStockDetail::checkedAdd(result.survivorNextShotAdjustment,pendingShotAdjustments[actor.unit]))return {};
        for (const auto &actor : plan.actors)
        {
            const auto &entry = problem.actors()[actor.actorSlot];
            const int i = entry.knowledgeUnitIndex;
            if (repricedOnF1[i] || !actor.repricedSurvivor || !terrain.units[i].capper || homeCapperShots.contains(i) || (result.guardCovered && i == reservedGuard))
                continue;
            const auto previous = nextShot[i];
            if (result.terminalTick == 1 || horizon < 1)
                nextShot[i] = 0;
            else if (changedTargets)
            {
                const auto &choice = entry.candidates[actor.candidateIndex].source;
                const auto refreshed = survivorShotAtFriendlyAction(*choice.replyFacts->survivorWeapon,
                    actor.survivorAmmo, terrain, positions, futureHp, i, choice.movementCost, budget);
                if (!refreshed)
                    return {};
                nextShot[i] = *refreshed;
            }
            if (!RelativeStockDetail::checkedAdd(result.survivorNextShotAdjustment, nextShot[i] - previous))
                return {};
            result.survivorNextShots.append(QJsonObject{{"unit", actor.engineUnitId},
                {"afterReply", static_cast<qint64>(previous)}, {"atFriendlyAction", static_cast<qint64>(nextShot[i])}});
        }
        if(routeCache && !proposalOnly){
            f1Quotes=result.survivorNextShots;f1Fires=result.returnFire;f1Quoted=true;
        }
        if(requestedFollowup && (horizon<=0 || result.terminalTick==1 || result.returnFire.isEmpty()))
            return {};
        if (horizon > 0 && result.terminalTick != 1 && !result.returnFire.isEmpty())
        {
            std::vector<int> movement(hp.size(), -1);
            for (const auto &actor : friendlyActors)
            {
                const int i=actor.unit;
                if (homeCapperShots.contains(i)) continue;
                if (!terrain.units[i].capper || (i == reservedGuard && result.guardCovered))
                    continue;
                const auto &unit = knowledge.units()[i];
                const int spent=actor.projectedF0Movement;
                movement[i] = unit.fuel < 0 ? unit.movementPoints : std::min(unit.movementPoints,remainingOwnFuel(i,spent));
            }
            // Limit only the optional extra search. Preserve enough shared work for the baseline capture forecast.
            const auto quota=terrain.quoteWorkQuota(budget,12,150000);
            prepareWeaponLoads();
            std::optional<CombinedFollowupShot> offer;
            const auto describeOffer=[&](const CombinedFollowupShot &q) {
                const auto i=q.actor,t=q.target;
                return QJsonObject{{"unit",terrain.units[i].engineUnitId},{"target",terrain.units[t].engineUnitId},
                    {"vehicle",terrain.units[q.vehicle].engineUnitId},{"end",QJsonArray{q.end.x,q.end.y}},
                    {"actorHpBefore",real[i]},{"actorHpAfter",q.actorAfter},{"targetHpBefore",real[t]},
                    {"targetHpAfter",q.targetAfter},{"moveCost",q.movementCost},{"weapon",q.slot},{"counter",q.counterSlot},
                    {"previousShot",qint64(nextShot[i])},{"captureCarry",carry[i]},{"net",qint64(q.net)},
                    {"selectedSourceFamily",q.net>0 && q.net>=nextShot[i]}};
            };
            if (!quota || requestedFollowup) {
                // A reduced offer can still enumerate the F1 actions. A partial
                // result never makes an unseen action disappear.
                FollowupFrontier frontier;
                const auto start=budget;
                findCombinedFollowup(terrain,knowledge.units(),positions,futureHp,real,ammo,
                    vehicleOnTarget,movement,nextShot,budget,supportLoad,friendlyBirthsOnF1,&frontier);
                QJsonArray feasible;
                bool hasSourceOffer=false;
                for(const auto &q:frontier.shots) {
                    const auto i=q.actor,t=q.target;
                    const bool admissible=q.net>0 && q.net>=nextShot[i];
                    hasSourceOffer|=admissible;
                    feasible.append(describeOffer(q));
                }
                result.followupDecision={{"tick",2},{"includesDecline",true},{"shots",feasible},
                    {"sourceFriendlyMenuComplete",frontier.friendlyComplete},{"workExhausted",frontier.workExhausted},
                    {"spent",qint64(start-budget)},{"enemyMenuComplete",false},{"fullRank",false}};
                result.followupDecision["declinePriorKnown"]=frontier.friendlyComplete && !hasSourceOffer;
                if(const auto upper=upperOptionalFollowupMaterial(frontier))
                    result.followupDecision["localMaterialUpper"]=qint64(*upper);
                if(requestedFollowup) {
                    const auto &wanted=*requestedFollowup;
                    auto selected=std::find_if(frontier.shots.begin(),frontier.shots.end(),[&](const auto &q) {
                        return terrain.units[q.actor].engineUnitId==wanted.actor &&
                            terrain.units[q.target].engineUnitId==wanted.target &&
                            terrain.units[q.vehicle].engineUnitId==wanted.vehicle &&
                            q.end==wanted.end && q.slot==wanted.weapon;
                    });
                    if(selected==frontier.shots.end()) {
                        const auto gap=refuseWork("REQUESTED_FOLLOWUP_UNAVAILABLE",0);
                        if(routeCache)routeCache->lastFailure["followupDecision"]=result.followupDecision;
                        return gap;
                    }
                    // This is an instruction, not a cached value. Rebind the
                    // exchange and the selected actor's prior shot to this
                    // exact team and board before replaying the normal tail.
                    offer=*selected;
                    result.followupDecision["requestedOrderRevalidated"]=true;
                } else if(!frontier.friendlyComplete || hasSourceOffer) {
                    const auto gap=refuseWork("COMBINED_FOLLOWUP_SUCCESSOR",150000);
                    if(routeCache)routeCache->lastFailure["followupDecision"]=result.followupDecision;
                    return gap;
                } else {
                    // All source-admissible extra shots are absent. The
                    // legal but nonpositive shots remain available to replay.
                    offer=CombinedFollowupShot{};
                }
            } else {
                const auto allowance=*quota;
                auto searchLeft=allowance;
                bool selectionComplete=false;
                FollowupFrontier alternatives;
                offer=findCombinedFollowup(terrain,knowledge.units(),positions,futureHp,real,ammo,
                    vehicleOnTarget,movement,nextShot,searchLeft,supportLoad,friendlyBirthsOnF1,&alternatives,&selectionComplete);
                budget-=allowance-searchLeft;
                if(!alternatives.shots.empty() || !selectionComplete) {
                    QJsonArray all;for(const auto &q:alternatives.shots)all.append(describeOffer(q));
                    result.followupDecision={{"tick",2},{"shots",all},{"includesDecline",true},{"declinePriorKnown",selectionComplete},
                        {"sourceFriendlyMenuComplete",alternatives.friendlyComplete},{"enemyMenuComplete",false},
                        {"workExhausted",alternatives.workExhausted},{"fullRank",false}};
                    if(selectionComplete && offer && offer->actor!=NO_UNIT)result.followupDecision["declineOffer"]=describeOffer(*offer);
                }
                if(!selectionComplete){
                    // A provisional local offer can guide search. Its decline
                    // and prior opportunity debit require the selected real
                    // actor, even when one candidate has already been found.
                    result.followupDecision["tick"]=2;result.followupDecision["status"]="SEARCH_LIMIT";
                    result.followupDecision["includesDecline"]=true;result.followupDecision["sourceSelectorComplete"]=false;
                    result.followupDecision["workExhausted"]=true;result.followupDecision["fullRank"]=false;
                    result.followupDecision["unbound"]="SELECTED_ACTOR_PRIOR_SHOT_AND_REMAINING_FOLLOWUPS";
                }
            }
            if (!offer)
                result.combinedFollowup = {{"status", "SEARCH_LIMIT"}};
            else if (offer->actor != NO_UNIT)
            {
                const int i = offer->actor, t = offer->target;
                const int before = futureHp[t], after = ceilHpSteps(offer->targetAfter);
                const auto origin = positions[i];
                result.combinedFollowup = {{"status", takeCombinedFollowup ? "TAKEN" : "AVAILABLE"},
                    {"unit", terrain.units[i].engineUnitId}, {"vehicle", terrain.units[offer->vehicle].engineUnitId},
                    {"target", terrain.units[t].engineUnitId}, {"origin", QJsonArray{origin.x,origin.y}},
                    {"from", QJsonArray{offer->end.x,offer->end.y}}, {"targetTile", QJsonArray{positions[t].x,positions[t].y}},
                    {"hpBefore", before}, {"hpAfter", after}, {"net", static_cast<qint64>(offer->net)},
                    {"previousShot", static_cast<qint64>(nextShot[i])}};
                // These two physical continuations compare this capper spending F1
                // on the pair versus retaining it for capture, guard or movement.
                // Neither one can also collect a day-start estimate of a different order.
                result.replacedCapperNextShot = nextShot[i];
                nextShot[i] = 0;
                if (takeCombinedFollowup)
                {
                    supportActor = i;
                    if (!RelativeStockDetail::checkedAdd(result.returnValue, offer->net)) return {};
                    result.returnFire.append(QJsonObject{{"unit",terrain.units[i].engineUnitId},
                        {"target",terrain.units[t].engineUnitId}, {"from",QJsonArray{offer->end.x,offer->end.y}},
                        {"targetTile",QJsonArray{positions[t].x,positions[t].y}}, {"hpBefore",before}, {"hpAfter",after},
                        {"value",static_cast<qint64>(offer->net)}, {"afterVehicle",terrain.units[offer->vehicle].engineUnitId}});
                    --ammo[i];
                    spendCombatRound(supportLoad[i],offer->slot);
                    if (offer->countered) { --ammo[t]; spendCombatRound(supportLoad[t],offer->counterSlot); }
                    lastTargetTick[t]=2;
                    real[t] = offer->targetAfter;
                    real[i] = offer->actorAfter;
                    futureHp[t] = after;
                    futureHp[i] = ceilHpSteps(real[i]);
                    if (after == 0) positions[t] = INVALID_TILE;
                    positions[i] = futureHp[i] > 0 ? offer->end : INVALID_TILE;
                    if (positions[i] != origin) carry[i] = progress[i] = 0;
                    holdFacts[i] = nullptr;
                    lastShot[i] = 1;
                }
            }
        }
        if (homeCapturer!=NO_UNIT)
        {
            QJsonArray shots;
            for (const auto &v : result.returnFire)
                if (v.toObject().value("target").toInt(NO_UNIT)==terrain.units[homeCapturer].engineUnitId)
                    shots.append(v);
            result.homeCaptureResponse["shots"]=shots;
            result.homeCaptureResponse["hpAfterF1"]=real[homeCapturer];
            result.homeCaptureResponse["nextRateAfter"]=homeRate();
            result.homeCaptureResponse["secured"]=!winsNextCapture();
            result.homeCaptureResponse["cleared"]=futureHp[homeCapturer]<=0;
        }
        // Capture progress is optional on the next friendly turn for both WAIT and CAPTURE.
        // Waive further holding damage only when the unit actually spends F1 moving to
        // a reachable free tile outside the surviving enemies' possible E1 weapon ranges.
        if ((!stayForCapture || boxedUnit != NO_UNIT) && horizon > 1 && result.terminalTick != 1)
        {
            for (int i = 0; i < static_cast<int>(positions.size()); ++i)
            {
                const bool boxEscape = i == boxedUnit && !boxedUnitFired &&
                    !(result.guardCovered && i == reservedGuard);
                if (futureHp[i] <= 0 || positions[i] == INVALID_TILE ||
                    (!boxEscape && (stayForCapture || !holdFacts[i] || holdFacts[i]->tile->shooters.empty())))
                    continue;
                const auto &u = knowledge.units()[i];
                const auto &pawn = terrain.units[i];
                const int rate = replyCaptureRate(u.captureRate, u.hpRounded - futureHp[i], false);
                if (!boxEscape && !unstarted[i] && carry[i] + rate >= field.capturePointsToCapture())
                    continue;
                if (pawn.grid < 0 || pawn.movement < 1)
                    continue;
                const auto from = positions[i];
                std::vector<TilePoint> enemies, exits;
                for (int j = 0; j < static_cast<int>(positions.size()); ++j)
                    if (terrain.units[j].enemy && positions[j] != INVALID_TILE && futureHp[j] > 0)
                        enemies.push_back(positions[j]);
                const int mp = pawn.movement;
                for (int y = std::max(0, from.y - mp); y <= std::min(terrain.height - 1, from.y + mp); ++y)
                    for (int x = std::max(0, from.x - mp); x <= std::min(terrain.width - 1, from.x + mp); ++x)
                    {
                        const TilePoint tile{x, y};
                        if (tile == from || std::abs(x - from.x) + std::abs(y - from.y) > mp)
                            continue;
                        const auto cost = 1 + static_cast<std::int64_t>(positions.size());
                        if (budget < cost)
                            return {};
                        budget -= cost;
                        if (friendlyBirthsOnF1.contains({tile.x,tile.y}) ||
                            std::find(positions.begin(), positions.end(), tile) != positions.end())
                            continue;
                        bool exposed = false;
                        for (int j = 0; j < static_cast<int>(positions.size()); ++j)
                        {
                            const auto &foe = terrain.units[j];
                            const auto &enemy = knowledge.units()[j];
                            if (!foe.enemy || positions[j] == INVALID_TILE || futureHp[j] <= 0 || !enemy.canFire)
                                continue;
                            const int distance = std::abs(x - positions[j].x) + std::abs(y - positions[j].y);
                            const int movement = enemy.canMoveAndFire ? foe.movement : 0;
                            if (distance <= enemy.maxRange + movement && distance + movement >= enemy.minRange)
                            {
                                exposed = true;
                                break;
                            }
                        }
                        if (!exposed)
                            exits.push_back(tile);
                    }
                if (exits.empty())
                    continue;
                const auto cost = 2LL * terrain.width * terrain.height + exits.size();
                if (cost > static_cast<std::uint64_t>(budget))
                    return {};
                budget -= cost;
                ActivationField routes;
                const auto &grid = terrain.grids[pawn.grid];
                routes.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(enemies), from, mp, 1, exits);
                std::stable_sort(exits.begin(), exits.end(), [&](auto a, auto b) {
                    return std::abs(a.x - from.x) + std::abs(a.y - from.y) <
                           std::abs(b.x - from.x) + std::abs(b.y - from.y);
                });
                auto to = std::find_if(exits.begin(), exits.end(),
                                       [&](auto tile) { return routes.activations(tile.x, tile.y) == 1; });
                if (to == exits.end())
                    continue;
                result.hasOptionalHold |= !boxEscape;
                withdrew[i] = true;
                holdFacts[i] = nullptr;
                result.withdrawals.append(QJsonObject{{"unit", pawn.engineUnitId},
                                                      {"from", QJsonArray{from.x, from.y}},
                                                      {"to", QJsonArray{to->x, to->y}},
                                                      {"abandonedPoints", carry[i]},
                                                      {"dayOffset", 1}});
                positions[i] = *to;
                carry[i] = 0;
                progress[i] = 0;
                if (!boxEscape && nextShot[i] > 0)
                    losses.emplace_back(2, nextShot[i]);
            }
        }
        // An open exit does not undo a shot already spent in place. Follow up at the center
        // unless the vehicle actually escaped or was removed during the modeled friendly turn.
        const auto &box = plan.encirclement;
        int confined = NO_UNIT;
        const ReplyFacts *confinedFacts = nullptr;
        QJsonArray boxShots;
        if (box.sealed && box.targetKnowledge != NO_UNIT)
        {
            const int i = box.targetKnowledge;
            bool closed = futureHp[i] > 0 && positions[i] == box.center;
            for (const auto &exit : box.exits)
                if (exit.passable && (exit.unitIndex == NO_UNIT || futureHp[exit.unitIndex] <= 0 ||
                                      positions[exit.unitIndex] != exit.tile))
                    closed = false;
            result.boxContinuation = QJsonObject{{"unit", box.target}, {"afterEnemyReply", hp[i]},
                {"afterFriendlyAction", futureHp[i]}, {"stillSealed", closed}, {"firedInResponse", boxedUnitFired},
                {"friendlyEnd", QJsonArray{positions[i].x, positions[i].y}}};
            if (positions[i] == box.center && futureHp[i] > 0 && horizon > 1 && result.terminalTick != 1)
            {
                for (const auto &actor : plan.actors)
                    if (actor.engineUnitId == box.target)
                        confinedFacts = problem.actors()[actor.actorSlot].candidates[actor.candidateIndex].source.replyFacts.get();
                confined = i;
            }
        }
        const bool guardReserved = result.guardCovered;
        std::vector<int> casualtyDay(hp.size(), -1);
        const bool stages=branch==ApproachBranch::Shoot || branch==ApproachBranch::Bypass;
        int approachFuel=-1, priorDeclineFuel=-1;
        auto describeApproach=[&](const CaptureApproach &c,QString status) {
            auto xy=[](TilePoint p){return QJsonArray{p.x,p.y};};
            return QJsonObject{{"status",status},{"unit",terrain.units[c.actor].engineUnitId},
                {"shooter",terrain.units[c.shooter].engineUnitId},{"origin",xy(c.origin)},
                {"stage",xy(c.stage)},{"target",xy(c.target)},{"gun",xy(c.gun)},
                {"firstLegCost",c.firstCost},{"originalRate",c.rate},{"originalFinish",c.finish}};
        };
        auto fuelAfterF0=[&](int i) {
            return f0Movement[i]>=0 ? remainingOwnFuel(i,f0Movement[i]) : -1;
        };
        auto takeApproachOrder=[&](const CaptureApproach &order,bool move,int &fuel) {
            const int i=order.actor;
            if (positions[i]!=order.origin || futureHp[i]<=0 || withdrew[i] || holdFacts[i] ||
                lastShot[i]>=1 || (guardReserved && i==reservedGuard) ||
                (move && std::find(positions.begin(),positions.end(),order.stage)!=positions.end()))
                return false;
            if (!RelativeStockDetail::checkedAdd(result.replacedCapperNextShot,nextShot[i])) return false;
            nextShot[i]=0;
            lastShot[i]=1;
            fuel=fuelAfterF0(i);
            if (move)
            {
                if (fuel>=0 && fuel<order.firstCost) return false;
                if (fuel>=0) fuel-=order.firstCost;
                positions[i]=order.stage;
                carry[i]=progress[i]=0;
            }
            return true;
        };
        if (branch!=ApproachBranch::Observe)
        {
            if (!approach) return {};
            if (priorDecline && (!stages || priorDecline->actor==approach->actor ||
                priorDecline->shooter!=approach->shooter || priorDecline->stage!=approach->stage ||
                !takeApproachOrder(*priorDecline,false,priorDeclineFuel))) return {};
            if (!takeApproachOrder(*approach,stages,approachFuel)) return {};
            if (branch==ApproachBranch::Shoot) prepareWeaponLoads();
            result.approachRecord=describeApproach(*approach,branch==ApproachBranch::Shoot ? "SHOOT" :
                stages ? "BYPASS" : "DECLINE");
            result.approachRecord["fuelAfterF1"]=approachFuel;
            result.approachRecord["f1End"]=QJsonArray{positions[approach->actor].x,positions[approach->actor].y};
            if (priorDecline)
            {
                result.approachRecord["priorDeclineUnit"]=terrain.units[priorDecline->actor].engineUnitId;
                result.approachRecord["priorDeclineEnd"]=QJsonArray{priorDecline->origin.x,priorDecline->origin.y};
                result.approachRecord["priorDeclineFuelAfterF1"]=priorDeclineFuel;
            }
        }
        for (int day = 1; day < horizon; ++day)
        {
            bool exposed = day == 1 && confinedFacts != nullptr;
            std::set<int> used;
            if (day == 1 && guardReserved)
            {
                const int i = reservedGuard;
                auto volley = headquartersGuardVolley(terrain, knowledge, positions, futureHp, real, ammo, i, homeHQ);
                if (!volley)
                    return {};
                result.guardAction["afterFriendlyAction"] = futureHp[i];
                QJsonArray orders;
                // Use a lethal HQ response ahead of attacks on ordinary captures. Every
                // shot and counter then changes the shared board and each attacker's clock.
                if (volley->cleared)
                {
                    for (const auto &shot : volley->shots)
                    {
                        const int a = shot.shooter;
                        const int hit = futureHp[i] - ceilHpSteps(shot.guardAfter);
                        const int back = futureHp[a] - ceilHpSteps(shot.shooterAfter);
                        used.insert(a);
                        lastShot[a] = day;
                        --ammo[a];
                        if (!supportLoad.empty())
                        {
                            const int d = manhattanDistance(positions[a],positions[i]);
                            spendAt(a,i,d);
                            if (shot.counter) spendAt(i,a,d);
                        }
                        if (shot.counter)
                            --ammo[i];
                        const double attackerBefore = real[a], guardBefore = real[i];
                        real[a] = shot.shooterAfter;
                        futureHp[a] -= back;
                        lastTargetTick[i]=2*day+1;
                        real[i] = shot.guardAfter;
                        futureHp[i] -= hit;
                        if (futureHp[a] == 0)
                            casualtyDay[a] = day;
                        losses.emplace_back(2 * day + 1, terrain.ownFire ? combatCapitalLoss(terrain.units[i].replacementCost,
                            guardBefore, real[i]) : bookDamage(terrain.units[i].replacementCost, hit));
                        if (shot.shooterAfter < attackerBefore)
                            losses.emplace_back(2 * day + 1, terrain.ownFire ? -combatCapitalLoss(terrain.units[a].replacementCost,
                                attackerBefore, real[a]) : -bookDamage(terrain.units[a].replacementCost, back));
                        orders.append(QJsonObject{{"unit", terrain.units[a].engineUnitId},
                            {"from", QJsonArray{positions[a].x, positions[a].y}}, {"guardAfter", futureHp[i]},
                            {"shooterAfter", futureHp[a]}});
                    }
                    casualtyDay[i] = day;
                    result.guardCovered = false;
                }
                result.guardAction["nextEnemyFire"] = orders;
                result.guardAction["clearableByStationaryVolley"] = volley->cleared;
                result.guardAction["covered"] = result.guardCovered;
                result.guardAction["afterFollowup"] = futureHp[i];
            }
            if(requestedFollowup && day==1 && supportActor!=NO_UNIT && futureHp[supportActor]<=0)
                result.followupDecision["responseMenu"]=QJsonObject{{"answers",QJsonArray{}},{"decline",true},
                    {"enumerationComplete",true},{"physicalE1MenuComplete",true},{"reason","SUPPORTER_ALREADY_DEAD_ON_E1"}};
            if (day == 1 && supportActor != NO_UNIT && futureHp[supportActor] > 0 && result.terminalTick != 1)
            {
                const int t = supportActor;
                std::vector<SupportCounterattack> answers;
                const auto wanted=requestedFollowup && requestedFollowup->answer ? &*requestedFollowup->answer : nullptr;
                auto reply = bestSupportCounterattack(terrain,knowledge.units(),positions,futureHp,real,supportLoad,
                    used,t,budget,requestedFollowup ? &answers : nullptr);
                if(requestedFollowup){
                    QJsonArray options;
                    for(const auto &a:answers) options.append(QJsonObject{{"shooter",terrain.units[a.shooter].engineUnitId},
                        {"end",QJsonArray{a.firingTile.x,a.firingTile.y}},{"weapon",a.weapon},{"counter",a.counterWeapon},
                        {"moveCost",a.movementCost},{"supporterAfter",a.supporterAfter},{"shooterAfter",a.shooterAfter},
                        {"gain",qint64(a.gain())}});
                    result.followupDecision["responseMenu"]=QJsonObject{{"answers",options},{"enumerationComplete",bool(reply)},
                        {"physicalE1MenuComplete",false},{"decline",true},
                        {"missing","POST_F1_QUEUE_AND_RELOCATED_ENEMY_FUEL_OR_OTHER_DUTIES"}};
                }
                if (!reply) {
                    const auto gap=refuseWork("ADDITIONAL_E1_FIRE_MENU",1);
                    if(routeCache) routeCache->lastFailure["followupDecision"]=result.followupDecision;
                    return gap;
                }
                if(wanted) {
                    auto found=std::find_if(answers.begin(),answers.end(),[&](const auto &x){
                        return terrain.units[x.shooter].engineUnitId==wanted->shooter && x.firingTile==wanted->end && x.weapon==wanted->weapon;
                    });
                    if(found==answers.end())return {};
                    reply=*found;requestedAnswerPlayed=true;
                    result.followupDecision["requestedSourceReplyRevalidated"]=true;
                    result.followupDecision["physicalE1FundingAndServiceComplete"]=false;
                }
                if (reply->shooter == NO_UNIT)
                    result.combinedFollowup["adverseReply"] = QJsonObject{{"status","NO_PROFITABLE_LEGAL_SHOT"}};
                else
                {
                    const int a=reply->shooter;
                    result.combinedFollowup["adverseReply"] = QJsonObject{{"status",punishCombinedFollowup ? "TAKEN" : "AVAILABLE"},
                        {"unit",terrain.units[a].engineUnitId},{"origin",QJsonArray{positions[a].x,positions[a].y}},
                        {"from",QJsonArray{reply->firingTile.x,reply->firingTile.y}},{"target",terrain.units[t].engineUnitId},
                        {"weapon",reply->weapon},{"counterWeapon",reply->counterWeapon},{"supporterBefore",real[t]},
                        {"supporterAfter",reply->supporterAfter},{"shooterBefore",real[a]},{"shooterAfter",reply->shooterAfter},
                        {"damageValue",static_cast<qint64>(reply->damageValue)},{"counterValue",static_cast<qint64>(reply->counterValue)}};
                    if (punishCombinedFollowup)
                    {
                        used.insert(a);
                        lastShot[a]=day;
                        if (positions[a]!=reply->firingTile) carry[a]=progress[a]=0;
                        positions[a]=reply->firingTile;
                        spendCombatRound(supportLoad[a],reply->weapon);
                        spendCombatRound(supportLoad[t],reply->counterWeapon);
                        --ammo[a];
                        if(reply->counterWeapon>=0) --ammo[t];
                        real[a]=reply->shooterAfter;
                        lastTargetTick[t]=2*day+1;
                        real[t]=reply->supporterAfter;
                        futureHp[a]=ceilHpSteps(real[a]);
                        futureHp[t]=ceilHpSteps(real[t]);
                        if(futureHp[a]<=0) casualtyDay[a]=day;
                        if(futureHp[t]<=0) casualtyDay[t]=day;
                        losses.emplace_back(2*day+1,reply->damageValue);
                        if(reply->counterValue>0) losses.emplace_back(2*day+1,-reply->counterValue);
                    }
                }
            }
            if (day==1 && branch==ApproachBranch::Shoot)
            {
                const int i=approach->actor,a=approach->shooter;
                if (used.contains(a) || lastShot[a]>=1 || positions[a]!=approach->gun || futureHp[a]<=0)
                    return {};
                const auto shot=standingApproachShot(terrain,knowledge.units(),*approach,futureHp,real,supportLoad);
                if (shot.shooter==NO_UNIT) return {};
                result.approachRecord["hpBefore"]=real[i];
                result.approachRecord["hpAfter"]=shot.supporterAfter;
                result.approachRecord["damageValue"]=static_cast<qint64>(shot.damageValue);
                result.approachRecord["counterValue"]=static_cast<qint64>(shot.counterValue);
                used.insert(a);
                lastShot[a]=1;
                spendCombatRound(supportLoad[a],shot.weapon);
                --ammo[a];
                if (shot.counterWeapon>=0) {spendCombatRound(supportLoad[i],shot.counterWeapon); --ammo[i];}
                real[a]=shot.shooterAfter; futureHp[a]=ceilHpSteps(real[a]);
                lastTargetTick[i]=2*day+1;
                real[i]=shot.supporterAfter; futureHp[i]=ceilHpSteps(real[i]);
                if (futureHp[a]<=0) casualtyDay[a]=1;
                if (futureHp[i]<=0) casualtyDay[i]=1;
                losses.emplace_back(3,shot.damageValue-shot.counterValue);
                result.approachRecord["afterRate"]=replyCaptureRate(knowledge.units()[i].captureRate,
                    knowledge.units()[i].hpRounded-futureHp[i],futureHp[i]<=0);
            }
            for (int i = 0; i < static_cast<int>(hp.size()); ++i)
            {
                if (!holdFacts[i] || finishDay[i] >= 0 || futureHp[i] <= 0)
                    continue;
                const auto &u = knowledge.units()[i];
                progress[i] += replyCaptureRate(u.captureRate, u.hpRounded - futureHp[i], false);
                if (progress[i] >= field.capturePointsToCapture())
                    finishDay[i] = day;
                else
                    exposed = true;
            }
            if (!exposed)
                break;
            for (;;)
            {
                int attacker = NO_UNIT, victim = NO_UNIT, hit = 0;
                double bestDamage = 0, counterDamage = 0;
                for (int i = 0; i < static_cast<int>(hp.size()); ++i)
                {
                    const bool boxTarget = day == 1 && i == confined && confinedFacts;
                    if ((!holdFacts[i] && !boxTarget) || futureHp[i] <= 0 || finishDay[i] >= 0)
                        continue;
                    const auto &facts = boxTarget ? *confinedFacts : *holdFacts[i];
                    if (budget < static_cast<std::int64_t>(facts.tile->shooters.size()) + 1)
                        return {};
                    budget -= facts.tile->shooters.size() + 1;
                    for (std::size_t j = 0; j < facts.tile->shooters.size(); ++j)
                    {
                        const auto &sh = facts.tile->shooters[j];
                        const int a = sh.shooterIndex;
                        if (used.contains(a) || futureHp[a] <= 0 || ammo[a] <= 0)
                            continue;
                        const auto from = positions[a], to = positions[i];
                        if (from == INVALID_TILE || to == INVALID_TILE)
                            continue;
                        int distance = std::abs(from.x - to.x) + std::abs(from.y - to.y);
                        const auto &u = knowledge.units()[a];
                        if (distance < u.minRange || distance > u.maxRange)
                            continue;
                        const auto &base = facts.shots[j];
                        // Use the live weapon only. No speculative repair or resupply.
                        if (base.rawBase <= 0)
                            continue;
                        const double damage = engineShotDamage(
                            terrainReducedDamage(facts.terrain, base.rawBase, futureHp[a], futureHp[i]));
                        const double effective = std::min(10 * real[i], damage);
                        if (effective <= bestDamage)
                            continue;
                        bestDamage = effective;
                        attacker = a;
                        victim = i;
                        const int remaining = ceilHpSteps(engineHpAfterReply(real[i], damage));
                        hit = futureHp[i] - remaining;
                        counterDamage = 0;
                        if (distance == 1 && sh.direct && ammo[i] > 0 && base.counterRawBase >= 0)
                            counterDamage = engineShotDamage(boxTarget && terrain.defense ?
                                terrainReducedDamage(terrain.defense(a, from), base.counterRawBase, remaining, futureHp[a]) :
                                hpScaledBase(base.counterRawBase, remaining));
                    }
                }
                if (attacker == NO_UNIT)
                    break;
                used.insert(attacker);
                lastShot[attacker] = day;
                --ammo[attacker];
                if (counterDamage > 0)
                    --ammo[victim];
                const double attackerBefore = real[attacker], victimBefore = real[victim];
                real[attacker] = engineHpAfterReply(real[attacker], counterDamage);
                int counter = futureHp[attacker] - ceilHpSteps(real[attacker]);
                futureHp[attacker] -= counter;
                lastTargetTick[victim]=2*day+1;
                real[victim] = engineHpAfterReply(real[victim], bestDamage);
                futureHp[victim] = ceilHpSteps(real[victim]);
                if (futureHp[victim] <= 0)
                    casualtyDay[victim] = day;
                if (futureHp[attacker] <= 0)
                    casualtyDay[attacker] = day;
                if (day == 1 && victim == confined)
                    boxShots.append(QJsonObject{{"unit", terrain.units[attacker].engineUnitId},
                        {"from", QJsonArray{positions[attacker].x, positions[attacker].y}},
                        {"after", futureHp[victim]}, {"damageSteps", hit}, {"counterSteps", counter}});
                losses.emplace_back(2 * day + 1, terrain.ownFire ? combatCapitalLoss(terrain.units[victim].replacementCost,
                    victimBefore, real[victim]) : bookDamage(terrain.units[victim].replacementCost, hit));
                if (real[attacker] < attackerBefore)
                    losses.emplace_back(2 * day + 1, terrain.ownFire ? -combatCapitalLoss(terrain.units[attacker].replacementCost,
                        attackerBefore, real[attacker]) : -bookDamage(terrain.units[attacker].replacementCost, counter));
            }
        }
        if (!result.boxContinuation.isEmpty())
        {
            result.boxContinuation["nextEnemyFire"] = boxShots;
            result.boxContinuation["afterFollowup"] = futureHp[box.targetKnowledge];
        }
        if (guardReserved && casualtyDay[reservedGuard] == 1)
        {
            result.guardCovered = false;
            result.guardAction["covered"] = false;
            result.guardAction["afterFollowup"] = 0;
        }
        for (int i = 0; i < static_cast<int>(hp.size()); ++i)
            if (holdFacts[i])
            {
                result.holds.append(
                    QJsonObject{{"unit", terrain.units[i].engineUnitId},
                                {"startsNextTurn", static_cast<bool>(unstarted[i])},
                                {"pointsAfterOwn", carry[i]},
                                {"hpAfterReply", hp[i]},
                                {"hpAfterHold", futureHp[i]},
                                {"finishDay", finishDay[i]},
                                {"tile", QJsonArray{holdFacts[i]->tile->target.x, holdFacts[i]->tile->target.y}}});
                if (finishDay[i] < 0 && hp[i] > 0 && nextShot[i] > 0)
                    losses.emplace_back(2, nextShot[i]);
            }
        struct Row
        {
            int unit, nextDay, rate, carry;
            TilePoint origin;
            std::vector<int> travel, departures;
            int fuel{-1};
            std::vector<int> tripFuel;
            // Capture options whose immediate approach conflicted at the named
            // F1 stop. This particular delayed bid spends F1 waiting at origin.
            std::map<int,bool> waitForStage;
        };
        std::vector<Row> rows;
        std::set<int> openingDays[2];
        std::vector<int> occupant(terrain.width * terrain.height, NO_UNIT);
        std::vector<int> vacatedDay(occupant.size(), horizon);
        auto cell = [&](TilePoint p) { return p.y * terrain.width + p.x; };
        for (int i = 0; i < static_cast<int>(terrain.units.size()); ++i)
        {
            const auto &pawn = terrain.units[i];
            const auto origin = positions[i];
            if (origin == INVALID_TILE)
                continue;
            occupant[cell(origin)] = i;
            if (casualtyDay[i] >= 0)
            {
                // An unspent enemy may move after a kill on E(d). A friendly
                // route can use that square only on its following F(d+1).
                openingDays[!pawn.enemy].insert(casualtyDay[i] + int(pawn.enemy));
                continue;
            }
            if ((i == reservedGuard && guardReserved) || !knowledge.units()[i].canCapture || pawn.grid < 0 || hp[i] <= 0)
                continue;
            const int readyHp = pawn.enemy || i == supportActor || homeCapperShots.contains(i) ||
                                (branch==ApproachBranch::Shoot && i==approach->actor) ? futureHp[i] : hp[i];
            const int rate = replyCaptureRate(knowledge.units()[i].captureRate,
                                              knowledge.units()[i].hpRounded - readyHp, readyHp <= 0);
            if (rate > 0 && (!holdFacts[i] || finishDay[i] >= 0))
            {
                int fuel=branch!=ApproachBranch::Observe && i==approach->actor ? approachFuel :
                    priorDecline && i==priorDecline->actor ? priorDeclineFuel : -1;
                if (fuel>=static_cast<std::int64_t>(pawn.movement)*horizon) fuel=-1;
                rows.push_back({i, std::max(lastShot[i] + 1, withdrew[i] ? 2 : 1), rate, carry[i], origin, {}, {},fuel,{},{}});
            }
        }
        std::map<std::tuple<int, bool, int>, MobilityCostGrid> grids;
        ActivationField reach;
        std::vector<bool> reserved(columns.size());
        std::vector<int> rowIndex(positions.size(), -1);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            rowIndex[rows[i].unit] = i;
        // A stationary deterrent can guard from a factory. Its defended hold and a
        // hypothetical departure to enable a purchase cannot use the same square and day.
        std::map<std::pair<int,int>,int> guardOccupancy;
        std::set<int> friendlyFirstShot;
        for (const auto &shot : result.returnFire)
            friendlyFirstShot.insert(shot.toObject().value("unit").toInt());
        const int fixedTerminalTick=result.terminalTick,fixedTerminalSide=result.terminalSide;
        std::vector<int> routeFuelLeft;
        auto prepareRouteFuel=[&] {
            if(!routeFuelLeft.empty()) return;
            for(int i=0;i<static_cast<int>(positions.size());++i)
            {
                const auto &pawn=terrain.units[i];
                int available=servicedFuel[i]>=0?servicedFuel[i]:terrain.orderedOpening?terrain.orderedOpening->afterService[i].fuel:knowledge.units()[i].fuel;
                if(available>=0)
                {
                    const int ownFuel=fuelAfterF0(i);
                    if(!pawn.enemy && ownFuel>=0) available=std::min(available,ownFuel);
                    const auto *charged=terrain.orderedOpening?&plan.servicedStockShots:nullptr;
                    const auto moved=charged?std::find_if(charged->begin(),charged->end(),[&](const auto &a){return a.shot.shooter==i;}):std::vector<OpeningLaneAction>::const_iterator{};
                    const auto block=std::find_if(plan.encirclement.blockOrders.begin(),plan.encirclement.blockOrders.end(),
                        [&](const auto &b){return b.unitIndex==i && b.movementCost>=0;});
                    if(plan.openingRecruitFight && i==plan.openingRecruitFight->shooter)
                        available=std::max(0,available-plan.openingRecruitFight->movementCost);
                    else if(charged && moved!=charged->end())available=std::max(0,available-moved->moveCost);
                    else if(block!=plan.encirclement.blockOrders.end())available=std::max(0,available-block->movementCost);
                    else if(pawn.enemy && productionPositionsAfterE0[i]!=pawn.tile) available=std::max(0,available-pawn.movement);
                    if(positions[i]!=productionPositionsAfterE0[i]) available=std::max(0,available-pawn.movement);
                }
                routeFuelLeft.push_back(available);
            }
        };
        // A previous opponent reply cannot donate its unused search work to the next
        // squad. Both start with exactly the fields paid for during board setup.
        StockRouteLowerBounds lower(routeCache ? &routeCache->preparedPropertyBounds : nullptr);
        for (;;)
        {
            int bestRow = -1, bestColumn = -1, bestFinish = 0;
            MilliFunds bestSwing = 0, bestShotCost = 0;
            int bestDeparture = 0;
            std::tuple<int, MilliFunds, int, int, int> bestKey;
            for (int r = 0; r < static_cast<int>(rows.size()); ++r)
            {
                auto &row = rows[r];
                if (row.nextDay >= horizon)
                    continue;
                const auto &pawn = terrain.units[row.unit];
                if (row.travel.empty())
                {
                    row.travel.assign(targets.size(), UNREACHABLE);
                    row.departures.assign(targets.size(), row.nextDay);
                    if (row.fuel>=0) row.tripFuel.assign(targets.size(),0);
                    const auto &openings = openingDays[pawn.enemy];
                    auto nextOpening = openings.upper_bound(row.nextDay);
                    int boardDay = nextOpening == openings.begin() ? 0 : *std::prev(nextOpening);
                    int departure = row.nextDay;
                    for (;;)
                    {
                        const auto queryWork = static_cast<std::int64_t>(positions.size() + targets.size() + 1);
                        if (budget < queryWork)
                            return {};
                        budget -= queryWork;
                        std::vector<TilePoint> blockers;
                        std::vector<int> blockedCells;
                        if (!pawn.ignoresCollision)
                            for (int k = 0; k < static_cast<int>(positions.size()); ++k)
                                if (terrain.units[k].enemy != pawn.enemy && positions[k] != INVALID_TILE &&
                                    (casualtyDay[k] < 0 || casualtyDay[k] + int(!pawn.enemy) > boardDay))
                                {
                                    blockers.push_back(positions[k]);
                                    blockedCells.push_back(cell(positions[k]));
                                }
                        // Knowledge order is stable within one comparison. Keeping that order
                        // can miss an equivalent position, but cannot conflate different boards.
                        CaptureRaceRouteCache::Key routeKey{pawn.grid, cell(row.origin), pawn.movement,
                            horizon - departure, int(pawn.ignoresCollision), std::move(blockedCells)};
                        std::vector<int> calculated;
                        std::vector<int> routeFuel;
                        const auto distances = captureTargetDistances(row.fuel>=0 ? nullptr : routeCache,std::move(routeKey),terrain,budget,calculated,
                            [&](auto &result) {
                            const MobilityCostGrid *grid = &terrain.grids[pawn.grid];
                            if (!pawn.ignoresCollision)
                            {
                                const auto key = std::tuple{pawn.grid, pawn.enemy, boardDay};
                                auto it = grids.find(key);
                                if (it == grids.end())
                                    it = grids.emplace(key, grid->withBlockedTiles(blockers)).first;
                                grid = &it->second;
                            }
                            reach.build(*grid, row.origin, pawn.movement, horizon - departure, targets);
                            for (const auto &target : targets)
                            {
                                int days=reach.activations(target.x,target.y);
                                if (row.fuel>=0)
                                {
                                    const int use=reach.routeCost(target.x,target.y);
                                    if (use<0 || use>row.fuel) days=UNREACHABLE;
                                    routeFuel.push_back(use);
                                }
                                result.push_back(days);
                            }
                        });
                        if (!distances) return {};
                        for (std::size_t j = 0; j < targets.size(); ++j)
                        {
                            const int travel = distances->at(j);
                            const int elapsed = departure - row.nextDay + travel;
                            if (travel > 0 && (row.travel[j] < 0 || elapsed < row.travel[j]))
                            {
                                row.travel[j] = elapsed;
                                row.departures[j] = departure;
                                if (row.fuel>=0) row.tripFuel[j]=routeFuel[j];
                            }
                        }
                        if (pawn.ignoresCollision || nextOpening == openings.end() || *nextOpening >= horizon)
                            break;
                        boardDay = departure = *nextOpening++;
                    }
                }
                if (columns.size() > static_cast<std::uint64_t>(budget))
                    return {};
                budget -= columns.size();
                for (int j = 0; j < static_cast<int>(columns.size()); ++j)
                {
                    const auto &column = columns[j];
                    const int standing = occupant[cell(column.tile)];
                    if (reserved[j] || row.travel[j] < 1 ||
                        column.ownerBefore == (pawn.enemy ? OwnerSign::Enemy : OwnerSign::Ours))
                        continue;
                    const bool holding = holdFacts[row.unit] && row.nextDay == 1 && !pawn.enemy;
                    if (holding && column.tile != row.origin)
                        continue;
                    const int earliest = row.nextDay + row.travel[j] - 1;
                    int entry = earliest;
                    if (standing != NO_UNIT && standing != row.unit)
                    {
                        if (casualtyDay[standing] >= 0)
                            entry = std::max(entry, casualtyDay[standing] + int(!pawn.enemy));
                        else if (terrain.units[standing].enemy != pawn.enemy)
                            continue;
                        else
                            entry = std::max(entry, vacatedDay[cell(column.tile)]);
                    }
                    auto finish = entry +
                                  widenedCaptureTurns(column.tile == row.origin ? row.carry : 0, row.rate,
                                                      field.capturePointsToCapture()) -
                                  1;
                    if (holding)
                        finish = finishDay[row.unit];
                    if (finish < entry || finish >= horizon)
                        continue;
                    auto swing = captureRaceSwing(column, pawn.enemy, static_cast<int>(finish), horizon);
                    int departure = row.departures[j];
                    const bool waitsForStage=row.waitForStage.contains(j);
                    MilliFunds shotCost = !pawn.enemy && (departure == 1 || waitsForStage) ? nextShot[row.unit] : 0;
                    if (shotCost > 0 && !holding && !waitsForStage)
                    {
                        const int delay = entry == earliest ? 1 : 0;
                        const auto later =
                            finish + delay < horizon
                                ? captureRaceSwing(column, false, static_cast<int>(finish + delay), horizon)
                                : 0;
                        if (shootBeforeCapture(swing, later, shotCost))
                        {
                            ++departure;
                            entry += delay;
                            finish += delay;
                            swing = later;
                            shotCost = 0;
                        }
                    }
                    const auto benefit = pawn.enemy ? -swing : swing - shotCost;
                    if (benefit <= 0)
                        continue;
                    const auto key = std::make_tuple(2 * entry + static_cast<int>(pawn.enemy), -benefit,
                                                     static_cast<int>(finish), row.unit, column.slot);
                    if (bestRow >= 0 && key >= bestKey)
                        continue;
                    bestKey = key;
                    bestRow = r;
                    bestColumn = j;
                    bestFinish = static_cast<int>(finish);
                    bestSwing = swing;
                    bestShotCost = shotCost;
                    bestDeparture = departure;
                }
            }
            if (bestRow < 0)
                break;
            auto &row = rows[bestRow];
            auto tile = columns[bestColumn].tile;
            const bool assaultByEnemy = terrain.units[row.unit].enemy;
            if (approach && !assaultByEnemy && row.unit!=approach->actor && row.nextDay==1 &&
                bestDeparture==1 && std::get<0>(bestKey)==4 && row.travel[bestColumn]==2 && row.carry==0)
            {
                // After a decline the same funded timetable can be reassigned
                // to another capper. A renamed unit still needs the same F1
                // tile. In a paid staging branch that tile is already occupied.
                const auto &pawn=terrain.units[row.unit];
                const int fuel=fuelAfterF0(row.unit);
                std::vector<int> blockers;
                if (!pawn.ignoresCollision)
                    for (int i=0;i<static_cast<int>(positions.size());++i)
                        if (terrain.units[i].enemy && positions[i]!=INVALID_TILE &&
                            (futureHp[i]>0 || casualtyDay[i]>=1))
                            blockers.push_back(cell(positions[i]));
                const auto step=compulsoryApproachStep(terrain,pawn.grid,row.origin,tile,pawn.movement,
                    fuel<0 ? std::numeric_limits<int>::max() : fuel,std::move(blockers),
                    routeCache ? &routeCache->approachSteps : nullptr,budget);
                if (!step) return {};
                if (step->tile==approach->stage)
                {
                    prepareWeaponLoads();
                    const int i=row.unit, gun=approach->shooter;
                    CaptureApproach alternate{i,gun,row.origin,step->tile,tile,approach->gun,
                        step->first,row.rate,bestFinish};
                    const bool gunFree=positions[gun]==approach->gun && lastShot[gun]<1 &&
                        futureHp[gun]>0 && carry[gun]==0;
                    const auto shot=gunFree ? standingApproachShot(terrain,knowledge.units(),alternate,
                        futureHp,real,supportLoad) : SupportCounterattack{};
                    const auto clock=approachCaptureClock(knowledge.units()[i],
                        shot.shooter==NO_UNIT ? real[i] : shot.supporterAfter,2,field.capturePointsToCapture(),horizon);
                    const bool delayed=shot.shooter!=NO_UNIT && clock.finish>bestFinish;
                    if (stages || delayed)
                    {
                        if (!stages && !result.replacementApproach) result.replacementApproach=alternate;
                        const bool safeWait=!stationaryThreatAt(terrain,knowledge.units(),i,row.origin,
                            positions,futureHp,supportLoad,lastShot);
                        auto edits=result.approachRecord.value("avoidedReplacementOptions").toArray();
                        auto receipt=describeApproach(alternate,stages ? "F1_STOP_OCCUPIED" : "SAME_F1_GUN");
                        receipt["safeWaitOrigin"]=safeWait;
                        receipt["rateAfterPossibleShot"]=clock.rate;
                        edits.append(receipt);
                        result.approachRecord["avoidedReplacementOptions"]=edits;
                        row.waitForStage[bestColumn]=safeWait;
                        ++row.travel[bestColumn];
                        ++row.departures[bestColumn];
                        continue; // reconsider all rows and paydays with this changed bid
                    }
                }
            }
            if (tile == (assaultByEnemy ? homeHQ : enemyHQ))
            {
                // A later itinerary is an unopposed capture prediction. A defender with a prior
                // legal activation can deny entry. Neither hypothetical order earns a transfer or
                // consumes an actor in the alternative in which the attack is not attempted.
                const int entryTick = std::get<0>(bestKey);
                bool challenged = false;
                std::vector<TilePoint> hostilePositions;
                for (int i = 0; i < static_cast<int>(positions.size()); ++i)
                    if (terrain.units[i].enemy == assaultByEnemy && futureHp[i] > 0 && positions[i] != INVALID_TILE)
                        hostilePositions.push_back(positions[i]);
                std::map<int,MobilityCostGrid> blockedGrids;
                for (int i = 0; i < static_cast<int>(positions.size()); ++i)
                {
                    const auto &defender = terrain.units[i];
                    if (defender.enemy == assaultByEnemy || futureHp[i] <= 0 || positions[i] == INVALID_TILE ||
                        defender.grid < 0 || (guardReserved && i == reservedGuard) || (holdFacts[i] && finishDay[i] < 0))
                        continue;
                    // An opposing reserve that neither fired, captured, waited nor sealed an exit
                    // can already move on E0, before our next F1 approach. Its first legal order
                    // is a real defense even when a later greedy itinerary spends it on another town.
                    const bool freeEnemyNow = defender.enemy && !fired[i] && !capturedNow[i] && !blockedExit[i] &&
                        i != plan.enemyWaiting && lastShot[i] < 1 && plan.combatLocations[i] == positions[i];
                    int ready = std::max(freeEnemyNow ? 0 : 1, lastShot[i] + 1);
                    TilePoint from = positions[i];
                    if (!defender.enemy && (withdrew[i] || friendlyFirstShot.contains(defender.engineUnitId)))
                        ready = std::max(2,ready);
                    if (holdFacts[i])
                        ready = std::max(ready, finishDay[i] + 1);
                    // A future trip to a town can be declined when an HQ invasion would
                    // otherwise win. Keep all already paid shots, holds and withdrawals. If
                    // the invasion is declined, the ordinary town itinerary remains available.
                    const bool canRedirect = !holdFacts[i] && !withdrew[i] && lastShot[i] < 1 &&
                        !friendlyFirstShot.contains(defender.engineUnitId) && positions[i] == plan.combatLocations[i];
                    if (!freeEnemyNow && !canRedirect && rowIndex[i] >= 0)
                    {
                        const auto &other = rows[rowIndex[i]];
                        ready = std::max(ready, other.nextDay);
                        from = other.origin;
                    }
                    const int deadline = (entryTick - int(defender.enemy) - 1) / 2;
                    if (ready > deadline || 2 * ready + int(defender.enemy) >= entryTick)
                        continue;
                    const auto work = 2LL * terrain.width * terrain.height + static_cast<std::int64_t>(positions.size());
                    if (work > budget)
                        return {};
                    budget -= work;
                    const MobilityCostGrid *grid = &terrain.grids[defender.grid];
                    if (!defender.ignoresCollision)
                    {
                        auto at = blockedGrids.find(defender.grid);
                        if (at == blockedGrids.end())
                            at = blockedGrids.emplace(defender.grid, grid->withBlockedTiles(hostilePositions)).first;
                        grid = &at->second;
                    }
                    reach.build(*grid, from, defender.movement, deadline - ready + 1, {tile});
                    const int turns = reach.activations(tile.x,tile.y);
                    if (turns <= 0 || ready + turns - 1 > deadline)
                        continue;
                    if (terrain.weapon && terrain.defense)
                    {
                        // The proposed capturer must enter and capture on this activation. Only
                        // other uncommitted guns still at their firing positions can clear a defender.
                        auto availableAmmo = ammo;
                        availableAmmo[row.unit] = 0;
                        for (int gun = 0; gun < static_cast<int>(positions.size()); ++gun)
                        {
                            const int gunRow = rowIndex[gun];
                            if (lastShot[gun] >= entryTick / 2 || (gunRow >= 0 &&
                                (rows[gunRow].origin != positions[gun] || rows[gunRow].nextDay > entryTick / 2)))
                                availableAmmo[gun] = 0;
                        }
                        const auto volley = headquartersGuardVolley(terrain, knowledge, positions, futureHp, real,
                                                                     availableAmmo, i, tile);
                        if (!volley) return {};
                        if (volley->cleared) continue;
                    }
                    challenged = true;
                    result.contestedHeadquarters.append(QJsonObject{{"attacker",terrain.units[row.unit].engineUnitId},
                        {"defender",defender.engineUnitId}, {"hq", QJsonArray{tile.x,tile.y}},
                        {"defenderFrom",QJsonArray{from.x,from.y}}, {"firstFreeDay",ready}, {"firstEnemyActionAvailable",freeEnemyNow},
                        {"canRedirectFromFutureTown",canRedirect},
                        {"defenderArrivalTick",2*(ready+turns-1)+int(defender.enemy)}, {"attackerEntryTick",entryTick},
                        {"predictedCompletionTick",2*bestFinish+int(assaultByEnemy)}});
                    break;
                }
                if (challenged)
                {
                    reserved[bestColumn] = true;
                    continue;
                }
            }
            const int entryTick = std::get<0>(bestKey);
            const int returnTick = entryTick + 1;
            const bool opponentOwned = columns[bestColumn].ownerBefore ==
                (assaultByEnemy ? OwnerSign::Ours : OwnerSign::Enemy);
            if (opponentOwned && tile != homeHQ && tile != enemyHQ && returnTick < 2 * bestFinish + int(assaultByEnemy) &&
                !holdFacts[row.unit] && terrain.weapon && terrain.defense)
            {
                // First-entry income assumes the capper lives to collect the property. Check a
                // defender which can shoot lethally from a retained square on the intervening
                // activation. A declined trip earns neither the transfer nor the speculative kill.
                bool denied = false;
                for (int gun = 0; gun < static_cast<int>(positions.size()); ++gun)
                {
                    const auto &defender = terrain.units[gun];
                    const auto &unit = knowledge.units()[gun];
                    const int turn = returnTick / 2;
                    if (defender.enemy == assaultByEnemy || defender.capper || !unit.canFire ||
                        futureHp[gun] <= 0 || ammo[gun] <= 0 || positions[gun] == INVALID_TILE ||
                        positions[gun] != plan.combatLocations[gun] || lastShot[gun] >= turn ||
                        (guardReserved && gun == reservedGuard) ||
                        (!defender.enemy && turn==1 && (withdrew[gun] || nextShot[gun]>0 ||
                            friendlyFirstShot.contains(defender.engineUnitId))))
                        continue;
                    const int distance = manhattanDistance(positions[gun],tile);
                    if (distance < unit.minRange || distance > unit.maxRange)
                        continue;
                    if (!terrain.pay(1)) return {};
                    const double power = terrain.weapon(gun,row.unit,distance);
                    const double damage = power > 0 ? engineShotDamage(terrainReducedDamage(terrain.defense(row.unit,tile),
                        power, futureHp[gun], futureHp[row.unit])) : 0;
                    if (damage <= 0 || engineHpAfterReply(real[row.unit],damage) > 0)
                        continue;
                    // Only actual prior damage in the joint projection can remove this gun.
                    // An escort which merely could shoot it has not spent that activation.
                    // This capturer declines the unsupported trip. No defender fires, so
                    // a later raid cannot use this abandoned proposal as a diversion. A
                    // shot already spent in the real continuation is excluded by lastShot.

                    const auto standing = std::pair{positions[gun].x,positions[gun].y};
                    auto [guard, firstHold] = guardOccupancy.try_emplace(standing,returnTick);
                    if (!firstHold) guard->second = std::max(guard->second,returnTick);
                    result.contestedPropertyCaptures.append(QJsonObject{{"unit",terrain.units[row.unit].engineUnitId},
                        {"tile",QJsonArray{tile.x,tile.y}}, {"entryTick",entryTick},
                        {"scheduledCompletionTick",2*bestFinish+int(assaultByEnemy)},
                        {"defender",defender.engineUnitId}, {"from",QJsonArray{positions[gun].x,positions[gun].y}},
                        {"shotTick",returnTick}, {"capperHp",real[row.unit]}, {"damage",damage},
                        {"carry",tile==row.origin ? row.carry : 0}});
                    row.travel[bestColumn] = UNREACHABLE;
                    denied = true;
                    break;
                }
                if (denied) continue;
            }
            if (!RelativeStockDetail::checkedAdd(result.stock, bestSwing))
                return {};
            if (!RelativeStockDetail::checkedAdd(result.foregoneShot, bestShotCost))
                return {};
            const bool enemy = terrain.units[row.unit].enemy;
            transfers.push_back({columns[bestColumn], enemy, bestFinish, bestShotCost, bestDeparture,
                                 row.unit, std::get<0>(bestKey) / 2, row.rate, tile==row.origin ? row.carry : 0,row.origin});
            if (auto waited=row.waitForStage.find(bestColumn);waited!=row.waitForStage.end())
            {
                if (!waited->second)
                {
                    if (branch!=ApproachBranch::Decline) return {};
                    result.approachRecord["avoidCertified"]=false;
                }
                auto edits=result.approachRecord.value("avoidedReplacementOptions").toArray();
                for (int j=0;j<edits.size();++j)
                {
                    auto e=edits[j].toObject();
                    if (e["unit"].toInt()!=terrain.units[row.unit].engineUnitId ||
                        e["target"].toArray()!=QJsonArray{tile.x,tile.y}) continue;
                    e["acceptedDeparture"]=bestDeparture;
                    e["acceptedEntry"]=std::get<0>(bestKey)/2;
                    e["acceptedFinish"]=bestFinish;
                    e["foregoneF1Shot"]=static_cast<qint64>(bestShotCost);
                    edits[j]=e;
                }
                result.approachRecord["avoidedReplacementOptions"]=edits;
            }
            if (tile == (enemy ? homeHQ : enemyHQ))
            {
                int tick = 2 * bestFinish + int(enemy);
                if (result.terminalTick < 0 || tick < result.terminalTick)
                {
                    result.terminalTick = tick;
                    result.terminalSide = enemy ? -1 : 1;
                }
            }
            result.claims.append(QJsonObject{{"unit", terrain.units[row.unit].engineUnitId},
                                             {"unitKnowledge", row.unit},
                                             {"enemy", terrain.units[row.unit].enemy},
                                             {"tile", QJsonArray{tile.x, tile.y}},
                                             {"departureDayOffset", bestDeparture},
                                             {"foregoneShot", static_cast<qint64>(bestShotCost)},
                                             {"entryDayOffset", std::get<0>(bestKey) / 2},
                                             {"ownerBefore",static_cast<int>(columns[bestColumn].ownerBefore)},
                                             {"completionDayOffset", bestFinish},
                                             {"rate", row.rate},
                                             {"carry", tile == row.origin ? row.carry : 0},
                                             {"signedIncome", static_cast<qint64>(bestSwing)}});
            if (tile != row.origin && occupant[cell(row.origin)] == row.unit)
                vacatedDay[cell(row.origin)] = std::min(vacatedDay[cell(row.origin)], bestDeparture + 1);
            reserved[bestColumn] = true;
            if (holdFacts[row.unit] && row.nextDay == 1)
                row.rate = replyCaptureRate(knowledge.units()[row.unit].captureRate,
                                            knowledge.units()[row.unit].hpRounded - futureHp[row.unit], false);
            row.origin = tile;
            if (row.fuel>=0) row.fuel-=row.tripFuel[bestColumn];
            row.nextDay = bestFinish + 1;
            row.carry = 0;
            row.travel.clear();
            row.waitForStage.clear();
        }
        if (branch==ApproachBranch::Observe && terrain.weapon && terrain.defense && horizon>=3 &&
            (result.terminalTick<0 || result.terminalTick>3))
        {
            // When another later hostile volley has already changed this board,
            // it is not the untouched E1 position. Leave that continuation alone.
            bool laterContact=false;
            for (int i=0;i<static_cast<int>(lastShot.size());++i)
                laterContact|=casualtyDay[i]>=1 || (terrain.units[i].enemy && lastShot[i]>=1);
            const Transfer *trip=nullptr;
            MilliFunds swing=0;
            if (!laterContact)
                for (const auto &t:transfers)
                {
                    if (t.enemy || t.departure!=1 || t.entry!=2 || t.carry!=0 || t.unit<0 ||
                        t.previous.tile==enemyHQ ||
                        t.previous.tile==homeHQ || positions[t.unit]!=productionPositionsAfterE0[t.unit] ||
                        positions[t.unit]==INVALID_TILE || holdFacts[t.unit] || withdrew[t.unit] ||
                        lastShot[t.unit]>=1 || t.unit==reservedGuard) continue;
                    const auto value=captureRaceSwing(t.previous,false,t.day,horizon);
                    if (value>swing) {trip=&t;swing=value;}
                }
            if (trip)
            {
                const int i=trip->unit;
                const auto &pawn=terrain.units[i];
                const auto &unit=knowledge.units()[i];
                if (f0Movement[i]>=0 && pawn.grid>=0 && pawn.movement>0)
                {
                    const int fuel=unit.fuel<0 ? std::numeric_limits<int>::max() :
                        remainingOwnFuel(i,f0Movement[i]);
                    std::vector<int> blocked;
                    if (!pawn.ignoresCollision)
                        for (int a=0;a<static_cast<int>(positions.size());++a)
                            if (terrain.units[a].enemy && positions[a]!=INVALID_TILE) blocked.push_back(cell(positions[a]));
                    const auto step=compulsoryApproachStep(terrain,pawn.grid,positions[i],trip->previous.tile,
                        pawn.movement,fuel,std::move(blocked),routeCache ? &routeCache->approachSteps : nullptr,budget);
                    if (!step) return {};
                    if (step->tile!=INVALID_TILE && occupant[cell(step->tile)]==NO_UNIT &&
                        (!terrain.production || std::none_of(terrain.production->sites.begin(),terrain.production->sites.end(),
                            [&](const auto &s) {return s.tile==step->tile;})) &&
                        std::none_of(transfers.begin(),transfers.end(),[&](const auto &t) {
                            return t.unit!=i && t.entry<=1 && t.day>=1 &&
                                   (t.previous.tile==step->tile || t.previous.tile==positions[i]);
                        }))
                    {
                        prepareWeaponLoads();
                        const bool originExposed=stationaryThreatAt(terrain,knowledge.units(),i,positions[i],
                            positions,futureHp,supportLoad);
                        std::tuple<int,MilliFunds> bestThreat{};
                        for (int a=0;!originExposed && a<static_cast<int>(positions.size());++a)
                        {
                            if (!terrain.units[a].enemy || positions[a]==INVALID_TILE ||
                                positions[a]!=productionPositionsAfterE0[a] || futureHp[a]<=0 || carry[a]>0) continue;
                            CaptureApproach proposed{i,a,positions[i],step->tile,trip->previous.tile,positions[a],
                                step->first,trip->rate,trip->day};
                            if (budget<1) return {};
                            --budget;
                            const auto shot=standingApproachShot(terrain,knowledge.units(),proposed,futureHp,real,supportLoad);
                            if (shot.shooter==NO_UNIT) continue;
                            const auto clock=approachCaptureClock(unit,shot.supporterAfter,2,field.capturePointsToCapture(),horizon);
                            const int finish=clock.finish;
                            const auto threat=std::tuple{finish-trip->day,shot.gain()};
                            if (finish>trip->day && (!result.approach || threat>bestThreat))
                            {
                                result.approach=proposed;
                                result.approachRecord=describeApproach(proposed,"OFFERED");
                                result.approachRecord["hpBefore"]=real[i];
                                result.approachRecord["hpAfter"]=shot.supporterAfter;
                                result.approachRecord["afterRate"]=clock.rate;
                                result.approachRecord["firstPossibleFinishAfterHit"]=finish;
                                bestThreat=threat;
                            }
                        }
                    }
                }
            }
        }
        const int proposalCut=fixedTerminalTick<0 ? 2*horizon : fixedTerminalTick;
        // Average the cumulative advantage at every turn boundary. A payday in round one
        // appears in all H balances; one in the last round appears only once. Keep the
        // nominal ledger for auditing. A projected terminal freezes the balance at its cut.
        std::vector<MilliFunds> cash(horizon, 0);
        ProductionProjection datedProduction;
        if (horizon > 0)
        {
            if (terrain.production)
            {
                const auto &sites = terrain.production->sites;
                std::vector<ProductionAccess> access(sites.size()*2);
                std::vector<ProductionProperty> property;
                std::vector<int> commitment(hp.size(),0);
                std::vector<int> fuelSpent(hp.size(),0);
                std::vector<TilePoint> afterF0;
                for (const auto &a:plan.actors)
                {
                    const auto &actor=problem.actors()[a.actorSlot];
                    fuelSpent[actor.knowledgeUnitIndex]=actor.candidates[a.candidateIndex].source.movementCost;
                    if (stages && actor.knowledgeUnitIndex==approach->actor)
                        fuelSpent[actor.knowledgeUnitIndex]+=approach->firstCost;
                    if (a.stepsAfterOwnActions>0)
                        afterF0.push_back(plan.replay.plan.action(a.actionIndex).destination);
                }
                for (int i=0;i<static_cast<int>(hp.size());++i)
                {
                    const auto &unit = knowledge.units()[i];
                    // These units occupied their native post before E0, even if the
                    // hostile reply subsequently clears them from a purchase site.
                    const bool observedOwn = unit.hasMoved && unit.ownerId == knowledge.observerId() &&
                        unit.carrier == KnownUnit::NO_CARRIER && terrain.units[i].present;
                    const bool enemySurvivedF0 = terrain.units[i].enemy && terrain.units[i].present &&
                        (!plan.enemyAfterOwn.contains(i) || plan.enemyAfterOwn.at(i)>0);
                    if (observedOwn || enemySurvivedF0) afterF0.push_back(terrain.units[i].tile);
                    if (fired[i] || capturedNow[i] || blockedExit[i] || plan.enemyWaiting==i || withdrew[i] ||
                        (i==reservedGuard && guardReserved) || holdFacts[i])
                        commitment[i]=1;
                    if(lastShot[i]>=1 && terrain.units[i].enemy)
                        commitment[i]=std::max(commitment[i],lastShot[i]+1);
                    // E0 may have used some fuel to move. Its exact route is not
                    // retained here; reserving a full activation is conservative.
                    if (terrain.units[i].enemy && productionPositionsAfterE0[i]!=terrain.units[i].tile)
                        fuelSpent[i]=terrain.units[i].movement;
                }
                auto projectedOccupant=occupant;
                std::set<int> activeRows;
                std::set<int> firedOnF1;
                for (const auto &shot : result.returnFire)
                    firedOnF1.insert(shot.toObject().value("unit").toInt());
                for (std::size_t i=0;i<hp.size();++i)
                    if(firedOnF1.contains(terrain.units[i].engineUnitId))
                        commitment[i]=1;
                for (std::size_t j=0;j<initial.size();++j)
                {
                    const auto &col=initial[j];
                    ProductionProperty spec{col.tile,col};
                    for (const auto &t:transfers)
                        if (t.previous.slot==col.slot)
                        {
                            const int tick=2*t.day+int(t.enemy);
                            spec.ownershipChanges.push_back({2*t.entry+int(t.enemy),tick,
                                t.enemy ? OwnerSign::Enemy : OwnerSign::Ours});
                            if(spec.existingCompletion<0 || tick<2*spec.existingCompletion+int(spec.existingEnemy))
                            {
                                spec.existingCompletion=t.day;
                                spec.existingEnemy=t.enemy;
                                spec.existingEntry=t.entry;
                                spec.capturer=t.unit;
                                spec.rate=t.rate;
                                spec.carry=t.carry;
                            }
                            if (t.day>0) activeRows.insert(t.unit);
                            if (positions[t.unit]==col.tile && finishDay[t.unit]>=0)
                                commitment[t.unit]=std::max(commitment[t.unit],finishDay[t.unit]);
                        }
                    property.push_back(spec);
                }
                bool unpricedDeparture=false;
                auto confirmDeparture = [&](ProductionAccess &access, int i, TilePoint from) {
                    const auto &pawn=terrain.units[i];
                    const auto guard=guardOccupancy.find({from.x,from.y});
                    if (from!=productionPositionsAfterE0[i] || withdrew[i] || lastShot[i]>=1 ||
                        firedOnF1.contains(pawn.engineUnitId) || (i==reservedGuard && guardReserved) ||
                        (guard!=guardOccupancy.end() && guard->second>=2+int(pawn.enemy)))
                        return false;
                    const Transfer *trip=nullptr;
                    std::vector<int> stops;
                    for (const auto &t:transfers)
                    {
                        if (t.unit==i && t.departure==1 && t.previous.tile!=from) trip=&t;
                        if (t.unit!=i && t.entry<=1 && t.day>=1)
                            stops.push_back(cell(t.previous.tile));
                    }
                    if (!trip) return false;
                    const int fuel=knowledge.units()[i].fuel;
                    const auto exit=bookedFactoryExit(terrain,i,from,trip->previous.tile,trip->entry,
                        fuel<0 ? std::numeric_limits<int>::max() : remainingOwnFuel(i,std::max(0,fuelSpent[i])),
                        positions,std::move(stops),routeCache,budget);
                    if (!exit) { unpricedDeparture=true; return false; }
                    access.exit=*exit;
                    if (access.exit==INVALID_TILE) return false;
                    access.vacater=pawn.engineUnitId;
                    return true;
                };
                for (std::size_t f=0;f<sites.size();++f)
                {
                    const auto tile=sites[f].tile;
                    auto prop=std::find_if(property.begin(),property.end(),[&](const auto &p){return p.tile==tile;});
                    if (prop==property.end()) continue;
                    int side=prop->column.ownerBefore==OwnerSign::Ours ? 0 :
                             prop->column.ownerBefore==OwnerSign::Enemy ? 1 : -1;
                    int day=0;
                    if (prop->existingCompletion>=0)
                    {
                        // A later transfer leaves the current owner a genuine build window
                        // if the newborn can depart before the hostile capture starts.
                        const int entryTick=2*prop->existingEntry+int(prop->existingEnemy);
                        const bool oldOwnerCanBuild=side>=0 && side!=int(prop->existingEnemy) && entryTick>2+side;
                        if(oldOwnerCanBuild)
                        {
                            const auto &atBuild=side ? productionPositionsAfterE0 : afterF0;
                            int first=std::find(atBuild.begin(),atBuild.end(),tile)==atBuild.end() ? 0 : 1;
                            const int occupantBeforeLoss=projectedOccupant[cell(tile)];
                            if (occupantBeforeLoss!=NO_UNIT)
                            {
                                if (casualtyDay[occupantBeforeLoss]>=0)
                                    first=std::max(first,casualtyDay[occupantBeforeLoss]+int(!side));
                                else if (int(terrain.units[occupantBeforeLoss].enemy)==side &&
                                         vacatedDay[cell(tile)]<horizon)
                                    first=std::max(first,vacatedDay[cell(tile)]-1);
                                else first=horizon;
                            }
                            if (2*(first+1)+side<entryTick && first<horizon)
                            {
                                auto &beforeLoss=access[f+sites.size()];
                                if (first!=1 || occupantBeforeLoss==NO_UNIT || casualtyDay[occupantBeforeLoss]>=0 ||
                                    confirmDeparture(beforeLoss,occupantBeforeLoss,tile))
                                {
                                    beforeLoss.side=side;
                                    beforeLoss.buildDay=first;
                                    beforeLoss.beforeEntry=entryTick;
                                }
                            }
                        }
                        side=int(prop->existingEnemy);
                        // The capturing unit still occupies the property when its action ends.
                        // A booked onward trip or actual projected move must free the factory.
                        day=prop->existingCompletion+1;
                        if(prop->existingEntry>=0 && prop->existingCompletion>0)
                        {
                            auto onward=std::find_if(transfers.begin(),transfers.end(),[&](const auto &t) {
                                return t.unit==prop->capturer && t.departure>prop->existingCompletion;
                            });
                            if(onward==transfers.end()) continue;
                            day=std::max(day,onward->departure);
                        }
                    }
                    if(side<0) continue;
                    const int occupantNow=projectedOccupant[cell(tile)];
                    if(occupantNow!=NO_UNIT)
                    {
                        if(casualtyDay[occupantNow]>=0)
                            day=std::max(day,casualtyDay[occupantNow]+int(!side));
                        else if (vacatedDay[cell(tile)]<horizon && int(terrain.units[occupantNow].enemy)==side)
                        {
                            day=std::max(day,vacatedDay[cell(tile)]-1);
                            if (day==1 && !confirmDeparture(access[f],occupantNow,tile))
                                continue;
                        }
                        else
                        {
                            const int i=occupantNow;
                            const auto &pawn=terrain.units[i];
                            // This factory was due to open now. An actual later departure already
                            // identified above can enable it, but another uncommitted wait cannot.
                            if (!side && terrain.production->matureFactoryCells.contains({tile.x,tile.y}))
                            {
                                result.unconfirmedFactoryReleases.append(QJsonObject{{"unit",pawn.engineUnitId},
                                    {"factory",QJsonArray{tile.x,tile.y}}});
                                continue;
                            }
                            if(int(pawn.enemy)!=side || pawn.grid<0 || pawn.movement<=0 || activeRows.contains(i) ||
                               i==reservedGuard || i==boxedUnit || holdFacts[i] || nextShot[i]>0 ||
                               firedOnF1.contains(pawn.engineUnitId) ||
                               lastShot[i]>=1 || withdrew[i] || futureHp[i]!=hp[i] ||
                               positions[i]!=productionPositionsAfterE0[i])
                                continue;
                            // Only an otherwise unused activation can be reassigned to clear a
                            // factory later. Its distinct empty endpoint remains reserved if bought.
                            for(const auto dir:ENTRY_DIRECTIONS)
                            {
                                auto delta=entryStep(dir);
                                TilePoint to{tile.x+delta.dx,tile.y+delta.dy};
                                int cost=terrain.grids[pawn.grid].cost(to.x,to.y,dir);
                                if(cost<0 || cost>pawn.movement || projectedOccupant[cell(to)]!=NO_UNIT ||
                                   std::any_of(transfers.begin(),transfers.end(),[&](const auto &t) {
                                        return t.previous.tile==to; }))
                                    continue;
                                access[f].exit=to;
                                access[f].vacater=pawn.engineUnitId;
                                break;
                            }
                            if(access[f].exit==INVALID_TILE) continue;
                            day=std::max(day,1);
                        }
                    }
                    const bool blockedF0=std::find(afterF0.begin(),afterF0.end(),tile)!=afterF0.end();
                    if (!side && blockedF0) day=std::max(day,1);
                    if (std::find(productionPositionsAfterE0.begin(),productionPositionsAfterE0.end(),tile)!=productionPositionsAfterE0.end())
                        day=std::max(day,1);
                    if (day<horizon)
                    {
                        access[f].side=side;
                        access[f].buildDay=day;
                        if (!side && day==1 && stages && tile==approach->origin)
                        {
                            access[f].vacater=terrain.units[approach->actor].engineUnitId;
                            access[f].exit=approach->stage;
                        }
                    }
                }
                for (std::size_t window=0; window<access.size(); ++window)
                {
                    auto &offer = access[window];
                    if (offer.side < 0) continue;
                    const auto at = sites[window % sites.size()].tile;
                    const auto occupiedUntil = guardOccupancy.find({at.x,at.y});
                    if (occupiedUntil == guardOccupancy.end() || 2*offer.buildDay+offer.side > occupiedUntil->second)
                        continue;
                    // Only an explicitly reserved exit can open this factory after the
                    // defender's last promised activation. Never reuse a vacater before then.
                    if (offer.exit == INVALID_TILE)
                        offer.side = -1;
                    else
                        offer.buildDay = (occupiedUntil->second-offer.side)/2+1;
                }
                if(unpricedDeparture)
                {
                    if(routeCache) routeCache->lastFailure=QJsonObject{{"phase","FACTORY_RELEASE"},{"reason","MOVEMENT_WORK"}};
                    return {};
                }
                std::vector<int> clearedAfterE1;
                if (branch==ApproachBranch::Shoot)
                    for (int u:{approach->actor,approach->shooter})
                        if (casualtyDay[u]==1) clearedAfterE1.push_back(u);
                const auto beforeFunding=budget;
                auto production=projectFundedProduction(terrain,knowledge,access,property,positions,futureHp,commitment,
                    projectedOccupant,field.capturePointsToCapture(),horizon,proposalCut,homeHQ,enemyHQ,clearedAfterE1,
                    afterF0,productionPositionsAfterE0,!proposalOnly,routeCache ? &routeCache->lastFailure : nullptr,
                    plan.openingRecruitFight ? &*plan.openingRecruitFight : nullptr,
                    terrain.orderedOpening ? &terrain.orderedOpening->repair : nullptr,
                    !proposalOnly && routeCache && routeCache->fundedPostCommitment ? &*routeCache->fundedPostCommitment : nullptr,
                    friendlyService ? &*friendlyService : nullptr,priceCapturedSources);
                result.fundedProjectionWork=beforeFunding-budget;
                if(!production) return {};
                if(selectedNewSource && std::any_of(production->orders.begin(),production->orders.end(),
                    [](const auto &v){return v.toObject().contains("stockFactoryParent");})) *selectedNewSource=true;
                result.alliedCaptureVacancy=production->alliedCaptureVacancy;
                result.productionValue=production->net;
                result.availableProductionSites=production->availableSites;
                result.affordableProductionOrders=production->affordableOrders;
                result.productionLimited=production->limited;
                result.productionOrders=production->orders;
                cash=std::move(production->flow);
                datedProduction=std::move(*production);
            }
            if(proposalOnly) return result;
            std::vector<StockCaptureTrip> itinerary;
            for(const auto &t:transfers)
                if(t.from!=INVALID_TILE) itinerary.push_back({t.unit,int(t.enemy),t.from,t.previous.tile,2*t.departure+int(t.enemy),
                    2*t.entry+int(t.enemy),2*t.day+int(t.enemy),t.rate,t.carry,t.previous,
                    !withdrew[t.unit] && !holdFacts[t.unit] && t.unit!=boxedUnit && t.unit!=supportActor &&
                    lastShot[t.unit]<1 && lastTargetTick[t.unit]<2 ? 2+int(t.enemy) : -1});
            StockCaptureSettlement settled;
            if(!positions.empty() && horizon>=2)
            {
                prepareWeaponLoads();prepareRouteFuel();
                // Cap this optional itinerary's work so one speculative capture
                // cannot consume its team's other reply comparisons. A bounded
                // leg earns no transfer unless it produced a positive witness.
                const auto area=static_cast<std::int64_t>(terrain.width)*terrain.height;
                const auto routeQuota=[&](int divisor,std::int64_t base,std::int64_t effort){
                    const auto limit=terrain.comparisonWork>=0?terrain.comparisonWork/divisor:budget/divisor;
                    return terrain.quoteWorkOffer(budget,divisor,base>limit/effort?limit:base*effort);
                };
                const auto baseStockCap=horizon*(4*area+static_cast<std::int64_t>(hp.size())*hp.size());
                const auto stockQuota=routeQuota(16,baseStockCap,routeCache?routeCache->stockRouteEffort:1);
                const auto paidQuota=routeQuota(32,horizon*(2*area+static_cast<std::int64_t>(hp.size())*hp.size()/2),routeCache?routeCache->paidRouteEffort:1);
                if (budget<stockQuota+paidQuota) return refuseWork("STOCK_AND_PAID_ROUTES",stockQuota+paidQuota);
                std::int64_t work=stockQuota,paidWork=paidQuota;
                StockDutyAllowance duty;
                std::set<StockDutyTrip> paired;
                if(routeCache)
                    for(const auto &i:routeCache->stockChallengesFor(routeCache->stockComparisonPolicy))
                        paired.insert({i[3],i[0],i[1],i[4]});
                if(routeCache && (!routeCache->requestedDutyTrips.empty() || !paired.empty()))
                {
                    duty.requested=routeCache->requestedDutyTrips;
                    duty.requested.insert(paired.begin(),paired.end());
                    const auto limit=terrain.comparisonWork>=0?terrain.comparisonWork/16:budget/16;
                    const auto factor=routeCache->townEffort;
                    auto extra=baseStockCap>limit/std::max<std::int64_t>(1,factor)?limit:baseStockCap*factor;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                    if(routeCache->privateDutyAllowance)extra=*routeCache->privateDutyAllowance;
#endif
                    std::set<StockDutyTrip> present;
                    for(const auto &leg:itinerary)
                    {
                        const StockDutyTrip key{terrain.units[leg.actor].engineUnitId,leg.target.x,leg.target.y,leg.entry};
                        if(duty.requested.contains(key))present.insert(key);
                    }
                    std::map<StockDutyTrip,StockDutyTrip> assigned;
                    std::set<StockDutyTrip> active=present;
                    const auto matchWork=static_cast<std::int64_t>(paired.size())*(1+static_cast<std::int64_t>(itinerary.size()));
                    if(budget<matchWork || budget-matchWork<work+paidWork)
                    {
                        return refuseWork("MATCHED_STOCK_REPLAY",matchWork+work+paidWork);
                    }
                    budget-=matchWork;
                    for(const auto &key:paired)
                    {
                        std::optional<StockDutyTrip> best;
                        for(const auto &leg:itinerary)
                        {
                            const StockDutyTrip candidate{terrain.units[leg.actor].engineUnitId,leg.target.x,leg.target.y,leg.entry};
                            if(candidate[0]!=key[0] || candidate[1]!=key[1] || candidate[2]!=key[2])continue;
                            if(!best || std::pair{std::abs(candidate[3]-key[3]),candidate[3]}<
                                std::pair{std::abs((*best)[3]-key[3]),(*best)[3]})best=candidate;
                        }
                        if(!best)continue;
                        // A retimed proposal can use the same validator, but
                        // only its final, exact dated transfer answers the key.
                        active.insert(*best);assigned.emplace(key,*best);duty.pairedCharges.try_emplace(*best,key);
                    }
                    const auto per=extra/static_cast<std::int64_t>(duty.requested.size());
                    if(extra<0)return {};
                    const auto required=work+paidWork+per*static_cast<std::int64_t>(active.size());
                    if(budget<required)return refuseWork("STOCK_DUTY_REPLAY",required);
                    duty.remaining=extra;
                    for(const auto &key:duty.requested)result.stockDutyReview.append(QJsonObject{
                        {"unit",key[0]},{"tile",QJsonArray{key[1],key[2]}},{"entryTick",key[3]},
                        {"equalOffer",static_cast<qint64>(per)},{"presentInStockItinerary",present.contains(key)},
                        {"replayCandidateEntry",assigned.contains(key)?assigned.at(key)[3]:-1},
                        {"matchedStockIntention",paired.contains(key)}});
                }
                const auto before=work+paidWork+duty.remaining;
                std::vector<TilePoint> excluded{homeHQ,enemyHQ};
                for(const auto &t:transfers) if(t.from==INVALID_TILE) excluded.push_back(t.previous.tile);
                settled=settleStockCaptureTrips(terrain,knowledge.units(),itinerary,datedProduction,positions,real,casualtyDay,
                    routeFuelLeft,supportLoad,lastShot,nextShot,guardReserved ? reservedGuard : NO_UNIT,horizon,proposalCut,
                    field.capturePointsToCapture(),lower,work,initial,excluded,paidWork,&duty);
                budget-=before-work-paidWork-duty.remaining;
                for(const auto &loss:settled.losses) losses.push_back(loss);
            }
            result.routePricingPasses=1;result.localRouteBids=settled.attempts;result.deferredRouteAssignments=settled.deferred;
            result.observedStockComplete &= !settled.dependentSearchIncomplete && !settled.interceptRouteIncomplete;
            result.routeTraversalWork=settled.routeWork;result.routeDefenseWork=settled.defenseWork;
            result.routeProofs=std::move(settled.proofs);
            for(int i=0;i<datedProduction.orders.size();++i){
                auto order=datedProduction.orders[i].toObject();
                if(!order.contains("stockFactoryParent"))continue;
                const auto *proofs=&result.routeProofs;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                QJsonArray withheld;
                if(qgetenv("COW_COMBAT_REPRICE_ORDERS")=="1" && qgetenv("COW_DIAGNOSTIC_PARENT_EXIT")=="withhold"){
                    for(const auto &value:*proofs){
                        const auto p=value.toObject();
                        if(p["from"]!=order["factory"] || p["tile"]==order["factory"])withheld.append(value);
                    }
                    proofs=&withheld;
                }
#endif
                const auto evidence=proveCapturedProductionExit(order,*proofs,
                    settled.townTraffic?&*settled.townTraffic:nullptr,terrain.width,static_cast<int>(terrain.units.size()));
                if(evidence["status"]!="WITNESSED"){
                    if(routeCache)routeCache->lastFailure={{"phase","CAPTURED_SOURCE_PREREQUISITE"},{"order",order},{"parent",evidence}};
                    return {};
                }
                order["stockFactoryExitReceipt"]=evidence;datedProduction.orders[i]=order;
            }
            result.paidCaptureWorkGap=settled.paidCaptureWorkGap;
            result.paidCaptureUnsupported=settled.paidCaptureUnsupported;
            // Rewrite the ownership events, their foregone shots and terminal cut
            // together. An abandoned leg never transfers a later chained property.
            std::vector<Transfer> proven;
            auto proposedClaims=std::move(result.claims);
            std::size_t next=0;
            result.terminalTick=fixedTerminalTick;result.terminalSide=fixedTerminalSide;
            for(auto t:transfers)
            {
                if(t.from!=INVALID_TILE)
                {
                    const auto &dated=settled.closures.at(next++);
                    if(dated.completion<0) continue;
                    auto it=std::find_if(proposedClaims.begin(),proposedClaims.end(),[&](const auto &v) {
                        const auto o=v.toObject();return o["unit"].toInt()==terrain.units[t.unit].engineUnitId &&
                            o["tile"].toArray()==QJsonArray{t.previous.tile.x,t.previous.tile.y} && o["entryDayOffset"].toInt()==t.entry;
                    });
                    if(it==proposedClaims.end()) return {};
                    auto witness=(*it).toObject();t.entry=dated.entry/2;t.day=dated.completion/2;
                    if(dated.replanned)
                    {
                        t.from=dated.from;t.departure=dated.departure/2;t.rate=dated.rate;t.carry=0;
                        t.shot=!t.enemy && t.departure==1?nextShot[t.unit]:0;
                        witness["departureDayOffset"]=t.departure;witness["rate"]=t.rate;witness["carry"]=0;
                        witness["foregoneShot"]=static_cast<qint64>(t.shot);
                    }
                    witness["entryDayOffset"]=t.entry;witness["completionDayOffset"]=t.day;result.claims.append(witness);
                    if(t.previous.tile==(t.enemy?homeHQ:enemyHQ) && (result.terminalTick<0 || dated.completion<result.terminalTick))
                    {result.terminalTick=dated.completion;result.terminalSide=t.enemy?-1:1;}
                }
                proven.push_back(t);
            }
            transfers=std::move(proven);
            std::stable_sort(transfers.begin(),transfers.end(),[](const auto &a,const auto &b){return 2*a.day+int(a.enemy)<2*b.day+int(b.enemy);});
            result.stock=0;result.foregoneShot=0;
            std::vector<ProductionProperty> finalProperties;
            for(const auto &col:initial) finalProperties.push_back({col.tile,col});
            std::vector<Transfer> distinct;
            for(auto &t:transfers)
            {
                auto &col=*std::find_if(finalProperties.begin(),finalProperties.end(),[&](const auto &p){return p.column.slot==t.previous.slot;});
                const int closure=2*t.day+int(t.enemy),entry=2*t.entry+int(t.enemy);
                t.previous.ownerBefore=productionOwnerAt(col,closure+1);
                const auto owner=t.enemy?OwnerSign::Enemy:OwnerSign::Ours;
                if(t.previous.ownerBefore==owner)
                {
                    for(int i=result.claims.size()-1;i>=0;--i)
                    {
                        const auto q=result.claims[i].toObject();
                        if(q["unit"].toInt()==terrain.units[t.unit].engineUnitId && q["tile"].toArray()==QJsonArray{t.previous.tile.x,t.previous.tile.y} && q["completionDayOffset"].toInt()==t.day)
                            result.claims.removeAt(i);
                    }
                    continue;
                }
                distinct.push_back(t);
                col.ownershipChanges.push_back({entry,closure,owner});
                const auto swing=captureRaceSwing(t.previous,t.enemy,t.day,horizon);
                if(!RelativeStockDetail::checkedAdd(result.stock,swing) || !RelativeStockDetail::checkedAdd(result.foregoneShot,t.shot)) return {};
                for(int i=0;i<result.claims.size();++i)
                {
                    auto q=result.claims[i].toObject();
                    if(q["unit"].toInt()!=terrain.units[t.unit].engineUnitId || q["tile"].toArray()!=QJsonArray{t.previous.tile.x,t.previous.tile.y} || q["completionDayOffset"].toInt()!=t.day) continue;
                    q["ownerBefore"]=static_cast<int>(t.previous.ownerBefore);q["signedIncome"]=static_cast<qint64>(swing);result.claims[i]=q;break;
                }
            }
            transfers=std::move(distinct);
            if (result.terminalTick >= 0)
            {
                // Each side collects before its own action. Suppress all paydays and promises past a modeled win.
                result.stock = 0;
                result.foregoneShot = 0;
                for (const auto &event : transfers)
                {
                    const int tick = 2 * event.day + int(event.enemy);
                    if (event.departure * 2 <= result.terminalTick &&
                        !RelativeStockDetail::checkedAdd(result.foregoneShot, event.shot))
                        return {};
                    if (tick > result.terminalTick)
                        continue;
                    MilliFunds swing = 0;
                    const auto after = event.enemy ? OwnerSign::Enemy : OwnerSign::Ours;
                    for (int day = 0; day <= horizon; ++day)
                    {
                        const int f = 2 * day, e = f + 1;
                        if (day > 0 && f > tick && f <= result.terminalTick)
                            swing += event.previous.income.oursPerTurn *
                                     (int(after == OwnerSign::Ours) - int(event.previous.ownerBefore == OwnerSign::Ours));
                        if (day < horizon && e > tick && e <= result.terminalTick)
                            swing += event.previous.income.enemyPerTurn *
                                     (int(event.previous.ownerBefore == OwnerSign::Enemy) - int(after == OwnerSign::Enemy));
                    }
                    for (int i = 0; i < result.claims.size(); ++i)
                    {
                        auto claim = result.claims[i].toObject();
                        if (claim["tile"].toArray() != QJsonArray{event.previous.tile.x, event.previous.tile.y} ||
                            claim["enemy"].toBool() != event.enemy || claim["completionDayOffset"].toInt() != event.day)
                            continue;
                        claim["uncutSignedIncome"] = claim["signedIncome"];
                        claim["signedIncome"] = static_cast<qint64>(swing);
                        result.claims[i] = claim;
                        break;
                    }
                    if (!RelativeStockDetail::checkedAdd(result.stock, swing))
                        return {};
                }
                for (int i = result.claims.size() - 1; i >= 0; --i)
                {
                    auto claim = result.claims[i].toObject();
                    if (2 * claim["completionDayOffset"].toInt() + int(claim["enemy"].toBool()) > result.terminalTick)
                        result.claims.removeAt(i);
                }
            }
            const int last = result.terminalTick < 0 ? 2 * horizon : result.terminalTick;
            // Only actual, transition-changing stock trips survived the
            // dated validator, reconciliation and distinct-owner filter above.
            // Record each once for comparison; its income is already in stock.
            const auto identityWork=static_cast<std::int64_t>(transfers.size()+result.stockDutyReview.size());
            if(budget<identityWork)
            {
                if(routeCache)routeCache->lastFailure={{"phase","MATCHED_STOCK_REPLAY"},{"reason","STOCK_TRANSFER_IDENTITY_WORK_LIMIT"}};
                return {};
            }
            budget-=identityWork;
            std::set<TownIntention> earnedStock;
            for(const auto &t:transfers)
            {
                const auto at=t.previous.tile;
                const int side=int(t.enemy),entry=2*t.entry+side,done=2*t.day+side;
                if(t.from==INVALID_TILE || at==homeHQ || at==enemyHQ || entry<0 || done<entry || done>=last ||
                   (t.previous.income.oursPerTurn<=0 && t.previous.income.enemyPerTurn<=0))continue;
                const auto owner=t.enemy?OwnerSign::Enemy:OwnerSign::Ours;
                const bool changesOwnPay=done+2-done%2<=last && t.previous.income.oursPerTurn>0 &&
                    (t.previous.ownerBefore==OwnerSign::Ours)!=(owner==OwnerSign::Ours);
                const bool changesEnemyPay=done+1+done%2<=last && t.previous.income.enemyPerTurn>0 &&
                    (t.previous.ownerBefore==OwnerSign::Enemy)!=(owner==OwnerSign::Enemy);
                if(!changesOwnPay && !changesEnemyPay)continue;
                const TownIntention i{at.x,at.y,side,terrain.units[t.unit].engineUnitId,entry};
                if(earnedStock.insert(i).second)
                {
                    auto row=describeTownIntention(i);row["completionTick"]=done;
                    row["firstAffectedPayday"]=std::min(changesOwnPay?done+2-done%2:last+1,changesEnemyPay?done+1+done%2:last+1);
                    result.settledStockIntentions.append(row);
                }
            }
            for(int j=0;j<result.stockDutyReview.size();++j)
            {
                auto row=result.stockDutyReview[j].toObject();const auto t=row["tile"].toArray();const int entry=row["entryTick"].toInt();
                row["exactRetainedStockTransfer"]=earnedStock.contains({t[0].toInt(),t[1].toInt(),entry%2,row["unit"].toInt(),entry});
                result.stockDutyReview[j]=row;
            }
            for (const auto &[tick, amount] : losses)
                if (tick <= last && !RelativeStockDetail::checkedAdd(result.holdingLoss, amount))
                    return {};
            for (const auto &c : initial)
            {
                const int pays = c.ownerBefore == OwnerSign::Enemy ? (last + 1) / 2 : last / 2;
                if (!RelativeStockDetail::checkedAdd(result.ownedIncome, c.income.valuePerTurn(c.ownerBefore) * pays))
                    return {};
            }
            // The route proofs supplied the effective capture dates. Pricing a
            // fired interception now adds only the actual material consequences.
            settled.proofs=result.routeProofs;
            auto originalOrders=datedProduction.orders;
            if(!settleFundedOrders(terrain,datedProduction,finalProperties,settled,last,horizon,routeCache ? &routeCache->lastFailure : nullptr)) return {};
            for(const auto &candidate:settled.paidCaptures)
            {
                if(candidate.finish>=last) continue;
                auto proposed=datedProduction;proposed.orders=originalOrders;
                if(!installPaidCaptureOrder(proposed,candidate,terrain,field.capturePointsToCapture())) continue;
                auto proposedOrders=proposed.orders;
                // New income is evaluated through exactly the ownership/bank
                // ledger used by the passive alternative. The opponent's dated
                // planned buys must remain affordable under the changed title.
                if(!settleFundedOrders(terrain,proposed,finalProperties,settled,last,horizon)) continue;
                const auto base=cashTimingAdjustment(datedProduction.flow),next=cashTimingAdjustment(proposed.flow);
                if(!base || !next) return {};
                if((proposed.net+*next-datedProduction.net-*base)*(proposed.fundedGuns[candidate.gun].side?-1:1)<=0) continue;
                originalOrders=std::move(proposedOrders);datedProduction=std::move(proposed);
            }
            result.paidCaptureChecks=std::move(settled.paidCaptureChecks);
            result.alliedCaptureVacancy=datedProduction.alliedCaptureVacancy;
            result.productionValue=datedProduction.net;result.productionOrders=std::move(datedProduction.orders);
            for(const auto &item:result.productionOrders){
                const auto buy=item.toObject();if(!buy["enemy"].toBool() || buy["buildHalfTurn"].toInt()!=1)continue;
                const auto xy=buy["factory"].toArray();const TilePoint born{xy[0].toInt(),xy[1].toInt()};
                for(const auto &a:friendlyActors){
                    const int i=a.unit;const auto &u=knowledge.units()[i];
                    if((!a.observed && !repricedOnF1[i] && !servicedQuote[i]) || productionPositionsAfterE0[i]==INVALID_TILE ||
                       !(u.canFire || (friendlyService && terrain.units[i].hasNativeWeapon)))continue;
                    const auto radius=(u.canMoveAndFire?terrain.units[i].movementBeforeFuel:0)+u.maxRange;
                    if(manhattanDistance(productionPositionsAfterE0[i],born)<=radius)
                        result.friendlyServiceOpen="F1_ENEMY_PURCHASE_MAY_CHANGE_FIRE_OR_MOVEMENT";
                }
            }
            if(terrain.production) cash=std::move(datedProduction.flow);
            const auto work = static_cast<std::int64_t>(horizon) * (initial.size() + transfers.size() + 1);
            if (work > static_cast<std::uint64_t>(budget))
                return {};
            budget -= work;
            for (int round = 1; round <= horizon; ++round)
            {
                const int enemyPay = 2 * round - 1, friendlyPay = 2 * round;
                auto &amount = cash[round - 1];
                for (const auto &c : initial)
                {
                    if (c.ownerBefore == OwnerSign::Ours && friendlyPay <= last &&
                        !RelativeStockDetail::checkedAdd(amount, c.income.oursPerTurn))
                        return {};
                    if (c.ownerBefore == OwnerSign::Enemy && enemyPay <= last &&
                        !RelativeStockDetail::checkedSubtract(amount, c.income.enemyPerTurn))
                        return {};
                }
                for (const auto &t : transfers)
                {
                    const int completed = 2 * t.day + int(t.enemy);
                    const auto previous = t.previous.ownerBefore;
                    const auto next = t.enemy ? OwnerSign::Enemy : OwnerSign::Ours;
                    if (friendlyPay > completed && friendlyPay <= last &&
                        !RelativeStockDetail::checkedAdd(
                            amount, t.previous.income.oursPerTurn *
                                        (int(next == OwnerSign::Ours) - int(previous == OwnerSign::Ours))))
                        return {};
                    if (enemyPay > completed && enemyPay <= last &&
                        !RelativeStockDetail::checkedAdd(
                            amount, t.previous.income.enemyPerTurn *
                                        (int(previous == OwnerSign::Enemy) - int(next == OwnerSign::Enemy))))
                        return {};
                }
            }
            for (const auto &[tick, loss] : losses)
                if (tick > 0 && tick <= last && !RelativeStockDetail::checkedSubtract(cash[(tick - 1) / 2], loss))
                    return {};
            // A separately financed reserve may act only after stock movement
            // it has actually observed. Compare the title already in the cash
            // book before ordinary town bids can reuse these actors.
            if(routeCache && !routeCache->fundedPostQuestions.isEmpty())
            {
                const auto requests=routeCache->fundedPostQuestions.value(routeCache->stockComparisonPolicy).toArray();
                for(const auto &v:requests)
                {
                    const auto question=v.toObject();const auto target=question["tile"].toArray();
                    const auto site=std::find_if(finalProperties.begin(),finalProperties.end(),[&](const auto &p){
                        return QJsonArray{p.tile.x,p.tile.y}==target;});
                    if(site==finalProperties.end())continue;
                    for(const auto &r:result.routeProofs)
                    {
                        const auto leg=r.toObject();const auto installments=leg["captureInstallments"].toArray();
                        if(leg["tile"]!=target || leg["unit"]!=question["capper"] || installments.isEmpty() ||
                           installments.first().toObject()["tick"]!=question["entry"])continue;
                        for(const auto &post:question["posts"].toArray())
                        {
                            const auto limit=std::min<std::int64_t>(budget/8,16LL*terrain.width*terrain.height+8LL*static_cast<std::int64_t>(positions.size()));
                            auto work=limit;
                            auto answer=hearFundedPost(question,post.toArray(),terrain,knowledge.units(),datedProduction,settled,
                                result.productionOrders,leg,*site,positions,real,supportLoad,cash,last,field.capturePointsToCapture(),
                                lower,cashTimingAdjustment,work);
                            budget-=limit-work;answer["usedWork"]=static_cast<qint64>(limit-work);
                            result.fundedPostResponses.append(answer);
                        }
                        break;
                    }
                }
            }
            if(result.terminalTick<0 && last>=5 && !settled.proofs.isEmpty() && !datedProduction.fundedGuns.empty())
            {
                const auto cap=std::int64_t(terrain.width)*terrain.height*std::max<std::int64_t>(16,positions.size()*horizon);
                const auto allowance=terrain.quoteWorkQuota(budget,32,cap);
                if(!allowance)return refuseWork("OBSERVED_STOCK_MARKET",terrain.quoteWorkOffer(budget,32,cap));
                auto work=*allowance;
                auto options=priceObservedStockMarket(terrain,knowledge.units(),real,supportLoad,lastShot,lastTargetTick,settled,
                    datedProduction,result.productionOrders,finalProperties,result.routeProofs,cash,homeHQ,enemyHQ,last,
                    lower,cashTimingAdjustment,work);
                result.observedStockResponses=std::move(options.evidence);
                result.observedStockComplete&=options.complete;
                if(options.selected)
                {
                    if(!applyObservedStockBid(terrain,last,*options.selected,settled,datedProduction,finalProperties,
                            result.claims,result.routeProofs,result.productionOrders,cash,result.stock,result.holdingLoss,&lower,&work))
                        result.observedStockComplete=false;
                    else settled.proofs=result.routeProofs;
                }
                budget-=*allowance-work;
            }
            // This is one contingent use of actors still genuinely free on the final calendar.
            // Reprice the original wallets and the same ten dated material/income payments.
            if (last>=3)
            {
                const auto area=static_cast<std::int64_t>(terrain.width)*terrain.height;
                auto townCap=horizon*(4*area+static_cast<std::int64_t>(hp.size())*hp.size());
                if(routeCache && routeCache->townEffort>1 && terrain.comparisonWork>0)
                {
                    const auto maximum=terrain.comparisonWork/16;
                    townCap=townCap>maximum/routeCache->townEffort?maximum:townCap*routeCache->townEffort;
                }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                if(routeCache && routeCache->privateTownQuoteCap)townCap=*routeCache->privateTownQuoteCap;
#endif
                const auto townQuota=terrain.quoteWorkQuota(budget,16,townCap);
                if (!townQuota) return refuseWork("TOWN_MARKET",terrain.quoteWorkOffer(budget,16,townCap));
                auto townWork=*townQuota;
                const auto offeredWork=townWork;
                std::vector<int> freeDay(positions.size());
                for (int i=0;i<static_cast<int>(positions.size());++i)
                {
                    freeDay[i]=std::max(lastShot[i]+1,nextShot[i]>0 ? 2 : 1);
                    // Actors retain already-priced shots and hits. A failed or abandoned
                    // capture can leave after the last such event and starts the new town at zero.
                    if (lastTargetTick[i]>=0)
                        freeDay[i]=std::max(freeDay[i],1+(lastTargetTick[i]-int(terrain.units[i].enemy))/2);
                    if (holdFacts[i] && finishDay[i]>=0) freeDay[i]=std::max(freeDay[i],finishDay[i]+1);
                }
                auto matched=routeCache?routeCache->requiredTownChallenges:std::vector<TownQuoteFocus>{};
                for(const auto &v:result.stockDutyReview)
                {
                    const auto row=v.toObject();
                    if(row[row["matchedStockIntention"].toBool()?"exactRetainedStockTransfer":"presentInStockItinerary"].toBool())continue;
                    const auto id=row["unit"].toInt(),date=row["entryTick"].toInt();
                    const auto tile=row["tile"].toArray();
                    matched.push_back({{tile[0].toInt(),tile[1].toInt()},date%2,id,date});
                }
                if(routeCache)if(const auto i=routeCache->priorityFor(routeCache->stockComparisonPolicy))
                    matched.push_back({{(*i)[0],(*i)[1]},(*i)[2],(*i)[3],(*i)[4],INVALID_TILE,*townQuota/2});
                std::vector<int> marketBranch{int(stayForCapture),reservedGuard,int(takeCombinedFollowup),
                    int(punishCombinedFollowup),int(interruptHomeCapture),int(branch)};
                if(approach || priorDecline || requestedFollowup)
                {
                    const auto appendApproach=[&](const CaptureApproach *a){
                        marketBranch.push_back(a?1:0);
                        if(!a)return;
                        marketBranch.insert(marketBranch.end(),{a->actor,a->shooter,a->origin.x,a->origin.y,
                            a->stage.x,a->stage.y,a->target.x,a->target.y,a->gun.x,a->gun.y,a->firstCost,a->rate,a->finish});
                    };
                    appendApproach(approach);
                    appendApproach(priorDecline);
                    marketBranch.push_back(requestedFollowup?1:0);
                    if(requestedFollowup)
                    {
                        marketBranch.insert(marketBranch.end(),{requestedFollowup->actor,requestedFollowup->vehicle,
                            requestedFollowup->target,requestedFollowup->weapon,requestedFollowup->end.x,requestedFollowup->end.y,
                            requestedFollowup->answer?1:0});
                        if(requestedFollowup->answer)
                            marketBranch.insert(marketBranch.end(),{requestedFollowup->answer->shooter,
                                requestedFollowup->answer->weapon,requestedFollowup->answer->end.x,requestedFollowup->answer->end.y});
                    }
                }
                auto contest=priceOrdinaryTownEncounters(terrain,knowledge.units(),datedProduction,result.productionOrders,
                    settled,finalProperties,positions,real,routeFuelLeft,supportLoad,freeDay,casualtyDay,
                    guardReserved ? reservedGuard : NO_UNIT,homeHQ,enemyHQ,horizon,last,field.capturePointsToCapture(),
                    lower,cash,cashTimingAdjustment,townWork,{},
                    std::span<const TownQuoteFocus>(matched),
                    routeCache?&routeCache->townMarkets:nullptr,marketBranch,true,screen);
                if(routeCache && routeCache->townMarkets.ownerHooks)routeCache->townMarkets.ownerHooks->published(contest,townWork);
                result.townEncounterBids=contest.bids;result.townEncounterWork=offeredWork-townWork;
                budget-=result.townEncounterWork;result.townEncounters=std::move(contest.evidence);
                result.townSearch=std::move(contest.searches);
                if(routeCache)
                {
                    std::set<int> frozenGuards;
                    for(const auto &row:contest.coverage)
                    {
                        const auto slot=row.toObject();
                        if(slot["complete"].toBool())continue;
                        for(const auto &group:slot["datedOccupants"].toArray())
                            for(const auto &v:group.toObject()["stops"].toArray())
                            {
                                const auto post=v.toObject();
                                if(post["unique"].toBool() && post.contains("unit") && !post["canRelease"].toBool())
                                    frozenGuards.insert(post["unit"].toInt());
                            }
                        const auto actor=slot["requestedActor"].toObject();
                        if(actor["conditionalGunThrough"].toInt(-1)>=0)frozenGuards.insert(actor["unit"].toInt(NO_UNIT));
                    }
                    for(const auto &[gun,obligations]:settled.townGunClaims)
                    {
                        if(gun<0 || gun>=static_cast<int>(terrain.units.size()) || !frozenGuards.contains(terrain.units[gun].engineUnitId))continue;
                        for(const auto &claim:obligations)if(claim.incomplete)
                            routeCache->seenDutyTrips.insert({terrain.units[claim.claimant].engineUnitId,claim.target.x,claim.target.y,claim.entry});
                    }
                }
                result.townCoverage=std::move(contest.coverage);
                result.townConflicts=std::move(contest.conflicts);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                if (routeCache && routeCache->townAuditFocus)
                {
                    // Price the same specified enemy opportunity against this exact team before
                    // applying the normally chosen contest. This witness never votes on the rank.
                    StockRouteLowerBounds auditBounds(&routeCache->preparedPropertyBounds);
                    std::int64_t auditWork=120000;
                    auto audit=priceOrdinaryTownEncounters(terrain,knowledge.units(),datedProduction,result.productionOrders,
                        settled,finalProperties,positions,real,routeFuelLeft,supportLoad,freeDay,casualtyDay,
                        guardReserved ? reservedGuard : NO_UNIT,homeHQ,enemyHQ,horizon,last,field.capturePointsToCapture(),
                        auditBounds,cash,cashTimingAdjustment,auditWork,routeCache->townAuditFocus,{},nullptr,{},true,screen);
                    result.townFocusAudit=townAuditRecord(audit);
                    result.townFocusAudit["allowance"]=120000;
                    result.townFocusAudit["used"]=static_cast<qint64>(120000-auditWork);
                }
#endif
                if (!contest.selected.empty())
                {
                    if (!contest.ledger) return {};
                    if (!RelativeStockDetail::checkedAdd(result.holdingLoss,contest.ledger->capitalLoss) ||
                        !RelativeStockDetail::checkedAdd(result.stock,contest.ledger->ownership)) return {};
                    cash=std::move(contest.ledger->flow);result.productionOrders=std::move(contest.purchases);
                    for (auto &applied:contest.selected)
                    {
                        const auto &line=applied.line;
                        if (line.finishTick<0) continue;
                        const auto col=std::find_if(finalProperties.begin(),finalProperties.end(),[&](const auto &p){return p.tile==line.town;});
                        if (col==finalProperties.end() || line.capper<0 || line.capper>=static_cast<int>(terrain.units.size())) return {};
                        const int engineId=terrain.units[line.capper].engineUnitId;
                        auto proof=std::move(applied.captureProof);
                        if (proof.isEmpty()) return {};
                        const int departure=proof["departureTick"].toInt();
                        const int gained=line.installments.front().second;
                        result.claims.append(QJsonObject{{"source","TOWN_ASSAULT"},{"unit",engineId},{"unitKnowledge",line.capper},
                            {"enemy",bool(line.side)},{"tile",QJsonArray{line.town.x,line.town.y}},
                            {"departureDayOffset",departure/2},{"entryDayOffset",line.entryTick/2},{"completionDayOffset",line.finishTick/2},
                            {"carry",0},{"rate",gained},{"ownerBefore",static_cast<int>(applied.priorOwner)},
                            {"signedIncome",static_cast<qint64>(applied.ownership)}});
                        result.routeProofs.append(std::move(proof));
                    }
                }
            }
            if(terrain.completeProductionMenu || (routeCache && !routeCache->fundedPostQuestions.isEmpty()))
            {
                std::set<int> dates;
                for(const auto &g:datedProduction.fundedGuns)
                    if(g.unit && g.buildTick+2<=last)dates.insert(g.buildTick+2);
                if(routeCache)for(const auto &v:routeCache->fundedPostQuestions.value(routeCache->stockComparisonPolicy).toArray())
                {
                    const int build=v.toObject()["build"].toInt(-1);
                    if(build>=0 && build+2<=last)dates.insert(build+2);
                }
                if(!dates.empty())
                {
                    prepareRouteFuel();prepareWeaponLoads();
                    QJsonArray origins,opening;
                    const auto square=[](TilePoint p){return QJsonArray{p.x,p.y};};
                    const auto work=4LL*static_cast<qint64>(positions.size()+plan.actors.size()+plan.shots.size());
                    bool valid=work<=budget && supportLoad.size()==positions.size() && routeFuelLeft.size()==positions.size();
                    if(valid)
                    {
                        budget-=work;
                        for(int i=0;i<int(positions.size());++i)
                        {
                            const auto &u=knowledge.units()[i];const auto &p=terrain.units[i];
                            origins.append(QJsonArray{p.engineUnitId,u.unitId,u.ownerId,p.present,square(p.tile),p.realHp,u.hpRounded,
                                u.capturePoints,u.fuel,u.ammo1,u.ammo2,square(productionPositionsAfterE0[i]),square(positions[i]),
                                real[i],hp[i],futureHp[i],carry[i],progress[i],routeFuelLeft[i],ammo[i],
                                QJsonArray{supportLoad[i][0],supportLoad[i][1]},lastShot[i],lastTargetTick[i],casualtyDay[i]});
                        }
                        for(const auto &a:plan.actors)
                        {
                            if(a.actionIndex<0 || a.actionIndex>=plan.replay.plan.actionCount()){valid=false;break;}
                            const auto &action=plan.replay.plan.action(a.actionIndex);
                            QJsonArray path;for(const auto tile:action.path)path.append(square(tile));
                            const auto &hit=a.ownShot;
                            opening.append(QJsonArray{a.engineUnitId,action.actionId,a.actionIndex,a.candidateIndex,path,
                                square(action.destination),square(action.target),action.targetUnitId,hit.attackerBefore,hit.attackerAfter,
                                hit.targetBefore,hit.targetAfter,hit.report.weapon,hit.report.counterWeapon});
                        }
                        for(const auto &shot:plan.shots)
                        {
                            const int victim=targetIndex(shot);
                            opening.append(QJsonArray{terrain.units[shot.shooterIndex].engineUnitId,terrain.units[victim].engineUnitId,
                                square(shot.firingTile),shot.realBefore,shot.realAfter,static_cast<qint64>(shot.counterCapital),static_cast<qint64>(shot.targetCapital)});
                        }
                    }
                    for(int first:dates)
                    {
                        if(valid)result.fundedPostPrefixes.emplace(first,measureFundedPostPrefix(result,first,terrain,settled,datedProduction,
                            finalProperties,origins,opening,losses,cash,budget));
                        else result.fundedPostPrefixes[first].open={{"reason","PREDEPLOYMENT_ACTOR_OR_ORDER_INPUT_UNAVAILABLE"}};
                    }
                }
            }
            if(!datedProduction.paidFounderWindows.isEmpty()){
                result.paidCaptureUnsupported["futureProduction"]=datedProduction.paidFounderWindows;
                result.observedStockComplete=false;
            }
            auto adjustment = cashTimingAdjustment(cash);
            if (!adjustment)
                return {};
            result.timingAdjustment = *adjustment;
            for (auto value : cash)
                result.roundCashFlow.append(static_cast<qint64>(value));
        }
        if(requestedFollowup && takeCombinedFollowup && !result.followupDecision.contains("responseMenu")){
            // A later casualty cannot certify a vacancy on an earlier date.
            const bool absent=horizon<=1 || result.terminalTick==1;
            result.followupDecision["responseMenu"]=QJsonObject{{"answers",QJsonArray{}},{"decline",true},
                {"enumerationComplete",absent},{"physicalE1MenuComplete",absent}};
        }
        if(requestedFollowup && requestedFollowup->answer && !requestedAnswerPlayed)return {};
        return result;
    }
    // Initial purchase proposals are inputs to reply evaluation. No unproved
    // stock value leaves this interface or ranks an executable squad.
    inline std::optional<QJsonArray> proposeOpeningPurchases(const CombatTerrain &terrain,const BattlefieldKnowledge &knowledge,
        const MasterAssignmentProblem &problem,const PropertyStockField &field,const ReplyPassEvaluation &plan,
        std::int64_t &budget,TilePoint homeHQ,TilePoint enemyHQ,CaptureRaceRouteCache *cache)
    {
        auto result=projectCaptureRaceAttempt(true,terrain,knowledge,problem,field,plan,budget,homeHQ,enemyHQ,
            false,NO_UNIT,false,false,cache);
        if(!result) return {};
        return std::move(result->productionOrders);
    }

    inline std::optional<CaptureRaceResult> projectCaptureRace(
        const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge, const MasterAssignmentProblem &problem,
        const PropertyStockField &field, const ReplyPassEvaluation &plan, std::int64_t &budget,
        TilePoint homeHQ = INVALID_TILE, TilePoint enemyHQ = INVALID_TILE, bool stayForCapture = false,
        int reservedGuard = NO_UNIT, bool takeCombinedFollowup = false, bool punishCombinedFollowup = false,
        CaptureRaceRouteCache *routeCache = nullptr, bool interruptHomeCapture = false,
        ApproachBranch branch=ApproachBranch::Observe, const CaptureApproach *approach=nullptr,
        const CaptureApproach *priorDecline=nullptr, const RequestedFollowup *requestedFollowup=nullptr)
    {
        bool selectedNewSource=false;
        const auto run=[&](bool coupled){
            return projectCaptureRaceAttempt(false,terrain,knowledge,problem,field,plan,budget,homeHQ,enemyHQ,
                stayForCapture,reservedGuard,takeCombinedFollowup,punishCombinedFollowup,routeCache,interruptHomeCapture,
                branch,approach,priorDecline,requestedFollowup,coupled,coupled?&selectedNewSource:nullptr);
        };
        auto result=run(true);
        if(!selectedNewSource || !routeCache)return result;
        if(result){
            result->observedStockComplete=false;
            result->paidCaptureUnsupported["contingentPortfolio"]="ALTERNATIVE_FUNDING_AND_RECRUIT_JOBS_UNPRICED";
            return result;
        }
        const auto rejected=routeCache->lastFailure;
        auto retained=run(false);
        if(retained){
            retained->observedStockComplete=false;
            retained->paidCaptureUnsupported["rejectedFuturePurchase"]=rejected;
        }else if(!rejected.isEmpty()){
            const auto baseline=routeCache->lastFailure;
            routeCache->lastFailure={{"phase","FUTURE_PURCHASE_AND_RETAINED_POLICY_OPEN"},
                {"futurePurchase",rejected},{"retainedPolicy",baseline}};
        }
        return retained;
    }
} // namespace Coordinator
