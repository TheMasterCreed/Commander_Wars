#include "tests/arena/arenamatchrunner.h"

#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJSValue>
#include <QVariantMap>

#include "3rd_party/oxygine-framework/oxygine/actor/Stage.h"
#include "ai/coreai.h"
#include "ai/coordinatedai.h"
#include "ai/dummyai.h"
#include "coreengine/gameconsole.h"
#include "coreengine/filesupport.h"
#include "coreengine/globalutils.h"
#include "coreengine/interpreter.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/settings.h"
#include "game/actionperformer.h"
#include "game/gameaction.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/victoryrule.h"
#include "gameinput/basegameinputif.h"
#include "network/networkinterface.h"
#include "resource_management/cospritemanager.h"
#include "resource_management/gamerulemanager.h"
#include "tests/arena/arenatestsupport.h"
#include "tests/arena/relativestockprobe.h"

namespace
{
const QString SETUP_PREFIX = QStringLiteral("ARENA_MATCH_SETUP:");
const QString TURN_LIMIT_RULE = QStringLiteral("VICTORYRULE_TURNLIMIT");
constexpr qint32 TURN_LIMIT_ITEM = 0;
constexpr qint32 RESULT_VERSION = 2;
constexpr quint8 PRIMARY_CO = 0;
constexpr quint8 SECONDARY_CO = 1;

QString fileSha256(const QString & path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(),
                                                        QCryptographicHash::Sha256).toHex());
}

void quiesceAiTurnQueue(GameMap* pMap, ActionPerformer* pPerformer)
{
    for (qint32 i = 0; i < pMap->getPlayerCount(); ++i)
    {
        auto* pAi = dynamic_cast<CoreAI*>(pMap->getPlayer(i)->getBaseGameInput());
        if (pAi == nullptr)
        {
            continue;
        }
        QObject::disconnect(pPerformer, &ActionPerformer::sigActionPerformed,
                            pAi, &CoreAI::nextAction);
        QCoreApplication::removePostedEvents(pAi, QEvent::MetaCall);
    }
}
}

bool ArenaMatchRunner::shouldRun()
{
    return qEnvironmentVariable(AiArenaTestSupport::ENVIRONMENT_FLAG) == "1" &&
           QFile::exists(configPath());
}

QString ArenaMatchRunner::configPath()
{
    return QDir(Settings::getInstance()->getUserPath()).filePath(ArenaMatchConfig::FILE_NAME);
}

ArenaMatchRunner::ArenaMatchRunner(QObject* pParent)
    : QObject(pParent)
{
}

spGameMap ArenaMatchRunner::prepareFixture(const ArenaMatchConfig & config, QString & error)
{
    ArenaMatchRunner fixture;
    fixture.m_config = config;
    if (!fixture.loadMap(error) || !fixture.applyPlayerSetup(error) || !fixture.applyGameRules(error))
    {
        return {};
    }
    return fixture.m_pMap;
}

void ArenaMatchRunner::run()
{
    if (!loadConfig())
    {
        return;
    }
    GlobalUtils::seed(m_config.masterSeed);
    GlobalUtils::setUseSeed(true);
    GameAction::pinActionSeeds(m_config.masterSeed);
    QString error;
    if (!loadMap(error) || !applyPlayerSetup(error) || !applyGameRules(error))
    {
        failSetup(error);
        return;
    }
    setStockControlPrefixHold(m_actionPrefix && !m_actionPrefix->complete());
    Settings::getInstance()->setAutoSavingCycle(0);
    Settings::getInstance()->setRecord(false);
    m_stopBoundary.configure(m_config);
    m_pMap->initPlayersAndSelectCOs();
    startMenue();
    spliceActionHandling();
    spliceTerminalHandling();
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &ArenaMatchRunner::onWatchdog);
    m_watchdog.start(m_config.watchdogMs);
}

bool ArenaMatchRunner::loadConfig()
{
    QString error;
    if (!ArenaMatchConfig::load(configPath(), m_config, error))
    {
        failSetup(error);
        return false;
    }
    if (!m_config.actionPrefixFile.isEmpty())
    {
        m_actionPrefix = std::make_unique<ArenaActionPrefix>();
        const auto path = QDir(Settings::getInstance()->getUserPath()).filePath(m_config.actionPrefixFile);
        if (!m_actionPrefix->load(path, error) || m_actionPrefix->initialSeed() != m_config.masterSeed)
        {
            failSetup(error.isEmpty() ? "masterSeed must match the recorded initial action seed" : error);
            return false;
        }
    }
    return true;
}

bool ArenaMatchRunner::loadMap(QString & error)
{
    m_pMap = MemoryManagement::create<GameMap>(m_config.mapPath, true, false, false);
    if (m_pMap->getPlayerCount() <= 0 || !m_config.validatePlayerCount(m_pMap->getPlayerCount(), error))
    {
        error = error.isEmpty() ? "map loaded without players" : error;
        return false;
    }
    return true;
}

bool ArenaMatchRunner::applyPlayerSetup(QString & error)
{
    if (!m_config.coordinatedRefinementReceiptFile.isEmpty())
    {
        m_relativeStockProbe = std::make_shared<ArenaRelativeStockProbe>(
            m_config.coordinatedFrontierReceipts, m_config.coordinatedFrontierDay);
        if (!m_config.coordinatedValuationFile.isEmpty())
        {
            const auto path = QDir(Settings::getInstance()->getUserPath()).filePath(m_config.coordinatedValuationFile);
            if (!m_relativeStockProbe->loadValuations(path, error))
            {
                return false;
            }
        }
    }
    QStringList buildList;
    if (!m_config.buildListFile.isEmpty())
    {
        const auto path = QDir(Settings::getInstance()->getUserPath()).filePath(m_config.buildListFile);
        buildList = Filesupport::readList(path, "").items;
        if (buildList.isEmpty())
        {
            error = "build list is unavailable or empty";
            return false;
        }
    }
    for (qint32 i = 0; i < m_pMap->getPlayerCount(); ++i)
    {
        const ArenaMatchPlayerConfig & playerConfig = m_config.players.at(i);
        if (!playerConfig.coId.isEmpty() && !COSpriteManager::getInstance()->exists(playerConfig.coId))
        {
            error = "unknown co " + playerConfig.coId;
            return false;
        }
        Player* pPlayer = m_pMap->getPlayer(i);
        if (!buildList.isEmpty())
        {
            pPlayer->setBuildList(buildList);
        }
        pPlayer->setControlType(playerConfig.aiType);
        pPlayer->setBaseGameInput(BaseGameInputIF::createAi(m_pMap.get(), playerConfig.aiType));
        BaseGameInputIF* pInput = pPlayer->getBaseGameInput();
        if (pInput == nullptr || dynamic_cast<DummyAi*>(pInput) != nullptr)
        {
            error = "player " + QString::number(i) + " did not receive a local ai";
            return false;
        }
        if (auto* pCoordinated = dynamic_cast<CoordinatedAi*>(pInput))
        {
            auto tierAudit = Coordinator::SequentialTierAuditMode::Full;
            if (!m_config.coordinatedTierAudit)
            {
                tierAudit = Coordinator::SequentialTierAuditMode::CountersOnly;
            }
            pCoordinated->setRefinementTestControls(m_config.coordinatedRefinementWorkLimit, m_relativeStockProbe,
                m_config.coordinatedRefinementSchedule, m_config.coordinatedScheduleDay, tierAudit);
        }
        pPlayer->setCO(playerConfig.coId, PRIMARY_CO);
        pPlayer->setCO({}, SECONDARY_CO);
    }
    return true;
}

void ArenaMatchRunner::setStockControlPrefixHold(bool held)
{
    for (qint32 player = 0; player < m_pMap->getPlayerCount(); ++player)
    {
        auto* coordinated = dynamic_cast<CoordinatedAi*>(m_pMap->getPlayer(player)->getBaseGameInput());
        if (coordinated != nullptr)
        {
            coordinated->setStockControlTestHold(held);
        }
    }
}

bool ArenaMatchRunner::applyGameRules(QString & error)
{
    GameRules* pRules = m_pMap->getGameRules();
    if (m_config.rulesetFile.isEmpty())
    {
        addDefaultVictoryRules(pRules);
        pRules->setRandomWeather(false);
        pRules->setAiBehaviorMode(m_config.aiBehaviorMode);
    }
    else
    {
        QFile file(QDir(Settings::getInstance()->getUserPath()).filePath(m_config.rulesetFile));
        if (!file.open(QIODevice::ReadOnly))
        {
            error = "saved ruleset cannot be read";
            return false;
        }
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_5);
        pRules->deserializeObject(stream);
        if (stream.status() != QDataStream::Ok || pRules->getAiBehaviorMode() != m_config.aiBehaviorMode)
        {
            error = "saved ruleset is invalid or aiBehaviorMode differs";
            return false;
        }
        if (m_actionPrefix && pRules->getFogMode() != GameEnums::Fog_Off)
        {
            error = "partial-path action prefix requires fog off";
            return false;
        }
        auto* turnLimit = pRules->getVictoryRule(TURN_LIMIT_RULE);
        if (m_config.turnLimit == 0 && turnLimit != nullptr && turnLimit->getRuleValue(TURN_LIMIT_ITEM) > 0)
        {
            error = "saved ruleset contains an active turn limit";
            return false;
        }
    }
    if (m_config.turnLimit > 0)
    {
        pRules->addVictoryRule(TURN_LIMIT_RULE);
        VictoryRule* pTurnLimit = pRules->getVictoryRule(TURN_LIMIT_RULE);
        if (pTurnLimit == nullptr)
        {
            error = "turn limit victory rule is unavailable";
            return false;
        }
        pTurnLimit->setRuleValue(m_config.turnLimit, TURN_LIMIT_ITEM);
    }
    else
    {
        pRules->removeVictoryRule(TURN_LIMIT_RULE);
    }
    return applyFixtureRuntime(m_config, error);
}

bool ArenaMatchRunner::applyFixtureRuntime(const ArenaMatchConfig & config, QString & error)
{
    if (config.deterministicCounterpointSeed)
    {
        const QJSValue result = Interpreter::getInstance()->doString(
            "typeof COUNTERPOINTAI !== 'undefined' && "
            "(COUNTERPOINTAI.PLANNER_DETERMINISTIC_SEED = true)");
        if (result.isError() || !result.toBool())
        {
            error = "Counterpoint deterministic seed control is unavailable";
            return false;
        }
    }
    return true;
}

void ArenaMatchRunner::addDefaultVictoryRules(GameRules* pRules)
{
    GameRuleManager* pManager = GameRuleManager::getInstance();
    const qint32 initialRuleCount = pRules->getVictoryRuleSize();
    for (qint32 i = 0; i < pManager->getVictoryRuleCount(); ++i)
    {
        const QString ruleId = pManager->getVictoryRuleID(i);
        if (pRules->getVictoryRule(ruleId) != nullptr)
        {
            continue;
        }
        spVictoryRule pRule = MemoryManagement::create<VictoryRule>(ruleId, m_pMap.get());
        const QStringList types = pRule->getRuleType();
        for (qint32 item = 0; item < types.size(); ++item)
        {
            const bool disabledCheckbox =
                !types.isEmpty() && types[0] == VictoryRule::checkbox && item == 0 &&
                initialRuleCount > 0;
            pRule->setRuleValue(disabledCheckbox ? 0 : pRule->getDefaultValue(item), item);
        }
        pRules->addVictoryRule(pRule);
    }
}

void ArenaMatchRunner::startMenue()
{
    m_pMenue = MemoryManagement::create<GameMenue>(m_pMap, false, spNetworkInterface());
    oxygine::Stage::getStage()->addChild(m_pMenue);
}

void ArenaMatchRunner::spliceActionHandling()
{
    ActionPerformer* pPerformer = m_pMenue->getActionPerformer();
    for (qint32 i = 0; i < m_pMap->getPlayerCount(); ++i)
    {
        auto* pAi = dynamic_cast<CoreAI*>(m_pMap->getPlayer(i)->getBaseGameInput());
        if (pAi == nullptr)
        {
            continue;
        }
        QObject::disconnect(pAi, &CoreAI::sigPerformAction, pPerformer,
                            &ActionPerformer::performAction);
        m_connections.append(connect(pAi, &CoreAI::sigPerformAction, this,
                                     &ArenaMatchRunner::onAiAction, Qt::DirectConnection));
        if (!m_actionPrefix)
        {
            m_connections.append(connect(pAi, &CoreAI::sigPerformAction, pPerformer,
                                         &ActionPerformer::performAction, Qt::DirectConnection));
        }
    }
    m_connections.append(connect(pPerformer, &ActionPerformer::sigActionPerformed,
                                 this, &ArenaMatchRunner::onActionPerformed,
                                 Qt::DirectConnection));
}

void ArenaMatchRunner::spliceTerminalHandling()
{
    GameRules* pRules = m_pMap->getGameRules();
    QObject::disconnect(pRules, &GameRules::sigVictory, m_pMenue.get(), &GameMenue::victory);
    QObject::disconnect(m_pMenue.get(), &GameMenue::sigVictory,
                        m_pMenue.get(), &GameMenue::victory);
    m_connections.append(connect(pRules, &GameRules::sigVictory, this,
                                 &ArenaMatchRunner::onVictory, Qt::DirectConnection));
    m_connections.append(connect(m_pMenue.get(), &GameMenue::sigVictory, this,
                                 &ArenaMatchRunner::onVictory, Qt::DirectConnection));
}

void ArenaMatchRunner::onAiAction(spGameAction pAction)
{
    if (m_finished || pAction == nullptr || m_pMap->getCurrentPlayer() == nullptr)
    {
        return;
    }
    if (m_actionPrefix)
    {
        QString error;
        if (!m_actionPrefix->apply(pAction, *m_pMap, error))
        {
            m_finished = true;
            failSetup(error);
            return;
        }
    }
    m_stopBoundary.observeAction(m_pMap->getCurrentDay(),
                                 m_pMap->getCurrentPlayer()->getPlayerID());
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    pAction->serializeObject(stream);
    const QPoint target = pAction->getTarget();
    m_actionIds.append(pAction->getActionID());
    m_preActionStateHashes.append(QString::fromLatin1(m_pMap->getMapHash().toHex()));
    m_actionPayloadSha256.append(QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    m_actionPayloadBase64.append(QString::fromLatin1(payload.toBase64()));
    m_seedTrace.append(QVariant::fromValue<qulonglong>(pAction->getSeed()));
    m_actionTargets.append(QVariantMap{{"x", target.x()}, {"y", target.y()}});
    auto* player = m_pMap->getCurrentPlayer();
    QVariant planSequence;
    if (const auto* coordinated = dynamic_cast<const CoordinatedAi*>(player->getBaseGameInput()))
    {
        planSequence = QString::number(coordinated->testPlanSequence());
    }
    m_actionPlanningContexts.append(QVariantMap{
        {"day", m_pMap->getCurrentDay()}, {"player", player->getPlayerID()}, {"planSequence", planSequence},
    });
    if (m_actionPrefix)
    {
        m_pMenue->getActionPerformer()->performAction(pAction);
    }
}

void ArenaMatchRunner::onActionPerformed()
{
    Player* pCurrent = m_pMap->getCurrentPlayer();
    if (m_actionPrefix && !m_finished && m_actionPrefix->observeBoundary(*m_pMap))
    {
        setStockControlPrefixHold(false);
    }
    if (!m_finished && pCurrent != nullptr &&
        m_stopBoundary.shouldStop(m_pMap->getCurrentDay(), pCurrent->getPlayerID()))
    {
        finishMatch(Outcome::Complete);
    }
}

void ArenaMatchRunner::onVictory(qint32)
{
    if (!m_finished)
    {
        finishMatch(m_config.hasStopBoundary() ? Outcome::BoundaryMissed : Outcome::Complete);
    }
}

void ArenaMatchRunner::onWatchdog()
{
    if (!m_finished)
    {
        finishMatch(Outcome::Watchdog);
    }
}

void ArenaMatchRunner::finishMatch(Outcome outcome)
{
    if (outcome == Outcome::Complete && m_actionPrefix && !m_actionPrefix->complete())
    {
        outcome = Outcome::BoundaryMissed;
    }
    m_finished = true;
    m_watchdog.stop();
    for (const auto & connection : std::as_const(m_connections))
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();
    quiesceAiTurnQueue(m_pMap.get(), m_pMenue->getActionPerformer());

    QVariantList players;
    for (qint32 i = 0; i < m_pMap->getPlayerCount(); ++i)
    {
        players.append(buildPlayerResult(i));
    }
    const qint32 exitCode = outcome == Outcome::Complete ? 0 :
                            outcome == Outcome::Watchdog ? 5 : 4;
    QVariantMap result = {
        {"version", RESULT_VERSION},
        {"mapPath", m_config.mapPath},
        {"mapSha256", fileSha256(m_config.mapPath)},
        {"masterSeed", QVariant::fromValue<qulonglong>(m_config.masterSeed)},
        {"turnLimit", m_config.turnLimit},
        {"stopAfterDay", m_config.hasStopBoundary() ? QVariant(m_config.stopAfterDay) : QVariant()},
        {"stopAfterPlayer", m_config.hasStopBoundary() ? QVariant(m_config.stopAfterPlayer) : QVariant()},
        {"players", players},
        {"seedTrace", m_seedTrace},
        {"actionIds", m_actionIds},
        {"actionTargets", m_actionTargets},
        {"actionPlanningContexts", m_actionPlanningContexts},
        {"preActionStateHashes", m_preActionStateHashes},
        {"actionPayloadSha256", m_actionPayloadSha256},
        {"actionPayloadBase64", m_actionPayloadBase64},
        {"finalStateHash", QString::fromLatin1(m_pMap->getMapHash().toHex())},
        {"actionCount", m_actionIds.size()},
        {"watchdog", outcome == Outcome::Watchdog},
        {"exitCode", exitCode},
        {"pass", outcome == Outcome::Complete},
    };
    if (m_actionPrefix)
    {
        result.insert("actionPrefix", m_actionPrefix->result());
    }
    AiArenaTestSupport* pSupport = support();
    if (pSupport == nullptr)
    {
        CONSOLE_PRINT(SETUP_PREFIX + "arena support is unavailable", GameConsole::eERROR);
        return;
    }
    if ((m_relativeStockProbe &&
         !pSupport->writeJsonResult(m_config.coordinatedRefinementReceiptFile, m_relativeStockProbe->result())) ||
        !pSupport->writeJsonResult(m_config.resultFileName, result))
    {
        pSupport->finish(ArenaExitCode::SetupError);
        return;
    }
    pSupport->finish(exitCode);
}

QVariantMap ArenaMatchRunner::buildPlayerResult(qint32 playerId) const
{
    Player* pPlayer = m_pMap->getPlayer(playerId);
    const ArenaMatchPlayerConfig & config = m_config.players.at(playerId);
    QVariantMap result{
        {"playerId", playerId},
        {"team", pPlayer->getTeam()},
        {"aiType", config.aiTypeName},
        {"co", config.coId},
        {"defeated", pPlayer->getIsDefeated()},
        {"funds", pPlayer->getFunds()},
        {"income", pPlayer->calcIncome()},
        {"unitCount", pPlayer->getUnitCount()},
        {"buildingCount", pPlayer->getBuildingCount()},
        {"armyValue", pPlayer->calcArmyValue()},
    };
    if (const auto* coordinated = dynamic_cast<const CoordinatedAi*>(pPlayer->getBaseGameInput()))
    {
        result.insert("coordinatedPlanCount", QString::number(coordinated->testPlanSequence()));
        result.insert(ArenaMatchConfig::TIER_AUDIT_KEY, coordinated->testTierAuditEnabled());
    }
    return result;
}

AiArenaTestSupport* ArenaMatchRunner::support() const
{
    Interpreter* pInterpreter = Interpreter::getInstance();
    return pInterpreter == nullptr ? nullptr : pInterpreter->findChild<AiArenaTestSupport*>();
}

void ArenaMatchRunner::failSetup(const QString & reason)
{
    CONSOLE_PRINT(SETUP_PREFIX + reason, GameConsole::eERROR);
    if (AiArenaTestSupport* pSupport = support())
    {
        pSupport->finish(ArenaExitCode::SetupError);
    }
}
