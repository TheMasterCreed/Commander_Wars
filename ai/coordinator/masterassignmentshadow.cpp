#include "ai/coordinator/masterassignmentshadow.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmenttrace.h"

namespace
{
    using namespace Coordinator;
    constexpr std::int32_t SHADOW_SCHEMA_VERSION = 2;

    using ShadowOutcome = MasterShadowOutcome;

    QString outcomeName(ShadowOutcome outcome)
    {
        switch (outcome)
        {
            case ShadowOutcome::MasterCertifiedBetter:
                return QStringLiteral("MASTER_CERTIFIED_BETTER");
            case ShadowOutcome::LegacyCertifiedBetter:
                return QStringLiteral("LEGACY_CERTIFIED_BETTER");
            case ShadowOutcome::ExactEquality:
                return QStringLiteral("EXACT_EQUALITY");
            case ShadowOutcome::Unresolved:
                return QStringLiteral("UNRESOLVED");
            case ShadowOutcome::UnsupportedDomain:
                return QStringLiteral("UNSUPPORTED_DOMAIN");
            case ShadowOutcome::RepresentationMismatch:
                return QStringLiteral("REPRESENTATION_MISMATCH");
            case ShadowOutcome::ReplayFailure:
                return QStringLiteral("REPLAY_FAILURE");
        }
        return QStringLiteral("UNRESOLVED");
    }

    ShadowOutcome outcomeOf(const MasterAssignmentSearchResult & result)
    {
        if (result.stats.proofFailures != 0 || result.invalidInput)
        {
            return ShadowOutcome::ReplayFailure;
        }
        if (result.improvement)
        {
            return ShadowOutcome::MasterCertifiedBetter;
        }
        if (result.best && result.incumbent &&
            result.best->state.choices == result.incumbent->state.choices)
        {
            return ShadowOutcome::ExactEquality;
        }
        if (result.stats.unsupported != 0)
        {
            return ShadowOutcome::UnsupportedDomain;
        }
        return ShadowOutcome::Unresolved;
    }

    void writeShadow(const MasterShadowObservation & observation, QElapsedTimer & total,
                     MasterShadowTiming & measured)
    {
        const auto recordStart = total.nsecsElapsed();
        ++measured.exportRecords;
        auto record = masterShadowRecord(observation);
        record.insert("observeNanosBeforeWrite", QString::number(total.nsecsElapsed()));
        const auto encodingStart = total.nsecsElapsed();
        measured.recordNanos = encodingStart - recordStart;
        const auto line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        const auto outputStart = total.nsecsElapsed();
        measured.encodingNanos = outputStart - encodingStart;
        measured.exportBytes = line.size();
        {
            QFile output(observation.config.outputPath);
            measured.exportSucceeded = output.open(QIODevice::WriteOnly | QIODevice::Append) &&
                output.write(line) == line.size() && output.flush();
            if (!measured.exportSucceeded)
            {
                qWarning() << "Master assignment shadow receipt write failed:"
                           << observation.config.outputPath << output.errorString();
            }
        }
        measured.outputNanos = total.nsecsElapsed() - outputStart;
    }

    void finishShadow(const MasterShadowObservation & observation, MasterProfileScope & scope,
                      QElapsedTimer & total, MasterShadowTiming & measured, MasterShadowInspection* inspection)
    {
        scope.finish();
        measured.computationNanos = total.nsecsElapsed();
        measured.phases = observation.profile.samples;
        measured.firstImprovementNanos = observation.profile.firstImprovementNanos;
        measured.outcome = observation.outcome;
        measured.legacyPlanUnchanged = observation.legacyPlanUnchanged;
        measured.legacyLedgerUnchanged = observation.legacyBefore == observation.legacyAfter;
        if (observation.search != nullptr)
        {
            measured.firstImprovementWork = observation.search->stats.firstImprovementWork;
            measured.refinementRefusal = observation.search->refinementRefusal;
            measured.gap = observation.search->gap;
            if (observation.search->improvement)
            {
                measured.improvementLower = observation.search->improvement->comparison.deltaLower;
            }
        }
        if (observation.config.exportMode == MasterShadowExportMode::Full)
        {
            writeShadow(observation, total, measured);
        }
        if (inspection != nullptr)
        {
            const auto inspectionStart = total.nsecsElapsed();
            inspection->completed(observation);
            measured.inspectionNanos = total.nsecsElapsed() - inspectionStart;
        }
    }

    void evaluateShadow(const AssignmentInput & input, const AssignmentResult & incumbent,
                        const PropertyStockField & field, RefinementLedger & ledger,
                        MasterShadowObservation & observation, MasterAssignmentProfile & profile,
                        QElapsedTimer & total, MasterShadowTiming & measured, MasterShadowInspection* inspection)
    {
        MasterProfileScope timing(&profile, MasterPhase::Shadow);
        if (ledger.stats().refusals != 0)
        {
            finishShadow(observation, timing, total, measured, inspection);
            return;
        }
        auto capture = captureMasterAssignment(input, incumbent, observation.identity.planSequence, ledger, &profile);
        observation.capture = &capture;
        if (!capture.extraction.valid())
        {
            if (!capture.budgetExhausted && !capture.arithmeticFailure)
            {
                observation.outcome = ShadowOutcome::RepresentationMismatch;
            }
            finishShadow(observation, timing, total, measured, inspection);
            return;
        }
        const auto & problem = *capture.extraction.problem;
        const auto originalPlan = incumbent.plan;
        const auto originalOrder = incumbent.executionOrder;
        MasterAssignmentStockContext stock;
        const auto setup = stock.prepare(field, input, ledger, &profile, observation.config.tierAuditMode);
        observation.stockSetup = setup;
        if (setup != MasterStockSetupStatus::Prepared)
        {
            observation.legacyPlanUnchanged =
                incumbent.plan.equivalentState(originalPlan) && incumbent.executionOrder == originalOrder;
            if (!observation.legacyPlanUnchanged)
            {
                observation.outcome = ShadowOutcome::RepresentationMismatch;
            }
            finishShadow(observation, timing, total, measured, inspection);
            return;
        }
        MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs(), &profile);
        const auto result = searchMasterAssignment(problem, evaluator, ledger, {.profile = &profile});
        observation.search = &result;
        observation.session = stock.session();
        observation.evaluator = &evaluator;
        observation.future = evaluator.futureCatalog();
        observation.outcome = outcomeOf(result);
        observation.legacyPlanUnchanged =
            incumbent.plan.equivalentState(originalPlan) && incumbent.executionOrder == originalOrder;
        if (!observation.legacyPlanUnchanged)
        {
            observation.outcome = ShadowOutcome::RepresentationMismatch;
        }
        finishShadow(observation, timing, total, measured, inspection);
    }
}

namespace Coordinator
{
    QString masterShadowOutcomeName(MasterShadowOutcome outcome)
    {
        return outcomeName(outcome);
    }

    QJsonObject masterShadowRecord(const MasterShadowObservation & observation)
    {
        const auto & identity = observation.identity;
        QJsonObject record{
            {"outcome", masterShadowOutcomeName(observation.outcome)}, {"fullDomain", true}, {"domainAccounted", true},
            {"legacyPlanUnchanged", observation.legacyPlanUnchanged}, {"extracted", false},
            {"tierAuditMode", static_cast<int>(observation.config.tierAuditMode)},
            {"schemaVersion", SHADOW_SCHEMA_VERSION}, {"day", identity.day}, {"player", identity.playerId},
            {"planSequence", QString::number(identity.planSequence)}, {"horizon", identity.horizonTurns},
            {"legacyExecuted", true}, {"installed", false}, {"coldCaches", true},
            {"allowance", QString::number(observation.config.allowance)},
            {"masterWork", masterWorkRecord(observation.masterLedger.stats())},
            {"lastRefusal", masterRefusalRecord(observation.masterLedger.lastRefusal())},
            {"firstImprovementNanos", masterValueRecord(observation.profile.firstImprovementNanos)},
            {"legacyWork", masterWorkRecord(observation.legacyBefore)},
            {"legacyLedgerUnchanged", observation.legacyBefore == observation.legacyAfter},
            {"profile", masterProfileRecord(observation.profile)}};
        if (observation.capture == nullptr)
        {
            record.insert("setupBudgetExhausted", true);
            return record;
        }
        const auto & capture = *observation.capture;
        record.insert("extractionWork", QString::number(capture.workUsed));
        record.insert("setupBudgetExhausted", capture.budgetExhausted);
        record.insert("setupArithmeticFailure", capture.arithmeticFailure);
        if (!capture.extraction.valid())
        {
            if (!capture.budgetExhausted && !capture.arithmeticFailure)
            {
                record.insert("representationError", static_cast<int>(capture.extraction.error));
                record.insert("errorActor", capture.extraction.actorSlot);
                record.insert("errorCandidate", capture.extraction.candidateIndex);
            }
            return record;
        }
        record.insert("extracted", true);
        const auto model = masterModelRecord(*capture.extraction.problem);
        record.insert("model", model);
        record.insert("modelSummarySHA256", QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(model).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        if (observation.stockSetup)
        {
            const auto setup = *observation.stockSetup;
            record.insert("stockSetup", static_cast<int>(setup));
            if (setup != MasterStockSetupStatus::Prepared)
            {
                record.insert("setupBudgetExhausted", setup == MasterStockSetupStatus::BudgetExhausted);
                record.insert("setupArithmeticFailure", setup == MasterStockSetupStatus::ArithmeticFailure);
            }
        }
        if (observation.search != nullptr)
        {
            const auto & result = *observation.search;
            record.insert("refinementRefusal", masterRefinementRefusalRecord(result.refinementRefusal));
            if (observation.outcome == ShadowOutcome::ExactEquality)
            {
                record.insert("equalityBasis", QStringLiteral("identical full assignment, not a closed objective gap"));
            }
            record.insert("search", masterSearchRecord(result));
            if (observation.future != nullptr)
            {
                record.insert("futureResources", static_cast<qint64>(observation.future->columns.size()));
                record.insert("futureActions", static_cast<qint64>(observation.future->actions.size()));
                record.insert("futureClasses", static_cast<qint64>(observation.future->classes.size()));
            }
            if (result.incumbent)
            {
                record.insert("incumbent", masterEvaluationRecord(*result.incumbent));
            }
            if (result.best)
            {
                record.insert("bestLowerPlan", masterEvaluationRecord(*result.best));
            }
            if (result.improvement)
            {
                record.insert("provedIncumbent", masterEvaluationRecord(result.improvement->incumbent));
                record.insert("provedChallenger", masterEvaluationRecord(result.improvement->challenger));
            }
        }
        return record;
    }

    MasterShadowTiming observeMasterAssignment(const MasterShadowConfig & config, const AssignmentInput & input,
                                        const AssignmentResult & incumbent, const PropertyStockField & field,
                                        const RefinementLedger & legacyLedger, RefinementLedger & masterLedger,
                                        const DecisionTraceIdentity & identity, MasterShadowInspection* inspection)
    {
        MasterShadowTiming measured;
        if (!config.enabled())
        {
            return measured;
        }
        QElapsedTimer total;
        total.start();
        const auto legacyBefore = legacyLedger.stats();
        MasterAssignmentProfile profile;
        MasterShadowObservation observation{
            config, identity, profile, legacyBefore, legacyLedger.stats(), masterLedger};
        evaluateShadow(input, incumbent, field, masterLedger, observation, profile, total, measured, inspection);
        measured.totalNanos = total.nsecsElapsed();
        return measured;
    }
}
