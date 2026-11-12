#include "tests/arena/playabletrialrunner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

#include <utility>

#include "3rd_party/oxygine-framework/oxygine/actor/Stage.h"
#include "ai/coordinatedai.h"
#include "ai/dummyai.h"
#include "coreengine/globalutils.h"
#include "coreengine/interpreter.h"
#include "coreengine/mainapp.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/settings.h"
#include "game/actionperformer.h"
#include "game/building.h"
#include "game/co.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/unit.h"
#include "gameinput/basegameinputif.h"
#include "menue/victorymenue.h"
#include "network/networkinterface.h"
#include "tests/arena/arenamatchrunner.h"

namespace
{
constexpr qint32 RESULT_VERSION = 1;
constexpr qint32 NATIVE_ACTOR_VERSION = 1;
const QString NATIVE_ACTOR_FIELD = QStringLiteral("nativeActor");
constexpr qint32 STARTUP_DELAY_MS = 250;
constexpr qint32 PULSE_MS = 100;
constexpr qint32 STOP_OBSERVATION_MS = 1000;
constexpr qint32 SHUTDOWN_GRACE_MS = 15000;

QString sha256(const QByteArray & bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readFile(const QString & path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}

bool writeJson(const QString & path, const QVariantMap & result)
{
    if (QFileInfo::exists(path))
    {
        return false;
    }
    QSaveFile file(path);
    const auto bytes = QJsonDocument::fromVariant(result).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

template<class T>
std::shared_ptr<T> findStageMenu()
{
    const auto & children = oxygine::Stage::getStage()->getChildren();
    for (auto child = children.rbegin(); child != children.rend(); ++child)
    {
        if (auto menu = std::dynamic_pointer_cast<T>(*child))
        {
            return menu;
        }
    }
    return {};
}

template<class T>
std::shared_ptr<T> stageMenu()
{
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
    {
        return findStageMenu<T>();
    }
    std::shared_ptr<T> result;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [&result]() { result = findStageMenu<T>(); },
                              Qt::BlockingQueuedConnection);
    return result;
}

QVariant planSequence(const BaseGameInputIF* input)
{
    if (const auto* coordinated = dynamic_cast<const CoordinatedAi*>(input))
    {
        return QString::number(coordinated->testPlanSequence());
    }
    return {};
}

qint32 nativeOwnerIndex(GameMap & map, const Player* owner)
{
    constexpr qint32 SUPPORTED_PLAYERS = 2;
    constexpr qint32 UNKNOWN_OWNER = -2;
    if (owner == nullptr)
    {
        return -1;
    }
    if (map.getPlayerCount() != SUPPORTED_PLAYERS)
    {
        return UNKNOWN_OWNER;
    }
    for (qint32 index = 0; index < SUPPORTED_PLAYERS; ++index)
    {
        if (map.getPlayer(index) == owner)
        {
            return index;
        }
    }
    return UNKNOWN_OWNER;
}

QVariantMap nativeActorState(GameMap & map, Unit & actor)
{
    const auto x = actor.getX();
    const auto y = actor.getY();
    const bool onMap = actor.getMap() == &map && map.onMap(x, y) &&
        map.getTerrain(x, y) == actor.getTerrain() && map.getTerrain(x, y)->getUnit() == &actor;
    return {{"id", actor.getUniqueID()}, {"owner", nativeOwnerIndex(map, actor.getOwner())},
        {"x", x}, {"y", y}, {"hp", actor.getHp()}, {"fuel", actor.getFuel()},
        {"moved", actor.getHasMoved()}, {"onMap", onMap}};
}

bool reject(QString & error, const QString & reason)
{
    error = reason;
    return false;
}
}

bool PlayableTrialRunner::shouldRun()
{
    return qgetenv(PlayableTrialConfig::ENVIRONMENT_FLAG) == "1" &&
        QFileInfo(QDir(Settings::getInstance()->getUserPath()).filePath(PlayableTrialConfig::FILE_NAME)).isFile();
}

PlayableTrialRunner::PlayableTrialRunner(spMainwindow window, QObject* parent)
    : QObject(parent), m_startWindow(std::move(window))
{
}

void PlayableTrialRunner::run()
{
    m_profile = Settings::getInstance()->getUserPath();
    connect(&m_watchdog, &QTimer::timeout, this, &PlayableTrialRunner::onWatchdog);
    connect(&m_pulse, &QTimer::timeout, this, &PlayableTrialRunner::onPulse);
    m_watchdog.setSingleShot(true);
    m_pulse.start(PULSE_MS);
    m_result.insert("mainWindowCreated", m_startWindow != nullptr);
    if (!prepare())
    {
        return;
    }
    m_watchdog.start(m_config.fixture.watchdogMs);
    if (m_config.mode == PlayableTrialMode::Resume)
    {
        QTimer::singleShot(STARTUP_DELAY_MS, this, &PlayableTrialRunner::startResume);
    }
    else
    {
        QTimer::singleShot(STARTUP_DELAY_MS, this, &PlayableTrialRunner::startFresh);
    }
}

bool PlayableTrialRunner::prepare()
{
    QString error;
    if (!validateRuntime(error) || !PlayableTrialConfig::load(m_profile, m_config, error))
    {
        finish(Outcome::SetupFailure, error);
        return false;
    }
    m_master = Coordinator::readMasterShadowConfig();
    if (!m_master.controlsStock() || m_master.runtime != Coordinator::MasterStockControlRuntime::PlayableTrial ||
        (m_master.exportMode != Coordinator::MasterShadowExportMode::Full &&
         m_master.exportMode != Coordinator::MasterShadowExportMode::Off))
    {
        finish(Outcome::SetupFailure, "trial requires native StockControl with Full or Off export");
        return false;
    }
    if (!validateProofOutput(error))
    {
        finish(Outcome::SetupFailure, error);
        return false;
    }
    m_actions.setFileName(localPath(m_config.actionsFile));
    if (!m_actions.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        finish(Outcome::SetupFailure, "cannot create exclusive action receipt");
        return false;
    }
    m_executableSha256 = sha256(readFile(QCoreApplication::applicationFilePath()));
    m_result.insert("nativeConfig", nativeConfig());
    m_result.insert("requestSha256", sha256(readFile(localPath(PlayableTrialConfig::FILE_NAME))));
    m_result.insert("fixtureSha256", sha256(readFile(localPath(m_config.fixtureFile))));
    m_result.insert("mapSha256", sha256(readFile(m_config.fixture.mapPath)));
    m_stopBoundary.configure(m_config.fixture);
    m_checkpointBoundary.configure(m_config.checkpointBoundary);
    if (m_config.mode == PlayableTrialMode::Resume && !validateResume(error))
    {
        finish(Outcome::SetupFailure, error);
        return false;
    }
    GlobalUtils::seed(m_config.fixture.masterSeed);
    GlobalUtils::setUseSeed(true);
    GameAction::pinActionSeeds(m_config.fixture.masterSeed);
    if (m_config.mode == PlayableTrialMode::Resume)
    {
        if (!ArenaMatchRunner::applyFixtureRuntime(m_config.fixture, error))
        {
            finish(Outcome::SetupFailure, error);
            return false;
        }
        m_result.insert("fixtureRuntimeApplied", true);
    }
    return true;
}

bool PlayableTrialRunner::validateRuntime(QString & error)
{
    QString explicitPath;
    auto* app = Mainapp::getInstance();
    auto* settings = Settings::getInstance();
    if (qEnvironmentVariableIsSet("COW_AI_ARENA_TEST") ||
        qgetenv("COW_MASTER_ASSIGNMENT_TRIAL") != "1" || app->getNoUi() ||
        app->getNoAudio() ||
        settings->getSpawnAiProcess() || settings->getAiSlave() || Mainapp::getSlave() ||
        !app->getParser().getUserPath(explicitPath) ||
        !Coordinator::isolatedMasterTrialPath(QCoreApplication::applicationDirPath(), explicitPath, m_profile))
    {
        return reject(error, "playable test requires isolated graphical/audio in-process trial with arena flag absent");
    }
    return true;
}

bool PlayableTrialRunner::validateProofOutput(QString & error) const
{
    if (m_master.exportMode == Coordinator::MasterShadowExportMode::Off)
    {
        return true;
    }
    if (QFileInfo(m_master.outputPath).absolutePath() != QDir(m_profile).absolutePath() ||
        QFileInfo::exists(m_master.outputPath))
    {
        return reject(error, "Full export requires a new proof file in this profile");
    }
    const QStringList reservedNames{PlayableTrialConfig::FILE_NAME, m_config.fixtureFile, m_config.actionsFile,
                                    m_config.resultFile, m_config.saveFile, m_config.checkpointFile};
    for (const auto & name : reservedNames)
    {
        if (!name.isEmpty() && QFileInfo(m_master.outputPath).fileName().compare(name, Qt::CaseInsensitive) == 0)
        {
            return reject(error, "proof file conflicts with a trial input or output");
        }
    }
    return true;
}

bool PlayableTrialRunner::validateResume(QString & error)
{
    const auto document = QJsonDocument::fromJson(readFile(localPath(m_config.checkpointFile)));
    if (!document.isObject())
    {
        return reject(error, "checkpoint is not a JSON object");
    }
    m_checkpoint = document.object().toVariantMap();
    if (m_checkpoint.value("version").toInt() != RESULT_VERSION ||
        m_checkpoint.value("saveFile").toString() != m_config.saveFile ||
        m_checkpoint.value("saveSha256").toString() != sha256(readFile(localPath(m_config.saveFile))) ||
        m_checkpoint.value("fixtureSha256") != m_result.value("fixtureSha256") ||
        m_checkpoint.value("mapSha256") != m_result.value("mapSha256"))
    {
        return reject(error, "save bytes, fixture, or map input differ from the checkpoint");
    }
    const auto previousConfig = m_checkpoint.value("nativeConfig").toMap();
    const auto currentConfig = nativeConfig();
    for (const auto & key : {"mode", "runtime", "error", "allowance", "tierAudit", "export",
                             "executableSha256", "actionSeedProtocol"})
    {
        if (!previousConfig.contains(key) || previousConfig.value(key) != currentConfig.value(key))
        {
            return reject(error, QString("native resume configuration differs: ") + key);
        }
    }
    return true;
}

void PlayableTrialRunner::startFresh()
{
    QString error;
    const auto map = ArenaMatchRunner::prepareFixture(m_config.fixture, error);
    if (map == nullptr)
    {
        finish(Outcome::SetupFailure, error);
        return;
    }
    m_result.insert("fixtureRuntimeApplied", true);
    if (map->getCampaign() != nullptr)
    {
        finish(Outcome::SetupFailure, "campaign fixtures are outside the single-player trial");
        return;
    }
    map->initPlayersAndSelectCOs();
    auto menu = MemoryManagement::create<GameMenue>(map, false, spNetworkInterface());
    oxygine::Stage::getStage()->addChild(menu);
    if (!bindMenu(menu.get(), false, error))
    {
        finish(Outcome::SetupFailure, error);
        return;
    }
    m_startWindow->leaveMenue();
}

void PlayableTrialRunner::startResume()
{
    m_startWindow->loadGame(localPath(m_config.saveFile));
    auto* menu = qobject_cast<GameMenue*>(Interpreter::getInstance()->getGlobal("game").toQObject());
    QString error;
    if (!bindMenu(menu, true, error))
    {
        finish(Outcome::SetupFailure, error);
    }
}

bool PlayableTrialRunner::bindMenu(GameMenue* menu, bool loaded, QString & error)
{
    if (menu == nullptr || menu->getMap() == nullptr || menu->getNetworkInterface() != nullptr ||
        menu->getMap()->getCampaign() != nullptr)
    {
        return reject(error, "normal single-player menu was not created");
    }
    m_menu = menu;
    auto* map = menu->getMap();
    if (map->getPlayerCount() != m_config.fixture.players.size())
    {
        return reject(error, "loaded player count differs");
    }
    for (qint32 player = 0; player < map->getPlayerCount(); ++player)
    {
        auto* input = map->getPlayer(player)->getBaseGameInput();
        if (input == nullptr || dynamic_cast<DummyAi*>(input) != nullptr ||
            input->getAiType() != m_config.fixture.players.at(player).aiType)
        {
            return reject(error, "loaded controller differs from the requested local AI");
        }
        if (auto* coordinated = dynamic_cast<CoordinatedAi*>(input))
        {
            if (!coordinated->testTierAuditEnabled())
            {
                return reject(error, "local controller does not use the ordinary Full fixture audit mode");
            }
            m_connections.append(connect(coordinated, &CoordinatedAi::sigStockControlStopped, this,
                                         &PlayableTrialRunner::onStockControlStopped, Qt::DirectConnection));
        }
    }
    if (loaded && (state().value("mapHash") != m_checkpoint.value("state").toMap().value("mapHash") ||
                   controllers() != m_checkpoint.value("controllers").toList()))
    {
        return reject(error, "loaded map state or controller identities differ from the saved checkpoint");
    }
    if (loaded)
    {
        for (qint32 player = 0; player < map->getPlayerCount(); ++player)
        {
            const auto sequence = planSequence(map->getPlayer(player)->getBaseGameInput());
            if (sequence.isValid() && sequence.toString() != "0")
            {
                return reject(error, "loaded controller planned before the new receipt session was attached");
            }
        }
    }
    auto* performer = menu->getActionPerformer();
    m_connections.append(connect(performer, &ActionPerformer::sigAiProcesseSendAction, this,
                                 &PlayableTrialRunner::onAcceptedAction, Qt::DirectConnection));
    m_connections.append(connect(performer, &ActionPerformer::sigActionPerformed, this,
                                 &PlayableTrialRunner::onActionCompleted, Qt::DirectConnection));
    m_connections.append(connect(menu, &GameMenue::sigVictory, this,
                                 &PlayableTrialRunner::onVictory, Qt::DirectConnection));
    m_connections.append(connect(map->getGameRules(), &GameRules::sigVictory, this,
                                 &PlayableTrialRunner::onVictory, Qt::DirectConnection));
    m_phase = Phase::Running;
    startSession(loaded);
    return true;
}

void PlayableTrialRunner::startSession(bool loaded)
{
    m_sessionActionStart = m_actionCount;
    m_session = {
        {"sessionId", m_config.sessionId + "/" + QString::number(m_sessions.size())},
        {"loaded", loaded}, {"loadedStateChecked", loaded},
        {"initialState", state()}, {"initialRoster", roster()},
        {"actionByteStart", QString::number(m_actions.pos())},
        {"proofExportEnabled", m_master.exportMode == Coordinator::MasterShadowExportMode::Full},
        {"nativeConfig", nativeConfig()}, {"controllers", controllers()},
        {"fixtureRuntimeApplied", m_result.value("fixtureRuntimeApplied")},
    };
    if (m_master.exportMode == Coordinator::MasterShadowExportMode::Full)
    {
        m_proofStart = QFileInfo(m_master.outputPath).exists() ? QFileInfo(m_master.outputPath).size() : 0;
        m_session.insert("proofFile", m_master.outputPath);
        m_session.insert("proofByteStart", QString::number(m_proofStart));
    }
    if (loaded)
    {
        m_session.insert("startupBoundary", m_session.value("initialState"));
        m_session.insert("entryBoundary", "postStartupBeforeQueuedPlanning");
    }
    else
    {
        m_session.insert("entryBoundary", "beforeNormalStartupAction");
    }
}

void PlayableTrialRunner::closeSession()
{
    if (m_session.isEmpty())
    {
        return;
    }
    m_session.insert("finalState", state());
    m_session.insert("finalRoster", roster());
    m_session.insert("actionCount", QString::number(m_actionCount - m_sessionActionStart));
    m_session.insert("actionByteEnd", QString::number(m_actions.pos()));
    if (m_master.exportMode == Coordinator::MasterShadowExportMode::Full)
    {
        const auto proof = readFile(m_master.outputPath);
        m_session.insert("proofByteEnd", QString::number(proof.size()));
        m_session.insert("proofSegmentSha256", sha256(proof.mid(m_proofStart)));
    }
    m_session.insert("noPendingAction", m_pendingAction.isEmpty());
    m_sessions.append(m_session);
    m_session.clear();
}

void PlayableTrialRunner::onAcceptedAction(spGameAction action)
{
    if (m_phase != Phase::Running || !m_pendingAction.isEmpty() || action == nullptr)
    {
        finish(Outcome::LinkageFailure, "accepted action outside a running quiescent session");
        return;
    }
    auto* map = m_menu->getMap();
    auto* player = map->getCurrentPlayer();
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    action->serializeObject(stream);
    const auto target = action->getTarget();
    m_pendingAction = {
        {"sessionId", m_session.value("sessionId")}, {"index", QString::number(m_actionCount)},
        {"day", map->getCurrentDay()}, {"player", player->getPlayerID()},
        {"planSequence", planSequence(player->getBaseGameInput())},
        {"actionId", action->getActionID()}, {"target", QVariantMap{{"x", target.x()}, {"y", target.y()}}},
        {"seed", QString::number(action->getSeed())},
        {"payloadBase64", QString::fromLatin1(bytes.toBase64())}, {"payloadSha256", sha256(bytes)},
        {"preState", state()}, {"acceptedByNormalPerformer", true},
    };
    observeActorBefore(*action);
    if (!observeCaptureBefore(*action))
    {
        finish(Outcome::LinkageFailure, "native capture objects unavailable at accepted action");
        return;
    }
    m_stopBoundary.observeAction(map->getCurrentDay(), player->getPlayerID());
    m_checkpointBoundary.observeAction(map->getCurrentDay(), player->getPlayerID());
}

bool PlayableTrialRunner::completePendingAction()
{
    if (m_pendingAction.isEmpty())
    {
        return true;
    }
    observeActorAfter();
    observeCaptureAfter();
    m_pendingAction.insert("postState", state());
    m_pendingAction.insert("completed", true);
    const auto bytes = QJsonDocument::fromVariant(m_pendingAction).toJson(QJsonDocument::Compact) + '\n';
    if (m_actions.write(bytes) != bytes.size() || !m_actions.flush())
    {
        finish(Outcome::LinkageFailure, "action receipt write failed");
        return false;
    }
    ++m_actionCount;
    m_pendingAction.clear();
    m_actionActor.clear();
    m_pendingCapture.reset();
    m_captureActor.clear();
    m_captureProperty.clear();
    return true;
}

void PlayableTrialRunner::observeActorBefore(GameAction & action)
{
    m_actionActor = action.getTargetUnit();
    if (m_actionActor)
    {
        m_pendingAction.insert(NATIVE_ACTOR_FIELD, QVariantMap{
            {"version", NATIVE_ACTOR_VERSION}, {"boundary", NativeCaptureReceipt::BOUNDARY},
            {"actorId", m_actionActor->getUniqueID()}, {"before", nativeActorState(*m_menu->getMap(), *m_actionActor)}});
    }
}

void PlayableTrialRunner::observeActorAfter()
{
    if (!m_pendingAction.contains(NATIVE_ACTOR_FIELD))
    {
        return;
    }
    auto record = m_pendingAction.value(NATIVE_ACTOR_FIELD).toMap();
    const bool quiescent = !m_menu->getActionPerformer()->getActionRunning() && m_menu->getSaveAllowed();
    record.insert("quiescent", quiescent);
    record.insert("nativeObjectLive", !m_actionActor.isNull());
    record.insert("after", m_actionActor && quiescent
        ? QVariant(nativeActorState(*m_menu->getMap(), *m_actionActor)) : QVariant{});
    m_pendingAction.insert(NATIVE_ACTOR_FIELD, record);
}

bool PlayableTrialRunner::observeCaptureBefore(GameAction & action)
{
    m_pendingCapture.reset();
    m_captureActor.clear();
    m_captureProperty.clear();
    if (action.getActionID() != CoreAI::ACTION_CAPTURE)
    {
        return true;
    }
    auto* map = m_menu->getMap();
    const auto target = action.getActionTarget();
    if (!map->onMap(target.x(), target.y()))
    {
        return false;
    }
    auto* actor = action.getTargetUnit();
    auto* property = map->getTerrain(target.x(), target.y())->getBuilding();
    if (actor == nullptr || property == nullptr)
    {
        return false;
    }
    m_captureActor = actor;
    m_captureProperty = property;
    m_pendingCapture = NativeCaptureReceipt::Observation{
        actor->getUniqueID(), target,
        {property->getBuildingID(), nativeOwnerIndex(*map, property->getOwner()),
         nativeOwnerIndex(*map, actor->getOwner()), actor->getCapturePoints()}, {}, false, false};
    return true;
}

void PlayableTrialRunner::observeCaptureAfter()
{
    if (!m_pendingCapture)
    {
        return;
    }
    auto* map = m_menu->getMap();
    const auto target = m_pendingCapture->target;
    if (m_captureActor && m_captureProperty && map->onMap(target.x(), target.y()) &&
        map->getTerrain(target.x(), target.y())->getBuilding() == m_captureProperty.data())
    {
        m_pendingCapture->after = NativeCaptureReceipt::State{
            m_captureProperty->getBuildingID(), nativeOwnerIndex(*map, m_captureProperty->getOwner()),
            nativeOwnerIndex(*map, m_captureActor->getOwner()), m_captureActor->getCapturePoints()};
        m_pendingCapture->actorOnTarget = m_captureActor->getTerrain() == map->getTerrain(target.x(), target.y());
        m_pendingCapture->actionConsumed = m_captureActor->getHasMoved();
    }
    m_pendingAction.insert(NativeCaptureReceipt::FIELD, NativeCaptureReceipt::record(*m_pendingCapture));
}

void PlayableTrialRunner::onActionCompleted()
{
    if (m_phase != Phase::Running)
    {
        return;
    }
    if (m_menu->getActionPerformer()->getActionRunning() || !m_menu->getSaveAllowed())
    {
        finish(Outcome::LinkageFailure, "completion was not quiescent");
        return;
    }
    if (!completePendingAction())
    {
        return;
    }
    if (!m_session.contains("startupBoundary"))
    {
        m_session.insert("startupBoundary", state());
    }
    auto* map = m_menu->getMap();
    const auto day = map->getCurrentDay();
    const auto player = map->getCurrentPlayer()->getPlayerID();
    const bool checkpointMode = m_config.mode == PlayableTrialMode::Save || m_config.mode == PlayableTrialMode::Reload;
    if (checkpointMode && !m_checkpointSaved && m_checkpointBoundary.shouldStop(day, player))
    {
        if (!saveCheckpoint())
        {
            finish(Outcome::LinkageFailure, "normal save or checkpoint verification failed");
        }
        else if (m_config.mode == PlayableTrialMode::Reload)
        {
            reloadCheckpoint();
        }
        else
        {
            finish(Outcome::Complete, "saved at completed requested turn boundary");
        }
        return;
    }
    if (m_stopBoundary.shouldStop(day, player))
    {
        finish(m_config.expectedStop.isEmpty() ? Outcome::Complete : Outcome::BoundaryMissed,
               "completed requested turn boundary");
    }
}

bool PlayableTrialRunner::saveCheckpoint()
{
    const auto before = state();
    m_menu->saveMap(localPath(m_config.saveFile), false);
    const auto save = readFile(localPath(m_config.saveFile));
    if (save.isEmpty() || state().value("mapHash") != before.value("mapHash"))
    {
        return false;
    }
    m_checkpoint = {
        {"version", RESULT_VERSION}, {"sessionId", m_session.value("sessionId")},
        {"saveFile", m_config.saveFile}, {"saveSha256", sha256(save)}, {"state", before},
        {"controllers", controllers()}, {"roster", roster()},
        {"fixtureSha256", m_result.value("fixtureSha256")}, {"mapSha256", m_result.value("mapSha256")},
        {"nativeConfig", nativeConfig()},
        {"completedActionCount", QString::number(m_actionCount)},
        {"afterDay", m_config.checkpointBoundary.stopAfterDay},
        {"afterPlayer", m_config.checkpointBoundary.stopAfterPlayer},
    };
    m_checkpointSaved = writeJson(localPath(m_config.checkpointFile), m_checkpoint);
    m_result.insert("checkpoint", m_checkpoint);
    return m_checkpointSaved;
}

void PlayableTrialRunner::reloadCheckpoint()
{
    m_menu->setGameStarted(false);
    closeSession();
    for (const auto & connection : m_connections)
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();
    m_menu->loadSaveGame(localPath(m_config.saveFile));
    auto* loaded = qobject_cast<GameMenue*>(Interpreter::getInstance()->getGlobal("game").toQObject());
    QString error;
    if (!bindMenu(loaded, true, error))
    {
        finish(Outcome::LinkageFailure, error);
    }
}

void PlayableTrialRunner::onStockControlStopped(const QString & reason)
{
    if (m_phase != Phase::Running || !m_pendingAction.isEmpty())
    {
        finish(Outcome::LinkageFailure, "StockControl stopped outside an idle action boundary");
        return;
    }
    m_stopReason = reason;
    m_stoppedActionCount = m_actionCount;
    m_stoppedState = m_menu->getMap()->getMapHash();
    m_phase = Phase::ObservingStop;
    m_transitionTimer.restart();
    m_result.insert("stopSignal", QVariantMap{
        {"reason", reason}, {"state", state()}, {"actionCount", QString::number(m_actionCount)},
        {"gameStarted", m_menu->getGameStarted()},
    });
}

void PlayableTrialRunner::onVictory(qint32)
{
    if (m_phase == Phase::Running)
    {
        finish(Outcome::BoundaryMissed, "victory occurred before the requested completed turn boundary");
    }
}

void PlayableTrialRunner::onPulse()
{
    if (m_phase == Phase::Running && m_menu != nullptr && m_menu->getGameStarted() &&
        Interpreter::getInstance()->getGlobal("game").toQObject() != m_menu)
    {
        finish(Outcome::LinkageFailure, "active normal menu lost its interpreter registration");
    }
    if (m_phase == Phase::ObservingStop && m_transitionTimer.elapsed() >= STOP_OBSERVATION_MS)
    {
        const bool stopped = m_menu != nullptr && !m_menu->getGameStarted() &&
            !m_menu->getActionPerformer()->getActionRunning() && m_actionCount == m_stoppedActionCount &&
            m_menu->getMap()->getMapHash() == m_stoppedState;
        m_result.insert("stopObservationMs", m_transitionTimer.elapsed());
        m_result.insert("noActionAfterStop", stopped);
        finish(stopped && m_stopReason == m_config.expectedStop ? Outcome::ExpectedStop : Outcome::LinkageFailure,
               m_stopReason);
    }
    if (m_phase != Phase::Closing)
    {
        return;
    }
    if (!m_victoryExited)
    {
        if (const auto victory = stageMenu<VictoryMenue>())
        {
            m_victoryExited = true;
            victory->exitMenue();
        }
    }
    if (const auto window = stageMenu<Mainwindow>(); window && (m_menu == nullptr || window != m_startWindow))
    {
        completeShutdown(window.get(), true);
    }
    else if (m_transitionTimer.elapsed() >= SHUTDOWN_GRACE_MS)
    {
        m_outcome = Outcome::Watchdog;
        m_reason += "; normal menu shutdown grace expired";
        completeShutdown(m_startWindow.get(), false);
    }
}

void PlayableTrialRunner::onWatchdog()
{
    finish(Outcome::Watchdog, "playable lifecycle watchdog expired");
}

void PlayableTrialRunner::finish(Outcome outcome, const QString & reason)
{
    if (m_phase == Phase::Quitting)
    {
        return;
    }
    if (m_phase == Phase::Closing)
    {
        m_outcome = Outcome::LinkageFailure;
        m_reason += "; " + reason;
        return;
    }
    m_outcome = outcome;
    m_reason = reason;
    m_watchdog.stop();
    m_phase = Phase::Closing;
    m_transitionTimer.restart();
    closeSession();
    if (m_menu != nullptr)
    {
        m_menu->setGameStarted(false);
        m_menu->exitGame();
    }
}

void PlayableTrialRunner::completeShutdown(Mainwindow* window, bool returnedToMainMenu)
{
    m_phase = Phase::Quitting;
    m_pulse.stop();
    m_result.insert("returnedToMainMenu", returnedToMainMenu);
    m_result.insert("gameMenuExitRequested", m_menu != nullptr || !m_sessions.isEmpty());
    m_result.insert("mainMenuQuitRequested", window != nullptr);
    if (!writeResult())
    {
        qCritical() << "Playable trial result could not be written:" << m_reason;
    }
    if (window != nullptr)
    {
        window->quitGame();
    }
}

QVariantList PlayableTrialRunner::controllers() const
{
    QVariantList result;
    if (m_menu == nullptr)
    {
        return result;
    }
    auto* map = m_menu->getMap();
    for (qint32 index = 0; index < map->getPlayerCount(); ++index)
    {
        auto* player = map->getPlayer(index);
        auto* input = player->getBaseGameInput();
        result.append(QVariantMap{
            {"player", index}, {"controlType", static_cast<int>(player->getControlType())},
            {"inputType", input == nullptr ? -1 : static_cast<int>(input->getAiType())},
            {"coordinated", dynamic_cast<CoordinatedAi*>(input) != nullptr},
            {"dummy", dynamic_cast<DummyAi*>(input) != nullptr},
        });
    }
    return result;
}

QVariantList PlayableTrialRunner::roster() const
{
    QVariantList result;
    if (m_menu == nullptr)
    {
        return result;
    }
    auto* map = m_menu->getMap();
    for (qint32 index = 0; index < map->getPlayerCount(); ++index)
    {
        auto* player = map->getPlayer(index);
        QVariantList units;
        const auto ownedUnits = player->getSpUnits();
        for (qint32 unitIndex = 0; unitIndex < ownedUnits->size(); ++unitIndex)
        {
            auto* unit = ownedUnits->at(unitIndex);
            units.append(QVariantMap{
                {"id", unit->getUniqueID()}, {"type", unit->getUnitID()},
                {"x", unit->getX()}, {"y", unit->getY()}, {"hp", unit->getHp()},
            });
        }
        result.append(QVariantMap{
            {"player", index}, {"team", player->getTeam()}, {"funds", player->getFunds()},
            {"buildList", player->getBuildList()}, {"units", units},
            {"buildingCount", player->getBuildingCount()}, {"defeated", player->getIsDefeated()},
            {"co", player->getCO(0) == nullptr ? QString() : player->getCO(0)->getCoID()},
            {"planSequence", planSequence(player->getBaseGameInput())},
        });
    }
    return result;
}

QVariantMap PlayableTrialRunner::state() const
{
    if (m_menu == nullptr || m_menu->getMap() == nullptr)
    {
        return {};
    }
    auto* map = m_menu->getMap();
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    map->serializeObject(stream);
    return {
        {"day", map->getCurrentDay()},
        {"player", map->getCurrentPlayer() == nullptr ? -1 : map->getCurrentPlayer()->getPlayerID()},
        {"mapHash", QString::fromLatin1(map->getMapHash().toHex())},
        {"serializedSha256", sha256(bytes)}, {"serializedBase64", QString::fromLatin1(bytes.toBase64())},
        {"controllers", controllers()},
    };
}

QVariantMap PlayableTrialRunner::nativeConfig() const
{
    return {
        {"mode", static_cast<int>(m_master.mode)}, {"runtime", static_cast<int>(m_master.runtime)},
        {"error", static_cast<int>(m_master.error)}, {"allowance", QString::number(m_master.allowance)},
        {"tierAudit", static_cast<int>(m_master.tierAuditMode)}, {"export", static_cast<int>(m_master.exportMode)},
        {"profile", m_profile}, {"application", QCoreApplication::applicationFilePath()},
        {"executableSha256", m_executableSha256},
        {"arguments", QCoreApplication::arguments()}, {"arenaFlagPresent", qEnvironmentVariableIsSet("COW_AI_ARENA_TEST")},
        {"spawnAiProcess", Settings::getInstance()->getSpawnAiProcess()},
        {"noUi", Mainapp::getInstance()->getNoUi()}, {"actionSeedProtocol", "fresh-process-reset-only"},
        {"movementAnimations", Settings::getInstance()->getMovementAnimations()},
        {"captureAnimation", Settings::getInstance()->getCaptureAnimation()},
        {"battleAnimationMode", static_cast<int>(Settings::getInstance()->getBattleAnimationMode())},
        {"battleAnimationType", static_cast<int>(Settings::getInstance()->getBattleAnimationType())},
        {"pauseAfterAction", Settings::getInstance()->getPauseAfterAction()},
        {"animationSpeed", Settings::getInstance()->getAnimationSpeedValue()},
        {"battleAnimationSpeed", Settings::getInstance()->getBattleAnimationSpeedValue()},
        {"fixtureTierAudit", m_config.fixture.coordinatedTierAudit},
        {"deterministicCounterpointSeed", m_config.fixture.deterministicCounterpointSeed},
    };
}

QString PlayableTrialRunner::localPath(const QString & name) const
{
    return QDir(m_profile).filePath(name);
}

bool PlayableTrialRunner::writeResult()
{
    static const QStringList outcomes{"complete", "expectedStop", "setupFailure", "linkageFailure",
                                      "boundaryMissed", "watchdog"};
    m_actions.close();
    m_result.insert("version", RESULT_VERSION);
    m_result.insert("sessionId", m_config.sessionId);
    m_result.insert("outcome", outcomes.at(static_cast<int>(m_outcome)));
    m_result.insert("reason", m_reason);
    m_result.insert("pass", m_outcome == Outcome::Complete || m_outcome == Outcome::ExpectedStop);
    m_result.insert("sessions", m_sessions);
    m_result.insert("actionCount", QString::number(m_actionCount));
    m_result.insert("actionsFile", m_config.actionsFile);
    m_result.insert("actionsSha256", sha256(readFile(localPath(m_config.actionsFile))));
    m_result.insert("normalExitRequired", true);
    m_result.insert("actionScope", "accepted nonlocal actions; initial startup NEXT_PLAYER is a separate state boundary");
    m_result.insert("actionPrefixApplied", false);
    m_result.insert("stopAfterDay", m_config.fixture.stopAfterDay);
    m_result.insert("stopAfterPlayer", m_config.fixture.stopAfterPlayer);
    if (m_config.mode == PlayableTrialMode::Save)
    {
        m_result.insert("stopAfterDay", m_config.checkpointBoundary.stopAfterDay);
        m_result.insert("stopAfterPlayer", m_config.checkpointBoundary.stopAfterPlayer);
    }
    return !m_config.resultFile.isEmpty() && writeJson(localPath(m_config.resultFile), m_result);
}
