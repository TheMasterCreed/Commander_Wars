#include "tests/arena/arenamatchrunner.h"

#include <utility>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QVariantMap>

#include "3rd_party/oxygine-framework/oxygine/actor/Stage.h"
#include "ai/coreai.h"
#include "ai/dummyai.h"
#include "coreengine/gameconsole.h"
#include "coreengine/globalutils.h"
#include "coreengine/interpreter.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/settings.h"
#include "game/actionperformer.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/victoryrule.h"
#include "gameinput/basegameinputif.h"
#include "network/networkinterface.h"
#include "resource_management/cospritemanager.h"
#include "resource_management/gamerulemanager.h"
#include "tests/arena/arenatestsupport.h"

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

void ArenaMatchRunner::run()
{
    if (!loadConfig())
    {
        return;
    }
    GlobalUtils::seed(m_config.masterSeed);
    GlobalUtils::setUseSeed(true);
    if (!loadMap() || !applyPlayerSetup() || !applyGameRules())
    {
        return;
    }
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
    return true;
}

bool ArenaMatchRunner::loadMap()
{
    m_pMap = MemoryManagement::create<GameMap>(m_config.mapPath, true, false, false);
    QString error;
    if (m_pMap->getPlayerCount() <= 0 || !m_config.validatePlayerCount(m_pMap->getPlayerCount(), error))
    {
        failSetup(error.isEmpty() ? "map loaded without players" : error);
        return false;
    }
    return true;
}

bool ArenaMatchRunner::applyPlayerSetup()
{
    for (qint32 i = 0; i < m_pMap->getPlayerCount(); ++i)
    {
        const ArenaMatchPlayerConfig & playerConfig = m_config.players.at(i);
        if (!COSpriteManager::getInstance()->exists(playerConfig.coId))
        {
            failSetup("unknown co " + playerConfig.coId);
            return false;
        }
        Player* pPlayer = m_pMap->getPlayer(i);
        pPlayer->setControlType(playerConfig.aiType);
        pPlayer->setBaseGameInput(BaseGameInputIF::createAi(m_pMap.get(), playerConfig.aiType));
        BaseGameInputIF* pInput = pPlayer->getBaseGameInput();
        if (pInput == nullptr || dynamic_cast<DummyAi*>(pInput) != nullptr)
        {
            failSetup("player " + QString::number(i) + " did not receive a local ai");
            return false;
        }
        pPlayer->setCO(playerConfig.coId, PRIMARY_CO);
        pPlayer->setCO({}, SECONDARY_CO);
    }
    return true;
}

bool ArenaMatchRunner::applyGameRules()
{
    GameRules* pRules = m_pMap->getGameRules();
    addDefaultVictoryRules(pRules);
    pRules->addVictoryRule(TURN_LIMIT_RULE);
    VictoryRule* pTurnLimit = pRules->getVictoryRule(TURN_LIMIT_RULE);
    if (pTurnLimit == nullptr)
    {
        failSetup("turn limit victory rule is unavailable");
        return false;
    }
    pTurnLimit->setRuleValue(m_config.turnLimit, TURN_LIMIT_ITEM);
    pRules->setRandomWeather(false);
    pRules->setAiBehaviorMode(m_config.aiBehaviorMode);
    if (m_config.deterministicCounterpointSeed)
    {
        const QJSValue result = Interpreter::getInstance()->doString(
            "typeof COUNTERPOINTAI !== 'undefined' && "
            "(COUNTERPOINTAI.PLANNER_DETERMINISTIC_SEED = true)");
        if (result.isError() || !result.toBool())
        {
            failSetup("Counterpoint deterministic seed control is unavailable");
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
        m_connections.append(connect(pAi, &CoreAI::sigPerformAction, pPerformer,
                                     &ActionPerformer::performAction, Qt::DirectConnection));
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
}

void ArenaMatchRunner::onActionPerformed()
{
    Player* pCurrent = m_pMap->getCurrentPlayer();
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
    m_finished = true;
    m_watchdog.stop();
    for (const auto & connection : std::as_const(m_connections))
    {
        QObject::disconnect(connection);
    }
    m_connections.clear();

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
        {"preActionStateHashes", m_preActionStateHashes},
        {"actionPayloadSha256", m_actionPayloadSha256},
        {"actionPayloadBase64", m_actionPayloadBase64},
        {"finalStateHash", QString::fromLatin1(m_pMap->getMapHash().toHex())},
        {"actionCount", m_actionIds.size()},
        {"watchdog", outcome == Outcome::Watchdog},
        {"exitCode", exitCode},
        {"pass", outcome == Outcome::Complete},
    };
    AiArenaTestSupport* pSupport = support();
    if (pSupport == nullptr)
    {
        CONSOLE_PRINT(SETUP_PREFIX + "arena support is unavailable", GameConsole::eERROR);
        return;
    }
    if (!pSupport->writeJsonResult(m_config.resultFileName, result))
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
    return {
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
