#pragma once

#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

class GameMap;
class GameMenue;
class GameRules;
class Building;
class Player;
class QJsonObject;
class SimpleProductionSystem;
class Unit;

class CounterpointTestSupport final : public QObject
{
    Q_OBJECT
public:
    explicit CounterpointTestSupport(QObject* pParent = nullptr);

    Q_INVOKABLE bool writeGameRules(GameRules* pRules, const QString & fileName) const;
    Q_INVOKABLE bool readGameRules(GameRules* pRules, const QString & fileName) const;
    Q_INVOKABLE qint32 readGameRulesVersion(const QString & fileName) const;
    Q_INVOKABLE qint32 getGameRulesVersion(GameRules* pRules) const;
    Q_INVOKABLE QString fileSha256(const QString & fileName) const;
    Q_INVOKABLE QString gameMapHash(GameMap* pMap) const;
    Q_INVOKABLE bool overwriteLastInt32(const QString & fileName, qint32 value) const;
    Q_INVOKABLE bool productionQueryPreservesState(GameMap* pMap,
                                                   qint32 playerId,
                                                   Building* pBuilding,
                                                   const QString & actionId) const;
    Q_INVOKABLE bool beginRandomProbe();
    Q_INVOKABLE bool finishRandomProbe();
    Q_INVOKABLE bool captureProductionSystem(SimpleProductionSystem* pProductionSystem);
    Q_INVOKABLE bool restoreProductionSystem(SimpleProductionSystem* pProductionSystem);
    Q_INVOKABLE qint32 getCapturedProductionSystemSize() const;
    Q_INVOKABLE bool executeCounterpointBuildTargets(GameMap* pMap,
                                                     qint32 playerId,
                                                     qint32 x,
                                                     qint32 y,
                                                     const QString & unitId,
                                                     qint32 ordinal,
                                                     qint32 expectedCost) const;
    Q_INVOKABLE bool beginProductionActionProbe(GameMap* pMap, qint32 playerId);
    Q_INVOKABLE bool finishProductionActionProbe(qint32 x,
                                                 qint32 y,
                                                 const QString & unitId);
    Q_INVOKABLE bool enableSeededProduction(GameMenue* pMenu,
                                            qint32 playerId,
                                            quint32 seed,
                                            bool resetInitialProduction,
                                            const QString & resultFileName);
    Q_INVOKABLE void finish(qint32 exitCode);

private:
    QString getOutputPath(const QString & fileName) const;
    Unit* findProducedUnit(Player* pPlayer) const;
    bool writeJsonResult(const QJsonObject & result) const;
    void onActionPerformed();
    void armWatchdog(const QString & reason);
    void stopObservation();
    static constexpr qint32 OBSERVER_WATCHDOG_MS = 60000;
    static constexpr qint32 EXIT_CODE_ASSERTION = 4;
    static constexpr qint32 EXIT_CODE_WATCHDOG = 5;
    static constexpr qint32 EXIT_CODE_SETUP = 6;

    QTimer m_watchdog;
    QPointer<GameMenue> m_pMenu;
    QMetaObject::Connection m_actionConnection;
    QMetaObject::Connection m_productionActionConnection;
    QSet<qint32> m_initialUnitIds;
    QByteArray m_capturedProductionSystem;
    QString m_resultFileName;
    QString m_productionActionId;
    QString m_productionActionUnitId;
    qint32 m_expectedRandomValue{0};
    qint32 m_productionActionCount{0};
    qint32 m_productionActionX{-1};
    qint32 m_productionActionY{-1};
    qint32 m_playerId{-1};
    qint32 m_startFunds{0};
    quint32 m_seed{0};
    quint32 m_startBuildCount{0};
    bool m_resetInitialProduction{false};
    bool m_prepared{false};
    bool m_observing{false};
    bool m_exitQueued{false};
    bool m_randomProbeActive{false};
};
