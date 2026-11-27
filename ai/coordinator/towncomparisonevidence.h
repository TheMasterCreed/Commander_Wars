#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace Coordinator
{
    // Target, attacking side, capper's engine ID, intended entry tick.
    using TownIntention=std::array<int,5>;
    using TownSlot=std::array<int,3>;

    inline QJsonObject describeTownIntention(const TownIntention &i)
    {
        return {{"tile",QJsonArray{i[0],i[1]}},{"attackingSide",i[2]},{"capper",i[3]},{"entryTick",i[4]}};
    }

    inline std::optional<TownIntention> readTownIntention(const QJsonObject &o)
    {
        const auto tile=o.value("tile").toArray();
        if (tile.size()!=2) return {};
        const TownIntention i{tile[0].toInt(-1),tile[1].toInt(-1),o.value("attackingSide").toInt(-1),
                              o.value("capper").toInt(-1),o.value("entryTick").toInt(-1)};
        if (i[0]<0 || i[1]<0 || i[2]<0 || i[2]>1 || i[3]<0 || i[4]<0) return {};
        return i;
    }

    using StockChallengeMenu=std::map<QString,std::set<TownIntention>>;

    inline void mergeStockChallenges(StockChallengeMenu &into,const StockChallengeMenu &other)
    {
        for(const auto &[policy,keys]:other)if(!keys.empty())
            into[policy].insert(keys.begin(),keys.end());
    }

    inline QJsonArray describeStockChallenges(const StockChallengeMenu &menu)
    {
        QJsonArray items;
        for(const auto &[policy,keys]:menu)for(const auto &i:keys)
        {auto row=describeTownIntention(i);row["replyPolicy"]=policy;items.append(row);}
        return items;
    }

    inline std::set<TownIntention> stockWitnesses(const QJsonObject &row)
    {
        std::set<TownIntention> result;
        for(const auto &v:row["settledStockIntentions"].toArray())
            if(const auto key=readTownIntention(v.toObject()))result.insert(*key);
        return result;
    }

    inline StockChallengeMenu stockIntentionsByReply(const QJsonObject &plan)
    {
        StockChallengeMenu out;
        const auto add=[&](const QJsonObject &row){
            const auto policy=row["replyPolicy"].toString();
            if(policy.isEmpty())return;
            const auto facts=stockWitnesses(row);
            out[policy].insert(facts.begin(),facts.end());
        };
        add(plan);
        for(const auto &value:plan["replyAlternatives"].toArray())add(value.toObject());
        return out;
    }

    // The same physical transfer in the two arms is already paid for in
    // both. Only a real disagreement on the same opponent policy needs a
    // matched stock allowance; town attack intentions use their own gate.
    inline StockChallengeMenu additionalStockChallenges(const QJsonObject &left,const QJsonObject &right,
                                                        const StockChallengeMenu &already={})
    {
        const auto a=stockIntentionsByReply(left),b=stockIntentionsByReply(right);
        StockChallengeMenu pending;
        const auto gather=[&](const auto &from,const auto &other){
            for(const auto &[policy,keys]:from)
            {
                const auto peer=other.find(policy),old=already.find(policy);
                for(const auto &key:keys)
                    if((peer==other.end() || !peer->second.contains(key)) &&
                       (old==already.end() || !old->second.contains(key)))pending[policy].insert(key);
            }
        };
        gather(a,b);gather(b,a);return pending;
    }

    inline bool namedTownAnswer(const QJsonArray &coverage,const TownIntention &intent)
    {
        for(const auto &v:coverage)
        {
            const auto row=v.toObject();const auto at=row["tile"].toArray();
            if(at!=QJsonArray{intent[0],intent[1]} || row["attackingSide"].toInt(-1)!=intent[2] ||
               row["capperUnit"].toInt(-1)!=intent[3] || row["requiredEntryTick"].toInt(-1)!=intent[4] ||
               !row["required"].toBool() || !row["complete"].toBool())continue;
            const auto status=row["status"].toString();
            // An economic projection ending early is not by itself a
            // physically forced terminal. The exact E0 policy handles that
            // exemption separately when it carries a witnessed terminal rank.
            if(row["reason"]=="AFTER_CUTOFF")continue;
            if(status=="BASELINE" || status=="UNAVAILABLE" ||
               (status=="PRICED" && row["exhaustedActorIds"].toArray().contains(intent[3])))return true;
        }
        return false;
    }

    inline QJsonArray matchStockAnswers(const QJsonArray &duties,const QJsonArray &coverage)
    {
        QJsonArray result;
        for(const auto &v:duties)
        {
            const auto d=v.toObject();if(!d["matchedStockIntention"].toBool())continue;
            const auto at=d["tile"].toArray();const int date=d["entryTick"].toInt(-1);
            if(at.size()!=2 || date<0)continue;
            const TownIntention i{at[0].toInt(),at[1].toInt(),date%2,d["unit"].toInt(-1),date};
            auto row=describeTownIntention(i);
            const bool stock=d["exactRetainedStockTransfer"].toBool(),named=!stock && namedTownAnswer(coverage,i);
            row["stockAllowance"]=d["equalOffer"];
            row["complete"]=stock || named;
            row["basis"]=stock?"SETTLED_STOCK_TRANSFER":named?"NAMED_TOWN_ALTERNATIVE":"UNVERIFIED_STOCK_OR_RETASK";
            result.append(row);
        }
        return result;
    }

    inline bool stockComparisonCovered(const QJsonObject &left,const QJsonObject &right,const StockChallengeMenu &required,
                                       QJsonArray *gaps=nullptr)
    {
        struct Arm
        {
            std::map<std::pair<QString,TownIntention>,qint64> offers;
            std::set<std::pair<QString,TownIntention>> answered;
            std::map<QString,int> ends;
            std::set<QString> policies;
            bool endedBeforeReply{};
            bool ended(const QString &policy,int entry) const
            {
                const auto i=ends.find(policy);
                return endedBeforeReply || (i!=ends.end() && i->second<=entry);
            }
        };
        const auto receipts=[](const QJsonObject &plan){
            Arm out;
            const auto add=[&](const QJsonObject &p){
                const auto policy=p["replyPolicy"].toString();
                if(!policy.isEmpty())out.policies.insert(policy);
                const int end=p["projectedTerminalHalfTurn"].toInt(-1);
                if(p["priceStatus"]=="WITNESSED" && p["terminalClass"].toInt()!=0 && end>=0)
                {
                    out.ends[policy]=end;
                    if(end==0 && p["hqStatus"]=="CAPTURE_WINS_NOW")out.endedBeforeReply=true;
                }
                for(const auto &v:p["matchedStockChecks"].toArray())
                {
                    const auto row=v.toObject();const auto i=readTownIntention(row);
                    const auto offer=row["stockAllowance"].toInteger(-1);
                    if(!i || offer<0)continue;
                    const auto key=std::pair{policy,*i};
                    out.offers.try_emplace(key,offer);
                    if(row["complete"].toBool() && offer>0)
                    {out.offers[key]=offer;out.answered.insert(key);}
                }
            };
            add(plan);
            for(const auto &r:plan["replyAlternatives"].toArray())add(r.toObject());
            return out;
        };
        if(required.empty())return true;
        const auto a=receipts(left),b=receipts(right);
        bool complete=true;
        for(const auto &[policy,keys]:required)for(const auto &i:keys)
        {
            const auto x=a.offers.find({policy,i}),y=b.offers.find({policy,i});
            const bool cutA=a.ended(policy,i[4]),cutB=b.ended(policy,i[4]);
            const bool missingA=!cutA && !a.answered.contains({policy,i});
            const bool missingB=!cutB && !b.answered.contains({policy,i});
            if(!missingA && !missingB && (cutA || cutB || x->second==y->second))continue;
            if(!gaps)return false;
            complete=false;
            auto row=describeTownIntention(i);row["replyPolicy"]=policy;
            row["reason"]=!a.policies.contains(policy) || !b.policies.contains(policy)?"OPPONENT_POLICY_NOT_PAIRED":
                missingA || missingB?"STOCK_OR_NAMED_PRICE_OUTSTANDING":"UNEQUAL_STOCK_ALLOWANCES";
            row["leftAnswered"]=!missingA;row["rightAnswered"]=!missingB;
            row["leftAllowance"]=x==a.offers.end()?QJsonValue(QJsonValue::Null):QJsonValue(x->second);
            row["rightAllowance"]=y==b.offers.end()?QJsonValue(QJsonValue::Null):QJsonValue(y->second);
            gaps->append(row);
        }
        return complete;
    }

    struct TownComparisonEvidence
    {
        bool complete{true};
        std::size_t unresolvedEvents{};
        QJsonArray unpricedChoices;

        void noteUnknown() {complete=false;++unresolvedEvents;}
        void deferChoice(QString decision,QString reason,const QJsonArray &achievable={},const QJsonArray &attackCeiling={})
        {
            noteUnknown();
            QJsonObject gap{{"decision",decision},{"reason",reason}};
            if(!achievable.isEmpty())gap["knownChoiceFloor"]=achievable;
            if(!attackCeiling.isEmpty())gap["attackUpperBound"]=attackCeiling;
            unpricedChoices.append(gap);
        }

        std::set<TownIntention> intentions;
        std::set<TownIntention> requiredChallenges;
        std::set<TownSlot> unfinished;

        void observe(const QJsonArray &coverage,const QJsonArray &conflicts,
                     const std::set<TownIntention> &required={},int terminalTick=-1)
        {
            requiredChallenges.insert(required.begin(),required.end());
            if (!conflicts.isEmpty()) noteUnknown();
            std::set<TownIntention> answered;
            for (const auto &value:coverage)
            {
                const auto row=value.toObject();
                const auto tile=row.value("tile").toArray();
                if (tile.size()!=2) {noteUnknown();continue;}
                if (!row.value("complete").toBool())
                {
                    noteUnknown();
                    unfinished.insert({tile[0].toInt(),tile[1].toInt(),row.value("attackingSide").toInt(-1)});
                }
                else if (row.value("required").toBool())
                {
                    const auto state=row.value("status").toString();
                    const auto actor=row.value("capperUnit").toInt(-1);
                    const auto examined=row.value("exhaustedActorIds").toArray();
                    const bool named=examined.contains(actor);
                    if (state=="UNAVAILABLE" || state=="BASELINE" || (state=="PRICED" && named))
                        answered.insert({tile[0].toInt(),tile[1].toInt(),row.value("attackingSide").toInt(-1),
                                         actor,row.value("requiredEntryTick").toInt(-1)});
                }
                for (const auto &v:row.value("intentions").toArray())
                    if (auto i=readTownIntention(v.toObject())) intentions.insert(*i);
            }
            for (const auto &i:required)
                if ((terminalTick<0 || i[4]<terminalTick) && !answered.contains(i))
                {
                    noteUnknown();
                    unfinished.insert({i[0],i[1],i[2]});
                }
        }

        QJsonObject record() const
        {
            QJsonArray witnesses,open;
            for (const auto &i:intentions) witnesses.append(describeTownIntention(i));
            for (const auto &s:unfinished) open.append(QJsonArray{s[0],s[1],s[2]});
            QJsonArray certified;
            if(complete)for(const auto &i:requiredChallenges)certified.append(describeTownIntention(i));
            QJsonObject out{{"complete",complete},{"intentions",witnesses},{"checkedChallenges",certified},{"unfinishedSlots",open}};
            if(!unpricedChoices.isEmpty())out["unpricedContinuationChoices"]=unpricedChoices;
            return out;
        }
    };

    inline std::map<QString,std::set<TownIntention>> quotedTownIntentions(const QJsonObject &plan)
    {
        std::map<QString,std::set<TownIntention>> out;
        for (const auto &v:plan.value("replyAlternatives").toArray())
        {
            const auto row=v.toObject();
            auto &reply=out[row.value("replyPolicy").toString()];
            for (const auto &i:row.value("townComparisonEvidence").toObject().value("intentions").toArray())
                if (const auto key=readTownIntention(i.toObject())) reply.insert(*key);
        }
        return out;
    }

    inline std::set<TownIntention> additionalTownChallenges(const QJsonObject &left,const QJsonObject &right,
                                                             const std::set<TownIntention> &alreadyRequired={})
    {
        const auto a=quotedTownIntentions(left),b=quotedTownIntentions(right);
        std::set<TownIntention> pending;
        const auto consider=[&](const auto &from,const auto &other) {
            for (const auto &[policy,intentions]:from)
            {
                const auto peer=other.find(policy);
                for (const auto &intent:intentions)
                    if ((peer==other.end() || !peer->second.contains(intent)) && !alreadyRequired.contains(intent))
                        pending.insert(intent);
            }
        };
        consider(a,b);
        consider(b,a);
        return pending;
    }

    struct SharedTownReply
    {
        bool complete{};
        std::set<TownIntention> discovered,answered,earnedStock;
        std::map<TownIntention,qint64> stockOffers;
    };

    // A full economic point needs the whole continuation. Independently
    // witnessed one-sided and immediate terminal certificates have their
    // own evidence and do not derive money from this test.
    inline bool hasOpenFinancialContinuation(const QJsonObject &row)
    {
        const int terminal=row.value("terminalClass").toInt();
        if(terminal==-1 || terminal==1)return false;
        return (row.contains("continuationRangeInputsComplete") && !row.value("continuationRangeInputsComplete").toBool()) ||
               (row.contains("observedStockComplete") && !row.value("observedStockComplete").toBool()) ||
               !row.value("unconfirmedFactoryReleases").toArray().isEmpty() ||
               !row.value("unpricedFinancialInputs").toObject().isEmpty();
    }

    inline SharedTownReply readSharedTownReply(const QJsonObject &reply)
    {
        const auto evidence=reply.value("townComparisonEvidence").toObject();
        SharedTownReply result;
        result.complete=evidence.value("complete").toBool() && reply.contains("policyTotal") &&
            !reply.value("policyTotal").isNull() && !hasOpenFinancialContinuation(reply);
        for(const auto &v:evidence.value("intentions").toArray())
            if(auto i=readTownIntention(v.toObject()))result.discovered.insert(*i);
        result.earnedStock=stockWitnesses(reply);
        for(const auto &v:reply["matchedStockChecks"].toArray())
        {
            const auto row=v.toObject();const auto i=readTownIntention(row);const auto fee=row["stockAllowance"].toInteger(-1);
            if(i && fee>0 && row["complete"].toBool())result.stockOffers.emplace(*i,fee);
        }
        if(result.complete)
        {
            result.answered=result.discovered;
            for(const auto &v:evidence.value("checkedChallenges").toArray())
                if(auto i=readTownIntention(v.toObject()))result.answered.insert(*i);
        }
        return result;
    }

    // A feasible reply bounds this particular squad's worst outcome. It may
    // reject that squad only against an incumbent which also faced the known
    // challenge. Never allow a poor partial scalar to stand in for the witness.
    template<class Rank> class SharedReplyBound
    {
    public:
        explicit SharedReplyBound(const QJsonObject *incumbent)
        {
            if(!incumbent || incumbent->value("priceStatus")!="WITNESSED")return;
            for(const auto &v:incumbent->value("replyAlternatives").toArray())
            {
                const auto r=v.toObject();
                m_peer[r.value("replyPolicy").toString()]=readSharedTownReply(r);
            }
        }

        void observe(const QJsonObject &record,const Rank &rank) {observe(record,&rank);}
        void observeOpen(const QJsonObject &record) {observe(record,nullptr);}
    private:
        void observe(const QJsonObject &record,const Rank *rank)
        {
            const auto name=record.value("replyPolicy").toString();
            const auto mine=readSharedTownReply(record);
            const auto it=m_peer.find(name);
            if(it==m_peer.end() || !it->second.complete)
            {
                m_allMatched=false;
                if(!mine.discovered.empty() || !mine.earnedStock.empty())m_newForIncumbent=true;
                return;
            }
            const auto &peer=it->second;
            if(!std::includes(peer.answered.begin(),peer.answered.end(),mine.discovered.begin(),mine.discovered.end()))
                m_newForIncumbent=true;
            const bool matched=mine.complete && std::includes(mine.answered.begin(),mine.answered.end(),
                peer.discovered.begin(),peer.discovered.end());
            auto knownStock=[](const SharedTownReply &from,const SharedTownReply &other,const TownIntention &i){
                if(other.earnedStock.contains(i))return true;
                const auto a=from.stockOffers.find(i),b=other.stockOffers.find(i);
                return a!=from.stockOffers.end() && b!=other.stockOffers.end() && a->second==b->second;
            };
            for(const auto &i:mine.earnedStock)if(!knownStock(mine,peer,i))m_newForIncumbent=true;
            const bool stockMatched=std::all_of(peer.earnedStock.begin(),peer.earnedStock.end(),
                [&](const auto &i){return knownStock(peer,mine,i);});
            m_allMatched&=matched && stockMatched;
            if(rank && matched && stockMatched && (!m_upper || *rank<*m_upper)){m_upper=*rank;m_witness=name;}
        }
    public:
        bool hasMatchedUpper() const {return !m_newForIncumbent && m_upper.has_value();}
        bool rejects(const Rank &incumbent) const
        {return hasMatchedUpper() && *m_upper<incumbent;}
        template<class Enumerate> bool coversUniverse(Enumerate enumerate) const
        {
            if(m_peer.empty())return false;
            for(const auto &[name,reply]:m_peer)
            {
                if(!reply.complete || !enumerate([&](const TownIntention &i){
                    return reply.answered.contains(i);}))return false;
            }
            return true;
        }
        bool allMatched() const {return m_allMatched && !m_newForIncumbent;}
        const QString &witness() const {return m_witness;}
        const Rank &upper() const {return *m_upper;}
    private:
        std::map<QString,SharedTownReply> m_peer;
        std::optional<Rank> m_upper;
        QString m_witness;
        bool m_allMatched{true},m_newForIncumbent{};
    };

    template<class Rank> bool passiveBoundsOptionalAttack(bool passiveKnown,const Rank &passive,
        bool unpunishedKnown,const Rank &unpunished)
    {
        // The opponent can decline punishment. Thus our attack cannot
        // guarantee more than its value on this actual, unpunished reply.
        return passiveKnown && unpunishedKnown && unpunished<=passive;
    }

    inline void recordEconomicEvidence(QJsonObject &record,bool complete)
    {
        complete=complete && record.contains("policyTotal") && !hasOpenFinancialContinuation(record);
        record["priceStatus"]=complete ? "WITNESSED" : "UNRESOLVED";
        if (!complete && record.contains("policyTotal")) record["partialPolicyTotal"]=record.take("policyTotal");
        if (complete) record.remove("partialPolicyTotal");
    }
}
