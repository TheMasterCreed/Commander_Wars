#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include "resource_management/gamemanager.h"
#include "ai/coordinator/continuationinterval.h"
#include "tests/arena/arenamatchconfig.h"

namespace
{
int checks = 0;

bool failed(bool condition)
{
    ++checks;
    if (condition)
    {
        std::printf("FAILED: check %d\n", checks);
    }
    return condition;
}

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

bool refinementControls(const QString & path)
{
    ArenaMatchConfig config;
    if (failed(!load(path, validConfig(), config) || config.coordinatedRefinementWorkLimit != -1 ||
        !config.coordinatedRefinementReceiptFile.isEmpty()))
    {
        return false;
    }
    for (const qint64 limit : {qint64{0}, qint64{160000}, qint64{Coordinator::LIVE_PAIR_REFINEMENT_BUDGET}})
    {
        auto root = validConfig();
        root["coordinatedRefinementWorkLimit"] = limit;
        root["coordinatedRefinementReceiptFile"] = "refinement-receipts.json";
        if (failed(!load(path, root, config) || config.coordinatedRefinementWorkLimit != limit ||
            config.coordinatedRefinementReceiptFile != "refinement-receipts.json"))
        {
            return false;
        }
    }
    for (const QJsonValue & invalid : {QJsonValue{-1}, QJsonValue{1.5}, QJsonValue{"160000"},
                                      QJsonValue{static_cast<qint64>(Coordinator::LIVE_PAIR_REFINEMENT_BUDGET + 1)}})
    {
        auto root = validConfig();
        root["coordinatedRefinementWorkLimit"] = invalid;
        if (failed(load(path, root, config)))
        {
            return false;
        }
    }
    for (const QString & invalid : {QStringLiteral("../receipt.json"), QStringLiteral("arena_result.json"),
                                   QStringLiteral("ARENA_RESULT.JSON"), QString{}})
    {
        auto root = validConfig();
        root["coordinatedRefinementReceiptFile"] = invalid;
        if (failed(load(path, root, config)))
        {
            return false;
        }
    }
    for (const QString & schedule : {QStringLiteral("current"), QStringLiteral("certified-first"),
                                    QStringLiteral("requeue"), QStringLiteral("fair-domains")})
    {
        auto root = validConfig();
        root["coordinatedRefinementSchedule"] = schedule;
        root["coordinatedScheduleDay"] = 3;
        if (failed(!load(path, root, config) || config.coordinatedScheduleDay != 3))
        {
            return false;
        }
    }
    auto invalidSchedule = validConfig();
    invalidSchedule["coordinatedRefinementSchedule"] = "capture-first";
    if (failed(load(path, invalidSchedule, config)))
    {
        return false;
    }
    return true;
}

bool replayControls(const QString & path)
{
    ArenaMatchConfig config;
    auto root = validConfig();
    root["players"] = QJsonArray{
        QJsonObject{{"aiType", "Coordinated"}, {"co", ""}},
        QJsonObject{{"aiType", "Normal"}, {"co", ""}},
    };
    root["rulesetFile"] = "live-rules.grl";
    root["buildListFile"] = "live-build-list.bl";
    root["actionPrefixFile"] = "live-prefix.json";
    root["coordinatedFrontierReceipts"] = true;
    root["coordinatedFrontierDay"] = 3;
    root["coordinatedRefinementReceiptFile"] = "frontier.json";
    root["coordinatedValuationFile"] = "valuations.json";
    if (failed(!load(path, root, config) || !config.players[0].coId.isEmpty() ||
        !config.coordinatedFrontierReceipts || config.coordinatedFrontierDay != 3 ||
        config.rulesetFile != "live-rules.grl" || config.buildListFile != "live-build-list.bl" ||
        config.actionPrefixFile != "live-prefix.json" || config.coordinatedValuationFile != "valuations.json"))
    {
        return false;
    }
    for (const QString & key : {QStringLiteral("rulesetFile"), QStringLiteral("buildListFile"),
                               QStringLiteral("actionPrefixFile"), QStringLiteral("coordinatedValuationFile")})
    {
        auto invalid = root;
        invalid[key] = "../outside";
        if (failed(load(path, invalid, config)))
        {
            return false;
        }
    }
    root.remove("coordinatedRefinementReceiptFile");
    if (failed(load(path, root, config)))
    {
        return false;
    }
    root = validConfig();
    root["coordinatedFrontierReceipts"] = "true";
    if (failed(load(path, root, config)))
    {
        return false;
    }
    root = validConfig();
    root["coordinatedValuationFile"] = "valuations.json";
    if (failed(load(path, root, config)))
    {
        return false;
    }
    root = validConfig();
    root["coordinatedFrontierDay"] = 3;
    return !failed(load(path, root, config));
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    const QString path = directory.filePath("arena_match.json");
    ArenaMatchConfig config;
    if (failed(!directory.isValid() || !load(path, validConfig(), config)))
    {
        return 1;
    }

    const QStringList heavyAiIds = {
        QStringLiteral("HEAVY_ALPHA"),
        QStringLiteral("HEAVY_BETA"),
    };
    const auto heavy0 = GameManager::getHeavyAiType(0);
    const auto heavy1 = GameManager::getHeavyAiType(1);
    if (failed(!heavy0.has_value() || !heavy1.has_value()))
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
        if (failed(!load(path, withAiType(name), config, heavyAiIds) ||
            config.players.at(0).aiType != expectedType ||
            config.players.at(0).aiTypeName != name ||
            identities.contains(static_cast<qint32>(expectedType))))
        {
            return 3;
        }
        identities.insert(static_cast<qint32>(expectedType));
    }
    if (failed(!load(path, withAiType(heavyAiIds.at(0)), config, heavyAiIds) ||
        config.players.at(0).aiType != *heavy0 ||
        !load(path, withAiType(heavyAiIds.at(1)), config, heavyAiIds) ||
        config.players.at(0).aiType != *heavy1))
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
    QJsonObject zeroTurnLimit = validConfig();
    zeroTurnLimit["turnLimit"] = 0;
    QJsonObject negativeTurnLimit = validConfig();
    negativeTurnLimit["turnLimit"] = -1;
    if (failed(!rejected(unsafePath) || !rejected(invalidController) ||
        !rejected(partialStop) || !rejected(incomplete) ||
        !rejected(zeroTurnLimit) || !rejected(negativeTurnLimit)))
    {
        return 5;
    }
    QJsonObject unlimited = validConfig();
    unlimited.remove("turnLimit");
    if (failed(!load(path, unlimited, config) || config.turnLimit != 0))
    {
        return 11;
    }
    for (const QString & invalidHeavy :
         {QStringLiteral("Heavy"), QStringLiteral("Heavy:-1"),
          QStringLiteral("Heavy:2"), QStringLiteral("HEAVY_UNKNOWN")})
    {
        ArenaMatchConfig rejectedConfig;
        if (failed(load(path, withAiType(invalidHeavy), rejectedConfig, heavyAiIds)))
        {
            return 6;
        }
    }

    QJsonObject stopped = validConfig();
    stopped["stopAfterDay"] = 1;
    stopped["stopAfterPlayer"] = 1;
    if (failed(!load(path, stopped, config)))
    {
        return 7;
    }
    QString error;
    config.stopAfterPlayer = 2;
    if (failed(config.validatePlayerCount(2, error)))
    {
        return 8;
    }
    config.stopAfterPlayer = 1;

    ArenaStopBoundary boundary;
    boundary.configure(config);
    boundary.observeAction(1, 0);
    if (failed(boundary.shouldStop(1, 1)))
    {
        return 9;
    }
    boundary.observeAction(1, 1);
    if (failed(boundary.shouldStop(1, 1) || !boundary.shouldStop(2, 0)))
    {
        return 10;
    }
    if (!refinementControls(path) || !replayControls(path))
    {
        return 12;
    }
    std::printf("%d checks, 0 failures\n", checks);
    return 0;
}
