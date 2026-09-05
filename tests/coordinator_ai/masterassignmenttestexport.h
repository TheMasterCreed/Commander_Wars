#pragma once

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/masterassignmentsearch.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"

namespace MasterAssignmentTest
{
    inline QJsonArray choicesRecord(std::span<const std::int32_t> choices)
    {
        QJsonArray result;
        for (const auto value : choices)
        {
            result.append(value);
        }
        return result;
    }

    inline QJsonValue valueRecord(std::optional<MilliFunds> value)
    {
        return value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null);
    }

    inline QJsonObject searchRecord(const MasterAssignmentSearchResult & result)
    {
        QJsonArray work;
        for (std::size_t category = 0; category < REFINEMENT_WORK_CLASSES; ++category)
        {
            work.append(QString::number(result.stats.work.grantedByWork[category] -
                                        result.stats.work.refundedByWork[category]));
        }
        QJsonArray residual;
        for (const auto & region : result.residual)
        {
            residual.append(QJsonObject{{"choices", choicesRecord(region.state.choices)},
                {"upper", valueRecord(region.upper)}, {"complete", region.completeTuple},
                {"unsupported", region.unsupported}});
        }
        QJsonArray prunes;
        for (const auto & prune : result.prunes)
        {
            prunes.append(QJsonObject{{"choices", choicesRecord(prune.state.choices)},
                {"upper", QString::number(prune.upper)}, {"feasibleLower", QString::number(prune.feasibleLower)}});
        }
        QJsonObject improvement;
        if (result.improvement)
        {
            const auto & proof = *result.improvement;
            improvement = {{"choices", choicesRecord(proof.challenger.state.choices)},
                {"deltaLower", QString::number(proof.comparison.deltaLower)},
                {"deltaUpper", QString::number(proof.comparison.deltaUpper)},
                {"commonEnemy", proof.comparison.commonEnemyCancelled}, {"work", QString::number(proof.work)}};
        }
        return {{"allowance", QString::number(result.stats.work.total)},
            {"spent", QString::number(result.stats.work.spent())}, {"netSpentByWork", work},
            {"schedule", QJsonObject{{"currentDualRounds", result.schedule.currentDualRounds},
                {"continuationSlice", QString::number(result.schedule.continuationSlice)},
                {"futureDualAllowance", QString::number(result.schedule.futureDualAllowance)},
                {"nodesPerRefinement", QString::number(result.schedule.nodesPerRefinement)}}},
            {"lower", valueRecord(result.lower)}, {"upper", valueRecord(result.upper)},
            {"gap", valueRecord(result.gap)}, {"globalOptimal", result.globalOptimal},
            {"budgetExhausted", result.budgetExhausted}, {"domainAccounted", result.domainAccounted},
            {"fullDomain", result.fullDomain}, {"noFeasibleAssignment", result.noFeasibleAssignment},
            {"implicitRoot", result.implicitRoot}, {"residualRegions", QString::number(result.stats.residualRegions)},
            {"peakQueuedRegions", QString::number(result.stats.peakFrontier)},
            {"peakResidentRegions", QString::number(result.stats.peakResidentRegions)},
            {"incumbentPreserved", result.incumbentPreserved}, {"canonicalRepresentative", result.canonicalRepresentative},
            {"bestChoices", result.best ? choicesRecord(result.best->state.choices) : QJsonArray{}},
            {"expanded", QString::number(result.stats.expanded)}, {"evaluated", QString::number(result.stats.evaluated)},
            {"infeasible", QString::number(result.stats.infeasible)}, {"pruned", QString::number(result.stats.pruned)},
            {"unsupported", QString::number(result.stats.unsupported)},
            {"arithmeticFailures", QString::number(result.stats.arithmeticFailures)},
            {"proofFailures", QString::number(result.stats.proofFailures)},
            {"refinements", QString::number(result.stats.refinements)}, {"residual", residual},
            {"prunes", prunes}, {"improvement", improvement}};
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
