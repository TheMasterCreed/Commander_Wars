#pragma once

#include "ai/coordinator/replypassevaluation.h"
#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "game/gamemap.h"
#include "game/building.h"
#include "game/player.h"
#include "game/unit.h"

namespace Coordinator
{
    inline std::optional<OpeningRecruitTarget> openingRecruitTarget(GameMap &map,
        const BattlefieldKnowledge &knowledge,const CombatTerrain &terrain,const ReplyPassEvaluation &f0,
        const ProductionSite &site,const ProductionChoice &item,int gun,DamageOracle &oracle)
    {
        if(gun<0 || gun>=static_cast<int>(terrain.units.size()) || item.defense.empty())return {};
        const auto &pawn=terrain.units[gun];const auto &u=knowledge.units()[gun];
        if(!pawn.enemy || !pawn.present || pawn.realHp<=0 || u.minRange<1 || u.maxRange<u.minRange)return {};
        auto *square=map.getTerrain(u.x,u.y);
        auto *native=square?square->getUnit():nullptr;
        if(!native || native->getUniqueID()!=pawn.engineUnitId)return {};
        OpeningRecruitTarget out;
        out.shooter=gun;out.minRange=u.minRange;out.maxRange=u.maxRange;out.factory=site.tile;
        out.recruitCost=item.cost;out.recruitLoad=item.captureLoad;out.stockLoad=pawn.ammoByWeapon;
        out.stockHp=pawn.realHp;out.defense=item.defense.back();
        for(const auto &actor:f0.actors)
        {
            const auto &shot=actor.ownShot;
            if(shot.target!=gun)continue;
            out.stockHp=std::min(out.stockHp,shot.targetAfter);
            const int slot=shot.report.counterWeapon;
            if(shot.report.counter>=0 && slot>=0 && slot<2 && out.stockLoad[slot]>0)--out.stockLoad[slot];
        }
        // A beginning-of-turn repair/resupply is another dated event. Until
        // priced here its extra ammunition or HP cannot establish a free shot.
        const bool incompleteLoad=(native->getMaxAmmo1()>0 && out.stockLoad[0]<native->getMaxAmmo1()) ||
            (native->getMaxAmmo2()>0 && out.stockLoad[1]<native->getMaxAmmo2()) ||
            (native->getMaxFuel()>0 && native->getFuel()<native->getMaxFuel());
        if(square->getBuilding() && square->getBuilding()->getOwner()==native->getOwner() &&
           (incompleteLoad || out.stockHp<10.))return {};
        if(incompleteLoad)
            for(int i=0;i<static_cast<int>(terrain.units.size());++i)
            {
                const auto &p=terrain.units[i];
                if(!p.present || !p.enemy || std::abs(p.tile.x-u.x)+std::abs(p.tile.y-u.y)!=1)continue;
                const auto left=f0.enemyAfterOwn.find(i);
                if(left!=f0.enemyAfterOwn.end() && left->second<=0)continue;
                auto *ally=map.getTerrain(p.tile.x,p.tile.y)->getUnit();
                if(!ally) return {};
                // An adjacent native ration/supply capability can alter both
                // the legal route and the available slot before E0 opens.
                const auto actions=ally->getActionList();
                if(std::any_of(actions.begin(),actions.end(),[](const QString &action){
                    return action.startsWith("ACTION_SUPPORT") && (action.endsWith("RATION") || action.endsWith("SUPPLY"));
                }))return {};
            }
        std::map<int,OpeningRecruitPowers> powers;
        auto *owner=map.getPlayer(knowledge.observerId());
        Unit recruit(item.type,owner,false,&map);
        for(int distance=u.minRange;distance<=std::min(u.maxRange,terrain.width+terrain.height);++distance)
        {
            if(!terrain.pay(4))return {};
            OpeningRecruitPowers at{};
            for(int slot=0;slot<2;++slot)
            {
                at[0][slot]=oracle.bestSlotDamage(*native,recruit,distance,u.minRange,u.maxRange,slot==0,slot==1);
                if(distance==1 && item.minRange<=1 && item.maxRange>=1)
                    at[1][slot]=oracle.bestSlotDamage(recruit,*native,distance,item.minRange,item.maxRange,slot==0,slot==1);
            }
            powers.emplace(distance,at);
        }
        out.powers=[powers=std::move(powers)](int distance)->std::optional<OpeningRecruitPowers>{
            const auto at=powers.find(distance);
            if(at==powers.end())return {};
            return at->second;
        };
        return out;
    }

    inline QJsonObject openingRecruitRecord(const OpeningRecruitFight &shot,int engineId)
    {
        const auto pt=[](TilePoint p){return QJsonArray{p.x,p.y};};
        return {{"enemyShooter",engineId},{"origin",pt(shot.origin)},{"from",pt(shot.from)},{"factory",pt(shot.factory)},
            {"movementCost",shot.movementCost},{"weapon",shot.weapon},{"counterWeapon",shot.counterWeapon},
            {"shooterBefore",shot.stockBefore},{"shooterAfter",shot.stockAfter},{"recruitBefore",shot.recruitBefore},
            {"recruitAfter",shot.recruitAfter},{"shooterAmmoAfter",QJsonArray{shot.stockLoad[0],shot.stockLoad[1]}},
            {"recruitAmmoAfter",QJsonArray{shot.recruitLoad[0],shot.recruitLoad[1]}},
            {"shooterCapitalLost",static_cast<qint64>(shot.stockCapitalLost)},
            {"recruitCapitalLost",static_cast<qint64>(shot.recruitCapitalLost)}};
    }
}
