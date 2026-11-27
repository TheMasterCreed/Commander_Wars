#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <cstdint>
#include <cstdlib>

namespace Coordinator
{
    // Compare two recorded executions. An overlap can refute these particular
    // orders or require a third action; absence of an overlap never prices a
    // joint alternative. All coordinates and actors belong to one cash/calendar frame.
    inline QJsonObject stockTownOrderInteraction(const QJsonObject &stock,const QJsonObject &town,std::int64_t &work)
    {
        const auto tile=stock.value("tile").toArray();
        const int earlier=stock.value("unit").toInt(-1),later=town.value("capper").toObject().value("unit").toInt(-1);
        const int requested=stock.value("proposedEntryTick").toInt(-1),arrival=town.value("entryTick").toInt(-1);
        if(stock.value("status")!="DECLINED_EXPOSED_APPROACH" || tile.size()!=2 || town.value("tile")!=tile ||
           !stock.value("enemy").isBool() || stock.value("enemy")!=town.value("enemy") ||
           earlier<0 || later<0 || earlier==later || requested<0 || arrival<=requested ||
           stock.value("shots").toArray().isEmpty() || stock.value("stops").toArray().isEmpty())return {};
        QJsonObject result{{"tile",tile},{"stockCapper",earlier},{"townCapper",later},{"stockRouteAttempt",stock.value("routeAttempt")},
            {"stockProposedEntry",requested},{"townRequestedEntry",arrival},
            {"ifStockPasses","ATTEMPT_NOT_EXECUTED_ON_RETAINED_CALENDAR"},
            {"ifStockAttempts","JOINT_REPLAY_REQUIRED"}};
        const auto charge=[&](){if(work>0){--work;return true;}result["ifStockAttempts"]="JOINT_CHECK_WORK_LIMIT";return false;};
        const auto townShots=town.value("shots").toArray(),stockShots=stock.value("shots").toArray();
        // Start with an activation collision: no ordering or alternate route
        // can fire the same unit twice at separate targets on one activation.
        for(const auto &original:stockShots)for(const auto &laterShot:townShots)
        {
            if(!charge())return result;
            const auto a=original.toObject(),b=laterShot.toObject(),who=b.value("actor").toObject();
            const int t=a.value("tick").toInt(-1),u=b.value("tick").toInt(-1),gun=a.value("gun").toInt(-1);
            if(t<0 || u<0 || b.value("target").toObject().value("unit").toInt(-1)!=later)continue;
            const bool same=gun>=0?who.value("unit").toInt(-1)==gun:
                a.value("birthSite").toArray().size()==2 && who.value("birth")==a.value("birthSite") &&
                a.value("birthTick").toInt(-1)>=0 && who.value("builtTick")==a.value("birthTick") &&
                !a.value("recruit").toString().isEmpty() && who.value("type")==a.value("recruit");
            if(!same)continue;
            QString reason;
            if(t==u)reason="ONE_GUN_TWO_TARGETS_ON_SAME_ACTIVATION";
            else if(t<u && a.value("shooterHpAfter").toDouble(1)<=0)reason="STOCK_GUN_DEAD_BEFORE_TOWN_SHOT";
            else if(u<t && b.value("shooterAfter").toDouble(1)<=0)reason="TOWN_GUN_DEAD_BEFORE_STOCK_SHOT";
            else if(t<u)
            {
                const int slot=b.value("weapon").toInt(-1);
                const auto clip=a.value("shooterAmmoAfter").toArray();
                if(slot>=0 && slot<clip.size() && clip[slot].toInt(-1)==0)reason="TOWN_SHOT_REQUIRES_REPLENISHING_SPENT_CLIP";
                else if(a.value("returnTick").toInt(-1)==u && !a.value("returnPath").toArray().isEmpty() &&
                        a.value("returnPath").toArray().last()!=b.value("from"))reason="RETURN_AND_TOWN_SHOT_REQUIRE_DIFFERENT_POSTS";
            }
            if(!reason.isEmpty())
            {
                result["ifStockAttempts"]=reason;result["stockShot"]=a;result["townShot"]=b;return result;
            }
        }
        // A capper's recorded stop lasts across the opposing activation. A
        // new casualty or release can open that square, but changes this pair
        // of lines and needs its own HP, timing and cash-flow replay.
        for(const auto &v:stock.value("stops").toArray())
        {
            const auto stop=v.toObject();const int t=stop.value("tick").toInt(-1);const auto square=stop.value("tile").toArray();
            if(t<0 || square.size()!=2)continue;
            for(const auto &z:townShots)
            {
                if(!charge())return result;
                const auto shot=z.toObject();const int fire=shot.value("tick").toInt(-1);
                if(fire<0 || std::abs(t-fire)!=1 || shot.value("from")!=square ||
                   shot.value("shooterAfter").toDouble(0)<=0 || shot.value("target").toObject().value("unit").toInt(-1)!=later)continue;
                result["ifStockAttempts"]="OVERNIGHT_POST_REQUIRES_CLEARANCE_OR_DIFFERENT_ORDERS";
                result["stockStop"]=stop;result["townShot"]=shot;return result;
            }
        }
        return result;
    }
    inline QJsonObject collectStockTownInteractions(const QJsonArray &stockProofs,const QJsonArray &encounters,
        const QJsonArray &rivals,std::int64_t &work)
    {
        QJsonArray examples;int examined=0;bool limited=false;
        const auto observe=[&](const QJsonObject &alternative,const QString &policy,const QJsonObject &rival){
            const auto line=alternative.value("response").toObject();
            if(line.isEmpty() || limited)return;
            for(const auto &s:stockProofs)
            {
                if(work<=0){limited=true;return;}
                --work;auto item=stockTownOrderInteraction(s.toObject(),line,work);
                if(item.isEmpty())continue;
                ++examined;item["townPolicy"]=policy;
                if(!rival.isEmpty())
                {
                    item["earlierRivalEntry"]=rival.value("entryTick");
                    for(const auto *key:{"unit","birth","buildTick"})if(rival.contains(key))item[key]=rival.value(key);
                }
                if(examples.size()<12)examples.append(item);
                if(item["ifStockAttempts"]=="JOINT_CHECK_WORK_LIMIT"){limited=true;return;}
            }
        };
        for(const auto &v:encounters)observe(v.toObject(),"DIRECT",{});
        for(const auto &v:rivals)
        {
            const auto rival=v.toObject();
            for(const auto *key:{"ifRivalDeclinesEarlyOccupation","ifRivalGuardsWithoutCapturing","lateChallengeAfterEarlyFire"})
                observe(rival.value(key).toObject(),QString::fromLatin1(key),rival);
        }
        if(!examined && !limited)return {};
        return {{"scope","PAIR_OF_RECORDED_LINES_ONLY"},{"comparisons",examined},{"workLimited",limited},{"examples",examples}};
    }

}
