#pragma once

#include "ai/productionSystem/simpleproductionsystem.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Coordinator
{
    inline QJsonObject quoteFactoryProgramBuyer(SimpleProductionSystem &buyer, QJsonObject projection,
                                                const QString &forecastPolicyKey)
    {
        const bool future=projection.contains("futureDay");
        const auto unavailable=[](const QString &why){
            return QJsonObject{{"supported",false},{"reason",why},{"orders",QJsonArray{}}};
        };
        if(forecastPolicyKey.isEmpty() || !projection["projectedDay"].isDouble() || !projection["player"].isDouble())
            return unavailable(QStringLiteral("missing factory program phase identity"));
        projection["includeContinuationState"]=true;
        auto result=buyer.previewCounterpointOpening(projection);
        if(!result["supported"].toBool()) return result;
        const auto states=result["continuationState"].toArray();
        if(states.size()!=1) return unavailable(QStringLiteral("missing ordinary buyer state"));
        const auto state=QJsonDocument::fromJson(states.at(0).toString().toUtf8()).object();
        if(state["playerId"]!=projection["player"] || state["day"]!=projection["projectedDay"] ||
           result["projectedDay"]!=state["day"] || state["seed"]!=result["seed"] ||
           (future && (result["seedMode"]!="common-supplied" || projection["comparisonSeed"]!=result["seed"])))
            return unavailable(QStringLiteral("buyer date, seat or stream mismatch"));
        auto hash=[](const QJsonDocument &doc){
            return QString::fromLatin1(QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
        };
        result["startingFunds"]=projection["funds"];
        result["player"]=state["playerId"];
        result["sourceBoardSHA256"]=hash(QJsonDocument(projection));
        result["phaseStateSHA256"]=hash(QJsonDocument(states));
        result["forecastPolicyKey"]=forecastPolicyKey;
        result["policy"]=future?"PREDECLARED_ORDINARY_BUYER":"CURRENT_ORDINARY_BUYER";
        return result;
    }
}
