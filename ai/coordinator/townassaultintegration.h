#pragma once

#include "ai/coordinator/townopportunitymenu.h"
#include "ai/coordinator/townrivalguard.h"
#include "ai/coordinator/townreportfields.h"
#include "ai/coordinator/conditionalrivalreprice.h"
#include "ai/coordinator/townobservedresponses.h"
#include "ai/coordinator/towninitiativeportfolio.h"

#include "ai/coordinator/townassaultledger.h"
#include "ai/coordinator/townfrontieradmission.h"
#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/stocktowntimeline.h"
#include "ai/coordinator/stockcasualtycontinuation.h"
#include "ai/coordinator/townmarketidentity.h"
#include "ai/coordinator/townteammaterelease.h"
#include "ai/coordinator/townretainedowner.h"
#include <QJsonDocument>
#include <QFile>
#include <QThread>
#include <atomic>
#include <thread>

#include "ai/coordinator/townwithdrawaljob.h"
#include "ai/coordinator/townnamedstockrelease.h"

namespace Coordinator
{
    struct PricedTownEncounter
    {
        std::vector<AppliedTownEncounter> selected;
        std::vector<TownChallengeReceipt> challenges;
        std::optional<TownAssaultLedger> ledger;
        QJsonArray purchases,auditOriginalPurchases,evidence,searches,coverage,conflicts;
        QJsonObject requestedCapper;
        std::set<std::array<int,5>> retainedBoardBounds;
        std::map<std::array<int,5>,std::vector<TownArrivalCeiling>> landingBounds;
        std::set<std::array<int,5>> boundsWithoutWitness;

        void retainBound(std::array<int,5> key,const TownArrivalCeiling *witness=nullptr)
        {
            retainedBoardBounds.insert(key);
            if(witness && witness->witness)landingBounds[key].push_back(*witness);
            else boundsWithoutWitness.insert(key);
        }
        int bids{};
        std::int64_t work{};
        std::int64_t ownerAuditAllowance{},ownerAuditWork{};
        const AppliedTownEncounter *firstSelected() const { return selected.empty()?nullptr:&selected.front(); }
    };

    // A frame owns the local proofs before the two initiatives are joined.
    // The final rank, allocation of effort, private geometry and callback
    // views remain local to the current comparison.
    struct TownRivalReport
    {
        qsizetype index{};
        QJsonObject opening;
        std::array<int,8> suffix{};
        std::int64_t credit{};
        std::shared_ptr<const TownRivalGuardComparison> contest;
        QJsonObject auditExpected;
    };

    struct TownMarketFrame
    {
        struct SlotQuote
        {
            PricedTownEncounter price;
            std::vector<TownRivalReport> pendingReports;
            std::array<std::vector<PricedTownInitiative>,2> leaders;
            std::int64_t restartFloor{};
            bool advanced{};
        };
        struct DateWork
        {
            std::optional<QJsonObject> conditionalReprice;
            TownAssaultProgress attack;
            TownRivalSuffixes suffixes;
            std::shared_ptr<TownReleaseSearch> release;
            std::optional<NeutralOccupancy> arrival;
            std::shared_ptr<const TownRivalGuardComparison> finalContest;
            int resumedSearches{},retainedFirstShots{};
            bool complete() const
            {
                return conditionalReprice.has_value() || attack.complete || (release && release->complete) || bool(finalContest) || (arrival && arrival->originUnknown);
            }
        };
        using DatedProgress=std::map<std::tuple<std::size_t,int,int,int,int,int,bool>,DateWork>;
        struct PrefixWork
        {
            NeutralRouteHistory routes;
            std::map<std::tuple<std::size_t,int,int,int,int>,std::shared_ptr<TownReleaseSearch>> friendlyReleases;
            std::map<std::size_t,std::map<std::pair<int,bool>,NeutralOccupancy>> candidates;
            std::map<std::size_t,TownRivalOpenPolicy> open;
            std::map<std::vector<int>,TownRivalPrefix> known;
            int pass{};
        };

        using Key=std::array<int,8>;
        using Discovery=std::set<std::array<qint64,8>>;
        struct Saved
        {
            std::optional<SlotQuote> finished;
            std::shared_ptr<const Discovery> finishedDiscovery;
            QJsonObject decisionAdmission;std::int64_t decisionAdmissionOffer{};
            std::set<std::array<int,5>> decisionAdmissionCovered;
            DatedProgress dates;
            PrefixWork prefixes;
            std::size_t retainedBytes{};
        };
        std::map<Key,Saved> opportunities;
        std::uint64_t physicalRevision{};
        TownGuardStops guardStops;
        std::shared_ptr<const int> world=std::make_shared<const int>(0);
        std::size_t retainedBytes{};
    };

    struct DetachedTownFrame
    {
        TownMarketFrame *frame{};
        std::shared_ptr<const TownMarketFrame::Discovery> discovery;
    };
    struct TownMarketOwnerHooks;
    struct TownMarketHistory
    {
        struct Record
        {
            std::vector<int> label,branch;
            TownMarketIdentity board;
            TownMarketFrame frame;
            std::uint64_t lastUse{};
            bool allowAliases{};
        };
        struct Squad {std::vector<int> orders;std::vector<Record> frames;};
        std::shared_ptr<const TownMarketWorld> world=std::make_shared<const TownMarketWorld>();
        std::optional<Squad> reference,challenger;
        std::vector<int> orders,reply;
        bool enabled{};
        int completedReads{},resumedReads{},aliasCompletedReads{},priorityEvictions{};
        std::uint64_t visits{};
        TownMarketOwnerHooks *ownerHooks{};

        TownMarketFrame *find(TownMarketIdentity board,std::span<const int> branch,bool priority,bool allowAliases,
                              std::vector<const TownMarketFrame*> &aliases)
        {
            if(orders.empty())return nullptr;
            auto &owner=!reference || reference->orders==orders?reference:challenger;
            if(!owner || owner->orders!=orders)owner.emplace(Squad{orders,{}});
            const std::vector<int> decisions(branch.begin(),branch.end());
            auto &frames=owner->frames;
            ++visits;
            const auto found=std::find_if(frames.begin(),frames.end(),[&](const auto &f){
                return f.allowAliases==allowAliases && f.label==reply && f.branch==decisions && f.board==board;
            });
            static constexpr std::size_t MAX_RETAINED_TOWN_FRAMES=8;
            Record *active=nullptr;
            if(found!=frames.end())active=&*found;
            else if(frames.size()<MAX_RETAINED_TOWN_FRAMES)
            {
                frames.push_back({reply,decisions,std::move(board),{},visits,allowAliases});
                active=&frames.back();
            }
            else if(priority)
            {
                // A focused hearing late in the menu can retain its own cursor.
                // Replacing the least recently visited frame only forgets work.
                auto oldest=std::min_element(frames.begin(),frames.end(),[](const auto &a,const auto &b){return a.lastUse<b.lastUse;});
                *oldest={reply,decisions,std::move(board),{},visits,allowAliases};active=&*oldest;++priorityEvictions;
            }
            if(active)active->lastUse=visits;
            if(allowAliases)for(const auto &f:frames)
            {
                if(&f==active || !f.allowAliases || f.label==reply || f.branch!=decisions)continue;
                if(f.board==(active?active->board:board))aliases.push_back(&f.frame);
            }
            return active?&active->frame:nullptr;
        }
    };

 }
#include "ai/coordinator/townmarketownerhooks.h"
#include "ai/coordinator/datedformulaledger.h"
#include <QScopeGuard>
#include <QCborValue>
namespace Coordinator
{
    inline void recheckRetainedTownBounds(PricedTownEncounter &result,
        const std::function<bool(const std::array<int,5>&,const std::vector<TownArrivalCeiling>&)> &holds={})
    {
        if(result.selected.empty())return;
        // Either team can open a route, remove a guard or change the wallet.
        // These specific negative results only hold on the retained board.
        for(const auto &slot:result.retainedBoardBounds)
        {
            const auto where=result.landingBounds.find(slot);
            if(holds && !result.boundsWithoutWitness.contains(slot) && where!=result.landingBounds.end() &&
               holds(slot,where->second))continue;
            result.conflicts.append(QJsonObject{{"status","RECHECK_DECLINED_GUARD_ON_CHANGED_PORTFOLIO"},
                {"tile",QJsonArray{slot[0],slot[1]}},{"attackingSide",slot[2]},
                {"capper",slot[3]},{"entryTick",slot[4]}});
        }
    }

    template<class Timing> bool landingCounterStillHolds(const TownAssaultBoard &board,
        const TownArrivalCeiling &proof,std::span<const AppliedTownEncounter> selected,std::span<const MilliFunds> jointCash,
        int cutoff,Timing timing,std::int64_t &budget)
    {
        if(!proof.witness || proof.witness->finishTick>=0 || (proof.assumedArrival && !selected.empty()))return false;
        const auto &counter=*proof.witness;
        const int side=counter.side,entry=counter.entryTick;
        for(const auto &other:selected)
        {
            if(budget<=0)return false;
            --budget;
            if(!incompatibleTownInitiatives(board,counter,other.line,budget).isEmpty())return false;
            // A newly moved screen can create a profitable escorted clearing
            // that the previously empty town did not offer. A capture of the
            // same property also changes the challenge outright.
            if(other.line.town==counter.town)return false;
            for(const auto &m:other.line.moves)
            {
                if(m.fromTick>entry)continue;
                if(manhattanDistance(m.origin,counter.town)<=1)return false;
                for(const auto &step:m.path.steps)
                {
                    if(budget<=0)return false;
                    --budget;
                    if(step.tick<=entry && manhattanDistance(step.tile,counter.town)<=1)return false;
                }
            }
        }
        // A separate initiative can change the timing adjustment or impair
        // solvency. Here every certified continuation earns zero title and
        // keeps the existing buyer queue. Reprice its actual material losses
        // (and each free exit) on the joint, already funded cash flow.
        PropertyStockColumn noTransfer;noTransfer.tile=counter.town;
        auto prefix=counter;
        const auto denied=[&](){
            if(budget<1+static_cast<std::int64_t>(prefix.losses.size()))return false;
            budget-=1+prefix.losses.size();
            const auto priced=townAssaultLedger(prefix,noTransfer,jointCash,cutoff,timing);
            return priced && !TownAssaultDetail::better(priced->score,0,side);
        };
        if(!denied())return false;
        for(const auto &v:proof.withdrawals)
        {
            const auto t=v.toObject()["afterTick"].toInt(-1);
            if(t<entry)return false;
            prefix.losses.clear();
            for(const auto &loss:counter.losses)if(loss.first<=t)prefix.losses.push_back(loss);
            if(!denied())return false;
        }
        return true;
    }

    // Each side can exercise one offensive initiative. Compare all its
    // individually best options before choosing mutually compatible uses.
    // Sequential cash flow composes exact dated title and material changes.
    template<class Timing>
    void settleTownInitiatives(PricedTownEncounter &result,const TownAssaultBoard &board,
        std::array<std::vector<PricedTownInitiative>,2> &leaders,int cutoff,Timing timing,std::int64_t &budget)
    {
        auto apply=[&](PricedTownInitiative &bid,MilliFunds otherAlone) {
            auto witness=result.evidence[bid.evidence].toObject();witness["appliedToSchedule"]=true;
            const auto &line=bid.chosen.line;
            if(line.capper>=0 && line.capper<static_cast<int>(board.terrain.units.size()) && line.entryTick>=0 && result.ledger)
            {
                auto marginal=result.ledger->score;
                if(RelativeStockDetail::checkedSubtract(marginal,otherAlone))
                {
                    witness["appliedMarginal"]=static_cast<qint64>(marginal);
                    witness["appliedIntention"]=QJsonObject{{"tile",QJsonArray{line.town.x,line.town.y}},
                        {"attackingSide",line.side},{"capper",board.terrain.units[line.capper].engineUnitId},{"entryTick",line.entryTick}};
                }
            }
            result.evidence[bid.evidence]=witness;result.selected.push_back(std::move(bid.chosen));
        };
        if(leaders[0].empty() || leaders[1].empty())
        {
            for(auto &side:leaders) if(!side.empty())
            {
                auto &bid=side.front();result.ledger=bid.ledger;result.purchases=bid.purchases;apply(bid,0);
            }
            return;
        }
        QJsonArray conflicts;
        for(auto &f:leaders[0]) for(auto &e:leaders[1])
        {
            auto conflict=incompatibleTownInitiatives(board,f.chosen.line,e.chosen.line,budget);
            if(!conflict.isEmpty())
            {
                if(conflicts.size()<16)conflicts.append(conflict);
                if(budget<=0)break;
                continue;
            }
            auto wallet=townAssaultPurchases(f.purchases,e.chosen.line,e.property);
            auto joint=townAssaultLedger(e.chosen.line,e.property,f.ledger.flow,cutoff,timing);
            if(!wallet || !joint || !RelativeStockDetail::checkedAdd(joint->ownership,f.ledger.ownership) ||
               !RelativeStockDetail::checkedAdd(joint->capitalLoss,f.ledger.capitalLoss) ||
               !RelativeStockDetail::checkedAdd(joint->score,f.ledger.score) || !RelativeStockDetail::checkedAdd(joint->timing,f.ledger.timing))
            {conflicts.append(QJsonObject{{"status","JOINT_WALLET_OR_ARITHMETIC_UNRESOLVED"}});continue;}
            result.ledger=std::move(*joint);result.purchases=std::move(*wallet);apply(f,e.ledger.score);apply(e,f.ledger.score);return;
        }
        result.conflicts=std::move(conflicts);
        for(int i=0;i<static_cast<int>(result.challenges.size());++i)
        {
            auto &r=result.challenges[i];const auto &options=leaders[r.attackingSide];
            if(std::none_of(options.begin(),options.end(),[&](const auto &b){return b.property.tile==r.tile;})) continue;
            r.status=TownChallengeStatus::Conflict;r.signedChange=0;
            auto detail=result.coverage[i].toObject();detail["status"]="CONFLICT";detail["complete"]=false;
            detail["signedChange"]=QJsonValue(QJsonValue::Null);detail["reason"]="JOINT_INITIATIVES_UNPRICED";
            result.coverage[i]=detail;
        }
    }

    inline QJsonObject townAuditRecord(const PricedTownEncounter &result)
    {
        QJsonArray applied;
        for(const auto &s:result.selected) applied.append(QJsonObject{{"town",QJsonArray{s.line.town.x,s.line.town.y}},
            {"attackingSide",s.line.side},{"entryTick",s.line.entryTick},{"finishTick",s.line.finishTick},
            {"ownership",static_cast<qint64>(s.ownership)},{"captureProof",s.captureProof}});
        QJsonObject record{{"coverage",result.coverage},{"encounters",result.evidence},{"search",result.searches},
            {"selected",applied},{"conflicts",result.conflicts},{"requestedCapper",result.requestedCapper},
            {"fundedPurchases",result.auditOriginalPurchases},{"purchasesAfterSelectedLines",result.purchases},
            {"work",static_cast<qint64>(result.work)}};
        if(result.ownerAuditAllowance>0)record["retainedOwnerAuditMeter"]=QJsonObject{
            {"allowance",static_cast<qint64>(result.ownerAuditAllowance)},{"used",static_cast<qint64>(result.ownerAuditWork)}};
        if(result.ledger) record["ledger"]=QJsonObject{{"ownership",static_cast<qint64>(result.ledger->ownership)},
            {"capitalLoss",static_cast<qint64>(result.ledger->capitalLoss)},{"timing",static_cast<qint64>(result.ledger->timing)},
            {"score",static_cast<qint64>(result.ledger->score)}};
        return record;
    }

    template<class Identity> QJsonObject renderTownEncounter(int width,const TownAssaultLine &line,Identity identity)
    {
        const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
        QJsonArray moves,shots,installments;
        for (const auto &move:line.moves)
        {
            QJsonArray route;
            for (const auto &step:move.path.steps)
            {
                QJsonArray cells;
                for (auto sq:step.cells) cells.append(point({sq%width,sq/width}));
                route.append(QJsonObject{{QStringLiteral("tick"),step.tick},{QStringLiteral("stop"),point(step.tile)},{QStringLiteral("cells"),cells}});
            }
            moves.append(QJsonObject{{QStringLiteral("actor"),identity(move.actor)},{QStringLiteral("from"),point(move.origin)},{QStringLiteral("fuel"),move.path.fuel},
                {QStringLiteral("to"),point(move.destination)},{QStringLiteral("actionTick"),move.actionTick},{QStringLiteral("route"),route}});
        }
        for (const auto &shot:line.shots)
            shots.append(QJsonObject{{QStringLiteral("actor"),identity(shot.actor)},{QStringLiteral("target"),identity(shot.target)},
                {QStringLiteral("tick"),shot.tick},{QStringLiteral("from"),point(shot.from)},{QStringLiteral("to"),point(shot.to)},{QStringLiteral("weapon"),shot.weapon},{QStringLiteral("counterWeapon"),shot.counterWeapon},
                {QStringLiteral("targetBefore"),shot.targetBefore},{QStringLiteral("targetAfter"),shot.targetAfter},
                {QStringLiteral("shooterBefore"),shot.actorBefore},{QStringLiteral("shooterAfter"),shot.actorAfter},
                {QStringLiteral("targetLoss"),static_cast<qint64>(shot.targetLoss)},{QStringLiteral("counterLoss"),static_cast<qint64>(shot.counterLoss)}});
        for (auto [tick,progress]:line.installments) installments.append(QJsonArray{tick,progress});
        return {{QStringLiteral("enemy"),bool(line.side)},{QStringLiteral("tile"),point(line.town)},{QStringLiteral("guard"),identity(line.blocker)},
            {QStringLiteral("capper"),identity(line.capper)},{QStringLiteral("entryTick"),line.entryTick},{QStringLiteral("finishTick"),line.finishTick},
            {QStringLiteral("capperHp"),line.capperHp},{QStringLiteral("moves"),moves},{QStringLiteral("shots"),shots},{QStringLiteral("installments"),installments}};
    }

    inline QJsonObject describeTownEncounter(const CombatTerrain &terrain,const ProductionProjection &production,const TownAssaultLine &line)
    {
        return renderTownEncounter(terrain.width,line,[&](int i){return townActorIdentity(terrain,production.fundedGuns,i);});
    }

    template<class Describe> QJsonObject renderTownArrival(const QJsonObject &who,int tick,const TownArrivalCeiling &proof,Describe describe)
    {
        QJsonObject result{{"unit",who.value("unit").toInt(NO_UNIT)},{"entry",tick},{"attackerCeiling",static_cast<qint64>(proof.value)},
            {"withdrawals",proof.withdrawals},{"onTownDefenders",proof.defenders},{"assumedArrivalBeforeRoute",proof.assumedArrival}};
        if(!proof.complete())
        {
            result.remove("attackerCeiling");
            result[proof.withdrawalCoverage?"captureOrWithdrawValue":"continueCaptureValue"]=static_cast<qint64>(proof.value);
            result["scope"]=proof.nativeExchangeRequired?"NATIVE_COMBAT_MODIFIERS_UNPRICED":"UNEXAMINED_POST_ENTRY_ACTION";
            if(proof.unexaminedActionTick>=0)
            {
                result["nextCapperAction"]=proof.unexaminedActionTick;
                result["capperHpAtNextAction"]=proof.hpAtUnexaminedAction;
            }
            if(!proof.alternative.reason.isEmpty())
            {
                const auto &a=proof.alternative;
                result["scope"]=a.reason;
                result["linkedJob"]=QJsonObject{{"tile",QJsonArray{a.tile.x,a.tile.y}},{"unit",a.actor},{"tick",a.tick}};
            }
            if(proof.witness)result["conditionalResponse"]=describe(*proof.witness);
        }
        if(proof.lateOptions.attempted){const auto &a=proof.lateOptions;
            QJsonObject p{{"ownTick",a.tick},{"hp",a.hp},{"remainingFuel",a.remainingFuel},{"ammo",QJsonArray{a.remainingAmmo[0],a.remainingAmmo[1]}},
                {"actions",a.actions},{"open",a.open},{"futureJobs",a.jobs},{"enumerated",a.enumerated},{"allArrivalResources",a.allArrivalResources}};
            if(a.withdraw)p["withdrawalUpper"]=static_cast<qint64>(*a.withdraw);if(a.best)p["attackerBoundIncludingSafeExit"]=static_cast<qint64>(*a.best);result["activeCapperMenu"]=p;
        }
        if(who.contains("birth"))result["paidCapper"]=who;
        if(who.isEmpty())result["actorStatus"]="UNRESOLVED_PARTICIPANT_IDENTITY";
        if(proof.ordinaryAfterConditional && proof.witness)
            result["ordinaryResponseAfterWithholdingCommittedGun"]=describe(*proof.witness);
        return result;
    }

    inline QJsonObject townArrivalRecord(const CombatTerrain &terrain,const ProductionProjection &production,int actor,int tick,
        const TownArrivalCeiling &proof)
    {
        return renderTownArrival(townActorIdentity(terrain,production.fundedGuns,actor),tick,proof,
            [&](const auto &line){return describeTownEncounter(terrain,production,line);});
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    inline QJsonObject sealedTownEncounter(const CombatTerrain &terrain,const ProductionProjection &production,
        const TownAssaultLine &line)
    {
        const auto point=[](TilePoint p) {return QJsonArray{p.x,p.y};};
        const auto identity=[&](int i){return townActorIdentity(terrain,production.fundedGuns,i);};
        QJsonArray moves,shots,installments;
        for (const auto &move:line.moves)
        {
            QJsonArray route;
            for (const auto &step:move.path.steps)
            {
                QJsonArray cells;
                for (auto sq:step.cells) cells.append(point({sq%terrain.width,sq/terrain.width}));
                route.append(QJsonObject{{QStringLiteral("tick"),step.tick},{QStringLiteral("stop"),point(step.tile)},{QStringLiteral("cells"),cells}});
            }
            moves.append(QJsonObject{{QStringLiteral("actor"),identity(move.actor)},{QStringLiteral("from"),point(move.origin)},{QStringLiteral("fuel"),move.path.fuel},
                {QStringLiteral("to"),point(move.destination)},{QStringLiteral("actionTick"),move.actionTick},{QStringLiteral("route"),route}});
        }
        for (const auto &shot:line.shots)
            shots.append(QJsonObject{{QStringLiteral("actor"),identity(shot.actor)},{QStringLiteral("target"),identity(shot.target)},
                {QStringLiteral("tick"),shot.tick},{QStringLiteral("from"),point(shot.from)},{QStringLiteral("to"),point(shot.to)},{QStringLiteral("weapon"),shot.weapon},{QStringLiteral("counterWeapon"),shot.counterWeapon},
                {QStringLiteral("targetBefore"),shot.targetBefore},{QStringLiteral("targetAfter"),shot.targetAfter},
                {QStringLiteral("shooterBefore"),shot.actorBefore},{QStringLiteral("shooterAfter"),shot.actorAfter},
                {QStringLiteral("targetLoss"),static_cast<qint64>(shot.targetLoss)},{QStringLiteral("counterLoss"),static_cast<qint64>(shot.counterLoss)}});
        for (auto [tick,progress]:line.installments) installments.append(QJsonArray{tick,progress});
        return {{QStringLiteral("enemy"),bool(line.side)},{QStringLiteral("tile"),point(line.town)},{QStringLiteral("guard"),identity(line.blocker)},
            {QStringLiteral("capper"),identity(line.capper)},{QStringLiteral("entryTick"),line.entryTick},{QStringLiteral("finishTick"),line.finishTick},
            {QStringLiteral("capperHp"),line.capperHp},{QStringLiteral("moves"),moves},{QStringLiteral("shots"),shots},{QStringLiteral("installments"),installments}};
    }

    inline QJsonObject sealedTownArrivalRecord(const CombatTerrain &terrain,const ProductionProjection &production,int actor,int tick,
        const TownArrivalCeiling &proof)
    {
        const auto who=townActorIdentity(terrain,production.fundedGuns,actor);
        QJsonObject result{{"unit",who.value("unit").toInt(NO_UNIT)},{"entry",tick},{"attackerCeiling",static_cast<qint64>(proof.value)},
            {"withdrawals",proof.withdrawals},{"onTownDefenders",proof.defenders},{"assumedArrivalBeforeRoute",proof.assumedArrival}};
        if(!proof.complete())
        {
            result.remove("attackerCeiling");
            result[proof.withdrawalCoverage?"captureOrWithdrawValue":"continueCaptureValue"]=static_cast<qint64>(proof.value);
            result["scope"]=proof.nativeExchangeRequired?"NATIVE_COMBAT_MODIFIERS_UNPRICED":"UNEXAMINED_POST_ENTRY_ACTION";
            if(proof.unexaminedActionTick>=0)
            {
                result["nextCapperAction"]=proof.unexaminedActionTick;
                result["capperHpAtNextAction"]=proof.hpAtUnexaminedAction;
            }
            if(!proof.alternative.reason.isEmpty())
            {
                const auto &a=proof.alternative;
                result["scope"]=a.reason;
                result["linkedJob"]=QJsonObject{{"tile",QJsonArray{a.tile.x,a.tile.y}},{"unit",a.actor},{"tick",a.tick}};
            }
            if(proof.witness)result["conditionalResponse"]=sealedTownEncounter(terrain,production,*proof.witness);
        }
        if(proof.lateOptions.attempted){const auto &a=proof.lateOptions;
            QJsonObject p{{"ownTick",a.tick},{"hp",a.hp},{"remainingFuel",a.remainingFuel},{"ammo",QJsonArray{a.remainingAmmo[0],a.remainingAmmo[1]}},
                {"actions",a.actions},{"open",a.open},{"futureJobs",a.jobs},{"enumerated",a.enumerated},{"allArrivalResources",a.allArrivalResources}};
            if(a.withdraw)p["withdrawalUpper"]=static_cast<qint64>(*a.withdraw);if(a.best)p["attackerBoundIncludingSafeExit"]=static_cast<qint64>(*a.best);result["activeCapperMenu"]=p;
        }
        if(who.contains("birth"))result["paidCapper"]=who;
        if(who.isEmpty())result["actorStatus"]="UNRESOLVED_PARTICIPANT_IDENTITY";
        if(proof.ordinaryAfterConditional && proof.witness)
            result["ordinaryResponseAfterWithholdingCommittedGun"]=sealedTownEncounter(terrain,production,*proof.witness);
        return result;
    }

    inline QJsonObject verifySealedRival(const CombatTerrain &terrain,const ProductionProjection &production,
        const QJsonArray &orders,const TownMarketFrame::DateWork &day,const TownRivalGuardComparison &contest,QJsonObject r,
        std::int64_t suffixCredit)
    {
        const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const auto describeEncounter=[&](const TownAssaultLine &line){return sealedTownEncounter(terrain,production,line);};
        if(!contest.retainedWalk.isEmpty())r["singleOriginalBlockerWalk"]=contest.retainedWalk;
        r["guardedSuffixResumes"]=contest.heldResumes;r["observedSuffixResumes"]=contest.observedResumes;
        r["guardedCompleteReads"]=day.suffixes.held.completeReads;
        r["observedCompleteReads"]=day.suffixes.afterFire.completeReads;
        r["guardedFirstShotsRestored"]=day.suffixes.held.retainedFirstShots;
        r["observedFirstShotsRestored"]=day.suffixes.afterFire.retainedFirstShots;
        r["guardedNextTick"]=day.suffixes.held.search.tick;r["observedNextTick"]=day.suffixes.afterFire.search.tick;
        r["guardedNextLane"]=day.suffixes.held.search.firstLane;r["observedNextLane"]=day.suffixes.afterFire.search.firstLane;
        r["suffixWorkRetained"]=static_cast<qint64>(suffixCredit);
        r["openResponseReused"]=contest.reusedOpen;r["earlierCaptureReused"]=contest.reusedCapture;r["quietThrough"]=contest.verifiedGuardThrough;
        r["reusedGuardExchangeThrough"]=contest.reusedFireThrough;
        r["guardStatus"]=contest.guardStatus;
        if(!contest.bookedApproachShot.isEmpty())r["bookedApproachShot"]=contest.bookedApproachShot;
        if(contest.guardGunsChecked>=0)
        {
            r["guardApproachGunsCleared"]=contest.guardGunsChecked;
            r["guardApproachRouteExpansions"]=contest.guardRouteExpansions;
            r["approachShotScope"]="FIRST_EXECUTABLE_PROFITABLE_OR_BOOKED_SHOT";
        }
        if(contest.guardPendingTick>=0)
        {
            r["guardPendingTick"]=contest.guardPendingTick;
            r["guardPendingTile"]=point(contest.guardPendingTile);
        }
        r["guardDepartureTick"]=contest.departure;r["guardFuelAfter"]=contest.remainingFuel;
        auto describe=[&](const TownAssaultAlternatives &answer){
            QJsonObject q{{"workLimited",answer.workLimited},{"unresolved",answer.unresolved},
                {"capperRoutes",answer.search.capperRoutes},{"fullyPricedLines",answer.search.fullyPriced},
                {"unassistedArrivalsBounded",answer.search.dominatedAtArrival},
                {"declineScore",static_cast<qint64>(answer.declineValue)}};
            if(!answer.unpricedPaidArrivals.isEmpty())q["unexaminedPaidEntrants"]=answer.unpricedPaidArrivals;
            if(!answer.ordinaryCounterChecks.isEmpty())q["retainedOrdinaryCounterChecks"]=answer.ordinaryCounterChecks;
            if(answer.assault)
            {
                q["assault"]=describeEncounter(*answer.assault);
                q["unansweredScore"]=static_cast<qint64>(answer.assaultValue);
                q["withResponseScore"]=static_cast<qint64>(answer.responseValue);
                q["choosesAssault"]=answer.selectsAssault;
                if(answer.response)q["response"]=describeEncounter(*answer.response);
            }
            if(!answer.partialArrivalActions.empty())
            {
                QJsonArray partial;
                for(const auto &[k,p]:answer.partialArrivalActions)
                    partial.append(sealedTownArrivalRecord(terrain,production,k.first,k.second,p));
                q["conditionalPostEntryResponses"]=partial;
            }
            if(!answer.arrivalsRefuted.empty())
            {
                QJsonArray bounds;
                for(const auto &[intent,proof]:answer.arrivalsRefuted)
                {
                    auto receipt=sealedTownArrivalRecord(terrain,production,intent.first,intent.second,proof);
                    if(receipt.contains("actorStatus"))q["unresolved"]=true;
                    bounds.append(receipt);
                }
                q["unassistedArrivalBounds"]=bounds;
            }
            return q;
        };
        if(contest.openRefutes){}
        else if(contest.paidGuardUnnecessaryForThisAttack)
        {
            r["ifRivalDeclinesEarlyOccupation"]=describe(contest.open);
            r["attackerUpperBound"]=static_cast<qint64>(contest.open.declineValue);
            r["keepsOtherFundedJobsOpen"]=true;
        }
        else if(contest.dominatedOnRetainedBoard)
        {
            r["ifRivalDeclinesEarlyOccupation"]=describe(contest.open);
            r["attackerUpperBound"]=static_cast<qint64>(contest.open.declineValue);
        }
        else
        {
            r["ifRivalDeclinesEarlyOccupation"]=describe(contest.open);
            if(contest.guarded)r["ifRivalGuardsWithoutCapturing"]=describe(*contest.guarded);
            if(contest.earlierCapture)r["ifRivalStartsCapture"]=describe(*contest.earlierCapture);
            if(contest.afterEarlyTitle)
            {
                const auto &after=*contest.afterEarlyTitle;
                QJsonObject account{{"status",after.status},{"holderCanActBeforeLateEntry",after.holderHasTurnBeforeChallenge},
                    {"responseStartsAfterCompletedTitle",true}};
                if(after.late)account["challenge"]=describe(*after.late);
                if(after.outcome)
                {
                    const auto &ledger=after.outcome->ledger;QJsonArray balances;
                    for(int k=0;k<after.outcome->purchases.size();++k)
                    {
                        const auto o=after.outcome->purchases[k].toObject(),prior=orders[k].toObject();
                        if(o["treasuryBefore"]==prior["treasuryBefore"] && o["treasuryAfter"]==prior["treasuryAfter"])continue;
                        balances.append(QJsonObject{{"tick",o["buildHalfTurn"]},{"enemy",o["enemy"]},{"factory",o["factory"]},
                            {"before",o["treasuryBefore"]},{"after",o["treasuryAfter"]}});
                    }
                    account["ownership"]=static_cast<qint64>(ledger.ownership);
                    account["capitalLoss"]=static_cast<qint64>(ledger.capitalLoss);
                    account["timing"]=static_cast<qint64>(ledger.timing);account["score"]=static_cast<qint64>(ledger.score);
                    account["changedBalances"]=balances;
                }
                r["ifEarlyTitleHolderStays"]=account;
            }
            if(contest.guardExchange)
            {
                r["earlyFireAgainstGuard"]=describeEncounter(*contest.guardExchange);
                r["earlyFireStatus"]=contest.exchangeStatus;
                if(!contest.competingGuardGunDuties.isEmpty())r["otherGunDuties"]=contest.competingGuardGunDuties;
            }
            if(contest.afterGuardExchange)r["lateChallengeAfterEarlyFire"]=describe(*contest.afterGuardExchange);
            if(!contest.approachShots.isEmpty())r["attacksOnApproach"]=contest.approachShots;
        }
        return r;
    }
#endif

    // Rendering uses only immutable values. The application thread resolves all
    // unit identities and makes every proof and work-allocation decision.
    struct TownReportInputs
    {
        int width{};
        std::vector<QJsonObject> names;
        QJsonArray orders;
        QJsonObject actor(int i) const {return i>=0 && i<int(names.size())?names[i]:QJsonObject{};}
    };

    struct TownReportLineCache
    {
        TownReportFields reportFields;
        struct Saved {TownAssaultLine line;QJsonObject value;};
        std::array<std::unique_ptr<Saved>,64> entries;
        using Summary=std::tuple<bool,bool,int,int,int,MilliFunds>;
        std::array<std::optional<std::pair<Summary,QJsonObject>>,64> summaries;
        struct SavedPath {std::vector<StockCaptureStep> steps;QJsonArray value;};
        std::array<std::unique_ptr<SavedPath>,256> paths;

        QJsonObject summary(const TownAssaultAlternatives &answer)
        {
            const Summary key{answer.workLimited,answer.unresolved,answer.search.capperRoutes,answer.search.fullyPriced,
                              answer.search.dominatedAtArrival,answer.declineValue};
            const auto hash=std::apply([](const auto &...v){return qHashMulti(std::size_t{0},v...);},key);
            auto &last=summaries[hash%summaries.size()];
            if(last && last->first==key)return last->second;
            QJsonObject value{{"workLimited",answer.workLimited},{"unresolved",answer.unresolved},
                {"capperRoutes",answer.search.capperRoutes},{"fullyPricedLines",answer.search.fullyPriced},
                {"unassistedArrivalsBounded",answer.search.dominatedAtArrival},{"declineScore",static_cast<qint64>(answer.declineValue)}};
            last.emplace(key,value);
            return value;
        }

        QJsonArray path(int width,const std::vector<StockCaptureStep> &steps)
        {
            std::size_t hash=0;
            for(const auto &step:steps)
            {
                hash=qHashMulti(hash,step.tick,step.tile.x,step.tile.y,step.cells.size());
                for(int cell:step.cells)hash=qHashMulti(hash,cell);
            }
            auto &last=paths[hash%paths.size()];
            if(last && last->steps==steps)return last->value;
            QJsonArray value;
            for(const auto &step:steps)
            {
                QJsonArray cells;
                for(int cell:step.cells)cells.append(QJsonArray{cell%width,cell/width});
                value.append(QJsonObject{{"tick",step.tick},{"tile",QJsonArray{step.tile.x,step.tile.y}},{"cells",cells}});
            }
            last=std::make_unique<SavedPath>(SavedPath{steps,value});
            return value;
        }
        template<class Identity> QJsonObject get(int width,const TownAssaultLine &line,Identity identity)
        {
            const auto key=qHashMulti(size_t{0},line.town.x,line.town.y,line.side,line.capper,line.blocker,
                                      line.entryTick,line.finishTick,line.moves.size(),line.shots.size());
            auto &last=entries[key%entries.size()];
            if(last && last->line==line)return last->value;
            auto value=renderTownEncounter(width,line,identity);
            last=std::make_unique<Saved>(Saved{line,value});
            return value;
        }
    };

    inline QJsonObject renderRivalReport(const TownRivalReport &row,const TownReportInputs &inputs,TownReportLineCache &lines)
    {
        auto &r=lines.reportFields;
        r.begin(row.opening);
        const auto &contest=*row.contest;
        const auto &orders=inputs.orders;
        const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const auto identity=[&](int i){return inputs.actor(i);};
        const auto describeEncounter=[&](const TownAssaultLine &line){return lines.get(inputs.width,line,identity);};
        if(!contest.retainedWalk.isEmpty())r[TownReportField::singleOriginalBlockerWalk]=contest.retainedWalk;
        r[TownReportField::guardedSuffixResumes]=contest.heldResumes;r[TownReportField::observedSuffixResumes]=contest.observedResumes;
        r[TownReportField::guardedCompleteReads]=row.suffix[0];
        r[TownReportField::observedCompleteReads]=row.suffix[1];
        r[TownReportField::guardedFirstShotsRestored]=row.suffix[2];
        r[TownReportField::observedFirstShotsRestored]=row.suffix[3];
        r[TownReportField::guardedNextTick]=row.suffix[4];r[TownReportField::observedNextTick]=row.suffix[5];
        r[TownReportField::guardedNextLane]=row.suffix[6];r[TownReportField::observedNextLane]=row.suffix[7];
        r[TownReportField::suffixWorkRetained]=static_cast<qint64>(row.credit);
        r[TownReportField::openResponseReused]=contest.reusedOpen;r[TownReportField::earlierCaptureReused]=contest.reusedCapture;r[TownReportField::quietThrough]=contest.verifiedGuardThrough;
        r[TownReportField::reusedGuardExchangeThrough]=contest.reusedFireThrough;
        r[TownReportField::guardStatus]=contest.guardStatus;
        if(!contest.bookedApproachShot.isEmpty())r[TownReportField::bookedApproachShot]=contest.bookedApproachShot;
        if(contest.guardGunsChecked>=0)
        {
            r[TownReportField::guardApproachGunsCleared]=contest.guardGunsChecked;
            r[TownReportField::guardApproachRouteExpansions]=contest.guardRouteExpansions;
            r[TownReportField::approachShotScope]="FIRST_EXECUTABLE_PROFITABLE_OR_BOOKED_SHOT";
        }
        if(contest.guardPendingTick>=0)
        {
            r[TownReportField::guardPendingTick]=contest.guardPendingTick;
            r[TownReportField::guardPendingTile]=point(contest.guardPendingTile);
        }
        r[TownReportField::guardDepartureTick]=contest.departure;r[TownReportField::guardFuelAfter]=contest.remainingFuel;
        auto describe=[&](const TownAssaultAlternatives &answer){
            auto q=lines.summary(answer);
            if(!answer.unpricedPaidArrivals.isEmpty())q["unexaminedPaidEntrants"]=answer.unpricedPaidArrivals;
            if(!answer.ordinaryCounterChecks.isEmpty())q["retainedOrdinaryCounterChecks"]=answer.ordinaryCounterChecks;
            if(answer.assault)
            {
                q["assault"]=describeEncounter(*answer.assault);
                q["unansweredScore"]=static_cast<qint64>(answer.assaultValue);
                q["withResponseScore"]=static_cast<qint64>(answer.responseValue);
                q["choosesAssault"]=answer.selectsAssault;
                if(answer.response)q["response"]=describeEncounter(*answer.response);
            }
            if(!answer.partialArrivalActions.empty())
            {
                QJsonArray partial;
                for(const auto &[k,p]:answer.partialArrivalActions)
                    partial.append(renderTownArrival(identity(k.first),k.second,p,describeEncounter));
                q["conditionalPostEntryResponses"]=partial;
            }
            if(!answer.arrivalsRefuted.empty())
            {
                QJsonArray bounds;
                for(const auto &[intent,proof]:answer.arrivalsRefuted)
                {
                    auto receipt=renderTownArrival(identity(intent.first),intent.second,proof,describeEncounter);
                    if(receipt.contains("actorStatus"))q["unresolved"]=true;
                    bounds.append(receipt);
                }
                q["unassistedArrivalBounds"]=bounds;
            }
            return q;
        };
        if(contest.openRefutes)return r.finish();
        if(contest.paidGuardUnnecessaryForThisAttack || contest.dominatedOnRetainedBoard)
        {
            r[TownReportField::ifRivalDeclinesEarlyOccupation]=describe(contest.open);
            r[TownReportField::attackerUpperBound]=static_cast<qint64>(contest.open.declineValue);
            if(contest.paidGuardUnnecessaryForThisAttack)r[TownReportField::keepsOtherFundedJobsOpen]=true;
        }
        else
        {
            r[TownReportField::ifRivalDeclinesEarlyOccupation]=describe(contest.open);
            if(contest.guarded)r[TownReportField::ifRivalGuardsWithoutCapturing]=describe(*contest.guarded);
            if(contest.earlierCapture)r[TownReportField::ifRivalStartsCapture]=describe(*contest.earlierCapture);
            if(contest.afterEarlyTitle)
            {
                const auto &after=*contest.afterEarlyTitle;
                QJsonObject account{{"status",after.status},{"holderCanActBeforeLateEntry",after.holderHasTurnBeforeChallenge},
                    {"responseStartsAfterCompletedTitle",true}};
                if(after.late)account["challenge"]=describe(*after.late);
                if(after.outcome)
                {
                    const auto &ledger=after.outcome->ledger;QJsonArray balances;
                    for(int k=0;k<after.outcome->purchases.size();++k)
                    {
                        const auto o=after.outcome->purchases[k].toObject(),prior=orders[k].toObject();
                        if(o["treasuryBefore"]==prior["treasuryBefore"] && o["treasuryAfter"]==prior["treasuryAfter"])continue;
                        balances.append(QJsonObject{{"tick",o["buildHalfTurn"]},{"enemy",o["enemy"]},{"factory",o["factory"]},
                            {"before",o["treasuryBefore"]},{"after",o["treasuryAfter"]}});
                    }
                    account["ownership"]=static_cast<qint64>(ledger.ownership);
                    account["capitalLoss"]=static_cast<qint64>(ledger.capitalLoss);
                    account["timing"]=static_cast<qint64>(ledger.timing);account["score"]=static_cast<qint64>(ledger.score);
                    account["changedBalances"]=balances;
                }
                r[TownReportField::ifEarlyTitleHolderStays]=account;
            }
            if(contest.guardExchange)
            {
                r[TownReportField::earlyFireAgainstGuard]=describeEncounter(*contest.guardExchange);
                r[TownReportField::earlyFireStatus]=contest.exchangeStatus;
                if(!contest.competingGuardGunDuties.isEmpty())r[TownReportField::otherGunDuties]=contest.competingGuardGunDuties;
            }
            if(contest.afterGuardExchange)r[TownReportField::lateChallengeAfterEarlyFire]=describe(*contest.afterGuardExchange);
            if(!contest.approachShots.isEmpty())r[TownReportField::attacksOnApproach]=contest.approachShots;

        }
        return r.finish();
    }

    inline QJsonObject townCaptureProof(const CombatTerrain &terrain,const ProductionProjection &production,
        const TownAssaultLine &line,double startingHp,int threshold)
    {
        const auto square=[](TilePoint p){return QJsonArray{p.x,p.y};};
        QJsonArray stops,shots,installments;
        const auto move=std::find_if(line.moves.begin(),line.moves.end(),[&](const auto &m){return m.actor==line.capper;});
        if (move==line.moves.end() || line.finishTick<0 || line.installments.empty()) return {};
        for (const auto &s:move->path.steps)
        {
            QJsonArray cells;
            for (int sq:s.cells) cells.append(square({sq%terrain.width,sq/terrain.width}));
            stops.append(QJsonObject{{"tick",s.tick},{"tile",square(s.tile)},{"cells",cells}});
        }
        const int n=static_cast<int>(terrain.units.size());
        for (const auto &s:line.shots)
        {
            if (s.target!=line.capper) continue;
            QJsonObject shot{{"gun",s.actor<n ? terrain.units[s.actor].engineUnitId : -1},{"tick",s.tick},
                {"from",square(s.from)},{"hpBefore",s.targetBefore},{"hpAfter",s.targetAfter},{"shooterHpAfter",s.actorAfter},
                {"targetCapital",static_cast<qint64>(s.targetLoss)},{"counterCapital",static_cast<qint64>(s.counterLoss)},
                {"newSignedLoss",static_cast<qint64>((line.side?-1:1)*(s.targetLoss-s.counterLoss))},{"alreadyPriced",false}};
            if (s.actor>=n)
            {
                const auto &gun=production.fundedGuns.at(s.actor-n);
                shot["birthSite"]=square(gun.factory);shot["birthTick"]=gun.buildTick;shot["recruit"]=gun.unit->type;
            }
            shots.append(shot);
        }
        int carry=0;
        double hp=startingHp;
        auto hit=shots.begin();
        for (const auto &[tick,progress]:line.installments)
        {
            while (hit!=shots.end() && (*hit).toObject()["tick"].toInt()<tick)
            {
                hp=(*hit).toObject()["hpAfter"].toDouble();
                ++hit;
            }
            installments.append(QJsonObject{{"tick",tick},{"hpBeforeReply",hp},{"carryBefore",carry},{"carryAfter",progress}});
            carry=progress;
        }
        return {{"source","TOWN_ASSAULT"},{"status","WITNESSED"},{"unit",terrain.units[line.capper].engineUnitId},
            {"enemy",bool(line.side)},{"from",square(move->origin)},{"tile",square(line.town)},{"departureTick",move->fromTick},
            {"entryTick",line.entryTick},{"completionTick",line.finishTick},{"actualCompletionTick",line.finishTick},
            {"finalCarry",carry},{"captureThreshold",threshold},{"hpAfter",line.capperHp},{"fuelSpent",move->path.fuel},
            {"stops",stops},{"shots",shots},{"captureInstallments",installments}};
    }

    template<class Timing>
    PricedTownEncounter priceOrdinaryTownEncounters(const CombatTerrain &terrain,std::span<const KnownUnit> units,
        const ProductionProjection &production,const QJsonArray &orders,const StockCaptureSettlement &settled,
        std::span<const ProductionProperty> properties,std::span<const TilePoint> positions,std::span<const double> hp,
        std::span<const int> fuel,std::span<const std::array<int,2>> ammo,std::span<const int> freeDay,std::span<const int> casualty,int guard,TilePoint home,TilePoint away,
        int horizon,int cutoff,int threshold,StockRouteLowerBounds &lower,std::span<const MilliFunds> cash,
        Timing timing,std::int64_t &budget,std::optional<TownQuoteFocus> focus={},std::span<const TownQuoteFocus> required={},
        TownMarketHistory *history=nullptr,std::span<const int> branch={},bool recordOwnerUpper=true,ScreenOpening screen={},const DetachedTownFrame *detached=nullptr)
    {
        PricedTownEncounter result;
        if(focus)result.auditOriginalPurchases=orders;
        if(!settled.townTraffic || horizon<2 || cutoff<3)
        {
            const auto roster=frozenTownOpportunities(terrain,units,properties,home,away,horizon,threshold,budget,focus,required,&lower);
            for(const auto &slot:roster)
            {
                const bool impossible=horizon<2 || cutoff<3;
                const auto state=impossible?TownChallengeStatus::Unavailable:TownChallengeStatus::Unsupported;
                result.challenges.push_back({slot.tile,slot.attacker,state,0,slot.capperUnit,slot.entryTick,slot.required});
                result.coverage.append(QJsonObject{{"tile",QJsonArray{slot.tile.x,slot.tile.y}},{"attackingSide",slot.attacker},
                    {"status",townChallengeName(state)},{"complete",impossible},{"reason",impossible?"AFTER_CUTOFF":"NO_SETTLED_TRAFFIC"},
                    {"signedChange",impossible?QJsonValue(0):QJsonValue(QJsonValue::Null)},{"offeredWork",static_cast<qint64>(slot.allowance)},
                    {"usedWork",0},{"capperUnit",slot.capperUnit},{"requiredEntryTick",slot.entryTick},{"required",slot.required},
                    {"checkedActorIds",QJsonArray{}},{"exhaustedActorIds",QJsonArray{}},{"intentions",QJsonArray{}}});
            }
            return result;
        }
        TownMarketFrame *savedFrame=nullptr;
        std::vector<const TownMarketFrame*> aliasFrames;
        std::shared_ptr<const TownMarketFrame::Discovery> discovery;
        QByteArray originalRecords;
        bool capturedOriginalRecords=false;
        if(detached){
            savedFrame=detached->frame;
            discovery=detached->discovery;
        }else if(history && history->enabled && !focus)
        {
            auto identity=identifyTownMarket(terrain,units,production,orders,settled,properties,positions,hp,fuel,ammo,
                freeDay,casualty,guard,home,away,horizon,cutoff,threshold,cash,history->world,screen);
            if(identity)
            {
                auto due=std::make_shared<TownMarketFrame::Discovery>();bool priority=false;
                for(const auto &r:required)
                {
                    due->insert({r.tile.x,r.tile.y,r.attackingSide,r.capperUnit,r.entryTick,r.friendlyExit.x,r.friendlyExit.y,r.decisionWork});
                    priority|=r.decisionWork>0;
                }
                discovery=std::move(due);
                if(history->ownerHooks && history->ownerHooks->tracking(*history))try{
                    QJsonArray all;
                    for(const auto &record:identity->records)all.append(record);
                    originalRecords=QCborValue::fromJsonValue(all).toCbor();
                    capturedOriginalRecords=true;
                }catch(const std::bad_alloc&){}
                savedFrame=history->find(std::move(*identity),branch,priority,!terrain.orderedOpening,aliasFrames);
            }
        }
        std::optional<TownMarketOwnerView> ownerView;
        if(savedFrame && !detached){
            const auto previous=savedFrame->physicalRevision++;
            if constexpr(std::is_same_v<Timing,decltype(&cashTimingAdjustment)>){
                if(capturedOriginalRecords && history->ownerHooks && history->ownerHooks->tracking(*history) && aliasFrames.empty() && !focus && timing==&cashTimingAdjustment){
                    ownerView.emplace(TownMarketOwnerView{terrain,units,production,orders,settled,properties,positions,hp,
                        fuel,ammo,freeDay,casualty,guard,home,away,horizon,cutoff,threshold,lower,cash,focus,required,
                        branch,recordOwnerUpper,screen,*history,*savedFrame,discovery,std::move(originalRecords),previous,budget});
                    if(auto priced=history->ownerHooks->consume(*ownerView,budget))return std::move(*priced);
                }
            }
        }
        const auto pendingExceptions=std::uncaught_exceptions();
        const auto rememberMarket=qScopeGuard([&]{if(ownerView && std::uncaught_exceptions()==pendingExceptions)
            history->ownerHooks->completed(*ownerView,budget);});
        const auto cell=[&](TilePoint p){return p.y*terrain.width+p.x;};
        const int n=static_cast<int>(positions.size()),last=std::min(2*horizon-1,cutoff);
        const StockCaptureTraffic *finalCalendar=&*settled.townTraffic;
        std::optional<StockCaptureTraffic> paidCalendar;
        std::set<int> paidCaptureParticipants;
        // A late, affordable paid-capper reassignment replaces its former deployment.
        // Other actors must see the paid unit's final route before pricing an assault.
        for (const auto &v:orders)
        {
            const auto o=v.toObject(), proof=o["paidCaptureProof"].toObject();
            if (proof.isEmpty()) continue;
            const auto birth=o["factory"].toArray();
            auto found=std::find_if(production.fundedGuns.begin(),production.fundedGuns.end(),[&](const auto &g) {
                return g.unit && g.unit->type==o["type"].toString() && g.side==int(o["enemy"].toBool()) &&
                    g.buildTick==o["buildHalfTurn"].toInt() && QJsonArray{g.factory.x,g.factory.y}==birth;
            });
            if (found==production.fundedGuns.end())
            {
                result.conflicts.append(QJsonObject{{"status","MISSING_PAID_CAPPER_IN_DATED_CALENDAR"},
                    {"birthSite",birth},{"birthTick",o["buildHalfTurn"]},{"type",o["type"]},{"enemy",o["enemy"]}});
                return result;
            }
            if (!paidCalendar) paidCalendar=*finalCalendar;
            const int actor=n+static_cast<int>(std::distance(production.fundedGuns.begin(),found));
            paidCalendar->removeFutureUses(actor,o["firstStepHalfTurn"].toInt());
            const auto xy=[](const QJsonValue &v){const auto a=v.toArray(); return TilePoint{a.at(0).toInt(),a.at(1).toInt()};};
            for (const auto &st:proof["stops"].toArray())
            {
                const auto step=st.toObject();const int tick=step["tick"].toInt();
                for (const auto &sq:step["cells"].toArray()) paidCalendar->reserve(cell(xy(sq)),tick,tick,found->side,actor,false);
                paidCalendar->reserve(cell(xy(step["tile"])),tick,tick+1,found->side,actor,true);
            }
            paidCalendar->reserve(cell(xy(o["end"])),o["arrivalHalfTurn"].toInt(),last,found->side,actor,true);
            for(const auto &x:proof["shots"].toArray())
            {
                const auto shot=x.toObject();const int id=shot["gun"].toInt(NO_UNIT);
                if(shot.contains("birthSite"))
                {
                    const auto lender=std::find_if(production.fundedGuns.begin(),production.fundedGuns.end(),[&](const auto &g){
                        return g.unit && g.unit->type==shot["recruit"].toString() && g.side!=found->side &&
                            g.side==int(shot["enemy"].toBool()) &&
                            g.buildTick==shot["birthTick"].toInt(-1) && xy(shot["birthSite"])==g.factory;
                    });
                    if(lender==production.fundedGuns.end())
                    {
                        result.conflicts.append(QJsonObject{{"status","MISSING_PAID_DEFENDER_IN_DATED_CALENDAR"},
                            {"birthSite",shot["birthSite"]},{"birthTick",shot["birthTick"]},
                            {"type",shot["recruit"]},{"enemy",!found->side}});
                        return result;
                    }
                    const int paid=n+static_cast<int>(lender-production.fundedGuns.begin()),tick=shot["tick"].toInt();
                    if(shot["shooterHpAfter"].toDouble()<=0)paidCalendar->removeFutureUses(paid,tick);
                    else if(shot["loan"].toBool())
                    {
                        paidCalendar->removeUsesWithin(paid,tick,tick+2);
                        for(const auto &sq:shot["outbound"].toArray())paidCalendar->reserve(cell(xy(sq)),tick,tick,lender->side,paid,false);
                        paidCalendar->reserve(cell(xy(shot["from"])),tick,tick+1,lender->side,paid,true);
                        for(const auto &sq:shot["returnPath"].toArray())paidCalendar->reserve(cell(xy(sq)),tick+2,tick+2,lender->side,paid,false);
                        paidCalendar->reserve(cell(xy(shot["returnStop"])),tick+2,tick+2,lender->side,paid,true);
                    }
                    continue;
                }
                for (int i=0;i<n;++i) if (terrain.units[i].engineUnitId==id)
                {
                    paidCaptureParticipants.insert(i);
                    const auto at=xy(shot["from"]);const int tick=shot["tick"].toInt();
                    const auto origin=shot["movedFrom"].toArray();
                    if (origin.size()==2 && xy(origin)!=INVALID_TILE)
                    {
                        paidCalendar->vacate(i,cell(xy(origin)),tick);
                        for (const auto &sq:shot["movementCells"].toArray())
                            paidCalendar->reserve(cell(xy(sq)),tick,tick,int(terrain.units[i].enemy),i,false);
                        paidCalendar->reserve(cell(at),tick,last,int(terrain.units[i].enemy),i,true);
                    }
                    if (shot["shooterHpAfter"].toDouble()<=0) paidCalendar->vacate(i,cell(at),tick);
                }
            }
            finalCalendar=&*paidCalendar;
        }
        const auto &calendar=*finalCalendar;
        const auto screenThrough=[&](int holder,TilePoint tile) {
            const int sq=cell(tile);
            if (!settled.townProtectedPosts.contains(sq)) return -1;
            const auto found=settled.townPostDependencies.find(sq);
            if (found==settled.townPostDependencies.end()) return last;
            int through=-1;
            for (const auto &dep:found->second)
            {
                if (dep.holder==NO_UNIT || dep.occupiedTick<0) return last;
                if (dep.holder==holder) through=std::max(through,dep.occupiedTick);
            }
            return through;
        };
        const auto gunDutyThrough=[&](int actor) {
            const auto duty=settled.townGunDutyThrough.find(actor);
            return duty==settled.townGunDutyThrough.end() ? -1 : duty->second;
        };
        std::set<int> futureVictims;
        for (const auto &value:orders)
        {
            const auto order=value.toObject();
            if (order["target"].toInt(-1)>=0) futureVictims.insert(order["target"].toInt());
            for (const auto &fire:order["otherPricedShots"].toArray())
                if (fire.toObject()["target"].toInt(-1)>=0) futureVictims.insert(fire.toObject()["target"].toInt());
        }
        const bool ownerAudit=recordOwnerUpper && focus && focus->capperUnit==NO_UNIT;
        if(ownerAudit)result.ownerAuditAllowance=std::max<std::int64_t>(512,8*n+16*cutoff);
        auto auditLeft=result.ownerAuditAllowance;
        const auto beforeJobPositions=positions;const auto beforeJobHp=hp;
        std::vector<TilePoint> releasePosition;
        std::vector<double> releaseHp;
        std::vector<std::array<int,2>> releaseAmmo;
        if (!settled.townAfterCapture.empty() || !settled.townAfterFactoryExit.empty())
        {
            releasePosition.assign(positions.begin(),positions.end());
            releaseHp.assign(hp.begin(),hp.end());
            releaseAmmo.assign(ammo.begin(),ammo.end());
            for(const auto *source:{&settled.townAfterCapture,&settled.townAfterFactoryExit})
            for(const auto &[actor,job]:*source)
            {
                releasePosition.at(actor)=job.tile;
                releaseHp.at(actor)=job.hp;
                releaseAmmo.at(actor)=job.ammo;
            }
            positions=releasePosition;hp=releaseHp;ammo=releaseAmmo;
        }
        std::vector<TownAssaultRight> rights(n),beforeGunDuty;
        if(!settled.townGunClaims.empty())beforeGunDuty.resize(n);
        std::array<int,2> guns{},feet{};
        for (int i=0;i<n;++i)
        {
            const auto &releases=settled.townAfterFactoryExit.contains(i)?settled.townAfterFactoryExit:settled.townAfterCapture;
            const auto job=releases.find(i);
            const bool released=job!=releases.end();
            if (positions[i]==INVALID_TILE || hp[i]<=0 || casualty[i]>=0 || i==guard ||
                futureVictims.contains(terrain.units[i].engineUnitId) || paidCaptureParticipants.contains(i) ||
                (!released && settled.townBusyStock.contains(i))) continue;
            const auto &unit=terrain.units[i];
            const int side=int(unit.enemy);
            const int afterDuty=gunDutyThrough(i)+1;
            const int base=std::max(2*freeDay[i]+side,released ? job->second.readyTick : 0);
            const int afterScreen=screenThrough(i,positions[i])+1;
            if(!beforeGunDuty.empty() && base<=last)
                beforeGunDuty[i]={base,last,released?job->second.fuel:fuel[i],0,std::max(base,afterScreen+int(afterScreen%2!=side))};
            const int ready=std::max(base,afterDuty+int(afterDuty%2!=side));
            if (ready>last) continue;
            rights[i]={ready,last,released ? job->second.fuel : fuel[i],0};
            rights[i].mobileFrom=std::max(ready,afterScreen+int(afterScreen%2!=side));
            if (units[i].canFire) ++guns[side];
            if (unit.capper) ++feet[side];
        }
        std::vector<const StockCaptureRelease*> actorFactoryExit(n,nullptr);
        std::vector<int> actorScreenThrough(n,-1);
        for(int i=0;i<n;++i)
        {
            if(const auto exit=settled.townAfterFactoryExit.find(i);exit!=settled.townAfterFactoryExit.end())
                actorFactoryExit[i]=&exit->second;
            if(positions[i]!=INVALID_TILE)
                actorScreenThrough[i]=screenThrough(i,positions[i]);
        }
        const auto at=[&](int i,int tick) {
            if (i<0 || i>=n || positions[i]==INVALID_TILE) return INVALID_TILE;
            return retainedOriginalPostAt(calendar,terrain.width,i,tick,beforeJobPositions[i],positions[i],
                actorFactoryExit[i]);
        };
        std::vector<int> recruitReserveReady(production.fundedGuns.size(),std::numeric_limits<int>::max());
        for(const auto &v:orders)
        {
            const auto o=v.toObject();
            if(o["kind"].toString()!=QStringLiteral("DEPLOY") ||
               !o["otherPricedShots"].toArray().isEmpty() ||
               !o["paidCaptureDefenseUses"].toArray().isEmpty()) continue;
            const int buildHalf=o["buildHalfTurn"].toInt();
            const auto factoryArr=o["factory"].toArray();
            if(factoryArr.size()!=2) continue;
            const TilePoint factoryTile{factoryArr[0].toInt(),factoryArr[1].toInt()};
            const int reserveReady=o["reserveReadyTick"].toInt(buildHalf+2);
            for(std::size_t k=0;k<production.fundedGuns.size();++k)
            {
                const auto &gun=production.fundedGuns[k];
                if(gun.buildTick==buildHalf && gun.factory==factoryTile)
                    recruitReserveReady[k]=std::min(recruitReserveReady[k],reserveReady);
            }
        }
        std::vector<int> recruitMinFirst(production.fundedGuns.size(),std::numeric_limits<int>::max());
        for(std::size_t k=0;k<production.fundedGuns.size();++k)
        {
            if(settled.townBusyPaid.contains(static_cast<int>(k)) || recruitReserveReady[k]==std::numeric_limits<int>::max()) continue;
            const auto &gun=production.fundedGuns[k];
            if(!gun.unit) continue;
            int minFirst=std::max(recruitReserveReady[k],gun.buildTick+1);
            if(gun.observedService) minFirst=std::max(minFirst,gun.observedService->nextReady);
            const int blockedThrough=std::max(gunDutyThrough(n+static_cast<int>(k)),screenThrough(n+static_cast<int>(k),gun.destination));
            if(blockedThrough<std::numeric_limits<int>::max())
                minFirst=std::max(minFirst,blockedThrough+1);
            else
                minFirst=std::numeric_limits<int>::max();
            recruitMinFirst[k]=minFirst;
        }
        const auto freeRecruit=[&](int k,int first,int end) {
            return k>=0 && k<static_cast<int>(recruitMinFirst.size()) && end<=last && first>=recruitMinFirst[k];
        };
        TownAssaultBoard board{terrain,units,calendar,positions,hp,ammo,rights,production.fundedGuns,
                              at,freeRecruit,threshold,horizon,last};
        board.fundedGuardCache=std::make_shared<PaidTownCalendarCache>();
        board.postEntryProperties=properties;
        board.withdrawalJob=[&](int actor,TilePoint town,int tick,double remainingHp,std::int64_t &work){
            return unpricedTownWithdrawal(terrain,units,properties,settled,production,positions,rights,
                actor,town,tick,remainingHp,last,std::min(cutoff,2*horizon),threshold,calendar,work);
        };
        PaidNeutralOriginCache paidOrigins(calendar,production.fundedGuns,n);
        board.beforeConditionalDuty=beforeGunDuty;
        board.conditionalClaims=&settled.townGunClaims;
        board.canLoseReactionGun=[&](int i,int tick){
            return i>=0 && i<n && !beforeGunDuty.empty() && beforeGunDuty[i].readyTick>=0 &&
                beforeGunDuty[i].readyTick<=tick+1 && tick>actorScreenThrough[i];
        };
        board.canRemove=[&](int i,int tick) {
            if(i>=n && i-n<static_cast<int>(production.fundedGuns.size()))
            {
                const auto &g=production.fundedGuns[i-n];
                if(g.observedService && tick<g.observedService->returned)return false;
            }
            // A defender can be hit on the opposing activation immediately preceding
            // its next free action. Earlier committed actions and screening posts stand.
            return i>=0 && i<n && rights[i].readyTick>=0 && rights[i].readyTick<=tick+1 &&
                   tick>actorScreenThrough[i];
        };
        // Failed provisional stock assignments do not own their target. Keep
        // the original party for evidence; only spare stock actors can bid.
        std::map<int,std::vector<int>> priorClaims;
        for(const auto &v:settled.proofs)
        {
            const auto claim=v.toObject();const auto loc=claim["tile"].toArray();
            if(claim["status"]=="WITNESSED" || loc.size()!=2) continue;
            const int uid=claim["unit"].toInt(NO_UNIT);
            for(int i=0;i<n;++i) if(terrain.units[i].engineUnitId==uid)
            {
                auto &prior=priorClaims[cell({loc[0].toInt(),loc[1].toInt()})];
                if(std::find(prior.begin(),prior.end(),i)==prior.end())prior.push_back(i);
                break;
            }
        }
        const auto startWork=budget;
        const auto point=[](TilePoint t){return QJsonArray{t.x,t.y};};
        auto actorFacts=[&](int uid) {
            QJsonObject fact{{"unit",uid},{"present",false}};
            for(int i=0;i<n;++i)if(terrain.units[i].engineUnitId==uid)
            {
                fact={{"unit",uid},{"present",positions[i]!=INVALID_TILE && hp[i]>0},{"side",int(terrain.units[i].enemy)},
                    {"position",point(positions[i])},{"hp",hp[i]},{"priorFreeTick",2*freeDay[i]+int(terrain.units[i].enemy)},{"readyTick",rights[i].readyTick},{"mobileFrom",rights[i].mobileFrom},
                    {"fuel",rights[i].fuel},{"ammo",QJsonArray{ammo[i][0],ammo[i][1]}},{"conditionalGunThrough",gunDutyThrough(i)},
                    {"casualtyDay",casualty[i]},{"guard",guard==i},{"priorStockDuty",settled.townBusyStock.contains(i)},
                    {"priorPaidFight",paidCaptureParticipants.contains(i)},{"fundedTarget",futureVictims.contains(uid)}};
                if(positions[i]!=INVALID_TILE)fact["screenThrough"]=screenThrough(i,positions[i]);
                if(const auto exit=settled.townAfterFactoryExit.find(i);exit!=settled.townAfterFactoryExit.end())
                {
                    fact["factoryExitFrom"]=point(beforeJobPositions[i]);fact["factoryExitTick"]=exit->second.readyTick-2;
                    fact["factoryExitEnd"]=point(exit->second.tile);fact["factoryExitFuelAfter"]=exit->second.fuel;
                }
                break;
            }
            return fact;
        };
        if(focus && focus->capperUnit>=0)result.requestedCapper=actorFacts(focus->capperUnit);
        // The menu and its base shares come from F0. Unused work is returned
        // to the caller, never borrowed from a later challenge on another team.
        const auto menu=frozenTownOpportunities(terrain,units,properties,home,away,horizon,threshold,
            focus ? budget : budget-budget/16,focus,required,&lower);
        struct Admission {TownChallengeStatus status{TownChallengeStatus::Unsupported};QString reason{"NO_CERTIFIED_CHALLENGE"};};
        std::map<std::pair<int,int>,Admission> admission;
        struct Target {TownAssaultTarget candidate;int earliest;std::vector<int> prior;std::vector<NeutralRaceEntrant> rivals;std::size_t baseIndex{std::numeric_limits<std::size_t>::max()};int rivalId{NO_UNIT};std::optional<NeutralOccupancy> observed;};
        std::vector<Target> targets;
        struct Vacancy {PropertyStockColumn column;int first;};
        std::map<int,Vacancy> vacant;
        std::map<int,const ProductionProperty*> originalTitles;std::map<int,QJsonObject> paidTitles;
        for (const auto &p:properties)
        {
            if (p.tile==home || p.tile==away) continue;
            auto column=p.column;originalTitles[cell(p.tile)]=&p;
            int lastTitle=-1;
            for (const auto &event:p.ownershipChanges)
            {
                if (event.completionTick<lastTitle) continue;
                lastTitle=event.completionTick;
                column.ownerBefore=event.owner;
            }
            const int first=std::max(2,lastTitle+1);
            if (first>last)
            {
                for(int side=0;side<2;++side)admission[{cell(p.tile),side}]={TownChallengeStatus::Unavailable,"AFTER_CUTOFF"};
                continue;
            }
            if(column.ownerBefore!=OwnerSign::Neutral)
                admission[{cell(p.tile),int(column.ownerBefore==OwnerSign::Enemy)}]={TownChallengeStatus::Baseline,"OWNED_ON_SETTLED_CALENDAR"};
            if (p.column.income.oursPerTurn<=0 && p.column.income.enemyPerTurn<=0) continue;
            const auto purchased=std::find_if(orders.begin(),orders.end(),[&](const auto &v) {
                const auto order=v.toObject();return order["kind"].toString()=="CAPTURE" && order["end"].toArray()==point(p.tile);
            });
            if(purchased!=orders.end())
            {
                paidTitles[cell(p.tile)]=(*purchased).toObject();
                const int who=(*purchased).toObject()["enemy"].toBool();
                admission[{cell(p.tile),who}]={TownChallengeStatus::Baseline,"PAID_CAPTURE_ALREADY_SETTLED"};
                admission[{cell(p.tile),1-who}]={TownChallengeStatus::Unsupported,"WOULD_REPRICE_EXISTING_PAID_CAPTURE"};
                continue;
            }
            if (terrain.production && std::any_of(terrain.production->sites.begin(),terrain.production->sites.end(),
                [&](const auto &site){return site.tile==p.tile;})) continue;
            if(column.ownerBefore==OwnerSign::Neutral)
            {
                vacant.emplace(cell(p.tile),Vacancy{column,first});
                continue;
            }
            const int defender=int(column.ownerBefore==OwnerSign::Enemy),attacker=1-defender;
            if (!feet[attacker])
            {admission[{cell(p.tile),attacker}]={TownChallengeStatus::Unavailable,"NO_FREE_STOCK_CAPPER"};continue;}
            // A promised later job for the occupant would also have to be repriced on death.
            // Consider only a defender whose retained post is actually present on this timeline.
            bool hasGarrison=false;
            for (int i=0;i<n;++i)
            {
                if (rights[i].readyTick<0 || int(terrain.units[i].enemy)!=defender || positions[i]!=p.tile) continue;
                hasGarrison=true;
                if (!guns[attacker]) continue;
                int nearest=last+1;
                for (int a=0;a<n;++a)
                {
                    const auto &u=terrain.units[a];
                    if (rights[a].readyTick<0 || int(u.enemy)!=attacker || !u.capper || u.movement<=0) continue;
                    // The empty-grid estimate orders searches only. The assault must replay every step.
                    nearest=std::min(nearest,rights[a].readyTick+2*((manhattanDistance(positions[a],p.tile)-1)/u.movement));
                }
                int firstGun=last+1;
                for (int a=0;a<n;++a)
                {
                    const auto &u=terrain.units[a];const auto &known=units[a];
                    if (rights[a].readyTick<0 || int(u.enemy)!=attacker || !known.canFire) continue;
                    const int gap=std::max(0,manhattanDistance(positions[a],p.tile)-known.maxRange);
                    if (gap>0 && (u.movement<=0 || rights[a].fuel==0)) continue;
                    const int moves=gap>0 ? (gap+u.movement-1)/u.movement : 0;
                    firstGun=std::min(firstGun,rights[a].readyTick+2*std::max(0,moves-int(known.canMoveAndFire)));
                }
                const int canAttack=std::max({first,rights[i].readyTick-1,screenThrough(i,p.tile)+1});
                const int earliest=std::max({canAttack,nearest,firstGun});
                if (canAttack<=last) targets.push_back({{column,defender,i,canAttack,last},earliest});
            }
            // Test the exact window of vacancy, even when the starting board
            // had no garrison. A paid or busy original unit occupying the
            // site still blocks entry and a defender may arrive before us.
            if(!hasGarrison)vacant.emplace(cell(p.tile),Vacancy{column,first});
        }
        using SlotQuote=TownMarketFrame::SlotQuote;
        using DatedProgress=TownMarketFrame::DatedProgress;
        using PrefixWork=TownMarketFrame::PrefixWork;
        const auto &admissionBounds=lower;
        // This token is never shared between different submitted teams, rival
        // policies or dated production wallets. It outlives all this quote's
        // date-local continuations, including equal-looking coordinates.
        TownGuardStops temporaryGuardStops;
        auto &guardStops=savedFrame?savedFrame->guardStops:temporaryGuardStops;
        const auto suffixWorld=savedFrame?savedFrame->world:std::make_shared<const int>(0);
        TownReportLineCache localLines;
        const auto describeEncounter=[&](const TownAssaultLine &line){return localLines.get(terrain.width,line,
            [&](int i){return townActorIdentity(terrain,production.fundedGuns,i);});};
        std::optional<TownReportInputs> reportInputs;
        const auto freezeReportInputs=[&]() -> const TownReportInputs & {
            if(!reportInputs)
            {
                reportInputs.emplace();reportInputs->width=terrain.width;reportInputs->orders=orders;
                for(int i=0;i<int(terrain.units.size()+production.fundedGuns.size());++i)
                    reportInputs->names.push_back(townActorIdentity(terrain,production.fundedGuns,i));
            }
            return *reportInputs;
        };
        std::size_t pendingRivalCount{};
        constexpr std::size_t maxPendingRivalReports=64;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        const bool auditReports=qEnvironmentVariableIsSet("COW_COORDINATED_AUDIT_TOWN_REPORTS");
        const bool eagerReports=qEnvironmentVariableIsSet("COW_COORDINATED_EAGER_TOWN_REPORTS");
#else
        constexpr bool auditReports=false,eagerReports=false;
#endif
        struct ReportAudit {int eager{},deferred{},discarded{},finalized{},named{},saved{},reused{},sized{},limited{},resumed{},crosschecked{},partial{};} reportAudit;
        const auto priceSlot=[&](const TownOpportunity &slot,StockRouteLowerBounds &lower,DatedProgress &continuations,PrefixWork &prefixes) {
            SlotQuote out;
            auto &result=out.price;auto &leaders=out.leaders;
            const int side=slot.attacker;const auto key=std::pair{cell(slot.tile),side};
            auto initial=admission[key];TownChallengeReceipt row{slot.tile,side,initial.status};
            auto reason=initial.reason;
            row.capperUnit=slot.capperUnit;row.entryTick=slot.entryTick;row.required=slot.required;
            bool declaredTitle=false,lateMaterialOnly=slot.materialOnly;
            int remainingNeutralPaydays=-1;
            if(slot.entryTick>=0 && originalTitles.contains(cell(slot.tile)))
            {
                const auto &building=*originalTitles.at(cell(slot.tile));
                const int at=slot.entryTick;
                auto owner=productionOwnerAt(building,at+1);int mostRecent=-1;bool later=false;
                for(const auto &event:building.ownershipChanges)
                {
                    if(event.completionTick<=at)mostRecent=std::max(mostRecent,event.completionTick);
                    else later=true;
                }
                if(building.ownershipChanges.empty() && building.existingCompletion>=0)
                {
                    const int tick=2*building.existingCompletion+int(building.existingEnemy);
                    if(tick<=at)mostRecent=tick;else later=true;
                }
                if(const auto paid=paidTitles.find(cell(slot.tile));paid!=paidTitles.end())
                {
                    const int tick=paid->second["impactHalfTurn"].toInt();
                    if(tick>at)later=true;
                    else if(tick>=mostRecent)owner=paid->second["enemy"].toBool()?OwnerSign::Enemy:OwnerSign::Ours;
                }
                if(owner==(side?OwnerSign::Enemy:OwnerSign::Ours))
                {declaredTitle=true;row.status=TownChallengeStatus::Baseline;reason="ALREADY_OWNS_AT_REQUESTED_TICK";}
                else if(later || paidTitles.contains(cell(slot.tile)))
                {declaredTitle=true;row.status=TownChallengeStatus::Unsupported;reason="REQUIRES_RETIMING_EXISTING_TITLE";}
            }
            if(!declaredTitle && slot.entryTick>=0 && slot.capperUnit>=0 && vacant.contains(cell(slot.tile)) &&
                vacant.at(cell(slot.tile)).column.ownerBefore==OwnerSign::Neutral)
            {
                for(int cap=0;cap<n;++cap) if(terrain.units[cap].engineUnitId==slot.capperUnit)
                {
                    const int maxRate=units[cap].captureRate+std::max(0,10-units[cap].hpRounded);
                    if(maxRate<=0)break;
                    const int carry=positions[cap]==slot.tile?threshold-1:0;
                    const auto earliest=slot.entryTick+2*std::max<std::int64_t>(0,widenedCaptureTurns(carry,maxRate,threshold)-1);
                    remainingNeutralPaydays=0;
                    for(int t=1;t<=cutoff;++t)if(t>earliest && t%2==side)++remainingNeutralPaydays;
                    if(remainingNeutralPaydays==0)
                    {
                        lateMaterialOnly=townMaterialCouldChange(terrain,units);
                        if(!lateMaterialOnly){declaredTitle=true;row.status=TownChallengeStatus::Unavailable;reason="NO_REMAINING_PAYDAY_OR_POSSIBLE_COMBAT";}
                    }
                    break;
                }
            }
            std::vector<Target> neutral;
            QJsonArray modelDates,retainedDirect;
            bool matched=false,limited=false,unresolved=false,blockedRival=false,committedOccupant=false,competingReaction=false;int completed=0;
            QJsonArray occupantEvidence,intentions,focusAttempts,rivalProofs,conditionalReactions,stagingResponses,pendingAttacks;std::set<int> checked;
            auto remaining=slot.allowance;
            QJsonArray casualtyBranches;
            if(slot.capperUnit>=0 && slot.entryTick>=0 && !settled.attemptedCasualties.empty() && originalTitles.contains(cell(slot.tile)))
            {
                const auto &property=originalTitles.at(cell(slot.tile))->column;
                const TownAssaultTarget named{property,1-side,NO_UNIT,slot.entryTick,slot.entryTick,slot.capperUnit};
                auto work=remaining/2;const auto offer=work;
                casualtyBranches=priceNamedStockCasualty(settled,board,named,cutoff,cash,lower,timing,work);
                remaining-=offer-work;
            }
            std::vector<const Target*> compatible;
            if(auto it=vacant.find(cell(slot.tile));!declaredTitle && it!=vacant.end() && it->second.column.ownerBefore!=(side?OwnerSign::Enemy:OwnerSign::Ours))
            {
                const auto prior=priorClaims.find(cell(slot.tile));const std::vector<int> none;
                std::array<int,2> entries{last+1,last+1},occupants=entries;
                const auto offers=neutralStockRebids(board,it->second.column,prior==priorClaims.end()?none:prior->second,admissionBounds,
                    true,slot.capperUnit,&entries,&occupants,slot.entryTick,it->second.column.ownerBefore!=OwnerSign::Neutral,it->second.first,&paidOrigins);
                for(const auto &offer:offers) if(1-offer.target.defendingSide==side)
                {
                    neutral.push_back({offer.target,offer.first,offer.earlierClaimants,offer.potentialRivals});
                    if(ownerAudit && offer.target.property.ownerBefore!=OwnerSign::Neutral)
                        for(int t=offer.target.occupiedFrom;t<=offer.target.occupiedThrough;t+=2)
                        {if(auditLeft<=0)break;--auditLeft;modelDates.append(t);}
                }
                if(neutral.empty())
                {
                    row.status=TownChallengeStatus::Unavailable;
                    reason=slot.entryTick>=0?"NAMED_CAPPER_CANNOT_ENTER_AT_REQUESTED_DATE":"NO_AVAILABLE_CAPPER_BEFORE_CUTOFF";
                }
                for(const auto &candidate:neutral)compatible.push_back(&candidate);
            }
            for(const auto &t:targets)if(!declaredTitle && t.candidate.property.tile==slot.tile && 1-t.candidate.defendingSide==side)compatible.push_back(&t);
            // Admission below reads the same immutable terrain bounds on every
            // retry. Even two different rival prefixes with the same dates
            // keep separate records by their place in this fixed menu.
            const auto dateKey=[&](const Target &item,std::size_t index){
                const int begin=slot.entryTick>=0?slot.entryTick:item.candidate.occupiedFrom;
                const int end=slot.entryTick>=0?slot.entryTick:item.candidate.occupiedThrough;
                return std::tuple{item.baseIndex==std::numeric_limits<std::size_t>::max()?index:item.baseIndex,item.rivalId,item.candidate.occupant,begin,end,slot.capperUnit,bool(item.observed && item.observed->conditionalDuty)};
            };
            ++prefixes.pass;
            struct SharedGroup {int dates{},active{};std::int64_t offered{},remaining{};TownObservedResponses late;};
            std::map<std::vector<int>,SharedGroup> groups;
            QJsonArray groupedReceipts;
            struct OpenGroup {int dates{},active{};std::int64_t offered{},remaining{};};
            std::map<std::size_t,OpenGroup> openGroups;
            std::vector<Target> options;
            QJsonArray unexaminedRivals;
            std::set<std::size_t> incompleteCensus,refutedWithoutGuard;
            const auto contested=std::count_if(compatible.begin(),compatible.end(),[](const auto *t){return !t->rivals.empty();});
            if(contested)
            {
                auto geo=remaining/4;const auto geoOffered=geo;
                int surveyBefore=-1;
                for(const auto *t:compatible)if(!t->rivals.empty())surveyBefore=std::max(surveyBefore,t->earliest);
                for(std::size_t i=0;i<compatible.size();++i)
                {
                    const auto &t=*compatible[i];
                    if(t.rivals.empty()){options.push_back(t);options.back().baseIndex=i;continue;}
                    auto &proposals=prefixes.candidates[i];bool missing=false,observed=false;
                    for(const auto &r:t.rivals)
                    {
                        const auto identity=std::pair{r.actor,r.conditionalDuty};
                        auto known=proposals.find(identity);
                        if(known==proposals.end())
                        {
                            // Every date asks about the same physical board. Spend the
                            // geometry pool on an earliest-arrival proof once per rival.
                            auto arrival=earlierNeutralOccupant(board,slot.tile,t.earliest,{r},lower,geo,&prefixes.routes,surveyBefore);
                            if(!arrival){missing=true;unexaminedRivals.append(QJsonArray{t.earliest,r.actor});continue;}
                            known=proposals.emplace(identity,std::move(*arrival)).first;out.advanced=true;
                        }
                        if(known->second.actor==NO_UNIT)continue;
                        auto choice=t;choice.baseIndex=i;choice.rivalId=r.actor;choice.rivals={r};choice.observed=known->second;
                        options.push_back(std::move(choice));observed=true;
                    }
                    if(missing){incompleteCensus.insert(i);matched=true;}
                    if(!observed && !missing){options.push_back(t);options.back().baseIndex=i;options.back().rivals.clear();}
                }
                remaining-=geoOffered-geo;
                compatible.clear();for(const auto &option:options)compatible.push_back(&option);
                std::int64_t live=0;
                for(std::size_t i=0;i<compatible.size();++i)
                {
                    const auto &t=*compatible[i];auto &d=continuations[dateKey(t,i)];
                    if(t.observed)d.arrival=t.observed;
                    if(!d.complete())++live;
                    if(!t.observed || t.observed->originUnknown)continue;
                    auto &prefix=groups[rivalPrefixKey(terrain,*t.observed)];auto &open=openGroups[t.baseIndex];
                    ++prefix.dates;++open.dates;
                    if(!d.complete()){++prefix.active;++open.active;}
                }
                // Each defender policy has a distinct held-board response, but
                // the open board at a date and a defensive prefix shared by
                // several dates are each researched just once.
                const auto available=remaining;
                if(live>0)
                {
                    for(auto &[key,g]:groups)if(g.dates>1)
                    {g.offered=(available/3)*g.active/live;g.remaining=g.offered;remaining-=g.offered;}
                    const bool sharedEarly=std::any_of(groups.begin(),groups.end(),[](const auto &entry){
                        return entry.second.dates>1 && entry.second.active>0;});
                    for(auto &[date,g]:openGroups)if(g.dates>1)
                    {
                        // A single required date with several earlier rivals
                        // has two work pools: the common open board and their
                        // individual replies. There is no separate shared
                        // early prefix to reserve a third pool for.
                        const int pools=slot.capperUnit>=0 && slot.entryTick>=0 && !sharedEarly?2:3;
                        g.offered=(available/pools)*g.active/live;g.remaining=g.offered;remaining-=g.offered;
                    }
                }
            }
            std::vector<int> cachedSuffix(compatible.size()+1);
            for(int i=static_cast<int>(compatible.size())-1;i>=0;--i)
            {
                const auto &item=*compatible[i];const auto found=continuations.find(dateKey(item,i));
                cachedSuffix[i]=cachedSuffix[i+1]+int(found!=continuations.end() && found->second.complete());
            }
            for(std::size_t i=0;i<compatible.size();++i)
            {
                const auto &target=*compatible[i];auto candidate=target.candidate;
                candidate.requiredCapper=slot.capperUnit;
                if(slot.entryTick>=0)
                {
                    if(slot.entryTick%2!=side || slot.entryTick<candidate.occupiedFrom || slot.entryTick>candidate.occupiedThrough)
                    {row.status=TownChallengeStatus::Unavailable;reason="REQUESTED_DATE_OUTSIDE_OWNERSHIP_OR_ACTIVATION";continue;}
                    candidate.occupiedFrom=candidate.occupiedThrough=slot.entryTick;
                }
                const auto &property=candidate.property;matched=true;
                const auto openingHolder=conditionalOpeningHolder(board,screen,candidate,settled);
                if(!openingHolder.isEmpty())occupantEvidence.append(openingHolder);
                bool pricedStandingOccupant=false;int friendlyHolder=NO_UNIT;
                if(candidate.occupant==NO_UNIT)
                {
                    int first=candidate.occupiedFrom;first+=int(first%2!=side);
                    bool vacancy=false,hasCandidate=false;
                    for(int cap=0;cap<n && !vacancy;++cap)
                    {
                        if(int(terrain.units[cap].enemy)!=side || !units[cap].canCapture || hp[cap]<=0 || positions[cap]==INVALID_TILE ||
                           rights[cap].readyTick<0 || (candidate.requiredCapper!=NO_UNIT && candidate.requiredCapper!=terrain.units[cap].engineUnitId))continue;
                        for(int tick=first;tick<=std::min(last,candidate.occupiedThrough);tick+=2)
                        {
                            if(rights[cap].readyTick>tick || rights[cap].lastFreeTick<tick)continue;
                            hasCandidate=true;
                            if(calendar.open(cap,side,cell(slot.tile),tick,true) && calendar.open(cap,side,cell(slot.tile),tick+1,true))
                            {vacancy=true;break;}
                        }
                    }
                    if(hasCandidate && !vacancy)
                    {
                        // A vehicle can garrison a neutral property without
                        // transferring its title. Price the actual attack that
                        // would clear it, with exactly the native neutral income.
                        const int sq=cell(slot.tile);
                        int defender=NO_UNIT;bool pricedDates=true,oneFriendly=true;QJsonArray uses;
                        for(int t=first;t<=std::min(last,candidate.occupiedThrough);t+=2)
                        {
                            if(remaining<=0){limited=true;pricedDates=false;oneFriendly=false;break;}
                            --remaining;
                            int occupant=NO_UNIT;bool ambiguous=false;
                            for(const auto &use:calendar.uses(sq))
                            {
                                if(!use.stopped || use.first>t || use.last<t)continue;
                                if(use.actor<0 || (occupant!=NO_UNIT && occupant!=use.actor)){ambiguous=true;continue;}
                                occupant=use.actor;
                            }
                            QJsonObject info{{"tick",t},{"unique",!ambiguous && occupant>=0}};
                            if(occupant>=0 && occupant<n)
                            {
                                info["unit"]=terrain.units[occupant].engineUnitId;
                                info["side"]=int(terrain.units[occupant].enemy);
                                info["canRelease"]=board.canRemove && board.canRemove(occupant,t);
                                info["sourcePosition"]=point(positions[occupant]);info["hp"]=hp[occupant];
                            }
                            else if(occupant>=n && occupant-n<static_cast<int>(production.fundedGuns.size()))
                            {
                                const auto &g=production.fundedGuns[occupant-n];
                                info["birth"]=point(g.factory);info["birthTick"]=g.buildTick;
                                info["type"]=g.unit->type;info["side"]=g.side;
                            }
                            uses.append(info);
                            if(ambiguous || occupant<0 || occupant>=n || int(terrain.units[occupant].enemy)!=side ||
                               positions[occupant]!=slot.tile || board.at(occupant,t)!=slot.tile ||
                               (defender!=NO_UNIT && defender!=occupant))oneFriendly=false;
                            if(ambiguous || occupant<0 || occupant>=n ||
                               int(terrain.units[occupant].enemy)==side || positions[occupant]!=slot.tile ||
                               board.at(occupant,t)!=slot.tile || !board.canRemove || !board.canRemove(occupant,t) ||
                               (defender!=NO_UNIT && defender!=occupant))pricedDates=false;
                            if(defender==NO_UNIT)defender=occupant;
                        }
                        if(!uses.isEmpty())occupantEvidence.append(QJsonObject{{"from",first},
                            {"through",candidate.occupiedThrough},{"stops",uses},{"clearingSupported",pricedDates && defender>=0}});
                        if(oneFriendly && defender>=0)friendlyHolder=defender;
                        else
                        {
                            if(!pricedDates || defender<0){committedOccupant=true;unresolved=true;continue;}
                            candidate.occupant=defender;pricedStandingOccupant=true;
                        }
                    }
                }
                auto &day=continuations[dateKey(target,i)];auto &cursor=day.attack;
                if(friendlyHolder!=NO_UNIT && !day.release)
                {
                    const auto key=std::tuple{target.baseIndex,friendlyHolder,candidate.occupiedFrom,candidate.occupiedThrough,slot.capperUnit};
                    auto &release=prefixes.friendlyReleases[key];
                    if(!release)release=std::make_shared<TownReleaseSearch>();
                    day.release=release;
                }
                const bool cached=day.complete();
                const auto active=static_cast<std::int64_t>(compatible.size()-i)-cachedSuffix[i];
                auto share=cached?std::min<std::int64_t>(remaining,1):
                    std::max<std::int64_t>(0,remaining-cachedSuffix[i])/std::max<std::int64_t>(1,active);
                const auto initialShare=share;
                if(cached)
                {
                    if(share<=0){limited=true;++out.restartFloor;continue;}
                    --share;
                }
                const int resumedTick=cursor.tick,resumedLane=cursor.firstLane,resumedSecond=cursor.secondLane;
                const int priorEmptySteps=cursor.emptySteps;
                std::map<std::pair<int,int>,std::optional<QJsonArray>> wallets;
                const auto purchases=[&](const TownAssaultLine &line)->const std::optional<QJsonArray>& {
                    auto [it,fresh]=wallets.try_emplace(std::pair{line.side,line.finishTick});
                    if(fresh)it->second=townAssaultPurchases(orders,line,property);
                    return it->second;
                };
                const auto value=[&](const TownAssaultLine &line)->std::optional<MilliFunds> {
                    if(!purchases(line))return {};
                    auto ledger=townAssaultLedger(line,property,cash,cutoff,timing);
                    return ledger?std::optional<MilliFunds>{ledger->score}:std::nullopt;
                };
                if(friendlyHolder==NO_UNIT && !pricedStandingOccupant && !target.rivals.empty())
                {
                    const bool hadArrival=day.arrival.has_value();
                    if(!hadArrival)day.arrival=earlierNeutralOccupant(board,slot.tile,target.earliest,target.rivals,lower,share,&prefixes.routes);
                    else if(!cached)
                    {
                        if(share<=0){limited=true;out.restartFloor+=initialShare;continue;}
                        --share;
                    }
                    const auto &rival=day.arrival;
                    out.advanced|=!hadArrival && rival.has_value();
                    if(!rival || rival->actor!=NO_UNIT)
                    {
                        const auto unfinishedContest=share;
                        std::int64_t suffixCredit=0;
                        limited|=!rival;
                        if(rival && rival->originUnknown)
                        {
                            const auto &g=production.fundedGuns[rival->actor-n];
                            unresolved=true;
                            rivalProofs.append(QJsonObject{{"status","PAID_ORIGIN_UNRESOLVED"},{"birth",point(g.factory)},
                                {"buildTick",g.buildTick},{"requestedEntry",target.earliest}});
                        }
                        else if(rival)
                        {
                            const auto path=localLines.path(terrain.width,rival->path.steps);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                            if(auditReports)
                            {
                                QJsonArray expectedPath;
                                for(const auto &step:rival->path.steps)
                                {
                                    QJsonArray cells;
                                    for(int sq:step.cells)cells.append(point({sq%terrain.width,sq/terrain.width}));
                                    expectedPath.append(QJsonObject{{"tick",step.tick},{"tile",point(step.tile)},{"cells",cells}});
                                }
                                if(QJsonDocument(path).toJson(QJsonDocument::Compact)!=QJsonDocument(expectedPath).toJson(QJsonDocument::Compact))
                                    qFatal("Guard path differs from sealed renderer");
                            }
#endif
                            QJsonObject r{{"entryTick",rival->entry},{"stops",path},{"cachedDate",cached},
                                {"requestedEntry",candidate.occupiedFrom}};
                            if(rival->actor<n)r["unit"]=terrain.units[rival->actor].engineUnitId;
                            else {const auto &g=production.fundedGuns[rival->actor-n];r["birth"]=point(g.factory);r["buildTick"]=g.buildTick;}
                            if(rival->conditionalDuty){
                                bool exhausted=false;
                                auto response=day.conditionalReprice?*day.conditionalReprice:
                                    repriceConditionalRival(board,settled,candidate,*rival,screen,cash,cutoff,timing,lower,share,exhausted);
                                if(!exhausted)day.conditionalReprice=response;
                                r["conditionalRivalCommitment"]=response;
                                r["guardStatus"]="EARLY_POST_CHANGES_ANOTHER_CONDITIONAL_RESPONSE";
                                r["scope"]="ALTERNATIVE_RIVAL_PREFIX_AND_NAMED_DUTY";
                                rivalProofs.append(r);blockedRival=true;unresolved=true;limited|=exhausted;
                                remaining-=initialShare-share;continue;
                            }
                            std::shared_ptr<const TownRivalGuardComparison> incomplete;
                            if(!day.finalContest)
                            {
                                auto key=rivalPrefixKey(terrain,*rival);
                                auto &prefix=prefixes.known[key];
                                auto group=groups.find(key);
                                auto *common=group!=groups.end() && group->second.offered>0?&group->second.remaining:nullptr;
                                auto localOpen=openGroups.find(target.baseIndex);
                                auto *openPool=localOpen!=openGroups.end() && localOpen->second.offered>0?&localOpen->second.remaining:nullptr;
                                auto &open=prefixes.open[target.baseIndex];
                                const auto retake=[&](const TownAssaultAlternatives &early,std::int64_t &work){
                                    return challengeQuietEarlyTitle(board,early,candidate,lower,orders,cash,cutoff,timing,work);
                                };
                                screen.orders=&orders;screen.diagnostic=focus.has_value();
                                const TownWalkOpening walkOpening{beforeJobPositions,freeDay,casualty,screen};
                                auto quote=compareTownRivalGuard(board,candidate,*rival,lower,value,share,&prefix,prefixes.pass,common,&open,
                                    openPool,retake,suffixWorld,&day.suffixes,false,&walkOpening,&guardStops);
                                suffixCredit=quote.suffixCredit;out.advanced|=quote.suffixAdvanced || quote.guardProbeAdvanced;
                                if(quote.workLimited)incomplete=std::make_shared<TownRivalGuardComparison>(std::move(quote));
                                else day.finalContest=std::make_shared<TownRivalGuardComparison>(std::move(quote));
                            }
                            const auto &contest=day.finalContest?*day.finalContest:*incomplete;
                            if(contest.openRefutes || contest.dominatedOnRetainedBoard)
                                refutedWithoutGuard.insert(target.baseIndex);
                            if(auto found=groups.find(rivalPrefixKey(terrain,*rival));found!=groups.end())
                                found->second.late.observe(side,candidate.occupiedFrom,contest);
                            if(contest.openRefutes)checked.insert(contest.excludedUnits.begin(),contest.excludedUnits.end());
                            else if(contest.paidGuardUnnecessaryForThisAttack)
                                result.retainBound({slot.tile.x,slot.tile.y,side,slot.capperUnit,candidate.occupiedFrom});
                            else if(contest.dominatedOnRetainedBoard)
                            {
                                for(int cap:contest.open.checkedCappers)checked.insert(terrain.units[cap].engineUnitId);
                                const std::array<int,5> key{slot.tile.x,slot.tile.y,side,slot.capperUnit,candidate.occupiedFrom};
                                if(contest.guardStatus=="ON_TOWN_REPLY_COVERS_UNASSISTED_ARRIVALS")
                                    for(const auto &[id,proof]:contest.open.arrivalsRefuted)result.retainBound(key,&proof);
                                else result.retainBound(key);
                            }
                            else{blockedRival=true;unresolved=true;limited|=contest.workLimited;}
                            QJsonObject expected;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                            if(auditReports)expected=verifySealedRival(terrain,production,orders,day,contest,r,suffixCredit);
#endif
                            TownRivalReport report{rivalProofs.size(),std::move(r),
                                {day.suffixes.held.completeReads,day.suffixes.afterFire.completeReads,
                                 day.suffixes.held.retainedFirstShots,day.suffixes.afterFire.retainedFirstShots,
                                 day.suffixes.held.search.tick,day.suffixes.afterFire.search.tick,
                                 day.suffixes.held.search.firstLane,day.suffixes.afterFire.search.firstLane},
                                suffixCredit,day.finalContest?day.finalContest:incomplete,std::move(expected)};
                            const auto &inputs=freezeReportInputs();
                            const bool named=slot.capperUnit>=0 && slot.entryTick>=0;
                            if(named || eagerReports || pendingRivalCount>=maxPendingRivalReports)
                            {
                                auto record=renderRivalReport(report,inputs,localLines);
                                if(auditReports && QJsonDocument(record).toJson(QJsonDocument::Compact)!=
                                   QJsonDocument(report.auditExpected).toJson(QJsonDocument::Compact))qFatal("Named/eager town report differs from sealed renderer");
                                rivalProofs.append(record);
                                if(auditReports){++reportAudit.eager;reportAudit.named+=named;}
                            }
                            else
                            {
                                if(auditReports)
                                {
                                    ++reportAudit.deferred;reportAudit.limited+=contest.workLimited;
                                    reportAudit.partial+=!contest.open.partialArrivalActions.empty() ||
                                        (contest.guarded && !contest.guarded->partialArrivalActions.empty()) ||
                                        (contest.earlierCapture && !contest.earlierCapture->partialArrivalActions.empty());
                                }
                                ++pendingRivalCount;out.pendingReports.push_back(std::move(report));
                                rivalProofs.append(QJsonObject{});
                            }
                        }
                        out.advanced|=!cached && day.complete();
                        out.restartFloor+=day.complete()?1:rival?std::max<std::int64_t>(1,1+unfinishedContest-suffixCredit):initialShare;
                        remaining-=initialShare-share;continue;
                    }
                }
                const auto beforeAssault=share;
                if(!cached && resumedTick>=0)
                {
                    ++day.resumedSearches;
                    if(resumedSecond>=0 && cursor.afterFirst)++day.retainedFirstShots;
                }
                const int priorRelease=day.release?day.release->attempts:0;
                auto trial=friendlyHolder==NO_UNIT?priceTownAssault(board,candidate,lower,value,share,&cursor,true,true):
                    priceTeammateTownEntries(board,candidate,friendlyHolder,home,away,lower,value,share,*day.release,
                        focus?focus->friendlyExit:INVALID_TILE);
                if(friendlyHolder!=NO_UNIT)
                {
                    // Moving aside may reopen an earlier rival option. Keep its
                    // price open until the rival's dated entry has also been settled.
                    if(!target.rivals.empty()){blockedRival=true;unresolved=true;}
                    out.advanced|=!cached && (day.release->complete || day.release->attempts>priorRelease);
                    if(!day.release->offers.isEmpty())occupantEvidence.append(QJsonObject{{"friendlyReleaseOffers",day.release->offers}});
                }
                if(friendlyHolder==NO_UNIT && !modelDates.isEmpty() && !trial.workLimited && !trial.unresolved)
                    for(int t=candidate.occupiedFrom;t<=candidate.occupiedThrough;t+=2)
                    {
                        if(auditLeft<=0)break;--auditLeft;
                        retainedDirect.append(QJsonObject{{"entry",t},{"completeOnRetainedRoster",true},
                            {"stockReplyScore",static_cast<qint64>(trial.selectsAssault?trial.responseValue:trial.declineValue)}});
                    }
                out.advanced|=!cached && (cursor.complete || cursor.emptySteps!=priorEmptySteps || std::tuple{resumedTick,resumedLane,resumedSecond}!=
                    std::tuple{cursor.tick,cursor.firstLane,cursor.secondLane});
                out.restartFloor+=(cached?0:initialShare-beforeAssault)+(cursor.complete?1:cursor.unfinishedAllowance);
                ++result.bids;remaining-=initialShare-share;
                limited|=trial.workLimited;unresolved|=trial.unresolved;
                competingReaction|=trial.competingReaction;
                for(const auto &fight:trial.conditionalReactions)conditionalReactions.append(fight);
                for(const auto &fight:trial.stagingResponses)stagingResponses.append(fight);
                const auto &search=trial.search;completed+=search.fullyPriced;
                for(int cap:trial.checkedCappers)checked.insert(terrain.units[cap].engineUnitId);
                for(const auto &[key,opt]:trial.intentions)
                {
                    QJsonArray path;for(const auto &step:opt.move.path.steps)path.append(QJsonArray{step.tick,step.tile.x,step.tile.y});
                    intentions.append(QJsonObject{{"capper",terrain.units[key.first].engineUnitId},{"entryTick",key.second},{"finishTick",opt.finish},
                        {"attackingSide",side},{"tile",point(slot.tile)},{"kind",candidate.occupant==NO_UNIT?"ENTER_EMPTY_TOWN":"CLEAR_OCCUPIED_TOWN"},
                        {"stops",path},{"score",static_cast<qint64>(opt.score)}});
                }
                if(slot.capperUnit>=0)for(const auto &attempt:trial.focusedAttempts)focusAttempts.append(attempt);
                QJsonArray partialActions;
                for(const auto &[k,p]:trial.partialArrivalActions)
                {
                    auto item=townArrivalRecord(terrain,production,k.first,k.second,p);
                    if(!p.assumedArrival && !trial.arrivalsRefuted.contains(k) && pendingAttacks.size()<8)
                        pendingAttacks.append(QJsonObject{{"unit",item["unit"]},{"entry",k.second},{"scope",item["scope"]},
                            {"nextCapperAction",p.unexaminedActionTick}});
                    partialActions.append(item);
                }
                QJsonArray assumed;
                for(const auto &[id,proof]:trial.arrivalsRefuted)if(proof.assumedArrival)
                    assumed.append(QJsonObject{{"unit",terrain.units[id.first].engineUnitId},{"entryTick",id.second},
                        {"attackerCeiling",static_cast<qint64>(proof.value)},{"onTownDefenders",proof.defenders},
                        {"withdrawals",proof.withdrawals}});
                result.searches.append(QJsonObject{{"tile",point(slot.tile)},{"attackingSide",side},{"guarded",candidate.occupant!=NO_UNIT},
                    {"required",slot.required},{"capperUnit",slot.capperUnit},{"requiredEntryTick",slot.entryTick},
                    {"fromTick",candidate.occupiedFrom},{"throughTick",candidate.occupiedThrough},{"cachedDate",cached},
                    {"resumeTick",resumedTick},{"resumeFirstLane",resumedLane},{"resumeSecondLane",resumedSecond},
                    {"stockMoveCacheHits",lower.datedHits},{"emptyTownSteps",cursor.emptySteps},{"previousEmptyTownSteps",priorEmptySteps},
                    {"assumedStockArrivalsRefuted",assumed},{"conditionalPostEntryResponses",partialActions},
                    {"unexaminedPaidEntrants",trial.unpricedPaidArrivals},
                    {"nextTick",cursor.tick},{"nextFirstLane",cursor.firstLane},{"nextSecondLane",cursor.secondLane},
                    {"resumedSearches",day.resumedSearches},{"retainedFirstShots",day.retainedFirstShots},
                    {"unfinishedAllowance",static_cast<qint64>(cursor.unfinishedAllowance)},
                    {"ticks",search.ticks},{"eligibleDates",search.eligibleDates},{"lanes",search.lanes},{"guardedLanes",search.guardedLanes},
                    {"firstHits",search.firstHits},{"clears",search.clears},{"capperRoutes",search.capperRoutes},{"fullyPriced",search.fullyPriced},
                    {"offeredWork",static_cast<qint64>(initialShare)},{"usedWork",static_cast<qint64>(initialShare-share)},{"workLimited",trial.workLimited}});
                // A later competing initiative could remove this very body
                // and reopen the bid. Retain the board dependency for joint pricing.
                // An unassisted route was ruled out by the defender's retained
                // firing post. A joined initiative on either side may reopen it.
                for(const auto &[key,proof]:trial.arrivalsRefuted)
                    result.retainBound({slot.tile.x,slot.tile.y,side,terrain.units[key.first].engineUnitId,key.second},&proof);
                if(pricedStandingOccupant && !trial.selectsAssault && !trial.workLimited && !trial.unresolved)
                    result.retainBound({slot.tile.x,slot.tile.y,side,slot.capperUnit,candidate.occupiedFrom});
                if(!trial.assault)continue;
                QJsonObject witness{{"town",point(slot.tile)},{"attackingSide",side},{"assault",describeEncounter(*trial.assault)},
                    {"priorOwner",int(property.ownerBefore)},{"occupiedFrom",candidate.occupiedFrom},{"declineScore",static_cast<qint64>(trial.declineValue)},
                    {"assaultScore",static_cast<qint64>(trial.assaultValue)},{"responseScore",static_cast<qint64>(trial.responseValue)},
                    {"attackChosen",trial.selectsAssault},{"responseChosen",trial.selectsResponse},{"appliedToSchedule",false},
                    {"workLimited",trial.workLimited},{"unresolved",trial.unresolved}};
                if(property.ownerBefore==OwnerSign::Neutral)
                {
                    QJsonArray prior;for(int actor:target.prior)prior.append(terrain.units[actor].engineUnitId);
                    witness["priorUnfulfilledStockBidders"]=prior;witness["lastUncontestedEntryTick"]=candidate.occupiedThrough;
                }
                if(trial.response)witness["response"]=describeEncounter(*trial.response);
                if(trial.assault)
                {
                    const auto bound=trial.arrivalsRefuted.find({trial.assault->capper,trial.assault->entryTick});
                    if(bound!=trial.arrivalsRefuted.end())
                    {
                        witness["continueCaptureAfterReply"]=static_cast<qint64>(trial.responseValue);
                        witness["upperBoundIncludingWithdrawal"]=static_cast<qint64>(bound->second.value);
                        witness["withdrawalChoices"]=bound->second.withdrawals;
                    }
                }
                result.evidence.append(witness);
                if(!trial.selectsAssault)continue;
                const auto selectedKey=std::pair{trial.assault->capper,trial.assault->entryTick};
                const auto known=trial.partialArrivalActions.find(selectedKey);
                if(known!=trial.partialArrivalActions.end() && !known->second.assumedArrival &&
                   !trial.arrivalsRefuted.contains(selectedKey))continue;
                // A later projected death is not a dated order vacating a live
                // primary capper at F1. Neither a replacement capper nor a
                // rival landing earns this title without clearing that body.
                if(!openingHolder.isEmpty()){unresolved=true;committedOccupant=true;continue;}
                if(friendlyHolder!=NO_UNIT && (!day.release->complete || trial.unresolved || trial.workLimited || !target.rivals.empty()))
                    continue;
                const auto admit=[&](const TownAssaultLine &line){
                    auto ledger=townAssaultLedger(line,property,cash,cutoff,timing);const auto &pay=purchases(line);
                    if(!ledger || !pay){unresolved=true;return;}
                    if(TownAssaultDetail::better(ledger->score,row.signedChange,side))row.signedChange=ledger->score;
                    auto &options=leaders[side];
                    if(!options.empty() && TownAssaultDetail::better(options.front().ledger.score,ledger->score,side))return;
                    if(!options.empty() && TownAssaultDetail::better(ledger->score,options.front().ledger.score,side))options.clear();
                    if(std::any_of(options.begin(),options.end(),[&](const auto &x){return x.property.tile==property.tile &&
                        x.chosen.line==line;}))return;
                    auto proof=line.finishTick<0?QJsonObject{}:townCaptureProof(terrain,production,line,hp[line.capper],threshold);
                    options.push_back({{line,property.ownerBefore,std::move(proof),ledger->ownership},property,*ledger,*pay,static_cast<int>(result.evidence.size()-1)});
                };
                admit(trial.selectsResponse && trial.response?*trial.response:*trial.assault);
                if(friendlyHolder!=NO_UNIT)for(const auto &line:day.release->equalChoices)
                {
                    result.evidence.append(QJsonObject{{"town",point(slot.tile)},{"attackingSide",side},
                        {"jointReleaseLine",describeEncounter(line)},{"appliedToSchedule",false}});
                    admit(line);
                }
            }
            for(auto index:incompleteCensus)if(!refutedWithoutGuard.contains(index))limited=true;
            if(matched)
            {
                row.status=limited ? TownChallengeStatus::WorkLimited : unresolved ? TownChallengeStatus::Unsupported : TownChallengeStatus::Priced;
                reason=limited?"DATED_SEARCH_LIMIT":committedOccupant?"REQUIRES_CLEARING_OR_RELEASING_COMMITTED_OCCUPANT":competingReaction?"REQUIRES_REPRICING_COMPETING_GUN_DUTY":blockedRival?"EARLIER_RIVAL_OCCUPATION":unresolved?"UNPRICED_RESPONSE":completed?"ATTACK_COUNTER_AND_DECLINE_COMPARED":"NO_ADMITTED_EXECUTABLE_LINE";
                if(blockedRival && !limited && !unresolved && !completed)row.status=TownChallengeStatus::Unsupported;
            }
            if(lateMaterialOnly && row.status!=TownChallengeStatus::Baseline && townChallengeComplete(row.status) && !completed && row.signedChange==0)
            {row.status=TownChallengeStatus::Unsupported;reason="NO_PAYDAY_MATERIAL_STILL_UNPRICED";}
            for(const auto &[key,group]:groups)if(group.dates>1)
            {
                remaining+=group.remaining;
                groupedReceipts.append(QJsonObject{{"unit",key[0]<n?terrain.units[key[0]].engineUnitId:NO_UNIT},{"actor",key[0]},
                    {"entryTick",key[1]},{"routeFuel",key[2]},{"lateDates",group.dates},{"offered",static_cast<qint64>(group.offered)},
                    {"origin",QJsonArray{key[3],key[4]}},{"departure",key[5]},{"initialFuel",key[6]},
                    {"used",static_cast<qint64>(group.offered-group.remaining)},{"afterObservation",group.late.record(terrain,group.dates,production.fundedGuns)}});
            }
            for(const auto &[date,g]:openGroups)remaining+=g.remaining;
            result.challenges.push_back(row);
            auto detail=QJsonObject{{"tile",point(slot.tile)},{"attackingSide",side},{"firstPossible",slot.firstPossible},
                {"required",slot.required},{"capperUnit",slot.capperUnit},{"requiredEntryTick",slot.entryTick},{"noPaydayButMaterialPossible",lateMaterialOnly},
                {"offeredWork",static_cast<qint64>(slot.allowance)},{"usedWork",static_cast<qint64>(slot.allowance-remaining)},
                {"status",townChallengeName(row.status)},{"complete",townChallengeComplete(row.status)},{"reason",reason},
                {"fullyPricedLines",completed},{"signedChange",townChallengeComplete(row.status)?QJsonValue(static_cast<qint64>(row.signedChange)):QJsonValue(QJsonValue::Null)}};
            if(remainingNeutralPaydays>=0)detail["maxRemainingNeutralPaydays"]=remainingNeutralPaydays;
            if(!groupedReceipts.isEmpty())detail["sharedRivalPrefixes"]=groupedReceipts;
            if(!modelDates.isEmpty())
            {detail["ownedTownStockDates"]=modelDates;if(!retainedDirect.isEmpty())detail["retainedDirectStockDates"]=retainedDirect;}
            if(slot.capperUnit>=0 && slot.entryTick>=0)
                for(const auto &[date,g]:openGroups)if(g.dates>1)
                    detail["sharedOpenBoardWork"]=QJsonObject{{"rivals",g.dates},{"offered",static_cast<qint64>(g.offered)},
                        {"used",static_cast<qint64>(g.offered-g.remaining)},{"workLimited",prefixes.open[date].quote &&
                            prefixes.open[date].quote->workLimited}};
            if(!unexaminedRivals.isEmpty())detail["unexaminedRivalDatesAndActors"]=unexaminedRivals;
            if(slot.capperUnit>=0)
            {
                const auto facts=actorFacts(slot.capperUnit);detail["requestedActor"]=facts;
                if(row.status==TownChallengeStatus::Unavailable && reason=="NAMED_CAPPER_CANNOT_ENTER_AT_REQUESTED_DATE" &&
                   namedEntryDependsOnScreen(facts,slot.tile,side,slot.entryTick))
                {
                    auto release=reviewNamedScreenEntry(terrain,settled,board.traffic,facts,slot.tile,slot.entryTick,lower,remaining);
                    const auto available=vacant.find(cell(slot.tile));
                    const auto actor=std::find_if(terrain.units.begin(),terrain.units.end(),
                        [&](const auto &pawn){return pawn.engineUnitId==slot.capperUnit;});
                    const bool canRetry=release["oldScreensPreserved"].toBool() && available!=vacant.end() && actor!=terrain.units.end();
                    if(canRetry && remaining<n)
                    {
                        release["workLimited"]=true;
                        release["minimumWorkForReleasedBoard"]=n;
                        release["status"]="RELEASED_BOARD_WORK_LIMIT";
                    }
                    if(canRetry &&
                       remaining>=n)
                    {
                        remaining-=n;
                        const int index=static_cast<int>(actor-terrain.units.begin());
                        auto freeToMove=rights;freeToMove[index].mobileFrom=freeToMove[index].readyTick;
                        auto released=board;released.rights=freeToMove;released.fundedGuardCache.reset();
                        const TownAssaultTarget intended{available->second.column,1-side,NO_UNIT,slot.entryTick,
                            slot.entryTick,slot.capperUnit};
                        const auto localValue=[&](const TownAssaultLine &line)->std::optional<MilliFunds>{
                            if(!townAssaultPurchases(orders,line,intended.property))return {};
                            const auto ledger=townAssaultLedger(line,intended.property,cash,cutoff,timing);
                            return ledger?std::optional<MilliFunds>{ledger->score}:std::nullopt;
                        };
                        // Reprice this specific release on its real calendar. A
                        // competing capper or a move that preempts a funded
                        // defender still needs the joint team comparison.
                        const auto priced=priceTownAssault(released,intended,lower,localValue,remaining);
                        QJsonObject continuation{{"workLimited",priced.workLimited},{"unresolved",priced.unresolved},
                            {"fullyPricedLines",priced.search.fullyPriced},{"capperRoutes",priced.search.capperRoutes}};
                        if(priced.assault)continuation["attack"]=describeEncounter(*priced.assault);
                        if(priced.response)continuation["defense"]=describeEncounter(*priced.response);
                        release["namedCaptureAfterRelease"]=continuation;
                        if(priced.workLimited)release["workLimited"]=true;
                    }
                    detail["releasedScreen"]=release;
                    row.status=release["workLimited"].toBool()?TownChallengeStatus::WorkLimited:TownChallengeStatus::Unsupported;
                    result.challenges.back().status=row.status;
                    detail["status"]=townChallengeName(row.status);detail["complete"]=false;
                    detail["signedChange"]=QJsonValue(QJsonValue::Null);
                    detail["reason"]="NAMED_ENTRY_REQUIRES_REPRICING_SCREEN_RELEASE";
                }
                // The contract fixes the actual exit that bought this unit. An earlier different exit might keep the same purchase while improving the walk.
                if(facts.contains("factoryExitTick") && reason=="NAMED_CAPPER_CANNOT_ENTER_AT_REQUESTED_DATE" && row.status==TownChallengeStatus::Unavailable)
                {
                    row.status=TownChallengeStatus::Unsupported;result.challenges.back().status=row.status;
                    detail["status"]=townChallengeName(row.status);detail["complete"]=false;detail["signedChange"]=QJsonValue(QJsonValue::Null);
                    detail["reason"]="RETAINED_FACTORY_EXIT_TOO_LATE_OTHER_EXIT_UNPRICED";
                }
                const bool actorNotChecked=row.status==TownChallengeStatus::Priced && !checked.contains(slot.capperUnit);
                const bool orderedElsewhere=facts["present"].toBool() && (facts["guard"].toBool() || facts["priorStockDuty"].toBool() ||
                    facts["priorPaidFight"].toBool() || facts["fundedTarget"].toBool() ||
                    (slot.entryTick>=0 && facts["conditionalGunThrough"].toInt(-1)>=slot.entryTick));
                if(actorNotChecked || (row.status==TownChallengeStatus::Unavailable && orderedElsewhere &&
                    (facts["readyTick"].toInt(-1)<0 || (slot.entryTick>=0 && facts["readyTick"].toInt()>slot.entryTick))))
                {
                    const bool absent=!facts["present"].toBool() || facts["side"].toInt(-1)!=side;
                    row.status=absent?TownChallengeStatus::Unavailable:TownChallengeStatus::Unsupported;result.challenges.back().status=row.status;
                    detail["status"]=townChallengeName(row.status);detail["complete"]=absent;
                    detail["signedChange"]=absent?QJsonValue(0):QJsonValue(QJsonValue::Null);
                    detail["reason"]=absent?"NAMED_ACTOR_ABSENT_OR_OPPOSING":orderedElsewhere?"REQUIRES_REPRICING_EXISTING_COMMITMENT":"NAMED_ACTOR_NOT_EXHAUSTED";
                }
            }
            QJsonArray actorIds;for(int uid:checked)actorIds.append(uid);
            detail["checkedActorIds"]=actorIds;detail["exhaustedActorIds"]=townChallengeComplete(row.status)?actorIds:QJsonArray{};
            if(!casualtyBranches.isEmpty())detail["conditionalStockCasualties"]=casualtyBranches;
            detail["intentions"]=intentions;if(!rivalProofs.isEmpty())detail["rivalOccupationProofs"]=rivalProofs;
            if(!occupantEvidence.isEmpty())detail["datedOccupants"]=occupantEvidence;
            if(!conditionalReactions.isEmpty())detail["conditionalReactions"]=conditionalReactions;
            if(!stagingResponses.isEmpty())detail["stagingResponses"]=stagingResponses;
            if(!focusAttempts.isEmpty())detail["requestedAttempts"]=focusAttempts;
            if(!pendingAttacks.isEmpty())detail["unpricedPostEntryActions"]=pendingAttacks;
            if(row.signedChange)detail["witnessedChange"]=static_cast<qint64>(row.signedChange);
            if(slot.capperUnit>=0 && slot.entryTick>=0 && (!result.evidence.isEmpty() || !rivalProofs.isEmpty()))
            {
                // Spend only residual work after the dated alternatives have been priced.
                auto left=std::min<std::int64_t>(remaining,4096);const auto start=left;
                if(auditReports){++reportAudit.crosschecked;if(!out.pendingReports.empty())qFatal("Unmaterialized rival report reached a crosscheck");}
                auto joint=collectStockTownInteractions(settled.proofs,result.evidence,rivalProofs,left);
                remaining-=start-left;
                if(!joint.isEmpty())detail["stockTownInteractions"]=joint;
            }
            result.work=slot.allowance-remaining;budget-=result.work;
            detail["usedWork"]=static_cast<qint64>(result.work);
            result.coverage.append(detail);
            return out;
        };
        std::int64_t slotPool=0;
        for(const auto &slot:menu)slotPool+=slot.allowance;
        std::vector<SlotQuote> quotes(menu.size());
        std::vector<DatedProgress> progress(menu.size());
        std::vector<PrefixWork> prefixProgress(menu.size());
        std::vector<std::unique_ptr<StockRouteLowerBounds>> privateGeometry(menu.size());
        std::vector<QJsonArray> attempts(menu.size());
        std::vector<TownChallengeStatus> statuses(menu.size());
        std::vector<std::int64_t> previous(menu.size()),charged(menu.size()),offered(menu.size());
        std::vector<bool> firstVisit(menu.size(),true);
        std::vector<TownMarketFrame::Saved*> retained(menu.size());
        std::vector<const SlotQuote*> aliasQuotes(menu.size());
        if(savedFrame || !aliasFrames.empty())for(std::size_t i=0;i<menu.size();++i)
        {
            const auto &s=menu[i];
            const TownMarketFrame::Key key{s.tile.x,s.tile.y,s.attacker,s.capperUnit,s.entryTick,s.firstPossible,
                int(s.required),int(s.materialOnly)};
            if(savedFrame)retained[i]=&savedFrame->opportunities[key];
            if(retained[i] && retained[i]->finished)continue;
            for(const auto *alias:aliasFrames)
            {
                const auto it=alias->opportunities.find(key);
                if(it==alias->opportunities.end() || !it->second.finished || !it->second.finishedDiscovery ||
                   *it->second.finishedDiscovery!=*discovery)continue;
                aliasQuotes[i]=&*it->second.finished;break;
            }
        }
        const auto run=[&](std::size_t i,std::int64_t allowance) {
            auto slot=menu[i];slot.allowance=allowance;
            auto *saved=retained[i];
            if((saved || aliasQuotes[i]) && firstVisit[i] && allowance>0)
            {
                firstVisit[i]=false;
                if(aliasQuotes[i] || saved->finished)
                {
                    quotes[i]=aliasQuotes[i]?*aliasQuotes[i]:*saved->finished;
                    if(aliasQuotes[i])++history->aliasCompletedReads;
                    const auto state=quotes[i].price.challenges.front().status;
                    statuses[i]=state;
                    ++charged[i];++offered[i];--slotPool;--budget;
                    ++history->completedReads;
                    if(auditReports){++reportAudit.reused;if(!quotes[i].pendingReports.empty())qFatal("Incomplete report read from town cache");}
                    attempts[i].append(QJsonObject{{"status",townChallengeName(state)},{"used",1},
                        {"offered",static_cast<qint64>(allowance)},{"reusedExactCompletedSlot",true},
                        {"reusedAcrossPhysicalReplyLabels",bool(aliasQuotes[i])}});
                    return;
                }
                if(!saved->dates.empty() || !saved->prefixes.candidates.empty())
                {
                    progress[i]=std::move(saved->dates);
                    prefixProgress[i]=std::move(saved->prefixes);
                    savedFrame->retainedBytes-=saved->retainedBytes;
                    saved->retainedBytes=0;
                    ++history->resumedReads;
                    if(auditReports)++reportAudit.resumed;
                }
            }
            // Retain only terrain work already paid by this very slot. An
            // earlier team, policy or sibling slot cannot warm its search.
            if(!privateGeometry[i])
            {
                privateGeometry[i]=std::make_unique<StockRouteLowerBounds>(&lower);
                privateGeometry[i]->datedStorageLeft=4*1024*1024/menu.size();
            }
            if(auditReports)for(const auto &old:quotes[i].pendingReports)
            {
                if(QJsonDocument(renderRivalReport(old,*reportInputs,localLines)).toJson(QJsonDocument::Compact)!=QJsonDocument(old.auditExpected).toJson(QJsonDocument::Compact))qFatal("Overwritten town report changed");
                ++reportAudit.discarded;
            }
            pendingRivalCount-=quotes[i].pendingReports.size();
            quotes[i]=priceSlot(slot,*privateGeometry[i],progress[i],prefixProgress[i]);
            const auto &price=quotes[i].price;
            const auto status=price.challenges.front().status;
            statuses[i]=status;previous[i]=quotes[i].advanced?std::min(allowance,quotes[i].restartFloor):allowance;
            if(status!=TownChallengeStatus::WorkLimited){privateGeometry[i].reset();progress[i].clear();prefixProgress[i]=PrefixWork{};}
            charged[i]+=price.work;offered[i]+=allowance;slotPool-=price.work;result.bids+=price.bids;
            attempts[i].append(QJsonObject{{"offered",static_cast<qint64>(allowance)},{"used",static_cast<qint64>(price.work)},
                {"status",townChallengeName(status)},{"restartFloor",static_cast<qint64>(previous[i])}});
        };
        std::map<std::size_t,QJsonObject> admissions;
        for(std::size_t i=0;i<menu.size();++i)if(menu[i].decisionWork>0)
        {
            const auto &slot=menu[i];std::int64_t used=0;
            const auto earmarked=slot.capperUnit==NO_UNIT?slot.decisionWork/3:0;
            run(i,slot.allowance-earmarked);
            if(slot.capperUnit==NO_UNIT && slot.decisionWork>=3)
            {
                const auto it=originalTitles.find(cell(slot.tile));
                const auto offer=slot.decisionWork/3;
                if(it!=originalTitles.end() && it->second->ownershipChanges.empty() && it->second->existingCompletion<0 &&
                   !paidTitles.contains(cell(slot.tile)))
                {
                    auto *saved=retained[i];
                    if(saved && saved->decisionAdmissionCovered==quotes[i].price.retainedBoardBounds && !saved->decisionAdmission.isEmpty() &&
                       (!saved->decisionAdmission["workLimited"].toBool() || offer<=saved->decisionAdmissionOffer))
                    {
                        used=1;admissions[i]=saved->decisionAdmission;admissions[i]["reused"]=true;
                    }
                    else
                    {
                        auto left=offer;StockRouteLowerBounds trialBounds(&lower);
                        admissions[i]=hearTownProperty(board,slot,it->second->column,quotes[i].price.retainedBoardBounds,trialBounds,cash,cutoff,timing,left);
                        used=offer-left;
                        if(saved){saved->decisionAdmission=admissions[i];saved->decisionAdmissionOffer=offer;saved->decisionAdmissionCovered=quotes[i].price.retainedBoardBounds;}
                    }
                    admissions[i]["offer"]=static_cast<qint64>(offer);admissions[i]["chargedThisVisit"]=static_cast<qint64>(used);
                }
            }
            slotPool-=used;budget-=used;offered[i]+=earmarked;charged[i]+=used;
        }
        for(std::size_t i=0;i<menu.size();++i)if(!menu[i].decisionWork)run(i,menu[i].allowance);
        while(slotPool>0)
        {
            const auto shares=townRetryAllowances(statuses,previous,slotPool);
            if(std::none_of(shares.begin(),shares.end(),[](auto work){return work>0;}))break;
            for(std::size_t i=0;i<shares.size();++i)if(shares[i]>0)run(i,shares[i]);
        }
        std::vector<std::size_t> reportBytes(quotes.size()),jobs;
        std::vector<unsigned char> needsSize(quotes.size());
        for(std::size_t i=0;i<quotes.size();++i)
        {
            const bool resumable=statuses[i]==TownChallengeStatus::WorkLimited &&
                (!progress[i].empty() || !prefixProgress[i].candidates.empty());
            needsSize[i]=retained[i] && !aliasQuotes[i] && !firstVisit[i] && !retained[i]->finished &&
                (townChallengeComplete(statuses[i]) || resumable);
            if(needsSize[i] || !quotes[i].pendingReports.empty())jobs.push_back(i);
        }
        const auto materialize=[&](std::size_t i,TownReportLineCache &lines){
            auto &q=quotes[i];
            TownDeferredCoverageSize coverageBytes;
            if(!q.pendingReports.empty())
            {
                auto detail=q.price.coverage.first().toObject();
                auto rivals=detail["rivalOccupationProofs"].toArray();
                // Each deferred entry currently occupies the two compact bytes
                // of an empty object. All other coverage, including eager rival
                // entries and string escaping, is measured by the Qt writer.
                if(needsSize[i])
                    coverageBytes.begin(q.price.coverage);
                for(const auto &item:q.pendingReports)
                {
                    auto record=renderRivalReport(item,*reportInputs,lines);
                    if(auditReports && QJsonDocument(record).toJson(QJsonDocument::Compact)!=QJsonDocument(item.auditExpected).toJson(QJsonDocument::Compact))qFatal("Surviving town report changed");
                    if(needsSize[i])coverageBytes.replace(rivals.at(item.index),lines.reportFields);
                    rivals[item.index]=std::move(record);
                }
                detail["rivalOccupationProofs"]=rivals;q.price.coverage[0]=detail;
                q.pendingReports.clear();
            }
            if(needsSize[i])
            {
                reportBytes[i]=2*static_cast<std::size_t>(coverageBytes.finish(q.price.coverage)+QJsonDocument(q.price.evidence).toJson(QJsonDocument::Compact).size()+
                    QJsonDocument(q.price.searches).toJson(QJsonDocument::Compact).size());
            }
        };
        if(auditReports)for(const auto &q:quotes)reportAudit.finalized+=static_cast<int>(q.pendingReports.size());
        // Worker and caller own distinct outputs. All game callbacks, mutable
        // caches, scheduling credits and chronological cache admission stay above
        // or below this block on the caller.
        constexpr std::size_t grain=4;
        if(!detached && jobs.size()>=std::max<std::size_t>(8,2*grain) && QThread::idealThreadCount()>1)
        {
            std::atomic<std::size_t> next{0};
            const auto take=[&](TownReportLineCache &lines){
                for(;;)
                {
                    const auto start=next.fetch_add(grain,std::memory_order_relaxed);
                    if(start>=jobs.size())break;
                    for(auto k=start;k<std::min(jobs.size(),start+grain);++k)materialize(jobs[k],lines);
                }
            };
            std::exception_ptr workerFailure;
            std::jthread peer;
            try
            {
                peer=std::jthread([&]{
                    try{TownReportLineCache local;take(local);}
                    catch(...){workerFailure=std::current_exception();}
                });
            }
            catch(const std::system_error &){/* The caller completes this batch. */}
            take(localLines);
            if(peer.joinable())peer.join();
            if(workerFailure)std::rethrow_exception(workerFailure);
        }
        else for(auto i:jobs)materialize(i,localLines);
        std::array<std::vector<PricedTownInitiative>,2> leaders;
        for(std::size_t i=0;i<quotes.size();++i)
        {
            const bool resumable=statuses[i]==TownChallengeStatus::WorkLimited &&
                (!progress[i].empty() || !prefixProgress[i].candidates.empty());
            if(retained[i] && !aliasQuotes[i] && !firstVisit[i] && !retained[i]->finished &&
               (townChallengeComplete(statuses[i]) || resumable))
            {
                const auto &quote=quotes[i].price;
                const auto held=progress[i].size();
                // Each suspended date can own an independent copy of occupancy and
                // actor resources. Bound retention per immutable reply and squad.
                // A slot which does not fit is recomputed on the next visit.
                const std::size_t calendarCells=static_cast<std::size_t>(terrain.width)*terrain.height;
                const std::size_t stored=1024+reportBytes[i]+held*(1024+calendarCells*32+units.size()*64);
                if(auditReports){++reportAudit.sized;
                    const auto baseline=2*static_cast<std::size_t>(QJsonDocument(quote.coverage).toJson(QJsonDocument::Compact).size()+
                        QJsonDocument(quote.evidence).toJson(QJsonDocument::Compact).size()+QJsonDocument(quote.searches).toJson(QJsonDocument::Compact).size());
                    if(baseline!=reportBytes[i] || !quotes[i].pendingReports.empty())qFatal("Retained town report identity changed");}

                constexpr std::size_t frameBudget=4*1024*1024;
                if(stored<=frameBudget-savedFrame->retainedBytes)
                {
                    if(townChallengeComplete(statuses[i])){retained[i]->finished=quotes[i];retained[i]->finishedDiscovery=discovery;if(auditReports)++reportAudit.saved;}
                    else if(statuses[i]==TownChallengeStatus::WorkLimited)
                    {
                        retained[i]->dates=std::move(progress[i]);
                        retained[i]->prefixes=std::move(prefixProgress[i]);
                    }
                    if(retained[i]->finished || !retained[i]->dates.empty() || !retained[i]->prefixes.candidates.empty())
                    {
                        retained[i]->retainedBytes=stored;
                        savedFrame->retainedBytes+=stored;
                    }
                }
            }
            auto &q=quotes[i];
            if(auditReports && !q.pendingReports.empty())qFatal("Unfinished report reached town publication");
            auto detail=q.price.coverage.first().toObject();
            if(menu[i].requestedDecisionWork)
            {
                detail["decisionPriorityRequested"]=static_cast<qint64>(menu[i].requestedDecisionWork);
                detail["decisionPriorityOffer"]=static_cast<qint64>(menu[i].decisionWork);
                detail["decisionPriorityHearing"]=!menu[i].decisionWork?"INSUFFICIENT_ROOM_FOR_REMAINING_MENU":
                    attempts[i].first().toObject()["reusedExactCompletedSlot"].toBool()?"REUSED_EXACT_ANSWER":"SEARCH_ATTEMPTED_ON_THIS_TEAM";
            }
            if(menu[i].capperUnit==NO_UNIT && originalTitles.contains(cell(menu[i].tile)))
            {
                // The comparison admits only a stable single-property transfer.
                // Reclaiming a previously earned property needs its own ledger.
                const auto &property=*originalTitles.at(cell(menu[i].tile));
                if(property.ownershipChanges.empty() && property.existingCompletion<0 && !paidTitles.contains(cell(menu[i].tile)))
                    detail["decisionTitleSchedule"]=QJsonObject{{"owner",static_cast<int>(property.column.ownerBefore)},
                        {"ours",static_cast<qint64>(property.column.income.oursPerTurn)},{"enemy",static_cast<qint64>(property.column.income.enemyPerTurn)},
                        {"halfTurnCutoff",cutoff},{"rounds",static_cast<int>(cash.size())}};
                if(admissions.contains(i))detail["decisionAdmission"]=admissions.at(i);
            }
            detail["baseOfferedWork"]=static_cast<qint64>(menu[i].allowance);
            detail["offeredWork"]=static_cast<qint64>(offered[i]);detail["usedWork"]=static_cast<qint64>(charged[i]);
            if(attempts[i].size()>1)detail["workAttempts"]=attempts[i];
            result.coverage.append(detail);result.challenges.push_back(q.price.challenges.front());
            result.retainedBoardBounds.insert(q.price.retainedBoardBounds.begin(),q.price.retainedBoardBounds.end());
            result.boundsWithoutWitness.insert(q.price.boundsWithoutWitness.begin(),q.price.boundsWithoutWitness.end());
            for(auto &[key,proofs]:q.price.landingBounds)
            {
                auto &dst=result.landingBounds[key];
                dst.insert(dst.end(),std::make_move_iterator(proofs.begin()),std::make_move_iterator(proofs.end()));
            }
            const int offset=result.evidence.size();
            for(const auto &v:q.price.evidence)result.evidence.append(v);
            for(const auto &v:q.price.searches)result.searches.append(v);
            for(int side=0;side<2;++side)for(auto &bid:q.leaders[side])
            {
                auto &options=leaders[side];
                if(!options.empty() && TownAssaultDetail::better(options.front().ledger.score,bid.ledger.score,side))continue;
                if(!options.empty() && TownAssaultDetail::better(bid.ledger.score,options.front().ledger.score,side))options.clear();
                if(std::any_of(options.begin(),options.end(),[&](const auto &x){
                    return x.property.tile==bid.property.tile && x.chosen.line==bid.chosen.line;
                }))continue;
                bid.evidence+=offset;options.push_back(std::move(bid));
            }
        }
        settleTownInitiatives(result,board,leaders,cutoff,timing,budget);
        recheckRetainedTownBounds(result,[&](const auto &,const auto &proofs){
            if(!result.ledger || proofs.empty())return false;
            return std::all_of(proofs.begin(),proofs.end(),[&](const auto &p){
                return landingCounterStillHolds(board,p,result.selected,result.ledger->flow,cutoff,timing,budget);
            });
        });
        // Later stock captures, loans and deaths can alter the calendar that
        // originally ruled out a route. Withdraw the price on the first broken
        // or unverified dependency, even when this town comparison found a line.
        std::optional<StockCaptureTraffic> changedPosts;
        if(!settled.entryObligations.empty() && !result.selected.empty())
        {
            std::set<int> custodians;
            for(const auto &proof:settled.entryObligations)
                for(const auto &post:proof.posts)custodians.insert(post.actor);
            auto edited=[&]() -> StockCaptureTraffic& {
                if(!changedPosts)changedPosts.emplace(calendar);
                return *changedPosts;
            };
            for(const auto &chosen:result.selected)
            {
                for(const auto &move:chosen.line.moves)
                    if(custodians.contains(move.actor))TownAssaultDetail::reserve(terrain,edited(),move,last);
                for(const auto &shot:chosen.line.shots)
                {
                    if(shot.targetAfter<=0 && custodians.contains(shot.target))edited().removeFutureUses(shot.target,shot.tick);
                    if(shot.actorAfter<=0 && custodians.contains(shot.actor))edited().removeFutureUses(shot.actor,shot.tick);
                }
            }
        }
        const auto &reviewCalendar=changedPosts?*changedPosts:calendar;
        for(const auto &obligation:settled.entryObligations)
        {
            // This generation belongs to the same settlement or a private copy
            // derived from it. An unchanged calendar retains its original proof.
            if(reviewCalendar.generation()==obligation.generation)continue;
            const auto check=recheckStockEntry(reviewCalendar,obligation,budget);
            if(check.status==StockEntryReview::Status::Retained)continue;
            const int g=check.actor,unit=terrain.units[obligation.claimant].engineUnitId;
            QJsonObject conflict{{"status",check.status==StockEntryReview::Status::WorkLimited?
                "ENTRY_SCREEN_RECHECK_LIMIT":"ENTRY_SCREEN_NO_LONGER_HOLDS"},
                {"claimant",unit},{"target",point(obligation.target)},{"tick",check.tick},
                {"post",point({check.cell%terrain.width,check.cell/terrain.width})}};
            if(g<n)conflict["guard"]=terrain.units[g].engineUnitId;
            else
            {
                const auto &purchase=production.fundedGuns[g-n];
                conflict["birthSite"]=point(purchase.factory);conflict["birthTick"]=purchase.buildTick;
                conflict["recruit"]=purchase.unit->type;conflict["enemy"]=bool(purchase.side);
            }
            result.conflicts.append(conflict);
            break;
        }
        // Examine unilateral limits after all physically selected jobs have
        // been priced. A changed portfolio needs its own quote. The evidence
        // does not complete a row or contribute an invented economic score.
        result.work=startWork-budget;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if(auditReports)
        {
            QFile file(qEnvironmentVariable("COW_COORDINATED_AUDIT_TOWN_REPORTS"));
            if(!file.open(QIODevice::Append|QIODevice::Text))qFatal("Town report audit file unavailable");
            file.write(QJsonDocument(QJsonObject{{"eager",reportAudit.eager},{"named",reportAudit.named},{"deferred",reportAudit.deferred},
                {"discarded",reportAudit.discarded},{"finalized",reportAudit.finalized},{"saved",reportAudit.saved},
                {"reused",reportAudit.reused},{"sized",reportAudit.sized},{"limited",reportAudit.limited},
                {"resumed",reportAudit.resumed},{"crosschecked",reportAudit.crosschecked},{"partial",reportAudit.partial}}).toJson(QJsonDocument::Compact));
            file.write("\n");
        }
#endif
        if(!ownerAudit)return result;
        for(int i=0;i<result.coverage.size() && auditLeft>0;++i)
        {
            auto r=result.coverage[i].toObject();
            if(r["capperUnit"].toInt(-1)>=0)continue;
            const auto xy=r["tile"].toArray();const TilePoint town{xy[0].toInt(),xy[1].toInt()};
            const int sq=cell(town),side=r["attackingSide"].toInt();
            if(!originalTitles.contains(sq) || paidTitles.contains(sq) || !vacant.contains(sq))continue;
            const auto *p=originalTitles.at(sq);
            if(p->column.ownerBefore!=(side?OwnerSign::Ours:OwnerSign::Enemy))continue;
            const auto offered=auditLeft;
            auto certificate=retainedOwnerTownUpper(board,*p,side,beforeJobPositions,beforeJobHp,lower,cash,cutoff,timing,auditLeft);
            if(certificate.details.isEmpty())continue;
            auto dates=r["retainedDirectStockDates"].toArray();std::set<int> seen;
            for(const auto &v:r["rivalOccupationProofs"].toArray())
            {
                if(auditLeft<=0)break;--auditLeft;
                const auto choice=v.toObject();const int tick=choice["requestedEntry"].toInt(-1);
                const auto open=choice["ifRivalDeclinesEarlyOccupation"].toObject();
                if(tick<0 || open.isEmpty() || !seen.insert(tick).second)continue;
                const bool counted=!open["workLimited"].toBool() && !open["unresolved"].toBool();
                const auto score=open["choosesAssault"].toBool()?open["withResponseScore"]:open["declineScore"];
                dates.append(QJsonObject{{"entry",tick},{"completeOnRetainedRoster",counted},
                    {"stockReplyScore",counted?score:QJsonValue(QJsonValue::Null)}});
            }
            certificate.details["openDatesOnAssignedRoster"]=dates;
            certificate.details["generatedStockDates"]=r["ownedTownStockDates"].toArray();
            certificate.details["generatedStockMenuSignedUpper"]=auditLeft>0?
                retainedRosterDateUpper(side,r["ownedTownStockDates"].toArray(),dates):QJsonValue(QJsonValue::Null);
            certificate.details["otherSelectedInitiativesToRecheck"]=static_cast<int>(result.selected.size());
            certificate.details["usedWork"]=static_cast<qint64>(offered-auditLeft);
            r["retainedOwnerUpper"]=certificate.details;result.coverage[i]=r;
        }
        result.ownerAuditWork=result.ownerAuditAllowance-auditLeft;
        return result;
    }
}
