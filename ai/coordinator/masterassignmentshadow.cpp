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

    enum class ShadowOutcome : std::int8_t
    {
        MasterCertifiedBetter,
        LegacyCertifiedBetter,
        ExactEquality,
        Unresolved,
        UnsupportedDomain,
        RepresentationMismatch,
        ReplayFailure,
    };

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

    QJsonObject evaluateShadow(const AssignmentInput & input,
                               const AssignmentResult & incumbent, const PropertyStockField & field,
                               RefinementLedger & ledger, const DecisionTraceIdentity & identity,
                               MasterAssignmentProfile & profile)
    {
        MasterProfileScope timing(&profile, MasterPhase::Shadow);
        QJsonObject record{
            {"outcome", outcomeName(ShadowOutcome::Unresolved)}, {"fullDomain", true},
            {"domainAccounted", true}, {"legacyPlanUnchanged", true}, {"extracted", false}};
        if (ledger.stats().refusals != 0)
        {
            record.insert("setupBudgetExhausted", true);
            return record;
        }
        auto capture = captureMasterAssignment(input, incumbent, identity.planSequence, ledger, &profile);
        record.insert("extractionWork", QString::number(capture.workUsed));
        record.insert("setupBudgetExhausted", capture.budgetExhausted);
        record.insert("setupArithmeticFailure", capture.arithmeticFailure);
        if (!capture.extraction.valid())
        {
            if (!capture.budgetExhausted && !capture.arithmeticFailure)
            {
                record.insert("outcome", outcomeName(ShadowOutcome::RepresentationMismatch));
                record.insert("representationError", static_cast<int>(capture.extraction.error));
                record.insert("errorActor", capture.extraction.actorSlot);
                record.insert("errorCandidate", capture.extraction.candidateIndex);
            }
            return record;
        }
        record.insert("extracted", true);
        const auto & problem = *capture.extraction.problem;
        const auto originalPlan = incumbent.plan;
        const auto originalOrder = incumbent.executionOrder;
        const auto model = masterModelRecord(problem);
        record.insert("model", model);
        record.insert("modelSummarySHA256", QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(model).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        MasterAssignmentStockContext stock;
        const auto setup = stock.prepare(field, input, ledger, &profile);
        record.insert("stockSetup", static_cast<int>(setup));
        if (setup != MasterStockSetupStatus::Prepared)
        {
            record.insert("setupBudgetExhausted", setup == MasterStockSetupStatus::BudgetExhausted);
            record.insert("setupArithmeticFailure", setup == MasterStockSetupStatus::ArithmeticFailure);
        }
        else
        {
            MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs(), &profile);
            const auto result = searchMasterAssignment(problem, evaluator, ledger, {.profile = &profile});
            const auto outcome = outcomeOf(result);
            record.insert("outcome", outcomeName(outcome));
            if (outcome == ShadowOutcome::ExactEquality)
            {
                record.insert("equalityBasis", QStringLiteral("identical full assignment, not a closed objective gap"));
            }
            record.insert("search", masterSearchRecord(result));
            if (const auto* future = evaluator.futureCatalog())
            {
                record.insert("futureResources", static_cast<qint64>(future->columns.size()));
                record.insert("futureActions", static_cast<qint64>(future->actions.size()));
                record.insert("futureClasses", static_cast<qint64>(future->classes.size()));
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
        const bool unchanged = incumbent.plan.equivalentState(originalPlan) && incumbent.executionOrder == originalOrder;
        record.insert("legacyPlanUnchanged", unchanged);
        if (!unchanged)
        {
            record.insert("outcome", outcomeName(ShadowOutcome::RepresentationMismatch));
        }
        return record;
    }
}

namespace Coordinator
{
    std::int64_t observeMasterAssignment(const MasterShadowConfig & config, const AssignmentInput & input,
                                        const AssignmentResult & incumbent, const PropertyStockField & field,
                                        const RefinementLedger & legacyLedger, RefinementLedger & masterLedger,
                                        const DecisionTraceIdentity & identity)
    {
        if (!config.enabled())
        {
            return 0;
        }
        QElapsedTimer total;
        total.start();
        const auto legacyBefore = masterWorkRecord(legacyLedger.stats());
        MasterAssignmentProfile profile;
        auto record = evaluateShadow(input, incumbent, field, masterLedger, identity, profile);
        record.insert("schemaVersion", SHADOW_SCHEMA_VERSION);
        record.insert("day", identity.day);
        record.insert("player", identity.playerId);
        record.insert("planSequence", QString::number(identity.planSequence));
        record.insert("horizon", identity.horizonTurns);
        record.insert("legacyExecuted", true);
        record.insert("installed", false);
        record.insert("coldCaches", true);
        record.insert("allowance", QString::number(config.allowance));
        record.insert("masterWork", masterWorkRecord(masterLedger.stats()));
        record.insert("legacyWork", legacyBefore);
        record.insert("legacyLedgerUnchanged", legacyBefore == masterWorkRecord(legacyLedger.stats()));
        record.insert("profile", masterProfileRecord(profile));
        record.insert("observeNanosBeforeWrite", QString::number(total.nsecsElapsed()));
        QFile output(config.outputPath);
        const auto line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (!output.open(QIODevice::WriteOnly | QIODevice::Append) ||
            output.write(line) != line.size() || !output.flush())
        {
            qWarning() << "Master assignment shadow receipt write failed:" << config.outputPath << output.errorString();
        }
        return total.nsecsElapsed();
    }
}
