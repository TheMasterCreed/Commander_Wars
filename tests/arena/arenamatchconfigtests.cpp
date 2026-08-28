#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

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
        return 2;
    }

    QJsonObject stopped = validConfig();
    stopped["stopAfterDay"] = 1;
    stopped["stopAfterPlayer"] = 1;
    if (!load(path, stopped, config))
    {
        return 3;
    }
    QString error;
    config.stopAfterPlayer = 2;
    if (config.validatePlayerCount(2, error))
    {
        return 4;
    }
    config.stopAfterPlayer = 1;

    ArenaStopBoundary boundary;
    boundary.configure(config);
    boundary.observeAction(1, 0);
    if (boundary.shouldStop(1, 1))
    {
        return 5;
    }
    boundary.observeAction(1, 1);
    if (boundary.shouldStop(1, 1) || !boundary.shouldStop(2, 0))
    {
        return 6;
    }
    return 0;
}
