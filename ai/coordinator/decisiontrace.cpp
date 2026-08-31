#include "ai/coordinator/decisiontrace.h"

#include <utility>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include "coreengine/settings.h"

namespace
{
    struct TraceFileState
    {
        QMutex mutex;
        QStringList initializedPaths;
        QStringList failedPaths;
    };

    TraceFileState & traceFileState()
    {
        static TraceFileState state;
        return state;
    }

    bool writeTraceLines(const QString & path, const QStringList & lines)
    {
        TraceFileState & state = traceFileState();
        QMutexLocker lock(&state.mutex);
        if (state.failedPaths.contains(path))
        {
            return false;
        }
        const bool initialized = state.initializedPaths.contains(path);
        QFile file(path);
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        mode |= initialized ? QIODevice::Append : QIODevice::Truncate;
        if (!file.open(mode))
        {
            state.failedPaths.push_back(path);
            return false;
        }
        QByteArray payload;
        for (const QString & line : lines)
        {
            payload += line.toUtf8();
            payload += '\n';
        }
        if (file.write(payload) != payload.size() || !file.flush())
        {
            state.failedPaths.push_back(path);
            return false;
        }
        if (!initialized)
        {
            state.initializedPaths.push_back(path);
        }
        return true;
    }

    class FileDecisionTrace final : public Coordinator::DecisionTrace
    {
    public:
        FileDecisionTrace(
            Coordinator::DecisionTraceIdentity identity,
            QString path,
            bool candidateDetails,
            bool stockDetails)
            : m_identity(identity),
              m_path(std::move(path)),
              m_candidateDetails(candidateDetails),
              m_stockDetails(stockDetails)
        {
        }

        bool candidateDetailsEnabled() const override
        {
            return m_active && m_candidateDetails;
        }

        bool stockDetailsEnabled() const override
        {
            return m_active && m_stockDetails;
        }

        void record(const QString & category, const QString & fields) override
        {
            if (!m_active)
            {
                return;
            }
            QString line =
                QStringLiteral("[COORD][%1] day=%2 player=%3 ai=COORDINATED rng=unused plan=%4 horizon=%5")
                    .arg(category)
                    .arg(m_identity.day)
                    .arg(m_identity.playerId)
                    .arg(m_identity.planSequence)
                    .arg(m_identity.horizonTurns);
            if (!fields.isEmpty())
            {
                line += QLatin1Char(' ');
                line += fields;
            }
            m_buffer.push_back(std::move(line));
        }

        bool flush() override
        {
            if (!m_active)
            {
                return false;
            }
            if (m_buffer.empty())
            {
                return true;
            }
            m_active = writeTraceLines(m_path, m_buffer);
            m_buffer.clear();
            return m_active;
        }

    private:
        Coordinator::DecisionTraceIdentity m_identity;
        QString m_path;
        QStringList m_buffer;
        bool m_candidateDetails{false};
        bool m_stockDetails{false};
        bool m_active{true};
    };
}

bool Coordinator::decisionTraceEnabled()
{
#ifdef GAMEDEBUG
    Settings* pSettings = Settings::getInstance();
    return pSettings != nullptr &&
           pSettings->getCoordinatedDecisionLog();
#else
    return false;
#endif
}

bool Coordinator::planningTimingAuditEnabled()
{
#ifdef GAMEDEBUG
    return qgetenv("COW_COORDINATED_TIMING_AUDIT") ==
           QByteArray("1");
#else
    return false;
#endif
}

std::unique_ptr<Coordinator::DecisionTrace>
Coordinator::openDecisionTrace(const DecisionTraceIdentity & identity)
{
#ifdef GAMEDEBUG
    Settings* pSettings = Settings::getInstance();
    if (!decisionTraceEnabled())
    {
        return nullptr;
    }
    const QString path =
        pSettings->getUserPath() +
        QStringLiteral("coordinated-ai.log");
    if (!writeTraceLines(path, QStringList{}))
    {
        return nullptr;
    }
    return std::make_unique<FileDecisionTrace>(
        identity,
        path,
        pSettings->getCoordinatedDecisionLogCandidates(),
        pSettings->getCoordinatedDecisionLogStock());
#else
    static_cast<void>(identity);
    return nullptr;
#endif
}

void Coordinator::writePlanningTimingAudit(
    const DecisionTraceIdentity & identity,
    const QString & fields)
{
#ifdef GAMEDEBUG
    Settings* pSettings = Settings::getInstance();
    if (!planningTimingAuditEnabled() ||
        pSettings == nullptr)
    {
        return;
    }
    QString line =
        QStringLiteral("[COORD][PHASE_TIMING] day=%1 player=%2 ai=COORDINATED rng=unused plan=%3 horizon=%4")
            .arg(identity.day)
            .arg(identity.playerId)
            .arg(identity.planSequence)
            .arg(identity.horizonTurns);
    if (!fields.isEmpty())
    {
        line += QLatin1Char(' ');
        line += fields;
    }
    writeTraceLines(
        pSettings->getUserPath() +
            QStringLiteral(
                "coordinated-ai-timing.log"),
        QStringList{line});
#else
    static_cast<void>(identity);
    static_cast<void>(fields);
#endif
}
