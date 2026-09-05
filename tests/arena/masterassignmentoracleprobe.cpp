#include "tests/arena/masterassignmentoracleprobe.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/decisiontrace.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace
{
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_ORACLE_DIAGNOSTIC";
    constexpr auto REVISION_VARIABLE = "COW_ORACLE_SOURCE_REVISION";
    constexpr auto FIXTURE_VARIABLE = "COW_ORACLE_FIXTURE_REFERENCE";
    constexpr std::uint64_t COMPLETE_DOMAIN_LIMIT = 64;
    constexpr std::size_t DERIVED_MUTABLE_ACTORS = 3;

    void restrictDomain(const Coordinator::MasterAssignmentProblem & problem,
                        MasterAssignmentOracle::Request & request)
    {
        request.domain = MasterAssignmentOracle::fullDomain(problem);
        const auto product = MasterAssignmentOracle::tupleProduct(request.domain);
        if (product && *product <= COMPLETE_DOMAIN_LIMIT)
        {
            return;
        }
        std::size_t expanded = 0;
        for (std::size_t slot = 0; slot < request.domain.size(); ++slot)
        {
            auto & domain = request.domain[slot];
            if (domain.size() < 2)
            {
                continue;
            }
            const auto incoming = problem.incumbentChoices()[slot];
            const auto incumbent = std::find(domain.begin(), domain.end(), incoming);
            std::vector<std::int32_t> selected{incumbent == domain.end() ? domain.front() : incoming};
            if (expanded < DERIVED_MUTABLE_ACTORS)
            {
                const auto alternative = std::find_if(domain.begin(), domain.end(),
                    [&](auto choice) { return choice != selected.front(); });
                selected.push_back(*alternative);
                ++expanded;
            }
            domain = std::move(selected);
        }
        request.restriction = QStringLiteral(
            "Full domain exceeds 64 tuples. First three canonical multi-candidate actors retain their incumbent "
            "(canonical first if unassigned) and first distinct canonical alternative. Other actors retain only "
            "their incumbent (canonical first if unassigned). Full candidates and exact retained indices are exported.");
    }
}

namespace ArenaMasterAssignmentOracleProbe
{
    void record(const Coordinator::MasterAssignmentProblem & problem,
                const Coordinator::DecisionTraceIdentity & identity)
    {
        const auto directory = QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE));
        if (directory.isEmpty() || qgetenv("COW_AI_ARENA_TEST") != "1")
        {
            return;
        }
        MasterAssignmentOracle::Request request;
        request.sourceRevision = QString::fromLocal8Bit(qgetenv(REVISION_VARIABLE));
        request.fixture = QStringLiteral("plan-%1-%2-%3").arg(identity.day).arg(identity.playerId).arg(identity.planSequence);
        request.continuation.mode = MasterAssignmentOracle::ContinuationMode::Unavailable;
        request.continuation.immutableReference = QString::fromLocal8Bit(qgetenv(FIXTURE_VARIABLE));
        request.tupleLimit = COMPLETE_DOMAIN_LIMIT;
        if (request.sourceRevision.isEmpty() || request.continuation.immutableReference.isEmpty())
        {
            qWarning() << "Master oracle requires source revision and preserved fixture reference.";
            return;
        }
        restrictDomain(problem, request);
        const auto result = MasterAssignmentOracle::enumerate(problem, request);
        const auto bytes = MasterAssignmentOracle::exportDiagnostic(problem, request, result);
        QFile output(QDir(directory).filePath(request.fixture + QStringLiteral(".json")));
        if (!QDir().mkpath(directory) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(bytes) != bytes.size() || !output.flush())
        {
            qWarning() << "Master oracle export failed:" << output.errorString();
            return;
        }
        QFile measurements(QDir(directory).filePath(QStringLiteral("measurements.jsonl")));
        const auto line = QJsonDocument(QJsonObject{
            {"fixture", request.fixture}, {"horizon", identity.horizonTurns},
            {"elapsedNanos", QString::number(result.elapsedNanos)},
            {"serializedBytes", QString::number(bytes.size())},
            {"retainedTupleRecords", QString::number(result.tuples.size())},
            {"continuationWork", QString::number(result.continuationWork)},
        }).toJson(QJsonDocument::Compact) + '\n';
        if (!measurements.open(QIODevice::WriteOnly | QIODevice::Append) ||
            measurements.write(line) != line.size() || !measurements.flush())
        {
            qWarning() << "Master oracle measurement write failed:" << measurements.errorString();
        }
    }
}
