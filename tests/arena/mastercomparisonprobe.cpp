#include "tests/arena/mastercomparisonprobe.h"

#include <limits>
#include <map>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentstock.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"
#include "tests/arena/masterstockreference.h"
#include "ai/coordinator/masterassignmentshadow.h"

namespace
{
    using namespace Coordinator;
    constexpr auto REQUEST_VARIABLE = "COW_MASTER_COMPARISON_REQUEST";
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_COMPARISON_OUTPUT";
    constexpr auto STOCK_REFERENCE_VARIABLE = "COW_MASTER_STOCK_REFERENCE";
    constexpr auto CERTIFICATE_PREPARATION_KEY = "certificatePreparation";
    constexpr auto READINESS_AUDIT_KEY = "readinessAudit";
    constexpr auto WARM_START_AUDIT_KEY = "warmStartAudit";
    constexpr auto WARM_START_COUNTERS_KEY = "warmStartCounters";
    constexpr auto PACKING_COST_AUDIT_KEY = "packingCostAudit";
    constexpr auto PACKING_COST_OPERATIONS_KEY = "packingCostOperations";
    constexpr qsizetype MAXIMUM_STEPS = 128;
    constexpr std::int64_t READINESS_INITIAL_SLICE = 128;
    constexpr std::int64_t READINESS_MAXIMUM_SLICE = 65536;
    constexpr std::int64_t READINESS_DUAL_ALLOWANCE = 262144;
    constexpr std::int64_t WARM_START_MAXIMUM_SLICE = 1048576;

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
        SearchObserver(QString path, const MasterAssignmentEvaluator & evaluator,
                       const RefinementLedger & ledger, MasterValuationTariffs tariffs)
            : m_path(std::move(path)),
              m_evaluator(evaluator),
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
            QJsonObject preparation;
            if (term == MasterRefinementTerm::FutureDual)
            {
                preparation = masterCertificatePreparationRecord(m_evaluator.certificatePreparation());
            }
            event(evaluation, workAfter, {{"event", QStringLiteral("refined")},
                {"term", static_cast<int>(term)}, {"allowance", QString::number(allowance)},
                {"status", static_cast<int>(status)}, {"workBefore", QString::number(workBefore)},
                {"workAfter", QString::number(workAfter)},
                {"remainingBefore", QString::number(m_ledger.stats().total - workBefore)},
                {CERTIFICATE_PREPARATION_KEY, preparation},
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
        const MasterAssignmentEvaluator & m_evaluator;
        const RefinementLedger & m_ledger;
        MasterValuationTariffs m_tariffs;
        QElapsedTimer m_timer;
        bool m_complete{true};
    };

    class CertificateReadinessAudit final
    {
    public:
        CertificateReadinessAudit(MasterAssignmentEvaluator & evaluator, RelativeStockSession & session,
                                 RefinementLedger & ledger, MasterAssignmentProfile & profile)
            : m_evaluator(evaluator), m_session(session), m_ledger(ledger), m_profile(profile)
        {
        }

        QJsonObject run(const MasterAssignmentState & incoming, const MasterAssignmentState & challenger)
        {
            auto evaluated = m_evaluator.evaluate(incoming);
            auto other = m_evaluator.evaluate(challenger);
            bool valid = evaluated.quote() && other.quote() &&
                evaluated.replay->continuationKey != other.replay->continuationKey;
            for (auto cap = READINESS_INITIAL_SLICE; valid && !m_ready && cap <= READINESS_MAXIMUM_SLICE; cap *= 2)
            {
                valid = checkStep(evaluated, cap);
            }
            valid = valid && m_ready && m_missing > 0;
            QJsonObject repeated;
            bool certified = false;
            bool staleRejected = false;
            if (valid)
            {
                const auto readyQuote = *evaluated.quote();
                const auto ownedProof = evaluated.proof;
                valid = refineOtherKey(other);
                const auto before = m_ledger.stats().spent();
                const auto preparation = m_session.prepareOurCertificate(readyQuote);
                repeated = masterCertificatePreparationRecord(preparation);
                valid = valid && preparation.status == RelativeStockPreparationStatus::Ready &&
                    preparation.scanWork == m_scanWork &&
                    preparation.readinessWork == m_ledger.stats().spent() - before;
                const auto status = m_evaluator.refine(evaluated, MasterRefinementTerm::FutureDual,
                                                       READINESS_DUAL_ALLOWANCE);
                certified = status == MasterRefinementStatus::Updated && evaluated.quote() &&
                    evaluated.quote()->ourUpperCertificateId != 0 && evaluated.upperVerified &&
                    evaluated.objectiveStatus == MasterObjectiveStatus::Verified &&
                    m_session.compare(*evaluated.quote(), *evaluated.quote()).valid() &&
                    m_session.replayLowerProof(*ownedProof);
                const auto staleBefore = m_ledger.stats().spent();
                const auto setupBefore = setupCalls();
                const auto stale = m_session.prepareOurCertificate(readyQuote);
                staleRejected = stale.status == RelativeStockPreparationStatus::Invalid &&
                    stale.readinessWork == m_ledger.stats().spent() - staleBefore && setupCalls() == setupBefore;
            }
            return {{"ok", valid && certified && staleRejected}, {"diagnosticOnly", true},
                {"normalSearch", false}, {"installed", false}, {"missingRowChecks", m_missing},
                {"ready", m_ready}, {"steps", m_steps}, {"afterOtherKey", repeated},
                {"otherKeySearch", m_otherKeySearch},
                {"certificateAndOwnedWitnessValid", certified}, {"staleQuoteRejected", staleRejected},
                {"work", masterWorkRecord(m_ledger.stats())}};
        }

    private:
        bool refineOtherKey(MasterAssignmentEvaluation & evaluation)
        {
            if (m_evaluator.refine(evaluation, MasterRefinementTerm::Friendly, READINESS_INITIAL_SLICE) !=
                MasterRefinementStatus::Updated)
            {
                return false;
            }
            const auto before = m_ledger.stats();
            const auto tariff = m_session.ourRerunWork();
            const auto status = m_evaluator.refine(evaluation, MasterRefinementTerm::Friendly, READINESS_MAXIMUM_SLICE);
            const auto & after = m_ledger.stats();
            constexpr auto slice = static_cast<std::size_t>(RefinementWork::SearchSlice);
            const auto granted = after.grantedByWork[slice] - before.grantedByWork[slice];
            const auto refunded = after.refundedByWork[slice] - before.refundedByWork[slice];
            const auto states = granted - refunded - tariff;
            m_otherKeySearch = {{"granted", QString::number(granted)}, {"refunded", QString::number(refunded)},
                {"tariff", QString::number(tariff)}, {"chargedStates", QString::number(states)}};
            return status == MasterRefinementStatus::Updated && tariff >= 0 && states > 0;
        }

        std::int64_t setupCalls() const
        {
            return m_profile.samples[static_cast<std::size_t>(MasterPhase::NativeCertificateSetup)].calls;
        }

        bool checkStep(MasterAssignmentEvaluation & evaluation, std::int64_t cap)
        {
            if (m_evaluator.refine(evaluation, MasterRefinementTerm::Friendly, cap) != MasterRefinementStatus::Updated)
            {
                return false;
            }
            const auto proof = evaluation.proof;
            const auto before = m_ledger.stats().spent();
            const auto setupBefore = setupCalls();
            const auto status = m_evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, 1);
            const auto preparation = m_evaluator.certificatePreparation();
            const auto spent = m_ledger.stats().spent() - before;
            m_steps.append(QJsonObject{{"friendlyAllowance", QString::number(cap)},
                {"dualStatus", static_cast<int>(status)}, {"work", QString::number(spent)},
                {"nativeSetupCalls", QString::number(setupCalls() - setupBefore)},
                {CERTIFICATE_PREPARATION_KEY, masterCertificatePreparationRecord(preparation)}});
            if (!preparation || evaluation.proof != proof)
            {
                return false;
            }
            if (preparation->status == RelativeStockPreparationStatus::MissingRows)
            {
                ++m_missing;
                return status == MasterRefinementStatus::Unavailable && preparation->scanWork == 0 &&
                    spent == preparation->readinessWork && setupCalls() == setupBefore;
            }
            m_ready = preparation->status == RelativeStockPreparationStatus::Ready;
            m_scanWork = preparation->scanWork;
            return m_ready && status == MasterRefinementStatus::Updated &&
                setupCalls() == setupBefore + 1 && spent >= preparation->readinessWork + preparation->scanWork;
        }

        MasterAssignmentEvaluator & m_evaluator;
        RelativeStockSession & m_session;
        RefinementLedger & m_ledger;
        MasterAssignmentProfile & m_profile;
        QJsonArray m_steps;
        QJsonObject m_otherKeySearch;
        std::int32_t m_missing{0};
        std::int64_t m_scanWork{0};
        bool m_ready{false};
    };

    QJsonObject warmStartRecord(const DiagnosticWarmStart & record)
    {
        QJsonObject result{
            {"witnesses", QString::number(record.witnesses)}, {"keyWidth", QString::number(record.keyWidth)},
            {"rows", QString::number(record.rows)}, {"columns", QString::number(record.columns)},
            {"floor", QString::number(record.floor)}, {"repair", QString::number(record.repair)},
            {"booked", QString::number(record.booked)}, {"states", QString::number(record.states)},
            {"accepted", record.accepted}, {"selected", record.selected}, {"witnessReplays", record.witnessReplays},
            {"surcharge", QString::number(JointPlanStockValuer::diagnosticWarmStartTariff(
                record.witnesses, record.keyWidth, record.rows, record.columns))},
        };
        if (record.seedValue)
        {
            result.insert("seedValue", QString::number(*record.seedValue));
        }
        return result;
    }

    QJsonObject sequentialCostRecord(const SequentialCostAudit & audit)
    {
        return {{"countOperations", audit.countOperations}, {"overflow", audit.overflow},
            {"solves", QString::number(audit.solves)}, {"packingCalls", QString::number(audit.packingCalls)},
            {"optionsVisited", QString::number(audit.optionsVisited)},
            {"boundRejectedOptions", QString::number(audit.boundRejectedOptions)},
            {"occupancyProbes", QString::number(audit.occupancyProbes)},
            {"conflictingOptions", QString::number(audit.conflictingOptions)},
            {"admittedOptions", QString::number(audit.admittedOptions)},
            {"refusedOptions", QString::number(audit.refusedOptions)},
            {"optionsAfterTruncation", QString::number(audit.optionsAfterTruncation)},
            {"compatibleBoundCalls", QString::number(audit.compatibleBoundCalls)},
            {"compatibleBoundWork", QString::number(audit.compatibleBoundWork)},
            {"compatibleBoundPrunes", QString::number(audit.compatibleBoundPrunes)},
            {"compatibleBoundIncomplete", QString::number(audit.compatibleBoundIncomplete)},
            {"relaxationRepairNanos", QString::number(audit.relaxationRepairNanos)},
            {"rowOptionsNanos", QString::number(audit.rowOptionsNanos)},
            {"packingNanos", QString::number(audit.packingNanos)},
            {"finalWitnessNanos", QString::number(audit.finalWitnessNanos)}};
    }

    class WarmStartAudit final
    {
    public:
        WarmStartAudit(MasterAssignmentStockContext & stock, MasterAssignmentEvaluator & evaluator,
                       RefinementLedger & ledger, MasterAssignmentProfile & profile,
                       const MasterAssignmentProblem & problem, const AssignmentInput & input,
                       const PropertyStockField & field, SequentialTierAuditMode auditMode)
            : m_stock(stock), m_evaluator(evaluator), m_session(*stock.session()),
              m_ledger(ledger), m_profile(profile), m_problem(problem), m_input(input), m_field(field),
              m_auditMode(auditMode), m_baseOurTariff(m_session.ourRerunWork())
        {
        }

        QJsonObject run(const MasterAssignmentState & incoming, const MasterAssignmentState & challenger)
        {
            auto first = m_evaluator.evaluate(incoming);
            auto other = m_evaluator.evaluate(challenger);
            bool valid = first.quote() && other.quote() && first.quote()->key != other.quote()->key &&
                m_baseOurTariff >= 0;
            bool friendlyAccepted = false;
            bool enemyAccepted = false;
            bool keyMiss = false;
            bool sharedMask = false;
            bool ownedReplay = false;
            bool boundaries = false;
            bool preflight = false;
            bool fresh = false;
            const bool tariffs = tariffBoundaries();
            if (valid)
            {
                m_baseEnemyTariffs.emplace(first.quote()->capturedColumns,
                    m_session.enemyRerunWork(first.quote()->capturedColumns));
                m_baseEnemyTariffs.emplace(other.quote()->capturedColumns,
                    m_session.enemyRerunWork(other.quote()->capturedColumns));
                valid = earnSeed(first, MasterRefinementTerm::Friendly, friendlyAccepted);
                const auto heldProof = first.proof;
                if (valid)
                {
                    valid = attempt(*other.quote(), MasterRefinementTerm::Friendly, 1, false);
                    const auto* miss = m_stock.diagnosticLastWarmStart();
                    keyMiss = valid && miss && miss->witnesses > 0 && !miss->seedValue && !miss->accepted;
                    valid = valid && m_evaluator.refresh(other) &&
                        earnSeed(first, MasterRefinementTerm::Enemy, enemyAccepted);
                }
                if (valid && first.quote()->capturedColumns == other.quote()->capturedColumns)
                {
                    valid = m_evaluator.refresh(other) &&
                        attempt(*other.quote(), MasterRefinementTerm::Enemy, 1, false);
                    const auto* shared = m_stock.diagnosticLastWarmStart();
                    sharedMask = valid && shared && shared->seedValue && shared->accepted;
                    valid = valid && m_evaluator.refresh(other);
                }
                if (valid)
                {
                    valid = m_evaluator.refresh(first);
                    ownedReplay = valid && m_ledger.consume(m_stock.tariffs().witness, RefinementWork::MasterWitness) &&
                        heldProof && m_session.replayLowerProof(*heldProof);
                    fresh = ownedReplay && freshContext(incoming, *heldProof);
                    boundaries = valid && directBoundary(*first.quote(), MasterRefinementTerm::Friendly) &&
                        directBoundary(*first.quote(), MasterRefinementTerm::Enemy);
                    preflight = m_evaluator.refresh(other) && evaluatorBoundary(other);
                    valid = valid && differentMask(incoming, first);
                }
            }
            const bool coreChecks = valid && friendlyAccepted && enemyAccepted && keyMiss && sharedMask &&
                ownedReplay && boundaries && preflight && fresh && tariffs;
            return {{"ok", coreChecks && m_friendlySelected && m_enemySelected && m_maskCovered},
                {"coreChecksPassed", coreChecks},
                {"diagnosticOnly", true}, {"installed", false}, {"normalSearch", false},
                {"friendlyAccepted", friendlyAccepted}, {"enemyAccepted", enemyAccepted},
                {"friendlySelected", m_friendlySelected}, {"enemySelected", m_enemySelected},
                {"differentKeyMiss", keyMiss}, {"sharedEnemyMask", sharedMask},
                {"retainedProofReplays", ownedReplay}, {"directBudgetBoundaries", boundaries},
                {"evaluatorPreflight", preflight}, {"lookupTariffBoundaries", tariffs},
                {"tierAudit", static_cast<int>(m_auditMode)},
                {"differentReachableMaskCovered", m_maskCovered}, {"differentMaskTrials", m_maskTrials},
                {"freshContextCovered", fresh}, {"freshContext", m_fresh},
                {"steps", m_steps}, {"work", masterWorkRecord(m_ledger.stats())}};
        }

    private:
        static bool tariffBoundaries()
        {
            const auto tariff = JointPlanStockValuer::diagnosticWarmStartTariff;
            constexpr auto sizeMaximum = std::numeric_limits<std::size_t>::max();
            const bool common = tariff(0, sizeMaximum, sizeMaximum, sizeMaximum) == 0 &&
                tariff(1, 4, 4, 7) == 51 && tariff(2, 4, 4, 7) == 61;
            if constexpr (std::numeric_limits<std::size_t>::digits < std::numeric_limits<std::int64_t>::digits)
            {
                return common && tariff(1, sizeMaximum, sizeMaximum, sizeMaximum) ==
                    9 * static_cast<std::int64_t>(sizeMaximum) + 6;
            }
            constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
            return common &&
                tariff(1, maximum, 0, 0) < 0 &&
                tariff(1, maximum / 3, 0, 0) < 0 &&
                tariff(1, maximum / 3 - 1, 0, 0) < 0 &&
                tariff(1, maximum / 3 - 2, 0, 0) == static_cast<std::int64_t>(maximum - 1) &&
                tariff(1, 0, maximum, maximum) < 0 &&
                tariff(1, 0, std::numeric_limits<std::size_t>::max(), 0) < 0;
        }

        std::int64_t rerunWork(const RelativeStockQuote & quote, MasterRefinementTerm term) const
        {
            return term == MasterRefinementTerm::Friendly ? m_session.ourRerunWork()
                : m_session.enemyRerunWork(quote.capturedColumns);
        }

        std::int64_t setupCalls() const
        {
            return m_profile.samples[static_cast<std::size_t>(MasterPhase::FriendlySetup)].calls +
                m_profile.samples[static_cast<std::size_t>(MasterPhase::EnemySetup)].calls;
        }

        std::int64_t baseTariff(const RelativeStockQuote & quote, MasterRefinementTerm term) const
        {
            if (term == MasterRefinementTerm::Friendly)
            {
                return m_baseOurTariff;
            }
            const auto known = m_baseEnemyTariffs.find(quote.capturedColumns);
            return known == m_baseEnemyTariffs.end() ? -1 : known->second;
        }

        static std::int64_t expectedTariff(std::int64_t base, const DiagnosticWarmStart & native)
        {
            const auto surcharge = JointPlanStockValuer::diagnosticWarmStartTariff(
                native.witnesses, native.keyWidth, native.rows, native.columns);
            if (base < 0 || surcharge < 0 || !RelativeStockDetail::checkedAdd(base, surcharge))
            {
                return -1;
            }
            return base;
        }

        bool attempt(const RelativeStockQuote & quote, MasterRefinementTerm term,
                     std::int64_t cap, bool expectRefusal)
        {
            const auto tariff = rerunWork(quote, term);
            const auto before = m_ledger.stats();
            const auto callsBefore = setupCalls();
            const auto refined = term == MasterRefinementTerm::Friendly
                ? m_session.refineOur(quote.key, cap) : m_session.refineEnemy(quote.capturedColumns, cap);
            const auto* native = m_stock.diagnosticLastWarmStart();
            const auto & after = m_ledger.stats();
            const auto granted = after.granted - before.granted;
            const auto refunded = after.refunded - before.refunded;
            const auto base = baseTariff(quote, term);
            const auto expected = native ? expectedTariff(base, *native) : -1;
            QJsonObject row{
                {"term", static_cast<int>(term)}, {"cap", QString::number(cap)},
                {"capturedColumns", masterChoicesRecord(quote.capturedColumns)},
                {"balanceBefore", QString::number(before.total - before.spent())},
                {"tariff", QString::number(tariff)}, {"baseTariff", QString::number(base)},
                {"expectedTariff", QString::number(expected)}, {"granted", QString::number(granted)},
                {"refunded", QString::number(refunded)}, {"states", QString::number(refined.statesUsed)},
                {"workUsed", QString::number(refined.workUsed)}, {"valid", refined.valid},
                {"budgetExhausted", refined.budgetExhausted}, {"expectedRefusal", expectRefusal},
                {"setupCalls", QString::number(setupCalls() - callsBefore)},
            };
            if (native)
            {
                row.insert("native", warmStartRecord(*native));
                m_friendlySelected = m_friendlySelected ||
                    (term == MasterRefinementTerm::Friendly && native->selected);
                m_enemySelected = m_enemySelected ||
                    (term == MasterRefinementTerm::Enemy && native->selected);
            }
            m_steps.append(row);
            if (expectRefusal)
            {
                const auto expectedInterval = term == MasterRefinementTerm::Friendly ? quote.ours : quote.enemy;
                return !refined.valid && refined.budgetExhausted && native == nullptr &&
                    setupCalls() == callsBefore && granted == 0 && refunded == 0 &&
                    refined.workUsed == 0 && refined.statesUsed == 0 &&
                    refined.interval.lower == expectedInterval.lower &&
                    refined.interval.upper == expectedInterval.upper;
            }
            return tariff >= 0 && tariff == expected && refined.valid && !refined.budgetExhausted && native &&
                native->states == refined.statesUsed && native->states >= 0 && native->states <= cap &&
                granted == tariff + cap && refunded == cap - native->states &&
                refined.workUsed == tariff + native->states && after.spent() - before.spent() == refined.workUsed &&
                (!native->seedValue || (native->accepted && native->witnessReplays &&
                                       native->booked >= *native->seedValue));
        }

        bool earnSeed(MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term, bool & accepted)
        {
            for (std::int64_t cap = READINESS_INITIAL_SLICE; cap <= WARM_START_MAXIMUM_SLICE; cap *= 2)
            {
                if (!attempt(*evaluation.quote(), term, cap, false) || !m_evaluator.refresh(evaluation) ||
                    !attempt(*evaluation.quote(), term, 1, false) || !m_evaluator.refresh(evaluation))
                {
                    return false;
                }
                const auto* native = m_stock.diagnosticLastWarmStart();
                accepted = accepted || (native && native->accepted);
                const auto interval = term == MasterRefinementTerm::Friendly
                    ? evaluation.quote()->ours : evaluation.quote()->enemy;
                if (native && native->accepted && (native->selected || interval.exact()))
                {
                    return true;
                }
            }
            return accepted;
        }

        bool directBoundary(const RelativeStockQuote & quote, MasterRefinementTerm term)
        {
            auto required = rerunWork(quote, term);
            if (required < 0 || !RelativeStockDetail::checkedAdd(required, 1) ||
                required <= 1 || m_ledger.balance() < required)
            {
                return false;
            }
            const auto held = m_ledger.balance() - required + 1;
            const auto hold = m_ledger.draw(held, RefinementWork::MasterPreparation);
            if (hold == NO_GRANT)
            {
                return false;
            }
            const bool refused = attempt(quote, term, 1, true);
            const bool released = m_ledger.refund(hold, 1);
            const bool exact = released && m_ledger.balance() == required && attempt(quote, term, 1, false);
            const bool restored = held == 1 || m_ledger.refund(hold, held - 1);
            return refused && exact && restored;
        }

        bool evaluatorBoundary(MasterAssignmentEvaluation & evaluation)
        {
            if (evaluation.quote()->ours.exact())
            {
                return false;
            }
            const auto & tariffs = m_stock.tariffs();
            auto required = m_session.ourRerunWork();
            if (required < 0 || !RelativeStockDetail::checkedAdd(required, 1) ||
                !RelativeStockDetail::checkedAdd(required, tariffs.quote) ||
                !RelativeStockDetail::checkedAdd(required, tariffs.quote) ||
                !RelativeStockDetail::checkedAdd(required, tariffs.witness) || m_ledger.balance() < required)
            {
                return false;
            }
            const auto held = m_ledger.balance() - required + 1;
            const auto hold = m_ledger.draw(held, RefinementWork::MasterPreparation);
            if (hold == NO_GRANT)
            {
                return false;
            }
            const auto before = m_ledger.stats().spent();
            const auto callsBefore = setupCalls();
            const auto proof = evaluation.proof;
            const auto status = m_evaluator.refine(evaluation, MasterRefinementTerm::Friendly, 1);
            const auto refusal = m_evaluator.refinementRefusal();
            const bool rejected = status == MasterRefinementStatus::BudgetExhausted && refusal &&
                refusal->requested == required && refusal->balance == required - 1 &&
                before == m_ledger.stats().spent() && callsBefore == setupCalls() && evaluation.proof == proof;
            return m_ledger.refund(hold, held) && rejected;
        }

        bool freshContext(const MasterAssignmentState & incoming, const RelativeStockProofSnapshot & heldProof)
        {
            MasterAssignmentStockContext freshStock;
            if (freshStock.prepare(m_field, m_input, m_ledger, &m_profile, m_auditMode) !=
                MasterStockSetupStatus::Prepared)
            {
                return false;
            }
            MasterAssignmentEvaluator freshEvaluator(
                m_problem, m_ledger, *freshStock.session(), freshStock.tariffs(), &m_profile);
            const auto evaluated = freshEvaluator.evaluate(incoming);
            if (!evaluated.quote() ||
                !m_ledger.consume(freshStock.tariffs().witness, RefinementWork::MasterWitness))
            {
                return false;
            }
            const bool identityRejected = !freshStock.session()->replayLowerProof(heldProof);
            WarmStartAudit freshAudit(
                freshStock, freshEvaluator, m_ledger, m_profile, m_problem, m_input, m_field, m_auditMode);
            const bool refined = freshAudit.attempt(*evaluated.quote(), MasterRefinementTerm::Friendly, 1, false);
            const auto* native = freshStock.diagnosticLastWarmStart();
            const bool cold = refined && native && native->witnesses == 0 && !native->seedValue &&
                !native->accepted && !native->selected;
            const bool preserved = m_ledger.consume(m_stock.tariffs().witness, RefinementWork::MasterWitness) &&
                m_session.replayLowerProof(heldProof);
            m_fresh = {{"cold", cold}, {"foreignIdentityRejected", identityRejected},
                {"originalOwnedProofReplays", preserved}, {"steps", freshAudit.m_steps}};
            return cold && identityRejected && preserved;
        }

        static CanonicalPlanActionKey stockKey(const MasterCandidate & candidate)
        {
            return CanonicalPlanActionKey::fromAction(candidate.stockRow, candidate.stockEndpoint.x,
                candidate.stockEndpoint.y, candidate.capture.has_value());
        }

        bool rememberColdEnemyTariff(const MasterAssignmentState & state)
        {
            MasterAssignmentStockContext coldStock;
            if (coldStock.prepare(m_field, m_input, m_ledger, &m_profile, m_auditMode) !=
                MasterStockSetupStatus::Prepared)
            {
                return false;
            }
            MasterAssignmentEvaluator evaluator(
                m_problem, m_ledger, *coldStock.session(), coldStock.tariffs(), &m_profile);
            const auto evaluated = evaluator.evaluate(state);
            if (!evaluated.quote())
            {
                return false;
            }
            const auto base = coldStock.session()->enemyRerunWork(evaluated.quote()->capturedColumns);
            m_baseEnemyTariffs.emplace(evaluated.quote()->capturedColumns, base);
            return base >= 0;
        }

        bool verifyDifferentMask(const MasterAssignmentState & state, const RelativeStockQuote & quote,
                                 const RelativeStockProofSnapshot & originalProof)
        {
            if (!rememberColdEnemyTariff(state))
            {
                return false;
            }
            const bool refined = attempt(quote, MasterRefinementTerm::Enemy, 1, false);
            const auto* native = m_stock.diagnosticLastWarmStart();
            const bool miss = refined && native && native->witnesses > 0 &&
                !native->seedValue && !native->accepted;
            const bool preserved = m_ledger.consume(m_stock.tariffs().witness, RefinementWork::MasterWitness) &&
                m_session.replayLowerProof(originalProof);
            m_maskCovered = miss && preserved;
            return m_maskCovered;
        }

        bool differentMask(const MasterAssignmentState & incoming, MasterAssignmentEvaluation & original)
        {
            if (!m_evaluator.refresh(original) || m_session.pricingModel() == nullptr)
            {
                return false;
            }
            const auto originalProof = original.proof;
            const auto* model = m_session.pricingModel();
            for (std::size_t slot = 0; slot < m_problem.actors().size(); ++slot)
            {
                const auto & actor = m_problem.actors()[slot];
                if (actor.stockRow == NO_STOCK_ROW || incoming.choices[slot] == NO_CANDIDATE)
                {
                    continue;
                }
                const auto* current = model->actionTerms(stockKey(
                    actor.candidates[static_cast<std::size_t>(incoming.choices[slot])]));
                for (const auto candidateIndex : actor.canonicalCandidates)
                {
                    if (!m_ledger.consume(1, RefinementWork::MasterBoundScan))
                    {
                        return false;
                    }
                    const auto & candidate = actor.candidates[static_cast<std::size_t>(candidateIndex)];
                    const auto* alternative = model->actionTerms(stockKey(candidate));
                    if (current == nullptr || alternative == nullptr ||
                        current->capturedColumn == alternative->capturedColumn)
                    {
                        continue;
                    }
                    if (!m_ledger.consume(static_cast<std::int64_t>(m_problem.actors().size()) + 1,
                                          RefinementWork::MasterPreparation))
                    {
                        return false;
                    }
                    auto state = incoming;
                    state.choices[slot] = candidateIndex;
                    auto evaluated = m_evaluator.evaluate(state);
                    ++m_maskTrials;
                    if (!evaluated.quote() || !evaluated.replay || !evaluated.replay->complete() ||
                        evaluated.quote()->capturedColumns == original.quote()->capturedColumns)
                    {
                        continue;
                    }
                    return verifyDifferentMask(state, *evaluated.quote(), *originalProof);
                }
            }
            return true;
        }

        MasterAssignmentStockContext & m_stock;
        MasterAssignmentEvaluator & m_evaluator;
        RelativeStockSession & m_session;
        RefinementLedger & m_ledger;
        MasterAssignmentProfile & m_profile;
        const MasterAssignmentProblem & m_problem;
        const AssignmentInput & m_input;
        const PropertyStockField & m_field;
        SequentialTierAuditMode m_auditMode;
        const std::int64_t m_baseOurTariff;
        std::map<std::vector<std::int32_t>, std::int64_t> m_baseEnemyTariffs;
        QJsonArray m_steps;
        QJsonObject m_fresh;
        int m_maskTrials{0};
        bool m_maskCovered{false};
        bool m_friendlySelected{false};
        bool m_enemySelected{false};
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
    StockReferenceResult recordStockReference(
        const MasterShadowConfig & config, const AssignmentInput & input, const AssignmentResult & incumbent,
        const PropertyStockField & field, const DecisionTraceIdentity & identity)
    {
        if (!ArenaMasterStockReference::enabled(config, qgetenv(STOCK_REFERENCE_VARIABLE)))
        {
            return StockReferenceResult::NotRequested;
        }
        const auto requestPath = QString::fromLocal8Bit(qgetenv(REQUEST_VARIABLE));
        const auto outputPath = QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE));
        QFile requestFile(requestPath);
        if (!QDir::isAbsolutePath(requestPath) || !QDir::isAbsolutePath(outputPath) ||
            !requestFile.open(QIODevice::ReadOnly) ||
            requestFile.size() > ArenaMasterStockReference::MAXIMUM_REQUEST_BYTES)
        {
            return ArenaMasterStockReference::failure(
                outputPath, {}, QStringLiteral("reference input path, size or read failure"));
        }
        const auto bytes = requestFile.readAll();
        QString error;
        const auto request = ArenaMasterStockReference::parse(bytes, identity, input.actors.size(), error);
        if (!request)
        {
            return ArenaMasterStockReference::failure(outputPath, bytes, error);
        }
        RefinementLedger ledger;
        if (!ledger.open(request->allowance))
        {
            return ArenaMasterStockReference::failure(outputPath, bytes, QStringLiteral("reference ledger refused"));
        }
        MasterAssignmentProfile profile;
        auto capture = captureMasterAssignment(input, incumbent, identity.planSequence, ledger, &profile);
        if (!capture.extraction.valid() || !ArenaMasterStockReference::matches(*request, *capture.extraction.problem))
        {
            return ArenaMasterStockReference::failure(
                outputPath, bytes, QStringLiteral("reference extraction, model or incoming tuple mismatch"), &ledger.stats());
        }
        MasterAssignmentStockContext stock;
        if (stock.prepare(field, input, ledger, &profile, SequentialTierAuditMode::Full) != MasterStockSetupStatus::Prepared)
        {
            return ArenaMasterStockReference::failure(
                outputPath, bytes, QStringLiteral("cold reference stock preparation failed"), &ledger.stats());
        }
        MasterAssignmentEvaluator evaluator(*capture.extraction.problem, ledger, *stock.session(), stock.tariffs(), &profile);
        return ArenaMasterStockReference::record(
            *request, *capture.extraction.problem, evaluator, ledger, outputPath, &profile);
    }

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
        MasterAssignmentSchedule schedule;
        if (config.contains("continuationSlice"))
        {
            bool validSlice = false;
            const auto slice = config.value("continuationSlice").toString().toLongLong(&validSlice);
            if (!config.value("runSearch").toBool() || !validSlice || slice <= 0)
            {
                qWarning() << "Invalid normal-search continuation slice.";
                return;
            }
            schedule.continuationSlice = slice;
        }
        SequentialCostAudit friendlyCost;
        SequentialCostAudit enemyCost;
        MasterAssignmentProfile profile;
        const bool packingCost = config.value(PACKING_COST_AUDIT_KEY).toBool();
        if (packingCost)
        {
            if (!config.value("runSearch").toBool() || config.value(WARM_START_AUDIT_KEY).toBool() ||
                config.value(READINESS_AUDIT_KEY).toBool())
            {
                qWarning() << "Packing cost audit requires an ordinary search probe.";
                return;
            }
            friendlyCost.countOperations = config.value(PACKING_COST_OPERATIONS_KEY).toBool();
            enemyCost.countOperations = friendlyCost.countOperations;
            profile.friendlySequentialCost = &friendlyCost;
            profile.enemySequentialCost = &enemyCost;
        }
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
        const auto auditMode = packingCost || (config.value(WARM_START_AUDIT_KEY).toBool() &&
            config.value(WARM_START_COUNTERS_KEY).toBool())
            ? SequentialTierAuditMode::CountersOnly : SequentialTierAuditMode::Full;
        const auto setup = stock.prepare(field, input, ledger, &profile, auditMode);
        if (setup != MasterStockSetupStatus::Prepared)
        {
            writeRecord(outputPath, {{"stockSetup", static_cast<int>(setup)}, {"work", masterWorkRecord(ledger.stats())}});
            return;
        }
        MasterAssignmentEvaluator evaluator(problem, ledger, *stock.session(), stock.tariffs(), &profile);
        if (config.value(WARM_START_AUDIT_KEY).toBool())
        {
            WarmStartAudit audit(stock, evaluator, ledger, profile, problem, input, field, auditMode);
            writeRecord(outputPath, {{WARM_START_AUDIT_KEY, audit.run(incoming, *challenger)}});
            return;
        }
        if (config.value(READINESS_AUDIT_KEY).toBool())
        {
            CertificateReadinessAudit audit(evaluator, *stock.session(), ledger, profile);
            writeRecord(outputPath, {{READINESS_AUDIT_KEY, audit.run(incoming, *challenger)}});
            return;
        }
        if (config.value("runSearch").toBool())
        {
            const auto eventsPath = outputPath + ".events.jsonl";
            if (QFile::exists(eventsPath))
            {
                qWarning() << "Master search event path already exists:" << eventsPath;
                return;
            }
            SearchObserver observer(eventsPath, evaluator, ledger, stock.tariffs());
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = schedule, .retainDiagnostics = true, .profile = &profile, .observer = &observer});
            QJsonObject record{{"normalSearch", true}, {"nominationInjected", false},
                {"eventsComplete", observer.complete()}, {"search", masterSearchRecord(result)},
                {"profile", masterProfileRecord(profile)}, {"elapsedNanos", QString::number(timer.nsecsElapsed())}};
            if (packingCost)
            {
                record.insert(PACKING_COST_AUDIT_KEY, QJsonObject{
                    {"diagnosticOnly", true}, {"tierAudit", static_cast<int>(auditMode)},
                    {"friendly", sequentialCostRecord(friendlyCost)}, {"enemy", sequentialCostRecord(enemyCost)}});
            }
            writeRecord(outputPath, record);
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
            operation.insert(CERTIFICATE_PREPARATION_KEY, masterCertificatePreparationRecord(evaluator.certificatePreparation()));
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
