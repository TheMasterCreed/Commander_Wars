#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <set>
#include <vector>

namespace Coordinator
{
    struct OpeningServicePortfolio
    {
        std::set<QString> friendlyRecruits,enemyRecruits;
        bool friendlyService{};

        std::vector<QString> methodKey() const
        {
            std::vector<QString> key{friendlyService?"F1":"E0"};
            for(const auto &type:friendlyRecruits)key.push_back("friendly/"+type);
            for(const auto &type:enemyRecruits)key.push_back("enemy/"+type);
            return key;
        }
    };

    inline OpeningServicePortfolio openingServicePortfolio(const QJsonArray &orders,bool friendlyService)
    {
        OpeningServicePortfolio result;
        result.friendlyService=friendlyService;
        for(const auto &item:orders){
            const auto order=item.toObject();
            if(!order["enemy"].toBool() && order["buildDay"].toInt()==0)
                result.friendlyRecruits.insert(order["type"].toString());
            if(friendlyService && order["enemy"].toBool())result.enemyRecruits.insert(order["type"].toString());
        }
        return result;
    }
}
