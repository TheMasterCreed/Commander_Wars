#pragma once

#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/realcombatcapital.h"
#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace Coordinator
{
    // One already-financed unit is a distinct E0 target despite having no F0
    // movement order. Its purchase price and its later damage are separate.
    struct OpeningRecruitFight
    {
        int shooter{NO_UNIT};
        TilePoint origin{INVALID_TILE}, factory{INVALID_TILE}, from{INVALID_TILE};
        int movementCost{}, weapon{-1}, counterWeapon{-1};
        double stockBefore{}, stockAfter{}, recruitBefore{}, recruitAfter{};
        std::array<int,2> stockLoad{}, recruitLoad{};
        MilliFunds stockCapitalLost{}, recruitCapitalLost{};
        bool operator==(const OpeningRecruitFight &) const = default;
    };
    using OpeningRecruitPowers=std::array<std::array<double,2>,2>;
    struct OpeningRecruitWindow
    {
        int afterStockFires{};
        TilePoint from{INVALID_TILE};
        bool beforeBlockOrders{};
    };
    struct OpeningRecruitTarget
    {
        static constexpr int AFTER_ALL=-1,COLLECT=-2;
        int shooter{NO_UNIT},minRange{},maxRange{};
        TilePoint factory{INVALID_TILE},from{INVALID_TILE};
        double stockHp{};
        MilliFunds recruitCost{};
        std::array<int,2> stockLoad{},recruitLoad{};
        TerrainDefenseRule defense;
        std::function<std::optional<OpeningRecruitPowers>(int)> powers;
        int afterStockFires{AFTER_ALL};
        bool beforeBlockOrders{};
    };

    inline int usableOpeningWeapon(const std::array<double,2> &power,const std::array<int,2> &load)
    {
        int best=-1;
        for(int i=0;i<2;++i)
            if(load[i]!=0 && std::isfinite(power[i]) && power[i]>0 && (best<0 || power[i]>power[best])) best=i;
        return best;
    }

    // moveCost is a live physical board query after the other charged E0
    // orders. slots(distance) obtains the native per-weapon matchup at this range.
    // Returning no shot simply leaves the existing stock-target alternative intact.
    template<class MoveCost,class Slots,class StockDefense>
    std::vector<OpeningRecruitFight> openingRecruitShots(int stock,TilePoint origin,TilePoint factory,
        int width,int height,int minRange,int maxRange,bool mobile,double stockHp,double recruitHp,
        MilliFunds stockCost,MilliFunds recruitCost,const std::array<int,2> &stockLoad,
        const std::array<int,2> &recruitLoad,const TerrainDefenseRule &factoryDefense,
        MoveCost moveCost,Slots slotsFor,StockDefense defenseAt,std::int64_t &work,bool &complete)
    {
        std::vector<OpeningRecruitFight> results;
        if(stock<0 || origin==INVALID_TILE || factory==INVALID_TILE || width<=0 || height<=0 ||
           minRange<1 || maxRange<minRange || stockHp<=0 || recruitHp<=0 || stockCost<=0 || recruitCost<=0)
        {complete=false;return results;}
        const int radius=std::min(width+height,maxRange);
        for(int y=std::max(0,factory.y-radius);y<std::min(height,factory.y+radius+1);++y)
            for(int x=std::max(0,factory.x-radius);x<std::min(width,factory.x+radius+1);++x)
            {
                const TilePoint from{x,y};const int range=std::abs(x-factory.x)+std::abs(y-factory.y);
                if((!mobile && from!=origin) || range<minRange || range>maxRange)continue;
                if(work<=0){complete=false;return results;}
                --work;
                const int cost=moveCost(from);
                if(cost<0 || cost==UNREACHABLE)continue;
                const auto power=slotsFor(range);
                if(!power){complete=false;return results;}
                const int attack=usableOpeningWeapon((*power)[0],stockLoad);
                if(attack<0)continue;
                const auto rawPrimary=terrainReducedDamage(factoryDefense,(*power)[0][attack],
                    ceilHpSteps(stockHp),ceilHpSteps(recruitHp));
                const auto capAfter=engineHpAfterReply(recruitHp,engineShotDamage(rawPrimary));
                // Positive native matchup power makes the shot legal even
                // when its damage rounds to zero. Movement, ammunition and
                // return fire still change the state for later actions.
                const int back=capAfter>0 ? usableOpeningWeapon((*power)[1],recruitLoad) : -1;
                const auto after=back<0 ? stockHp : engineHpAfterReply(stockHp,engineShotDamage(sameActionCounterDamage(
                    defenseAt(from),(*power)[1][back],recruitHp,rawPrimary,ceilHpSteps(stockHp))));
                OpeningRecruitFight shot{stock,origin,factory,from,cost,attack,back,stockHp,after,recruitHp,capAfter,
                    stockLoad,recruitLoad,combatCapitalLoss(stockCost,stockHp,after),
                    combatCapitalLoss(recruitCost,recruitHp,capAfter)};
                if(shot.stockLoad[attack]>0)--shot.stockLoad[attack];
                if(back>=0 && shot.recruitLoad[back]>0)--shot.recruitLoad[back];
                results.push_back(shot);
            }
        return results;
    }
}
