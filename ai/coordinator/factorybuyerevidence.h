#pragma once

#include "ai/coordinator/factoryprogrameconomy.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Coordinator
{
    // Confirm that this exact ordinary queue supplied the purchases whose
    // later jobs were priced. A different queue requires repricing that arm.
    // No numerical ledger data or native controller state is overwritten.
    inline QString bindFactoryProgramBuyer(FactoryEconomy::Phase &phase,QJsonObject projection,
        const QString &policyKey,quint32 expectedSeed,const QJsonObject &quote,
        const std::map<std::pair<int,int>,int> &sites,const QJsonObject &actualOpeningStream={})
    {
        const auto digest=[](const QJsonDocument &doc){
            return QString::fromLatin1(QCryptographicHash::hash(doc.toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256).toHex());
        };
        const auto currency=[](const QJsonValue &v)->std::optional<MilliFunds>{
            if(!v.isDouble())return {};
            const auto funds=v.toInteger(-1);
            if(funds<0 || funds>std::numeric_limits<MilliFunds>::max()/MILLI_FUNDS_PER_FUND || v.toDouble()!=double(funds))
                return {};
            return funds*MILLI_FUNDS_PER_FUND;
        };
        const bool future=projection.contains("futureDay");
        projection["includeContinuationState"]=true;
        if(!quote["supported"].toBool())return "NATIVE_BUYER_UNAVAILABLE_"+quote["reason"].toString();
        if(policyKey.isEmpty() || quote["forecastPolicyKey"]!=policyKey || !projection["projectedDay"].isDouble() ||
           !projection["player"].isDouble() || quote["projectedDay"]!=projection["projectedDay"] ||
           quote["player"]!=projection["player"] || quote["seed"].toInteger(-1)!=expectedSeed ||
           quote["sourceBoardSHA256"]!=digest(QJsonDocument(projection)))
            return "BUYER_PHASE_OR_ROOT_CHANGED";
        if(future){
            if(phase.tick<=0 || projection["comparisonSeed"].toInteger(-1)!=expectedSeed ||
               projection["projectedDay"]!=projection["futureDay"] || quote["seedMode"]!="common-supplied" ||
               quote["policy"]!="PREDECLARED_ORDINARY_BUYER")return "UNBOUND_COMMON_FUTURE_BUYER";
        }else if(phase.tick!=0 || quote["policy"]!="CURRENT_ORDINARY_BUYER")return "UNBOUND_ACTUAL_OPENING_BUYER";
        const auto state=quote["continuationState"].toArray();
        if(state.size()!=1 || !state[0].isString() || quote["phaseStateSHA256"]!=digest(QJsonDocument(state)))
            return "UNBOUND_BUYER_CONTINUATION_STATE";
        const auto persisted=QJsonDocument::fromJson(state[0].toString().toUtf8()).object();
        if(persisted["day"]!=projection["projectedDay"] || persisted["playerId"]!=quote["player"] ||
           persisted["seed"]!=quote["seed"] || !persisted["ordinaryPrepared"].toBool() ||
           !quote["drawCounter"].isDouble() || persisted["drawCounter"]!=quote["drawCounter"] ||
           quote["preparedDrawCounter"].toInteger(-1)<0 ||
           quote["preparedDrawCounter"].toInteger()>quote["drawCounter"].toInteger(-1))return "NATIVE_QUEUE_STATE_CHANGED";
        if(!future && quote["seedMode"]!="reserved-current-stream"){
            const auto mode=quote["seedMode"].toString();
            if((mode!="saved-current-stream" && mode!="buyer-derived") ||
               actualOpeningStream["mode"]!=mode || phase.before.empty() ||
               actualOpeningStream["observation"].toString().toStdString()!=phase.before ||
               actualOpeningStream["day"]!=persisted["day"] || actualOpeningStream["player"]!=persisted["playerId"] ||
               actualOpeningStream["seed"]!=persisted["seed"] || !persisted["generation"].isDouble() ||
               actualOpeningStream["generation"]!=persisted["generation"])return "UNBOUND_LIVE_OR_DERIVED_BUYER_STREAM";
            if(mode=="saved-current-stream"){
                const auto current=actualOpeningStream["persistedState"].toArray();
                if(current.size()!=1 || !current[0].isString())return "MISSING_CURRENT_BUYER_STATE";
                const auto native=QJsonDocument::fromJson(current[0].toString().toUtf8()).object();
                if(native["day"]!=persisted["day"] || native["playerId"]!=persisted["playerId"] ||
                   native["seed"]!=persisted["seed"] || native["generation"]!=persisted["generation"])
                    return "CURRENT_BUYER_GENERATION_CHANGED";
            }else if(!actualOpeningStream["nativeDeterministicMode"].toBool() ||
                     !actualOpeningStream["algorithmVersion"].isDouble() ||
                     actualOpeningStream["algorithmVersion"]!=persisted["algorithmVersion"])
                return "MISSING_NATIVE_SEED_DERIVATION";
        }
        auto wallet=currency(quote["startingFunds"]);
        if(!wallet || quote["startingFunds"]!=projection["funds"] || phase.startWallet!=*wallet ||
           !quote["orders"].isArray() || quote["orders"].toArray().size()!=int(phase.purchases.size()) ||
           !projection["unitSlots"].isDouble() || projection["unitSlots"].toInt(-1)<int(phase.purchases.size()))
            return "NATIVE_WALLET_OR_PURCHASE_COUNT_CHANGED";
        std::set<std::pair<int,int>> occupied,owned,paid;
        std::set<int> bodies;
        if(!projection["units"].isArray() || !projection["properties"].isArray())return "MISSING_BUYER_BOARD";
        for(const auto &v:projection["units"].toArray()){
            const auto u=v.toObject();const int id=u["id"].toInt(-1);
            if(id<0 || !bodies.insert(id).second || !u["hp"].isDouble())return "INVALID_PROJECTED_ROSTER";
            if(u["hp"].toDouble()>0)occupied.emplace(u["x"].toInt(-1),u["y"].toInt(-1));
        }
        for(const auto &v:projection["properties"].toArray()){
            const auto b=v.toObject();
            if(b["owner"]==projection["player"])owned.emplace(b["x"].toInt(-1),b["y"].toInt(-1));
        }
        const auto orders=quote["orders"].toArray();
        for(int i=0;i<orders.size();++i){
            const auto order=orders[i].toObject();
            const auto xy=std::pair{order["x"].toInt(-1),order["y"].toInt(-1)};
            const auto found=sites.find(xy);const auto amount=currency(order["price"]);
            const auto &b=phase.purchases[i];
            if(found==sites.end() || !owned.contains(xy) || occupied.contains(xy) || !paid.insert(xy).second ||
               b.site!=found->second || b.actor<0 || !bodies.insert(b.actor).second ||
               b.type.empty() || order["type"].toString().toStdString()!=b.type || !amount ||
               b.price!=*amount || b.nativeInitialCapital!=*amount || b.price>*wallet)
                return "ORDINARY_PURCHASE_REQUIRES_DIFFERENT_TEAM_OR_RECRUIT_JOB";
            *wallet-=b.price;
            if(*wallet!=b.walletAfter)return "NATIVE_TRANSACTION_ORDER_OR_FUNDS_CHANGED";
        }
        const auto end=currency(quote["remainingFunds"]);
        if(!end || *end!=*wallet || phase.endWallet!=*end)return "NATIVE_CLOSING_WALLET_CHANGED";
        const auto policy=QJsonArray{policyKey,quote["policy"],quote["player"],quote["projectedDay"],quote["seed"]};
        phase.buyer=future?FactoryEconomy::BuyerEvidence::PredeclaredOrdinaryPolicy:FactoryEconomy::BuyerEvidence::LiveReservation;
        phase.buyerPolicy=digest(QJsonDocument(policy)).toStdString();
        phase.buyerReceipt=digest(QJsonDocument(quote)).toStdString();
        return {};
    }
}
