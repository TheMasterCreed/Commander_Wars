#include <cmath>
#include <limits>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>

namespace
{
const QSet<QString> REQUIRED_FIELDS = {
    QStringLiteral("version"),
    QStringLiteral("mapPath"),
    QStringLiteral("mapSha256"),
    QStringLiteral("masterSeed"),
    QStringLiteral("turnLimit"),
    QStringLiteral("stopAfterDay"),
    QStringLiteral("stopAfterPlayer"),
    QStringLiteral("players"),
    QStringLiteral("seedTrace"),
    QStringLiteral("actionIds"),
    QStringLiteral("actionTargets"),
    QStringLiteral("preActionStateHashes"),
    QStringLiteral("actionPayloadSha256"),
    QStringLiteral("actionPayloadBase64"),
    QStringLiteral("finalStateHash"),
    QStringLiteral("actionCount"),
    QStringLiteral("watchdog"),
    QStringLiteral("exitCode"),
    QStringLiteral("pass"),
};

bool integerInRange(
    const QJsonValue & value, double low, double high)
{
    if (!value.isDouble())
    {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) &&
           std::floor(number) == number &&
           number >= low &&
           number <= high;
}

bool fixedHex(const QJsonValue & value, qint32 length)
{
    if (!value.isString() ||
        value.toString().size() != length)
    {
        return false;
    }
    for (const QChar character : value.toString())
    {
        if (!character.isDigit() &&
            (character < QChar('a') ||
             character > QChar('f')))
        {
            return false;
        }
    }
    return true;
}

bool hasRequiredFields(const QJsonObject & root)
{
    if (root.size() != REQUIRED_FIELDS.size())
    {
        return false;
    }
    for (const QString & field : REQUIRED_FIELDS)
    {
        if (!root.contains(field))
        {
            return false;
        }
    }
    return true;
}

bool validStopPair(const QJsonObject & root)
{
    const QJsonValue day = root.value("stopAfterDay");
    const QJsonValue player = root.value("stopAfterPlayer");
    if (day.isNull() || player.isNull())
    {
        return day.isNull() && player.isNull();
    }
    return integerInRange(
               day, 0, std::numeric_limits<qint32>::max()) &&
           integerInRange(
               player, 0, std::numeric_limits<qint32>::max());
}

bool validTargets(const QJsonArray & targets)
{
    for (const QJsonValue & value : targets)
    {
        if (!value.isObject())
        {
            return false;
        }
        const QJsonObject target = value.toObject();
        if (target.size() != 2 ||
            !integerInRange(
                target.value("x"),
                std::numeric_limits<qint32>::min(),
                std::numeric_limits<qint32>::max()) ||
            !integerInRange(
                target.value("y"),
                std::numeric_limits<qint32>::min(),
                std::numeric_limits<qint32>::max()))
        {
            return false;
        }
    }
    return true;
}

bool validActionArrays(
    const QJsonObject & root, qint32 actionCount)
{
    const QJsonArray seeds = root.value("seedTrace").toArray();
    const QJsonArray ids = root.value("actionIds").toArray();
    const QJsonArray targets =
        root.value("actionTargets").toArray();
    const QJsonArray preStates =
        root.value("preActionStateHashes").toArray();
    const QJsonArray payloadHashes =
        root.value("actionPayloadSha256").toArray();
    const QJsonArray payloads =
        root.value("actionPayloadBase64").toArray();
    if (seeds.size() != actionCount ||
        ids.size() != actionCount ||
        targets.size() != actionCount ||
        preStates.size() != actionCount ||
        payloadHashes.size() != actionCount ||
        payloads.size() != actionCount ||
        !validTargets(targets))
    {
        return false;
    }
    for (qint32 index = 0; index < actionCount; ++index)
    {
        if (!integerInRange(
                seeds.at(index),
                0,
                std::numeric_limits<quint32>::max()) ||
            !ids.at(index).isString() ||
            ids.at(index).toString().isEmpty() ||
            !fixedHex(preStates.at(index), 128) ||
            !fixedHex(payloadHashes.at(index), 64) ||
            !payloads.at(index).isString())
        {
            return false;
        }
        const QByteArray payload = QByteArray::fromBase64(
            payloads.at(index).toString().toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        if (payload.isEmpty() ||
            QString::fromLatin1(
                QCryptographicHash::hash(
                    payload, QCryptographicHash::Sha256).toHex()) !=
                payloadHashes.at(index).toString())
        {
            return false;
        }
    }
    return true;
}

bool validResult(const QByteArray & data)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        return false;
    }
    const QJsonObject root = document.object();
    if (!hasRequiredFields(root) ||
        root.value("version").toInt(-1) != 2 ||
        !root.value("mapPath").isString() ||
        root.value("mapPath").toString().isEmpty() ||
        !fixedHex(root.value("mapSha256"), 64) ||
        !integerInRange(
            root.value("masterSeed"),
            0,
            std::numeric_limits<quint32>::max()) ||
        !integerInRange(
            root.value("turnLimit"),
            0,
            std::numeric_limits<qint32>::max()) ||
        !validStopPair(root) ||
        !root.value("players").isArray() ||
        root.value("players").toArray().isEmpty() ||
        !integerInRange(
            root.value("actionCount"),
            1,
            std::numeric_limits<qint32>::max()) ||
        !root.value("watchdog").isBool() ||
        !integerInRange(root.value("exitCode"), 0, 6) ||
        !root.value("pass").isBool() ||
        !fixedHex(root.value("finalStateHash"), 128))
    {
        return false;
    }
    const qint32 count = root.value("actionCount").toInt();
    const bool watchdog = root.value("watchdog").toBool();
    const qint32 exitCode = root.value("exitCode").toInt();
    const bool pass = root.value("pass").toBool();
    const bool outcomeValid =
        pass == (!watchdog && exitCode == 0) &&
        (!watchdog || exitCode == 5) &&
        (exitCode != 5 || watchdog) &&
        (exitCode == 0 || exitCode == 4 ||
         exitCode == 5 || exitCode == 6);
    return outcomeValid && validActionArrays(root, count);
}

QByteArray encoded(const QJsonObject & root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
    {
        return 1;
    }
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly))
    {
        return 2;
    }
    const QByteArray validData = file.readAll();
    if (!validResult(validData))
    {
        return 3;
    }
    const QJsonObject valid =
        QJsonDocument::fromJson(validData).object();
    QJsonObject unlimited = valid;
    unlimited["turnLimit"] = 0;
    if (!validResult(encoded(unlimited)))
    {
        return 7;
    }
    for (const QString & field : REQUIRED_FIELDS)
    {
        QJsonObject missing = valid;
        missing.remove(field);
        if (validResult(encoded(missing)))
        {
            return 4;
        }
    }
    QList<QJsonObject> rejected;
    QJsonObject state = valid;
    state["finalStateHash"] = QStringLiteral("00");
    rejected.append(state);
    QJsonObject seed = valid;
    seed["seedTrace"] = QJsonArray();
    rejected.append(seed);
    QJsonObject payload = valid;
    payload["actionPayloadSha256"] =
        QJsonArray{QString(64, QChar('0'))};
    rejected.append(payload);
    QJsonObject count = valid;
    count["actionCount"] = 2;
    rejected.append(count);
    QJsonObject watchdog = valid;
    watchdog["watchdog"] = true;
    rejected.append(watchdog);
    QJsonObject pass = valid;
    pass["pass"] = false;
    rejected.append(pass);
    QJsonObject exit = valid;
    exit["exitCode"] = 4;
    rejected.append(exit);
    QJsonObject negativeTurnLimit = valid;
    negativeTurnLimit["turnLimit"] = -1;
    rejected.append(negativeTurnLimit);
    for (const QJsonObject & candidate : rejected)
    {
        if (validResult(encoded(candidate)))
        {
            return 5;
        }
    }
    return validResult(QByteArrayLiteral("{")) ? 6 : 0;
}
