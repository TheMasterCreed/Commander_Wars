#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <vector>

#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    struct CombatBlockOrder
    {
        int unitIndex{NO_UNIT}, engineUnitId{NO_UNIT};
        TilePoint origin{INVALID_TILE}, destination{INVALID_TILE};
        bool fired{};
        int movementCost{-1};
        bool operator==(const CombatBlockOrder &) const = default;
    };

    struct CombatExit
    {
        TilePoint tile{INVALID_TILE};
        int unitIndex{NO_UNIT}, engineUnitId{NO_UNIT};
        bool passable{};
        bool operator==(const CombatExit &) const = default;
    };

    struct CombatFiringOption
    {
        TilePoint tile{INVALID_TILE};
        int cost{}, defense{}, counterSteps{};
        double counterHpLoss{};
        bool operator==(const CombatFiringOption &) const = default;
    };

    struct CombatEncirclement
    {
        int target{NO_UNIT}, targetKnowledge{NO_UNIT}, opener{NO_UNIT}, openerKnowledge{NO_UNIT}, counterSteps{};
        TilePoint center{INVALID_TILE}, firingTile{INVALID_TILE};
        std::vector<CombatFiringOption> firingOptions;
        std::vector<CombatBlockOrder> blockOrders;
        std::vector<CombatExit> exits;
        bool sealed{};
        bool operator==(const CombatEncirclement &) const = default;
    };

    inline QJsonObject combatEncirclementRecord(const CombatEncirclement &trap)
    {
        auto tile = [](TilePoint p) { return QJsonArray{p.x, p.y}; };
        QJsonArray orders, exits, options;
        for (const auto &option : trap.firingOptions)
            options.append(QJsonObject{{"tile", tile(option.tile)}, {"cost", option.cost}, {"defenseStars", option.defense},
                {"counterSteps", option.counterSteps}, {"counterHpLoss", option.counterHpLoss}});
        for (const auto &order : trap.blockOrders)
            orders.append(QJsonObject{{"unit", order.engineUnitId}, {"knowledge", order.unitIndex},
                {"from", tile(order.origin)}, {"to", tile(order.destination)}, {"fired", order.fired}, {"movementCost",order.movementCost}});
        for (const auto &exit : trap.exits)
            exits.append(QJsonObject{{"tile", tile(exit.tile)}, {"unit", exit.engineUnitId},
                {"knowledge", exit.unitIndex}, {"passable", exit.passable}});
        return {{"target", trap.target}, {"center", tile(trap.center)}, {"opener", trap.opener},
            {"firingTile", tile(trap.firingTile)}, {"firingOptions", options}, {"counterSteps", trap.counterSteps}, {"blockOrders", orders},
            {"exits", exits}, {"sealed", trap.sealed}};
    }
}
