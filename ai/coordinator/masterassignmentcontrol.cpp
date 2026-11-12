#include "ai/coordinator/masterassignmentcontrol.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmenttrace.h"

#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
#include "coreengine/settings.h"
#endif

namespace
{
    using namespace Coordinator;

    constexpr std::int32_t MASTER_PLAN_PRICING_ROUNDS = 200;
    constexpr std::int64_t MASTER_PLAN_PRICING_WORK = 8000000;
    constexpr std::int64_t MASTER_PLAN_PRICING_WITNESS_WORK = 250000;

#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
    class StaleProbeScope final
    {
    public:
        StaleProbeScope(MasterStaleProbe probe, bool detailPresentation, bool retainedImprovement)
        {
            if (probe == MasterStaleProbe::BattleAnimationSpeed && detailPresentation && retainedImprovement)
            {
                auto* settings = Settings::getInstance();
                m_restore = static_cast<quint32>(settings->getBattleAnimationSpeedValue());
                // Speeds of 100 and above derive the same speed, so use distinct raw values.
                settings->setBattleAnimationSpeed(*m_restore == 1 ? 2 : 1);
            }
        }
        StaleProbeScope(const StaleProbeScope &) = delete;
        StaleProbeScope & operator=(const StaleProbeScope &) = delete;
        bool applied() const
        {
            return m_restore.has_value();
        }
        ~StaleProbeScope()
        {
            if (m_restore)
            {
                Settings::getInstance()->setBattleAnimationSpeed(*m_restore);
            }
        }

    private:
        std::optional<quint32> m_restore;
    };
#endif

    enum class CaptureScreen : std::int8_t
    {
        Inapplicable,
        Eligible,
        BudgetExhausted,
    };

    CaptureScreen screenCapture(const MasterAssignmentProblem & problem, const PropertyStockField & field,
                                std::span<const PropertyFacts> properties, RefinementLedger & ledger)
    {
        for (const auto & actor : problem.actors())
        {
            if (!ledger.consume(1, RefinementWork::MasterPreparation))
            {
                return CaptureScreen::BudgetExhausted;
            }
            if (!problem.canChangeActor(actor.canonicalSlot) || actor.incumbentCandidateIndex < 0)
            {
                continue;
            }
            const auto & incumbent = actor.candidates[actor.incumbentCandidateIndex];
            for (const auto & candidate : actor.candidates)
            {
                if (!ledger.consume(1, RefinementWork::MasterPreparation))
                {
                    return CaptureScreen::BudgetExhausted;
                }
                if (candidate.identity.originalCandidateIndex != actor.incumbentCandidateIndex &&
                    candidate.plannable && candidate.origin == candidate.destination &&
                    candidate.source.actor.hpSteps == UNIT_HP_STEPS && candidate.capture &&
                    candidate.capture->ownerBefore == OwnerSign::Neutral &&
                    candidate.capture->ownerAfter == OwnerSign::Ours &&
                    candidate.capture->turnsUntilOwned == 0 && candidate.destination == incumbent.destination)
                {
                    const auto index = field.propertyIndexAt(candidate.destination);
                    if (index >= 0 && static_cast<std::size_t>(index) < properties.size() &&
                        properties[index].buildingId == QStringLiteral("TOWN"))
                    {
                        return CaptureScreen::Eligible;
                    }
                }
            }
        }
        return CaptureScreen::Inapplicable;
    }

    struct ControlAttempt
    {
        MasterStockAuthority authority;
        MasterStockControlStatus status{MasterStockControlStatus::StockPreparationRefused};
        MasterNativeSourcePreparation native;
        MasterStockSetupStatus stockStatus{MasterStockSetupStatus::Unprepared};
        std::optional<MasterAssignmentSearchResult> search;
        MasterInstallationResult installed;
        std::unique_ptr<MasterAssignmentSearchTrace> trace;
        std::int64_t workBefore{0};
        std::int64_t refusalsBefore{0};
        std::int64_t startedNanos{0};
        std::int64_t searchNanos{0};
        std::int64_t finalizationNanos{0};
        std::int64_t reservedWork{0};
        bool staleProbeApplied{false};
    };

    struct ControlContext
    {
        const MasterShadowConfig & config;
        const DecisionTraceIdentity & identity;
        AssignmentResult & assignment;
        RefinementLedger & ledger;
        MasterAssignmentProfile & profile;
        MasterStockControlTiming & timing;
        QElapsedTimer & total;
        QJsonArray attempts;
    };

    QJsonObject attemptRecord(const ControlContext & context, const ControlAttempt & attempt)
    {
        const auto & search = attempt.search;
        const auto & installed = attempt.installed;
        const auto & measured = context.timing;
        QJsonObject record{
            {"planningAuthority", masterStockAuthorityName(attempt.authority)},
            {"stockRevision", attempt.authority == MasterStockAuthority::GuardedCapture
                ? QJsonValue(static_cast<int>(GuardedStockRevision::CoupledNativeTerminalV4)) : QJsonValue(QJsonValue::Null)},
            {"status", masterStockControlStatusName(attempt.status)},
            {"stockSetupStatus", static_cast<int>(attempt.stockStatus)},
            {"nativeSourceStatus", masterNativeSourceStatusName(attempt.native.status)},
            {"installationStatus", static_cast<int>(installed.status)},
            {"installed", installed.status == MasterInstallationStatus::Installed},
            {"attemptWorkBefore", QString::number(attempt.workBefore)},
            {"attemptWorkAfter", QString::number(context.ledger.stats().spent())},
            {"attemptWork", QString::number(context.ledger.stats().spent() - attempt.workBefore)},
            {"attemptRefusals", QString::number(context.ledger.stats().refusals - attempt.refusalsBefore)},
            {"attemptNanos", QString::number(context.total.nsecsElapsed() - attempt.startedNanos)},
            {"reservedWork", QString::number(attempt.reservedWork)},
            {"searchWorkIncludesReservation", search.has_value()},
            {"firstImprovementWorkExcludesReservation", true},
            {"finalizationWork", QString::number(installed.finalizationWork)},
            {"firstImprovementWork", masterValueRecord(measured.firstImprovementWork)},
            {"firstImprovementNanos", masterValueRecord(measured.firstImprovementNanos)},
            {"firstImprovementLower", masterValueRecord(measured.firstImprovementLower)},
            {"searchNanos", QString::number(attempt.searchNanos)},
            {"finalizationNanos", QString::number(attempt.finalizationNanos)},
            {"detailPresentation", attempt.native.detailPresentation},
            {"staleProbeApplied", attempt.staleProbeApplied}};
        if (context.ledger.stats().refusals != attempt.refusalsBefore)
        {
            record.insert("lastRefusal", masterRefusalRecord(context.ledger.lastRefusal()));
        }
        const auto nativeRecord = masterNativeSourceWorkRecord(attempt.native);
        for (auto field = nativeRecord.begin(); field != nativeRecord.end(); ++field)
        {
            record.insert(field.key(), field.value());
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
            if (installed.proof->guardedProof)
            {
                record.insert("installedGuardedProof", masterGuardedProofRecord(*installed.proof->guardedProof));
            }
            else
            {
                record.insert("strictMargin", masterValueRecord(installed.proof->comparison.deltaLower));
                record.insert("relativeUpper", masterValueRecord(installed.proof->comparison.deltaUpper));
            }
        }
        return record;
    }

    ControlAttempt beginAttempt(ControlContext & context, MasterStockAuthority authority)
    {
        context.profile.firstImprovementNanos.reset();
        ControlAttempt attempt{.authority = authority};
        attempt.workBefore = context.ledger.stats().spent();
        attempt.refusalsBefore = context.ledger.stats().refusals;
        attempt.startedNanos = context.total.nsecsElapsed();
        return attempt;
    }

    MasterStockControlStatus finishAttempt(ControlContext & context, ControlAttempt & attempt)
    {
        auto & measured = context.timing;
        measured.installationStatus = attempt.installed.status;
        measured.searchNanos += attempt.searchNanos;
        measured.finalizationNanos += attempt.finalizationNanos;
        measured.finalizationWork += attempt.installed.finalizationWork;
        measured.reservedWork = std::max(measured.reservedWork, attempt.reservedWork);
        measured.firstImprovementNanos = context.profile.firstImprovementNanos;
        measured.firstImprovementWork.reset();
        measured.firstImprovementLower.reset();
        measured.gap.reset();
        measured.staleProbeApplied = measured.staleProbeApplied || attempt.staleProbeApplied;
        if (attempt.search)
        {
            measured.gap = attempt.search->gap;
            measured.firstImprovementLower = attempt.search->stats.firstImprovementLower;
            if (attempt.search->stats.firstImprovementWork)
            {
                measured.firstImprovementWork = *attempt.search->stats.firstImprovementWork - attempt.reservedWork;
            }
        }
        if (attempt.installed.status == MasterInstallationStatus::Installed)
        {
            measured.selectedAuthority = attempt.authority;
            if (attempt.installed.proof->guardedProof)
            {
                measured.guardedPaymentShift = attempt.installed.proof->guardedProof->relation().minimumPaymentShift;
            }
            else
            {
                measured.improvementLower = attempt.installed.proof->comparison.deltaLower;
            }
        }
        if (context.config.exportMode == MasterShadowExportMode::Full)
        {
            const auto record = attemptRecord(context, attempt);
            if (attempt.trace)
            {
                attempt.trace->finish(record);
                measured.exportSucceeded = attempt.trace->complete() && measured.exportSucceeded;
            }
            context.attempts.append(record);
        }
        return attempt.status;
    }

    void searchAndInstall(ControlContext & context, ControlAttempt & attempt,
                          const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator)
    {
        MasterAssignmentInstallation installation(problem, evaluator, context.ledger);
        attempt.installed.status = installation.prepare(context.assignment);
        attempt.reservedWork = installation.reservedWork();
        if (attempt.installed.status != MasterInstallationStatus::Reserved)
        {
            attempt.status = MasterStockControlStatus::FinalizationRefused;
            return;
        }
        MasterAssignmentSearchOptions options{.profile = &context.profile};
        if (attempt.authority == MasterStockAuthority::RelativeStock)
        {
            options.schedule.initialIncumbentAlternatives = true;
            options.schedule.initialContributionCompletion = true;
            options.schedule.allocation = MasterRefinementAllocation::OperationService;
            options.schedule.planPricingRounds = MASTER_PLAN_PRICING_ROUNDS;
            options.schedule.planPricingWork = MASTER_PLAN_PRICING_WORK;
            options.schedule.planPricingWitnessWork = MASTER_PLAN_PRICING_WITNESS_WORK;
            options.schedule.planBoundCap = MasterPlanBoundCap::Verified;
            options.schedule.planNeighborAudit = context.config.planNeighborAudit;
        }
        attempt.trace = attachMasterStockTrace(
            context.config, context.identity, problem, context.ledger, attempt.reservedWork,
            options, masterStockAuthorityName(attempt.authority));
        const auto searchStart = context.total.nsecsElapsed();
        attempt.search = searchMasterAssignment(problem, evaluator, context.ledger, options);
        attempt.searchNanos = context.total.nsecsElapsed() - searchStart;
        if (const auto & audit = attempt.search->stats.planPricing.neighborAudit)
        {
            attempt.searchNanos -= audit->nanos;
        }
        const auto finalizationStart = context.total.nsecsElapsed();
        {
            MasterProfileScope timing(&context.profile, MasterPhase::Installation);
#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
            StaleProbeScope stale(context.config.staleProbe, attempt.native.detailPresentation,
                                  attempt.search->improvement.has_value());
            attempt.staleProbeApplied = stale.applied();
#endif
            attempt.installed = installation.finish(attempt.search->improvement, context.assignment);
        }
        attempt.finalizationNanos = context.total.nsecsElapsed() - finalizationStart;
        attempt.status = MasterStockControlStatus::Retained;
        if (attempt.installed.status == MasterInstallationStatus::Installed)
        {
            attempt.status = MasterStockControlStatus::Installed;
        }
    }

    MasterStockControlStatus attemptCapture(
        ControlContext & context, const MasterAssignmentProblem & problem, GameMap & map)
    {
        auto attempt = beginAttempt(context, MasterStockAuthority::GuardedCapture);
        attempt.native = prepareMasterNativeSource(map, problem, context.ledger);
        context.timing.nativeSourceStatus = attempt.native.status;
        context.timing.detailPresentation = attempt.native.detailPresentation;
        if (attempt.native.status != MasterNativeSourceStatus::Ready || !attempt.native.source)
        {
            return finishAttempt(context, attempt);
        }
        MasterGuardedStockOwner owner(problem, context.ledger, std::move(attempt.native.source));
        MasterAssignmentEvaluator evaluator(problem, context.ledger, owner, &context.profile);
        searchAndInstall(context, attempt, problem, evaluator);
        return finishAttempt(context, attempt);
    }

    MasterStockControlStatus attemptRelativeStock(
        ControlContext & context, const MasterAssignmentProblem & problem,
        const AssignmentInput & input, const PropertyStockField & field)
    {
        auto attempt = beginAttempt(context, MasterStockAuthority::RelativeStock);
        MasterAssignmentStockContext stock;
        attempt.stockStatus = stock.prepare(field, input, context.ledger, &context.profile, context.config.tierAuditMode);
        if (attempt.stockStatus != MasterStockSetupStatus::Prepared)
        {
            return finishAttempt(context, attempt);
        }
        attempt.stockStatus = stock.prepareOperationService();
        if (attempt.stockStatus != MasterStockSetupStatus::Prepared)
        {
            return finishAttempt(context, attempt);
        }
        MasterAssignmentEvaluator evaluator(problem, context.ledger, *stock.session(), stock.tariffs(), &context.profile);
        searchAndInstall(context, attempt, problem, evaluator);
        return finishAttempt(context, attempt);
    }

    QJsonObject controlRecord(
        const ControlContext & context, const std::optional<MasterCaptureResult> & capture,
        CaptureScreen screen, std::int64_t computationNanos)
    {
        const auto & measured = context.timing;
        const auto & assignment = context.assignment;
        QJsonObject record;
        if (!context.attempts.isEmpty())
        {
            record = context.attempts.last().toObject();
        }
        const QJsonObject totalRecord{
            {"schemaVersion", 3}, {"mode", QStringLiteral("stockControl")},
            {"selectedAuthority", masterStockAuthorityName(measured.selectedAuthority)},
            {"status", masterStockControlStatusName(measured.status)},
            {"day", context.identity.day}, {"player", context.identity.playerId},
            {"planSequence", QString::number(context.identity.planSequence)}, {"horizon", context.identity.horizonTurns},
            {"allowance", QString::number(context.config.allowance)}, {"masterWork", masterWorkRecord(measured.work)},
            {"staleProbe", static_cast<int>(context.config.staleProbe)},
            {"anyStaleProbeApplied", measured.staleProbeApplied},
            {"captureDetailPresentation", measured.detailPresentation},
            {"lastRefusal", masterRefusalRecord(context.ledger.lastRefusal())},
            {"constructorStatus", static_cast<int>(assignment.stockConstructionStatus)},
            {"constructorComplete", measured.constructorComplete},
            {"constructorWork", QString::number(assignment.stockConstructionWork)},
            {"nonStockRefinementRolledBack", assignment.nonStockRefinementRolledBack},
            {"captureSourceStatus", masterNativeSourceStatusName(measured.nativeSourceStatus)},
            {"installationStatus", static_cast<int>(measured.installationStatus)},
            {"installed", measured.status == MasterStockControlStatus::Installed},
            {"peakReservedWork", QString::number(measured.reservedWork)},
            {"totalFinalizationWork", QString::number(measured.finalizationWork)},
            {"constructorNanos", QString::number(measured.constructorNanos)},
            {"searchNanos", QString::number(measured.searchNanos)},
            {"finalizationNanos", QString::number(measured.finalizationNanos)},
            {"computationNanos", QString::number(computationNanos)},
            {"captureScreen", static_cast<int>(screen)},
            {"profile", masterProfileRecord(context.profile)}, {"attempts", context.attempts}};
        for (auto field = totalRecord.begin(); field != totalRecord.end(); ++field)
        {
            record.insert(field.key(), field.value());
        }
        if (capture)
        {
            record.insert("extractionWork", QString::number(capture->workUsed));
            record.insert("extractionError", static_cast<int>(capture->extraction.error));
            if (capture->extraction.valid())
            {
                record.insert("model", masterModelRecord(*capture->extraction.problem));
            }
        }
        return record;
    }

    MasterStockControlTiming finishControl(
        ControlContext & context, MasterProfileScope & timing,
        const std::optional<MasterCaptureResult> & capture, CaptureScreen screen, MasterStockControlStatus status)
    {
        timing.finish();
        auto & measured = context.timing;
        measured.status = status;
        measured.work = context.ledger.stats();
        if (context.config.exportMode == MasterShadowExportMode::Full)
        {
            const auto exportStart = context.total.nsecsElapsed();
            const auto line = QJsonDocument(controlRecord(context, capture, screen, exportStart)).toJson(QJsonDocument::Compact) + '\n';
            QFile output(context.config.outputPath);
            const bool written = output.open(QIODevice::WriteOnly | QIODevice::Append) &&
                output.write(line) == line.size() && output.flush();
            measured.exportSucceeded = written && measured.exportSucceeded;
            if (!written)
            {
                qWarning() << "Master stock control receipt write failed:" << output.fileName() << output.errorString();
            }
            measured.exportNanos = context.total.nsecsElapsed() - exportStart;
        }
        measured.totalNanos = context.total.nsecsElapsed();
        return measured;
    }
}

namespace Coordinator
{
    QString masterStockAuthorityName(MasterStockAuthority authority)
    {
        switch (authority)
        {
            case MasterStockAuthority::Constructor:
                return QStringLiteral("constructor");
            case MasterStockAuthority::RelativeStock:
                return QStringLiteral("relativeStock");
            case MasterStockAuthority::GuardedCapture:
                return QStringLiteral("guardedTerminalCapture");
        }
        return QStringLiteral("invalid");
    }

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
        const MasterShadowConfig & config, GameMap & map, const AssignmentInput & input,
        AssignmentResult & assignment, const PropertyStockField & field, std::span<const PropertyFacts> properties,
        const DecisionTraceIdentity & identity)
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
        CaptureScreen screen{CaptureScreen::Inapplicable};
        ControlContext context{config, identity, assignment, ledger, profile, measured, total, {}};
        if (!ledger.open(config.allowance))
        {
            assignment = AssignmentResult{};
            assignment.stockConstructionStatus = StockConstructionStatus::InvalidInput;
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ConstructorRefused);
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
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ConstructorRefused);
        }
        capture = captureMasterAssignment(input, assignment, identity.planSequence, ledger, &profile);
        if (!capture->extraction.valid())
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::ExtractionRefused);
        }
        const auto & problem = *capture->extraction.problem;
        screen = screenCapture(problem, field, properties, ledger);
        if (screen == CaptureScreen::BudgetExhausted)
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::StockPreparationRefused);
        }
        if (screen == CaptureScreen::Eligible &&
            attemptCapture(context, problem, map) == MasterStockControlStatus::Installed)
        {
            return finishControl(context, controlTiming, capture, screen, MasterStockControlStatus::Installed);
        }
        const auto status = attemptRelativeStock(context, problem, input, field);
        return finishControl(context, controlTiming, capture, screen, status);
    }
}
