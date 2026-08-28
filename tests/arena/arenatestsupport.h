#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

enum class ArenaExitCode : qint32
{
    Ok = 0,
    AssertFailed = 4,
    Watchdog = 5,
    SetupError = 6,
};

class AiArenaTestSupport final : public QObject
{
    Q_OBJECT
public:
    using Operation = std::function<QVariantMap(QObject*, const QVariantMap&)>;

    static const char* const ENVIRONMENT_FLAG;
    static const QString JS_GLOBAL_NAME;

    explicit AiArenaTestSupport(QObject* pParent = nullptr);

    static bool registerOperation(const QString & name, Operation operation);

    Q_INVOKABLE QVariantMap runOperation(const QString & name,
                                         QObject* pContext,
                                         const QVariantMap & arguments) const;
    Q_INVOKABLE bool writeJsonResult(const QString & fileName,
                                     const QVariantMap & result) const;
    Q_INVOKABLE bool armWatchdog(const QString & reason, qint32 timeoutMs);
    Q_INVOKABLE void cancelWatchdog();
    Q_INVOKABLE void finish(qint32 exitCode);
    void finish(ArenaExitCode exitCode);

private:
    QString getOutputPath(const QString & fileName) const;

    QTimer m_watchdog;
    QString m_watchdogReason;
    bool m_exitQueued{false};
};
