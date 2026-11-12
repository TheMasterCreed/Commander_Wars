#include "tests/arena/playabletrialconfig.h"

#include <cmath>
#include <limits>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace
{
constexpr qint32 REQUEST_VERSION = 1;

bool reject(QString & error, const QString & reason)
{
    error = reason;
    return false;
}

bool readName(const QJsonObject & object, const QString & key, QString & value, QString & error)
{
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    const auto json = object.value(key);
    value = json.toString();
    if (!json.isString() || !name.match(value).hasMatch())
    {
        return reject(error, key + " must be a plain local name");
    }
    return true;
}

bool readBoundary(const QJsonObject & object, ArenaMatchConfig & boundary, QString & error)
{
    const auto day = object.value("checkpointAfterDay");
    const auto player = object.value("checkpointAfterPlayer");
    if (!day.isDouble() || day.toDouble() < 1 || day.toDouble() > std::numeric_limits<qint32>::max() ||
        day.toDouble() != std::floor(day.toDouble()) ||
        !player.isDouble() || player.toDouble() < 0 || player.toDouble() >= boundary.players.size() ||
        player.toDouble() != std::floor(player.toDouble()))
    {
        return reject(error, "checkpoint boundary must identify a complete player turn");
    }
    boundary.stopAfterDay = static_cast<qint32>(day.toDouble());
    boundary.stopAfterPlayer = static_cast<qint32>(player.toDouble());
    return true;
}

bool validateFiles(const QString & profile, const PlayableTrialConfig & config, QString & error)
{
    QSet<QString> names;
    const QStringList files{PlayableTrialConfig::FILE_NAME, config.fixtureFile, config.resultFile,
                            config.actionsFile, config.saveFile, config.checkpointFile};
    for (const auto & name : files)
    {
        if (!name.isEmpty() && names.contains(name.toCaseFolded()))
        {
            return reject(error, "trial file names must be distinct");
        }
        names.insert(name.toCaseFolded());
    }
    const QDir directory(profile);
    for (const auto & name : {config.resultFile, config.actionsFile})
    {
        if (QFileInfo::exists(directory.filePath(name)))
        {
            return reject(error, "trial output already exists: " + name);
        }
    }
    if (config.mode == PlayableTrialMode::Save || config.mode == PlayableTrialMode::Reload)
    {
        if (QFileInfo::exists(directory.filePath(config.saveFile)) ||
            QFileInfo::exists(directory.filePath(config.checkpointFile)))
        {
            return reject(error, "checkpoint output already exists");
        }
    }
    if (config.mode == PlayableTrialMode::Resume &&
        (!QFileInfo(directory.filePath(config.saveFile)).isFile() ||
         !QFileInfo(directory.filePath(config.checkpointFile)).isFile()))
    {
        return reject(error, "resume requires an existing save and checkpoint receipt");
    }
    return true;
}

bool validateFixture(const PlayableTrialConfig & config, QString & error)
{
    const auto & fixture = config.fixture;
    if (!fixture.hasStopBoundary() || fixture.turnLimit != 0 || !fixture.actionPrefixFile.isEmpty() ||
        !fixture.coordinatedRefinementReceiptFile.isEmpty() || !fixture.coordinatedValuationFile.isEmpty() ||
        fixture.coordinatedFrontierReceipts || !fixture.coordinatedTierAudit || fixture.coordinatedRefinementWorkLimit != -1 ||
        fixture.coordinatedRefinementSchedule != Coordinator::RelativeStockSchedule::Current ||
        fixture.coordinatedScheduleDay != -1)
    {
        return reject(error, "trial requires Full fixture audit, a stop boundary, and no prefix, oracle, or alternate refinement policy");
    }
    if (config.mode == PlayableTrialMode::Reload &&
        (config.checkpointBoundary.stopAfterDay > fixture.stopAfterDay ||
         (config.checkpointBoundary.stopAfterDay == fixture.stopAfterDay &&
          config.checkpointBoundary.stopAfterPlayer >= fixture.stopAfterPlayer)))
    {
        return reject(error, "reload checkpoint must precede the final stop boundary");
    }
    return true;
}
}

const QString PlayableTrialConfig::FILE_NAME = QStringLiteral("playable_trial.json");
const char* const PlayableTrialConfig::ENVIRONMENT_FLAG = "COW_MASTER_ASSIGNMENT_PLAYABLE_TEST";

bool PlayableTrialConfig::load(const QString & profile, PlayableTrialConfig & config, QString & error)
{
    QFile file(QDir(profile).filePath(FILE_NAME));
    if (!file.open(QIODevice::ReadOnly))
    {
        return reject(error, "cannot read playable trial request");
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return reject(error, "playable trial request is not a JSON object");
    }
    const auto object = document.object();
    static const QSet<QString> keys{
        "version", "mode", "sessionId", "fixtureFile", "resultFile", "actionsFile",
        "saveFile", "checkpointFile", "checkpointAfterDay", "checkpointAfterPlayer", "expectedStop",
    };
    for (const auto & key : object.keys())
    {
        if (!keys.contains(key))
        {
            return reject(error, "unknown playable trial key: " + key);
        }
    }
    config = {};
    if (object.value("version").toDouble(-1) != REQUEST_VERSION ||
        !readName(object, "sessionId", config.sessionId, error) ||
        !readName(object, "fixtureFile", config.fixtureFile, error) ||
        !readName(object, "resultFile", config.resultFile, error) ||
        !readName(object, "actionsFile", config.actionsFile, error))
    {
        return reject(error, error.isEmpty() ? "unsupported playable trial version" : error);
    }
    const auto mode = object.value("mode").toString();
    if (mode == "fresh")
    {
        config.mode = PlayableTrialMode::Fresh;
    }
    else if (mode == "save")
    {
        config.mode = PlayableTrialMode::Save;
    }
    else if (mode == "reload")
    {
        config.mode = PlayableTrialMode::Reload;
    }
    else if (mode == "resume")
    {
        config.mode = PlayableTrialMode::Resume;
    }
    else
    {
        return reject(error, "unsupported playable trial mode");
    }
    if (object.contains("expectedStop"))
    {
        if (!object.value("expectedStop").isString() || object.value("expectedStop").toString().isEmpty() ||
            (config.mode != PlayableTrialMode::Fresh && config.mode != PlayableTrialMode::Resume))
        {
            return reject(error, "expectedStop requires fresh or resume mode and an explicit reason");
        }
        config.expectedStop = object.value("expectedStop").toString();
    }
    if (!ArenaMatchConfig::load(QDir(profile).filePath(config.fixtureFile), config.fixture, error))
    {
        return false;
    }
    config.checkpointBoundary = config.fixture;
    if (config.mode != PlayableTrialMode::Fresh)
    {
        if (!readName(object, "saveFile", config.saveFile, error) ||
            !readName(object, "checkpointFile", config.checkpointFile, error) ||
            !config.saveFile.endsWith(".sav", Qt::CaseInsensitive))
        {
            return reject(error, error.isEmpty() ? "saveFile must end in .sav" : error);
        }
    }
    else if (object.contains("saveFile") || object.contains("checkpointFile"))
    {
        return reject(error, "fresh mode cannot name checkpoint files");
    }
    if (config.mode == PlayableTrialMode::Save || config.mode == PlayableTrialMode::Reload)
    {
        if (!readBoundary(object, config.checkpointBoundary, error))
        {
            return false;
        }
    }
    else if (object.contains("checkpointAfterDay") || object.contains("checkpointAfterPlayer"))
    {
        return reject(error, "checkpoint boundary is only valid for save or reload");
    }
    return validateFiles(profile, config, error) && validateFixture(config, error);
}
