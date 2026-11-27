#pragma once

#include "ai/coordinator/stockneutralrebid.h"

namespace Coordinator
{
    struct TownQuoteFocus
    {
        TilePoint tile{INVALID_TILE};
        int attackingSide{},capperUnit{NO_UNIT},entryTick{-1};
        TilePoint friendlyExit{INVALID_TILE};
        std::int64_t decisionWork{};
    };

    enum class TownChallengeStatus { Priced, Baseline, Unavailable, WorkLimited, Unsupported, Conflict };
    struct TownChallengeReceipt
    {
        TilePoint tile;
        int attackingSide{};
        TownChallengeStatus status{TownChallengeStatus::Unsupported};
        MilliFunds signedChange{};
        int capperUnit{NO_UNIT},entryTick{-1};bool required{};
    };
    inline bool townChallengeComplete(TownChallengeStatus status)
    {
        return status==TownChallengeStatus::Priced || status==TownChallengeStatus::Baseline || status==TownChallengeStatus::Unavailable;
    }
    inline QString townChallengeName(TownChallengeStatus s)
    {
        switch(s)
        {
            case TownChallengeStatus::Priced: return "PRICED";
            case TownChallengeStatus::Baseline: return "BASELINE";
            case TownChallengeStatus::Unavailable: return "UNAVAILABLE";
            case TownChallengeStatus::WorkLimited: return "WORK_LIMITED";
            case TownChallengeStatus::Conflict: return "CONFLICT";
            default: return "UNSUPPORTED";
        }
    }

    struct TownOpportunity
    {
        TilePoint tile;
        int attacker,firstPossible;
        std::int64_t allowance{};
        int capperUnit{NO_UNIT},entryTick{-1};bool required{},materialOnly{};
        std::int64_t decisionWork{},requestedDecisionWork{};
    };

    inline std::pair<std::int64_t,std::int64_t> townMenuPools(std::int64_t ordinary,std::int64_t named,std::int64_t work,bool protectedMinimum)
    {
        const auto minimum=protectedMinimum?1:0;
        const auto surplus=work-minimum*(ordinary+named);
        const auto matched=named==0?0:ordinary?surplus/2:surplus;
        return {minimum*ordinary+surplus-matched,minimum*named+matched};
    }

    // Every member already received its starting-board share. A fresh
    // search must offer more than its previous limit to justify restarting.
    // Never borrow work from the separately reserved portfolio settlement.
    inline std::vector<std::int64_t> townRetryAllowances(std::span<const TownChallengeStatus> status,
        std::span<const std::int64_t> previous,std::int64_t remaining)
    {
        std::vector<std::int64_t> offers(status.size());
        std::vector<std::size_t> candidates;
        for(std::size_t i=0;i<status.size();++i)
            if(status[i]==TownChallengeStatus::WorkLimited)candidates.push_back(i);
        while(!candidates.empty())
        {
            const auto count=static_cast<std::int64_t>(candidates.size());
            const auto share=remaining/count,remainder=remaining%count;
            // If everyone would repeat the same allowance, a retry cannot
            // improve the result. No row can claim another attempt's work.
            std::vector<std::size_t> improved;
            for(std::int64_t j=0;j<count;++j)
                if(share+int(j<remainder)>previous[candidates[j]])improved.push_back(candidates[j]);
            if(improved.size()==candidates.size())
            {
                for(std::int64_t j=0;j<count;++j)offers[candidates[j]]=share+int(j<remainder);
                break;
            }
            candidates=std::move(improved);
        }
        return offers;
    }

    inline bool townMaterialCouldChange(const CombatTerrain &terrain,std::span<const KnownUnit> units)
    {
        std::array<bool,2> possibleBody{};bool possibleWeapon=false;
        for(std::size_t i=0;i<units.size();++i) if(terrain.units[i].present)
        {
            possibleBody[terrain.units[i].enemy]=true;
            possibleWeapon|=units[i].canFire || units[i].hasWeapons;
        }
        if(terrain.production)
            for(const auto &shop:terrain.production->sites)for(int side=0;side<2;++side)
                for(const auto &unit:shop.menu[side]){possibleBody[side]=true;possibleWeapon|=unit.maxRange>0;}
        return possibleBody[0] && possibleBody[1] && possibleWeapon;
    }

    // Schedule the same starting-board property/side combinations for every
    // F0 squad and E0 reply. Only measured physical capability and whether a
    // captured property could pay in time screen the common roster. Projected
    // purchases, movements, damage and prior provisional winners do not.
    inline std::vector<TownOpportunity> frozenTownOpportunities(const CombatTerrain &terrain,
        std::span<const KnownUnit> units,std::span<const ProductionProperty> properties,TilePoint home,TilePoint away,
        int horizon,int threshold,std::int64_t allowance,std::optional<TownQuoteFocus> focus={},std::span<const TownQuoteFocus> required={},const StockRouteLowerBounds *bounds=nullptr)
    {
        std::vector<TownOpportunity> opportunities;
        const int last=2*horizon-1;
        const bool possibleMaterial=townMaterialCouldChange(terrain,units);
        for(const auto &p:properties)
        {
            const auto at=p.tile;
            if(at==home || at==away || (p.column.income.oursPerTurn<=0 && p.column.income.enemyPerTurn<=0)) continue;
            if(terrain.production && std::any_of(terrain.production->sites.begin(),terrain.production->sites.end(),
                [&](const auto &shop){return shop.tile==at;})) continue;
            std::array<int,2> arrivals{last+1,last+1},titles=arrivals;
            for(int side=0;side<2;++side)
            {
                if(focus && (focus->attackingSide!=side || focus->tile!=at))continue;
                int first=last+1,title=last+2;
                for(int i=0;i<static_cast<int>(units.size());++i)
                {
                    const auto &p0=terrain.units[i];const auto &u=units[i];
                    if(!p0.present || int(p0.enemy)!=side || !u.canCapture || p0.grid<0 || u.captureRate<=0) continue;
                    const auto &grid=terrain.grids[p0.grid];
                    const auto *cache=terrain.movementCache.get();
                    const int floor=cache && cache->minimumEntryCost.contains(&grid)?cache->minimumEntryCost.at(&grid):0;
                    const auto *prepared=bounds?bounds->preparedCost(grid,at.y*terrain.width+at.x):nullptr;
                    const auto distance=prepared?std::int64_t((*prepared)[p0.tile.y*terrain.width+p0.tile.x]):
                        std::int64_t(manhattanDistance(p0.tile,at))*floor;
                    if(distance==std::numeric_limits<int>::max() || (p0.tile!=at && (p0.movement<=0 || (u.fuel>0 && distance>u.fuel))))continue;
                    const auto moves=p0.tile==at?0:(distance+p0.movement-1)/p0.movement;
                    const int earliest=static_cast<int>(std::min<std::int64_t>(last+1,std::max<std::int64_t>(2+side,side+2*std::max<std::int64_t>(0,moves-1))));
                    if(earliest>last)continue;
                    first=std::min(first,earliest);
                    const int possibleRate=u.captureRate+std::max(0,10-u.hpRounded);
                    const int carry=p0.tile==at ? std::max(0,u.capturePoints) : 0;
                    const int remainder=static_cast<int>(widenedCaptureTurns(carry,possibleRate,threshold));
                    title=std::min(title,earliest+2*std::max(0,remainder-1));
                }
                arrivals[side]=first;titles[side]=title;
            }
            for(int side=0;side<2;++side)
            {
                if(focus && (focus->attackingSide!=side || focus->tile!=at))continue;
                bool payable=false;
                for(int tick=titles[side]+1;tick<=2*horizon;++tick)
                {
                    const int payer=tick%2;
                    if(payer==side || p.column.ownerBefore==(side?OwnerSign::Ours:OwnerSign::Enemy) || titles[1-side]<titles[side])
                        payable|=(payer?p.column.income.enemyPerTurn:p.column.income.oursPerTurn)>0;
                }
                if((arrivals[side]<=last && (payable || possibleMaterial)) || focus)
                    opportunities.push_back({at,side,arrivals[side],0,focus?focus->capperUnit:NO_UNIT,focus?focus->entryTick:-1,false,!payable && possibleMaterial});
            }
        }
        std::stable_sort(opportunities.begin(),opportunities.end(),[](const auto &a,const auto &b) {
            return std::tuple{a.firstPossible,a.tile.y,a.tile.x,a.attacker}<std::tuple{b.firstPossible,b.tile.y,b.tile.x,b.attacker};
        });
        const auto count=static_cast<std::int64_t>(opportunities.size());
        std::set<std::tuple<int,int,int,int,int>> exact;
        if(!focus)for(const auto &f:required)
            if(f.tile!=INVALID_TILE && f.attackingSide>=0 && f.attackingSide<2 && f.capperUnit>=0 && f.entryTick>=0)
                exact.emplace(f.tile.y,f.tile.x,f.attackingSide,f.capperUnit,f.entryTick);
        // Both teams receive the same absolute initial grant from their measured
        // town quota. Preserve a share for every remaining ordinary/named row.
        using Key=std::tuple<int,int,int,int,int>;
        std::optional<Key> front;std::int64_t request=0;int broad=-1;
        if(!focus)for(const auto &f:required)if(f.decisionWork>0)
        {
            const Key key{f.tile.y,f.tile.x,f.attackingSide,f.capperUnit,f.entryTick};
            if(exact.contains(key)){front=key;request=f.decisionWork;break;}
            if(f.capperUnit==NO_UNIT && f.entryTick<0)
            {
                auto it=std::find_if(opportunities.begin(),opportunities.end(),[&](const auto &s){return s.tile==f.tile && s.attacker==f.attackingSide;});
                if(it!=opportunities.end()){front=key;request=f.decisionWork;broad=it-opportunities.begin();break;}
            }
        }
        const auto others=count+static_cast<std::int64_t>(exact.size())-int(front.has_value());
        const auto grant=front && allowance>=request && allowance-request>=others?request:0;
        if(grant)exact.erase(*front);
        const auto [ordinary,reserved]=townMenuPools(count-int(broad>=0 && grant),static_cast<std::int64_t>(exact.size()),allowance-grant,grant>0);
        const auto otherRows=count-int(broad>=0 && grant);
        for(std::int64_t i=0,j=0;i<count;++i)
        {
            auto &s=opportunities[i];
            if(i==broad && grant){s.allowance=grant;s.decisionWork=grant;s.requestedDecisionWork=request;}
            else{s.allowance=ordinary/otherRows+int(j++<ordinary%otherRows);}
        }
        std::int64_t index=0,named=exact.size();
        for(const auto &[y,x,side,cap,tick]:exact)
            opportunities.push_back({{x,y},side,tick,reserved/named+int(index++<reserved%named),cap,tick,true});
        if(grant && broad<0)
        {
            const auto &[y,x,side,cap,tick]=*front;
            opportunities.push_back({{x,y},side,tick,grant,cap,tick,true,false,grant,request});
        }
        else if(front)for(auto &slot:opportunities)
            if(Key{slot.tile.y,slot.tile.x,slot.attacker,slot.capperUnit,slot.entryTick}==*front)slot.requestedDecisionWork=request;
        return opportunities;
    }
}
