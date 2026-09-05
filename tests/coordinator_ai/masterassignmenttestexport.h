#pragma once

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/masterassignmentsearch.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"

namespace MasterAssignmentTest
{
    inline QJsonObject searchRecord(const MasterAssignmentSearchResult & result)
    {
        return masterSearchRecord(result);
    }

    inline void exportSearchCase(QString name, const MasterAssignmentProblem & problem,
                                 const MasterAssignmentOracle::Request & input,
                                 const MasterAssignmentOracle::Result & exact, const QJsonArray & searches)
    {
        const auto directory = QString::fromLocal8Bit(qgetenv("COW_MASTER_ORACLE_OUTPUT"));
        if (directory.isEmpty())
        {
            return;
        }
        require(QDir().mkpath(directory), "create master search evidence directory");
        const auto oracle = QJsonDocument::fromJson(
            MasterAssignmentOracle::exportDiagnostic(problem, input, exact)).object();
        const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"oracle", oracle}, {"searches", searches}})
            .toJson(QJsonDocument::Compact);
        QFile file(QDir(directory).filePath(name + QStringLiteral(".json")));
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush(),
                "write deterministic full-domain search receipt");
    }
}
