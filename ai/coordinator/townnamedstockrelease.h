#pragma once
#include "ai/coordinator/observedstockscreen.h"

namespace Coordinator
{
    inline bool namedEntryDependsOnScreen(const QJsonObject &actor,TilePoint town,int side,int entry)
    {
        const int ready=actor["readyTick"].toInt(-1);
        return entry>=0 && actor["present"].toBool() && actor["side"].toInt(-1)==side &&
            actor["position"].toArray()!=QJsonArray{town.x,town.y} && ready>=0 && ready<=entry &&
            actor["screenThrough"].toInt(-1)>=ready && actor["mobileFrom"].toInt(-1)>ready;
    }

    inline QJsonObject reviewNamedScreenEntry(const CombatTerrain &terrain,const StockCaptureSettlement &settled,
        const StockCaptureTraffic &market,const QJsonObject &facts,TilePoint town,int entry,StockRouteLowerBounds &bounds,std::int64_t &work)
    {
        const int unit=facts["unit"].toInt(NO_UNIT),first=facts["readyTick"].toInt();
        QJsonObject report{{"unit",unit},{"screen",facts["position"]},{"releaseTick",first},{"entryTick",entry}};
        QJsonArray attempts,otherTitles;
        const QJsonArray at{town.x,town.y};
        for(const auto &v:settled.proofs)
        {
            if(work<=0){report["status"]="SCREEN_RECHECK_WORK_LIMIT";report["workLimited"]=true;return report;}--work;
            const auto proof=v.toObject();
            if(proof["tile"].toArray()!=at)continue;
            if(proof["unit"].toInt(NO_UNIT)!=unit)
            {
                if(proof["status"]=="WITNESSED")
                    otherTitles.append(QJsonObject{{"unit",proof["unit"]},{"entry",proof["entryTick"]},
                        {"completion",proof["actualCompletionTick"]},{"enemy",proof["enemy"]}});
                continue;
            }
            for(const auto &a:proof["rejectedCaptureAttempts"].toArray())
            {
                const auto attempt=a.toObject();
                if(attempt["actualEntry"].toInt(-1)==entry)attempts.append(attempt);
            }
        }
        if(!attempts.isEmpty())report["entryAndDefenseOnStockRoute"]=attempts;
        if(!otherTitles.isEmpty())report["competingSettledTitles"]=otherTitles;
        const auto found=std::find_if(terrain.units.begin(),terrain.units.end(),[&](const auto &u){return u.engineUnitId==unit;});
        if(found==terrain.units.end()){report["status"]="MISSING_RELEASE_ACTOR";return report;}
        const ChangedScreenActor actor{static_cast<int>(found-terrain.units.begin()),first};
        const std::int64_t copyCost=2LL*market.cellCount();
        if(work<copyCost){report["status"]="SCREEN_RECHECK_WORK_LIMIT";report["workLimited"]=true;return report;}
        work-=copyCost;
        // Prior claim identifiers are in the stock settlement. The market's
        // final calendar also contains subsequent purchased moves and deaths.
        auto released=market;released.removeFutureUses(actor.actor,first);
        const auto change=reviewObservedScreens(terrain,settled,std::span(&actor,1),&released,bounds,work);
        report["trafficCase"]="FINAL_MARKET_WITH_HOLDER_REMOVED_FROM_RELEASE";
        report["otherCaptureChecks"]=change.checks;
        report["oldScreensPreserved"]=change.complete && change.preserved;
        const bool limited=std::any_of(change.checks.begin(),change.checks.end(),[](const auto &v){
            return v.toObject()["status"]=="SCREEN_RECHECK_WORK_LIMIT";
        });
        report["workLimited"]=!change.complete && (work<=0 || limited);
        report["status"]=!change.complete?"SCREEN_RECHECK_UNFINISHED":change.preserved?
            "OLD_SCREENS_SURVIVE_RELEASE_NAMED_CAPTURE_NEEDS_PRICE":"REPRICE_ROUTES_OPENED_BY_RELEASE";
        return report;
    }
}
