#include "tests/counterpoint_ai/counterpointtestsupport.h"

#include <cstdlib>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "ai/coreai.h"
#include "ai/productionSystem/simpleproductionsystem.h"

#include "coreengine/globalutils.h"
#include "coreengine/settings.h"

#include "game/actionperformer.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/gamerecording/gamerecorder.h"
#include "game/player.h"
#include "game/unit.h"

#include "gameinput/basegameinputif.h"

#include "menue/gamemenue.h"

namespace
{
static constexpr quint32 PRODUCTION_DISCOVERY_SEED = 0xC0FFEE;
static constexpr qint32 PRODUCTION_DISCOVERY_RANDOM_MAX = 100000;
static constexpr quint32 COUNTERPOINT_RANDOM_PROBE_SEED = 0x51A7E;
static constexpr qint32 COUNTERPOINT_RANDOM_PROBE_MAX = 100000;
}

CounterpointTestSupport::CounterpointTestSupport(QObject* pParent)
    : QObject(pParent)
{
}

bool CounterpointTestSupport::writeGameRules(GameRules* pRules, const QString & fileName) const
{
    const QString path = getOutputPath(fileName);
    if (pRules == nullptr || path.isEmpty())
    {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    pRules->serializeObject(stream);
    if (stream.status() != QDataStream::Ok)
    {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool CounterpointTestSupport::readGameRules(GameRules* pRules, const QString & fileName) const
{
    const QString path = getOutputPath(fileName);
    if (pRules == nullptr || path.isEmpty())
    {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    pRules->deserializeObject(stream);
    return stream.status() == QDataStream::Ok && file.atEnd();
}

qint32 CounterpointTestSupport::readGameRulesVersion(const QString & fileName) const
{
    const QString path = getOutputPath(fileName);
    if (path.isEmpty())
    {
        return -1;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return -1;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    qint32 version = -1;
    stream >> version;
    if (stream.status() != QDataStream::Ok)
    {
        return -1;
    }
    return version;
}

qint32 CounterpointTestSupport::getGameRulesVersion(GameRules* pRules) const
{
    if (pRules == nullptr)
    {
        return -1;
    }
    return pRules->getVersion();
}

QString CounterpointTestSupport::fileSha256(const QString & fileName) const
{
    const QString path = getOutputPath(fileName);
    if (path.isEmpty())
    {
        return QString();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString();
    }
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

QString CounterpointTestSupport::gameMapHash(GameMap* pMap) const
{
    if (pMap == nullptr)
    {
        return QString();
    }
    return QString::fromLatin1(pMap->getMapHash().toHex());
}

bool CounterpointTestSupport::overwriteLastInt32(const QString & fileName, qint32 value) const
{
    const QString path = getOutputPath(fileName);
    if (path.isEmpty())
    {
        return false;
    }
    QFile file(path);
    const qint64 valueSize = static_cast<qint64>(sizeof(value));
    if (!file.open(QIODevice::ReadWrite) || file.size() < valueSize || !file.seek(file.size() - valueSize))
    {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    stream << value;
    return stream.status() == QDataStream::Ok && file.flush();
}

bool CounterpointTestSupport::productionQueryPreservesState(GameMap* pMap,
                                                            qint32 playerId,
                                                            Building* pBuilding,
                                                            const QString & actionId) const
{
    if (pMap == nullptr || pBuilding == nullptr || playerId < 0 || playerId >= pMap->getPlayerCount())
    {
        return false;
    }
    Player* pPlayer = pMap->getPlayer(playerId);
    auto* pCoreAi = pPlayer != nullptr ? dynamic_cast<CoreAI*>(pPlayer->getBaseGameInput()) : nullptr;
    SimpleProductionSystem* pProductionSystem = pCoreAi != nullptr ? pCoreAi->getSimpleProductionSystem() : nullptr;
    GameRecorder* pRecorder = pMap->getGameRecorder();
    if (pPlayer == nullptr || pProductionSystem == nullptr || pRecorder == nullptr)
    {
        return false;
    }

    const QByteArray hashBefore = pMap->getMapHash();
    const qint32 fundsBefore = pPlayer->getFunds();
    const qint32 unitCountBefore = pPlayer->getUnitCount();
    const qint32 fireCountBefore = pBuilding->getFireCount();
    const quint32 buildCountBefore = pRecorder->getBuildedUnits(playerId);
    qint32 actionSignalCount = 0;
    const QMetaObject::Connection actionConnection = connect(pCoreAi,
                                                             &CoreAI::sigPerformAction,
                                                             this,
                                                             [&actionSignalCount](spGameAction, bool)
                                                             {
                                                                 ++actionSignalCount;
                                                             },
                                                             Qt::DirectConnection);

    GlobalUtils::seed(PRODUCTION_DISCOVERY_SEED);
    GlobalUtils::setUseSeed(true);
    const qint32 expectedFirst = GlobalUtils::randInt(0, PRODUCTION_DISCOVERY_RANDOM_MAX);
    const qint32 expectedSecond = GlobalUtils::randInt(0, PRODUCTION_DISCOVERY_RANDOM_MAX);
    GlobalUtils::seed(PRODUCTION_DISCOVERY_SEED);
    const qint32 actualFirst = GlobalUtils::randInt(0, PRODUCTION_DISCOVERY_RANDOM_MAX);
    ProductionActionData* pData = pProductionSystem->getProductionActionData(pBuilding, actionId);
    const qint32 actualSecond = GlobalUtils::randInt(0, PRODUCTION_DISCOVERY_RANDOM_MAX);
    QObject::disconnect(actionConnection);

    return pData != nullptr &&
           actualFirst == expectedFirst &&
           actualSecond == expectedSecond &&
           pMap->getMapHash() == hashBefore &&
           pPlayer->getFunds() == fundsBefore &&
           pPlayer->getUnitCount() == unitCountBefore &&
           pBuilding->getFireCount() == fireCountBefore &&
           pRecorder->getBuildedUnits(playerId) == buildCountBefore &&
           actionSignalCount == 0;
}

bool CounterpointTestSupport::beginRandomProbe()
{
    GlobalUtils::seed(COUNTERPOINT_RANDOM_PROBE_SEED);
    GlobalUtils::setUseSeed(true);
    const qint32 expectedFirst = GlobalUtils::randInt(0, COUNTERPOINT_RANDOM_PROBE_MAX);
    m_expectedRandomValue = GlobalUtils::randInt(0, COUNTERPOINT_RANDOM_PROBE_MAX);
    GlobalUtils::seed(COUNTERPOINT_RANDOM_PROBE_SEED);
    m_randomProbeActive = GlobalUtils::randInt(0, COUNTERPOINT_RANDOM_PROBE_MAX) == expectedFirst;
    return m_randomProbeActive;
}

bool CounterpointTestSupport::finishRandomProbe()
{
    if (!m_randomProbeActive)
    {
        return false;
    }
    m_randomProbeActive = false;
    return GlobalUtils::randInt(0, COUNTERPOINT_RANDOM_PROBE_MAX) == m_expectedRandomValue;
}

bool CounterpointTestSupport::captureProductionSystem(SimpleProductionSystem* pProductionSystem)
{
    if (pProductionSystem == nullptr)
    {
        return false;
    }
    m_capturedProductionSystem.clear();
    QDataStream stream(&m_capturedProductionSystem, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    pProductionSystem->serializeObject(stream);
    return stream.status() == QDataStream::Ok && !m_capturedProductionSystem.isEmpty();
}

bool CounterpointTestSupport::restoreProductionSystem(SimpleProductionSystem* pProductionSystem)
{
    if (pProductionSystem == nullptr || m_capturedProductionSystem.isEmpty())
    {
        return false;
    }
    QDataStream stream(m_capturedProductionSystem);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    pProductionSystem->deserializeObject(stream);
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

qint32 CounterpointTestSupport::getCapturedProductionSystemSize() const
{
    return static_cast<qint32>(m_capturedProductionSystem.size());
}

bool CounterpointTestSupport::executeCounterpointBuildTargets(GameMap* pMap,
                                                              qint32 playerId,
                                                              qint32 x,
                                                              qint32 y,
                                                              const QString & unitId,
                                                              qint32 ordinal,
                                                              qint32 expectedCost) const
{
    if (pMap == nullptr || playerId < 0 || playerId >= pMap->getPlayerCount())
    {
        return false;
    }
    Player* pPlayer = pMap->getPlayer(playerId);
    auto* pCoreAi = pPlayer != nullptr ? dynamic_cast<CoreAI*>(pPlayer->getBaseGameInput()) : nullptr;
    SimpleProductionSystem* pProductionSystem = pCoreAi != nullptr ? pCoreAi->getSimpleProductionSystem() : nullptr;
    if (pProductionSystem == nullptr)
    {
        return false;
    }
    spGameAction pCapturedAction;
    const QMetaObject::Connection actionConnection = connect(pCoreAi,
                                                             &CoreAI::sigPerformAction,
                                                             this,
                                                             [&pCapturedAction](spGameAction pAction, bool)
                                                             {
                                                                 pCapturedAction = pAction;
                                                             },
                                                             Qt::DirectConnection);
    const bool executed = pProductionSystem->executeCounterpointBuild(x,
                                                                      y,
                                                                      unitId,
                                                                      ordinal,
                                                                      expectedCost);
    QObject::disconnect(actionConnection);
    QString selectedUnitId;
    if (pCapturedAction != nullptr && pCapturedAction->getVariableCount() > 0)
    {
        pCapturedAction->startReading();
        selectedUnitId = pCapturedAction->readDataString();
    }
    return executed &&
           pCapturedAction != nullptr &&
           pCapturedAction->getActionID() == CoreAI::ACTION_BUILD_UNITS &&
           pCapturedAction->getTarget() == QPoint(x, y) &&
           pCapturedAction->getCosts() == expectedCost &&
           selectedUnitId == unitId;
}

bool CounterpointTestSupport::beginProductionActionProbe(GameMap* pMap, qint32 playerId)
{
    if (pMap == nullptr || playerId < 0 || playerId >= pMap->getPlayerCount())
    {
        return false;
    }
    Player* pPlayer = pMap->getPlayer(playerId);
    auto* pCoreAi = pPlayer != nullptr ? dynamic_cast<CoreAI*>(pPlayer->getBaseGameInput()) : nullptr;
    if (pCoreAi == nullptr)
    {
        return false;
    }
    if (m_productionActionConnection)
    {
        QObject::disconnect(m_productionActionConnection);
    }
    m_productionActionId.clear();
    m_productionActionUnitId.clear();
    m_productionActionCount = 0;
    m_productionActionX = -1;
    m_productionActionY = -1;
    m_productionActionConnection = connect(pCoreAi,
                                           &CoreAI::sigPerformAction,
                                           this,
                                           [this](spGameAction pAction, bool)
                                           {
                                               if (pAction == nullptr)
                                               {
                                                   return;
                                               }
                                               ++m_productionActionCount;
                                               m_productionActionId = pAction->getActionID();
                                               m_productionActionX = pAction->getTarget().x();
                                               m_productionActionY = pAction->getTarget().y();
                                               if (pAction->getVariableCount() > 0)
                                               {
                                                   pAction->startReading();
                                                   m_productionActionUnitId = pAction->readDataString();
                                               }
                                           },
                                           Qt::DirectConnection);
    return true;
}

bool CounterpointTestSupport::finishProductionActionProbe(qint32 x,
                                                          qint32 y,
                                                          const QString & unitId)
{
    if (m_productionActionConnection)
    {
        QObject::disconnect(m_productionActionConnection);
        m_productionActionConnection = QMetaObject::Connection();
    }
    return m_productionActionCount == 1 &&
           m_productionActionId == CoreAI::ACTION_BUILD_UNITS &&
           m_productionActionX == x &&
           m_productionActionY == y &&
           m_productionActionUnitId == unitId;
}

bool CounterpointTestSupport::enableSeededProduction(GameMenue* pMenu,
                                                     qint32 playerId,
                                                     quint32 seed,
                                                     bool resetInitialProduction,
                                                     const QString & resultFileName)
{
    if (m_exitQueued || pMenu == nullptr || getOutputPath(resultFileName).isEmpty())
    {
        return false;
    }
    GameMap* pMap = pMenu->getMap();
    if (pMap == nullptr || playerId < 0 || playerId >= pMap->getPlayerCount())
    {
        return false;
    }
    Player* pPlayer = pMap->getPlayer(playerId);
    if (pPlayer == nullptr)
    {
        return false;
    }
    auto* pCoreAi = dynamic_cast<CoreAI*>(pPlayer->getBaseGameInput());
    ActionPerformer* pActionPerformer = pMenu->getActionPerformer();
    GameRecorder* pRecorder = pMap->getGameRecorder();
    if (pCoreAi == nullptr || pActionPerformer == nullptr || pRecorder == nullptr)
    {
        return false;
    }
    if (resetInitialProduction && pCoreAi->getSimpleProductionSystem() == nullptr)
    {
        return false;
    }
    // Reject observers armed after the first build because their window was missed.
    if (pRecorder->getBuildedUnits(playerId) > 0)
    {
        qInfo().noquote() << QStringLiteral("COUNTERPOINT_REASON:production-hook-armed-late");
        return false;
    }

    stopObservation();
    m_pMenu = pMenu;
    m_playerId = playerId;
    m_seed = seed;
    m_resetInitialProduction = resetInitialProduction;
    m_resultFileName = resultFileName;
    m_startFunds = pPlayer->getFunds();
    m_startBuildCount = pRecorder->getBuildedUnits(playerId);
    m_initialUnitIds.clear();
    const auto pUnits = pPlayer->getSpUnits();
    for (const auto & pUnit : pUnits->getVector())
    {
        m_initialUnitIds.insert(pUnit->getUniqueID());
    }
    m_prepared = false;
    m_observing = true;
    m_actionConnection = connect(pActionPerformer,
                                 &ActionPerformer::sigActionPerformed,
                                 this,
                                 &CounterpointTestSupport::onActionPerformed,
                                 Qt::DirectConnection);
    armWatchdog(QStringLiteral("production-observer-watchdog"));
    GlobalUtils::seed(seed);
    GlobalUtils::setUseSeed(true);
    return true;
}

void CounterpointTestSupport::finish(qint32 exitCode)
{
    if (m_exitQueued)
    {
        return;
    }
    m_exitQueued = true;
    stopObservation();
    if (exitCode == 0)
    {
        qInfo().noquote() << QStringLiteral("COUNTERPOINT_TEST_PASS");
    }
    else
    {
        qInfo().noquote() << QStringLiteral("COUNTERPOINT_TEST_FAIL:") + QString::number(exitCode);
    }
    QCoreApplication* pApp = QCoreApplication::instance();
    if (pApp != nullptr)
    {
        QMetaObject::invokeMethod(pApp, [exitCode]()
        {
            std::_Exit(exitCode);
        }, Qt::QueuedConnection);
        return;
    }
    std::_Exit(exitCode);
}

QString CounterpointTestSupport::getOutputPath(const QString & fileName) const
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

Unit* CounterpointTestSupport::findProducedUnit(Player* pPlayer) const
{
    const auto pUnits = pPlayer->getSpUnits();
    for (const auto & pUnit : pUnits->getVector())
    {
        if (!m_initialUnitIds.contains(pUnit->getUniqueID()))
        {
            return pUnit.get();
        }
    }
    return nullptr;
}

bool CounterpointTestSupport::writeJsonResult(const QJsonObject & result) const
{
    const QString path = getOutputPath(m_resultFileName);
    if (path.isEmpty())
    {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }
    const QByteArray resultData = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (file.write(resultData) != resultData.size())
    {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void CounterpointTestSupport::onActionPerformed()
{
    if (!m_observing)
    {
        return;
    }
    if (m_pMenu.isNull())
    {
        finish(EXIT_CODE_SETUP);
        return;
    }
    GlobalUtils::setUseSeed(true);
    GameMap* pMap = m_pMenu->getMap();
    if (pMap == nullptr || m_playerId < 0 || m_playerId >= pMap->getPlayerCount())
    {
        finish(EXIT_CODE_SETUP);
        return;
    }
    Player* pPlayer = pMap->getPlayer(m_playerId);
    if (pPlayer == nullptr)
    {
        finish(EXIT_CODE_SETUP);
        return;
    }
    auto* pCoreAi = dynamic_cast<CoreAI*>(pPlayer->getBaseGameInput());
    if (pCoreAi == nullptr)
    {
        finish(EXIT_CODE_SETUP);
        return;
    }
    if (!m_prepared)
    {
        if (m_resetInitialProduction)
        {
            SimpleProductionSystem* pProductionSystem = pCoreAi->getSimpleProductionSystem();
            if (pProductionSystem == nullptr)
            {
                finish(EXIT_CODE_SETUP);
                return;
            }
            pProductionSystem->resetInitialProduction();
        }
        m_prepared = true;
    }

    GameRecorder* pRecorder = pMap->getGameRecorder();
    if (pRecorder == nullptr || pRecorder->getBuildedUnits(m_playerId) <= m_startBuildCount)
    {
        return;
    }
    Unit* pUnit = findProducedUnit(pPlayer);
    if (pUnit == nullptr)
    {
        finish(EXIT_CODE_SETUP);
        return;
    }

    QJsonObject result;
    result.insert(QStringLiteral("status"), QStringLiteral("pass"));
    result.insert(QStringLiteral("unitId"), pUnit->getUnitID());
    result.insert(QStringLiteral("x"), pUnit->getX());
    result.insert(QStringLiteral("y"), pUnit->getY());
    result.insert(QStringLiteral("playerId"), m_playerId);
    result.insert(QStringLiteral("controllerType"), static_cast<qint32>(pPlayer->getBaseGameInput()->getAiType()));
    result.insert(QStringLiteral("funds"), pPlayer->getFunds());
    result.insert(QStringLiteral("fundsBefore"), m_startFunds);
    result.insert(QStringLiteral("day"), pMap->getCurrentDay());
    result.insert(QStringLiteral("seed"), static_cast<qint64>(m_seed));
    result.insert(QStringLiteral("seededMode"), GlobalUtils::getUseSeed());
    result.insert(QStringLiteral("buildCountBefore"), static_cast<qint64>(m_startBuildCount));
    result.insert(QStringLiteral("buildCountAfter"), static_cast<qint64>(pRecorder->getBuildedUnits(m_playerId)));
    result.insert(QStringLiteral("resetInitialProduction"), m_resetInitialProduction);

    const QByteArray compactResult = QJsonDocument(result).toJson(QJsonDocument::Compact);
    qInfo().noquote() << QStringLiteral("COUNTERPOINT_RESULT:") + QString::fromUtf8(compactResult);
    const bool resultWritten = writeJsonResult(result);
    finish(resultWritten ? 0 : EXIT_CODE_SETUP);
}

void CounterpointTestSupport::armWatchdog(const QString & reason)
{
    m_watchdog.setSingleShot(true);
    m_watchdog.disconnect();
    connect(&m_watchdog, &QTimer::timeout, this, [this, reason]()
    {
        qInfo().noquote() << QStringLiteral("COUNTERPOINT_REASON:") + reason;
        finish(EXIT_CODE_WATCHDOG);
    });
    m_watchdog.start(OBSERVER_WATCHDOG_MS);
}

void CounterpointTestSupport::stopObservation()
{
    m_watchdog.stop();
    if (m_actionConnection)
    {
        QObject::disconnect(m_actionConnection);
        m_actionConnection = QMetaObject::Connection();
    }
    m_observing = false;
    m_pMenu.clear();
}
