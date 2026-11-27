#pragma once

#include "ai/coordinator/fundedstockfork.h"
#include "ai/coordinator/stockcapturevalidation.h"

namespace Coordinator
{
    struct FundedResponseWindow
    {
        int firstRedeploy{-1};
        double initialHp{};
        int initialFuel{},initialCarry{};
        std::array<int,2> initialAmmo{};
        PropertyStockColumn property;
    };

    // Screen a *specific* branch from the normal stock settlement. Unrelated
    // forecast posts (including stationary unassigned enemies) are not shots.
    // A positive price can subsequently use this window for a single response.
    inline std::optional<FundedResponseWindow> fundedResponseWindow(const CombatTerrain &t,
        const ProductionProjection &p,const QJsonArray &orders,const StockCaptureSettlement &settled,
        std::span<const ProductionProperty> properties,int actor,int reserve,const QJsonObject &proof,int cutoff,QString &reason)
    {
        reason="CAPTURE_OR_PURCHASE_WITNESS_DOES_NOT_MATCH";
        const int n=static_cast<int>(t.units.size());
        if(!settled.townTraffic || actor<0 || actor>=n || reserve<0 || reserve>=static_cast<int>(p.fundedGuns.size()) ||
           !stockEncounterPath(proof,t.width,t.height) || !settled.proofs.contains(proof))return {};
        if(settled.townBusyPaid.contains(reserve)){reason="RESERVE_ALREADY_COMMITTED";return {};}
        if(!settled.townAfterCapture.contains(actor)){reason="CAPTURE_RELEASE_HAS_ANOTHER_DEPENDENCY";return {};}
        const auto &g=p.fundedGuns[reserve];
        const auto &body=settled.townAfterCapture.at(actor);
        const int original=proof["actualCompletionTick"].toInt(-1),identity=t.units[actor].engineUnitId;
        const auto point=[](TilePoint tile){return QJsonArray{tile.x,tile.y};};
        const auto tile=[](const QJsonValue &v){const auto xy=v.toArray();return TilePoint{xy.at(0).toInt(-1),xy.at(1).toInt(-1)};};
        if(!g.unit || g.targetUnit!=NO_UNIT || g.unit->guaranteedShots<=0 || g.exposure>=g.unit->cost ||
           g.side==int(t.units[actor].enemy) || proof["unit"].toInt(NO_UNIT)!=identity ||
           original<0 || original>=cutoff || body.tile!=tile(proof["tile"]) || body.hp<=0 ||
           std::abs(body.hp-proof["hpAfter"].toDouble())>1e-8 || body.readyTick!=original+2)return {};
        auto col=std::find_if(properties.begin(),properties.end(),[&](const auto &v){return v.tile==body.tile;});
        if(col==properties.end())return {};
        auto pay=col->column;
        int transferCount=0;
        for(const auto &event:col->ownershipChanges)
        {
            if(event.completionTick>original)return {};
            if(event.completionTick==original)
            {
                if(event.entryTick!=proof["entryTick"].toInt(-1) ||
                   event.owner!=(t.units[actor].enemy ? OwnerSign::Enemy:OwnerSign::Ours))return {};
                ++transferCount;
            }
            else pay.ownerBefore=event.owner;
        }
        if(transferCount!=1){reason="ORIGINAL_TITLE_NOT_UNIQUELY_BOOKED";return {};}
        if(!proof["shots"].toArray().isEmpty())return {};
        bool funded=false;
        int first=-1;
        for(const auto &v:orders)
        {
            const auto o=v.toObject();
            if(o["buildHalfTurn"].toInt()==g.buildTick && o["factory"].toArray()==point(g.factory))
            {
                if(funded || o["kind"]!="DEPLOY" || !o["otherPricedShots"].toArray().isEmpty() ||
                   !o["paidCaptureDefenseUses"].toArray().isEmpty() ||
                   o["type"].toString()!=g.unit->type)return {};
                funded=true;
                // The fixed first stop is a genuine earlier activation. A
                // cancelled job's release is a separate deadline after arrival.
                const int reached=o.value("arrivalHalfTurn").toInt(-1);
                const bool firstIsPost=o["end"].toArray()==o["firstStop"].toArray();
                if(!firstIsPost && reached<0)return {};
                first=std::max({g.readyTick,o["reserveReadyTick"].toInt(g.readyTick),
                                (firstIsPost ? o["firstStepHalfTurn"].toInt(g.readyTick):reached)+2});
            }
            if(o["target"].toInt(NO_UNIT)==identity ||
               (o["kind"]=="CAPTURE" && o["end"].toArray()==point(body.tile))){reason="EARLIER_PURCHASE_OWNS_THIS_TARGET";return {};}
            for(const auto &s:o["otherPricedShots"].toArray())
                if(s.toObject()["target"].toInt(NO_UNIT)==identity)return {};
        }
        if(!funded || first>=original){reason="RESERVE_NOT_RELEASED_BEFORE_ORIGINAL_CAPTURE";return {};}
        // Losing or moving this capper must not leave another promised job on
        // the board. A declined competing route can also rely on this screen.
        const auto mustFire=[&](int who){
            const auto duty=settled.townGunDutyThrough.find(who);
            return duty!=settled.townGunDutyThrough.end() && duty->second>=first;
        };
        if(mustFire(actor) || mustFire(n+reserve)){reason="ORIGINAL_GUN_DUTY";return {};}
        FundedResponseWindow out{first};out.property=pay;
        out.initialAmmo=body.ammo;out.initialHp=body.hp;
        const auto spent=proof["fuelSpent"].toInt(-1);
        if(spent<0 || (body.fuel>=0 && spent>std::numeric_limits<int>::max()-body.fuel))return {};
        out.initialFuel=body.fuel<0?-1:body.fuel+spent;
        auto installments=proof["captureInstallments"].toArray();
        if(installments.isEmpty())return {};
        out.initialCarry=installments.first().toObject()["carryBefore"].toInt();
        for(const auto &v:settled.proofs)
        {
            const auto other=v.toObject();if(other==proof || other["status"]!="WITNESSED")continue;
            const int entry=other["entryTick"].toInt(-1),end=other["actualCompletionTick"].toInt(-1);
            const auto target=tile(other["tile"]);
            if(target==body.tile && end>=original)return {};
            if(other["unit"].toInt(NO_UNIT)==identity && entry>=original)return {};
            for(const auto &sv:other["shots"].toArray())
            {
                const auto s=sv.toObject();
                if(s["tick"].toInt(-1)<first)continue;
                if(s["gun"].toInt(NO_UNIT)==identity)return {};
            }
        }
        return out;
    }
}
