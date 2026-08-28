#include "tests/arena/arenamatchconfig.h"

#include <cmath>
#include <limits>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
constexpr qint32 CONFIG_VERSION = 2;
constexpr qint32 MIN_PLAYERS = 2;
constexpr qint32 MAX_WATCHDOG_MS = 3600000;

bool reject(QString & error, const QString & message)
{
    error = message;
    return false;
}

bool readInteger(const QJsonObject & root, const QString & key, double minimum,
                 double maximum, double & value, QString & error)
{
    const QJsonValue jsonValue = root.value(key);
    if (!jsonValue.isDouble())
    {
        return reject(error, key + " missing or not a number");
    }
    value = jsonValue.toDouble();
    if (value < minimum || value > maximum || value != std::floor(value))
    {
        return reject(error, key + " is outside its integer range");
    }
    return true;
}

bool plainFileName(const QString & name)
{
    return !name.isEmpty() && name != "." && name != ".." &&
           !name.contains('/') && !name.contains('\\') && !name.contains(':') &&
           QFileInfo(name).fileName() == name;
}

bool resolveAiType(const QString & name, GameEnums::AiTypes & type)
{
    static const QHash<QString, GameEnums::AiTypes> types = {
        {"VeryEasy", GameEnums::AiTypes_VeryEasy},
        {"Normal", GameEnums::AiTypes_Normal},
        {"NormalOffensive", GameEnums::AiTypes_NormalOffensive},
        {"NormalDefensive", GameEnums::AiTypes_NormalDefensive},
        {"Heavy", GameEnums::AiTypes_Heavy},
    };
    const auto entry = types.constFind(name);
    if (entry == types.constEnd())
    {
        return false;
    }
    type = entry.value();
    return true;
}

bool resolveBehavior(const QString & name, GameEnums::AiBehavior & behavior)
{
    if (name == "Standard")
    {
        behavior = GameEnums::AiBehavior_Standard;
        return true;
    }
    if (name == "Counterpoint")
    {
        behavior = GameEnums::AiBehavior_Counterpoint;
        return true;
    }
    return false;
}
}

const QString ArenaMatchConfig::FILE_NAME = QStringLiteral("arena_match.json");

bool ArenaMatchConfig::load(const QString & path, ArenaMatchConfig & config, QString & error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return reject(error, "cannot read match config " + path);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return reject(error, "match config is not a json object");
    }
    const QJsonObject root = document.object();
    config = {};

    double number = 0;
    if (!readInteger(root, "version", CONFIG_VERSION, CONFIG_VERSION, number, error))
    {
        return false;
    }

    const QJsonValue mapValue = root.value("mapPath");
    if (!mapValue.isString())
    {
        return reject(error, "mapPath missing or not a string");
    }
    const QString suppliedMapPath = mapValue.toString();
    config.mapPath = QDir::cleanPath(suppliedMapPath);
    if (config.mapPath != suppliedMapPath || QDir::isAbsolutePath(config.mapPath) ||
        config.mapPath == ".." ||
        config.mapPath.startsWith("../") || !config.mapPath.endsWith(".map", Qt::CaseInsensitive) ||
        !QFileInfo(config.mapPath).isFile())
    {
        return reject(error, "mapPath is not a safe existing map");
    }

    const QJsonValue playersValue = root.value("players");
    if (!playersValue.isArray() || playersValue.toArray().size() < MIN_PLAYERS)
    {
        return reject(error, "players missing or too short");
    }
    for (const QJsonValue & value : playersValue.toArray())
    {
        if (!value.isObject())
        {
            return reject(error, "players entry is not an object");
        }
        const QJsonObject entry = value.toObject();
        ArenaMatchPlayerConfig player;
        if (!entry.value("aiType").isString() ||
            !resolveAiType(entry.value("aiType").toString(), player.aiType))
        {
            return reject(error, "players entry has an unsupported aiType");
        }
        if (!entry.value("co").isString() || entry.value("co").toString().isEmpty())
        {
            return reject(error, "players entry has no co");
        }
        player.aiTypeName = entry.value("aiType").toString();
        player.coId = entry.value("co").toString();
        config.players.append(player);
    }

    const QJsonValue behaviorValue = root.value("aiBehaviorMode");
    if (!behaviorValue.isString() ||
        !resolveBehavior(behaviorValue.toString(), config.aiBehaviorMode))
    {
        return reject(error, "aiBehaviorMode is not supported");
    }
    if (!readInteger(root, "masterSeed", 0,
                     static_cast<double>(std::numeric_limits<quint32>::max()), number, error))
    {
        return false;
    }
    config.masterSeed = static_cast<quint32>(number);
    if (!readInteger(root, "turnLimit", 1, std::numeric_limits<qint32>::max(), number, error))
    {
        return false;
    }
    config.turnLimit = static_cast<qint32>(number);
    if (!readInteger(root, "watchdogMs", 1, MAX_WATCHDOG_MS, number, error))
    {
        return false;
    }
    config.watchdogMs = static_cast<qint32>(number);

    const QJsonValue resultValue = root.value("resultFileName");
    if (!resultValue.isString() || !plainFileName(resultValue.toString()))
    {
        return reject(error, "resultFileName is not a plain file name");
    }
    config.resultFileName = resultValue.toString();

    const QJsonValue stopDay = root.value("stopAfterDay");
    const QJsonValue stopPlayer = root.value("stopAfterPlayer");
    if (!root.contains("stopAfterDay") || !root.contains("stopAfterPlayer") ||
        stopDay.isNull() != stopPlayer.isNull())
    {
        return reject(error, "stopAfterDay and stopAfterPlayer must be supplied together");
    }
    if (!stopDay.isNull())
    {
        if (!stopDay.isDouble() || !stopPlayer.isDouble() ||
            stopDay.toDouble() != std::floor(stopDay.toDouble()) ||
            stopPlayer.toDouble() != std::floor(stopPlayer.toDouble()) ||
            stopDay.toDouble() > std::numeric_limits<qint32>::max() ||
            stopPlayer.toDouble() > std::numeric_limits<qint32>::max() ||
            stopDay.toInt() <= 0 || stopPlayer.toInt() < 0)
        {
            return reject(error, "stop boundary is invalid");
        }
        config.stopAfterDay = stopDay.toInt();
        config.stopAfterPlayer = stopPlayer.toInt();
    }

    const QJsonValue deterministicValue = root.value("deterministicCounterpointSeed");
    if (!deterministicValue.isBool())
    {
        return reject(error, "deterministicCounterpointSeed missing or not a boolean");
    }
    config.deterministicCounterpointSeed = deterministicValue.toBool();
    if (config.deterministicCounterpointSeed &&
        config.aiBehaviorMode != GameEnums::AiBehavior_Counterpoint)
    {
        return reject(error, "deterministicCounterpointSeed requires Counterpoint");
    }
    return true;
}

bool ArenaMatchConfig::validatePlayerCount(qint32 playerCount, QString & error) const
{
    if (playerCount != players.size())
    {
        return reject(error, "map and config player counts differ");
    }
    if (hasStopBoundary() && stopAfterPlayer >= playerCount)
    {
        return reject(error, "stopAfterPlayer is outside the map player range");
    }
    return true;
}

bool ArenaMatchConfig::hasStopBoundary() const
{
    return stopAfterDay > 0 && stopAfterPlayer >= 0;
}

void ArenaStopBoundary::configure(const ArenaMatchConfig & config)
{
    m_day = config.stopAfterDay;
    m_player = config.stopAfterPlayer;
    m_actionSeen = false;
}

void ArenaStopBoundary::observeAction(qint32 day, qint32 player)
{
    m_actionSeen = m_actionSeen || (day == m_day && player == m_player);
}

bool ArenaStopBoundary::shouldStop(qint32 day, qint32 player) const
{
    return m_actionSeen && (day != m_day || player != m_player);
}
