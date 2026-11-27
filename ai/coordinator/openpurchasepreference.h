#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace Coordinator
{
    using OpenPurchaseCell=std::pair<int,int>;

    struct OpenPurchaseSite
    {
        bool factory{};
        int incumbentOccupant{-2},challengerOccupant{-2}; // -1 is a confirmed empty tile.
    };
    struct OpenPurchaseBoard
    {
        bool complete{};
        std::map<OpenPurchaseCell,OpenPurchaseSite> sites;
        std::array<QByteArray,2> buyerStart;
    };
    struct OpenPurchaseVetoes
    {
        bool checked{},incumbentDefence{},incumbentHigherPriority{},challengerHeadquarters{};
    };
    struct OpenPurchasePreference
    {
        bool eligible{};
        QString reason;
        QJsonArray releasedPurchases,incumbentPurchases,challengerPurchases;
    };

    namespace OpenPurchaseDetail
    {
        inline std::optional<OpenPurchaseCell> cell(const QJsonArray &a)
        {
            if(a.size()!=2)return {};
            const int x=a[0].toInt(-1),y=a[1].toInt(-1);
            if(x<0 || y<0)return {};
            return OpenPurchaseCell{x,y};
        }

        struct Arm
        {
            QJsonArray buys;
            std::map<OpenPurchaseCell,QJsonObject> boughtAt;
            std::map<OpenPurchaseCell,int> waits;
            std::map<int,OpenPurchaseCell> destinations;
            qint64 cash{},seed{};
        };

        inline std::optional<Arm> readArm(const QJsonObject &receipt,const OpenPurchaseBoard &board,int side)
        {
            const auto team=receipt.value("physicalTeam").toObject(),obs=receipt.value("economicObservation").toObject();
            const auto boundary=receipt.value("purchaseBoundary").toObject();
            if(obs.value("board").toString().isEmpty() || team.value("purchaseBoundary")!=boundary)return {};
            for(auto field:{"day","player","horizon","planSequence"})
                if(obs.value(field).toInteger(-1)<0 || team.value(field)!=obs.value(field))return {};
            const auto order=team.value("executionOrder").toArray(),actions=team.value("orders").toArray();
            if(actions.isEmpty() || order.size()!=actions.size())return {};
            Arm arm;
            std::set<int> units,indices;
            for(qsizetype i=0;i<actions.size();++i){
                const auto a=actions[i].toObject();
                const int unit=a.value("unit").toInt(-1),index=a.value("actionIndex").toInt(-1);
                const auto at=cell(a.value("to").toArray());
                if(!at || unit<0 || index<0 || order[i].toInt(-1)!=index ||
                   !units.insert(unit).second || !indices.insert(index).second || a.value("action").toString().isEmpty())return {};
                arm.destinations.emplace(unit,*at);
                if(a.value("action")=="ACTION_WAIT")arm.waits.emplace(*at,unit);
            }
            const auto source=boundary.value("openingSource").toString();
            if(source!="COUNTERPOINT_PREVIEW" && source!="NO_FUNDED_SLOT")return {};
            const auto buyer=boundary.value("buyerPreview").toObject();
            if(!buyer.value("supported").toBool() || buyer.value("seedMode")!="reserved-current-stream" ||
               !buyer.value("orders").isArray() || !boundary.value("modeledOpening").isArray() ||
               !boundary.value("openAffordableSites").isArray())return {};
            arm.cash=boundary.value("spendableFunds").toInteger(-1);
            arm.seed=buyer.value("seed").toInteger(-1);
            const auto capacity=boundary.value("affordableBuildSlots").toInteger(-1);
            if(arm.seed<0 || arm.cash<0 || arm.cash%1000 || capacity<0)return {};
            qint64 left=arm.cash/1000;
            arm.buys=buyer.value("orders").toArray();
            if(arm.buys.size()>capacity || (source=="NO_FUNDED_SLOT" && (capacity || !arm.buys.isEmpty())))return {};
            std::set<OpenPurchaseCell> affordable;
            for(const auto &value:boundary.value("openAffordableSites").toArray()){
                const auto at=cell(value.toArray());
                if(!at || !affordable.insert(*at).second)return {};
            }
            std::map<OpenPurchaseCell,std::pair<QString,qint64>> paid;
            for(const auto &value:arm.buys){
                const auto b=value.toObject();
                const auto at=cell({b.value("x"),b.value("y")});
                const auto cost=b.value("price").toInteger(-1);
                const auto type=b.value("type").toString();
                if(!at || !affordable.contains(*at) || cost<=0 || cost>left || type.isEmpty())return {};
                const auto it=board.sites.find(*at);
                if(it==board.sites.end() || (side?it->second.challengerOccupant:it->second.incumbentOccupant)!=-1 ||
                   !arm.boughtAt.emplace(*at,b).second)return {};
                paid.emplace(*at,std::pair{type,cost*1000});
                left-=cost;
            }
            if(left!=buyer.value("remainingFunds").toInteger(-1))return {};
            // The forecast sorts its paid orders independently of the buyer.
            // Verify each tile, unit and exact price while keeping the buyer's own order for reporting.
            for(const auto &value:boundary.value("modeledOpening").toArray()){
                const auto b=value.toObject();
                const auto at=cell(b.value("factory").toArray());
                if(!at)return {};
                const auto it=paid.find(*at);
                if(it==paid.end() || it->second.first!=b.value("type").toString() || it->second.second!=b.value("price").toInteger(-1))return {};
                paid.erase(it);
            }
            if(!paid.empty())return {};
            return arm;
        }
    }

    // This selection is available only while the formula is open. It has no score,
    // and the caller must bind it again to the orders and buyer it actually publishes.
    inline OpenPurchasePreference evaluateOpenPurchasePreference(const QJsonObject &incumbent,const QJsonObject &challenger,
        const QJsonObject &decision,const OpenPurchaseBoard &board,const OpenPurchaseVetoes &vetoes)
    {
        OpenPurchasePreference result;
        const auto stop=[&](const char *s){result.reason=QString::fromLatin1(s);return result;};
        if(decision.value("status")!="INSUFFICIENT_BOUNDS" || decision.contains("preferred"))return stop("FORMULA_DECISION_HAS_PRIORITY");
        if(!vetoes.checked || vetoes.incumbentDefence || vetoes.incumbentHigherPriority || vetoes.challengerHeadquarters)
            return stop("DEFENSE_OR_HIGHER_PRIORITY_OUTCOME");
        const auto arms=decision.value("arms").toArray();
        if(arms.size()!=2)return stop("UNBOUND_COMPARISON");
        const auto adverse=arms[1].toObject();
        const auto upper=adverse.value("upper").toArray();
        if(!adverse.value("openingLossProof").toObject().isEmpty() || (!upper.isEmpty() && upper[0].toInt()==-1))
            return stop("CHALLENGER_HAS_CERTIFIED_TERMINAL_LOSS");
        const auto lower=arms[0].toObject().value("lower").toArray();
        const auto rank=[](const QJsonArray &v){
            return std::tuple{v[0].toInt(),v[1].toInt(),v[2].toInt(),v[3].toInteger()};
        };
        if(lower.size()==4 && upper.size()==4 && rank(lower)>rank(upper))return stop("INCUMBENT_CERTIFIED_ADVANTAGE");
        if(!board.complete || board.buyerStart[0].isEmpty() || board.buyerStart[0]!=board.buyerStart[1] ||
           incumbent.value("economicObservation")!=challenger.value("economicObservation"))return stop("UNBOUND_REPLAY_OR_BUYER_START");
        if(decision.value("bindings").toArray()!=QJsonArray{incumbent.value("physicalTeam"),challenger.value("physicalTeam")})
            return stop("STALE_TEAM_BINDINGS");
        const auto before=OpenPurchaseDetail::readArm(incumbent,board,0),after=OpenPurchaseDetail::readArm(challenger,board,1);
        if(!before || !after || before->cash!=after->cash || before->seed!=after->seed)return stop("UNVERIFIED_COUNTERPOINT_PORTFOLIOS");
        result.incumbentPurchases=before->buys;result.challengerPurchases=after->buys;
        if(after->buys.size()<=before->buys.size())return stop("NO_ADDITIONAL_PURCHASE");
        for(const auto &[tile,occupants]:board.sites){
            const int old=occupants.incumbentOccupant,now=occupants.challengerOccupant;
            if(old< -1 || now< -1)return stop("UNKNOWN_FINAL_OCCUPANT");
            if(!occupants.factory)continue;
            if(old==-1 && now>=0)return stop("NEW_FACTORY_BLOCK");
            if(old<0 || now>=0)continue;
            const auto wait=before->waits.find(tile);
            const auto buy=after->boughtAt.find(tile);
            const auto moved=after->destinations.find(old);
            if(wait!=before->waits.end() && wait->second==old && buy!=after->boughtAt.end() &&
               moved!=after->destinations.end() && moved->second!=tile){
                auto b=buy->second;b["previousWaitingUnit"]=old;result.releasedPurchases.append(b);
            }
        }
        if(result.releasedPurchases.isEmpty())return stop("NO_PURCHASE_ON_A_RELEASED_WAIT");
        result.eligible=true;result.reason="OPEN_ADDITIONAL_PURCHASE_ON_RELEASED_FACTORY";return result;
    }
}
