#include "tests/arena/arenatestsupport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "coreengine/settings.h"

namespace
{
const QString ARENA_TEST_PASS = QStringLiteral("ARENA_TEST_PASS");
const QString ARENA_TEST_FAIL_PREFIX = QStringLiteral("ARENA_TEST_FAIL:");
const QString ARENA_REASON_PREFIX = QStringLiteral("ARENA_REASON:");
constexpr qint32 MAX_WATCHDOG_MS = 3600000;

QHash<QString, AiArenaTestSupport::Operation> & operations()
{
    static QHash<QString, AiArenaTestSupport::Operation> registry;
    return registry;
}

qint32 safeExitCode(qint32 exitCode)
{
    switch (static_cast<ArenaExitCode>(exitCode))
    {
        case ArenaExitCode::Ok:
        case ArenaExitCode::AssertFailed:
        case ArenaExitCode::Watchdog:
        case ArenaExitCode::SetupError:
            return exitCode;
    }
    return static_cast<qint32>(ArenaExitCode::AssertFailed);
}
}

const char* const AiArenaTestSupport::ENVIRONMENT_FLAG = "COW_AI_ARENA_TEST";
const QString AiArenaTestSupport::JS_GLOBAL_NAME = QStringLiteral("arenaTest");

AiArenaTestSupport::AiArenaTestSupport(QObject* pParent)
    : QObject(pParent)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this]()
    {
        qInfo().noquote() << ARENA_REASON_PREFIX + m_watchdogReason;
        finish(ArenaExitCode::Watchdog);
    });
}

bool AiArenaTestSupport::registerOperation(const QString & name, Operation operation)
{
    auto & registry = operations();
    if (name.isEmpty() || !operation || registry.contains(name))
    {
        return false;
    }
    registry.insert(name, std::move(operation));
    return true;
}

QVariantMap AiArenaTestSupport::runOperation(const QString & name,
                                             QObject* pContext,
                                             const QVariantMap & arguments) const
{
    const auto & registry = operations();
    auto operation = registry.constFind(name);
    if (operation == registry.constEnd())
    {
        return {
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("unknown operation")},
        };
    }
    return operation.value()(pContext, arguments);
}

bool AiArenaTestSupport::writeJsonResult(const QString & fileName,
                                         const QVariantMap & result) const
{
    const QString path = getOutputPath(fileName);
    if (path.isEmpty())
    {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }
    const QByteArray data =
        QJsonDocument(QJsonObject::fromVariantMap(result)).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size())
    {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool AiArenaTestSupport::armWatchdog(const QString & reason, qint32 timeoutMs)
{
    if (m_exitQueued || timeoutMs <= 0 || timeoutMs > MAX_WATCHDOG_MS)
    {
        return false;
    }
    m_watchdogReason = reason;
    m_watchdog.start(timeoutMs);
    return true;
}

void AiArenaTestSupport::cancelWatchdog()
{
    m_watchdog.stop();
    m_watchdogReason.clear();
}

void AiArenaTestSupport::finish(qint32 exitCode)
{
    if (m_exitQueued)
    {
        return;
    }
    m_exitQueued = true;
    cancelWatchdog();
    exitCode = safeExitCode(exitCode);
    if (exitCode == static_cast<qint32>(ArenaExitCode::Ok))
    {
        qInfo().noquote() << ARENA_TEST_PASS;
    }
    else
    {
        qInfo().noquote() << ARENA_TEST_FAIL_PREFIX + QString::number(exitCode);
    }
    QCoreApplication* pApplication = QCoreApplication::instance();
    if (pApplication != nullptr)
    {
        QMetaObject::invokeMethod(pApplication, [exitCode]()
        {
            QCoreApplication::exit(exitCode);
        }, Qt::QueuedConnection);
    }
}

void AiArenaTestSupport::finish(ArenaExitCode exitCode)
{
    finish(static_cast<qint32>(exitCode));
}

QString AiArenaTestSupport::getOutputPath(const QString & fileName) const
{
    if (fileName.isEmpty() ||
        fileName == QStringLiteral(".") ||
        fileName == QStringLiteral("..") ||
        fileName.contains(QChar('/')) ||
        fileName.contains(QChar('\\')) ||
        fileName.contains(QChar(':')) ||
        QFileInfo(fileName).fileName() != fileName)
    {
        return QString();
    }
    QDir userDirectory(Settings::getInstance()->getUserPath());
    if (!userDirectory.exists() && !userDirectory.mkpath(QStringLiteral(".")))
    {
        return QString();
    }
    return userDirectory.filePath(fileName);
}
