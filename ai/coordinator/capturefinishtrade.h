#pragma once

#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/propertyeconomics.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include <QJsonArray>
#include <QJsonObject>

namespace Coordinator
{
    // Price the same conditional reply on both opening teams. The singled
    // out enemy continues its in-place claim; all other E0 orders decline.
    // Nothing here restricts the replies admitted in the ranked comparison.
    inline QJsonObject captureFinishPrefix(GameMap &map,const CombatTerrain &terrain,
        const BattlefieldKnowledge &knowledge,std::span<const PropertyFacts> sites,
        const QJsonObject &projection,const QJsonObject &buyer,int target,int threshold,std::int64_t &work)
    {
        const auto open=[](const char *why){return QJsonObject{{"status","OPEN"},{"reason",why}};};
        const int us=knowledge.observerId(),them=1-us;
        if(map.getPlayerCount()!=2 || !terrain.production || threshold<=0 || !buyer["supported"].toBool() ||
           buyer["remainingFunds"].toInteger(-1)<0 || !buyer["orders"].isArray() ||
           terrain.units.size()!=knowledge.units().size())return open("INCOMPLETE_NATIVE_PREFIX");
        for(int side:{us,them})if(!map.getPlayer(side) || map.getPlayer(side)->getCO(0) || map.getPlayer(side)->getCO(1))
            return open("PLAYER_INCOME_OR_CAPTURE_MODIFIERS_OPEN");
        const auto tariff=128LL+24LL*sites.size()+32LL*terrain.units.size()+12LL*buyer["orders"].toArray().size();
        if(tariff>work)return open("CAPTURE_FINISH_TRADE_WORK_LIMIT");
        work-=tariff;
        std::map<std::pair<int,int>,int> owners;
        for(const auto &x:projection["properties"].toArray()){
            const auto p=x.toObject();owners[{p["x"].toInt(),p["y"].toInt()}]=p["owner"].toInt();
        }
        if(owners.size()!=sites.size())return open("PROJECTED_OWNERS_UNBOUND");
        std::array<MilliFunds,2> income{},army{},funds{};
        for(int side:{us,them}){
            income[side]=MilliFunds(map.getPlayer(side)->calcIncome())*MILLI_FUNDS_PER_FUND;
            funds[side]=MilliFunds(map.getPlayer(side)->getFunds())*MILLI_FUNDS_PER_FUND;
        }
        const auto nativeRate=[&](const PropertyFacts &s,int side)->std::optional<MilliFunds>{
            if(side<0)return MilliFunds{0};
            const auto rate=double(s.baseIncome)*map.getPlayer(side)->getFundsModifier();
            if(s.coveredTileCount!=1 || !std::isfinite(rate) || rate<0 || rate>std::numeric_limits<int>::max())return {};
            return MilliFunds(int(rate))*MILLI_FUNDS_PER_FUND;
        };
        QJsonArray titles;
        for(const auto &s:sites){
            const auto it=owners.find({s.x,s.y});
            if(it==owners.end() || it->second< -1 || it->second>1)return open("INVALID_OWNER");
            if(it->second==s.ownerId)continue;
            const auto gained=nativeRate(s,it->second),lost=nativeRate(s,s.ownerId);
            if(!gained || !lost)return open("DATED_TITLE_INCOME_UNBOUND");
            if(it->second>=0)income[it->second]+=*gained;
            if(s.ownerId>=0)income[s.ownerId]-=*lost;
            titles.append(QJsonObject{{"tile",QJsonArray{s.x,s.y}},{"from",s.ownerId},{"to",it->second}});
        }
        const auto units=projection["units"].toArray();
        std::map<int,QJsonObject> bodies;
        for(const auto &v:units){const auto u=v.toObject();if(!bodies.emplace(u["id"].toInt(-1),u).second)return open("DUPLICATE_BODY");}
        int targetIndex=NO_UNIT;
        for(int i=0;i<int(terrain.units.size());++i){
            const auto &u=terrain.units[i];const auto found=bodies.find(u.engineUnitId);
            if(!u.present || found==bodies.end() || u.replacementCost<0)return open("MISSING_NATIVE_BODY");
            const double hp=found->second["hp"].toDouble(-1);
            if(!std::isfinite(hp) || hp<0 || hp>10.)return open("INVALID_NATIVE_HP");
            if(hp>0 && hp<10.){
                const auto at=std::pair{found->second["x"].toInt(),found->second["y"].toInt()};
                const auto own=owners.find(at);
                if(own!=owners.end() && own->second==knowledge.units()[i].ownerId)
                    return open("PAID_SERVICE_CHANGES_FIRST_PAYDAY");
            }
            army[knowledge.units()[i].ownerId]+=qRound64(double(u.replacementCost)*hp/10.);
            if(u.engineUnitId==target)targetIndex=i;
        }
        if(targetIndex<0 || !terrain.units[targetIndex].enemy)return open("TARGET_NOT_AN_OPPOSING_UNIT");
        funds[us]=buyer["remainingFunds"].toInteger()*MILLI_FUNDS_PER_FUND;
        for(const auto &b:buyer["orders"].toArray()){
            const auto price=b.toObject()["price"].toInteger(-1);
            if(price<0)return open("BUY_PRICE_UNBOUND");
            army[us]+=price*MILLI_FUNDS_PER_FUND;
        }
        const auto targetBody=bodies.at(target);const TilePoint at{targetBody["x"].toInt(),targetBody["y"].toInt()};
        const auto prop=std::ranges::find_if(sites,[&](const auto &s){return s.x==at.x && s.y==at.y;});
        if(prop==sites.end() || !prop->capturable || prop->capturerIndex!=targetIndex || !map.onMap(at.x,at.y))
            return open("NO_LIVE_OPPOSING_CLAIM");
        const int carry=targetBody["capPoints"].toInt(-1);const double hp=targetBody["hp"].toDouble(-1);
        if(carry<0 || hp<=0)return open("TARGET_CLAIM_OR_SURVIVAL_CHANGED");
        Unit card(knowledge.units()[targetIndex].unitId,map.getPlayer(them),false,&map);
        card.setTerrain(map.getTerrain(at.x,at.y));card.setHp(hp);
        const int rate=card.getCaptureRate(QPoint(at.x,at.y));
        if(rate<=0)return open("OPPOSING_CAPTURE_RATE_OPEN");
        const bool takes=std::int64_t(carry)+rate>=threshold;
        QJsonArray cashAtFirst,capitalAtFirst,followingIncome,incomeF0;
        for(int side=0;side<2;++side)incomeF0.append(qint64(income[side]));
        funds[them]+=income[them];
        if(takes){
            const int old=owners.at({at.x,at.y});const auto oldRate=nativeRate(*prop,old),newRate=nativeRate(*prop,them);
            if(old==them || !oldRate || !newRate)return open("OPPOSING_TITLE_UNBOUND");
            if(old>=0)income[old]-=*oldRate;
            income[them]+=*newRate;
        }
        funds[us]+=income[us];
        for(int side=0;side<2;++side){
            cashAtFirst.append(qint64(funds[side]));capitalAtFirst.append(qint64(army[side]));
            followingIncome.append(qint64(income[side]));
        }
        return {{"status","CONDITIONAL_FIRST_PAYDAY"},{"F0TitleChanges",titles},{"nativeOpeningBuyer",buyer},
            {"E0Reply","CONTINUE_NAMED_CAPTURE_AND_END_TURN"},{"target",target},{"targetTile",QJsonArray{at.x,at.y}},
            {"targetHpAfterF0",hp},{"targetProgressAfterE0",std::min(threshold,carry+rate)},{"targetTakesAtE0",takes},
            {"incomeAfterF0ByPlayer",incomeF0},{"cashAtFriendlyF1ByPlayer",cashAtFirst},{"openingArmyCapitalByPlayer",capitalAtFirst},
            {"incomeAfterE0ByPlayer",followingIncome},{"cashPlusOpeningArmyMarginAtF1",qint64(funds[us]+army[us]-funds[them]-army[them])},
            {"serviceAndLaterActionsIncluded",false},{"damageBasis","NATIVE_AVERAGE_OPENING"},{"fullHorizonPriced",false}};
    }
}
