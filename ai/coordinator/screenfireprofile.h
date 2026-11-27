#pragma once

#include "ai/coordinator/mobilityfield.h"
#include <map>
#include <set>
#include <span>
#include <QJsonArray>
#include <QString>

namespace Coordinator
{
    // Verified for one turn's actual rules, weather and native unit definitions.
    // The gross damage is in native percentage points before HP division by ten.
    struct ScreenFireType
    {
        int movement{},range{},grid{-1};
        std::map<QString,int> maxDamage;
        // Keep the uncapped native table power when a wounded shooter's HP
        // and the specific defender's terrain are independently bound.
        std::map<QString,int> weaponBase;
    };

    struct ScreenFireProfile
    {
        std::map<QString,ScreenFireType> types;
        std::set<QString> quietBirths;
        std::vector<ScreenFireType> original;
        std::vector<bool> ordinary;
        bool ordinaryOpeningPurchases{};
        int through{-1};
        QString reason;
    };

    struct ScreenRuleDemand
    {
        int actor{-1},holder{-1},tick{-1},requests{};
        bool regular{},audit{},attempted{};
        void request(bool diagnostic,int capper,int post,int crossing)
        {
            if(attempted)return;
            if((diagnostic && audit) || (!diagnostic && regular))return;
            actor=capper;holder=post;tick=crossing;
            if(diagnostic)audit=true;else regular=true;
            ++requests;
        }
        template<class Prepare>
        bool atSharedBoundary(bool afterRanking,std::int64_t available,std::int64_t reservation,
            std::int64_t rerunWork,Prepare prepare)
        {
            if(attempted || !(regular || (afterRanking && audit)))return false;
            attempted=true;
            if(reservation<=0 || available<reservation || available-reservation<rerunWork)return false;
            return prepare(reservation);
        }
    };

    struct ScreenOpening
    {
        std::span<const TilePoint> positions;
        std::span<const double> hp;
        const QJsonArray *orders{};
        bool diagnostic{};
        std::span<const int> openingCarry,openingSteps,openingFuel;
        std::span<const double> openingExactHp;
    };
}
