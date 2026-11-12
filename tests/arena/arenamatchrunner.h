#pragma once

#include <memory>

#include <QMetaObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include "game/gameaction.h"
#include "game/gamemap.h"
#include "menue/gamemenue.h"
#include "tests/arena/arenamatchconfig.h"
#include "tests/arena/arenaactionprefix.h"

class AiArenaTestSupport;
class GameRules;
class ArenaRelativeStockProbe;

class ArenaMatchRunner final : public QObject
{
    Q_OBJECT
public:
    static bool shouldRun();
    static spGameMap prepareFixture(const ArenaMatchConfig & config, QString & error);
    static bool applyFixtureRuntime(const ArenaMatchConfig & config, QString & error);

    explicit ArenaMatchRunner(QObject* pParent = nullptr);
    void run();

private:
    enum class Outcome
    {
        Complete,
        Watchdog,
        BoundaryMissed,
    };

    static QString configPath();

    bool loadConfig();
    bool loadMap(QString & error);
    bool applyPlayerSetup(QString & error);
    void setStockControlPrefixHold(bool held);
    bool applyGameRules(QString & error);
    void addDefaultVictoryRules(GameRules* pRules);
    void startMenue();
    void spliceActionHandling();
    void spliceTerminalHandling();
    void onAiAction(spGameAction pAction);
    void onActionPerformed();
    void onVictory(qint32 team);
    void onWatchdog();
    void finishMatch(Outcome outcome);
    QVariantMap buildPlayerResult(qint32 playerId) const;
    AiArenaTestSupport* support() const;
    void failSetup(const QString & reason);

    ArenaMatchConfig m_config;
    ArenaStopBoundary m_stopBoundary;
    spGameMap m_pMap;
    spGameMenue m_pMenue;
    QTimer m_watchdog;
    QVector<QMetaObject::Connection> m_connections;
    QStringList m_actionIds;
    QStringList m_preActionStateHashes;
    QStringList m_actionPayloadSha256;
    QStringList m_actionPayloadBase64;
    QVariantList m_seedTrace;
    QVariantList m_actionTargets;
    QVariantList m_actionPlanningContexts;
    std::shared_ptr<ArenaRelativeStockProbe> m_relativeStockProbe;
    std::unique_ptr<ArenaActionPrefix> m_actionPrefix;
    bool m_finished{false};
};
