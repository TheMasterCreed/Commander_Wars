#pragma once
#include "ai/coordinator/fundedinterceptvalue.h"
#include "ai/coordinator/combatrecruitreply.h"

namespace Coordinator
{
    struct StockReserveDuelDamage
    {
        double attack{},counter{-1};
        int weapon{-1},counterWeapon{-1};
    };
    enum class ObservedCapperAction {Capture,AttackReserve};
    struct ObservedStockLine
    {
        ObservedCapperAction action{};
        int completion{-1},carry{};
        double capperHp{},reserveHp{},capperAfterFirst{},reserveAfterFirst{};
        std::array<int,2> capperAmmo{},reserveAmmo{};
        std::array<int,2> reserveAmmoAfterFirst{};
        QJsonArray installments;
        std::vector<std::pair<int,MilliFunds>> losses;
        FundedInterceptionPrice price;
    };

    inline std::optional<StockReserveDuelDamage> observedReserveVolley(
        const CombatTerrain &t,int capper,TilePoint property,const FundedCaptureGun &reserve,TilePoint firing,
        bool stockActs,double hp,double victimHp,const std::array<int,2> &load,const std::array<int,2> &victimLoad,
        std::int64_t *work=nullptr)
    {
        if(!reserve.unit || !t.defense || capper<0 || capper>=static_cast<int>(t.units.size()) ||
           std::abs(property.x-firing.x)+std::abs(property.y-firing.y)!=1)return {};
        const auto &u=*reserve.unit;
        if(t.production)
        {
            if(!work || !t.production->contactDamage)return {};
            NativeContactPiece nativeStock{capper,int(t.units[capper].enemy),{},property,
                static_cast<int>(std::lround((stockActs?hp:victimHp)*10)),stockActs?load:victimLoad};
            NativeContactPiece nativePaid{NO_UNIT,reserve.side,u.type,firing,
                static_cast<int>(std::lround((stockActs?victimHp:hp)*10)),stockActs?victimLoad:load};
            const auto q=t.production->contactDamage(stockActs?nativeStock:nativePaid,stockActs?nativePaid:nativeStock,*work);
            if(!q || q->attack<0 || q->weapon<0 || q->weapon>1 || load[q->weapon]==0 ||
               (q->counter>=0 && (q->counterWeapon<0 || q->counterWeapon>1 || victimLoad[q->counterWeapon]==0)))return {};
            return StockReserveDuelDamage{q->attack,q->counter,q->weapon,q->counter>=0?q->counterWeapon:-1};
        }
        const int sq=firing.y*t.width+firing.x;
        if(capper>=static_cast<int>(u.captureCounter.size()) || capper>=static_cast<int>(u.againstCapper.size()) ||
            sq<0 || sq>=static_cast<int>(u.routeAt.size()))return {};
        const int route=u.routeAt[sq];
        if(route<0 || route>=static_cast<int>(u.defense.size())) return {};
        const auto stockDefense=t.defense(capper,property),paidDefense=u.defense[route];
        const auto &attack=stockActs?u.againstCapper[capper]:u.captureCounter[capper];
        const auto &counter=stockActs?u.captureCounter[capper]:u.againstCapper[capper];
        const auto slot=usableOpeningWeapon(attack,load);
        if(slot<0)return {};
        StockReserveDuelDamage result;
        result.weapon=slot;result.attack=terrainReducedDamage(stockActs?paidDefense:stockDefense,attack[slot],
            ceilHpSteps(hp),ceilHpSteps(victimHp));
        const auto left=engineHpAfterReply(victimHp,engineShotDamage(result.attack));
        if(left>0)
        {
            result.counterWeapon=usableOpeningWeapon(counter,victimLoad);
            if(result.counterWeapon>=0)result.counter=sameActionCounterDamage(stockActs?stockDefense:paidDefense,
                counter[result.counterWeapon],victimHp,result.attack,ceilHpSteps(hp));
        }
        return result;
    }

    // Compute the two actions the stock capper can actually take while
    // standing on a property. A stationary attack keeps the previous progress
    // and consumes its capture activation. Changed income is applied only by
    // the ordinary dated transfer calculation.
    template<class Volley,class Timing>
    std::optional<ObservedStockLine> quoteObservedStockLine(ObservedCapperAction action,int aidSide,
        int shot,int originalFinish,int cutoff,const PropertyStockColumn &property,int startCarry,int threshold,
        const KnownUnit &cap,double capHp,const std::array<int,2> &capLoad,MilliFunds capCost,
        double reserveHp,const std::array<int,2> &reserveLoad,MilliFunds reserveCost,
        std::span<const MilliFunds> baseline,const QJsonArray &purchases,Volley volley,Timing timing)
    {
        if(aidSide<0 || aidSide>1 || shot<0 || shot%2!=aidSide || shot>=originalFinish ||
           originalFinish>cutoff || startCarry<=0 || startCarry>=threshold || capHp<=0 ||
           reserveHp<=0 || capCost<=0 || reserveCost<=0)return {};
        ObservedStockLine out;out.action=action;out.carry=startCarry;
        out.capperHp=capHp;out.reserveHp=reserveHp;out.capperAmmo=capLoad;out.reserveAmmo=reserveLoad;
        const int side=1-aidSide;
        const auto fight=[&](int tick,bool stockActs){
            auto &a=stockActs?out.capperHp:out.reserveHp;
            auto &v=stockActs?out.reserveHp:out.capperHp;
            auto &rounds=stockActs?out.capperAmmo:out.reserveAmmo;
            auto &back=stockActs?out.reserveAmmo:out.capperAmmo;
            if(a<=0||v<=0)return false;
            const auto result=volley(stockActs,a,v,rounds,back);
            if(!result || result->weapon<0 || result->weapon>1 || rounds[result->weapon]==0 ||
               !std::isfinite(result->attack) || result->attack<0)return false;
            const auto previousA=a,previousV=v;
            v=engineHpAfterReply(v,engineShotDamage(result->attack));
            if(rounds[result->weapon]>0)--rounds[result->weapon];
            // Native average combat has already resolved initiative. A
            // first-striking defender can fire and still die in this exchange.
            if(result->counter>=0)
            {
                const int i=result->counterWeapon;
                if(!std::isfinite(result->counter)||i<0||i>1||back[i]==0)return false;
                a=engineHpAfterReply(a,engineShotDamage(result->counter));
                if(back[i]>0)--back[i];
            }
            const auto lostA=combatCapitalLoss(stockActs?capCost:reserveCost,previousA,a);
            const auto lostV=combatCapitalLoss(stockActs?reserveCost:capCost,previousV,v);
            const bool attackerIsOurs=(stockActs?side:aidSide)==0;
            out.losses.emplace_back(tick,attackerIsOurs?lostA-lostV:lostV-lostA);
            return true;
        };
        if(!fight(shot,false))return {};
        out.capperAfterFirst=out.capperHp;out.reserveAfterFirst=out.reserveHp;
        out.reserveAmmoAfterFirst=out.reserveAmmo;
        int next=shot+1;
        if(action==ObservedCapperAction::AttackReserve)
        {
            if(next>cutoff || !fight(next,true))return {};
            next+=2;
        }
        const int rate=replyCaptureRate(cap.captureRate,cap.hpRounded-ceilHpSteps(out.capperHp),out.capperHp<=0);
        for(int t=next;t<=cutoff && rate>0 && out.carry<threshold;t+=2)
        {
            const int before=out.carry;out.carry=std::min(threshold,out.carry+rate);
            out.installments.append(QJsonObject{{"tick",t},{"carryBefore",before},{"carryAfter",out.carry},{"hpBeforeReply",out.capperHp}});
            if(out.carry==threshold)out.completion=t;
        }
        auto price=replaceProvedCapture(side,originalFinish,out.completion,property,baseline,purchases,
            out.losses,cutoff,timing);
        if(!price)return {};
        out.price=std::move(*price);
        return out;
    }
}
