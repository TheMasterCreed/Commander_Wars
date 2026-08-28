#include "tests/arena/arenatestsupport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <memory>

#include "ai/coordinatedai.h"
#include "ai/normalai.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/settings.h"
#include "game/gameaction.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "gameinput/basegameinputif.h"

namespace
{
const QString ARENA_TEST_PASS = QStringLiteral("ARENA_TEST_PASS");
const QString ARENA_TEST_FAIL_PREFIX = QStringLiteral("ARENA_TEST_FAIL:");
const QString ARENA_REASON_PREFIX = QStringLiteral("ARENA_REASON:");
constexpr qint32 MAX_WATCHDOG_MS = 3600000;
constexpr qint32 COORDINATED_SHELL_MAP_SIZE = 8;
constexpr qint32 COORDINATED_SHELL_PLAYER_COUNT = 2;
constexpr qint32 COORDINATED_SHELL_PLAYER = 0;

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

struct CoordinatedShellFixture
{
    spGameMap pMap;
    spBaseGameInputIF pInput;
    spCoordinatedAi pAi;
    Player* pPlayer{nullptr};
};

void expect(bool condition, const QString & failure, QStringList & failures)
{
    if (!condition)
    {
        failures.append(failure);
    }
}

bool buildCoordinatedShellFixture(CoordinatedShellFixture & fixture,
                                  QStringList & failures)
{
    fixture.pMap = MemoryManagement::create<GameMap>(
        COORDINATED_SHELL_MAP_SIZE,
        COORDINATED_SHELL_MAP_SIZE,
        COORDINATED_SHELL_PLAYER_COUNT);
    fixture.pPlayer = fixture.pMap->getPlayer(COORDINATED_SHELL_PLAYER);
    fixture.pInput = BaseGameInputIF::createAi(
        fixture.pMap.get(), GameEnums::AiTypes_Coordinated);
    fixture.pAi = std::dynamic_pointer_cast<CoordinatedAi>(fixture.pInput);
    expect(fixture.pPlayer != nullptr, QStringLiteral("missing player"), failures);
    expect(fixture.pInput != nullptr, QStringLiteral("factory returned null"), failures);
    expect(fixture.pAi != nullptr, QStringLiteral("factory returned wrong class"), failures);
    if (fixture.pPlayer == nullptr || fixture.pAi == nullptr)
    {
        return false;
    }
    fixture.pMap->setCurrentPlayer(COORDINATED_SHELL_PLAYER);
    fixture.pPlayer->setControlType(GameEnums::AiTypes_Coordinated);
    fixture.pPlayer->setBaseGameInput(fixture.pInput);
    fixture.pAi->resetMoveMap();
    fixture.pAi->onGameStart();
    return true;
}

QStringList runCoordinatedShellProcess(CoordinatedAi & ai)
{
    QStringList actionIds;
    const QMetaObject::Connection connection = QObject::connect(
        &ai,
        &CoreAI::sigPerformAction,
        &ai,
        [&actionIds](spGameAction pAction, bool)
        {
            actionIds.append(pAction != nullptr
                                 ? pAction->getActionID()
                                 : QStringLiteral("<null>"));
        },
        Qt::DirectConnection);
    ai.process();
    QObject::disconnect(connection);
    return actionIds;
}

QVariantMap coordinatedShell(QObject*, const QVariantMap &)
{
    QStringList failures;
    CoordinatedShellFixture fixture;
    if (buildCoordinatedShellFixture(fixture, failures))
    {
        expect(dynamic_cast<NormalAi*>(fixture.pInput.get()) != nullptr,
               QStringLiteral("shell does not inherit NormalAi"), failures);
        expect(fixture.pInput->getAiType() == GameEnums::AiTypes_Coordinated,
               QStringLiteral("factory lost Coordinated identity"), failures);
        expect(fixture.pAi->getAiName() == NormalAi::DEFAULT_JS_NAME,
               QStringLiteral("shell changed NormalAi script name"), failures);
        expect(fixture.pAi->getLoadedIniCount(
                   QStringLiteral("normal/") + NormalAi::DEFAULT_INI_FILE) == 1,
               QStringLiteral("shell did not load NormalAi defaults"), failures);
        const QStringList actionIds = runCoordinatedShellProcess(*fixture.pAi);
        expect(actionIds == QStringList{QString::fromLatin1(CoreAI::ACTION_NEXT_PLAYER)},
               QStringLiteral("shell did not fall back to NormalAi"), failures);
    }
    return {
        {QStringLiteral("ok"), failures.isEmpty()},
        {QStringLiteral("failures"), failures},
    };
}

QVariantMap coordinatedShellPauseFallback(QObject*, const QVariantMap &)
{
    QStringList failures;
    CoordinatedShellFixture fixture;
    if (buildCoordinatedShellFixture(fixture, failures))
    {
        fixture.pAi->toggleAiPause();
        const QStringList pausedActions = runCoordinatedShellProcess(*fixture.pAi);
        expect(pausedActions.isEmpty(),
               QStringLiteral("paused shell emitted an action"), failures);
        fixture.pAi->toggleAiPause();
        const QStringList resumedActions = runCoordinatedShellProcess(*fixture.pAi);
        expect(resumedActions ==
                   QStringList{QString::fromLatin1(CoreAI::ACTION_NEXT_PLAYER)},
               QStringLiteral("resumed shell did not use NormalAi fallback"), failures);
    }
    return {
        {QStringLiteral("ok"), failures.isEmpty()},
        {QStringLiteral("failures"), failures},
    };
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

namespace
{
[[maybe_unused]] const bool COORDINATED_SHELL_REGISTERED =
    AiArenaTestSupport::registerOperation(
        QStringLiteral("coordinatedShell"), coordinatedShell);
[[maybe_unused]] const bool COORDINATED_SHELL_PAUSE_FALLBACK_REGISTERED =
    AiArenaTestSupport::registerOperation(
        QStringLiteral("coordinatedShellPauseFallback"),
        coordinatedShellPauseFallback);
}
