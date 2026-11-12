#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include "ai/coordinator/masterassignmentshadow.h"
#include "menue/mainwindow.h"
#include "menue/gamemenue.h"
#include "tests/arena/playabletrialconfig.h"
#include "tests/arena/nativecapturereceipt.h"

class PlayableTrialRunner final : public QObject
{
public:
    static bool shouldRun();
    explicit PlayableTrialRunner(spMainwindow window, QObject* parent);
    void run();

private:
    enum class Phase { Starting, Running, ObservingStop, Closing, Quitting };
    enum class Outcome { Complete, ExpectedStop, SetupFailure, LinkageFailure, BoundaryMissed, Watchdog };

    bool prepare();
    bool validateRuntime(QString & error);
    bool validateProofOutput(QString & error) const;
    bool validateResume(QString & error);
    bool bindMenu(GameMenue* menu, bool loaded, QString & error);
    void startFresh();
    void startResume();
    void startSession(bool loaded);
    void closeSession();
    void onAcceptedAction(spGameAction action);
    void onActionCompleted();
    void onStockControlStopped(const QString & reason);
    void onVictory(qint32 team);
    void onPulse();
    void onWatchdog();
    bool completePendingAction();
    void observeActorBefore(GameAction & action);
    void observeActorAfter();
    bool observeCaptureBefore(GameAction & action);
    void observeCaptureAfter();
    bool saveCheckpoint();
    void reloadCheckpoint();
    void finish(Outcome outcome, const QString & reason);
    void completeShutdown(Mainwindow* window, bool returnedToMainMenu);
    QVariantList roster() const;
    QVariantList controllers() const;
    QVariantMap state() const;
    QVariantMap nativeConfig() const;
    QString localPath(const QString & name) const;
    bool writeResult();

    spMainwindow m_startWindow;
    QPointer<GameMenue> m_menu;
    PlayableTrialConfig m_config;
    Coordinator::MasterShadowConfig m_master;
    QString m_profile;
    QString m_executableSha256;
    QFile m_actions;
    QVector<QMetaObject::Connection> m_connections;
    ArenaStopBoundary m_stopBoundary;
    ArenaStopBoundary m_checkpointBoundary;
    QTimer m_watchdog;
    QTimer m_pulse;
    QElapsedTimer m_transitionTimer;
    QVariantMap m_result;
    QVariantMap m_session;
    QVariantMap m_pendingAction;
    std::optional<NativeCaptureReceipt::Observation> m_pendingCapture;
    QPointer<Unit> m_actionActor;
    QPointer<Unit> m_captureActor;
    QPointer<Building> m_captureProperty;
    QVariantMap m_checkpoint;
    QVariantList m_sessions;
    QByteArray m_stoppedState;
    QString m_reason;
    QString m_stopReason;
    qint64 m_actionCount{0};
    qint64 m_sessionActionStart{0};
    qint64 m_proofStart{0};
    qint64 m_stoppedActionCount{0};
    bool m_checkpointSaved{false};
    bool m_victoryExited{false};
    Phase m_phase{Phase::Starting};
    Outcome m_outcome{Outcome::SetupFailure};
};
