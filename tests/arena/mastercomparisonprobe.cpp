#include "tests/arena/mastercomparisonprobe.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace
{
    using namespace Coordinator;
    constexpr auto REQUEST_VARIABLE = "COW_MASTER_COMPARISON_REQUEST";
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_COMPARISON_OUTPUT";
    constexpr qsizetype MAXIMUM_STEPS = 128;

    enum class ComparedPlan : std::int8_t
    {
        Incumbent,
        Challenger,
    };

    bool writeRecord(const QString & path, const QJsonObject & record)
    {
        QFile output(path);
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (!output.open(QIODevice::WriteOnly | QIODevice::Append) ||
            output.write(bytes) != bytes.size() || !output.flush())
        {
            qWarning() << "Master comparison receipt failed:" << path << output.errorString();
            return false;
        }
        return true;
    }

    std::optional<MasterAssignmentState> selectedState(
        const MasterAssignmentProblem & problem, const QJsonArray & selected)
    {
        if (selected.size() != static_cast<qsizetype>(problem.actors().size()))
        {
            return std::nullopt;
        }
        MasterAssignmentState result{problem.identity(), {}};
        for (const auto & choice : selected)
        {
            if (!choice.isDouble() || choice.toDouble() != choice.toInt(-2))
            {
                return std::nullopt;
            }
            result.choices.push_back(choice.toInt());
        }
        return result.complete() && validMasterState(problem, result) ? std::optional{result} : std::nullopt;
    }

    bool exportReference(const MasterAssignmentProblem & problem, const MasterAssignmentState & state,
                         const QJsonObject & config, const QString & path)
    {
        MasterAssignmentOracle::Request request;
        request.sourceRevision = config.value("sourceRevision").toString();
        request.fixture = config.value("fixture").toString();
        request.restriction = QStringLiteral("One externally nominated complete tuple, not a search result.");
        request.continuation.mode = MasterAssignmentOracle::ContinuationMode::Unavailable;
        request.continuation.immutableReference = request.fixture;
        request.tupleLimit = 1;
        for (const auto choice : state.choices)
        {
            request.domain.push_back({choice});
        }
        const auto result = MasterAssignmentOracle::enumerate(problem, request);
        const auto bytes = MasterAssignmentOracle::exportDiagnostic(problem, request, result);
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(bytes) != bytes.size() || !output.flush())
        {
            qWarning() << "Master comparison reference export failed:" << path << output.errorString();
            return false;
        }
        return result.tuples.size() == 1 && result.tuples.front().reference.complete &&
            result.tuples.front().comparisonApplicable && result.tuples.front().comparisonEqual;
    }

    QJsonObject comparisonRecord(const RelativeStockComparison & comparison)
    {
        return {{"valid", comparison.valid()}, {"reason", static_cast<int>(comparison.reason)},
            {"deltaLower", QString::number(comparison.deltaLower)},
            {"deltaUpper", QString::number(comparison.deltaUpper)},
            {"economicDelta", QString::number(comparison.economicDelta)},
            {"ownedDelta", QString::number(comparison.ownedDelta)},
            {"commonEnemyCancelled", comparison.commonEnemyCancelled}};
    }

    class SearchObserver final : public MasterAssignmentSearchObserver
    {
    public:
        SearchObserver(QString path, const RelativeStockSession & session,
                       const RefinementLedger & ledger, MasterValuationTariffs tariffs)
            : m_path(std::move(path)),
              m_session(session),
              m_ledger(ledger),
              m_tariffs(tariffs)
        {
            m_timer.start();
        }

        void evaluated(const MasterAssignmentEvaluation & evaluation, std::int64_t work) override
        {
            event(evaluation, work, {{"event", QStringLiteral("evaluated")}});
        }

        void refined(const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
                     std::int64_t allowance, MasterRefinementStatus status,
                     std::int64_t workBefore, std::int64_t workAfter) override
        {
            event(evaluation, workAfter, {{"event", QStringLiteral("refined")},
                {"term", static_cast<int>(term)}, {"allowance", QString::number(allowance)},
                {"status", static_cast<int>(status)}, {"workBefore", QString::number(workBefore)},
                {"workAfter", QString::number(workAfter)},
                {"remainingBefore", QString::number(m_ledger.stats().total - workBefore)},
                {"certificatePreparationAfter", QString::number(m_session.certificatePreparationWork())},
                {"quoteTariff", QString::number(m_tariffs.quote)},
                {"witnessTariff", QString::number(m_tariffs.witness)},
                {"certificateIdentityTariff", QString::number(m_tariffs.certificateIdentity)}});
        }

        void compared(const MasterAssignmentEvaluation & challenger,
                      const RelativeStockComparison & comparison, std::int64_t work) override
        {
            event(challenger, work, {{"event", QStringLiteral("compared")},
                {"comparison", comparisonRecord(comparison)}});
        }

        bool complete() const
        {
            return m_complete;
        }

    private:
        void event(const MasterAssignmentEvaluation & evaluation, std::int64_t work, QJsonObject record)
        {
            record.insert("work", QString::number(work));
            record.insert("elapsedNanos", QString::number(m_timer.nsecsElapsed()));
            record.insert("evaluation", masterEvaluationRecord(evaluation));
            m_complete = writeRecord(m_path, record) && m_complete;
        }

        QString m_path;
        const RelativeStockSession & m_session;
        const RefinementLedger & m_ledger;
        MasterValuationTariffs m_tariffs;
        QElapsedTimer m_timer;
        bool m_complete{true};
    };

    bool decided(const RelativeStockComparison & comparison)
    {
        return comparison.valid() && (comparison.deltaLower > 0 || comparison.deltaUpper < 0 ||
            (comparison.deltaLower == 0 && comparison.deltaUpper == 0));
    }

    std::optional<MasterRefinementTerm> refinementTerm(const QString & name)
    {
        if (name == QStringLiteral("friendly"))
        {
            return MasterRefinementTerm::Friendly;
        }
        if (name == QStringLiteral("enemy"))
        {
            return MasterRefinementTerm::Enemy;
        }
        if (name == QStringLiteral("dual"))
        {
            return MasterRefinementTerm::FutureDual;
        }
        return std::nullopt;
    }
}

namespace ArenaMasterComparisonProbe
{
    void record(const AssignmentInput & input, const AssignmentResult & incumbent,
                const PropertyStockField & field, const DecisionTraceIdentity & identity)
    {
        const auto requestPath = QString::fromLocal8Bit(qgetenv(REQUEST_VARIABLE));
        const auto outputPath = QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE));
        if (requestPath.isEmpty() || outputPath.isEmpty() || qgetenv("COW_AI_ARENA_TEST") != "1")
        {
            return;
        }
        QFile requestFile(requestPath);
        if (!requestFile.open(QIODevice::ReadOnly))
        {
            qWarning() << "Master comparison request cannot be read:" << requestPath;
            return;
        }
        QJsonParseError parseError;
        const auto requestBytes = requestFile.readAll();
        const auto document = QJsonDocument::fromJson(requestBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            qWarning() << "Invalid master comparison request:" << parseError.errorString();
            return;
        }
        const auto config = document.object();
        if (config.value("day").toInt(-1) != identity.day ||
            config.value("player").toInt(-1) != identity.playerId ||
            config.value("planSequence").toString() != QString::number(identity.planSequence))
        {
            return;
        }
        QElapsedTimer timer;
        timer.start();
        MasterAssignmentProfile profile;
        bool allowanceValid = false;
        const auto allowance = config.value("allowance").toString().toLongLong(&allowanceValid);
        const auto steps = config.value("steps").toArray();
        RefinementLedger ledger;
        if (!QDir::isAbsolutePath(outputPath) || QFile::exists(outputPath) ||
            !allowanceValid || allowance < 0 || !ledger.open(allowance) ||
            steps.size() > MAXIMUM_STEPS || config.value("sourceRevision").toString().isEmpty() ||
            config.value("fixture").toString().isEmpty())
        {
            qWarning() << "Invalid or already used master comparison request.";
            return;
        }
        const auto requestHash = QString::fromLatin1(QCryptographicHash::hash(
            requestBytes, QCryptographicHash::Sha256).toHex());
        auto capture = captureMasterAssignment(input, incumbent, identity.planSequence, ledger, &profile);
        if (!capture.extraction.valid())
        {
            writeRecord(outputPath, {{"requestSHA256", requestHash}, {"extracted", false},
                {"error", static_cast<int>(capture.extraction.error)}, {"work", masterWorkRecord(ledger.stats())}});
            return;
        }
        const auto & problem = *capture.extraction.problem;
        const auto modelHash = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(masterModelRecord(problem)).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex());
        if (modelHash != config.value("modelSHA256").toString() ||
            masterChoicesRecord(problem.incumbentChoices()) != config.value("incumbent").toArray())
        {
            writeRecord(outputPath, {{"requestSHA256", requestHash}, {"modelMismatch", true},
                {"actualModelSHA256", modelHash}, {"work", masterWorkRecord(ledger.stats())}});
            return;
        }
        const auto challenger = selectedState(problem, config.value("challenger").toArray());
        const MasterAssignmentState incoming{problem.identity(),
            std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())};
        const bool incumbentReference = exportReference(problem, incoming, config, outputPath + ".incumbent.json");
        const bool challengerReference = challenger &&
            exportReference(problem, *challenger, config, outputPath + ".challenger.json");
        if (!writeRecord(outputPath, {{"requestSHA256", requestHash}, {"modelSHA256", modelHash},
            {"diagnosticNominationOnly", true}, {"installed", false},
            {"incumbentReferenceMatches", incumbentReference}, {"challengerReferenceMatches", challengerReference},
            {"referenceAndExportNanos", QString::number(timer.nsecsElapsed())},
            {"work", masterWorkRecord(ledger.stats())}}) || !incumbentReference || !challengerReference)
        {
            return;
        }
        MasterAssignmentStockContext stock;
        const auto setup = stock.prepare(field, input, ledger, &profile);
        if (setup != MasterStockSetupStatus::Prepared)
        {
            writeRecord(outputPath, {{"stockSetup", static_cast<int>(setup)}, {"work", masterWorkRecord(ledger.stats())}});
            return;
        }
        MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs(), &profile);
        if (config.value("runSearch").toBool())
        {
            const auto eventsPath = outputPath + ".events.jsonl";
            if (QFile::exists(eventsPath))
            {
                qWarning() << "Master search event path already exists:" << eventsPath;
                return;
            }
            SearchObserver observer(eventsPath, *stock.session(), ledger, stock.tariffs());
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.retainDiagnostics = true, .profile = &profile, .observer = &observer});
            writeRecord(outputPath, {{"normalSearch", true}, {"nominationInjected", false},
                {"eventsComplete", observer.complete()}, {"search", masterSearchRecord(result)},
                {"profile", masterProfileRecord(profile)}, {"elapsedNanos", QString::number(timer.nsecsElapsed())}});
            return;
        }
        auto incumbentEvaluation = evaluator.evaluate(incoming);
        auto challengerEvaluation = evaluator.evaluate(*challenger);
        auto comparison = evaluator.compare(incumbentEvaluation, challengerEvaluation);
        auto snapshot = [&](std::int32_t step, const QJsonObject & operation) {
            return writeRecord(outputPath, {{"step", step}, {"operation", operation},
                {"incumbent", masterEvaluationRecord(incumbentEvaluation)},
                {"challenger", masterEvaluationRecord(challengerEvaluation)},
                {"comparison", comparisonRecord(comparison)}, {"work", masterWorkRecord(ledger.stats())},
                {"profile", masterProfileRecord(profile)}, {"elapsedNanos", QString::number(timer.nsecsElapsed())}});
        };
        if (!snapshot(-1, {}) || !comparison.valid() || decided(comparison))
        {
            return;
        }
        for (qsizetype index = 0; index < steps.size(); ++index)
        {
            auto operation = steps[index].toObject();
            const auto sideValue = operation.value("side").toInt(-1);
            const auto term = refinementTerm(operation.value("term").toString());
            bool sliceValid = false;
            const auto slice = operation.value("allowance").toString().toLongLong(&sliceValid);
            if ((sideValue != static_cast<int>(ComparedPlan::Incumbent) &&
                 sideValue != static_cast<int>(ComparedPlan::Challenger)) ||
                !term || !sliceValid || slice <= 0)
            {
                operation.insert("invalidRequest", true);
                snapshot(static_cast<std::int32_t>(index), operation);
                return;
            }
            const auto workBefore = ledger.stats().spent();
            const auto timeBefore = timer.nsecsElapsed();
            const auto side = static_cast<ComparedPlan>(sideValue);
            auto & selected = side == ComparedPlan::Incumbent ? incumbentEvaluation : challengerEvaluation;
            const auto status = evaluator.refine(selected, *term, slice);
            operation.insert("refinementStatus", static_cast<int>(status));
            operation.insert("refinementWork", QString::number(ledger.stats().spent() - workBefore));
            operation.insert("refinementNanos", QString::number(timer.nsecsElapsed() - timeBefore));
            comparison = evaluator.compare(incumbentEvaluation, challengerEvaluation);
            if (!snapshot(static_cast<std::int32_t>(index), operation) || !comparison.valid() ||
                decided(comparison) || status == MasterRefinementStatus::Invalid ||
                status == MasterRefinementStatus::BudgetExhausted)
            {
                return;
            }
        }
    }
}
