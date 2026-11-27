#pragma once

#include "ai/coordinator/captureapproachsurvival.h"
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

namespace Coordinator
{
    struct TownAssaultLine;
    struct TownWithdrawalJob
    {
        QString reason;
        TilePoint tile{INVALID_TILE};
        int actor{NO_UNIT},tick{-1};
    };
    struct TownAssaultBoard;
    struct TownArmedMenu
    {
        bool attempted{},enumerated{},allArrivalResources{};
        int tick{-1},remainingFuel{-1};double hp{};
        std::array<int,2> remainingAmmo{};std::optional<MilliFunds> best,withdraw;
        QJsonArray actions,open,jobs;
    };
    inline TownArmedMenu priceTownArmedPostEntry(const TownAssaultBoard &board,const TownAssaultLine &attack,
        const TownAssaultLine &counter,int tick,const std::function<std::optional<MilliFunds>(const TownAssaultLine&)> &value,std::int64_t &work);
    struct TownArrivalCeiling
    {
        MilliFunds value{};
        QJsonArray withdrawals;
        QJsonArray defenders;
        std::shared_ptr<const TownAssaultLine> witness;
        TownArmedMenu lateOptions;
        bool assumedArrival{};
        bool ordinaryAfterConditional{};
        int unexaminedActionTick{-1};
        TownWithdrawalJob alternative;
        double hpAtUnexaminedAction{};
        bool nativeExchangeRequired{},withdrawalCoverage{true};
        bool complete() const{return unexaminedActionTick<0 && !nativeExchangeRequired;}
    };

    // The local capture-and-withdrawal valuation does not include a fresh
    // active shot, a changed defender, or a shot taken while vacating the town.
    // This distinguishes a useful conditional line from a complete restriction.
    template<class Board,class Line>
    std::pair<int,double> unexaminedCapperAction(const Board &board,const Line &attack,const Line &counter,
        std::int64_t &work,TownWithdrawalJob &alternative)
    {
        const int cap=attack.capper,when=attack.entryTick;
        if(cap<0 || cap>=static_cast<int>(board.units.size()) || when<0 || when+2>board.cutoff ||
           (counter.finishTick>=0 && counter.finishTick<when+2))return {-1,0};
        double hp=attack.capperHp;
        for(const auto &shot:counter.shots)
        {
            if(shot.tick>=when+2)break;
            if(shot.target==cap)hp=shot.targetAfter;
        }
        if(hp<=0)return {-1,0};
        if(board.units[cap].hasWeapons || board.units[cap].canFire)return {when+2,hp};
        if(!board.withdrawalJob)
            alternative={"WITHDRAWAL_JOB_LEDGER_UNAVAILABLE",attack.town,board.terrain.units[cap].engineUnitId,when+2};
        else alternative=board.withdrawalJob(cap,attack.town,when+2,hp,work);
        if(!alternative.reason.isEmpty())return {when+2,hp};
        return {-1,0};
    }

    // Bound a single, initially full-health soldier's unassisted dated capture.
    // Let it walk unhurt by any route. Only replay witnessed defensive actions
    // starting after it has arrived, at which point all routes share the town.
    // The soldier can abandon without paying any further damage on its next turn.
    template<class Board,class Line,class Value>
    std::optional<TownArrivalCeiling> townArrivalCeiling(const Board &board,const Line &attack,
        const Line &counter,Value value,std::int64_t &work,bool priceActive=true)
    {
        const int n=static_cast<int>(board.units.size()),cap=attack.capper,when=attack.entryTick,side=attack.side;
        if(cap<0 || cap>=n || board.hp[cap]!=10. || attack.capperHp!=10. || when<0 || when%2!=side ||
           attack.blocker!=NO_UNIT || !attack.shots.empty() || !attack.losses.empty() ||
           attack.moves.size()!=1 || attack.moves.front().actor!=cap || attack.moves.front().actionTick!=when ||
           counter.capper!=cap || counter.town!=attack.town || counter.entryTick!=when || counter.shots.empty() ||
           counter.stoppedBeforeEntry)return {};
        if(board.positions[cap]==attack.town && board.units[cap].capturePoints>0)return {};
        for(const auto &m:counter.moves)
        {
            if(m.actor==cap)continue;
            if(m.actor<0 || m.actor>=n || int(board.terrain.units[m.actor].enemy)==side || m.fromTick<=when)return {};
            for(const auto &s:m.path.steps)if(s.tick<=when)return {};
        }
        auto rounds=board.load[cap];
        double hp=10.;
        int last=when;
        std::vector<std::pair<int,MilliFunds>> pricedLosses;
        TownArrivalCeiling out;
        out.nativeExchangeRequired=board.terrain.units[cap].needsNativeTownExchange;
        for(const auto &shot:counter.shots)
        {
            if(work<=0)return {};
            --work;
            const int gun=shot.actor,at=shot.tick;
            if(gun<0 || gun>=n || int(board.terrain.units[gun].enemy)==side || shot.target!=cap || shot.to!=attack.town ||
               at<=when || at<last || at%2==side || std::abs(hp-shot.targetBefore)>1e-6 ||
               board.rights[gun].readyTick<0 || board.rights[gun].readyTick>at || board.rights[gun].lastFreeTick<at)return {};
            out.nativeExchangeRequired|=board.terrain.units[gun].needsNativeTownExchange;
            if(board.conditionalClaims)
            {
                const auto it=board.conditionalClaims->find(gun);
                if(it!=board.conditionalClaims->end())
                    for(const auto &debt:it->second)if(debt.claimant!=cap && debt.through>=at)return {};
            }
            const auto &unit=board.units[cap];
            const int range=manhattanDistance(shot.from,attack.town);
            if(shot.targetAfter>0 && range==1 && unit.minRange<=1 && unit.maxRange>=1)
            {
                // A different stop on the way might resupply a weapon. Require
                // this counter to use the strongest native slot even on full supply.
                const auto full=supportWeapon(board.terrain,cap,gun,range,std::array<int,2>{-1,-1});
                const auto recorded=supportWeapon(board.terrain,cap,gun,range,rounds);
                if(full.base>recorded.base || (recorded.base>0 ? recorded.slot!=shot.counterWeapon :
                   shot.counterWeapon>=0 || shot.counterLoss!=0))return {};
            }
            else if(shot.counterWeapon>=0 || shot.counterLoss!=0)return {};
            spendCombatRound(rounds,shot.counterWeapon);
            pricedLosses.emplace_back(at,(side?-1:1)*(shot.targetLoss-shot.counterLoss));
            if(board.rights[gun].displacedValue>0)
                pricedLosses.emplace_back(at,(side?1:-1)*board.rights[gun].displacedValue);
            out.defenders.append(QJsonObject{{"unit",board.terrain.units[gun].engineUnitId},{"tick",at}});
            hp=shot.targetAfter;last=at;
        }
        if(pricedLosses!=counter.losses)return {};
        auto stopped=counter;
        const auto final=value(counter);
        if(!final)return {};
        out.value=*final;
        // Give the attacker a completely safe exit after any completed
        // opposing volley. It cannot move between two guns on that reply.
        for(std::size_t i=0;i<counter.shots.size();++i)
        {
            const auto &shot=counter.shots[i];
            if(shot.targetAfter<=0 || (i+1<counter.shots.size() && counter.shots[i+1].tick==shot.tick) ||
               (counter.finishTick>=0 && counter.finishTick<=shot.tick))continue;
            if(work<=0)return {};
            --work;
            stopped.finishTick=-1;stopped.losses.clear();
            for(const auto &loss:counter.losses)if(loss.first<=shot.tick)stopped.losses.push_back(loss);
            const auto price=value(stopped);
            if(!price)return {};
            out.withdrawals.append(QJsonObject{{"afterTick",shot.tick},{"hp",shot.targetAfter},{"score",static_cast<qint64>(*price)}});
            out.value=side?std::min(out.value,*price):std::max(out.value,*price);
        }
        // If the first shot comes after the first available decision to
        // leave, the attacker can abandon before that shot at no material cost.
        if(counter.shots.front().tick>when+1)
        {
            stopped.finishTick=-1;stopped.losses.clear();
            const auto noHit=value(stopped);if(!noHit)return {};
            out.value=side?std::min(out.value,*noHit):std::max(out.value,*noHit);
        }
        const auto [next,remaining]=unexaminedCapperAction(board,attack,counter,work,out.alternative);
        out.unexaminedActionTick=next;out.hpAtUnexaminedAction=remaining;
        if(priceActive && next>=0 && (board.units[cap].canFire||board.units[cap].hasWeapons)){
            auto offered=std::min<std::int64_t>(30000,work/3);const auto start=offered;
            out.lateOptions=priceTownArmedPostEntry(board,attack,counter,next,value,offered);work-=start-offered;
            if(out.lateOptions.enumerated && out.lateOptions.allArrivalResources && out.lateOptions.open.isEmpty() && out.lateOptions.best){
                out.value=side?std::min(out.value,*out.lateOptions.best):std::max(out.value,*out.lateOptions.best);
                out.unexaminedActionTick=-1;
            }
        }
        return out;
    }
}
