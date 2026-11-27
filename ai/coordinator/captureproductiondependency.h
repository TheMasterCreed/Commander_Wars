#pragma once

namespace Coordinator
{
    // A build window belongs to the stock capturer and its actual next
    // movement, including the interval between that move and the purchase.
    // The finished capture and the later purchase have separate evidence.
    inline QJsonObject proveCapturedProductionExit(const QJsonObject &order,const QJsonArray &proofs,
        const StockCaptureTraffic *traffic,int width,int originalUnitCount)
    {
        const auto parent=order["stockFactoryParent"].toObject();
        const int actor=parent["actor"].toInt(-1),id=parent["unit"].toInt(-1);
        const int buy=order["buildHalfTurn"].toInt(-1),side=int(order["enemy"].toBool());
        const auto square=order["factory"].toArray();
        QJsonObject result{{"status","OPEN"},{"unit",id},{"site",square},{"buildTick",buy}};
        if(actor<0 || id<0 || buy<0 || buy%2!=side || square.size()!=2 || !traffic)
            return result;
        QJsonObject capture,departure;
        for(const auto &item:proofs)
        {
            const auto p=item.toObject();
            if(p["status"]!="WITNESSED" || p["unit"].toInt(-1)!=id || int(p["enemy"].toBool())!=side)
                continue;
            if(p["tile"]==square && p["actualCompletionTick"].toInt(-1)>=0 &&
               p["actualCompletionTick"].toInt() <buy)
                capture=p;
        }
        if(capture.isEmpty()) { result["reason"]="NO_WITNESSED_PARENT_TITLE";return result; }
        const int finish=capture["actualCompletionTick"].toInt();
        result["capture"]=capture;
        for(const auto &item:proofs)
        {
            const auto p=item.toObject();
            if(p["status"]!="WITNESSED" || p["unit"].toInt(-1)!=id || int(p["enemy"].toBool())!=side ||
               p["from"]!=square || p["departureTick"].toInt(-1)<=finish || p["departureTick"].toInt()>buy) continue;
            for(const auto &step:p["stops"].toArray())
            {
                const auto stop=step.toObject();const int tick=stop["tick"].toInt(-1);
                if(tick<=finish || tick>buy || tick%2!=side || stop["tile"].toArray().size()!=2 ||
                   stop["tile"]==square || stop["cells"].toArray().isEmpty()) continue;
                departure=p;result["vacatedTick"]=tick;break;
            }
            if(!departure.isEmpty())break;
        }
        if(departure.isEmpty()){result["reason"]="NO_WITNESSED_OUTWARD_STOP_BEFORE_BUY";return result;}
        const int at=square[1].toInt()*width+square[0].toInt();
        if(at<0 || at>=traffic->cellCount())return result;
        for(const auto &use:traffic->uses(at))
            if(use.actor>=0 && use.actor<originalUnitCount && use.stopped && use.first<=buy && use.last>=buy)
            { result["reason"]=use.actor==actor?"PARENT_STILL_OCCUPIES_BUILD_SITE":"OTHER_ORIGINAL_OCCUPIES_BUILD_SITE";return result; }
        result["departure"]=departure;result["status"]="WITNESSED";
        return result;
    }
}
