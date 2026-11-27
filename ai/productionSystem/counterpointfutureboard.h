#pragma once

#include "ai/productionSystem/productionactiondata.h"
#include "coreengine/interpreter.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "resource_management/unitspritemanager.h"
#include <QJsonArray>
#include <QJsonObject>
#include <set>
#include <vector>

// Detached cards for the normal purchase policy. The objects are owned until its
// synchronous call returns. They are never installed in terrain, armies or queues.
struct CounterpointFutureBoard
{
    std::vector<spBuilding> buildings;
    std::vector<spUnit> units;
    std::vector<spProductionActionData> menus;
    QJSValue cards;

    QString collect(GameMap &map, Player &buyer, const QJsonObject &input)
    {
        auto *engine = Interpreter::getInstance();
        if (map.getPlayerCount() != 2 || !input["units"].isArray() || !input["properties"].isArray())
            return QStringLiteral("incomplete two-player production board");
        for (int side = 0; side < 2; ++side)
            if (map.getPlayer(side)->getCO(0) || map.getPlayer(side)->getCO(1))
                return QStringLiteral("future commander production modifiers unpriced");
        cards = engine->newObject();
        auto properties = engine->newObject();
        auto collectors = engine->newObject();
        auto recruits = engine->newObject();
        std::set<int> claimed;
        const QString action = QStringLiteral("ACTION_BUILD_UNITS");
        const int width = map.getMapWidth();
        for (const auto &value : input["properties"].toArray())
        {
            const auto row = value.toObject();
            const int x = row["x"].toInt(-1), y = row["y"].toInt(-1), owner = row["owner"].toInt(-2);
            if (!map.onMap(x, y) || owner < -1 || owner > 1 || !claimed.insert(y*width+x).second)
                return QStringLiteral("invalid future property");
            auto *ground = map.getTerrain(x, y);
            auto *building = ground->getBuilding();
            if (!building)
                return QStringLiteral("future property missing on map");
            const QString key = QString::number(y*width+x);
            if (building->getOwnerID() != owner)
            {
                auto shell = MemoryManagement::create<Building>(building->getBuildingID(), &map);
                shell->setTerrain(ground);
                shell->setOwner(owner < 0 ? nullptr : map.getPlayer(owner));
                building = shell.get();
                properties.setProperty(key, JsThis::getJsThis(building));
                buildings.push_back(std::move(shell));
            }
            if (owner == buyer.getPlayerID() && building->isProductionBuilding())
            {
                auto menu = MemoryManagement::create<ProductionActionData>(&map, x, y, action);
                collectors.setProperty(key, engine->newQObject(menu.get()));
                menus.push_back(std::move(menu));
            }
        }
        std::set<int> existing;
        for (int side = 0; side < 2; ++side)
        {
            const auto army = map.getSpUnits(map.getPlayer(side));
            for (const auto &unit : army->getVector())
                existing.insert(unit->getUniqueID());
        }
        std::set<int> seen;
        for (const auto &value : input["units"].toArray())
        {
            const auto row = value.toObject();
            const int id = row["id"].toInt(-1);
            if (id < 0 || !seen.insert(id).second)
                return QStringLiteral("invalid future unit identity");
            if (existing.contains(id) || row["hp"].toDouble() <= 0)
                continue;
            const int owner = row["owner"].toInt(-1), x = row["x"].toInt(-1), y = row["y"].toInt(-1);
            const QString type = row["type"].toString();
            if (owner < 0 || owner > 1 || !map.onMap(x,y) || !UnitSpriteManager::getInstance()->exists(type))
                return QStringLiteral("future recruit card unavailable");
            auto card = MemoryManagement::create<Unit>(type, map.getPlayer(owner), false, &map);
            card->setTerrain(map.getTerrain(x,y));
            recruits.setProperty(QString::number(id), JsThis::getJsThis(card.get()));
            units.push_back(std::move(card));
        }
        cards.setProperty(QStringLiteral("buildings"), properties);
        cards.setProperty(QStringLiteral("collectors"), collectors);
        cards.setProperty(QStringLiteral("units"), recruits);
        return {};
    }
};
