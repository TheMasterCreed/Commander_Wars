#pragma once

#include "ai/coordinator/towncomparisonevidence.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace Coordinator
{
    // Only open questions and their search allowances cross a process boundary.
    // Every monetary value and decision endpoint needs a fresh native replay.
    struct PairDiscoveryCheckpoint
    {
        std::set<TownIntention> towns;
        StockChallengeMenu stock;
        std::set<std::array<int,4>> duties;
        std::array<qint64,3> efforts{};
        QJsonObject fundedPosts;
        QJsonObject movementCuts;

        QJsonObject record(const QString &binding) const
        {
            QJsonArray requested,legs;
            for(const auto &i:towns)requested.append(describeTownIntention(i));
            for(const auto &d:duties)legs.append(QJsonArray{d[0],d[1],d[2],d[3]});
            return {{"version",movementCuts.isEmpty()?2:3},{"f1MovementCuts",movementCuts},{"binding",binding},{"fundedPosts",fundedPosts},{"towns",requested},{"stock",describeStockChallenges(stock)},
                {"duties",legs},{"efforts",QJsonArray{efforts[0],efforts[1],efforts[2]}}};
        }
    };

    template<class Valid> std::optional<PairDiscoveryCheckpoint> readPairDiscovery(const QJsonObject &row,
        const QString &binding,qint64 maximumEffort,Valid valid)
    {
        if(binding.isEmpty() || row["binding"]!=binding || (row["version"].toInt()!=2 && row["version"].toInt()!=3) || !row["fundedPosts"].isObject() ||
           !row["towns"].isArray() || !row["stock"].isArray() || !row["duties"].isArray())return {};
        PairDiscoveryCheckpoint out;
        const auto efforts=row["efforts"].toArray();
        if(efforts.size()!=3)return {};
        for(int i=0;i<3;++i)
        {
            out.efforts[i]=efforts[i].toInteger(-1);
            if(out.efforts[i]<1 || out.efforts[i]>maximumEffort)return {};
        }
        for(const auto &v:row["towns"].toArray())
        {
            const auto i=readTownIntention(v.toObject());
            if(!i || !valid(*i) || !out.towns.insert(*i).second)return {};
        }
        for(const auto &v:row["stock"].toArray())
        {
            const auto item=v.toObject();const auto i=readTownIntention(item);
            const auto policy=item["replyPolicy"].toString();
            if(!i || !valid(*i) || policy.isEmpty() || !out.stock[policy].insert(*i).second)return {};
        }
        for(const auto &v:row["duties"].toArray())
        {
            const auto a=v.toArray();
            if(a.size()!=4)return {};
            std::array<int,4> d{a[0].toInt(-1),a[1].toInt(-1),a[2].toInt(-1),a[3].toInt(-1)};
            if(!valid(TownIntention{d[1],d[2],d[3]%2,d[0],d[3]}) || !out.duties.insert(d).second)return {};
        }
        const auto posts=row["fundedPosts"].toObject();
        for(auto it=posts.begin();it!=posts.end();++it)
        {
            if(it.key().isEmpty() || !it.value().isArray())return {};
            for(const auto &value:it.value().toArray())
            {
                const auto q=value.toObject();
                const auto tile=q["tile"].toArray(),factory=q["factory"].toArray(),choices=q["posts"].toArray();
                const int capper=q["capper"].toInt(-1),entry=q["entry"].toInt(-1),build=q["build"].toInt(-1);
                if(!q["enemy"].isBool() || q["type"].toString().isEmpty() || choices.size()!=2 ||
                   choices[0]==choices[1] || build<0 || entry<=build || build%2!=int(q["enemy"].toBool()))return {};
                const int side=int(!q["enemy"].toBool());
                const auto admitted=[&](const QJsonArray &at){return at.size()==2 &&
                    valid(TownIntention{at[0].toInt(-1),at[1].toInt(-1),side,capper,entry});};
                if(!admitted(tile) || !admitted(factory) || !admitted(choices[0].toArray()) || !admitted(choices[1].toArray()))return {};
            }
        }
        if(row["version"].toInt()==3){
            if(!row["f1MovementCuts"].isObject())return {};
            const auto cuts=row["f1MovementCuts"].toObject();if(cuts.size()>128)return {};
            for(auto it=cuts.begin();it!=cuts.end();++it){
                if(it.key().size()!=64 || !it.value().isArray() || it.value().toArray().isEmpty())return {};
            }
            out.movementCuts=cuts;
        }
        out.fundedPosts=posts;
        return out;
    }
}
