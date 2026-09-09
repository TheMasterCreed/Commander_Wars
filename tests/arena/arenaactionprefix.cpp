#include "tests/arena/arenaactionprefix.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "ai/coreai.h"
#include "coreengine/memorymanagement.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/unitpathfindingsystem.h"

namespace
{
constexpr qsizetype MAX_PREFIX_ACTIONS = 512;
constexpr qint64 MAX_PREFIX_BYTES = 4 * 1024 * 1024;

bool reject(QString & error, const QString & message)
{
    error = "action prefix: " + message;
    return false;
}

bool integer(const QJsonValue & value, double minimum, double maximum)
{
    return value.isDouble() && value.toDouble() >= minimum && value.toDouble() <= maximum &&
        value.toDouble() == std::floor(value.toDouble());
}

bool point(const QJsonValue & value)
{
    const auto coordinates = value.toArray();
    return coordinates.size() == 2 &&
        integer(coordinates[0], -1, std::numeric_limits<qint32>::max()) &&
        integer(coordinates[1], -1, std::numeric_limits<qint32>::max());
}

QPoint toPoint(const QJsonValue & value)
{
    const auto coordinates = value.toArray();
    return {coordinates[0].toInt(), coordinates[1].toInt()};
}

QString buildType(const QString & text)
{
    QByteArray bytes;
    for (const auto & token : text.split(','))
    {
        bool valid = false;
        const auto byte = token.toUInt(&valid, 16);
        if (!valid || byte > 255)
        {
            return {};
        }
        bytes.append(static_cast<char>(byte));
    }
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_5);
    QString type;
    stream >> type;
    return stream.status() == QDataStream::Ok && stream.atEnd() ? type : QString{};
}

QVariantList pathReceipt(const QVector<QPoint> & path)
{
    QVariantList result;
    for (const auto & tile : path)
    {
        result.append(QVariant(QVariantList{tile.x(), tile.y()}));
    }
    return result;
}
}

bool ArenaActionPrefix::load(const QString & path, QString & error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_PREFIX_BYTES)
    {
        return reject(error, "missing or oversized input");
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto root = document.object();
    m_actions = root.value("actions").toArray();
    m_friendlyPaths = root.value("friendlyExecution").toArray();
    if (root.value("version").toInt() != 1 || m_actions.size() < 2 || m_actions.size() > MAX_PREFIX_ACTIONS)
    {
        return reject(error, "invalid version or action count");
    }
    for (const auto & value : m_actions)
    {
        const auto action = value.toObject();
        const auto id = action.value("actionId").toString();
        if ((id != CoreAI::ACTION_WAIT && id != CoreAI::ACTION_BUILD_UNITS && id != CoreAI::ACTION_NEXT_PLAYER) ||
            !integer(action.value("day"), 0, std::numeric_limits<qint32>::max()) ||
            !integer(action.value("player"), 0, std::numeric_limits<qint32>::max()) ||
            !integer(action.value("costs"), 0, std::numeric_limits<qint32>::max()) ||
            !integer(action.value("seed"), 0, std::numeric_limits<quint32>::max()) ||
            !point(action.value("target")) || (id == CoreAI::ACTION_WAIT && !point(action.value("destination"))))
        {
            return reject(error, "invalid action fields");
        }
    }
    const auto initial = m_actions.first().toObject();
    if (initial.value("day").toInt() != 0 || initial.value("actionId").toString() != CoreAI::ACTION_NEXT_PLAYER)
    {
        return reject(error, "first action must be initial NEXT_PLAYER");
    }
    m_initialSeed = static_cast<quint32>(initial.value("seed").toDouble());
    return true;
}

bool ArenaActionPrefix::apply(spGameAction & action, GameMap & map, QString & error)
{
    if (complete())
    {
        return true;
    }
    const auto expected = m_actions[m_next].toObject();
    auto* player = map.getCurrentPlayer();
    if (player == nullptr || map.getCurrentDay() != expected.value("day").toInt() ||
        player->getPlayerID() != expected.value("player").toInt())
    {
        return reject(error, "day/player mismatch at " + QString::number(m_next));
    }
    const auto id = expected.value("actionId").toString();
    const auto target = toPoint(expected.value("target"));
    const auto costs = expected.value("costs").toInt();
    auto replacement = MemoryManagement::create<GameAction>(
        id, &map, static_cast<quint32>(expected.value("seed").toDouble()));
    replacement->setTarget(target);
    replacement->setCosts(costs);
    bool matched = action->getActionID() == id && action->getTarget() == target && action->getCosts() == costs;
    QString pathSource;
    if (id == CoreAI::ACTION_WAIT)
    {
        auto* unit = replacement->getTargetUnit();
        if (unit == nullptr || unit->getOwner() != player || unit->getUnitID() != expected.value("unit").toString())
        {
            return reject(error, "moving actor mismatch");
        }
        const auto destination = toPoint(expected.value("destination"));
        UnitPathFindingSystem finder(&map, unit, player);
        finder.explore();
        auto path = finder.getPathFast(destination.x(), destination.y());
        pathSource = "legal shortest path, enemy intermediate tiles not logged";
        for (const auto & value : m_friendlyPaths)
        {
            const auto record = value.toObject();
            if (record.value("actor").toInt() == unit->getUniqueID() &&
                record.value("day").toInt() == map.getCurrentDay() &&
                record.value("player").toInt() == player->getPlayerID())
            {
                path.clear();
                const QRegularExpression coordinate("\\((-?\\d+),(-?\\d+)\\)");
                auto matches = coordinate.globalMatch(record.value("path").toString());
                while (matches.hasNext())
                {
                    const auto match = matches.next();
                    path.emplace_back(match.captured(1).toInt(), match.captured(2).toInt());
                }
                std::reverse(path.begin(), path.end());
                pathSource = "recorded Coordinated path";
                break;
            }
        }
        if (path.empty() || path.front() != destination || path.back() != target || finder.getCosts(path) != costs)
        {
            return reject(error, "movement path or fuel cost mismatch");
        }
        replacement->setMovepath(path, costs);
        matched = matched && action->getActionTarget() == destination;
        if (pathSource == "recorded Coordinated path")
        {
            matched = matched && action->getMovePath() == replacement->getMovePath();
        }
    }
    else if (id == CoreAI::ACTION_BUILD_UNITS)
    {
        const auto type = buildType(expected.value("dataText").toString());
        auto* building = replacement->getTargetBuilding();
        if (type.isEmpty() || building == nullptr || building->getOwner() != player ||
            !building->getConstructionList().contains(type) || player->getCosts(type, target) != costs ||
            player->getFunds() < costs)
        {
            return reject(error, "build type, legality or funds mismatch");
        }
        replacement->writeDataString(type);
        replacement->setInputStep(1);
        if (matched)
        {
            action->startReading();
            matched = action->readDataString() == type;
        }
    }
    if (!replacement->canBePerformed() || !replacement->isFinalStep())
    {
        return reject(error, "action is not legal and complete");
    }
    m_receipts.append(QVariantMap{
        {"index", static_cast<qint64>(m_next)}, {"sourceLine", expected.value("sourceLine").toInt()},
        {"day", map.getCurrentDay()}, {"player", player->getPlayerID()}, {"actionId", id},
        {"target", QVariantList{target.x(), target.y()}}, {"costs", costs}, {"pathSource", pathSource},
        {"enginePath", pathReceipt(replacement->getMovePath())}, {"matchedNaturalAction", matched},
        {"seed", static_cast<qulonglong>(replacement->getSeed())},
    });
    action = std::move(replacement);
    ++m_next;
    return true;
}

bool ArenaActionPrefix::observeBoundary(GameMap & map)
{
    if (!complete() || !m_boundary.isEmpty())
    {
        return false;
    }
    QVariantList units;
    for (qint32 y = 0; y < map.getMapHeight(); ++y)
    {
        for (qint32 x = 0; x < map.getMapWidth(); ++x)
        {
            auto* unit = map.getTerrain(x, y)->getUnit();
            if (unit != nullptr)
            {
                units.append(QVariantMap{
                    {"actor", unit->getUniqueID()}, {"type", unit->getUnitID()}, {"x", x}, {"y", y},
                    {"player", unit->getOwner()->getPlayerID()}, {"hp", unit->getHp()}, {"fuel", unit->getFuel()},
                    {"ammo1", unit->getAmmo1()}, {"ammo2", unit->getAmmo2()}, {"hasMoved", unit->getHasMoved()},
                    {"capturePoints", unit->getCapturePoints()}, {"aiMode", static_cast<int>(unit->getAiMode())},
                });
            }
        }
    }
    QVariantList players;
    for (qint32 index = 0; index < map.getPlayerCount(); ++index)
    {
        auto* player = map.getPlayer(index);
        players.append(QVariantMap{
            {"player", index}, {"team", player->getTeam()}, {"funds", player->getFunds()},
            {"coCount", player->getCoCount()}, {"buildList", player->getBuildList()},
        });
    }
    m_boundary = {
        {"day", map.getCurrentDay()}, {"player", map.getCurrentPlayer()->getPlayerID()},
        {"mapHash", QString::fromLatin1(map.getMapHash().toHex())}, {"units", units}, {"players", players},
        {"fog", static_cast<int>(map.getGameRules()->getFogMode())},
        {"aiBehavior", static_cast<int>(map.getGameRules()->getAiBehaviorMode())},
    };
    return true;
}

quint32 ArenaActionPrefix::initialSeed() const
{
    return m_initialSeed;
}

bool ArenaActionPrefix::complete() const
{
    return m_next >= m_actions.size();
}

QVariantMap ArenaActionPrefix::result() const
{
    return {{"originalSerializedReplay", false}, {"complete", complete()}, {"actions", m_receipts},
            {"initialNextPlayerSeed", static_cast<qulonglong>(m_initialSeed)}, {"boundary", m_boundary}};
}
