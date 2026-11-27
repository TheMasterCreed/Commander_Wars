#pragma once

#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    struct TownTitleOutcome
    {
        TownAssaultLedger ledger;
        QJsonArray purchases;
    };

    // On this branch it is one town. The incumbent earns its actual
    // paydays until the second transfer. Every purchase sees both changes
    // in chronological order before testing its day's wallet.
    template<class Timing>
    std::optional<TownTitleOutcome> priceTownTitleSequence(const TownAssaultLine &first,const TownAssaultLine &second,
        const PropertyStockColumn &town,const QJsonArray &orders,std::span<const MilliFunds> cash,int cutoff,Timing timing)
    {
        if(first.finishTick<0 || first.finishTick>cutoff || second.finishTick>cutoff ||
           first.town!=town.tile || second.town!=town.tile || first.side==second.side ||
           (second.finishTick>=0 && (second.entryTick<=first.finishTick || second.finishTick<second.entryTick)))return {};
        auto changed=town;changed.ownerBefore=first.side?OwnerSign::Enemy:OwnerSign::Ours;
        auto starting=townAssaultLedger(first,town,cash,cutoff,timing);
        if(!starting)return {};
        auto combined=townAssaultLedger(second,changed,starting->flow,cutoff,timing);
        if(!combined || !RelativeStockDetail::checkedAdd(combined->ownership,starting->ownership) ||
           !RelativeStockDetail::checkedAdd(combined->capitalLoss,starting->capitalLoss) ||
           !RelativeStockDetail::checkedAdd(combined->score,starting->score) ||
           !RelativeStockDetail::checkedAdd(combined->timing,starting->timing))return {};
        auto purchases=townPurchasesWithIncome(orders,[&](int tick)->std::optional<MilliFunds>{
            auto amount=assaultPaydayChange(first,town,tick);
            if(!RelativeStockDetail::checkedAdd(amount,assaultPaydayChange(second,changed,tick)))return {};
            return amount;
        });
        if(!purchases)return {};
        return TownTitleOutcome{std::move(*combined),std::move(*purchases)};
    }

    struct TownEarlyTitleResponse
    {
        QString status;
        bool holderHasTurnBeforeChallenge{};
        std::optional<TownAssaultAlternatives> late;
        std::optional<TownTitleOutcome> outcome;
    };

    template<class Timing>
    TownEarlyTitleResponse challengeQuietEarlyTitle(const TownAssaultBoard &board,const TownAssaultAlternatives &early,
        const TownAssaultTarget &late,StockRouteLowerBounds &bounds,const QJsonArray &orders,std::span<const MilliFunds> cash,
        int cutoff,Timing timing,std::int64_t &work)
    {
        TownEarlyTitleResponse response;
        const auto reject=[&](QString why){response.status=std::move(why);return response;};
        if(early.workLimited || early.unresolved || !early.assault)return reject("EARLY_POLICY_UNRESOLVED");
        const auto &line=early.selectsResponse && early.response?*early.response:*early.assault;
        const int owner=line.side,actor=line.capper,n=static_cast<int>(board.units.size());
        if(actor<0 || actor>=n || !board.units[actor].canCapture || line.town!=late.property.tile ||
           line.finishTick<0 || line.capperHp<=0 || line.installments.empty() || line.installments.back().second<board.threshold)
            return reject("NO_EARNED_EARLY_TITLE");
        if(!line.shots.empty() || line.moves.size()!=1 || line.moves.front().actor!=actor ||
           line.capperHp!=board.hp[actor] || (early.response && !early.response->shots.empty()))
            return reject("EARLY_COMBAT_REQUIRES_DATED_RESOURCE_REPLAY");
        if(late.occupiedFrom<=line.finishTick || late.defendingSide!=owner)return reject("CHALLENGE_PRECEDES_COMPLETED_TITLE");
        if(late.occupiedFrom!=late.occupiedThrough || late.occupiedFrom%2==owner)
            return reject("POST_TITLE_REQUIRES_EXACT_OPPOSING_ACTIVATION");
        response.holderHasTurnBeforeChallenge=line.finishTick+2<late.occupiedFrom;
        // A friendly town may buy repairs and replenish both weapons/fuel
        // before its owner acts. This quote has no post-maintenance unit or
        // bank snapshot and cannot carry yesterday's state across that turn.
        if(response.holderHasTurnBeforeChallenge)return reject("POST_TITLE_MAINTENANCE_STATE_UNPRICED");
        const auto &move=line.moves.front();
        if(move.destination!=line.town || move.origin!=board.positions[actor] || move.path.entryTick!=line.entryTick ||
           move.path.fuel<0 || (board.rights[actor].fuel>=0 && board.rights[actor].fuel<move.path.fuel))
            return reject("INCOMPATIBLE_EARLY_TRIP");
        if(board.rights[actor].lastFreeTick<late.occupiedFrom)return reject("HOLDER_HAS_ANOTHER_DATED_COMMITMENT");
        for(int tick=line.entryTick;tick<=board.cutoff;++tick)
        {
            if(work<=0)return reject("POST_TITLE_HOLD_WORK_LIMIT");--work;
            if(!board.traffic.open(actor,owner,TownAssaultDetail::cell(board.terrain,line.town),tick,true))
                return reject("HOLDER_COLLIDES_WITH_RETAINED_POST");
        }
        auto traffic=board.traffic;
        TownAssaultDetail::reserve(board.terrain,traffic,move,board.cutoff);
        auto positions=std::vector<TilePoint>(board.positions.begin(),board.positions.end());positions[actor]=line.town;
        auto ability=std::vector<TownAssaultRight>(board.rights.begin(),board.rights.end());
        auto contingent=std::vector<TownAssaultRight>(board.beforeConditionalDuty.begin(),board.beforeConditionalDuty.end());
        for(auto *rights:{&ability,&contingent})for(int i=0;i<static_cast<int>(rights->size());++i)
        {
            auto &r=(*rights)[i];if(r.readyTick<0)continue;
            const int side=int(board.terrain.units[i].enemy),next=line.finishTick+1+int((line.finishTick+1)%2!=side);
            r.readyTick=std::max(r.readyTick,next);r.mobileFrom=std::max(r.mobileFrom,r.readyTick);
            if(i==actor)
            {
                if(r.fuel>=0 && r.fuel<move.path.fuel){r.readyTick=-1;continue;}
                if(r.fuel>=0)r.fuel-=move.path.fuel;r.displacedValue=0;
            }
        }
        auto recruits=std::vector<FundedCaptureGun>(board.recruits.begin(),board.recruits.end());
        for(auto &g:recruits)g.readyTick=std::max(g.readyTick,line.finishTick+1+int((line.finishTick+1)%2!=g.side));
        const auto at=[&](int unit,int tick){return unit==actor && tick>=line.entryTick?line.town:board.at(unit,tick);};
        TownAssaultBoard observed{board.terrain,board.units,traffic,positions,board.hp,board.load,ability,recruits,at,
            board.recruitFree,board.threshold,board.horizon,board.cutoff,board.canRemove,board.minimumEntryCost,
            contingent,board.conditionalClaims,board.canLoseReactionGun};
        auto changed=late;changed.occupant=actor;changed.property.ownerBefore=owner?OwnerSign::Enemy:OwnerSign::Ours;
        const auto worth=[&](const TownAssaultLine &bid)->std::optional<MilliFunds>{
            auto evaluated=priceTownTitleSequence(line,bid,late.property,orders,cash,cutoff,timing);
            return evaluated?std::optional<MilliFunds>{evaluated->ledger.score}:std::nullopt;
        };
        response.late=priceTownAssault(observed,changed,bounds,worth,work);
        auto &challenge=*response.late;
        if(challenge.workLimited || challenge.unresolved)return reject("POST_TITLE_RESPONSE_INCOMPLETE");
        TownAssaultLine declined;declined.town=line.town;declined.side=1-owner;
        const auto &chosen=challenge.selectsAssault && challenge.assault?
            (challenge.selectsResponse && challenge.response?*challenge.response:*challenge.assault):declined;
        const bool removed=std::any_of(chosen.shots.begin(),chosen.shots.end(),[&](const auto &s){
            return s.target==actor && s.targetAfter<=0 && s.tick<line.finishTick+2;
        });
        if((line.capperHp<10 && !removed && line.finishTick+2<=cutoff) ||
           (chosen.finishTick>=0 && chosen.capperHp<10 && chosen.finishTick+2<=cutoff))
            return reject("NEW_TITLE_HOLDER_REPAIR_WALLET_UNPRICED");
        response.outcome=priceTownTitleSequence(line,chosen,late.property,orders,cash,cutoff,timing);
        response.status=response.outcome?"PRICED_AFTER_EARLY_TITLE_WITH_RETAINED_HOLDER":"TWO_TRANSFER_WALLET_UNRESOLVED";
        return response;
    }
}
