#include "ai/coordinator/masterassignmentcontrol.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmenttrace.h"

namespace
{
    using namespace Coordinator;

    struct ControlObservation
    {
        const MasterShadowConfig & config;
        const DecisionTraceIdentity & identity;
        const AssignmentResult & assignment;
        MasterStockControlTiming & timing;
        const RefinementLedger & ledger;
        const MasterAssignmentProfile & profile;
        const std::optional<MasterCaptureResult> & capture;
        const MasterAssignmentStockContext & stock;
        const std::optional<MasterAssignmentSearchResult> & search;
        const MasterInstallationResult & installed;
    };

    QJsonObject controlRecord(const ControlObservation & observation, std::int64_t computationNanos)
    {
        const auto & [config, identity, assignment, measured, ledger, profile, capture, stock, search, installed] =
            observation;
        QJsonObject record{
            {"schemaVersion", 1}, {"mode", QStringLiteral("stockControl")},
            {"status", masterStockControlStatusName(measured.status)},
            {"day", identity.day}, {"player", identity.playerId},
            {"planSequence", QString::number(identity.planSequence)}, {"horizon", identity.horizonTurns},
            {"allowance", QString::number(config.allowance)}, {"masterWork", masterWorkRecord(ledger.stats())},
            {"lastRefusal", masterRefusalRecord(ledger.lastRefusal())},
            {"constructorStatus", static_cast<int>(assignment.stockConstructionStatus)},
            {"constructorComplete", measured.constructorComplete},
            {"constructorWork", QString::number(assignment.stockConstructionWork)},
            {"nonStockRefinementRolledBack", assignment.nonStockRefinementRolledBack},
            {"stockSetup", static_cast<int>(stock.status())},
            {"installationStatus", static_cast<int>(installed.status)},
            {"installed", installed.status == MasterInstallationStatus::Installed},
            {"reservedWork", QString::number(measured.reservedWork)},
            {"searchWorkIncludesReservation", search.has_value()},
            {"firstImprovementWorkExcludesReservation", true},
            {"finalizationWork", QString::number(measured.finalizationWork)},
            {"firstImprovementWork", masterValueRecord(measured.firstImprovementWork)},
            {"firstImprovementNanos", masterValueRecord(measured.firstImprovementNanos)},
            {"firstImprovementLower", masterValueRecord(measured.firstImprovementLower)},
            {"constructorNanos", QString::number(measured.constructorNanos)},
            {"searchNanos", QString::number(measured.searchNanos)},
            {"finalizationNanos", QString::number(measured.finalizationNanos)},
            {"computationNanos", QString::number(computationNanos)},
            {"profile", masterProfileRecord(profile)}};
        if (capture)
        {
            record.insert("extractionWork", QString::number(capture->workUsed));
            record.insert("extractionError", static_cast<int>(capture->extraction.error));
            if (capture->extraction.valid())
            {
                record.insert("model", masterModelRecord(*capture->extraction.problem));
            }
        }
        if (search)
        {
            record.insert("search", masterSearchRecord(*search));
            if (search->incumbent)
            {
                record.insert("incoming", masterEvaluationRecord(*search->incumbent));
            }
            if (search->improvement)
            {
                record.insert("retainedIncumbent", masterEvaluationRecord(search->improvement->incumbent));
                record.insert("retainedChallenger", masterEvaluationRecord(search->improvement->challenger));
            }
        }
        if (installed.proof)
        {
            record.insert("installedIncumbent", masterEvaluationRecord(installed.proof->incumbent));
            record.insert("installedChallenger", masterEvaluationRecord(installed.proof->challenger));
            record.insert("strictMargin", masterValueRecord(installed.proof->comparison.deltaLower));
            record.insert("relativeUpper", masterValueRecord(installed.proof->comparison.deltaUpper));
        }
        return record;
    }

    bool writeControl(const ControlObservation & observation, std::int64_t computationNanos)
    {
        const auto line = QJsonDocument(controlRecord(observation, computationNanos)).toJson(QJsonDocument::Compact) + '\n';
        QFile output(observation.config.outputPath);
        const bool written = output.open(QIODevice::WriteOnly | QIODevice::Append) &&
            output.write(line) == line.size() && output.flush();
        if (!written)
        {
            qWarning() << "Master stock control receipt write failed:" << observation.config.outputPath
                       << output.errorString();
        }
        return written;
    }

    MasterStockControlTiming finishControl(
        const ControlObservation & observation, MasterProfileScope & timing, QElapsedTimer & total,
        MasterStockControlStatus status)
    {
        timing.finish();
        auto & measured = observation.timing;
        const auto & search = observation.search;
        const auto & installed = observation.installed;
        measured.status = status;
        measured.work = observation.ledger.stats();
        measured.installationStatus = installed.status;
        measured.firstImprovementNanos = observation.profile.firstImprovementNanos;
        if (search)
        {
            measured.gap = search->gap;
            measured.firstImprovementLower = search->stats.firstImprovementLower;
            if (search->stats.firstImprovementWork)
            {
                measured.firstImprovementWork = *search->stats.firstImprovementWork - measured.reservedWork;
            }
        }
        if (installed.proof)
        {
            measured.improvementLower = installed.proof->comparison.deltaLower;
        }
        if (observation.config.exportMode == MasterShadowExportMode::Full)
        {
            const auto exportStart = total.nsecsElapsed();
            measured.exportSucceeded = writeControl(observation, exportStart);
            measured.exportNanos = total.nsecsElapsed() - exportStart;
        }
        measured.totalNanos = total.nsecsElapsed();
        return measured;
    }
}

namespace Coordinator
{
    QString masterStockControlStatusName(MasterStockControlStatus status)
    {
        switch (status)
        {
            case MasterStockControlStatus::Disabled:
                return QStringLiteral("DISABLED");
            case MasterStockControlStatus::ConstructorRefused:
                return QStringLiteral("CONSTRUCTOR_REFUSED");
            case MasterStockControlStatus::ExtractionRefused:
                return QStringLiteral("EXTRACTION_REFUSED");
            case MasterStockControlStatus::StockPreparationRefused:
                return QStringLiteral("STOCK_PREPARATION_REFUSED");
            case MasterStockControlStatus::FinalizationRefused:
                return QStringLiteral("FINALIZATION_REFUSED");
            case MasterStockControlStatus::Retained:
                return QStringLiteral("RETAINED");
            case MasterStockControlStatus::Installed:
                return QStringLiteral("INSTALLED");
        }
        return QStringLiteral("INVALID");
    }

    MasterStockControlTiming controlMasterAssignment(
        const MasterShadowConfig & config, const AssignmentInput & input, AssignmentResult & assignment,
        const PropertyStockField & field, const DecisionTraceIdentity & identity)
    {
        MasterStockControlTiming measured;
        if (!config.controlsStock())
        {
            return measured;
        }
        QElapsedTimer total;
        total.start();
        RefinementLedger ledger;
        MasterAssignmentProfile profile;
        MasterProfileScope controlTiming(&profile, MasterPhase::StockControl);
        std::optional<MasterCaptureResult> capture;
        MasterAssignmentStockContext stock;
        std::optional<MasterAssignmentSearchResult> search;
        MasterInstallationResult installed;
        const ControlObservation observation{
            config, identity, assignment, measured, ledger, profile, capture, stock, search, installed};
        if (!ledger.open(config.allowance))
        {
            assignment = AssignmentResult{};
            assignment.stockConstructionStatus = StockConstructionStatus::InvalidInput;
            return finishControl(observation, controlTiming, total, MasterStockControlStatus::ConstructorRefused);
        }
        const auto constructorStart = total.nsecsElapsed();
        {
            MasterProfileScope timing(&profile, MasterPhase::IncomingConstruction);
            assignment = MaximumValueAssignment::constructStockIncumbent(input, ledger);
        }
        measured.constructorNanos = total.nsecsElapsed() - constructorStart;
        measured.constructorComplete = assignment.stockConstructionStatus == StockConstructionStatus::Complete;
        if (!measured.constructorComplete)
        {
            return finishControl(observation, controlTiming, total, MasterStockControlStatus::ConstructorRefused);
        }
        capture = captureMasterAssignment(input, assignment, identity.planSequence, ledger, &profile);
        if (!capture->extraction.valid())
        {
            return finishControl(observation, controlTiming, total, MasterStockControlStatus::ExtractionRefused);
        }
        const auto & problem = *capture->extraction.problem;
        if (stock.prepare(field, input, ledger, &profile, config.tierAuditMode) != MasterStockSetupStatus::Prepared)
        {
            return finishControl(observation, controlTiming, total, MasterStockControlStatus::StockPreparationRefused);
        }
        MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs(), &profile);
        MasterAssignmentInstallation installation(problem, evaluator, ledger);
        installed.status = installation.prepare(assignment);
        measured.reservedWork = installation.reservedWork();
        if (installed.status != MasterInstallationStatus::Reserved)
        {
            return finishControl(observation, controlTiming, total, MasterStockControlStatus::FinalizationRefused);
        }
        const auto searchStart = total.nsecsElapsed();
        search = searchMasterAssignment(problem, evaluator, ledger, {.profile = &profile});
        measured.searchNanos = total.nsecsElapsed() - searchStart;
        const auto finalizationStart = total.nsecsElapsed();
        {
            MasterProfileScope timing(&profile, MasterPhase::Installation);
            installed = installation.finish(search->improvement, assignment);
        }
        measured.finalizationNanos = total.nsecsElapsed() - finalizationStart;
        measured.finalizationWork = installed.finalizationWork;
        return finishControl(observation, controlTiming, total, installed.status == MasterInstallationStatus::Installed
            ? MasterStockControlStatus::Installed : MasterStockControlStatus::Retained);
    }
}
