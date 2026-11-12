#include "tests/arena/masterstockreference.h"

#include <array>
#include <limits>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace
{
    using namespace Coordinator;
    using namespace ArenaMasterStockReference;
    using ArenaMasterComparisonProbe::StockReferenceResult;
    constexpr std::array<const char*, 3> STOP_REASONS{
        "STOCK_REFERENCE_NOT_REQUESTED", "STOCK_REFERENCE_COMPLETE", "STOCK_REFERENCE_FAILED"};
    constexpr std::array<const char*, 3> TERM_NAMES{"friendly", "enemy", "dual"};
    constexpr int REFERENCE_SCHEMA = 1;

    enum class Outcome : std::int8_t
    {
        Running,
        SignsDecided,
        StepsExhausted,
        FundingRefused,
        Invalid,
        WriteFailure,
    };

    constexpr std::array<const char*, 6> OUTCOME_NAMES{
        "Running", "SignsDecided", "StepsExhausted", "FundingRefused", "Invalid", "WriteFailure"};

    QString sha256(const QByteArray & bytes)
    {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }

    bool reject(QString & error, const QString & reason)
    {
        error = reason;
        return false;
    }

    bool onlyKeys(const QJsonObject & object, std::initializer_list<const char*> names)
    {
        for (auto value = object.begin(); value != object.end(); ++value)
        {
            if (std::none_of(names.begin(), names.end(),
                [&](const char* name) { return value.key() == QLatin1String(name); }))
            {
                return false;
            }
        }
        return true;
    }

    bool readInteger(const QJsonValue & value, std::int32_t & result)
    {
        result = value.toInt(std::numeric_limits<std::int32_t>::min());
        return value.isDouble() && value.toDouble() == result;
    }

    bool readPositiveWork(const QJsonValue & value, std::int64_t & result)
    {
        bool valid = false;
        const auto text = value.toString();
        result = text.toLongLong(&valid);
        return value.isString() && valid && result > 0 && QString::number(result) == text;
    }

    bool readTuple(const QJsonValue & value, std::size_t actorCount, std::vector<std::int32_t> & result)
    {
        if (!value.isArray() || static_cast<std::size_t>(value.toArray().size()) != actorCount)
        {
            return false;
        }
        for (const auto & entry : value.toArray())
        {
            std::int32_t choice = NO_CANDIDATE;
            if (!readInteger(entry, choice) || choice < NO_CANDIDATE)
            {
                return false;
            }
            result.push_back(choice);
        }
        return true;
    }

    bool readIdentity(const QJsonObject & config, const DecisionTraceIdentity & identity, Request & request)
    {
        std::int32_t day = -1;
        std::int32_t player = -1;
        bool validSequence = false;
        const auto sequenceText = config.value("planSequence").toString();
        const auto sequence = sequenceText.toULongLong(&validSequence);
        request.sourceRevision = config.value("sourceRevision").toString();
        request.fixture = config.value("fixture").toString();
        request.modelSHA256 = config.value("modelSHA256").toString().toLower();
        request.identity = identity;
        static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
        return readInteger(config.value("day"), day) && day == identity.day &&
            readInteger(config.value("player"), player) && player == identity.playerId &&
            config.value("planSequence").isString() && validSequence && sequence == identity.planSequence &&
            sequenceText == QString::number(sequence) &&
            !request.sourceRevision.isEmpty() && !request.fixture.isEmpty() &&
            hashPattern.match(request.modelSHA256).hasMatch() &&
            readPositiveWork(config.value("allowance"), request.allowance);
    }

    bool readFamily(const QJsonObject & config, std::size_t actorCount, Request & request)
    {
        const auto family = config.value("family").toArray();
        if (!config.value("family").isArray() || family.isEmpty() ||
            static_cast<std::size_t>(family.size()) > MAXIMUM_PLANS ||
            !readTuple(config.value("incumbent"), actorCount, request.incumbent))
        {
            return false;
        }
        for (const auto & value : family)
        {
            std::vector<std::int32_t> tuple;
            if (!readTuple(value, actorCount, tuple) ||
                std::find(request.family.begin(), request.family.end(), tuple) != request.family.end())
            {
                return false;
            }
            request.family.push_back(std::move(tuple));
        }
        return request.family.front() == request.incumbent;
    }

    bool readSteps(const QJsonObject & config, Request & request)
    {
        if (!config.value("steps").isArray() ||
            static_cast<std::size_t>(config.value("steps").toArray().size()) > MAXIMUM_STEPS)
        {
            return false;
        }
        for (const auto & value : config.value("steps").toArray())
        {
            const auto object = value.toObject();
            std::int32_t plan = -1;
            Step step;
            const auto name = object.value("term").toString();
            const auto term = std::find_if(TERM_NAMES.begin(), TERM_NAMES.end(),
                [&](const char* candidate) { return name == QLatin1String(candidate); });
            if (!value.isObject() || !onlyKeys(object, {"plan", "term", "allowance"}) ||
                !readInteger(object.value("plan"), plan) || plan < 0 ||
                static_cast<std::size_t>(plan) >= request.family.size() || term == TERM_NAMES.end() ||
                !readPositiveWork(object.value("allowance"), step.allowance))
            {
                return false;
            }
            step.plan = static_cast<std::size_t>(plan);
            step.term = static_cast<MasterRefinementTerm>(term - TERM_NAMES.begin());
            request.steps.push_back(step);
        }
        return true;
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

    bool decided(const RelativeStockComparison & comparison)
    {
        return comparison.valid() && (comparison.deltaLower > 0 || comparison.deltaUpper <= 0);
    }

    class Writer
    {
    public:
        explicit Writer(const QString & path) : m_output(path)
        {
        }

        bool open()
        {
            return QDir::isAbsolutePath(m_output.fileName()) &&
                m_output.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        }

        bool write(const QString & event, QJsonObject record)
        {
            record.insert("event", event);
            record.insert("sequence", QString::number(m_sequence));
            const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
            if (m_output.write(bytes) != bytes.size() || !m_output.flush())
            {
                qWarning() << "Stock reference write failed:" << m_output.fileName() << m_output.errorString();
                return false;
            }
            ++m_sequence;
            return true;
        }

    private:
        QFile m_output;
        std::uint64_t m_sequence{0};
    };

    struct Plan
    {
        MasterAssignmentState state;
        MasterAssignmentOracle::TupleStatus disposition{MasterAssignmentOracle::TupleStatus::InvalidInput};
        std::optional<MasterAssignmentEvaluation> evaluation;
        RelativeStockComparison comparison;
        bool current{false};
        bool comparisonFresh{false};
    };

    class FamilyRun
    {
    public:
        FamilyRun(const Request & request, const MasterAssignmentProblem & problem,
                  MasterAssignmentEvaluator & evaluator, RefinementLedger & ledger,
                  const QString & output, MasterAssignmentProfile* profile)
            : m_request(request), m_problem(problem), m_evaluator(evaluator), m_ledger(ledger),
              m_writer(output), m_profile(profile)
        {
        }

        StockReferenceResult run()
        {
            m_timer.start();
            if (!m_writer.open() || !begin())
            {
                return StockReferenceResult::Failed;
            }
            if (m_request.family.empty() || m_request.family.size() > MAXIMUM_PLANS ||
                m_request.steps.size() > MAXIMUM_STEPS ||
                !matches(m_request, m_problem) || !m_evaluator.matches(m_problem, m_ledger) ||
                m_ledger.stats().total != m_request.allowance)
            {
                fail(Outcome::Invalid, QStringLiteral("reference context, model, incoming tuple or ledger mismatch"));
                return finish();
            }
            if (!prepareReferences() || !evaluate())
            {
                snapshot(-1, {});
                return finish();
            }
            if (!snapshot(-1, {}))
            {
                return finish();
            }
            for (std::size_t index = 0; index < m_request.steps.size() && m_outcome == Outcome::Running; ++index)
            {
                if (allDecided())
                {
                    m_outcome = Outcome::SignsDecided;
                    break;
                }
                perform(index);
            }
            if (m_outcome == Outcome::Running)
            {
                m_outcome = allDecided() ? Outcome::SignsDecided : Outcome::StepsExhausted;
            }
            return finish();
        }

    private:
        bool fail(Outcome outcome, const QString & error)
        {
            m_outcome = outcome;
            m_error = error;
            return false;
        }

        bool begin()
        {
            QJsonArray family;
            for (const auto & tuple : m_request.family)
            {
                family.append(masterChoicesRecord(tuple));
            }
            return m_writer.write(QStringLiteral("begin"), {
                {"schemaVersion", REFERENCE_SCHEMA}, {"requestSHA256", m_request.requestSHA256},
                {"sourceRevision", m_request.sourceRevision}, {"fixture", m_request.fixture},
                {"modelSHA256", m_request.modelSHA256}, {"model", masterModelRecord(m_problem)},
                {"day", m_request.identity.day}, {"player", m_request.identity.playerId},
                {"planSequence", QString::number(m_request.identity.planSequence)},
                {"horizon", m_request.identity.horizonTurns}, {"allowance", QString::number(m_request.allowance)},
                {"incumbent", masterChoicesRecord(m_request.incumbent)}, {"family", family},
                {"plannedSteps", static_cast<qint64>(m_request.steps.size())},
                {"diagnosticOnly", true}, {"productionSearchRun", false}, {"installed", false},
                {"globalOptimumClaimed", false}, {"productionPreambleOutsideReferenceLedger", true},
                {"legalityCrosscheckAndSerializationOutsideReferenceLedger", true},
                {"work", masterWorkRecord(m_ledger.stats())}});
        }

        bool prepareReferences()
        {
            for (std::size_t index = 0; index < m_request.family.size(); ++index)
            {
                const auto & tuple = m_request.family[index];
                MasterAssignmentOracle::Request request;
                request.sourceRevision = m_request.sourceRevision;
                request.fixture = m_request.fixture + QStringLiteral("/plan-%1").arg(static_cast<qulonglong>(index));
                request.restriction = QStringLiteral("One declared fixed-family member, not the full current-turn domain.");
                request.continuation.mode = MasterAssignmentOracle::ContinuationMode::Unavailable;
                request.continuation.immutableReference = m_request.fixture;
                request.tupleLimit = 1;
                for (const auto choice : tuple)
                {
                    request.domain.push_back({choice});
                }
                const auto reference = MasterAssignmentOracle::enumerate(m_problem, request);
                const auto exported = QJsonDocument::fromJson(
                    MasterAssignmentOracle::exportDiagnostic(m_problem, request, reference)).object();
                Plan plan{{m_problem.identity(), tuple}};
                if (reference.tuples.size() == 1)
                {
                    plan.disposition = reference.tuples.front().reference.status;
                }
                m_plans.push_back(std::move(plan));
                if (!m_writer.write(QStringLiteral("legality"), {{"plan", static_cast<qint64>(index)},
                        {"disposition", static_cast<int>(m_plans.back().disposition)}, {"reference", exported}}))
                {
                    return fail(Outcome::WriteFailure, QStringLiteral("legality output failed"));
                }
                ++m_references;
                if (reference.tuples.size() != 1 || reference.falsePositives != 0 ||
                    reference.falseNegatives != 0 || reference.reconstructionMismatches != 0)
                {
                    return fail(Outcome::Invalid, QStringLiteral("reference reconstruction disagreement or invalid tuple"));
                }
                const auto & checked = reference.tuples.front();
                const bool applicable = checked.reference.status == MasterAssignmentOracle::TupleStatus::Feasible ||
                    checked.reference.status == MasterAssignmentOracle::TupleStatus::Infeasible;
                if ((applicable && (!checked.comparisonApplicable || !checked.comparisonEqual)) ||
                    checked.reference.status == MasterAssignmentOracle::TupleStatus::InvalidInput ||
                    checked.reference.status == MasterAssignmentOracle::TupleStatus::ArithmeticFailure ||
                    (index == 0 && !checked.reference.complete))
                {
                    return fail(Outcome::Invalid, QStringLiteral("invalid reference, native reconstruction or incoming plan"));
                }
                if (index != 0 && checked.reference.complete)
                {
                    ++m_applicablePairs;
                }
            }
            return true;
        }

        bool evaluationValid(const MasterAssignmentEvaluation & evaluation)
        {
            if (evaluation.status == MasterEvaluationStatus::BudgetExhausted ||
                evaluation.objectiveStatus == MasterObjectiveStatus::BudgetExhausted)
            {
                return fail(Outcome::FundingRefused, QStringLiteral("reference evaluation exhausted its declared ledger"));
            }
            if (evaluation.status != MasterEvaluationStatus::Feasible ||
                evaluation.objectiveStatus != MasterObjectiveStatus::Verified ||
                !evaluation.lowerReplayed || !evaluation.upperVerified || !evaluation.replay ||
                !evaluation.replay->complete())
            {
                return fail(Outcome::Invalid, QStringLiteral("reference valuation or owned proof failed"));
            }
            return true;
        }

        bool evaluate()
        {
            for (auto & plan : m_plans)
            {
                if (plan.disposition != MasterAssignmentOracle::TupleStatus::Feasible)
                {
                    continue;
                }
                plan.evaluation = m_evaluator.evaluate(plan.state);
                if (!evaluationValid(*plan.evaluation))
                {
                    return false;
                }
                plan.current = true;
            }
            return true;
        }

        bool compareAll()
        {
            for (auto & plan : m_plans)
            {
                plan.comparisonFresh = false;
                plan.current = false;
            }
            if (m_plans.empty() || !m_plans.front().evaluation)
            {
                return fail(Outcome::Invalid, QStringLiteral("incoming evaluation unavailable"));
            }
            auto & incoming = *m_plans.front().evaluation;
            if (m_applicablePairs == 0)
            {
                m_plans.front().current = evaluationValid(incoming);
                return m_plans.front().current;
            }
            for (std::size_t index = 1; index < m_plans.size(); ++index)
            {
                auto & plan = m_plans[index];
                if (!plan.evaluation)
                {
                    continue;
                }
                plan.comparison = m_evaluator.compare(incoming, *plan.evaluation);
                if (!evaluationValid(incoming) || !evaluationValid(*plan.evaluation))
                {
                    return false;
                }
                if (!plan.comparison.valid())
                {
                    return fail(Outcome::Invalid, QStringLiteral("fresh whole-plan comparison failed"));
                }
                m_plans.front().current = true;
                plan.current = true;
                plan.comparisonFresh = true;
            }
            return true;
        }

        bool allDecided() const
        {
            return m_outcome != Outcome::Invalid && m_outcome != Outcome::WriteFailure &&
                !m_plans.empty() &&
                m_references == m_request.family.size() &&
                std::all_of(m_plans.begin() + 1, m_plans.end(), [](const Plan & plan)
                {
                    return plan.disposition == MasterAssignmentOracle::TupleStatus::Infeasible ||
                        (plan.disposition == MasterAssignmentOracle::TupleStatus::Feasible &&
                         plan.comparisonFresh && decided(plan.comparison));
                });
        }

        bool snapshot(std::int64_t step, const QJsonObject & operation)
        {
            if (m_outcome == Outcome::Running)
            {
                compareAll();
            }
            QJsonArray plans;
            for (std::size_t index = 0; index < m_plans.size(); ++index)
            {
                const auto & plan = m_plans[index];
                QJsonObject row{{"plan", static_cast<qint64>(index)},
                    {"choices", masterChoicesRecord(plan.state.choices)},
                    {"disposition", static_cast<int>(plan.disposition)}, {"evaluated", plan.evaluation.has_value()},
                    {"evaluationCurrent", plan.current}, {"comparisonFresh", plan.comparisonFresh}};
                if (plan.evaluation)
                {
                    row.insert("evaluation", masterEvaluationRecord(*plan.evaluation));
                }
                if (plan.comparisonFresh)
                {
                    row.insert("comparison", comparisonRecord(plan.comparison));
                }
                plans.append(row);
            }
            if (!m_writer.write(QStringLiteral("snapshot"), {{"step", static_cast<int>(step)},
                    {"operation", operation}, {"plans", plans}, {"allDecided", allDecided()},
                    {"work", masterWorkRecord(m_ledger.stats())},
                    {"elapsedNanos", QString::number(m_timer.nsecsElapsed())}}))
            {
                return fail(Outcome::WriteFailure, QStringLiteral("snapshot output failed"));
            }
            ++m_snapshots;
            return m_outcome == Outcome::Running;
        }

        void perform(std::size_t index)
        {
            const auto & step = m_request.steps[index];
            auto & plan = m_plans[step.plan];
            const auto before = m_ledger.stats().spent();
            QJsonObject operation{{"plan", static_cast<qint64>(step.plan)},
                {"term", QString::fromLatin1(TERM_NAMES[static_cast<std::size_t>(step.term)])},
                {"allowance", QString::number(step.allowance)}, {"workBefore", QString::number(before)}};
            for (auto & value : m_plans)
            {
                value.current = false;
                value.comparisonFresh = false;
            }
            if (plan.evaluation)
            {
                const auto status = m_evaluator.refine(*plan.evaluation, step.term, step.allowance);
                operation.insert("refinementStatus", static_cast<int>(status));
                operation.insert("certificatePreparation", masterCertificatePreparationRecord(m_evaluator.certificatePreparation()));
                operation.insert("refinementRefusal", masterRefinementRefusalRecord(m_evaluator.refinementRefusal()));
                if (status == MasterRefinementStatus::BudgetExhausted)
                {
                    fail(Outcome::FundingRefused, QStringLiteral("declared reference operation cannot be funded"));
                }
                else if (status == MasterRefinementStatus::Invalid)
                {
                    fail(Outcome::Invalid, QStringLiteral("native reference refinement or proof failed"));
                }
            }
            else
            {
                operation.insert("skippedNonExecutable", true);
            }
            operation.insert("workAfterRefinement", QString::number(m_ledger.stats().spent()));
            ++m_completedSteps;
            snapshot(static_cast<std::int64_t>(index), operation);
        }

        StockReferenceResult finish()
        {
            const bool successful = m_outcome == Outcome::SignsDecided || m_outcome == Outcome::StepsExhausted ||
                m_outcome == Outcome::FundingRefused;
            const auto result = successful ? StockReferenceResult::Complete : StockReferenceResult::Failed;
            QJsonArray errors;
            if (!successful)
            {
                errors.append(m_error);
            }
            QJsonObject record{{"result", ArenaMasterComparisonProbe::stockReferenceStopReason(result)},
                {"outcome", QString::fromLatin1(OUTCOME_NAMES[static_cast<std::size_t>(m_outcome)])},
                {"familySize", static_cast<qint64>(m_request.family.size())}, {"references", static_cast<qint64>(m_references)},
                {"applicablePairs", static_cast<qint64>(m_applicablePairs)}, {"completedSteps", static_cast<qint64>(m_completedSteps)},
                {"snapshots", static_cast<qint64>(m_snapshots)}, {"allDecided", allDecided()},
                {"unsupportedPlans", static_cast<qint64>(std::count_if(m_plans.begin(), m_plans.end(),
                    [](const Plan & plan) { return plan.disposition == MasterAssignmentOracle::TupleStatus::Unsupported; }))},
                {"infeasiblePlans", static_cast<qint64>(std::count_if(m_plans.begin(), m_plans.end(),
                    [](const Plan & plan) { return plan.disposition == MasterAssignmentOracle::TupleStatus::Infeasible; }))},
                {"work", masterWorkRecord(m_ledger.stats())}, {"errors", errors}, {"stopDetail", m_error},
                {"elapsedNanos", QString::number(m_timer.nsecsElapsed())},
                {"installed", false}, {"globalOptimumClaimed", false}};
            if (m_profile != nullptr)
            {
                record.insert("profile", masterProfileRecord(*m_profile));
            }
            if (!m_writer.write(QStringLiteral("end"), record))
            {
                return StockReferenceResult::Failed;
            }
            return result;
        }

        const Request & m_request;
        const MasterAssignmentProblem & m_problem;
        MasterAssignmentEvaluator & m_evaluator;
        RefinementLedger & m_ledger;
        Writer m_writer;
        MasterAssignmentProfile* m_profile;
        QElapsedTimer m_timer;
        std::vector<Plan> m_plans;
        Outcome m_outcome{Outcome::Running};
        QString m_error;
        std::uint64_t m_references{0};
        std::uint64_t m_applicablePairs{0};
        std::uint64_t m_completedSteps{0};
        std::uint64_t m_snapshots{0};
    };
}

namespace ArenaMasterComparisonProbe
{
    QString stockReferenceStopReason(StockReferenceResult result)
    {
        const auto index = static_cast<std::size_t>(result);
        return QString::fromLatin1(STOP_REASONS[index < STOP_REASONS.size()
            ? index : static_cast<std::size_t>(StockReferenceResult::Failed)]);
    }
}

namespace ArenaMasterStockReference
{
    bool enabled(const MasterShadowConfig & config, const QByteArray & stockReference)
    {
        return config.controlsStock() && config.runtime == MasterStockControlRuntime::PlayableTrial &&
            stockReference == "1";
    }

    std::optional<Request> parse(
        const QByteArray & bytes, const DecisionTraceIdentity & identity, std::size_t actorCount, QString & error)
    {
        if (bytes.isEmpty() || bytes.size() > MAXIMUM_REQUEST_BYTES)
        {
            reject(error, QStringLiteral("reference request is empty or oversized"));
            return std::nullopt;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        const auto config = document.object();
        Request request;
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            !onlyKeys(config, {"day", "player", "planSequence", "sourceRevision", "fixture", "modelSHA256",
                              "allowance", "incumbent", "family", "steps"}) ||
            !readIdentity(config, identity, request) || !readFamily(config, actorCount, request) ||
            !readSteps(config, request))
        {
            reject(error, QStringLiteral("invalid fixed-family request, identity or explicit step"));
            return std::nullopt;
        }
        request.requestSHA256 = sha256(bytes);
        return request;
    }

    bool matches(const Request & request, const MasterAssignmentProblem & problem)
    {
        return request.modelSHA256 == sha256(QJsonDocument(masterModelRecord(problem)).toJson(QJsonDocument::Compact)) &&
            request.incumbent == std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end());
    }

    StockReferenceResult record(
        const Request & request, const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator,
        RefinementLedger & ledger, const QString & outputPath, MasterAssignmentProfile* profile)
    {
        return FamilyRun(request, problem, evaluator, ledger, outputPath, profile).run();
    }

    StockReferenceResult failure(const QString & outputPath, const QByteArray & requestBytes,
                                 const QString & reason, const RefinementLedgerStats* work)
    {
        Writer writer(outputPath);
        if (writer.open() && writer.write(QStringLiteral("begin"), {{"schemaVersion", REFERENCE_SCHEMA},
                {"requestSHA256", sha256(requestBytes)}, {"diagnosticOnly", true}, {"installed", false}}))
        {
            QJsonObject end{{"result", ArenaMasterComparisonProbe::stockReferenceStopReason(StockReferenceResult::Failed)},
                {"outcome", QString::fromLatin1(OUTCOME_NAMES[static_cast<std::size_t>(Outcome::Invalid)])},
                {"errors", QJsonArray{reason}}, {"stopDetail", reason}, {"globalOptimumClaimed", false}};
            if (work != nullptr)
            {
                end.insert("work", masterWorkRecord(*work));
            }
            writer.write(QStringLiteral("end"), end);
        }
        qWarning() << "Stock reference failed:" << reason;
        return StockReferenceResult::Failed;
    }
}
