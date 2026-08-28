#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include "resource_management/gamemanager.h"
#include "tests/arena/arenamatchconfig.h"

namespace
{
QJsonObject validConfig()
{
    const QJsonArray players = {
        QJsonObject{{"aiType", "Normal"}, {"co", "CO_ANDY"}},
        QJsonObject{{"aiType", "Normal"}, {"co", "CO_JESS"}},
    };
    return {
        {"version", 2},
        {"mapPath", "maps/2_player/Bean Island.map"},
        {"players", players},
        {"aiBehaviorMode", "Standard"},
        {"masterSeed", 20260828},
        {"turnLimit", 2},
        {"watchdogMs", 120000},
        {"resultFileName", "arena_result.json"},
        {"stopAfterDay", QJsonValue::Null},
        {"stopAfterPlayer", QJsonValue::Null},
        {"deterministicCounterpointSeed", false},
    };
}

bool load(const QString & path, const QJsonObject & root, ArenaMatchConfig & config)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    QString error;
    return ArenaMatchConfig::load(path, config, error);
}

bool load(const QString & path, const QJsonObject & root, ArenaMatchConfig & config,
          const QStringList & heavyAiIds)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    QString error;
    return ArenaMatchConfig::load(path, config, error, heavyAiIds);
}

QJsonObject withAiType(const QString & name)
{
    QJsonObject root = validConfig();
    QJsonArray players = root["players"].toArray();
    QJsonObject player = players[0].toObject();
    player["aiType"] = name;
    players[0] = player;
    root["players"] = players;
    return root;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    const QString path = directory.filePath("arena_match.json");
    ArenaMatchConfig config;
    if (!directory.isValid() || !load(path, validConfig(), config))
    {
        return 1;
    }

    const QStringList heavyAiIds = {
        QStringLiteral("HEAVY_ALPHA"),
        QStringLiteral("HEAVY_BETA"),
    };
    const auto heavy0 = GameManager::getHeavyAiType(0);
    const auto heavy1 = GameManager::getHeavyAiType(1);
    if (!heavy0.has_value() || !heavy1.has_value())
    {
        return 2;
    }
    const QVector<QPair<QString, GameEnums::AiTypes>> supported = {
        {QStringLiteral("VeryEasy"), GameEnums::AiTypes_VeryEasy},
        {QStringLiteral("Normal"), GameEnums::AiTypes_Normal},
        {QStringLiteral("NormalOffensive"), GameEnums::AiTypes_NormalOffensive},
        {QStringLiteral("NormalDefensive"), GameEnums::AiTypes_NormalDefensive},
        {QStringLiteral("Heavy:0"), *heavy0},
        {QStringLiteral("Heavy:1"), *heavy1},
        {QStringLiteral("Coordinated"), GameEnums::AiTypes_Coordinated},
    };
    QSet<qint32> identities;
    for (const auto & [name, expectedType] : supported)
    {
        if (!load(path, withAiType(name), config, heavyAiIds) ||
            config.players.at(0).aiType != expectedType ||
            config.players.at(0).aiTypeName != name ||
            identities.contains(static_cast<qint32>(expectedType)))
        {
            return 3;
        }
        identities.insert(static_cast<qint32>(expectedType));
    }
    if (!load(path, withAiType(heavyAiIds.at(0)), config, heavyAiIds) ||
        config.players.at(0).aiType != *heavy0 ||
        !load(path, withAiType(heavyAiIds.at(1)), config, heavyAiIds) ||
        config.players.at(0).aiType != *heavy1)
    {
        return 4;
    }

    auto rejected = [&](QJsonObject root)
    {
        ArenaMatchConfig rejectedConfig;
        return !load(path, root, rejectedConfig);
    };
    QJsonObject unsafePath = validConfig();
    unsafePath["mapPath"] = "../Bean Island.map";
    QJsonObject invalidController = validConfig();
    QJsonArray players = invalidController["players"].toArray();
    QJsonObject player = players[0].toObject();
    player["aiType"] = "Proxy";
    players[0] = player;
    invalidController["players"] = players;
    QJsonObject partialStop = validConfig();
    partialStop["stopAfterDay"] = 1;
    QJsonObject incomplete = validConfig();
    incomplete.remove("watchdogMs");
    if (!rejected(unsafePath) || !rejected(invalidController) ||
        !rejected(partialStop) || !rejected(incomplete))
    {
        return 5;
    }
    for (const QString & invalidHeavy :
         {QStringLiteral("Heavy"), QStringLiteral("Heavy:-1"),
          QStringLiteral("Heavy:2"), QStringLiteral("HEAVY_UNKNOWN")})
    {
        ArenaMatchConfig rejectedConfig;
        if (load(path, withAiType(invalidHeavy), rejectedConfig, heavyAiIds))
        {
            return 6;
        }
    }

    QJsonObject stopped = validConfig();
    stopped["stopAfterDay"] = 1;
    stopped["stopAfterPlayer"] = 1;
    if (!load(path, stopped, config))
    {
        return 7;
    }
    QString error;
    config.stopAfterPlayer = 2;
    if (config.validatePlayerCount(2, error))
    {
        return 8;
    }
    config.stopAfterPlayer = 1;

    ArenaStopBoundary boundary;
    boundary.configure(config);
    boundary.observeAction(1, 0);
    if (boundary.shouldStop(1, 1))
    {
        return 9;
    }
    boundary.observeAction(1, 1);
    if (boundary.shouldStop(1, 1) || !boundary.shouldStop(2, 0))
    {
        return 10;
    }
    return 0;
}
