#include <array>
#include <functional>
#include <map>

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentinstallation.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masternativesource.h"
#include "ai/coordinator/masterplanpricingadapter.h"
#include "tests/coordinator_ai/masterassignmentstockfixture.h"
#include "tests/coordinator_ai/masterassignmenttestexport.h"
#include "tests/coordinator_ai/masterstockimport.h"

void testMasterStockReference();

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;

    class RefinementAllowanceObserver final : public MasterAssignmentSearchObserver
    {
    public:
        explicit RefinementAllowanceObserver(const MasterAssignmentSchedule & schedule)
            : m_schedule(schedule)
        {
        }

        void evaluated(const MasterAssignmentEvaluation &, std::int64_t) override
        {
        }

        void compared(const MasterAssignmentEvaluation &, const RelativeStockComparison &,
                      std::int64_t) override
        {
        }

        void refined(const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
                     std::int64_t allowance, MasterRefinementStatus status,
                     std::int64_t workBefore, std::int64_t workAfter) override
        {
            if (!firstRefinementWorkBefore)
            {
                firstRefinementWorkBefore = workBefore;
            }
            if (!firstCancelledWork && term == MasterRefinementTerm::Enemy &&
                status == MasterRefinementStatus::Unavailable && evaluation.quote() &&
                !evaluation.quote()->enemy.exact())
            {
                firstCancelledWork = workBefore;
                firstCancelledEvent = refinementEvents;
            }
            ++refinementEvents;
            const auto initial = term == MasterRefinementTerm::FutureDual
                ? m_schedule.futureDualAllowance : m_schedule.continuationSlice;
            auto [entry, inserted] = m_allowances.try_emplace(
                std::pair{evaluation.state.choices, term}, Allowance{initial});
            auto & expected = entry->second;
            expect(!expected.exhausted, "overflowed continuation growth cannot schedule another attempt");
            expect(allowance == expected.next, "skipped refinement cannot inflate its next native allowance");
            if (status == MasterRefinementStatus::Updated)
            {
                resumedEnemy += expected.skipped && term == MasterRefinementTerm::Enemy;
                expected.skipped = false;
                const auto growth = m_schedule.refinementGrowthFactor;
                expected.exhausted = allowance > std::numeric_limits<std::int64_t>::max() / growth;
                overflowedGrowth += expected.exhausted;
                if (!expected.exhausted)
                {
                    expected.next = allowance * growth;
                }
            }
            else if (status == MasterRefinementStatus::Unavailable)
            {
                expect(workBefore == workAfter, "unavailable refinement does not consume a native slice");
                expected.skipped = true;
                ++skipped;
            }
        }

        std::uint64_t skipped{0};
        std::uint64_t resumedEnemy{0};
        std::optional<std::int64_t> firstCancelledWork;
        std::uint64_t firstCancelledEvent{0};
        std::uint64_t refinementEvents{0};
        std::uint64_t overflowedGrowth{0};
        std::optional<std::int64_t> firstRefinementWorkBefore;

    private:
        struct Allowance
        {
            std::int64_t next;
            bool skipped{false};
            bool exhausted{false};
        };

        MasterAssignmentSchedule m_schedule;
        std::map<std::pair<std::vector<std::int32_t>, MasterRefinementTerm>, Allowance> m_allowances;
    };

    void partialStockBounds(const MasterAssignmentProblem & problem,
                            const MasterAssignmentOracle::Request & reference,
                            NativeStockSession & session, RefinementLedger & ledger)
    {
        MasterAssignmentBounds bounds(problem);
        require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                "complete future model binds to master bound basis");
        auto root = masterRootState(problem, ledger);
        require(root.has_value(), "stock partial root");
        for (const auto & full : MasterAssignmentOracle::enumerate(problem, reference).tuples)
        {
            auto state = *root;
            for (std::size_t depth = 0; depth <= problem.actors().size(); ++depth)
            {
                auto restricted = reference;
                restricted.domain = MasterAssignmentOracle::fullDomain(problem);
                restricted.restriction = QStringLiteral("stock partial-state completion domain");
                for (std::size_t actor = 0; actor < depth; ++actor)
                {
                    state.choices[actor] = full.choices[actor];
                    restricted.domain[actor] = {full.choices[actor]};
                }
                const auto exact = MasterAssignmentOracle::enumerate(problem, restricted);
                for (const auto family : {MasterBoundFamily::Immediate, MasterBoundFamily::CurrentResource,
                                          MasterBoundFamily::EndpointCoupled, MasterBoundFamily::CombinedCurrentFuture})
                {
                    const auto bound = bounds.bound(state, family, ledger);
                    require(bound.valid(), "partial stock upper available");
                    const auto verified = bounds.verify(*bound.certificate, ledger);
                    require(verified.valid(), "full partial-stock certificate verification");
                    const auto tight = bounds.tighten(*bound.certificate, 8, ledger);
                    require(tight.valid(), "joint current/future prices verify");
                    expect(!exact.exactOptimum || (bound.certificate->upper >= *exact.exactOptimum &&
                           tight.certificate->upper >= *exact.exactOptimum),
                           "all priced partial-state uppers cover exact restricted objective");
                }
            }
        }
        auto truncated = *session.futureCatalog();
        truncated.complete = false;
        MasterAssignmentBounds incomplete(problem);
        require(incomplete.prepare(session.pricingModel(), ledger, &truncated) == MasterBoundStatus::Valid,
                "truncated future input preserves other complete relaxations");
        expect(!incomplete.bound(*root, MasterBoundFamily::CombinedCurrentFuture, ledger).valid() &&
               incomplete.bound(*root, MasterBoundFamily::EndpointCoupled, ledger).valid(),
               "incomplete future domain cannot issue a full-domain node certificate");
    }

    void nativeStockSearch()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor, 1}, -3 - actor, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto problem = fixture.extract();
        auto reference = request();
        reference.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        const auto tuples = MasterAssignmentOracle::enumerate(problem, request());
        for (const auto & tuple : tuples.tuples)
        {
            reference.continuation.cases.push_back(continuationCase(tuple.reference.key));
        }
        const auto exact = MasterAssignmentOracle::enumerate(problem, reference);
        require(exact.exactOptimum.has_value(), "native stock has independent exact objective");
        PropertyStockField field;
        PropertyStockInstance enemy;
        {
            RefinementLedger ledger;
            require(ledger.open(100000000), "partial stock ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            partialStockBounds(problem, reference, session, ledger);
        }
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "incoming contender refinement ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = {.nodesPerRefinement = 1}});
            expect(session.firstFriendlyRefinement == problem.incumbentContinuationKey() &&
                   session.distinctQuotesBeforeFirstRefinement == 1,
                   "incoming uncertainty is refined before any challenger tuple is evaluated");
            expect(result.incumbent && result.incumbent->lowerReplayed &&
                   result.incumbent->objective->lower <= *exact.exactOptimum &&
                   result.stats.proofFailures == 0, "incoming refinement retains its verified witness");
            expect(result.stats.incumbentRefinements >= 2,
                   "unresolved incoming continuation receives repeated refinement despite challenger leaves");
        }
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "future cache ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentBounds bounds(problem);
            require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                    "future cache binds a complete immutable model");
            const auto root = masterRootState(problem, ledger);
            require(root.has_value(), "future cache root");
            const auto initial = bounds.bound(*root, MasterBoundFamily::CombinedCurrentFuture, ledger);
            require(initial.valid(), "future cache starts with a valid full-domain bound");
            const auto builds = bounds.futureCacheStats().recomputations;
            expect(bounds.verify(*initial.certificate, ledger).valid() &&
                   bounds.verify(*initial.certificate, ledger).valid() &&
                   bounds.futureCacheStats().recomputations == builds && bounds.futureCacheStats().hits >= 2,
                   "identical future prices reuse only the completed recurrence while verifying actor alternatives");
            auto invalid = *initial.certificate;
            invalid.prices.back() = -1;
            const auto beforeInvalid = bounds.futureCacheStats().recomputations;
            expect(!bounds.verify(invalid, ledger).valid() && !bounds.verify(invalid, ledger).valid() &&
                   bounds.futureCacheStats().recomputations == beforeInvalid + 2,
                   "invalid future-price results are never cached");
            expect(bounds.verify(*initial.certificate, ledger).valid(),
                   "failed cache refresh cannot replace a previous verified pricing result");
        }
        std::optional<MilliFunds> priorLower;
        std::optional<MilliFunds> priorUpper;
        QJsonArray searches;
        for (const std::int64_t allowance : {0, 300, 1000, 5000, 12000, 30000, 160000, 1000000})
        {
            RefinementLedger ledger;
            require(ledger.open(allowance), "native search ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
            searches.append(searchRecord(result));
            expect(!result.lower || *result.lower <= *exact.exactOptimum, "stock lower <= exhaustive objective");
            expect(!result.upper || *result.upper >= *exact.exactOptimum, "stock upper >= exhaustive objective");
            expect(!result.globalOptimal || *result.lower == *exact.exactOptimum, "stock exact search value");
            expect(result.stats.proofFailures == 0, "native stock search has no unexpected proof failure");
            expect(!priorLower || (result.lower && *result.lower >= *priorLower), "stock lower prefix monotonic");
            expect(!priorUpper || (result.upper && *result.upper <= *priorUpper), "stock upper prefix monotonic");
            priorLower = result.lower;
            priorUpper = result.upper;
            if (result.improvement)
            {
                const auto & proof = *result.improvement;
                const auto challenger = MasterAssignmentOracle::evaluateObjective(
                    MasterAssignmentOracle::constructReference(problem, proof.challenger.state.choices),
                    reference.continuation, 1000000);
                const auto incumbent = MasterAssignmentOracle::evaluateObjective(
                    MasterAssignmentOracle::constructReference(problem, proof.incumbent.state.choices),
                    reference.continuation, 1000000);
                expect(challenger.total > incumbent.total && proof.comparison.deltaLower > 0 &&
                       proof.comparison.deltaLower <= challenger.total - incumbent.total,
                       "formal common-enemy proof agrees with absolute exact objectives");
                expect(proof.incumbent.proof && proof.challenger.proof &&
                       session.replayLowerProof(*proof.incumbent.proof) &&
                       session.replayLowerProof(*proof.challenger.proof),
                       "returned strict proof owns replayable witnesses despite subsequent search work");
            }
            if (result.best)
            {
                expect(result.best->proof && session.replayLowerProof(*result.best->proof),
                       "returned best lower retains its own replayable proof");
            }
            for (const auto & region : result.residual)
            {
                if (region.completeTuple && region.upper)
                {
                    const auto objective = MasterAssignmentOracle::evaluateObjective(
                        MasterAssignmentOracle::constructReference(problem, region.state.choices),
                        reference.continuation, 1000000);
                    expect(*region.upper >= objective.total, "unresolved leaf retained with a sound upper");
                }
            }
            if (allowance == 1000000)
            {
                expect(result.globalOptimal && result.stats.refinements > 0, "native continuation search converges");
            }
        }
        exportSearchCase(QStringLiteral("master-native-stock-search"), problem, reference, exact, searches);
        for (const std::uint64_t burst : {1, 2, 24})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "challenger refinement burst ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = {.challengerRefinementBurst = burst}, .retainDiagnostics = true});
            expect(result.globalOptimal && result.lower == exact.exactOptimum &&
                   result.stats.proofFailures == 0 && result.stats.work.spent() <= 1000000,
                   "bounded challenger bursts preserve the independently exact objective and single budget");
            exportSearchCase(QStringLiteral("master-challenger-burst-%1").arg(burst), problem, reference, exact,
                             {searchRecord(result)});
        }
        std::uint64_t resumedEnemy = 0;
        std::uint64_t balancedSelections = 0;
        for (const bool deferFriendly : {false, true})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "per-term refinement allowance ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger,
                                       QuoteFault::None, true, deferFriendly);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const MasterAssignmentSchedule schedule{
                .continuationSlice = 1, .refinementGrowthFactor = 2,
                .futureDualAllowance = 1, .nodesPerRefinement = 1};
            RefinementAllowanceObserver observer(schedule);
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = schedule, .retainDiagnostics = true, .observer = &observer});
            expect(observer.skipped > 0, "per-term allowance fixtures exercise unavailable refinement");
            balancedSelections += result.stats.workBalancedSelections;
            resumedEnemy += observer.resumedEnemy;
            expect(result.lower && *result.lower <= *exact.exactOptimum &&
                       result.upper && *result.upper >= *exact.exactOptimum &&
                       result.stats.proofFailures == 0 && result.stats.work.spent() <= 1000000,
                   "independent term allowances preserve oracle bounds, proofs and one ledger");
            if (!deferFriendly)
            {
                expect(result.globalOptimal && result.lower == exact.exactOptimum,
                       "resolved relative focus must release enemy refinement to close the small native domain");
                require(observer.firstRefinementWorkBefore.has_value(), "first native refinement was observed");
                constexpr std::int64_t refreshQuoteWork = 32;
                constexpr std::int64_t accountingWork = 4;
                const auto accountingPrefix = *observer.firstRefinementWorkBefore -
                                              refreshQuoteWork - accountingWork;
                for (const auto remainder : {std::int64_t{0}, accountingWork - 1, accountingWork})
                {
                    RefinementLedger prefixLedger;
                    require(prefixLedger.open(accountingPrefix + remainder), "accounting-prefix ledger");
                    NativeStockSession prefixSession(field, enemy, reference.continuation.cases, prefixLedger);
                    MasterAssignmentEvaluator prefixEvaluator(
                        problem, prefixLedger, prefixSession, {.quote = 32, .witness = 32});
                    RefinementAllowanceObserver prefixObserver(schedule);
                    const auto prefix = searchMasterAssignment(problem, prefixEvaluator, prefixLedger,
                        {.schedule = schedule, .retainDiagnostics = true, .observer = &prefixObserver});
                    require(prefixLedger.lastRefusal().has_value(), "accounting prefix reaches a refusal");
                    const bool accountingPaid = remainder == accountingWork;
                    expect(prefix.budgetExhausted && !prefix.refinementRefusal &&
                               prefixObserver.refinementEvents == 0 && prefix.stats.refinements == 0 &&
                               prefixLedger.lastRefusal()->requested ==
                                   (accountingPaid ? refreshQuoteWork : accountingWork) &&
                               prefixLedger.lastRefusal()->work ==
                                   (accountingPaid ? RefinementWork::MasterValuation : RefinementWork::MasterExpansion) &&
                               prefixLedger.balance() == (accountingPaid ? 0 : remainder),
                           "accounting is prepaid exactly before selection and native refresh");
                    expect(!prefix.globalOptimal && prefix.incumbent && prefix.incumbent->proof &&
                               prefixSession.replayLowerProof(*prefix.incumbent->proof) &&
                               prefix.upper && *prefix.upper >= *exact.exactOptimum,
                           "accounting-prefix refusal retains the owned incumbent and oracle upper");
                }
                require(observer.firstCancelledWork.has_value(), "native fixture reaches accounted enemy cancellation");
                constexpr std::int64_t eligibilityWork = 32 + 4 * 6;
                for (const auto remaining : {std::int64_t{0}, eligibilityWork - 1})
                {
                    const auto limit = *observer.firstCancelledWork - eligibilityWork + remaining;
                    RefinementLedger interruptedLedger;
                    require(interruptedLedger.open(limit), "cancellation eligibility prefix ledger");
                    NativeStockSession interruptedSession(field, enemy, reference.continuation.cases,
                                                           interruptedLedger);
                    MasterAssignmentEvaluator interruptedEvaluator(
                        problem, interruptedLedger, interruptedSession, {.quote = 32, .witness = 32});
                    RefinementAllowanceObserver interruptedObserver(schedule);
                    const auto interrupted = searchMasterAssignment(
                        problem, interruptedEvaluator, interruptedLedger,
                        {.schedule = schedule, .retainDiagnostics = true, .observer = &interruptedObserver});
                    expect(interrupted.budgetExhausted && interruptedLedger.lastRefusal() &&
                               interruptedLedger.lastRefusal()->work == RefinementWork::MasterValuation &&
                               interruptedLedger.lastRefusal()->requested == eligibilityWork &&
                               interruptedLedger.balance() == remaining && !interrupted.refinementRefusal &&
                               interruptedObserver.refinementEvents == observer.firstCancelledEvent,
                           "unpaid cancellation eligibility stops before any native refinement event");
                    for (const auto & tuple : exact.tuples)
                    {
                        if (interrupted.lower && tuple.objective.total <= *interrupted.lower)
                        {
                            continue;
                        }
                        const auto covered = std::any_of(interrupted.residual.begin(), interrupted.residual.end(),
                            [&](const auto & region)
                            {
                                return region.state.choices.size() == tuple.choices.size() &&
                                    std::equal(region.state.choices.begin(), region.state.choices.end(),
                                               tuple.choices.begin(), [](auto selected, auto choice)
                                               {
                                                   return selected == MASTER_UNASSIGNED || selected == choice;
                                               }) &&
                                    (!region.upper || *region.upper >= tuple.objective.total);
                            });
                        expect(covered, "unpaid cancellation leaves every better oracle tuple covered");
                    }
                }
            }
            auto record = searchRecord(result);
            record.insert("unavailableRefinements", QString::number(observer.skipped));
            record.insert("resumedEnemyRefinements", QString::number(observer.resumedEnemy));
            exportSearchCase(QStringLiteral("master-term-allowance-%1").arg(deferFriendly),
                             problem, reference, exact, {record});
        }
        expect(resumedEnemy > 0, "native enemy refinement resumes after skipped terms on the same complete plan");
        expect(balancedSelections > 0, "native search exercises charged-work balancing");
        for (const auto growth : {std::int64_t{2}, std::int64_t{4},
                                  std::numeric_limits<std::int64_t>::max()})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "native growth boundary ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const MasterAssignmentSchedule schedule{
                .continuationSlice = 2, .refinementGrowthFactor = growth, .nodesPerRefinement = 1};
            RefinementAllowanceObserver observer(schedule);
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = schedule, .retainDiagnostics = true, .observer = &observer});
            expect(result.lower && *result.lower <= *exact.exactOptimum &&
                       result.upper && *result.upper >= *exact.exactOptimum &&
                       result.stats.proofFailures == 0 && result.stats.work.spent() <= 1000000,
                   "continuation growth and overflow preserve certified bounds and one work allowance");
            if (result.best)
            {
                expect(result.best->proof && session.replayLowerProof(*result.best->proof),
                       "growth boundary retains an independently replayable lower witness");
            }
            if (growth == std::numeric_limits<std::int64_t>::max())
            {
                expect(observer.overflowedGrowth > 0,
                       "the extreme growth case reaches the guarded multiplication");
            }
            exportSearchCase(QStringLiteral("master-native-growth-%1").arg(growth),
                             problem, reference, exact, {searchRecord(result)});
        }
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "certificate integration ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger, QuoteFault::None, true, true);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = {.refinementGrowthFactor = 2, .futureDualAllowance = 1},
                 .retainDiagnostics = true});
            expect(session.certificateCalls > 0 && session.verifiedCertificates > 0 &&
                   session.maximumCertificateAllowance > 1 &&
                   result.stats.work.grantedByWork[static_cast<std::size_t>(RefinementWork::PackingDualCertificate)] > 0,
                   "growing certificate slices reach a verified full catalog after the initial slice is too small");
            expect(result.stats.cancelledEnemyRefinements > 0,
                   "a shared enemy term is deferred while a complete challenger comparison is in focus");
            expect(result.lower && *result.lower <= *exact.exactOptimum &&
                   result.upper && *result.upper >= *exact.exactOptimum,
                   "native future-certificate search remains inside oracle bounds");
            exportSearchCase(QStringLiteral("master-native-node-certificates"), problem, reference, exact,
                             {searchRecord(result)});
        }
        {
            RefinementLedger ledger;
            require(ledger.open(100000), "stale certificate ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            const auto stale = session.quote(problem.incumbentContinuationKey(), problem.incumbentEconomicTotal());
            const auto tightened = session.tightenOurUpperWithVerifiedDual(stale, 4096);
            require(tightened.certificateValid, "stale certificate control produces a replacement certificate");
            expect(!session.compare(stale, stale).valid(), "previous certificate identity cannot authorize a fresh comparison");
            const auto recorded = session.captureProof(session.quote(
                problem.incumbentContinuationKey(), problem.incumbentEconomicTotal()));
            require(recorded && recorded->ourUpperCertificate, "owned receipt retains its verified upper certificate");
            const auto replaced = session.tightenOurUpperWithVerifiedDual(recorded->quote, 4096);
            require(replaced.certificateValid, "certificate registry advances");
            expect(recorded->ourUpperCertificate != replaced.certificate && session.replayLowerProof(*recorded) &&
                   !session.compare(recorded->quote, recorded->quote).valid(),
                   "old receipt owns its proof while stale registry authorization is rejected");
        }
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "completed certificate scheduling ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger,
                                       QuoteFault::None, true, true);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            auto evaluated = evaluator.evaluate({problem.identity(),
                std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
            require(evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 4096) ==
                    MasterRefinementStatus::Updated && evaluated.quote()->ourUpperCertificateId != 0,
                    "open friendly interval obtains a registered upper certificate");
            const auto certificateCalls = session.certificateCalls;
            const auto before = ledger.stats().spent();
            const auto proof = evaluated.proof;
            expect(evaluator.refine(evaluated, MasterRefinementTerm::FutureDual,
                                   std::numeric_limits<std::int64_t>::max()) == MasterRefinementStatus::Unavailable &&
                   session.certificateCalls == certificateCalls && ledger.stats().spent() == before &&
                   evaluated.proof == proof,
                   "already certified upper neither reserves an oversized slice nor repeats certificate work");
        }
        {
            RefinementLedger ledger;
            require(ledger.open(100000), "owned witness ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            auto evaluated = evaluator.evaluate({problem.identity(),
                std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
            require(evaluated.proof != nullptr, "initial evaluation owns lower proof");
            const auto recorded = evaluated.proof;
            const auto captures = session.proofCaptures;
            const auto witnessWork = ledger.stats().grantedByWork[
                static_cast<std::size_t>(RefinementWork::MasterWitness)];
            expect(evaluator.refresh(evaluated) && evaluator.refresh(evaluated) &&
                   evaluated.proof == recorded && session.proofCaptures == captures &&
                   ledger.stats().grantedByWork[static_cast<std::size_t>(RefinementWork::MasterWitness)] == witnessWork,
                   "an unchanged authenticated quote reuses its owned replayed proof without another witness charge");
            require(evaluator.refine(evaluated, MasterRefinementTerm::Friendly, 4096) == MasterRefinementStatus::Updated,
                    "mutable session replaces friendly continuation witness");
            const auto beforeOpenEnemy = ledger.stats().spent();
            expect(evaluated.quote()->ours.exact() && !evaluated.quote()->enemy.exact() &&
                   evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 1000000) ==
                       MasterRefinementStatus::Unavailable && ledger.stats().spent() == beforeOpenEnemy,
                   "open enemy interval does not justify certifying an already exact friendly interval");
            require(evaluator.refine(evaluated, MasterRefinementTerm::Enemy, 4096) == MasterRefinementStatus::Updated,
                    "mutable session replaces enemy continuation witness");
            expect(!sameRelativeStockQuote(recorded->quote, *evaluated.quote()) &&
                   evaluated.proof != recorded && session.proofCaptures > captures &&
                   session.replayLowerProof(*recorded), "changed quote replaces proof while the prior witness remains replayable");
            const auto beforeExact = ledger.stats().spent();
            expect(evaluated.exact &&
                   evaluator.refine(evaluated, MasterRefinementTerm::Friendly, 1000000) == MasterRefinementStatus::Unavailable &&
                   evaluator.refine(evaluated, MasterRefinementTerm::Enemy, 1000000) == MasterRefinementStatus::Unavailable &&
                   evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 1000000) == MasterRefinementStatus::Unavailable &&
                   ledger.stats().spent() == beforeExact,
                   "exact continuation terms need no larger rerun or budget reservation");
            const auto beforeCertificate = evaluated.proof;
            require(session.tightenOurUpperWithVerifiedDual(*evaluated.quote(), 4096).certificateValid &&
                    evaluator.refresh(evaluated), "an externally changed certificate refreshes an evaluated proof");
            expect(evaluated.proof != beforeCertificate && evaluated.quote()->ourUpperCertificateId != 0,
                   "a changed certificate identity cannot reuse the old authenticated proof");
            auto corrupted = *recorded;
            ++corrupted.quote.ours.lower;
            expect(!session.replayLowerProof(corrupted), "owned witness does not certify a changed lower value");
            NativeStockSession different(field, enemy, reference.continuation.cases, ledger);
            expect(!different.replayLowerProof(*recorded),
                   "owned receipt cannot cross into an equal-content fresh session");
            auto temporary = std::make_unique<NativeStockSession>(
                field, enemy, reference.continuation.cases, ledger);
            const auto expired = temporary->captureProof(temporary->quote(
                problem.incumbentContinuationKey(), problem.incumbentEconomicTotal()));
            require(expired && expired->quote.identity.established(), "temporary owned proof has a live identity");
            temporary.reset();
            expect(!expired->quote.identity.established() && !session.replayLowerProof(*expired),
                   "owned receipt does not keep an expired session identity alive");
        }
        for (const auto fault : {QuoteFault::ForeignIdentity, QuoteFault::MissingWitness, QuoteFault::MissingEnemyWitness,
                                QuoteFault::UnregisteredCertificate})
        {
            RefinementLedger ledger;
            require(ledger.open(160000), "bad quote ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger, fault);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger);
            expect(!result.lower && !result.globalOptimal && !result.improvement && !result.noFeasibleAssignment,
                   "bad identities and certificates cannot establish a feasible bound or remove unresolved leaves");
            expect(result.stats.proofFailures > 0, "rejected quote proofs are visible in the search result");
            expect(result.upper && *result.upper >= *exact.exactOptimum, "independent relaxation survives bad quotes");
        }
        RefinementLedger ledger;
        require(ledger.open(160000), "unsupported refinement ledger");
        NativeStockSession session(field, enemy, reference.continuation.cases, ledger, QuoteFault::None, false);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
        const auto unresolved = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        expect(unresolved.stats.residualRegions > 0 && !unresolved.globalOptimal && !unresolved.noFeasibleAssignment,
               "empty expansion queue cannot hide unresolved complete stock leaves");
        expect(unresolved.upper && *unresolved.upper >= *exact.exactOptimum, "unsupported refinement remains covered");
    }

    void futurePricingBoundaries()
    {
        const auto input = continuationCase({
            CanonicalPlanActionKey::fromAction(0, 0, 0, false),
            CanonicalPlanActionKey::fromAction(1, 1, 0, false),
            CanonicalPlanActionKey::fromAction(2, 2, 0, false)}).friendly;
        NativeSequentialModel native(input);
        MasterFutureCatalog catalog{input.horizon, input.columns, native.classes, input.rows, {}, true};
        const auto exact = MasterAssignmentOracle::exhaustSequential(input, 1000000);
        require(exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact, "future-bound exact oracle");
        for (const auto & prices : std::vector<std::vector<MilliFunds>>{{0, 0, 0}, {20, 30, 40}, {100, 0, 0}, {0, 200, 0}})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "future price ledger");
            const auto priced = priceMasterFuture(catalog, prices, ledger, RefinementWork::MasterDualVerification);
            require(priced.status == MasterFutureStatus::Valid, "full future topology priced");
            MilliFunds upper = 0;
            for (const auto price : prices)
            {
                upper += price;
            }
            for (const auto & row : priced.dayRows)
            {
                upper += row.upper;
            }
            expect(upper >= exact.value, "future relaxation covers independent itinerary packing optimum");
        }
        RefinementLedger ledger;
        require(ledger.open(1000000), "malformed future ledger");
        expect(priceMasterFuture(catalog, std::array<MilliFunds, 3>{-1, 0, 0}, ledger, RefinementWork::MasterDualVerification).status ==
                   MasterFutureStatus::Invalid, "negative future node prices rejected");
        catalog.dayRows[0].weight[0] = std::numeric_limits<MilliFunds>::min();
        expect(priceMasterFuture(catalog, std::array<MilliFunds, 3>{1, 0, 0}, ledger, RefinementWork::MasterDualVerification).status ==
                   MasterFutureStatus::ArithmeticFailure, "future row arithmetic fails closed");
        for (std::int64_t allowance = 0; allowance < 90; ++allowance)
        {
            RefinementLedger limited;
            require(limited.open(allowance), "limited future verifier");
            const auto result = priceMasterFuture(catalog, std::array<MilliFunds, 3>{0, 0, 0}, limited, RefinementWork::MasterDualVerification);
            expect(result.status == MasterFutureStatus::BudgetExhausted && limited.stats().spent() <= allowance,
                   "interrupted recurrence never yields a verified full-domain bound");
        }
        SequentialClassTable transition;
        transition.build(input.columns, std::array<std::int32_t, 9>{0, std::numeric_limits<std::int32_t>::max(), 1,
            std::numeric_limits<std::int32_t>::max(), 0, 1, 1, 1, 0}, 2, input.horizon);
        expect(transition.continuationOwnedTurn(0, 2, 1) == NO_CAPTURE_TURNS,
               "oversized transition cannot overflow into the planning horizon");
        transition.build(input.columns, {}, 2, input.horizon);
        catalog.classes[0] = transition;
        expect(priceMasterFuture(catalog, std::array<MilliFunds, 3>{0, 0, 0}, ledger,
                                 RefinementWork::MasterDualVerification).status == MasterFutureStatus::Invalid,
               "incomplete class topology is rejected before transition traversal");
    }

    void warmStartOracleBoundaries()
    {
        const auto source = continuationCase({
            CanonicalPlanActionKey::fromAction(0, 0, 0, false),
            CanonicalPlanActionKey::fromAction(1, 1, 0, false),
            CanonicalPlanActionKey::fromAction(2, 2, 0, false)}).friendly;
        for (std::int32_t variant = 0; variant < 8; ++variant)
        {
            auto input = source;
            if ((variant & 1) != 0)
            {
                input.rows[0].ownedTurn[0] = 1;
                input.rows[0].weight[0] = columnStreamValue(input.columns[0], 1, input.horizon);
            }
            if ((variant & 2) != 0)
            {
                input.rows[1].ownedTurn[1] = NO_CAPTURE_TURNS;
                input.rows[1].weight[1] = 17;
            }
            if ((variant & 4) != 0)
            {
                input.capturedNodes[2] = true;
                for (auto & row : input.rows)
                {
                    row.ownedTurn[2] = NO_CAPTURE_TURNS;
                    row.weight[2] = 0;
                }
            }
            std::vector<MilliFunds> weights;
            for (const auto & row : input.rows)
            {
                weights.insert(weights.end(), row.weight.begin(), row.weight.end());
            }
            AssignmentSolver assignment;
            assignment.solve(static_cast<std::int32_t>(input.rows.size()),
                             static_cast<std::int32_t>(input.columns.size()), weights);
            const auto floor = assignment.optimum();
            const auto oracle = MasterAssignmentOracle::exhaustSequential(input, 1000000);
            require(oracle.status == MasterAssignmentOracle::ObjectiveStatus::Exact,
                    "warm-start reference uses the independent complete itinerary oracle");
            NativeSequentialModel native(input);
            const auto instance = native.instance();
            const auto complete = solveSequentialPacking(instance, floor, 100000, nullptr, true,
                SequentialWitnessPolicy::BookedValue, SequentialTierAuditMode::CountersOnly);
            require((complete.certificate || complete.searchCompleted) && complete.bookedValue == oracle.value &&
                        replaySequentialWitness(instance, complete.witness, complete.bookedValue),
                    "native warm seed matches the independent optimum and replays");
            const auto compatible = solveSequentialPacking(instance, floor, 100000, nullptr, true,
                SequentialWitnessPolicy::BookedValue, SequentialTierAuditMode::CountersOnly, nullptr, nullptr,
                SequentialPackingPolicy::CompatibleRows);
            require((compatible.certificate || compatible.searchCompleted) && compatible.bookedValue == oracle.value &&
                        replaySequentialWitness(instance, compatible.witness, compatible.bookedValue),
                    "compatible-row pruning matches the independent complete itinerary oracle");
            const SequentialWarmStart seed{complete.witness, complete.bookedValue};
            for (std::int64_t cap = 0; cap <= compatible.searchStates + 1; ++cap)
            {
                for (const auto* warm : {static_cast<const SequentialWarmStart*>(nullptr), &seed})
                {
                    const auto result = solveSequentialPacking(instance, floor, cap, nullptr, true,
                        SequentialWitnessPolicy::BookedValue, SequentialTierAuditMode::CountersOnly, warm, nullptr,
                        SequentialPackingPolicy::CompatibleRows);
                    expect(result.searchStates <= cap && result.bookedValue <= oracle.value &&
                               result.relaxedValue >= oracle.value &&
                               (!result.searchCompleted || result.bookedValue == oracle.value),
                           "every charged compatible-bound interruption brackets the independent optimum");
                    expect((result.witness.empty() && !result.searchCompleted && !warm) ||
                               replaySequentialWitness(instance, result.witness, result.bookedValue),
                           "compatible pruning preserves owned cold and warm witness replay");
                }
            }
            for (std::int64_t cap = 0; cap <= complete.searchStates + 1; ++cap)
            {
                const auto result = solveSequentialPacking(instance, floor, cap, nullptr, true,
                    SequentialWitnessPolicy::BookedValue, SequentialTierAuditMode::CountersOnly, &seed);
                expect(result.warmStartAccepted && result.searchStates <= cap &&
                           result.floorValue == floor && result.bookedValue == oracle.value &&
                           result.relaxedValue == complete.relaxedValue && result.relaxedValue >= oracle.value &&
                           replaySequentialWitness(instance, result.witness, result.bookedValue),
                       "warm pruning and every interruption remain sound against the independent oracle");
                expect(!result.certificate || result.bookedValue == result.relaxedValue,
                       "a retained feasible seed is not an independent upper certificate");
            }
        }
    }

    void signedAndWideFutureDomains()
    {
        SequentialInput input;
        input.horizon = 4;
        input.columns = {{.slot = 0, .tile = {0, 0}, .income = {0, 0}},
                         {.slot = 1, .tile = {1, 0}, .income = {10, 10}}};
        input.classes = {{1, {0, 1, 1, 0}}};
        input.rows = {{0, {-1, 0}, {1, NO_CAPTURE_TURNS}}};
        input.capturedNodes = {false, false};
        auto exact = MasterAssignmentOracle::exhaustSequential(input, 1000000);
        require(exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact && exact.value == 19,
                "negative first leg remains on the positive exact itinerary");
        NativeSequentialModel model(input);
        MasterFutureCatalog catalog{input.horizon, input.columns, model.classes, input.rows, {}, true};
        RefinementLedger ledger;
        require(ledger.open(1000000), "signed future ledger");
        auto priced = priceMasterFuture(catalog, std::array<MilliFunds, 2>{5, 2}, ledger,
                                        RefinementWork::MasterDualVerification);
        expect(priced.status == MasterFutureStatus::Valid && priced.dayRows[0].upper + 7 == exact.value,
               "priced signed first leg retains its profitable continuation");
        input.rows = {{0, {17, 0}, {NO_CAPTURE_TURNS, NO_CAPTURE_TURNS}}};
        exact = MasterAssignmentOracle::exhaustSequential(input, 1000000);
        require(exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact && exact.value == 17,
                "no-continuation sentinel still permits a nonzero first leg");
        model = NativeSequentialModel(input);
        catalog = {input.horizon, input.columns, model.classes, input.rows, {}, true};
        priced = priceMasterFuture(catalog, std::array<MilliFunds, 2>{5, 0}, ledger,
                                   RefinementWork::MasterDualVerification);
        expect(priced.status == MasterFutureStatus::Valid && priced.dayRows[0].upper + 5 == exact.value &&
               priced.dayRows[0].nodes == std::vector<std::int32_t>{0},
               "priced no-continuation first leg is not silently omitted");
        input.columns.clear();
        for (std::int32_t column = 0; column < 65; ++column)
        {
            input.columns.push_back({.slot = column, .tile = {column, 0}, .income = {0, 0}});
        }
        input.classes = {{NO_CAPTURE_TURNS, std::vector<std::int32_t>(65 * 65, UNREACHABLE)}};
        input.rows = {{0, std::vector<MilliFunds>(65), std::vector<std::int32_t>(65, 1)}};
        input.rows[0].weight[64] = 200;
        input.capturedNodes.assign(65, false);
        exact = MasterAssignmentOracle::exhaustSequential(input, 1000000);
        require(exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact && exact.value == 200,
                "wide future oracle includes node 64");
        model = NativeSequentialModel(input);
        catalog = {input.horizon, input.columns, model.classes, input.rows, {}, true};
        std::vector<MilliFunds> prices(65, 0);
        prices[64] = 150;
        priced = priceMasterFuture(catalog, prices, ledger, RefinementWork::MasterDualVerification);
        expect(priced.status == MasterFutureStatus::Valid && priced.dayRows[0].upper + 150 == exact.value &&
               priced.dayRows[0].nodes == std::vector<std::int32_t>{64},
               "future prices and witnesses have no 64-node truncation");
    }

    void capturedSignaturesShareEnemy()
    {
        Fixture fixture(1);
        fixture.input.unitLinks[0].tile = {0, 2};
        auto wait = moveCandidate(0, {0, 2}, {0, 2}, 0);
        auto capture = wait;
        capture.generationIndex = 1;
        capture.bundle.components.push_back(captureComponent({.income = {11, 11}, .turnsUntilOwned = 0}));
        fixture.input.actors[0].candidates = {wait, capture};
        fixture.seat();
        fixture.selections[0].stockRow = 0;
        const auto problem = fixture.extract();
        auto input = request();
        input.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            auto entry = continuationCase(tuple.reference.key);
            if (tuple.choices[0] == 0)
            {
                entry.friendly.rows[0].ownedTurn[0] = 1;
                entry.friendly.rows[0].weight[0] = columnStreamValue(entry.friendly.columns[0], 1, entry.friendly.horizon);
            }
            else
            {
                entry.capturedColumns = {0};
                entry.friendly.capturedNodes[0] = true;
                entry.friendly.rows[0].ownedTurn[0] = NO_CAPTURE_TURNS;
                entry.friendly.rows[0].weight[0] = 0;
                entry.owned += ownershipFlipSwing(entry.friendly.columns[0], entry.friendly.horizon);
            }
            input.continuation.cases.push_back(std::move(entry));
        }
        const auto exact = MasterAssignmentOracle::enumerate(problem, input);
        require(exact.exactOptimum.has_value(), "captured signatures have exact reference");
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "captured signature ledger");
        NativeStockSession session(field, enemy, input.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
        const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        expect(result.lower && *result.lower <= *exact.exactOptimum &&
               result.upper && *result.upper >= *exact.exactOptimum,
               "captured signatures preserve absolute search bounds");
        auto left = evaluator.evaluate({problem.identity(), {0}});
        auto right = evaluator.evaluate({problem.identity(), {1}});
        for (auto* value : {&left, &right})
        {
            if (!value->quote()->ours.exact())
            {
                require(evaluator.refine(*value, MasterRefinementTerm::Friendly, 4096) == MasterRefinementStatus::Updated,
                        "remaining friendly uncertainty refines");
            }
            if (!value->quote()->enemy.exact())
            {
                require(evaluator.refine(*value, MasterRefinementTerm::Enemy, 4096) == MasterRefinementStatus::Updated,
                        "remaining enemy uncertainty refines");
            }
            expect(value->exact && value->proof && session.replayLowerProof(*value->proof),
                   "captured signatures retain exact replayed witnesses");
        }
        const auto comparison = evaluator.compare(left, right);
        const auto delta = exact.tuples[1].objective.total - exact.tuples[0].objective.total;
        expect(comparison.improves() && comparison.commonEnemyCancelled &&
               comparison.deltaLower == delta && comparison.deltaUpper == delta,
               "a changed capture signature still cancels the start-of-turn enemy exactly");
        exportSearchCase(QStringLiteral("master-captured-signature"), problem, input, exact, {searchRecord(result)});
    }

    void sharedStockCertificateWithDifferentEconomics()
    {
        Fixture fixture(1);
        fixture.input.actors[0].candidates = {
            moveCandidate(0, {0, 0}, {0, 0}, -5, 0),
            moveCandidate(0, {0, 0}, {0, 0}, 12, 1)};
        fixture.seat();
        fixture.selections[0].stockRow = 0;
        const auto problem = fixture.extract();
        auto input = request();
        input.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        input.continuation.cases.push_back(continuationCase(problem.incumbentContinuationKey()));
        const auto exact = MasterAssignmentOracle::enumerate(problem, input);
        require(exact.exactOptimum.has_value(), "same-continuation economic reference");
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "shared stock-certificate ledger");
        NativeStockSession session(field, enemy, input.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
        auto incumbent = evaluator.evaluate({problem.identity(), {0}});
        if (!incumbent.quote()->ours.exact())
        {
            require(evaluator.refine(incumbent, MasterRefinementTerm::Friendly, 4096) == MasterRefinementStatus::Updated,
                    "incoming friendly interval completes");
        }
        if (!incumbent.quote()->enemy.exact())
        {
            require(evaluator.refine(incumbent, MasterRefinementTerm::Enemy, 4096) == MasterRefinementStatus::Updated,
                    "incoming enemy interval completes");
        }
        require(session.tightenOurUpperWithVerifiedDual(*incumbent.quote(), 4096).certificateValid &&
                evaluator.refresh(incumbent), "incoming stock witness and externally registered certificate");
        const auto certificate = incumbent.quote()->ourUpperCertificateId;
        auto challenger = evaluator.evaluate({problem.identity(), {1}});
        require(challenger.lowerReplayed && challenger.upperVerified && challenger.quote() != nullptr,
                "another immediate value must not invalidate the same stock continuation certificate");
        expect(certificate != 0 && challenger.quote()->ourUpperCertificateId == certificate &&
               incumbent.quote()->economic == 0 && challenger.quote()->economic == 0,
               "continuation certificates retain one stock-only economic context");
        const auto comparison = evaluator.compare(incumbent, challenger);
        expect(comparison.improves() && comparison.deltaLower == 17 && comparison.deltaUpper == 17 &&
               comparison.economicDelta == 17 && challenger.objective->lower == exact.exactOptimum,
               "unchanged seated economics enter the full objective and strict margin exactly once");
        const auto reverse = evaluator.compare(challenger, incumbent);
        expect(reverse.valid() && !reverse.improves() && reverse.deltaLower == -17 && reverse.deltaUpper == -17,
               "signed economic offsets preserve the inverse comparison");
        const auto invalidContext = session.quote(problem.incumbentContinuationKey(), 1);
        expect(!session.compare(invalidContext, invalidContext).valid(),
               "native certificate authentication still rejects a different economic context");
        const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        expect(result.globalOptimal && result.lower == exact.exactOptimum && result.stats.proofFailures == 0,
               "the complete search retains all same-continuation action alternatives");
        exportSearchCase(QStringLiteral("master-shared-stock-different-economics"), problem, input, exact,
                         {searchRecord(result)});
        require(result.improvement.has_value(), "same-continuation search retains its strict economic proof");
        MasterCaptureResult capture;
        capture.extraction.problem = problem;
        capture.extraction.error = MasterAssignmentProblemError::None;
        MasterAssignmentProfile profile;
        MasterShadowConfig config;
        DecisionTraceIdentity identity;
        const auto before = ledger.stats();
        MasterShadowObservation observation{config, identity, profile, before, ledger.stats(), ledger};
        observation.capture = &capture;
        observation.search = &result;
        observation.session = &session;
        expect(!masterShadowProofsMatch(observation), "observation inspection needs the evaluator that owns the proof");
        observation.evaluator = &evaluator;
        expect(masterShadowProofsMatch(observation) && ledger.stats() == before,
               "observation inspection includes the strict proof's seated economic difference without extra work");
        const auto & retained = *result.improvement;
        require(session.tightenOurUpperWithVerifiedDual(*retained.incumbent.quote(), 4096).certificateValid,
                "a later verified certificate supersedes the retained proof's live registry entry");
        const auto afterRefresh = ledger.stats();
        expect(!session.compare(*retained.incumbent.quote(), *retained.challenger.quote()).valid() &&
               masterShadowProofsMatch(observation) && ledger.stats() == afterRefresh,
               "the actual observation predicate accepts owned snapshots after fresh authorization becomes stale");
        auto corrupted = result;
        ++corrupted.improvement->comparison.deltaLower;
        observation.search = &corrupted;
        expect(!masterShadowProofsMatch(observation),
               "historical inspection rejects an altered retained improvement margin");
    }

    void tenStockActors()
    {
        Fixture fixture(10);
        for (std::int32_t actor = 0; actor < 2; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(moveCandidate(actor, {actor, 0}, {actor, 1}, 0, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 10; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto problem = fixture.extract();
        auto input = request();
        input.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            input.continuation.cases.push_back(continuationCase(tuple.reference.key));
        }
        const auto exact = MasterAssignmentOracle::enumerate(problem, input);
        require(exact.exactOptimum.has_value(), "ten-row stock reference is exhaustive");
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "ten-row ledger");
        NativeStockSession session(field, enemy, input.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 64, .witness = 64});
        const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        expect(result.fullDomain && result.domainAccounted && result.globalOptimal &&
               result.lower == exact.exactOptimum && result.upper == exact.exactOptimum,
               "all ten stock-coupled rows participate in the same exact search domain");
        exportSearchCase(QStringLiteral("master-ten-stock-actors"), problem, input, exact, {searchRecord(result)});
    }

    void shadowControlsAndCapture()
    {
        const auto output = QDir::tempPath() + QStringLiteral("/master-shadow-test.jsonl");
        const auto disabled = parseMasterShadowConfig({}, "invalid", {}, "invalid", "invalid");
        expect(!disabled.enabled() && disabled.error == MasterShadowConfigError::None,
               "default disabled mode ignores inactive diagnostic settings");
        const auto control = parseMasterShadowConfig("stockControl", "60000000", output, {}, {},
                                                    MasterStockControlRuntime::Arena);
        expect(control.controlsStock() && !control.enabled() && control.allowance == 60000000,
               "StockControl requires an explicit mode and allowance without enabling Shadow");
        expect(parseMasterShadowConfig("stockControl", {}, output, {}, {}, MasterStockControlRuntime::Arena).error ==
               MasterShadowConfigError::MissingExplicitAllowance,
               "StockControl cannot inherit the global initial allowance implicitly");
        expect(parseMasterShadowConfig("stockControl", "60000000", output).error ==
               MasterShadowConfigError::StockControlOutsideArena,
               "StockControl cannot activate outside the explicit isolated arena");
        constexpr std::int64_t PLAY_ALLOWANCE = 60000000;
        const auto play = playMasterStockControlConfig();
        expect(play.controlsStock() && !play.enabled() && play.runtime == MasterStockControlRuntime::Play &&
                   play.allowance == PLAY_ALLOWANCE && play.exportMode == MasterShadowExportMode::Off &&
                   play.tierAuditMode == SequentialTierAuditMode::CountersOnly &&
                   play.error == MasterShadowConfigError::None && play.outputPath.isEmpty(),
               "the play setting admits StockControl with a fixed allowance and no export");
        const auto trial = parseMasterShadowConfig("stockControl", "60000000", {}, "counters", "off",
                                                  MasterStockControlRuntime::PlayableTrial);
        expect(trial.controlsStock() && !trial.enabled() && trial.allowance == 60000000 &&
                   trial.runtime == MasterStockControlRuntime::PlayableTrial,
               "explicit playable admission uses the same stock mode and allowance");
        expect(parseMasterShadowConfig("stockControl", {}, {}, "counters", "off",
                                       MasterStockControlRuntime::PlayableTrial).error ==
                   MasterShadowConfigError::MissingExplicitAllowance,
               "playable admission cannot inherit the default allowance");
        expect(!disabled.controlsStock() && disabled.allowance == MASTER_ASSIGNMENT_INITIAL_ALLOWANCE,
               "ordinary gameplay remains disabled with the unchanged initial allowance");
        expect(parseMasterShadowConfig("unknown", "60000000", output).error ==
               MasterShadowConfigError::UnknownMode, "unknown modes fail closed");
        expect(parseMasterShadowConfig("shadow", "-1", output).error ==
               MasterShadowConfigError::InvalidAllowance, "negative work fails closed");
        expect(parseMasterShadowConfig("shadow", "9223372036854775808", output).error ==
               MasterShadowConfigError::InvalidAllowance, "overflowed work fails closed");
        expect(parseMasterShadowConfig("shadow", {}, "relative.jsonl").error ==
               MasterShadowConfigError::MissingAbsoluteOutput, "diagnostics require an explicit absolute destination");
        expect(parseMasterStaleProbe({}) == MasterStaleProbe::None, "an absent stale probe changes nothing");
        expect(parseMasterStaleProbe("battleAnimationSpeed") == MasterStaleProbe::BattleAnimationSpeed,
               "the battle speed stale probe is recognized exactly");
        expect(!parseMasterStaleProbe("BattleAnimationSpeed") && !parseMasterStaleProbe("terrain"),
               "unknown stale probes fail closed");
        const auto enabled = parseMasterShadowConfig("shadow", {}, output);
        expect(enabled.enabled() && enabled.allowance == MASTER_ASSIGNMENT_INITIAL_ALLOWANCE &&
                   enabled.tierAuditMode == SequentialTierAuditMode::Full &&
                   enabled.exportMode == MasterShadowExportMode::Full,
               "explicit shadow starts at the initial allowance with full diagnostic retention");
        const auto counters = parseMasterShadowConfig("shadow", {}, output, "counters");
        expect(counters.enabled() && counters.tierAuditMode == SequentialTierAuditMode::CountersOnly,
               "optional tier copies can be disabled independently of witness capture");
        const auto full = parseMasterShadowConfig("shadow", {}, output, "full");
        expect(full.enabled() && full.tierAuditMode == SequentialTierAuditMode::Full,
               "full diagnostic retention remains explicitly selectable");
        const auto invalidAudit = parseMasterShadowConfig("shadow", {}, output, "invalid");
        expect(!invalidAudit.enabled() && invalidAudit.error == MasterShadowConfigError::InvalidTierAudit,
               "an unknown audit retention mode fails closed");
        for (const auto tier : {QByteArray("full"), QByteArray("counters")})
        {
            const auto off = parseMasterShadowConfig("shadow", {}, {}, tier, "off");
            expect(off.enabled() && off.exportMode == MasterShadowExportMode::Off && off.outputPath.isEmpty(),
                   "export-off needs no output path and is independent of tier retention");
            const auto fullExport = parseMasterShadowConfig("shadow", {}, output, tier, "full");
            expect(fullExport.enabled() && fullExport.exportMode == MasterShadowExportMode::Full,
                   "full export remains explicitly selectable for either tier mode");
        }
        expect(parseMasterShadowConfig("shadow", {}, {}, {}, "full").error ==
                   MasterShadowConfigError::MissingAbsoluteOutput &&
                   parseMasterShadowConfig("shadow", {}, output, {}, "invalid").error ==
                       MasterShadowConfigError::InvalidExport,
               "full export requires a path and unknown export controls fail closed");
        QTemporaryDir isolatedRoot;
        require(isolatedRoot.isValid(), "isolated trial path fixture exists");
        const auto package = isolatedRoot.path() + "/package";
        const auto profile = package + "/profiles/play";
        const auto otherProfile = package + "/profiles/other";
        const auto external = isolatedRoot.path() + "/external";
        require(QDir().mkpath(profile) && QDir().mkpath(otherProfile) && QDir().mkpath(external),
                "trial path fixture creates independent profiles");
        expect(isolatedMasterTrialPath(package, profile, profile + "/"),
               "explicit existing profile within the package is isolated");
        expect(!isolatedMasterTrialPath(package, {}, profile) &&
                   !isolatedMasterTrialPath(package, "profiles/play", profile) &&
                   !isolatedMasterTrialPath(package, profile, otherProfile),
               "missing, relative or INI-redirected user paths cannot admit the trial");
        expect(!isolatedMasterTrialPath(package, package, package) &&
                   !isolatedMasterTrialPath(package, external, external) &&
                   !isolatedMasterTrialPath(package, profile + "/../../..", isolatedRoot.path()) &&
                   !isolatedMasterTrialPath(package, package + "/absent", package + "/absent"),
               "application root, escaped, external and missing profiles are rejected");

        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 10, 1));
        fixture.seat();
        AssignmentResult incumbent;
        incumbent.plan = fixture.plan;
        incumbent.executionOrder = fixture.plan.executionOrder().order;
        for (const auto & selected : fixture.selections)
        {
            incumbent.selections.push_back({
                .knowledgeUnitIndex = selected.engineUnitId - 100,
                .engineUnitId = selected.engineUnitId,
                .actionIndex = selected.actionIndex,
                .candidateIndex = selected.candidateIndex,
                .seatedValue = selected.engineUnitId - 99,
                .stockCoupled = true});
        }
        RefinementLedger inactive;
        require(inactive.open(999), "disabled sentinel ledger");
        prepareMasterShadow(disabled, fixture.input, inactive);
        expect(!fixture.input.retainSelections && inactive.stats().total == 999 && inactive.stats().spent() == 0,
               "disabled mode does not modify selection retention or the ledger");
        RefinementLedger complete;
        prepareMasterShadow(enabled, fixture.input, complete);
        require(fixture.input.retainSelections && complete.stats().spent() > 0,
                "selection metadata is prepaid before legacy assignment records it");
        const auto captured = captureMasterAssignment(fixture.input, incumbent, 91, complete);
        require(captured.extraction.valid(), "budgeted real assignment interface extracts");
        const auto required = complete.stats().spent();
        for (std::int64_t allowance = 0; allowance <= required; ++allowance)
        {
            auto config = enabled;
            config.allowance = allowance;
            auto input = fixture.input;
            input.retainSelections = false;
            RefinementLedger ledger;
            prepareMasterShadow(config, input, ledger);
            const auto result = captureMasterAssignment(input, incumbent, 91, ledger);
            expect(ledger.stats().spent() <= allowance, "capture preparation never overspends");
            expect(result.extraction.valid() || result.budgetExhausted, "interrupted capture is not a model mismatch");
            if (result.extraction.valid())
            {
                const auto replay = replayMasterAssignment(*result.extraction.problem,
                    result.extraction.problem->incumbentChoices());
                expect(replay.complete() && replay.plan.equivalentState(incumbent.plan),
                       "budgeted extraction retains the complete incumbent");
            }
        }
        expect(incumbent.plan.equivalentState(fixture.plan) &&
               incumbent.executionOrder == fixture.plan.executionOrder().order,
               "every extraction interruption leaves the original plan untouched");
    }

    AssignmentResult installationIncoming(const Fixture & fixture, const MasterAssignmentProblem & problem)
    {
        AssignmentResult incoming;
        incoming.plan = fixture.plan;
        incoming.executionOrder = fixture.plan.executionOrder().order;
        for (const auto & actor : problem.actors())
        {
            incoming.selections.push_back({
                .knowledgeUnitIndex = actor.knowledgeUnitIndex,
                .engineUnitId = actor.engineUnitId,
                .actionIndex = actor.actionIndex,
                .candidateIndex = actor.incumbentCandidateIndex,
                .greedyCandidateIndex = actor.incumbentCandidateIndex,
                .seatedValue = actor.incumbentSeatedValue,
                .stockCoupled = actor.role == MasterActorRole::Mutable,
                .phase = AssignmentResult::SelectionPhase::Greedy});
        }
        return incoming;
    }

    bool sameInstalledPlan(const AssignmentResult & left, const AssignmentResult & right)
    {
        if (!left.plan.equivalentState(right.plan) || left.executionOrder != right.executionOrder ||
            left.stats != right.stats || left.stockConstructionStatus != right.stockConstructionStatus ||
            left.stockConstructionWork != right.stockConstructionWork ||
            left.selections.size() != right.selections.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.selections.size(); ++index)
        {
            const auto & a = left.selections[index];
            const auto & b = right.selections[index];
            if (a.knowledgeUnitIndex != b.knowledgeUnitIndex || a.engineUnitId != b.engineUnitId ||
                a.actionIndex != b.actionIndex || a.candidateIndex != b.candidateIndex ||
                a.greedyCandidateIndex != b.greedyCandidateIndex || a.seatedValue != b.seatedValue ||
                a.stockCoupled != b.stockCoupled || a.phase != b.phase ||
                !std::equal(a.completeValues.begin(), a.completeValues.end(),
                    b.completeValues.begin(), b.completeValues.end(), [](const auto & x, const auto & y)
                    {
                        return x.value == y.value && x.known == y.known;
                    }))
            {
                return false;
            }
        }
        return true;
    }

    void transactionalInstallation()
    {
        Fixture fixture(3);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 20, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 30, 1));
        fixture.input.actors[2].candidates.push_back(moveCandidate(2, {2, 0}, {2, 1}, 40, 1));
        fixture.seat();
        fixture.selections[1].role = MasterActorRole::Fixed;
        require(fixture.plan.commit(2), "committed stock action is fixed before installation preparation");
        const auto problem = fixture.extract();
        auto original = installationIncoming(fixture, problem);
        original.selections[2].completeValues.push_back({40, true});
        RefinementLedger ledger;
        require(ledger.open(1000000), "transaction ledger opens once");
        MasterAssignmentEvaluator evaluator(problem, ledger);
        MasterAssignmentInstallation installation(problem, evaluator, ledger);
        auto incoming = original;
        require(installation.prepare(incoming) == MasterInstallationStatus::Reserved,
                "finalization is reserved before normal search begins");
        const auto result = searchMasterAssignment(problem, evaluator, ledger);
        require(result.improvement.has_value(), "normal search supplies the retained strict challenger");
        require(ledger.consume(ledger.balance(), RefinementWork::MasterExpansion),
                "search can exhaust all unreserved balance");
        const auto installed = installation.finish(result.improvement, incoming);
        expect(installed.status == MasterInstallationStatus::Installed && installed.proof &&
               installed.proof->comparison.deltaLower == 19,
               "terminal reservation funds fresh strict proof and atomic installation");
        expect(incoming.plan.action(0).destination == TilePoint{0, 1} &&
               incoming.plan.action(1) == original.plan.action(1) &&
               incoming.plan.actionReservations(1) == original.plan.actionReservations(1) &&
               incoming.selections[1].phase == original.selections[1].phase,
               "the certified mutable action changes while the fixed action and claims remain exact");
        expect(incoming.plan.action(2) == original.plan.action(2) &&
               incoming.plan.actionReservations(2) == original.plan.actionReservations(2) &&
               incoming.selections[2].phase == original.selections[2].phase &&
               incoming.selections[2].completeValues.size() == 1 &&
               incoming.selections[2].completeValues[0].value == 40 &&
               incoming.selections[2].completeValues[0].known,
               "committed stock actors retain their action, claims, provenance and diagnostics");
        expect(incoming.selections[0].phase == AssignmentResult::SelectionPhase::Master &&
               incoming.executionOrder == installed.proof->challenger.replay->ordering.order &&
               installed.finalizationWork <= installed.reservedWork && ledger.stats().total == 1000000,
               "installed provenance, full execution order and one-ledger finalization are explicit");
        const auto after = ledger.stats();
        expect(installation.finish(result.improvement, incoming).status == MasterInstallationStatus::InvalidContext &&
               ledger.stats() == after, "a terminal reservation cannot be released or published twice");

        enum class Mutation
        {
            None,
            ForeignContext,
            DifferentIncoming,
            MissingClaim,
            ChangedOrder,
            FixedChoice,
            InvalidChoice,
            EqualChoice,
            DiagnosticGrowth,
        };
        for (const auto mutation : {Mutation::None, Mutation::ForeignContext, Mutation::DifferentIncoming,
             Mutation::MissingClaim, Mutation::ChangedOrder, Mutation::FixedChoice, Mutation::InvalidChoice,
             Mutation::EqualChoice, Mutation::DiagnosticGrowth})
        {
            RefinementLedger retryLedger;
            require(retryLedger.open(1000000), "failure transaction ledger");
            MasterAssignmentEvaluator retryEvaluator(problem, retryLedger);
            MasterAssignmentInstallation retry(problem, retryEvaluator, retryLedger);
            auto unchanged = original;
            require(retry.prepare(unchanged) == MasterInstallationStatus::Reserved,
                    "failure injection occurs after paid preparation");
            auto retained = result.improvement;
            switch (mutation)
            {
                case Mutation::None:
                    retained.reset();
                    break;
                case Mutation::ForeignContext:
                    retained->challenger.state.identity = fixture.extract().identity();
                    break;
                case Mutation::DifferentIncoming:
                    ++unchanged.selections[0].seatedValue;
                    break;
                case Mutation::MissingClaim:
                    require(unchanged.plan.replan(0, unchanged.plan.action(0)).accepted,
                            "remove an incoming claim after preparation");
                    break;
                case Mutation::ChangedOrder:
                    std::reverse(unchanged.executionOrder.begin(), unchanged.executionOrder.end());
                    break;
                case Mutation::FixedChoice:
                    retained->challenger.state.choices[1] = 1;
                    break;
                case Mutation::InvalidChoice:
                    retained->challenger.state.choices[0] = 99;
                    break;
                case Mutation::EqualChoice:
                    retained->challenger = retained->incumbent;
                    break;
                case Mutation::DiagnosticGrowth:
                    unchanged.selections[1].completeValues.push_back({0, true});
                    break;
            }
            const auto before = unchanged;
            const auto spent = retryLedger.stats().spent();
            const auto refused = retry.finish(retained, unchanged);
            expect(refused.status != MasterInstallationStatus::Installed && sameInstalledPlan(unchanged, before),
                   "every post-preparation failure leaves plan, claims, order and selections unchanged");
            expect(retryLedger.stats().total == 1000000 &&
                   retryLedger.stats().spent() == spent - refused.reservedWork + refused.finalizationWork &&
                   refused.finalizationWork <= refused.reservedWork,
                   "rollback keeps charged work and only returns the held finalization reservation");
        }
    }

    enum class StockInstallationFault
    {
        None,
        FriendlyWitness,
        EnemyWitness,
        OversizedWitness,
        ForeignWitness,
        SupersededCertificate,
        CurrentForeignQuote,
        CurrentMissingWitness,
        CurrentMissingEnemyWitness,
        CurrentUnregisteredCertificate,
        AccountingOverflow,
    };

    void corruptInstallationWitness(MasterImprovementProof & retained, StockInstallationFault fault,
                                    std::int64_t witnessTariff)
    {
        auto proof = std::make_shared<RelativeStockProofSnapshot>(*retained.challenger.proof);
        if (fault == StockInstallationFault::OversizedWitness)
        {
            proof->ourWitness.resize(static_cast<std::size_t>(witnessTariff) + 1);
        }
        else if (fault == StockInstallationFault::EnemyWitness)
        {
            if (proof->enemyWitness.empty())
            {
                proof->enemyWitness.push_back({NO_STOCK_ROW, {}, 0});
            }
            else
            {
                require(!proof->enemyWitness.front().steps.empty(), "retained enemy row has an owned step");
                ++proof->enemyWitness.front().steps.front().gain;
            }
        }
        else if (proof->ourKind == RelativeStockWitnessKind::Sequential)
        {
            require(!proof->ourWitness.empty() && !proof->ourWitness.front().steps.empty(),
                    "retained friendly sequential row has an owned step");
            ++proof->ourWitness.front().steps.front().gain;
        }
        else
        {
            require(!proof->cheapOurWitness.empty() && !proof->cheapOurWitness.front().nodes.empty(),
                    "retained friendly cheap row has an owned node");
            proof->cheapOurWitness.front().nodes.front() = std::numeric_limits<std::int32_t>::max();
        }
        retained.challenger.proof = std::move(proof);
    }

    Fixture stockInstallationFixture()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor, 1}, -3 - actor, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        return fixture;
    }

    MasterAssignmentOracle::Request stockInstallationReference(const MasterAssignmentProblem & problem)
    {
        auto reference = request();
        reference.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            reference.continuation.cases.push_back(continuationCase(tuple.reference.key));
        }
        return reference;
    }

    void stockInstallationAuthentication()
    {
        const auto fixture = stockInstallationFixture();
        const auto problem = fixture.extract();
        const auto original = installationIncoming(fixture, problem);
        const auto reference = stockInstallationReference(problem);
        const auto exact = MasterAssignmentOracle::enumerate(problem, reference);
        require(exact.exactOptimum.has_value(), "stock installation has an independent complete oracle");
        PropertyStockField field;
        PropertyStockInstance enemy;
        constexpr MasterValuationTariffs tariffs{.quote = 32, .witness = 32};
        struct Case
        {
            StockInstallationFault fault;
            MasterInstallationStatus expected;
        };
        for (const auto & test : {
            Case{StockInstallationFault::None, MasterInstallationStatus::Installed},
            Case{StockInstallationFault::FriendlyWitness, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::EnemyWitness, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::OversizedWitness, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::ForeignWitness, MasterInstallationStatus::InvalidContext},
            Case{StockInstallationFault::SupersededCertificate, MasterInstallationStatus::Installed},
            Case{StockInstallationFault::CurrentForeignQuote, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::CurrentMissingWitness, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::CurrentMissingEnemyWitness, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::CurrentUnregisteredCertificate, MasterInstallationStatus::ProofFailure},
            Case{StockInstallationFault::AccountingOverflow, MasterInstallationStatus::BudgetExhausted}})
        {
            RefinementLedger ledger;
            const auto allowance = test.fault == StockInstallationFault::AccountingOverflow
                ? std::numeric_limits<std::int64_t>::max() : 1000000;
            require(ledger.open(allowance), "native stock installation ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
            MasterAssignmentInstallation installation(problem, evaluator, ledger);
            auto incoming = original;
            require(installation.prepare(incoming) == MasterInstallationStatus::Reserved,
                    "native stock finalization is paid before discovery");
            if (test.fault == StockInstallationFault::SupersededCertificate)
            {
                const auto quote = session.quote(problem.incumbentContinuationKey(), 0);
                require(session.tightenOurUpperWithVerifiedDual(quote, 4096).certificateValid,
                        "incoming native certificate exists before the retained search proof");
            }
            const auto searched = searchMasterAssignment(problem, evaluator, ledger);
            require(searched.improvement && searched.improvement->incumbent.proof &&
                    searched.improvement->challenger.proof && searched.stats.proofFailures == 0,
                    "normal native search supplies genuine owned stock proofs");
            expect(searched.stats.firstImprovementWork && searched.stats.firstImprovementLower &&
                   *searched.stats.firstImprovementLower > 0 &&
                   *searched.stats.firstImprovementLower <= searched.improvement->comparison.deltaLower,
                   "first retained margin is positive and distinct from later proof metadata");
            auto retained = searched.improvement;
            std::unique_ptr<NativeStockSession> foreign;
            switch (test.fault)
            {
                case StockInstallationFault::FriendlyWitness:
                case StockInstallationFault::EnemyWitness:
                case StockInstallationFault::OversizedWitness:
                    corruptInstallationWitness(*retained, test.fault, tariffs.witness);
                    require(evaluator.ownsRetainedEvaluation(retained->challenger),
                            "witness-byte faults preserve the cheap retained association");
                    break;
                case StockInstallationFault::ForeignWitness:
                {
                    foreign = std::make_unique<NativeStockSession>(
                        field, enemy, reference.continuation.cases, ledger);
                    MasterAssignmentEvaluator foreignEvaluator(problem, ledger, *foreign, tariffs);
                    const auto evaluated = foreignEvaluator.evaluate(retained->challenger.state);
                    require(evaluated.proof != nullptr, "foreign equal-content session owns its own witness");
                    retained->challenger.proof = evaluated.proof;
                    break;
                }
                case StockInstallationFault::SupersededCertificate:
                {
                    const auto & previous = retained->incumbent.proof;
                    require(previous->ourUpperCertificate != nullptr, "retained proof owns its prior certificate");
                    const auto historical = masterEvaluationRecord(retained->incumbent);
                    const auto replacement = session.tightenOurUpperWithVerifiedDual(previous->quote, 4096);
                    require(replacement.certificateValid &&
                            replacement.certificateId != previous->quote.ourUpperCertificateId &&
                            session.replayLowerProof(*previous) &&
                            !session.compare(previous->quote, previous->quote).valid(),
                            "superseded certificate loses live authority without losing its owned lower witness");
                    const auto beforeExport = ledger.stats();
                    const auto captures = session.proofCaptures;
                    const auto certificates = session.certificateCalls;
                    const auto exported = masterEvaluationRecord(retained->incumbent);
                    expect(exported == historical && beforeExport == ledger.stats() &&
                           captures == session.proofCaptures && certificates == session.certificateCalls &&
                           exported["quote"].toObject()["ownedUpperCertificatePayload"].toObject()["dualObjective"].toString() ==
                               QString::number(previous->ourUpperCertificate->dualObjective),
                           "historical certificate export preserves owned bytes without registry replay or work");
                    break;
                }
                case StockInstallationFault::CurrentForeignQuote:
                    session.setQuoteFault(QuoteFault::ForeignIdentity);
                    break;
                case StockInstallationFault::CurrentMissingWitness:
                    session.setQuoteFault(QuoteFault::MissingWitness);
                    break;
                case StockInstallationFault::CurrentMissingEnemyWitness:
                    session.setQuoteFault(QuoteFault::MissingEnemyWitness);
                    break;
                case StockInstallationFault::CurrentUnregisteredCertificate:
                    session.setQuoteFault(QuoteFault::UnregisteredCertificate);
                    break;
                case StockInstallationFault::AccountingOverflow:
                    require(ledger.consume(std::numeric_limits<std::int64_t>::max() - ledger.stats().granted,
                                           RefinementWork::MasterExpansion),
                            "gross accounting reaches its limit after preparation and discovery");
                    break;
                case StockInstallationFault::None:
                    require(ledger.consume(ledger.balance(), RefinementWork::MasterExpansion),
                            "native search may consume every unreserved unit");
                    break;
            }
            const auto before = ledger.stats();
            const auto finalized = installation.finish(retained, incoming);
            expect(finalized.status == test.expected, "native installation reports the injected boundary");
            expect(finalized.finalizationWork <= finalized.reservedWork && ledger.stats().total == allowance &&
                   ledger.stats().spent() == before.spent() - finalized.reservedWork + finalized.finalizationWork,
                   "native finalization uses only its original reservation and preserves performed work");
            if (test.expected == MasterInstallationStatus::Installed)
            {
                require(finalized.proof && finalized.proof->comparison.improves(),
                        "installation publishes only a freshly authenticated strict proof");
                const auto & proof = *finalized.proof;
                const auto challenger = MasterAssignmentOracle::evaluateObjective(
                    MasterAssignmentOracle::constructReference(problem, proof.challenger.state.choices),
                    reference.continuation, 1000000);
                const auto incumbent = MasterAssignmentOracle::evaluateObjective(
                    MasterAssignmentOracle::constructReference(problem, proof.incumbent.state.choices),
                    reference.continuation, 1000000);
                expect(proof.comparison.deltaLower > 0 &&
                       proof.comparison.deltaLower <= challenger.total - incumbent.total &&
                       incoming.plan.equivalentState(proof.challenger.replay->plan) &&
                       incoming.executionOrder == proof.challenger.replay->ordering.order,
                       "installed stock margin and complete plan agree with the independent oracle");
                if (test.fault == StockInstallationFault::SupersededCertificate)
                {
                    expect(proof.incumbent.quote()->ourUpperCertificateId !=
                               retained->incumbent.quote()->ourUpperCertificateId &&
                           session.compare(*proof.incumbent.quote(), *proof.challenger.quote()).valid(),
                           "fresh certificate authentication replaces historical registry authority");
                }
            }
            else
            {
                expect(!finalized.proof && sameInstalledPlan(incoming, original),
                       "native authentication failure preserves all incoming selections, claims and order");
            }
            if (test.fault == StockInstallationFault::AccountingOverflow)
            {
                expect(ledger.lastRefusal() &&
                       ledger.lastRefusal()->reason == RefinementRefusalReason::AccountingOverflow &&
                       finalized.finalizationWork == 0,
                       "refund does not hide gross accounting overflow or fabricate performed work");
            }
        }
    }

    void ownedCertificateSerialization()
    {
        constexpr std::int64_t beyondExactDouble = 9007199254740993LL;
        const auto minimum = std::numeric_limits<std::int64_t>::min();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        PackingDualCertificate certificate;
        auto & identity = certificate.identity;
        auto & field = identity.immutableField;
        field.horizonTurns = 10;
        field.width = 2;
        field.height = 1;
        field.capturePointsToCapture = 20;
        field.ownedBaseline = minimum;
        field.ourRows = {{11, 0, 3, 7, 10, {1, 0}}};
        field.enemyRows = {{12, 1, 2, 3, 5, {0, 1}}};
        field.ourColumns = {{4, {1, 0}, {maximum, beyondExactDouble}, OwnerSign::Enemy, 19}};
        field.enemyColumns = {{9, {0, 0}, {minimum, -beyondExactDouble}, OwnerSign::Ours, 23}};
        field.ourWeights = {minimum, maximum, beyondExactDouble};
        field.enemyWeights = {-beyondExactDouble, 0};
        field.columnTiles = {{1, 0}, {0, 0}};
        field.columnCapturers = {-1, 11};
        field.columnCapturePoints = {0, 7};
        field.enemyArrivals = {-1, 2};
        MobilityCostGrid grid(2, 1);
        grid.setCost(0, 0, EntryDirection::West, -1);
        grid.setCost(0, 0, EntryDirection::East, 0);
        grid.setCost(0, 0, EntryDirection::South, std::numeric_limits<std::int32_t>::max());
        grid.setCost(0, 0, EntryDirection::North, 5);
        for (const auto direction : ENTRY_DIRECTIONS)
        {
            grid.setCost(1, 0, direction, static_cast<std::int32_t>(direction) + 1);
        }
        field.mobilityGrids = {grid, MobilityCostGrid{}};
        identity.session = std::numeric_limits<std::uint64_t>::max();
        identity.key = {CanonicalPlanActionKey::fromAction(0, 1, 0, true),
                        CanonicalPlanActionKey::fromAction(1, 0, 0, false)};
        identity.capturedColumns = {0, 129};
        identity.rowDomains = {{minimum, maximum}, {beyondExactDouble, -1}};
        identity.nodeColumns = field.enemyColumns;
        identity.usedNodes = PackingDualNodeMask(130);
        require(identity.usedNodes.set(0) && identity.usedNodes.set(64) && identity.usedNodes.set(129),
                "wide serialized node mask fixture");
        identity.economic = -beyondExactDouble;
        identity.owned = beyondExactDouble;
        identity.incumbentFloor = 17;
        identity.fixedValue = -3;
        certificate.rows = {0, 7};
        certificate.nodePrices = {beyondExactDouble, maximum, minimum};
        certificate.rowReducedMaxima = {minimum + 1, maximum - 1};
        certificate.dualObjective = maximum;
        auto proof = std::make_shared<RelativeStockProofSnapshot>();
        proof->ourUpperCertificate = std::make_shared<const PackingDualCertificate>(certificate);
        MasterAssignmentEvaluation evaluation;
        evaluation.proof = proof;
        const auto recorded = masterEvaluationRecord(evaluation);
        const auto encoded = QJsonDocument(recorded).toJson(QJsonDocument::Compact);
        const auto parsed = QJsonDocument::fromJson(encoded).object();
        const auto payload = parsed["quote"].toObject()["ownedUpperCertificatePayload"].toObject();
        const auto recordedIdentity = payload["identity"].toObject();
        const auto recordedField = recordedIdentity["immutableField"].toObject();
        expect(parsed == recorded && payload.size() == 5 && recordedIdentity.size() == 11 &&
               recordedField.size() == 16 && *proof->ourUpperCertificate == certificate &&
               !recorded["upperVerified"].toBool(),
               "owned certificate serialization is complete payload, not a new authentication claim");
        expect(payload["rows"].toArray() == QJsonArray{0, 7} &&
               payload["nodePrices"].toArray() ==
                   QJsonArray{QString::number(beyondExactDouble), QString::number(maximum), QString::number(minimum)} &&
               payload["rowReducedMaxima"].toArray() ==
                   QJsonArray{QString::number(minimum + 1), QString::number(maximum - 1)} &&
               payload["dualObjective"].isString() && payload["dualObjective"].toString() == QString::number(maximum),
               "certificate prices, maxima and objective retain signed 64-bit values exactly");
        expect(recordedIdentity["session"].toString() == QString::number(identity.session) &&
               recordedIdentity["key"].toArray() == QJsonArray{QJsonArray{0, 1, 0, true}, QJsonArray{1, 0, 0, false}} &&
               recordedIdentity["capturedColumns"].toArray() == QJsonArray{0, 129} &&
               recordedIdentity["rowDomains"].toArray() == QJsonArray{
                   QJsonArray{QString::number(minimum), QString::number(maximum)},
                   QJsonArray{QString::number(beyondExactDouble), QStringLiteral("-1")}} &&
               recordedIdentity["usedNodes"].toObject() ==
                   QJsonObject{{"nodeCount", 130}, {"nodes", QJsonArray{0, 64, 129}}},
               "certificate identity preserves full keys, domains and every wide-mask bit");
        expect(recordedIdentity["economic"].toString() == QString::number(-beyondExactDouble) &&
               recordedIdentity["owned"].toString() == QString::number(beyondExactDouble) &&
               recordedIdentity["incumbentFloor"].toString() == QStringLiteral("17") &&
               recordedIdentity["fixedValue"].toString() == QStringLiteral("-3"),
               "certificate identity economics remain lossless and distinct");
        expect(recordedField["horizonTurns"].toInt() == 10 && recordedField["width"].toInt() == 2 &&
               recordedField["height"].toInt() == 1 && recordedField["capturePointsToCapture"].toInt() == 20 &&
               recordedField["ownedBaseline"].toString() == QString::number(minimum) &&
               recordedField["ourWeights"].toArray() ==
                   QJsonArray{QString::number(minimum), QString::number(maximum), QString::number(beyondExactDouble)} &&
               recordedField["enemyWeights"].toArray() == QJsonArray{QString::number(-beyondExactDouble), QStringLiteral("0")},
               "immutable field dimensions, capture threshold and all weights are retained");
        expect(recordedField["ourRows"].toArray() == QJsonArray{QJsonObject{{"knowledgeIndex", 11}, {"gridIdentity", 0},
                   {"movementPoints", 3}, {"capturePoints", 7}, {"captureRate", 10}, {"tile", QJsonArray{1, 0}}}} &&
               recordedField["enemyRows"].toArray() == QJsonArray{QJsonObject{{"knowledgeIndex", 12}, {"gridIdentity", 1},
                   {"movementPoints", 2}, {"capturePoints", 3}, {"captureRate", 5}, {"tile", QJsonArray{0, 1}}}},
               "both sides retain every public stock-row field");
        const QJsonArray ourColumns{QJsonObject{{"slot", 4}, {"tile", QJsonArray{1, 0}},
            {"income", QJsonObject{{"oursPerTurn", QString::number(maximum)},
                {"enemyPerTurn", QString::number(beyondExactDouble)}}}, {"ownerBefore", -1}, {"buildingIndex", 19}}};
        const QJsonArray enemyColumns{QJsonObject{{"slot", 9}, {"tile", QJsonArray{0, 0}},
            {"income", QJsonObject{{"oursPerTurn", QString::number(minimum)},
                {"enemyPerTurn", QString::number(-beyondExactDouble)}}}, {"ownerBefore", 1}, {"buildingIndex", 23}}};
        expect(recordedField["ourColumns"].toArray() == ourColumns &&
               recordedField["enemyColumns"].toArray() == enemyColumns &&
               recordedIdentity["nodeColumns"].toArray() == enemyColumns,
               "stock and native-node columns preserve identity, ownership and side-specific income");
        expect(recordedField["columnTiles"].toArray() == QJsonArray{QJsonArray{1, 0}, QJsonArray{0, 0}} &&
               recordedField["columnCapturers"].toArray() == QJsonArray{-1, 11} &&
               recordedField["columnCapturePoints"].toArray() == QJsonArray{0, 7} &&
               recordedField["enemyArrivals"].toArray() == QJsonArray{-1, 2},
               "immutable capture and arrival metadata are not inferred from other fields");
        const auto grids = recordedField["mobilityGrids"].toArray();
        expect(grids.size() == 2 && grids[0].toObject() == QJsonObject{
                   {"width", 2}, {"height", 1}, {"entryDirections", QJsonArray{0, 1, 2, 3}},
                   {"costs", QJsonArray{QJsonArray{
                       QJsonArray{-1, 0, std::numeric_limits<std::int32_t>::max(), 5}, QJsonArray{1, 2, 3, 4}}}}} &&
               grids[1].toObject() == QJsonObject{{"width", 0}, {"height", 0},
                   {"entryDirections", QJsonArray{0, 1, 2, 3}}, {"costs", QJsonArray{}}},
               "mobility export preserves y/x/direction ordering, impassability and empty grids");
        proof->ourUpperCertificate.reset();
        expect(!masterEvaluationRecord(evaluation)["quote"].toObject().contains("ownedUpperCertificatePayload") &&
               !masterEvaluationRecord(MasterAssignmentEvaluation{}).contains("quote"),
               "missing owned certificate data is omitted rather than fabricated");
    }

    void stockFinalizationBudgets()
    {
        const auto fixture = stockInstallationFixture();
        const auto problem = fixture.extract();
        const auto original = installationIncoming(fixture, problem);
        const auto reference = stockInstallationReference(problem);
        PropertyStockField field;
        PropertyStockInstance enemy;
        constexpr MasterValuationTariffs tariffs{.quote = 32, .witness = 32};
        RefinementLedger ledger;
        require(ledger.open(1000000), "finalization upper ledger");
        NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
        const auto upper = evaluator.finalizationWork();
        require(upper.has_value(), "checked finalization upper is available before terminal work");
        const auto incomingState = MasterAssignmentState{
            problem.identity(), std::vector<std::int32_t>(problem.incumbentChoices().begin(),
                                                          problem.incumbentChoices().end())};
        const auto challengerState = MasterAssignmentState{problem.identity(), {1, 1, 1}};
        const auto retainedIncoming = evaluator.evaluate(incomingState);
        const auto retainedChallenger = evaluator.evaluate(challengerState);
        require(evaluator.ownsRetainedEvaluation(retainedIncoming) &&
                evaluator.ownsRetainedEvaluation(retainedChallenger),
                "terminal budget fixture owns both native lower snapshots");
        const auto before = ledger.stats();
        expect(evaluator.replayRetainedEvaluation(retainedIncoming) == MasterObjectiveStatus::Verified &&
               evaluator.replayRetainedEvaluation(retainedChallenger) == MasterObjectiveStatus::Verified,
               "generated native witness payloads fit their unchanged witness tariff");
        expect(ledger.stats().spent() - before.spent() == 2 * tariffs.witness &&
               ledger.stats().consumed - before.consumed == 2 * tariffs.witness,
               "both retained lower replays are explicitly charged and nonrefundable");
        auto freshIncoming = evaluator.evaluate(incomingState);
        auto freshChallenger = evaluator.evaluate(challengerState);
        expect(evaluator.compare(freshIncoming, freshChallenger).valid() &&
               ledger.stats().spent() - before.spent() <= *upper,
               "two owned replays, fresh full evaluations and current comparison fit the reserved upper");

        for (const std::int64_t balance : {std::int64_t{0}, tariffs.witness - 1, tariffs.witness})
        {
            RefinementLedger prefixLedger;
            require(prefixLedger.open(1000000), "owned replay prefix ledger");
            NativeStockSession prefixSession(field, enemy, reference.continuation.cases, prefixLedger);
            MasterAssignmentEvaluator prefixEvaluator(problem, prefixLedger, prefixSession, tariffs);
            const auto retained = prefixEvaluator.evaluate(incomingState);
            require(prefixEvaluator.ownsRetainedEvaluation(retained), "prefix failure starts with a native witness");
            require(prefixLedger.consume(prefixLedger.balance() - balance, RefinementWork::MasterExpansion),
                    "refinement consumes the available prefix before retained replay");
            const auto prefixBefore = prefixLedger.stats();
            const auto status = prefixEvaluator.replayRetainedEvaluation(retained);
            expect(status == (balance < tariffs.witness ? MasterObjectiveStatus::BudgetExhausted
                                                       : MasterObjectiveStatus::Verified) &&
                   prefixLedger.stats().spent() - prefixBefore.spent() ==
                       (balance < tariffs.witness ? 0 : tariffs.witness),
                   "one-short retained replay refuses before native work and exact tariff succeeds");
            if (balance < tariffs.witness)
            {
                expect(prefixLedger.lastRefusal() &&
                       prefixLedger.lastRefusal()->work == RefinementWork::MasterWitness &&
                       prefixLedger.lastRefusal()->requested == tariffs.witness &&
                       prefixLedger.lastRefusal()->balance == balance,
                       "retained replay budget failure records the exact shared-ledger boundary");
            }
        }
        for (const auto invalid : {
            MasterValuationTariffs{.quote = std::numeric_limits<std::int64_t>::max(), .witness = 32},
            MasterValuationTariffs{.quote = 32, .witness = std::numeric_limits<std::int64_t>::max()},
            MasterValuationTariffs{.quote = -1, .witness = 32},
            MasterValuationTariffs{.quote = 32, .witness = -1}})
        {
            RefinementLedger invalidLedger;
            require(invalidLedger.open(1000000), "invalid finalization tariff ledger");
            NativeStockSession invalidSession(field, enemy, reference.continuation.cases, invalidLedger);
            MasterAssignmentEvaluator invalidEvaluator(problem, invalidLedger, invalidSession, invalid);
            MasterAssignmentInstallation installation(problem, invalidEvaluator, invalidLedger);
            auto incoming = original;
            expect(installation.prepare(incoming) == MasterInstallationStatus::ArithmeticFailure &&
                   installation.finish(std::nullopt, incoming).status == MasterInstallationStatus::ArithmeticFailure &&
                   sameInstalledPlan(incoming, original) &&
                   invalidLedger.stats().grantedByWork[static_cast<std::size_t>(RefinementWork::MasterInstallation)] == 0,
                   "negative or overflowing terminal tariffs cannot reserve or publish a plan");
        }
        MasterAssignmentInstallation installation(problem, evaluator, ledger);
        const auto preparationBefore = ledger.stats().spent();
        require(installation.prepare(original) == MasterInstallationStatus::Reserved,
                "measure the complete successful preparation boundary");
        const auto required = ledger.stats().spent() - preparationBefore;
        const auto metadata = required - installation.reservedWork();
        auto unchanged = original;
        require(installation.finish(std::nullopt, unchanged).status == MasterInstallationStatus::NoImprovement &&
                sameInstalledPlan(unchanged, original), "unused native reservation is returned exactly once");
        for (const auto allowance : {std::int64_t{0}, metadata - 1, metadata, required - 1, required})
        {
            RefinementLedger prefixLedger;
            require(prefixLedger.open(allowance), "installation preparation prefix ledger");
            NativeStockSession prefixSession(field, enemy, reference.continuation.cases, prefixLedger);
            MasterAssignmentEvaluator prefixEvaluator(problem, prefixLedger, prefixSession, tariffs);
            MasterAssignmentInstallation prefix(problem, prefixEvaluator, prefixLedger);
            auto incoming = original;
            const auto status = prefix.prepare(incoming);
            expect(status == (allowance < required ? MasterInstallationStatus::BudgetExhausted
                                                  : MasterInstallationStatus::Reserved),
                   "preparation retains the exact metadata and terminal-reservation cutoff");
            const auto finalized = prefix.finish(std::nullopt, incoming);
            expect(sameInstalledPlan(incoming, original) && prefixLedger.stats().spent() <= allowance &&
                   finalized.finalizationWork == 0,
                   "all preparation cutoffs preserve the original complete plan without a second allowance");
        }
    }

    void observationalProfile()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 10, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        RefinementLedger plainLedger;
        RefinementLedger timedLedger;
        require(plainLedger.open(100000) && timedLedger.open(100000), "profiling ledgers");
        MasterAssignmentProfile profile;
        MasterAssignmentEvaluator plainEvaluator(problem, plainLedger);
        MasterAssignmentEvaluator timedEvaluator(problem, timedLedger, &profile);
        const auto plain = searchMasterAssignment(problem, plainEvaluator, plainLedger, {.retainDiagnostics = true});
        const auto timed = searchMasterAssignment(problem, timedEvaluator, timedLedger,
            {.retainDiagnostics = true, .profile = &profile});
        expect(masterSearchRecord(plain) == masterSearchRecord(timed),
               "wall-time instrumentation cannot alter the search schedule, proof, or result");
        std::int64_t exclusive = 0;
        for (const auto & sample : profile.samples)
        {
            expect(sample.exclusiveNanos >= 0 && sample.inclusiveNanos >= sample.exclusiveNanos,
                   "nested profiling separates inclusive and exclusive elapsed time");
            exclusive += sample.exclusiveNanos;
        }
        const auto & total = profile.samples[static_cast<std::size_t>(MasterPhase::Search)];
        expect(total.calls == 1 && exclusive == total.inclusiveNanos,
               "exclusive phase times account for the complete measured search");

        MasterAssignmentProfile ended;
        {
            MasterProfileScope outer(&ended, MasterPhase::Shadow);
            MasterProfileScope setup(&ended, MasterPhase::FriendlySetup);
            {
                MasterProfileScope nested(&ended, MasterPhase::Quote);
            }
            setup.finish();
            setup.finish();
            MasterProfileScope search(&ended, MasterPhase::FriendlySearch);
        }
        exclusive = 0;
        for (const auto & sample : ended.samples)
        {
            expect(sample.exclusiveNanos >= 0 && sample.inclusiveNanos >= sample.exclusiveNanos,
                   "explicitly ending setup preserves nonnegative nested measurements");
            exclusive += sample.exclusiveNanos;
        }
        expect(ended.samples[static_cast<std::size_t>(MasterPhase::FriendlySetup)].calls == 1 &&
                   ended.samples[static_cast<std::size_t>(MasterPhase::FriendlySearch)].calls == 1 &&
                   ended.samples[static_cast<std::size_t>(MasterPhase::Quote)].calls == 1 &&
                   exclusive == ended.samples[static_cast<std::size_t>(MasterPhase::Shadow)].inclusiveNanos,
               "ending setup once restores the parent scope while prepared objects remain alive");
    }

    void reservationFailureExport()
    {
        RefinementLedger ledger;
        require(ledger.open(5), "refusal export ledger");
        expect(masterRefusalRecord(ledger.lastRefusal()).isEmpty(), "no invented failure in an unused ledger");
        require(!ledger.consume(6, RefinementWork::MasterWitness), "export fixture refuses its indivisible witness");
        const auto stats = ledger.stats();
        const auto receipt = ledger.lastRefusal();
        const auto record = masterRefusalRecord(receipt);
        expect(record.size() == 5 && record.value("work").toInt() == static_cast<int>(RefinementWork::MasterWitness) &&
                   record.value("reason").toInt() == static_cast<int>(RefinementRefusalReason::Balance) &&
                   record.value("requested").toString() == QStringLiteral("6") &&
                   record.value("balance").toString() == QStringLiteral("5") &&
                   record.value("refundable").isBool() && !record.value("refundable").toBool(),
               "failure serialization preserves exact request, reason, category and reservation kind");
        expect(ledger.stats() == stats && ledger.lastRefusal() == receipt,
               "serializing failure metadata changes neither ledger work nor failure state");
    }

    void refinementPreflightReceipts()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(moveCandidate(actor, {actor, 0}, {actor, 1}, -3, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto problem = fixture.extract();
        std::vector<MasterAssignmentOracle::ContinuationCase> cases;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            cases.push_back(continuationCase(tuple.reference.key));
        }
        PropertyStockField field;
        PropertyStockInstance enemy;
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        constexpr std::int64_t nativeAllowance = 4096;
        for (const auto term : {MasterRefinementTerm::Friendly, MasterRefinementTerm::Enemy,
                                MasterRefinementTerm::FutureDual})
        {
            for (const auto shortfall : {0, 1})
            {
                RefinementLedger ledger;
                require(ledger.open(1000000), "preflight receipt ledger");
                NativeStockSession session(field, enemy, cases, ledger);
                MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
                auto evaluated = evaluator.evaluate({problem.identity(),
                    std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
                require(evaluated.lowerReplayed && evaluated.quote() && !evaluated.quote()->ours.exact() &&
                            !evaluated.quote()->enemy.exact(), "preflight fixture has unresolved native terms");
                auto preparation = session.ourRerunWork();
                auto overhead = tariffs.quote * 2 + tariffs.witness;
                if (term == MasterRefinementTerm::Enemy)
                {
                    preparation = session.enemyRerunWork(evaluated.quote()->capturedColumns);
                }
                else if (term == MasterRefinementTerm::FutureDual)
                {
                    preparation = session.preparation.scanWork;
                    overhead = tariffs.quote + tariffs.witness * 2 + tariffs.certificateIdentity;
                }
                const auto required = nativeAllowance + preparation + overhead;
                require(ledger.consume(ledger.balance() - required + shortfall, RefinementWork::MasterPreparation),
                        "fixture leaves the exact requested preflight boundary");
                const auto before = ledger.stats();
                const auto proof = evaluated.proof;
                const auto status = evaluator.refine(evaluated, term, nativeAllowance);
                if (shortfall == 0)
                {
                    expect(status == MasterRefinementStatus::Updated && !evaluator.refinementRefusal() &&
                               !ledger.lastRefusal(), "exact preflight balance admits native refinement");
                }
                else
                {
                    const auto refused = evaluator.refinementRefusal();
                    expect(status == MasterRefinementStatus::BudgetExhausted && refused &&
                               refused->term == term && refused->requested == required &&
                               refused->balance == required - 1 && refused->allowance == nativeAllowance &&
                               refused->preparation == preparation,
                           "preflight receipt identifies the exact composite native request");
                    expect(ledger.stats() == before && !ledger.lastRefusal() && evaluated.proof == proof,
                           "preflight evidence adds no ledger charge, refusal count, or witness change");
                    const auto record = masterRefinementRefusalRecord(refused);
                    expect(record.size() == 5 && record.value("term").toInt() == static_cast<int>(term) &&
                               record.value("requested").toString() == QString::number(required) &&
                               record.value("balance").toString() == QString::number(required - 1) &&
                               record.value("allowance").toString() == QString::number(nativeAllowance) &&
                               record.value("preparation").toString() == QString::number(preparation),
                           "preflight export preserves all native request components");
                    expect(evaluator.refine(evaluated, term, 0) == MasterRefinementStatus::Unavailable &&
                               !evaluator.refinementRefusal() && ledger.stats() == before,
                           "the next refinement call clears stale preflight metadata without spending work");
                }
            }
        }
        RefinementLedger searchLedger;
        require(searchLedger.open(50000), "search preflight receipt ledger");
        NativeStockSession searchSession(field, enemy, cases, searchLedger);
        MasterAssignmentEvaluator searchEvaluator(problem, searchLedger, searchSession, tariffs);
        const auto result = searchMasterAssignment(problem, searchEvaluator, searchLedger,
            {.schedule = {.continuationSlice = 1000000, .nodesPerRefinement = 1}});
        expect(result.budgetExhausted && result.refinementRefusal &&
                   result.refinementRefusal == searchEvaluator.refinementRefusal() && !searchLedger.lastRefusal() &&
                   result.refinementRefusal->term == MasterRefinementTerm::Friendly &&
                   result.refinementRefusal->allowance == 1000000 &&
                   result.refinementRefusal->balance == searchLedger.balance(),
               "bounded search retains the actual stopping preflight without inventing a ledger refusal");
    }

    void checkCertificateReadinessCutoff(
        const MasterAssignmentProblem & problem, const std::vector<MasterAssignmentOracle::ContinuationCase> & cases,
        std::int64_t remaining)
    {
        constexpr std::int64_t readinessFee = 37;
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "paid readiness ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
        auto evaluated = evaluator.evaluate({problem.identity(),
            std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
        require(evaluated.quote() && !evaluated.quote()->ours.exact(), "paid readiness has an open friendly term");
        session.preparation = {.status = RelativeStockPreparationStatus::MissingRows, .readinessWork = readinessFee};
        require(ledger.consume(ledger.balance() - remaining, RefinementWork::MasterPreparation),
                "leave exact readiness boundary");
        const auto before = ledger.stats();
        const auto proof = evaluated.proof;
        const auto grants = ledger.retainedGrantCount();
        const auto status = evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 4096);
        const auto receipt = evaluator.certificatePreparation();
        require(receipt.has_value(), "paid readiness retains its receipt");
        const auto paid = remaining < readinessFee ? 0 : readinessFee;
        expect(status == (remaining < readinessFee ? MasterRefinementStatus::BudgetExhausted
                                                  : MasterRefinementStatus::Unavailable) &&
                   ledger.stats().spent() - before.spent() == paid &&
                   ledger.stats().consumed - before.consumed == paid &&
                   ledger.retainedGrantCount() == grants && session.certificateCalls == 0 && evaluated.proof == proof,
               "missing or unaffordable rows never start native certificate work or replace a proof");
        expect(receipt->readinessWork == paid && receipt->scanWork == 0 && session.readinessCalls == 1,
               "readiness receipts report only paid work and do not invent an option scan");
        if (remaining < readinessFee)
        {
            expect(receipt->status == RelativeStockPreparationStatus::BudgetExhausted && ledger.lastRefusal() &&
                       ledger.lastRefusal()->requested == readinessFee &&
                       ledger.lastRefusal()->balance == remaining && !ledger.lastRefusal()->refundable,
                   "readiness refusal identifies the non-refundable fee before key inspection");
        }
        else
        {
            expect(receipt->status == RelativeStockPreparationStatus::MissingRows && !ledger.lastRefusal(),
                   "an exactly funded missing-row check remains distinct from budget exhaustion");
        }
        const auto after = ledger.stats();
        const auto record = masterCertificatePreparationRecord(receipt);
        expect(record.size() == 3 && record.value("readinessWork").toString() == QString::number(paid) &&
                   record.value("scanWork").toString() == QStringLiteral("0") &&
                   record.value("status").toInt() == static_cast<int>(receipt->status) &&
                   ledger.stats() == after && session.readinessCalls == 1,
               "serializing readiness does not execute the paid operation again");
        if (remaining == 100000)
        {
            session.preparation = {.status = RelativeStockPreparationStatus::Ready,
                                   .readinessWork = readinessFee, .scanWork = 42};
            expect(evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 1) == MasterRefinementStatus::Updated &&
                       session.certificateCalls == 1 && session.verifiedCertificates == 0 && evaluated.proof == proof,
                   "a newly ready catalog with an undersized native slice remains eligible for allowance growth");
            expect(evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 4096) == MasterRefinementStatus::Updated &&
                       session.certificateCalls == 2 && session.verifiedCertificates == 1 &&
                       session.replayLowerProof(*proof),
                   "a later funded attempt verifies a certificate while the earlier owned lower proof survives");
        }
        const auto finalWork = ledger.stats();
        expect(evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, 0) == MasterRefinementStatus::Unavailable &&
                   !evaluator.certificatePreparation() && !evaluator.refinementRefusal() && ledger.stats() == finalWork,
               "a new non-attempt clears old preparation receipts without charging another fee");
    }

    void checkCertificatePreparationReservation(
        const MasterAssignmentProblem & problem, const std::vector<MasterAssignmentOracle::ContinuationCase> & cases,
        std::int64_t scanWork, std::int64_t shortfall)
    {
        constexpr std::int64_t readinessFee = 37;
        constexpr std::int64_t nativeAllowance = 4096;
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "per-key preparation ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
        auto evaluated = evaluator.evaluate({problem.identity(),
            std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
        session.preparation = {.status = RelativeStockPreparationStatus::Ready,
                               .readinessWork = readinessFee, .scanWork = scanWork};
        const auto proof = evaluated.proof;
        constexpr std::int64_t ordinaryScanWork = 42;
        const auto required = nativeAllowance + ordinaryScanWork + tariffs.quote + 2 * tariffs.witness +
            tariffs.certificateIdentity;
        require(ledger.consume(ledger.balance() - required - readinessFee + shortfall, RefinementWork::MasterPreparation),
                "leave readiness plus native composite boundary");
        const auto before = ledger.stats().spent();
        const auto status = evaluator.refine(evaluated, MasterRefinementTerm::FutureDual, nativeAllowance);
        if (scanWork != ordinaryScanWork)
        {
            expect(status == MasterRefinementStatus::Invalid && session.certificateCalls == 0 &&
                       evaluated.proof == proof && ledger.stats().spent() - before == readinessFee,
                   "negative or overflowing scan reservations fail closed after only the real readiness fee");
        }
        else if (shortfall == 1)
        {
            const auto refused = evaluator.refinementRefusal();
            expect(status == MasterRefinementStatus::BudgetExhausted && refused &&
                       refused->requested == required && refused->balance == required - 1 &&
                       refused->preparation == scanWork && session.certificateCalls == 0 &&
                       !ledger.lastRefusal() && evaluated.proof == proof &&
                       ledger.stats().spent() - before == readinessFee,
                   "composite preflight retains refresh funding and distinguishes earlier paid readiness");
        }
        else
        {
            expect(status == MasterRefinementStatus::Updated && session.verifiedCertificates == 1 &&
                       !evaluator.refinementRefusal() && !ledger.lastRefusal() && ledger.balance() >= 0,
                   "exact readiness-plus-native funding admits independently verified tightening");
        }
    }

    void certificateReadinessAccounting()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(moveCandidate(actor, {actor, 0}, {actor, 1}, -3, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto problem = fixture.extract();
        std::vector<MasterAssignmentOracle::ContinuationCase> cases;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            cases.push_back(continuationCase(tuple.reference.key));
        }
        for (const auto remaining : {0, 36, 37, 100000})
        {
            checkCertificateReadinessCutoff(problem, cases, remaining);
        }
        checkCertificatePreparationReservation(problem, cases, 42, 0);
        checkCertificatePreparationReservation(problem, cases, 42, 1);
        checkCertificatePreparationReservation(problem, cases, -1, 0);
        checkCertificatePreparationReservation(problem, cases, std::numeric_limits<std::int64_t>::max(), 0);
    }

    MasterAssignmentProblem operationProblem()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(moveCandidate(actor, {actor, 0}, {actor, 1}, -3, 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        return fixture.extract();
    }

    std::vector<MasterAssignmentOracle::ContinuationCase> operationCases(const MasterAssignmentProblem & problem)
    {
        std::vector<MasterAssignmentOracle::ContinuationCase> cases;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            cases.push_back(continuationCase(tuple.reference.key));
        }
        return cases;
    }

    MasterAssignmentState operationIncumbent(const MasterAssignmentProblem & problem)
    {
        return {problem.identity(),
            std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())};
    }

    void operationFundingBoundaries()
    {
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        constexpr std::int64_t cap = 128;
        constexpr std::int64_t retention = 16;
        for (const auto term : {MasterRefinementTerm::Friendly, MasterRefinementTerm::Enemy})
        {
            for (const auto shortfall : {0, 1})
            {
                PropertyStockField field;
                PropertyStockInstance enemy;
                RefinementLedger ledger;
                require(ledger.open(1000000), "operation boundary ledger");
                NativeStockSession session(field, enemy, cases, ledger);
                session.operationServiceEnabled = true;
                MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
                auto evaluated = evaluator.evaluate(operationIncumbent(problem));
                std::shared_ptr<const MasterOperationReadiness> readiness;
                const auto beforeOffer = ledger.stats();
                const auto offer = evaluator.operationOffer(evaluated, term, cap, readiness);
                const auto expected = cap + session.operationTariff + 5 * tariffs.quote +
                    4 * tariffs.witness + retention;
                require(offer.status == MasterOperationStatus::Ready, "ordinary offer is initially affordable");
                expect(offer.remainingRequiredWork == expected && offer.preparationWork == 0 &&
                           offer.quotaWork == 0 && !readiness &&
                           ledger.stats().spent() - beforeOffer.spent() == offer.inspectionWork,
                       "offer funding includes both comparison refreshes and retained-proof storage");
                require(ledger.consume(ledger.balance() - expected + shortfall, RefinementWork::MasterPreparation),
                        "leave exact complete-transaction boundary");
                const auto before = ledger.stats();
                const auto proof = evaluated.proof;
                const auto copiedOffer = offer;
                const auto outcome = evaluator.executeOperation(evaluated, offer);
                if (shortfall != 0)
                {
                    expect(outcome.status == MasterRefinementStatus::BudgetExhausted && !outcome.admitted &&
                               outcome.executionWork == 0 && ledger.stats() == before &&
                               evaluated.proof == proof && session.nativeRefinementCalls == 0 &&
                               !ledger.lastRefusal(),
                           "one-short complete funding rejects before refresh or native admission");
                }
                else
                {
                    expect(outcome.status == MasterRefinementStatus::Updated && outcome.admitted &&
                               session.nativeRefinementCalls == 1 &&
                               outcome.executionWork == ledger.stats().spent() - before.spent() &&
                               ledger.balance() >= 2 * (tariffs.quote + tariffs.witness) + retention &&
                               evaluated.lowerReplayed && evaluated.upperVerified,
                           "exact full funding retains the post-operation proof and comparison reserve");
                }
                const auto after = ledger.stats();
                const auto duplicate = evaluator.executeOperation(evaluated, copiedOffer);
                expect(duplicate.status == MasterRefinementStatus::Invalid && !duplicate.admitted &&
                           duplicate.executionWork == 0 && ledger.stats() == after,
                       "copies share one paid inspection permit, including a refused first execution");
            }
        }
    }

    void operationReadinessReuse()
    {
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "operation readiness ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        session.operationServiceEnabled = true;
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {11, 17, 23});
        auto evaluated = evaluator.evaluate(operationIncumbent(problem));
        std::shared_ptr<const MasterOperationReadiness> readiness;
        session.operationPreparation = {.status = RelativeStockPreparationStatus::MissingRows, .paidWork = 37};
        const auto first = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
        expect(first.status == MasterOperationStatus::MissingPrerequisite && first.preparationWork == 37 &&
                   readiness && session.operationPreparationCalls == 1 && session.certificateCalls == 0,
               "paid missing prerequisite is not a native certificate attempt");
        const auto cached = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
        expect(cached.status == MasterOperationStatus::MissingPrerequisite && cached.preparationWork == 0 &&
                   session.operationPreparationCalls == 1 && session.certificateCalls == 0,
               "same identity and row revision do not repay a missing-row check");
        ++session.operationRevision;
        session.operationPreparation = {.status = RelativeStockPreparationStatus::Ready,
                                        .paidWork = 37, .scanWork = 42, .minimumNativeWork = 4096};
        const auto ready = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
        expect(ready.status == MasterOperationStatus::Ready && ready.allowance == 16384 &&
                   ready.preparationWork == 37 && ready.quotaWork == 8 &&
                   session.operationPreparationCalls == 2 && session.certificateCalls == 0 &&
                   readiness->key == evaluated.quote()->key,
               "new shared-row revision reopens readiness and plans strictly above the native minimum");
        ++session.operationEntryWork;
        const auto refreshed = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 16384, readiness);
        expect(refreshed.status == MasterOperationStatus::Ready && refreshed.preparationWork == 0 &&
                   refreshed.remainingRequiredWork == ready.remainingRequiredWork + 1 &&
                   session.operationPreparationCalls == 2,
               "cached readiness does not freeze the next native entry tariff");
        const auto outcome = evaluator.executeOperation(evaluated, refreshed);
        expect(outcome.status == MasterRefinementStatus::Updated && outcome.admitted &&
                   outcome.certificateProgress && session.certificateCalls == 1 &&
                   evaluated.upperVerified && evaluated.lowerReplayed,
               "paid prepared operation still uses the native certificate verifier and owned proof");
    }

    void operationRejectionAndProgress()
    {
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        for (const auto fault : {QuoteFault::None, QuoteFault::ForeignIdentity, QuoteFault::UnregisteredCertificate})
        {
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(1000000), "operation authentication ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
            auto evaluated = evaluator.evaluate(operationIncumbent(problem));
            std::shared_ptr<const MasterOperationReadiness> readiness;
            const auto disabled = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 1, readiness);
            expect(disabled.status == MasterOperationStatus::Unavailable && session.nativeRefinementCalls == 0,
                   "legacy fixture does not silently enable operation service");
            session.operationServiceEnabled = true;
            const auto offer = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 1, readiness);
            require(offer.status == MasterOperationStatus::Ready, "authentication offer is funded");
            session.setQuoteFault(fault);
            const auto outcome = evaluator.executeOperation(evaluated, offer);
            if (fault == QuoteFault::None)
            {
                const auto repeated = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 1, readiness);
                require(repeated.status == MasterOperationStatus::Ready, "unfinished native attempt remains available");
                const auto unchanged = evaluator.executeOperation(evaluated, repeated);
                expect(outcome.status == MasterRefinementStatus::Updated && outcome.admitted &&
                           unchanged.status == MasterRefinementStatus::Updated && unchanged.admitted &&
                           !unchanged.completed && !unchanged.lowerProgress && !unchanged.upperProgress &&
                           !unchanged.certificateProgress && unchanged.executionWork > 0,
                       "an admitted unfinished native attempt need not improve either endpoint");
            }
            else
            {
                expect(outcome.status == MasterRefinementStatus::Invalid && !outcome.admitted &&
                           session.nativeRefinementCalls == 0 && outcome.executionWork == tariffs.quote,
                       "foreign or stale certificate authorization fails on paid fresh quote before native entry");
            }
        }
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "dynamic tariff ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        session.operationServiceEnabled = true;
        MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
        auto evaluated = evaluator.evaluate(operationIncumbent(problem));
        std::shared_ptr<const MasterOperationReadiness> readiness;
        const auto offer = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 128, readiness);
        require(offer.status == MasterOperationStatus::Ready, "old dynamic tariff offer is funded");
        require(ledger.consume(ledger.balance() - offer.remainingRequiredWork, RefinementWork::MasterPreparation),
                "retain only the old envelope");
        session.operationTariff += offer.remainingRequiredWork;
        const auto outcome = evaluator.executeOperation(evaluated, offer);
        expect(outcome.status == MasterRefinementStatus::BudgetExhausted && !outcome.admitted &&
                   session.nativeRefinementCalls == 0 && evaluator.refinementRefusal() &&
                   outcome.executionWork == tariffs.quote && !ledger.lastRefusal(),
               "a later larger owner tariff cannot enter under an earlier cheaper offer");
    }

    void operationPreparationCutoffs()
    {
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        PropertyStockField field;
        PropertyStockInstance enemy;
        constexpr MasterValuationTariffs tariffs{11, 17, 23};
        for (const auto remaining : {0, 36, 37})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "operation preparation cutoff ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            session.operationServiceEnabled = true;
            session.operationPreparation = {.status = RelativeStockPreparationStatus::MissingRows, .paidWork = 37};
            MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
            auto evaluated = evaluator.evaluate(operationIncumbent(problem));
            std::shared_ptr<const MasterOperationReadiness> readiness;
            const auto measured = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 128, readiness);
            require(ledger.consume(ledger.balance() - measured.inspectionWork - remaining,
                                  RefinementWork::MasterPreparation), "leave inspection plus prerequisite boundary");
            const auto before = ledger.stats().spent();
            const auto result = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
            expect(result.status == (remaining < 37 ? MasterOperationStatus::FundingRefused
                                                    : MasterOperationStatus::MissingPrerequisite) &&
                       result.preparationWork == (remaining < 37 ? 0 : 37) &&
                       ledger.stats().spent() - before == result.inspectionWork + result.preparationWork &&
                       session.certificateCalls == 0,
                   "prerequisite refusal charges only completed inspection and preparation");
        }
        for (const auto invalid : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max()})
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "operation overflow ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            session.operationServiceEnabled = true;
            session.operationPreparation.minimumNativeWork = invalid;
            MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
            auto evaluated = evaluator.evaluate(operationIncumbent(problem));
            std::shared_ptr<const MasterOperationReadiness> readiness;
            const auto result = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
            expect(result.status == MasterOperationStatus::Invalid && session.certificateCalls == 0 &&
                       session.nativeRefinementCalls == 0,
                   "negative native requirements and unrepresentable strict growth fail before admission");
        }
        RefinementLedger ledger;
        require(ledger.open(1000000), "paid preparation refusal ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        session.operationServiceEnabled = true;
        session.operationPreparation = {.status = RelativeStockPreparationStatus::BudgetExhausted,
            .paidWork = 37, .requiredPreparationWork = 200, .readinessChecked = true};
        MasterAssignmentEvaluator evaluator(problem, ledger, session, tariffs);
        auto evaluated = evaluator.evaluate(operationIncumbent(problem));
        std::shared_ptr<const MasterOperationReadiness> readiness;
        const auto measured = evaluator.operationOffer(evaluated, MasterRefinementTerm::Friendly, 128, readiness);
        require(ledger.consume(ledger.balance() - measured.inspectionWork - 37 - 199,
                              RefinementWork::MasterPreparation), "leave a paid but unfinished preparation");
        const auto first = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
        const auto second = evaluator.operationOffer(evaluated, MasterRefinementTerm::FutureDual, 4096, readiness);
        expect(first.status == MasterOperationStatus::FundingRefused && first.preparationWork == 37 &&
                   second.status == MasterOperationStatus::FundingRefused && second.preparationWork == 0 &&
                   session.operationPreparationCalls == 1 && !ledger.lastRefusal() &&
                   evaluator.refinementRefusal() && evaluator.refinementRefusal()->requested == 200,
               "checked local preparation refusal is memoized without poisoning the shared ledger");
    }

    enum class ScriptedOperationCase : std::int8_t
    {
        Redirect,
        SharedKey,
        Resolved,
        Unavailable,
        FundingPark,
    };

    constexpr MilliFunds SCRIPT_SHARED_UPPER = 200;

    MasterAssignmentProblem scriptedOperationProblem(ScriptedOperationCase test)
    {
        Fixture fixture(1);
        fixture.input.actors[0].candidates[0] = moveCandidate(0, {0, 0}, {0, 0}, 0);
        if (test == ScriptedOperationCase::SharedKey || test == ScriptedOperationCase::Unavailable)
        {
            fixture.input.actors[0].candidates.push_back(
                moveCandidate(0, {0, 0}, {0, 0}, SCRIPT_SHARED_UPPER + 1, 1));
        }
        else
        {
            for (std::int32_t choice = 1; choice <= 2; ++choice)
            {
                auto candidate = moveCandidate(0, {0, 0}, {choice, 0}, 0, choice);
                if (test == ScriptedOperationCase::Redirect)
                {
                    candidate.bundle.components.push_back(
                        captureComponent({.income = {1, 1}, .turnsUntilOwned = 0}));
                }
                fixture.input.actors[0].candidates.push_back(std::move(candidate));
            }
        }
        fixture.seat();
        fixture.selections[0].stockRow = 0;
        return fixture.extract();
    }

    // Scripted proofs isolate scheduler transitions, not native valuation correctness.
    class ScriptedOperationSession final : public RelativeStockSession
    {
    public:
        ScriptedOperationSession(const MasterAssignmentProblem & problem, const PropertyStockField & field,
                                 const PropertyStockInstance & enemy, RefinementLedger & ledger,
                                 ScriptedOperationCase test)
            : RelativeStockSession(field, enemy), m_ledger(ledger), m_test(test),
              m_incumbent(problem.incumbentContinuationKey())
        {
            std::vector<CheapActionTerms> actions;
            for (const auto & candidate : problem.actors().front().candidates)
            {
                const auto key = CanonicalPlanActionKey::fromAction(0, candidate.stockEndpoint.x,
                    candidate.stockEndpoint.y, candidate.capture.has_value());
                const ContinuationKey full{key};
                if (m_entries.contains(full))
                {
                    continue;
                }
                const auto choice = candidate.identity.originalCandidateIndex;
                const MilliFunds upper = choice == 0 ? 350 : choice == 1 ? 400 : 300;
                const auto captured = !candidate.capture ? -1 : choice == 1 ? 1 : 0;
                const auto chainColumn = captured == 0 ? 1 : 0;
                CheapRowTerms row{0, {{{{chainColumn, 1, upper}}, upper}}};
                actions.push_back({key, 0, captured, row});
                Entry entry;
                entry.ours = choice == 0 ? StockInterval{0, SCRIPT_SHARED_UPPER}
                    : choice == 1 ? StockInterval{100, 150} : StockInterval{50, 250};
                if (m_test == ScriptedOperationCase::FundingPark && choice != 0)
                {
                    entry.ours = choice == 1 ? StockInterval{100, 250} : StockInterval{50, 350};
                }
                if (captured >= 0)
                {
                    entry.captured = {captured};
                }
                m_entries.emplace(full, std::move(entry));
                if (choice == 2)
                {
                    m_focus = full;
                }
            }
            m_enemies.emplace(std::vector<std::int32_t>{}, StockInterval{0, 1000});
            m_enemies.emplace(std::vector<std::int32_t>{0}, StockInterval{0, 10});
            m_enemies.emplace(std::vector<std::int32_t>{1}, StockInterval{0, 0});
            require(m_model.install(1, 2, 0, {0, 0}, {{0, {{{{0, 1, 350}}, 350}}}},
                                    std::move(actions), 1000), "scripted scheduler bound model");
        }

        const CheapPricingModel* pricingModel() const override
        {
            return &m_model;
        }

        RelativeStockQuote quote(const TurnPlan &, MilliFunds) override
        {
            return {};
        }

        RelativeStockQuote quote(const ContinuationKey & key, MilliFunds economic) override
        {
            auto & entry = m_entries.at(key);
            ++entry.quotes;
            if ((m_test == ScriptedOperationCase::Resolved && key == m_focus && entry.quotes >= 3) ||
                (m_resolveNext && key == m_incumbent))
            {
                entry.ours = {0, 0};
                m_resolveNext = false;
            }
            return {.key = key, .capturedColumns = entry.captured, .identity = identity(),
                    .economic = economic, .ours = entry.ours, .enemy = m_enemies.at(entry.captured),
                    .ourLowerWitnessReplays = true, .enemyLowerWitnessReplays = true, .valid = true};
        }

        RelativeStockOperationMetadata operationMetadata(const RelativeStockQuote & quote) const override
        {
            ++metadataQueries;
            return {.identity = identity(), .ourRerunWork = friendlyTariff(quote.key),
                    .enemyRerunWork = SCRIPT_TARIFF, .certificateReadinessWork = SCRIPT_PREPARATION_WORK,
                    .valid = owns(quote)};
        }

        RelativeStockRefinedTerm refineOur(const ContinuationKey & key, std::int64_t allowance) override
        {
            auto & interval = m_entries.at(key).ours;
            auto result = admit(interval, allowance, friendlyTariff(key));
            if (result.admitted && m_test == ScriptedOperationCase::FundingPark &&
                key != m_incumbent && key != m_focus)
            {
                interval.lower = 210;
                result.interval = interval;
            }
            return result;
        }

        RelativeStockRefinedTerm refineEnemy(const std::vector<std::int32_t> & captured,
                                            std::int64_t allowance) override
        {
            return admit(m_enemies.at(captured), allowance, SCRIPT_TARIFF);
        }

        bool replayOurWitness(const RelativeStockQuote & quote) override
        {
            return owns(quote) && quote.ourLowerWitnessReplays;
        }

        bool replayEnemyWitness(const RelativeStockQuote & quote) override
        {
            return owns(quote) && quote.enemyLowerWitnessReplays;
        }

        std::shared_ptr<const RelativeStockProofSnapshot> captureProof(const RelativeStockQuote & quote) override
        {
            if (!owns(quote))
            {
                return {};
            }
            auto proof = std::make_shared<RelativeStockProofSnapshot>();
            proof->quote = quote;
            return proof;
        }

        bool replayLowerProof(const RelativeStockProofSnapshot & proof) const override
        {
            return owns(proof.quote) && proof.quote.valid;
        }

        std::int64_t remainingWork() const override
        {
            return m_ledger.balance();
        }

        std::int64_t ourRerunWork() const override
        {
            return friendlyTariff(m_incumbent);
        }

        std::int64_t enemyRerunWork(const std::vector<std::int32_t> &) const override
        {
            return SCRIPT_TARIFF;
        }

        bool supportsVerifiedDual() const override
        {
            return m_test == ScriptedOperationCase::Redirect;
        }

        RelativeStockOperationPreparation prepareOurCertificateOperation(const RelativeStockQuote &) override
        {
            ++preparationCalls;
            if (!m_ledger.consume(SCRIPT_PREPARATION_WORK, RefinementWork::MasterValuation))
            {
                return {.status = RelativeStockPreparationStatus::BudgetExhausted,
                        .requiredPreparationWork = SCRIPT_PREPARATION_WORK};
            }
            return {.status = RelativeStockPreparationStatus::Ready, .identity = identity(),
                    .paidWork = SCRIPT_PREPARATION_WORK, .readinessChecked = true};
        }

        RelativeStockDualTightening tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote, std::int64_t allowance) override
        {
            const auto result = admit(quote.ours, allowance, 0);
            return {.intervalBefore = quote.ours, .intervalAfter = quote.ours,
                    .totalWork = result.workUsed, .globalFailure = !result.valid, .admitted = result.admitted};
        }

        void resolveNextFriendlyQuote()
        {
            m_resolveNext = true;
        }

        mutable std::uint64_t metadataQueries{0};
        std::uint64_t nativeCalls{0};
        std::uint64_t preparationCalls{0};

    private:
        std::int64_t friendlyTariff(const ContinuationKey & key) const
        {
            if (m_test == ScriptedOperationCase::FundingPark && nativeCalls >= 2 &&
                (key == m_incumbent || key == m_focus))
            {
                return SCRIPT_UNFUNDED_WORK;
            }
            return m_test == ScriptedOperationCase::Redirect && key == m_incumbent
                ? SCRIPT_BOOTSTRAP_WORK : SCRIPT_TARIFF;
        }

        RelativeStockRefinedTerm admit(const StockInterval & interval, std::int64_t allowance, std::int64_t tariff)
        {
            auto required = allowance;
            if (allowance <= 0 || !RelativeStockDetail::checkedAdd(required, tariff))
            {
                return {};
            }
            const auto grant = m_ledger.draw(required, RefinementWork::SearchSlice);
            if (grant == NO_GRANT)
            {
                return {.budgetExhausted = true};
            }
            require(m_ledger.refund(grant, allowance - 1), "scripted admitted slice refunds unused capacity");
            ++nativeCalls;
            return {.interval = interval, .statesUsed = 1, .workUsed = tariff + 1,
                    .lowerWitnessReplays = true, .valid = true, .admitted = true};
        }

        struct Entry
        {
            StockInterval ours;
            std::vector<std::int32_t> captured;
            std::uint64_t quotes{0};
        };

        static constexpr std::int64_t SCRIPT_UNFUNDED_WORK = 2000000;
        static constexpr std::int64_t SCRIPT_TARIFF = 7;
        static constexpr std::int64_t SCRIPT_BOOTSTRAP_WORK = 10000;
        static constexpr std::int64_t SCRIPT_PREPARATION_WORK = 5000;
        RefinementLedger & m_ledger;
        ScriptedOperationCase m_test;
        ContinuationKey m_incumbent;
        ContinuationKey m_focus;
        CheapPricingModel m_model;
        std::map<ContinuationKey, Entry> m_entries;
        std::map<std::vector<std::int32_t>, StockInterval> m_enemies;
        bool m_resolveNext{false};
    };

    class FundingParkingObserver final : public MasterAssignmentSearchObserver
    {
    public:
        void evaluated(const MasterAssignmentEvaluation &, std::int64_t) override {}
        void refined(const MasterAssignmentEvaluation &, MasterRefinementTerm, std::int64_t,
                     MasterRefinementStatus, std::int64_t, std::int64_t) override {}
        void compared(const MasterAssignmentEvaluation &, const RelativeStockComparison &, std::int64_t) override {}

        void operationExecuted(const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm,
                               const MasterOperationOutcome & outcome, std::int64_t, std::int64_t) override
        {
            if (parkedOnIncumbent && outcome.admitted && evaluation.state.choices == std::vector<std::int32_t>{1})
            {
                fundedAfterParking = true;
            }
        }

        void ticketFinished(const MasterRefinementTicketReceipt & ticket) override
        {
            if (ticket.fundingParked)
            {
                ++parked;
                parkedOnIncumbent = parkedOnIncumbent ||
                    ticket.ticket.state.choices == std::vector<std::int32_t>{0};
                expect(ticket.completed && ticket.refinementsAfter == ticket.refinementsBefore + 1,
                       "parking completes one ticket's bookkeeping");
            }
        }

        std::uint64_t parked{0};
        bool parkedOnIncumbent{false};
        bool fundedAfterParking{false};
    };

    void operationFundingParking()
    {
        const auto problem = scriptedOperationProblem(ScriptedOperationCase::FundingPark);
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "parking scheduler ledger");
        ScriptedOperationSession session(problem, field, enemy, ledger, ScriptedOperationCase::FundingPark);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {1, 1, 1});
        FundingParkingObserver observer;
        MasterAssignmentSearchOptions options;
        options.schedule.initialIncumbentAlternatives = true;
        options.schedule.allocation = MasterRefinementAllocation::OperationService;
        options.retainDiagnostics = true;
        options.observer = &observer;
        const auto result = searchMasterAssignment(problem, evaluator, ledger, options);
        expect(result.improvement && result.improvement->challenger.state.choices == std::vector<std::int32_t>{1} &&
                   observer.parkedOnIncumbent && observer.fundedAfterParking,
               "an unfundable focused comparison does not suppress a different affordable improvement");
        expect(observer.parked == result.stats.fundingParkedTickets && observer.parked > 0 &&
                   !result.operationStop && !result.budgetExhausted && ledger.stats().refusals == 0 && ledger.balance() > 0,
               "local parking keeps its own receipt and never fabricates a global budget stop");
        expect(!result.globalOptimal && result.stats.residualRegions > 0 &&
                   std::ranges::any_of(result.residual, [](const auto & region) {
                       return region.state.choices == std::vector<std::int32_t>{2};
                   }),
               "the parked challenger and its unresolved upper bound remain in the residual domain");
    }

    class ScriptedTicketObserver final : public MasterAssignmentSearchObserver
    {
    public:
        ScriptedTicketObserver(RefinementLedger & ledger, ScriptedOperationSession & session,
                               const MasterAssignmentSchedule & schedule, ScriptedOperationCase test)
            : m_ledger(ledger), m_session(session), m_schedule(schedule), m_test(test)
        {
        }

        void evaluated(const MasterAssignmentEvaluation & evaluation, std::int64_t) override
        {
            trackBest(evaluation);
        }

        void refined(const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm, std::int64_t,
                     MasterRefinementStatus, std::int64_t, std::int64_t) override
        {
            trackBest(evaluation);
        }

        void compared(const MasterAssignmentEvaluation & evaluation, const RelativeStockComparison & comparison,
                      std::int64_t) override
        {
            trackBest(evaluation);
            m_lastCompared = evaluation.state.choices;
            m_lastComparisonResolved = comparison.valid() && !comparison.unresolved();
            if (comparison.improves() && ++m_positiveComparisons == 2 && m_test == ScriptedOperationCase::Unavailable)
            {
                m_session.resolveNextFriendlyQuote();
            }
            if (m_test == ScriptedOperationCase::Resolved && !m_cut &&
                evaluation.state.choices == std::vector<std::int32_t>{2} &&
                m_lastComparisonResolved)
            {
                m_cut = true;
                m_metadataAtResolution = m_session.metadataQueries;
                m_nativeAtResolution = m_session.nativeCalls;
                require(m_ledger.consume(m_ledger.balance(), RefinementWork::MasterPreparation),
                        "scripted cutoff leaves no funds for an operation offer");
            }
        }

        void operationServiced(const MasterOperationTransaction & transaction) override
        {
            ++transactions;
            m_pending = Transaction{transaction.ticket.state.choices, transaction.ticketWork, transaction.admitted};
            expect(transaction.operationWork >= 0 && transaction.operationWork <= transaction.ticketWork,
                   "scripted physical service is contained in its ticket expenditure");
            if (transaction.challenger)
            {
                expect(m_lastCompared == transaction.challenger->state.choices,
                       "scripted handoff always recompares its exact challenger, never the current best");
            }
            const auto & quote = *transaction.operation->quote();
            const auto identity = std::pair{transaction.term,
                transaction.term == MasterRefinementTerm::Enemy ? enemyPayload(quote.capturedColumns)
                                                               : keyPayload(quote.key)};
            auto [known, inserted] = m_grants.try_emplace(identity,
                transaction.term == MasterRefinementTerm::FutureDual ? m_schedule.futureDualAllowance
                                                                    : m_schedule.continuationSlice);
            expect(transaction.allowance == known->second,
                   "scripted canonical grant persists across tickets and current-plan aliases");
            if (transaction.admitted && !transaction.completed &&
                known->second <= std::numeric_limits<std::int64_t>::max() / MASTER_OPERATION_GROWTH_FACTOR)
            {
                known->second *= MASTER_OPERATION_GROWTH_FACTOR;
            }
            if (!transaction.admitted)
            {
                ++unadmitted;
                expect(transaction.operationWork == 0, "freshly unavailable operation earns no service credit");
            }
            if (transaction.ticket.state.choices == std::vector<std::int32_t>{2} &&
                transaction.operation->state.choices == std::vector<std::int32_t>{0})
            {
                ++redirects;
                if (m_best == std::vector<std::int32_t>{1})
                {
                    ++nonBestRedirects;
                }
                expect(transaction.term != MasterRefinementTerm::Enemy,
                       "a pair never redirects service to its cancelled enemy term");
            }
            if (m_session.preparationCalls > m_seenPreparations)
            {
                if (m_seenPreparations == 0 && m_positiveComparisons == 0)
                {
                    expect(transaction.term != MasterRefinementTerm::FutureDual,
                           "first pair readiness payment raises debt and defers its ready operation");
                    ++paidPreparationDeferrals;
                }
                m_seenPreparations = m_session.preparationCalls;
            }
            if (transaction.operation->state.choices == std::vector<std::int32_t>{1} &&
                transaction.term == MasterRefinementTerm::Friendly &&
                transaction.allowance > m_schedule.continuationSlice)
            {
                ++sharedKeyHandoffs;
            }
        }

        void ticketFinished(const MasterRefinementTicketReceipt & ticket) override
        {
            ++tickets;
            auto & previous = m_tickets[ticket.ticket.state.choices];
            expect(ticket.cursorBefore == previous.cursor && ticket.aggregateBefore == previous.aggregate &&
                       ticket.refinementsBefore == m_refinements &&
                       ticket.aggregateAfter >= ticket.aggregateBefore,
                   "ticket cursor and aggregate belong to the ticket, not its physical recipient");
            const auto advanced = ticket.refinementsAfter - ticket.refinementsBefore;
            expect(advanced <= 1 && ticket.cursorAfter == (advanced == 1 ? next(ticket.cursorBefore)
                                                                        : ticket.cursorBefore),
                   "each ticket advances at most one cursor and one cadence count");
            if (ticket.ticketWasFocus && advanced == 1 && !m_lastComparisonResolved && !ticket.fundingParked)
            {
                expect(ticket.focusBefore > 0 && ticket.focusAfter == ticket.focusBefore - 1,
                       "redirected physical work ages exactly one focused ticket");
                ++agedFocus;
            }
            if (m_pending)
            {
                expect(m_pending->ticket == ticket.ticket.state.choices &&
                           ticket.aggregateAfter - ticket.aggregateBefore == m_pending->ticketWork,
                       "aggregate ticket debt equals its complete single-ledger transaction");
                if (ticket.workBalancedSelection)
                {
                    expect(ticket.refinementsBefore % m_schedule.incumbentRefinementStride == 0 &&
                               ticket.ticket.state.choices == std::vector<std::int32_t>{2} &&
                               ticket.ticketWasFocus &&
                               m_tickets[std::vector<std::int32_t>{0}].aggregate > ticket.aggregateBefore,
                           "actual work-balanced override obeys its stride, focus and aggregate-work guard");
                    ++balancedTickets;
                }
                if (ticket.proofBefore && m_pending->admitted)
                {
                    ++postProofAdmissions;
                }
                if (ticket.proofBefore && !m_pending->admitted)
                {
                    ++postProofUnadmitted;
                }
            }
            if (m_cut && ticket.ticket.state.choices == std::vector<std::int32_t>{2})
            {
                expect(ticket.completed && advanced == 1 && !m_pending && m_ledger.balance() == 0 &&
                           m_session.metadataQueries == m_metadataAtResolution &&
                           m_session.nativeCalls == m_nativeAtResolution,
                       "resolved low-funding ticket completes without screening or native admission");
                ++resolvedCutoffs;
            }
            previous = {ticket.cursorAfter, ticket.aggregateAfter};
            m_refinements = ticket.refinementsAfter;
            m_pending.reset();
        }

        std::uint64_t tickets{0};
        std::uint64_t transactions{0};
        std::uint64_t redirects{0};
        std::uint64_t nonBestRedirects{0};
        std::uint64_t sharedKeyHandoffs{0};
        std::uint64_t postProofAdmissions{0};
        std::uint64_t postProofUnadmitted{0};
        std::uint64_t balancedTickets{0};
        std::uint64_t agedFocus{0};
        std::uint64_t unadmitted{0};
        std::uint64_t resolvedCutoffs{0};
        std::uint64_t paidPreparationDeferrals{0};

    private:
        void trackBest(const MasterAssignmentEvaluation & evaluation)
        {
            if (evaluation.objective && (!m_bestLower || evaluation.objective->lower > *m_bestLower))
            {
                m_bestLower = evaluation.objective->lower;
                m_best = evaluation.state.choices;
            }
        }

        static MasterRefinementTerm next(MasterRefinementTerm term)
        {
            if (term == MasterRefinementTerm::Friendly)
            {
                return MasterRefinementTerm::Enemy;
            }
            return term == MasterRefinementTerm::Enemy ? MasterRefinementTerm::FutureDual
                                                      : MasterRefinementTerm::Friendly;
        }

        static std::vector<std::int32_t> enemyPayload(const std::vector<std::int32_t> & captured)
        {
            return captured;
        }

        static std::vector<std::int32_t> keyPayload(const ContinuationKey & key)
        {
            std::vector<std::int32_t> result;
            for (const auto & action : key)
            {
                result.insert(result.end(), {action.row(), action.x(), action.y(), action.captures() ? 1 : 0});
            }
            return result;
        }

        struct TicketState
        {
            MasterRefinementTerm cursor{MasterRefinementTerm::Friendly};
            std::int64_t aggregate{0};
        };

        struct Transaction
        {
            std::vector<std::int32_t> ticket;
            std::int64_t ticketWork;
            bool admitted;
        };

        RefinementLedger & m_ledger;
        ScriptedOperationSession & m_session;
        MasterAssignmentSchedule m_schedule;
        ScriptedOperationCase m_test;
        bool m_cut{false};
        bool m_lastComparisonResolved{false};
        std::uint64_t m_metadataAtResolution{0};
        std::uint64_t m_nativeAtResolution{0};
        std::uint64_t m_refinements{0};
        std::uint64_t m_positiveComparisons{0};
        std::uint64_t m_seenPreparations{0};
        std::optional<MilliFunds> m_bestLower;
        std::vector<std::int32_t> m_best;
        std::vector<std::int32_t> m_lastCompared;
        std::optional<Transaction> m_pending;
        std::map<std::vector<std::int32_t>, TicketState> m_tickets;
        std::map<std::pair<MasterRefinementTerm, std::vector<std::int32_t>>, std::int64_t> m_grants;
    };

    void scriptedOperationHandoffs()
    {
        for (const auto test : {ScriptedOperationCase::Redirect, ScriptedOperationCase::SharedKey,
                               ScriptedOperationCase::Resolved, ScriptedOperationCase::Unavailable})
        {
            const auto problem = scriptedOperationProblem(test);
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(1000000), "scripted scheduler ledger");
            ScriptedOperationSession session(problem, field, enemy, ledger, test);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {1, 1, 1});
            const MasterAssignmentSchedule schedule{
                .nodesPerRefinement = test == ScriptedOperationCase::SharedKey ||
                    test == ScriptedOperationCase::Unavailable ? 1u : 2u,
                .challengerRefinementBurst = 4,
                .allocation = MasterRefinementAllocation::OperationService};
            ScriptedTicketObserver observer(ledger, session, schedule, test);
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = schedule, .retainDiagnostics = true, .observer = &observer});
            std::printf("Scripted operation case %d: tickets=%llu redirects=%llu nonBest=%llu balanced=%llu "
                        "shared=%llu postProof=%llu unavailable=%llu resolved=%llu paidDeferrals=%llu\n",
                        static_cast<int>(test), static_cast<unsigned long long>(observer.tickets),
                        static_cast<unsigned long long>(observer.redirects),
                        static_cast<unsigned long long>(observer.nonBestRedirects),
                        static_cast<unsigned long long>(observer.balancedTickets),
                        static_cast<unsigned long long>(observer.sharedKeyHandoffs),
                        static_cast<unsigned long long>(observer.postProofAdmissions),
                        static_cast<unsigned long long>(observer.postProofUnadmitted),
                        static_cast<unsigned long long>(observer.resolvedCutoffs),
                        static_cast<unsigned long long>(observer.paidPreparationDeferrals));
            expect(!result.invalidInput && result.stats.proofFailures == 0 && observer.tickets > 0,
                   "scripted scheduler fixture reaches valid operation tickets");
            if (test == ScriptedOperationCase::Redirect)
            {
                expect(observer.redirects > 0 && observer.nonBestRedirects > 0 &&
                           observer.balancedTickets > 0 && observer.agedFocus > 0 &&
                           observer.paidPreparationDeferrals > 0,
                       "script forces incoming service on focused non-best challenger's balanced ticket");
            }
            else if (test == ScriptedOperationCase::SharedKey)
            {
                expect(result.improvement && observer.sharedKeyHandoffs > 0 && observer.postProofAdmissions > 0,
                       "bootstrap key grant is reused by a different complete tuple after the first proof");
            }
            else if (test == ScriptedOperationCase::Resolved)
            {
                expect(observer.resolvedCutoffs == 1 && result.stats.resolvedOperationTickets == 1,
                       "resolved comparison is not reclassified as an unfundable operation");
            }
            else
            {
                expect(observer.unadmitted > 0 && observer.postProofUnadmitted > 0,
                       "post-proof offered work becoming unavailable earns no admission or service");
            }
        }
    }

    class OperationTransactionObserver final : public MasterAssignmentSearchObserver
    {
    public:
        void evaluated(const MasterAssignmentEvaluation &, std::int64_t) override
        {
        }

        void refined(const MasterAssignmentEvaluation &, MasterRefinementTerm, std::int64_t,
                     MasterRefinementStatus, std::int64_t, std::int64_t) override
        {
        }

        void compared(const MasterAssignmentEvaluation & challenger, const RelativeStockComparison &,
                      std::int64_t) override
        {
            lastCompared = challenger.state.choices;
        }

        void operationServiced(const MasterOperationTransaction & transaction) override
        {
            ++transactions;
            expect(transaction.ticketWork >= transaction.operationWork && transaction.operationWork >= 0 &&
                       transaction.operation != nullptr && transaction.operation->lowerReplayed,
                   "ticket and physical service are two views of one funded transaction");
            if (transaction.challenger != nullptr)
            {
                expect(lastCompared == transaction.challenger->state.choices,
                       "servicing incoming work rechecks the exact ticket challenger");
            }
        }

        std::uint64_t transactions{0};
        std::vector<std::int32_t> lastCompared;
    };

    void operationSearchOracle()
    {
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        auto reference = request();
        reference.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        reference.continuation.cases = cases;
        const auto exact = MasterAssignmentOracle::enumerate(problem, reference);
        require(exact.exactOptimum.has_value(), "operation fixture has an independent exact optimum");
        QJsonObject first;
        for (const bool observe : {false, true})
        {
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(2000000), "operation search ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            session.operationServiceEnabled = true;
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {11, 17, 23});
            OperationTransactionObserver observer;
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = {.nodesPerRefinement = 1, .allocation = MasterRefinementAllocation::OperationService},
                 .retainDiagnostics = true, .observer = observe ? &observer : nullptr});
            expect(result.lower && result.upper && *result.lower <= *exact.exactOptimum &&
                       *result.upper >= *exact.exactOptimum && result.domainAccounted &&
                       result.stats.proofFailures == 0 && result.incumbent && result.incumbent->lowerReplayed &&
                       result.stats.operationPaidWork >= 0 && result.stats.operationScreeningWork >= 0 &&
                       result.stats.operationPaidWork + result.stats.operationScreeningWork <= ledger.stats().spent(),
                   "operation allocation preserves oracle bounds, owned incumbent and one work ledger");
            const auto record = searchRecord(result);
            if (observe)
            {
                expect(observer.transactions > 0 && record == first,
                       "operation observations preserve deterministic search proofs and work");
            }
            else
            {
                first = record;
            }
        }
    }

    QJsonArray readOperationTrace(const QString & path)
    {
        QFile input(path);
        require(input.open(QIODevice::ReadOnly), "actual operation trace file opens");
        const auto bytes = input.readAll();
        require(bytes.endsWith('\n'), "operation trace has complete line framing");
        auto lines = bytes.split('\n');
        lines.removeLast();
        QJsonArray records;
        for (const auto & line : lines)
        {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            require(error.error == QJsonParseError::NoError && document.isObject(),
                    "every operation trace line is a JSON object");
            records.append(document.object());
        }
        return records;
    }

    void checkOperationTrace(const QJsonArray & records)
    {
        require(records.size() > 2 && records.first().toObject()["event"] == "begin" &&
                    records.last().toObject()["event"] == "end",
                "trace has begin and terminal outcome frames");
        std::uint64_t tickets = 0;
        std::uint64_t completedTickets = 0;
        std::uint64_t offers = 0;
        std::uint64_t executed = 0;
        std::uint64_t serviced = 0;
        bool active = false;
        QJsonArray ticketChoices;
        QJsonArray operationChoices;
        for (qsizetype index = 0; index < records.size(); ++index)
        {
            const auto record = records[index].toObject();
            expect(record["sequence"].toString().toLongLong() == index + 1 &&
                       record["identity"].toObject()["planSequence"] == "77",
                   "trace sequence and plan identity are complete");
            const auto event = record["event"].toString();
            if (event == "ticketStarted")
            {
                expect(!active, "ticket start is not nested");
                active = true;
                ++tickets;
                ticketChoices = record["ticket"].toObject()["choices"].toArray();
            }
            if (active)
            {
                expect(record["ticketSequence"].toString().toULongLong() == tickets,
                       "all operation events belong to the current selected ticket");
            }
            if (event == "offered")
            {
                ++offers;
                const auto offer = record["offer"].toObject();
                const auto paid = offer["inspectionWork"].toString().toLongLong() +
                    offer["preparationWork"].toString().toLongLong() + offer["quotaWork"].toString().toLongLong();
                expect(record["workAfter"].toString().toLongLong() -
                           record["workBefore"].toString().toLongLong() == paid,
                       "actual offer writer distinguishes inspection, preparation and quota charges");
                expect(record.contains("lowerRelevant") && record.contains("upperRelevant") &&
                           offer["preparation"].toObject().contains("readinessChecked"),
                       "offer output preserves endpoint direction and preparation readiness");
            }
            if (event == "operationStarted")
            {
                operationChoices = record["operation"].toObject()["choices"].toArray();
            }
            if (event == "executed")
            {
                ++executed;
                expect(record["operation"].toObject()["choices"].toArray() == operationChoices &&
                           record["executionWork"].toString().toLongLong() ==
                               record["workAfter"].toString().toLongLong() -
                                   record["workBefore"].toString().toLongLong(),
                       "native outcome is linked to the physical operation and exact transaction work");
                if (record["nativeBefore"].isArray() && record["nativeAfter"].isArray())
                {
                    const auto before = record["nativeBefore"].toArray();
                    const auto after = record["nativeAfter"].toArray();
                    expect(record["lowerProgress"].toBool() ==
                               (after[0].toString().toLongLong() > before[0].toString().toLongLong()) &&
                               record["upperProgress"].toBool() ==
                               (after[1].toString().toLongLong() < before[1].toString().toLongLong()),
                           "native endpoint snapshots match the recorded progress direction");
                }
            }
            if (event == "serviced")
            {
                ++serviced;
                expect(record["ticket"].toObject()["choices"].toArray() == ticketChoices &&
                           record["operation"].toObject()["choices"].toArray() == operationChoices,
                       "service preserves the selected ticket and actual operation identities");
            }
            if (event == "ticketFinished")
            {
                expect(active && record["ticket"].toObject()["choices"].toArray() == ticketChoices,
                       "ticket completion matches its immutable start identity");
                ++completedTickets;
                active = false;
            }
        }
        const auto end = records.last().toObject();
        expect(!active && tickets > 0 && completedTickets == tickets && offers > 0 && executed > 0 && serviced > 0 &&
                   end["eventCount"].toString().toLongLong() == records.size() &&
                   end["ticketCount"].toString().toULongLong() == tickets,
               "actual writer includes operation, ticket and terminal framing without a missing tail");
    }

    void operationTraceWriter()
    {
        const auto oldEnvironment = qgetenv(MASTER_OPERATION_TRACE_ENV);
        const bool wasSet = qEnvironmentVariableIsSet(MASTER_OPERATION_TRACE_ENV);
        QTemporaryDir output;
        require(output.isValid(), "isolated actual-writer directory");
        const auto problem = operationProblem();
        const auto cases = operationCases(problem);
        QJsonObject unobserved;
        std::array<std::uint64_t, 6> unobservedCalls{};
        for (std::int32_t mode = 0; mode < 3; ++mode)
        {
            if (mode == 0)
            {
                qunsetenv(MASTER_OPERATION_TRACE_ENV);
            }
            else
            {
                qputenv(MASTER_OPERATION_TRACE_ENV, "1");
            }
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(2000000), "actual writer comparison ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            session.operationServiceEnabled = true;
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {11, 17, 23});
            MasterShadowConfig config{.mode = MasterAssignmentMode::StockControl,
                .runtime = MasterStockControlRuntime::PlayableTrial, .allowance = 2000000,
                .outputPath = output.path() + (mode == 2 ? "/missing/control.jsonl" : "/control.jsonl")};
            const DecisionTraceIdentity identity{4, 0, 77, 10};
            MasterAssignmentSearchOptions options{
                .schedule = {.nodesPerRefinement = 1, .allocation = MasterRefinementAllocation::OperationService},
                .retainDiagnostics = true};
            auto trace = attachMasterStockTrace(config, identity, problem, ledger, 0, options);
            expect((trace != nullptr) == (mode != 0) &&
                       options.observer == trace.get(), "native attachment helper obeys explicit diagnostic opt-in");
            const auto result = searchMasterAssignment(problem, evaluator, ledger, options);
            const auto record = searchRecord(result);
            const std::array calls{session.nativeRefinementCalls, session.certificateCalls,
                session.operationPreparationCalls, session.readinessCalls, session.proofCaptures,
                session.verifiedCertificates};
            if (trace)
            {
                trace->finish({{"search", record}});
                expect(trace->complete() == (mode == 1), "writer failure is diagnostic failure, not silent success");
                if (mode == 1)
                {
                    checkOperationTrace(readOperationTrace(trace->path()));
                }
            }
            if (mode == 0)
            {
                unobserved = record;
                unobservedCalls = calls;
            }
            else
            {
                expect(record == unobserved && calls == unobservedCalls,
                       "actual writer and failed writer preserve decisions, proofs, charges and native calls");
            }
        }
        QJsonObject unobservedScript;
        std::array<std::uint64_t, 3> unobservedScriptCalls{};
        for (const bool observe : {false, true})
        {
            const auto scripted = scriptedOperationProblem(ScriptedOperationCase::Redirect);
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(1000000), "paid-preparation writer ledger");
            ScriptedOperationSession session(scripted, field, enemy, ledger, ScriptedOperationCase::Redirect);
            MasterAssignmentEvaluator evaluator(scripted, ledger, session, {1, 1, 1});
            MasterAssignmentSearchOptions options{
                .schedule = {.nodesPerRefinement = 2, .challengerRefinementBurst = 4,
                             .allocation = MasterRefinementAllocation::OperationService},
                .retainDiagnostics = true};
            std::unique_ptr<MasterAssignmentSearchTrace> trace;
            if (observe)
            {
                trace = std::make_unique<MasterAssignmentSearchTrace>(
                    output.path() + "/preparation.jsonl", DecisionTraceIdentity{4, 0, 77, 10}, scripted, ledger, 0);
                options.observer = trace.get();
            }
            const auto result = searchMasterAssignment(scripted, evaluator, ledger, options);
            const auto record = searchRecord(result);
            const std::array calls{session.metadataQueries, session.nativeCalls, session.preparationCalls};
            if (!observe)
            {
                unobservedScript = record;
                unobservedScriptCalls = calls;
                continue;
            }
            trace->finish({{"search", record}});
            const auto records = readOperationTrace(trace->path());
            checkOperationTrace(records);
            std::uint64_t paidPreparation = 0;
            std::uint64_t admittedWithoutProgress = 0;
            for (const auto & value : records)
            {
                const auto event = value.toObject();
                paidPreparation += event["event"] == "offered" &&
                    event["offer"].toObject()["preparationWork"].toString().toLongLong() > 0;
                admittedWithoutProgress += event["event"] == "executed" && event["admitted"].toBool() &&
                    !event["lowerProgress"].toBool() && !event["upperProgress"].toBool() &&
                    event["nativeWork"].toString().toLongLong() > 0;
            }
            expect(trace->complete() && record == unobservedScript && calls == unobservedScriptCalls &&
                       paidPreparation > 0 && admittedWithoutProgress > 0,
                   "actual writer distinguishes paid preparation from admitted work without endpoint progress");
        }
        RefinementLedger ledger;
        require(ledger.open(1000), "snapshot writer ledger");
        MasterAssignmentSearchTrace snapshots(output.path() + "/snapshots.jsonl", {4, 0, 77, 10}, problem, ledger, 0);
        MasterAssignmentEvaluation mutableEvaluation;
        mutableEvaluation.state.choices = {5};
        mutableEvaluation.objective = StockInterval{7, 11};
        snapshots.evaluated(mutableEvaluation, 0);
        mutableEvaluation.state.choices = {9};
        mutableEvaluation.objective = StockInterval{17, 21};
        snapshots.evaluated(mutableEvaluation, 0);
        snapshots.finish({});
        const auto recorded = readOperationTrace(snapshots.path());
        expect(snapshots.complete() && recorded[1].toObject()["evaluation"].toObject()["lower"] == "7" &&
                   recorded[1].toObject()["evaluation"].toObject()["choices"].toArray() == QJsonArray{5} &&
                   recorded[2].toObject()["evaluation"].toObject()["lower"] == "17" && ledger.stats().spent() == 0,
               "writer serializes each callback boundary without retaining mutable evaluation references");
        MasterAssignmentSearchTrace nextFrame(
            snapshots.path(), {4, 0, 78, 10}, problem, ledger, 0);
        nextFrame.finish({});
        const auto appended = readOperationTrace(snapshots.path());
        expect(nextFrame.complete() && appended.size() == recorded.size() + 2 &&
                   appended[recorded.size()].toObject()["sequence"] == "1" &&
                   appended[recorded.size()].toObject()["identity"].toObject()["planSequence"] == "78",
               "multiple native plans append distinct complete trace frames");
        MasterShadowConfig shadow{.mode = MasterAssignmentMode::Shadow};
        MasterAssignmentSearchOptions options;
        expect(!attachMasterStockTrace(shadow, {4, 0, 77, 10}, problem, ledger, 0, options) &&
                   options.observer == nullptr, "Shadow never attaches the StockControl diagnostic observer");
        shadow.mode = MasterAssignmentMode::StockControl;
        shadow.exportMode = MasterShadowExportMode::Off;
        expect(!attachMasterStockTrace(shadow, {4, 0, 77, 10}, problem, ledger, 0, options),
               "export-off StockControl remains unobserved");
        if (wasSet)
        {
            qputenv(MASTER_OPERATION_TRACE_ENV, oldEnvironment);
        }
        else
        {
            qunsetenv(MASTER_OPERATION_TRACE_ENV);
        }
    }

    void nativeCollectionTrace()
    {
        Fixture fixture(1);
        fixture.seat();
        const auto problem = fixture.extract();
        RefinementLedger ledger;
        require(ledger.open(60000000), "native collection trace uses an independent diagnostic ledger");
        MasterNativeSourcePreparation source;
        source.status = MasterNativeSourceStatus::BudgetExhausted;
        source.work = 79;
        const auto actorStage = static_cast<std::size_t>(MasterNativeCollectionStage::Actors);
        source.collectionCosts[actorStage] = {3, 79, 31, 11};
        const auto workBefore = masterWorkRecord(ledger.stats());
        const auto nativeRecord = masterNativeSourceWorkRecord(source);
        const auto recorded = nativeRecord["nativeCollectionCosts"].toArray();
        expect(nativeRecord["nativeSourceWork"].toString() == QStringLiteral("79") &&
               nativeRecord["nativePreparationMaximumWork"].isNull() &&
               nativeRecord["nativeAuthenticationMaximumWork"].isNull(),
               "unready source records retain partial work without inventing funded quotes");
        source.status = MasterNativeSourceStatus::Ready;
        source.collectionWork = 57;
        source.expectedConstructionWork = 31;
        source.receiverConstructionWork = 11;
        source.commonCollectionWork = 15;
        source.boundModules = 3;
        source.preparationMaximumWork = std::numeric_limits<std::int64_t>::max();
        source.authenticationMaximumWork = 1356672;
        const auto readyRecord = masterNativeSourceWorkRecord(source);
        expect(readyRecord["nativeCollectionWork"].toString() == QStringLiteral("57") &&
               readyRecord["nativeExpectedConstructionWork"].toString() == QStringLiteral("31") &&
               readyRecord["nativeReceiverConstructionWork"].toString() == QStringLiteral("11") &&
               readyRecord["nativeCommonCollectionWork"].toString() == QStringLiteral("15") &&
               readyRecord["nativeBoundModules"].toInt() == 3 &&
               readyRecord["nativePreparationMaximumWork"].toString() ==
                   QString::number(std::numeric_limits<std::int64_t>::max()) &&
               readyRecord["nativeAuthenticationMaximumWork"].toString() == QStringLiteral("1356672") &&
               readyRecord["nativeCollectionCosts"].toArray() == recorded,
               "ready source serialization preserves owned quotes and exact integer identity");
        source.preparationMaximumWork = 0;
        source.authenticationMaximumWork.reset();
        source.collectionCosts[actorStage] = {4, 97, 41, 13};
        expect(recorded.size() == static_cast<int>(MasterNativeCollectionStage::Count) &&
               masterWorkRecord(ledger.stats()) == workBefore,
               "collection serialization preserves every canonical stage without ledger work");
        for (qsizetype index = 0; index < recorded.size(); ++index)
        {
            const auto row = recorded[index].toObject();
            expect(row["stage"].toInt() == index &&
                   row["observations"].toString() == QString::number(index == actorStage ? 3 : 0) &&
                   row["work"].toString() == QString::number(index == actorStage ? 79 : 0) &&
                   row["expectedConstructionWork"].toString() == QString::number(index == actorStage ? 31 : 0) &&
                   row["receiverConstructionWork"].toString() == QString::number(index == actorStage ? 11 : 0),
                   "collection records own callback-time values and distinguish unobserved stages");
        }
        QTemporaryDir directory;
        require(directory.isValid(), "native collection trace owns its output directory");
        DecisionTraceIdentity identity;
        MasterAssignmentSearchTrace trace(directory.filePath("collection.jsonl"), identity, problem, ledger, 0);
        trace.finish(readyRecord);
        const auto frames = readOperationTrace(trace.path());
        const auto outcome = frames.last().toObject()["outcome"].toObject();
        expect(trace.complete() && outcome["nativeCollectionCosts"].toArray() == recorded &&
               outcome == readyRecord &&
               outcome["nativePreparationMaximumWork"].toString() ==
                   QString::number(std::numeric_limits<std::int64_t>::max()) &&
               outcome["nativeAuthenticationMaximumWork"].toString() == QStringLiteral("1356672") &&
               masterWorkRecord(ledger.stats()) == workBefore,
               "actual trace framing owns quote snapshots without later mutations or ledger work");
        MasterNativeSourcePreparation empty;
        const auto emptyRecord = masterNativeCollectionCostsRecord(empty);
        expect(emptyRecord.size() == recorded.size() &&
               emptyRecord[actorStage].toObject()["observations"].toString() == QStringLiteral("0"),
               "an unavailable collection remains explicitly unobserved rather than omitted");
    }

    void ownedCancellationEligibility()
    {
        struct ComparisonCase
        {
            MilliFunds incumbent;
            MilliFunds challenger;
            bool valid;
            bool unresolved;
        };
        constexpr auto extreme = std::numeric_limits<MilliFunds>::max() / 2 + 10000;
        const std::array<ComparisonCase, 4> comparisons{{
            {0, 0, true, true}, {0, 10000, true, false},
            {0, -10000, true, false}, {-extreme, extreme, false, false}}};
        for (const auto & comparison : comparisons)
        {
            Fixture fixture(3);
            fixture.input.actors[0].candidates[0] =
                moveCandidate(0, {0, 0}, {0, 0}, comparison.incumbent, 0);
            for (std::int32_t actor = 0; actor < 3; ++actor)
            {
                fixture.input.actors[actor].candidates.push_back(
                    moveCandidate(actor, {actor, 0}, {actor, 1}, actor == 0 ? comparison.challenger : 0, 1));
            }
            fixture.seat();
            for (std::int32_t actor = 0; actor < 3; ++actor)
            {
                fixture.selections[actor].stockRow = actor;
            }
            const auto problem = fixture.extract();
            std::vector<MasterAssignmentOracle::ContinuationCase> cases;
            for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
            {
                cases.push_back(continuationCase(tuple.reference.key));
            }
            PropertyStockField field;
            PropertyStockInstance enemy;
            RefinementLedger ledger;
            require(ledger.open(1000000), "owned cancellation ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            auto incumbent = evaluator.evaluate({problem.identity(), {0, 0, 0}});
            auto challenger = evaluator.evaluate({problem.identity(), {1, 1, 1}});
            const auto priced = evaluator.compare(incumbent, challenger);
            require(incumbent.lowerReplayed && challenger.lowerReplayed && priced.commonEnemyCancelled,
                    "same-enemy owned comparison fixture");
            expect(priced.valid() == comparison.valid && priced.unresolved() == comparison.unresolved,
                   "seated economics distinguish overlap, resolved comparisons and overflow");
            const auto before = ledger.stats();
            const auto grants = ledger.retainedGrantCount();
            const auto captures = session.proofCaptures;
            const auto incumbentProof = incumbent.proof;
            const auto challengerProof = challenger.proof;
            expect(evaluator.unresolvedCommonEnemy(incumbent, challenger) == priced.unresolved(),
                   "cancellation eligibility agrees with the full economic comparison");
            const auto cost = ledger.stats().spent() - before.spent();
            const auto expectedCost = 32 + 4 * (incumbent.proof->quote.key.size() +
                challenger.proof->quote.key.size() + incumbent.proof->quote.capturedColumns.size() +
                challenger.proof->quote.capturedColumns.size());
            expect(cost == static_cast<std::int64_t>(expectedCost) &&
                       ledger.stats().consumed - before.consumed == cost &&
                       ledger.retainedGrantCount() == grants && session.proofCaptures == captures &&
                       incumbent.proof == incumbentProof && challenger.proof == challengerProof,
                   "owned eligibility is charged without native repricing, grant retention or witness replacement");
            auto missing = challenger;
            missing.upperVerified = false;
            expect(!evaluator.unresolvedCommonEnemy(incumbent, missing),
                   "unverified upper cannot justify scheduling cancellation");
            missing = challenger;
            missing.proof.reset();
            expect(!evaluator.unresolvedCommonEnemy(incumbent, missing),
                   "missing owned proof cannot justify scheduling cancellation");
            missing = challenger;
            missing.lowerReplayed = false;
            expect(!evaluator.unresolvedCommonEnemy(incumbent, missing),
                   "unreplayed lower cannot justify scheduling cancellation");
            missing = challenger;
            missing.replay.reset();
            expect(!evaluator.unresolvedCommonEnemy(incumbent, missing),
                   "missing seated economics cannot justify scheduling cancellation");
            auto foreign = challenger;
            auto foreignProof = std::make_shared<RelativeStockProofSnapshot>(*challenger.proof);
            foreignProof->quote.identity = {};
            foreign.proof = std::move(foreignProof);
            expect(!evaluator.unresolvedCommonEnemy(incumbent, foreign),
                   "foreign stock identity cannot justify scheduling cancellation");
            require(ledger.consume(ledger.balance() - cost, RefinementWork::MasterPreparation),
                    "owned eligibility exact allowance boundary");
            expect(evaluator.unresolvedCommonEnemy(incumbent, challenger) == priced.unresolved() &&
                       ledger.balance() == 0 && !ledger.lastRefusal(),
                   "exact eligibility allowance pays the whole comparison");
            expect(!evaluator.unresolvedCommonEnemy(incumbent, challenger) && ledger.lastRefusal() &&
                       ledger.lastRefusal()->work == RefinementWork::MasterValuation &&
                       ledger.lastRefusal()->requested == cost,
                   "unaffordable eligibility fails closed with its real ledger refusal");
            auto failed = challenger;
            require(!evaluator.refresh(failed) && failed.proof == challengerProof,
                    "failed fresh pricing can retain an older owned snapshot");
            const auto refusalCount = ledger.stats().refusals;
            expect(!evaluator.unresolvedCommonEnemy(incumbent, failed) &&
                       ledger.stats().refusals == refusalCount,
                   "a failed objective status cannot reuse stale success flags for scheduling");
        }
    }

    constexpr std::int64_t LEDGER_WORK = 100000000;
    constexpr std::int64_t CERTIFICATE_WORK = 1000000;
    constexpr std::int64_t ORACLE_STATES = 1000000;
    constexpr MasterValuationTariffs TARIFFS{.quote = 32, .witness = 32};
    constexpr std::int32_t SUGGESTION_ACTORS = 3;
    constexpr MilliFunds SUGGESTION_MOVE_VALUE = -3;
    constexpr std::int32_t CAPTURE_INCOME = 11;

    using ContinuationEdit = std::function<void(ContinuationCase &, const std::vector<std::int32_t> &)>;

    Fixture stockMoveFixture(std::int32_t actors, const std::function<MilliFunds(std::int32_t)> & valueFor)
    {
        Fixture fixture(actors);
        for (std::int32_t actor = 0; actor < actors; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor, 1}, valueFor(actor), 1));
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < actors; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        return fixture;
    }

    Fixture suggestionFixture()
    {
        return stockMoveFixture(SUGGESTION_ACTORS, [](std::int32_t actor) { return SUGGESTION_MOVE_VALUE - actor; });
    }

    std::vector<ContinuationCase> continuationCases(const MasterAssignmentProblem & problem,
                                                    const ContinuationEdit & edit = {})
    {
        std::vector<ContinuationCase> result;
        for (const auto & tuple : MasterAssignmentOracle::enumerate(problem, request()).tuples)
        {
            auto entry = continuationCase(tuple.reference.key);
            if (edit)
            {
                edit(entry, tuple.choices);
            }
            result.push_back(std::move(entry));
        }
        return result;
    }

    MasterAssignmentOracle::Request sequentialReference(const MasterAssignmentProblem & problem,
                                                        const ContinuationEdit & edit = {})
    {
        auto reference = request();
        reference.continuation.mode = MasterAssignmentOracle::ContinuationMode::Sequential;
        reference.continuation.cases = continuationCases(problem, edit);
        return reference;
    }

    const ContinuationCase* findContinuationCase(const std::vector<ContinuationCase> & cases, const ContinuationKey & key)
    {
        const auto found = std::find_if(cases.begin(), cases.end(),
                                        [&](const ContinuationCase & entry) { return entry.key == key; });
        return found == cases.end() ? nullptr : &*found;
    }

    std::int64_t adoptionCost(const NativeStockSession & session)
    {
        return session.ourRerunWork() + 2 * TARIFFS.quote + TARIFFS.witness;
    }

    void suggestedFriendlyBounds()
    {
        constexpr MilliFunds PRICE_STEP = 10;
        constexpr MilliFunds PRICE_LIMIT = 100;
        constexpr MilliFunds USED_NODE_PRICE = 1000;
        const auto fixture = suggestionFixture();
        const auto problem = fixture.extract();
        const auto reference = sequentialReference(problem);
        const auto & cases = reference.continuation.cases;
        const std::vector<std::int32_t> incumbentChoices{0, 0, 0};
        const std::vector<std::int32_t> challengerChoices{1, 1, 1};
        const auto incumbentKey = problem.incumbentContinuationKey();
        PropertyStockField field;
        PropertyStockInstance enemy;
        const auto caseFor = [&](const ContinuationKey & key) -> const ContinuationCase &
        {
            const auto * found = findContinuationCase(cases, key);
            require(found != nullptr, "suggestion fixture has the continuation case");
            return *found;
        };
        const auto optimum = [&](const ContinuationKey & key)
        {
            const NativeSequentialModel model(caseFor(key).friendly);
            auto tier = solveSequentialPacking(model.instance(), 0, ORACLE_STATES, nullptr, true,
                                               SequentialWitnessPolicy::BookedValue);
            const auto oracle = MasterAssignmentOracle::exhaustSequential(caseFor(key).friendly, ORACLE_STATES);
            require((tier.searchCompleted || tier.certificate) &&
                        oracle.status == MasterAssignmentOracle::ObjectiveStatus::Exact &&
                        tier.bookedValue == oracle.value && replaySequentialWitness(model.instance(), tier.witness, oracle.value),
                    "suggested witness is the independently exact itinerary packing");
            return tier;
        };
        const auto bestHint = [&](const ContinuationKey & key)
        {
            std::vector<MilliFunds> prices(caseFor(key).friendly.columns.size(), 0);
            std::vector<MilliFunds> best;
            auto bestUpper = std::numeric_limits<MilliFunds>::max();
            for (;;)
            {
                RefinementLedger ledger;
                require(ledger.open(LEDGER_WORK), "hint grid ledger");
                NativeStockSession session(field, enemy, cases, ledger);
                require(session.hintOurUpperPrices(key, prices), "grid prices are well formed hints");
                const auto tightened = session.tightenOurUpperWithVerifiedDual(session.quote(key, 0), CERTIFICATE_WORK);
                if (tightened.certificateValid && tightened.certifiedUpper < bestUpper)
                {
                    bestUpper = tightened.certifiedUpper;
                    best = prices;
                }
                std::size_t node = 0;
                for (; node < prices.size() && prices[node] == PRICE_LIMIT; ++node)
                {
                    prices[node] = 0;
                }
                if (node == prices.size())
                {
                    return std::pair{bestUpper, best};
                }
                prices[node] += PRICE_STEP;
            }
        };
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "adoption ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            require(evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                        MasterRefinementStatus::Updated && evaluation.quote()->ourUpperCertificateId != 0,
                    "adoption fixture holds a verified upper certificate");
            const auto before = *evaluation.quote();
            const auto best = optimum(incumbentKey);
            require(before.key == incumbentKey && before.ours.lower < best.bookedValue,
                    "cheap lower leaves room for a better suggested witness");
            const auto unchanged = [&](const char* message)
            {
                const auto proof = session.captureProof(session.quote(incumbentKey, 0));
                expect(proof && sameRelativeStockQuote(proof->quote, before) &&
                           proof->ourKind == RelativeStockWitnessKind::Cheap && proof->ourUpperCertificate &&
                           sameRelativeStockQuote(*evaluation.quote(), before) && !session.pricingFailure &&
                           !ledger.lastRefusal() && !evaluator.refinementRefusal(),
                       message);
            };
            expect(evaluator.adoptFriendlyWitness(evaluation, best.witness, best.bookedValue + 1) ==
                       MasterRefinementStatus::Unavailable, "a witness with the wrong total value is declined");
            unchanged("a wrong total leaves interval, witness and certificate untouched");
            const auto & rows = caseFor(incumbentKey).friendly.rows;
            const auto lastRow = static_cast<std::int32_t>(rows.size()) - 1;
            const auto lastNode = static_cast<std::int32_t>(caseFor(incumbentKey).friendly.columns.size()) - 1;
            const auto firstCapture = [&](std::int32_t row, std::int32_t node)
            {
                const auto & source = rows[static_cast<std::size_t>(row)];
                const auto slot = static_cast<std::size_t>(node);
                return SequentialRowWitness{row, {{node, source.ownedTurn[slot], source.weight[slot]}}, source.weight[slot]};
            };
            const std::vector<SequentialRowWitness> shared{firstCapture(lastRow - 1, lastNode), firstCapture(lastRow, lastNode)};
            const std::vector<SequentialRowWitness> disjoint{firstCapture(lastRow - 1, lastNode - 1),
                                                             firstCapture(lastRow, lastNode)};
            const auto sharedValue = shared.front().value + shared.back().value;
            const NativeSequentialModel model(caseFor(incumbentKey).friendly);
            require(sharedValue > before.ours.lower && !replaySequentialWitness(model.instance(), shared, sharedValue) &&
                        replaySequentialWitness(model.instance(), disjoint, disjoint.front().value + disjoint.back().value),
                    "the only defect of the shared-node witness is its reused node");
            expect(evaluator.adoptFriendlyWitness(evaluation, shared, sharedValue) == MasterRefinementStatus::Unavailable,
                   "two rows sharing a node are declined");
            unchanged("a shared node leaves interval, witness and certificate untouched");
            auto outOfRange = best.witness;
            outOfRange.push_back({static_cast<std::int32_t>(rows.size()), {{0, 1, 1}}, 1});
            expect(evaluator.adoptFriendlyWitness(evaluation, outOfRange, best.bookedValue + 1) ==
                       MasterRefinementStatus::Unavailable, "a row index outside the instance is declined");
            unchanged("an out-of-range row leaves interval, witness and certificate untouched");
            const auto spentBeforeNoOp = ledger.stats().spent();
            expect(evaluator.adoptFriendlyWitness(evaluation, best.witness, before.ours.lower) ==
                       MasterRefinementStatus::Unavailable && ledger.stats().spent() == spentBeforeNoOp,
                   "a value not above the current lower is an uncharged no-op");
            unchanged("a no-op suggestion leaves interval, witness and certificate untouched");
            const auto spentBeforeAdoption = ledger.stats().spent();
            require(evaluator.adoptFriendlyWitness(evaluation, best.witness, best.bookedValue) ==
                        MasterRefinementStatus::Updated, "a replaying better witness is adopted");
            const auto after = *evaluation.quote();
            const auto proof = session.captureProof(after);
            expect(after.valid && after.ours.lower == best.bookedValue && after.ours.upper == before.ours.upper &&
                       after.ourUpperCertificateId == before.ourUpperCertificateId &&
                       evaluation.objectiveStatus == MasterObjectiveStatus::Verified && evaluation.lowerReplayed &&
                       evaluation.proof->ourKind == RelativeStockWitnessKind::Sequential && proof &&
                       session.replayLowerProof(*proof) && session.compare(after, after).valid(),
                   "an adopted witness raises the lower to a replayed, verified evaluation");
            expect(ledger.stats().spent() - spentBeforeAdoption == adoptionCost(session) &&
                       !ledger.lastRefusal() && !evaluator.refinementRefusal() && !session.pricingFailure,
                   "adoption charges the rerun tariff, its quote and the refreshed proof");
        }
        for (const bool affordable : {false, true})
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "adoption budget ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            const auto best = optimum(incumbentKey);
            const auto required = adoptionCost(session);
            const auto reserve = affordable ? required : required - 1;
            require(ledger.consume(ledger.balance() - reserve, RefinementWork::MasterPreparation),
                    "adoption allowance boundary");
            const auto proof = evaluation.proof;
            const auto status = evaluator.adoptFriendlyWitness(evaluation, best.witness, best.bookedValue);
            if (affordable)
            {
                expect(status == MasterRefinementStatus::Updated && ledger.balance() == 0 &&
                           evaluation.quote()->ours.lower == best.bookedValue,
                       "the exact adoption allowance pays the session tariff and the refresh");
            }
            else
            {
                expect(status == MasterRefinementStatus::BudgetExhausted && ledger.balance() == reserve &&
                           evaluation.proof == proof &&
                           sameRelativeStockQuote(session.quote(incumbentKey, 0), proof->quote),
                       "an unaffordable suggestion is skipped without charging or changing the interval");
            }
            expect(!ledger.lastRefusal() && !evaluator.refinementRefusal(),
                   "suggestion funding never records a ledger or search refusal");
        }
        const auto [hintedUpper, hintPrices] = bestHint(incumbentKey);
        require(!hintPrices.empty(), "the price grid yields a verified certificate");
        {
            RefinementLedger optimizerLedger;
            require(optimizerLedger.open(LEDGER_WORK), "built-in optimizer ledger");
            NativeStockSession optimizerSession(field, enemy, cases, optimizerLedger);
            MasterAssignmentEvaluator optimizerEvaluator(problem, optimizerLedger, optimizerSession, TARIFFS);
            auto optimized = optimizerEvaluator.evaluate({problem.identity(), incumbentChoices});
            require(optimizerEvaluator.refine(optimized, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                        MasterRefinementStatus::Updated, "built-in optimizer certifies the incumbent");
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "hint ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            auto wrongSize = hintPrices;
            wrongSize.push_back(0);
            auto negative = hintPrices;
            negative.back() = -1;
            const std::vector<MilliFunds> zero(hintPrices.size(), 0);
            expect(!evaluator.hintFriendlyUpperPrices(evaluation, wrongSize) &&
                       !evaluator.hintFriendlyUpperPrices(evaluation, negative),
                   "hints with the wrong size or a negative price are rejected");
            require(evaluator.hintFriendlyUpperPrices(evaluation, zero) &&
                        evaluator.hintFriendlyUpperPrices(evaluation, hintPrices),
                    "a later hint for the same key replaces the earlier one");
            require(evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                        MasterRefinementStatus::Updated, "hinted prices certify the incumbent");
            const auto hinted = *evaluation.quote();
            const auto exact = MasterAssignmentOracle::exhaustSequential(caseFor(incumbentKey).friendly, ORACLE_STATES);
            expect(hinted.ourUpperCertificateId != 0 && hinted.ours.upper == hintedUpper &&
                       hintedUpper < optimized.quote()->ours.upper && hintedUpper == exact.value,
                   "a brute-forced hint certifies the exact friendly value where built-in prices stay loose");
            expect(evaluator.refresh(evaluation) && session.compare(*evaluation.quote(), *evaluation.quote()).valid(),
                   "a hinted certificate authorizes a refreshed comparison");
            const auto spent = ledger.stats().spent();
            require(evaluator.hintFriendlyUpperPrices(evaluation, zero), "a hint after certification is stored");
            expect(evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                       MasterRefinementStatus::Unavailable && sameRelativeStockQuote(*evaluation.quote(), hinted) &&
                       ledger.stats().spent() == spent,
                   "a hint stored after a certificate exists changes nothing");
        }
        {
            auto captured = caseFor(incumbentKey);
            captured.capturedColumns = {0};
            captured.friendly.capturedNodes[0] = true;
            for (auto & row : captured.friendly.rows)
            {
                removeCapturedNode(row, 0);
            }
            captured.owned += ownershipFlipSwing(captured.friendly.columns[0], captured.friendly.horizon);
            const std::vector<ContinuationCase> single{captured};
            const auto certify = [&](std::span<const MilliFunds> prices)
            {
                RefinementLedger ledger;
                require(ledger.open(LEDGER_WORK), "used-node hint ledger");
                NativeStockSession session(field, enemy, single, ledger);
                require(session.hintOurUpperPrices(incumbentKey, prices), "used-node hint stored");
                return session.tightenOurUpperWithVerifiedDual(session.quote(incumbentKey, 0), CERTIFICATE_WORK);
            };
            auto usedPriced = hintPrices;
            usedPriced.front() = USED_NODE_PRICE;
            auto usedFree = hintPrices;
            usedFree.front() = 0;
            const auto charged = certify(usedPriced);
            const auto zeroed = certify(usedFree);
            expect(charged.certificateValid && zeroed.certificateValid && charged.certificate->nodePrices.front() == 0 &&
                       charged.certifiedUpper == zeroed.certifiedUpper,
                   "a nonzero hint on a used node is forced to zero and still certifies");
        }
        RefinementLedger ledger;
        require(ledger.open(LEDGER_WORK), "suggestion comparison ledger");
        NativeStockSession session(field, enemy, cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
        auto incumbent = evaluator.evaluate({problem.identity(), incumbentChoices});
        auto challenger = evaluator.evaluate({problem.identity(), challengerChoices});
        const auto incumbentTotal = MasterAssignmentOracle::evaluateObjective(
            MasterAssignmentOracle::constructReference(problem, incumbentChoices), reference.continuation, ORACLE_STATES);
        const auto challengerTotal = MasterAssignmentOracle::evaluateObjective(
            MasterAssignmentOracle::constructReference(problem, challengerChoices), reference.continuation, ORACLE_STATES);
        require(challengerTotal.total > incumbentTotal.total && evaluator.compare(incumbent, challenger).unresolved(),
                "cheap intervals leave a truly better challenger unresolved");
        const auto challengerKey = challenger.quote()->key;
        const auto best = optimum(challengerKey);
        require(evaluator.adoptFriendlyWitness(challenger, best.witness, best.bookedValue) ==
                    MasterRefinementStatus::Updated, "challenger adopts its suggested witness");
        expect(evaluator.compare(incumbent, challenger).unresolved(),
               "a tight challenger lower alone cannot beat the cheap incumbent upper");
        require(evaluator.hintFriendlyUpperPrices(incumbent, hintPrices) &&
                    evaluator.refine(incumbent, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                        MasterRefinementStatus::Updated, "incumbent certifies its upper with hinted prices");
        const auto comparison = evaluator.compare(incumbent, challenger);
        expect(comparison.improves() && comparison.deltaLower <= challengerTotal.total - incumbentTotal.total,
               "adopted lower and hinted upper prove the oracle-confirmed improvement");
    }

    void untrustedSuggestionSafety()
    {
        constexpr std::int64_t FRIENDLY_WORK = 100000;
        const auto fixture = suggestionFixture();
        const auto problem = fixture.extract();
        const auto cases = continuationCases(problem);
        const std::vector<std::int32_t> incumbentChoices{0, 0, 0};
        const auto incumbentKey = problem.incumbentContinuationKey();
        const auto * found = findContinuationCase(cases, incumbentKey);
        require(found != nullptr, "safety fixture has the incumbent case");
        const NativeSequentialModel model(found->friendly);
        const auto instance = model.instance();
        const auto floor = model.floorValue();
        PropertyStockField field;
        PropertyStockInstance enemy;
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "below-floor ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            const auto before = *evaluation.quote();
            std::optional<std::vector<SequentialRowWitness>> belowFloor;
            MilliFunds belowValue = 0;
            const auto rows = static_cast<std::int32_t>(instance.rows.size());
            const auto nodes = instance.nodeCount();
            std::vector<std::int32_t> choice(static_cast<std::size_t>(rows), NO_SEQUENTIAL_NODE);
            const std::function<void(std::int32_t)> search = [&](std::int32_t row)
            {
                if (belowFloor)
                {
                    return;
                }
                if (row == rows)
                {
                    std::vector<SequentialRowWitness> witness;
                    MilliFunds value = 0;
                    for (std::int32_t index = 0; index < rows; ++index)
                    {
                        const auto node = choice[static_cast<std::size_t>(index)];
                        if (node == NO_SEQUENTIAL_NODE)
                        {
                            continue;
                        }
                        const auto & source = instance.rows[static_cast<std::size_t>(index)];
                        const auto slot = static_cast<std::size_t>(node);
                        witness.push_back({index, {{node, source.ownedTurn[slot], source.weight[slot]}}, source.weight[slot]});
                        value += source.weight[slot];
                    }
                    if (value > before.ours.lower && value < floor && replaySequentialWitness(instance, witness, value))
                    {
                        belowFloor = witness;
                        belowValue = value;
                    }
                    return;
                }
                search(row + 1);
                for (std::int32_t node = 0; node < nodes; ++node)
                {
                    if (std::find(choice.begin(), choice.end(), node) == choice.end())
                    {
                        choice[static_cast<std::size_t>(row)] = node;
                        search(row + 1);
                        choice[static_cast<std::size_t>(row)] = NO_SEQUENTIAL_NODE;
                    }
                }
            };
            search(0);
            require(belowFloor.has_value(), "the cheap lower leaves room for a replaying witness below the floor");
            expect(evaluator.adoptFriendlyWitness(evaluation, *belowFloor, belowValue) ==
                       MasterRefinementStatus::Unavailable &&
                       sameRelativeStockQuote(*evaluation.quote(), before) && !session.pricingFailure,
                   "a replaying witness below the floor is declined without touching the interval");
            expect(evaluator.refine(evaluation, MasterRefinementTerm::Friendly, FRIENDLY_WORK) ==
                       MasterRefinementStatus::Updated && !session.pricingFailure &&
                       evaluation.quote()->ours.lower >= floor,
                   "the next friendly refinement after a declined witness keeps the valuer live");
            std::printf("  below-floor witness: cheap lower %lld, witness %lld, floor %lld\n",
                        static_cast<long long>(before.ours.lower), static_cast<long long>(belowValue),
                        static_cast<long long>(floor));
        }
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "floor witness ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            auto tier = solveSequentialPacking(instance, floor, FRIENDLY_WORK, nullptr, true,
                                               SequentialWitnessPolicy::BookedValue);
            require(tier.bookedValue >= floor && tier.bookedValue > evaluation.quote()->ours.lower &&
                        replaySequentialWitness(instance, tier.witness, tier.bookedValue),
                    "the floor-seeded packing yields an adoptable witness");
            require(evaluator.adoptFriendlyWitness(evaluation, tier.witness, tier.bookedValue) ==
                        MasterRefinementStatus::Updated, "a witness at or above the floor is adopted");
            expect(evaluator.refine(evaluation, MasterRefinementTerm::Friendly, FRIENDLY_WORK) ==
                       MasterRefinementStatus::Updated && !session.pricingFailure,
                   "an adopted witness is accepted as the next refinement's warm start");
        }
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "foreign evaluation ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto local = evaluator.evaluate({problem.identity(), incumbentChoices});
            PropertyStockField foreignField;
            PropertyStockInstance foreignEnemy;
            RefinementLedger foreignLedger;
            require(foreignLedger.open(LEDGER_WORK), "foreign session ledger");
            NativeStockSession foreignSession(foreignField, foreignEnemy, cases, foreignLedger);
            MasterAssignmentEvaluator foreignEvaluator(problem, foreignLedger, foreignSession, TARIFFS);
            auto foreign = foreignEvaluator.evaluate({problem.identity(), incumbentChoices});
            auto tier = solveSequentialPacking(instance, floor, FRIENDLY_WORK, nullptr, true,
                                               SequentialWitnessPolicy::BookedValue);
            const auto localBefore = *local.quote();
            const auto foreignBefore = *foreign.quote();
            const auto spent = ledger.stats().spent();
            expect(evaluator.adoptFriendlyWitness(foreign, tier.witness, tier.bookedValue) ==
                       MasterRefinementStatus::Unavailable && ledger.stats().spent() == spent &&
                       sameRelativeStockQuote(session.quote(incumbentKey, 0), localBefore) &&
                       sameRelativeStockQuote(*foreign.quote(), foreignBefore),
                   "an evaluation from another session is never adopted and costs nothing");
        }
        std::optional<MilliFunds> optimizerUpper;
        std::optional<std::int64_t> optimizerWork;
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "unhinted certificate ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            const auto tightened = session.tightenOurUpperWithVerifiedDual(session.quote(incumbentKey, 0), CERTIFICATE_WORK);
            require(tightened.certificateValid && tightened.hintWork == 0, "the optimizer alone certifies the incumbent");
            optimizerUpper = tightened.certifiedUpper;
            optimizerWork = tightened.totalWork;
        }
        const auto columns = found->friendly.columns.size();
        const std::vector<MilliFunds> zero(columns, 0);
        const std::vector<MilliFunds> overflowing(columns, std::numeric_limits<MilliFunds>::max());
        for (const auto & hint : {zero, overflowing})
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "bad hint ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, TARIFFS);
            auto evaluation = evaluator.evaluate({problem.identity(), incumbentChoices});
            require(evaluator.hintFriendlyUpperPrices(evaluation, hint), "a non-negative hint is stored");
            expect(evaluator.refine(evaluation, MasterRefinementTerm::FutureDual, CERTIFICATE_WORK) ==
                       MasterRefinementStatus::Updated && evaluation.quote()->ourUpperCertificateId != 0 &&
                       evaluation.quote()->ours.upper <= *optimizerUpper && !session.pricingFailure,
                   "a bad hint never leaves the incumbent with a looser upper than no hint");
        }
        {
            RefinementLedger ledger;
            require(ledger.open(LEDGER_WORK), "short hint ledger");
            NativeStockSession session(field, enemy, cases, ledger);
            require(session.hintOurUpperPrices(incumbentKey, overflowing), "short-budget hint stored");
            const auto tightened = session.tightenOurUpperWithVerifiedDual(session.quote(incumbentKey, 0), *optimizerWork);
            expect(tightened.certificateValid && tightened.hintWork == 0 &&
                       tightened.certifiedUpper == *optimizerUpper && !session.pricingFailure,
                   "an allowance that only covers the optimizer keeps the optimizer's certificate");
        }
    }

    constexpr std::int32_t PLAN_PRICING_TEST_ROUNDS = 200;
    constexpr std::int64_t PLAN_PRICING_TEST_WORK = 400000;
    constexpr std::int64_t PLAN_PRICING_TEST_WITNESS_WORK = 50000;
    constexpr std::int64_t PLAN_PRICING_TEST_LEDGER = 4000000;

    enum class FriendlySearch : std::int8_t
    {
        Immediate,
        Deferred,
    };

    struct PlanPricingCase
    {
        const char* name;
        Fixture fixture;
        MasterAssignmentProblem problem;
        MasterAssignmentOracle::Request reference;
        std::optional<MilliFunds> optimum;
        MasterPlanPricingOutcome seededOutcome{MasterPlanPricingOutcome::Seeded};
    };

    struct PlanPricingRun
    {
        MasterAssignmentSearchResult result;
        QJsonObject record;
        std::int64_t refusals{0};
        std::uint64_t certificates{0};
        MasterInstallationStatus installed{MasterInstallationStatus::Unprepared};
        std::uint64_t capContradictions{0};
        std::optional<MilliFunds> heldIncumbentCap;
        RefinementLedgerStats ledgerAfterSearch;
        RefinementLedgerStats ledgerAfterInstall;
    };

    struct PlanPricingHooks
    {
        std::function<void(NativeStockSession &)> before;
        std::function<void(NativeStockSession &, const RelativeStockComparison &)> compared;
        std::function<void(MasterAssignmentSearchResult &)> tamper;
    };

    class ComparisonHook final : public MasterAssignmentSearchObserver
    {
    public:
        ComparisonHook(NativeStockSession & session, const PlanPricingHooks & hooks)
            : m_session(session), m_hooks(hooks)
        {
        }

        void evaluated(const MasterAssignmentEvaluation &, std::int64_t) override
        {
        }

        void refined(const MasterAssignmentEvaluation &, MasterRefinementTerm, std::int64_t,
                     MasterRefinementStatus, std::int64_t, std::int64_t) override
        {
        }

        void compared(const MasterAssignmentEvaluation &, const RelativeStockComparison & comparison,
                      std::int64_t) override
        {
            m_hooks.compared(m_session, comparison);
        }

    private:
        NativeStockSession & m_session;
        const PlanPricingHooks & m_hooks;
    };

    PlanPricingCase planPricingCase(const char* name, Fixture fixture, const ContinuationEdit & edit = {})
    {
        auto problem = fixture.extract();
        auto reference = sequentialReference(problem, edit);
        const auto optimum = MasterAssignmentOracle::enumerate(problem, reference).exactOptimum;
        require(optimum.has_value(), "plan pricing fixture has an exhaustive optimum");
        return {name, std::move(fixture), std::move(problem), std::move(reference), optimum};
    }

    std::vector<PlanPricingCase> planPricingCases()
    {
        constexpr std::int32_t SMALL_ACTORS = 3;
        constexpr std::int32_t LARGE_ACTORS = 4;
        constexpr MilliFunds MOVE_COST = 3;
        constexpr std::int32_t FAR_ROW = 2;
        std::vector<PlanPricingCase> result;
        result.push_back(planPricingCase("descending moves", stockInstallationFixture()));
        result.push_back(planPricingCase("equal moves",
                                         stockMoveFixture(SMALL_ACTORS, [](std::int32_t) { return -MOVE_COST; })));
        {
            Fixture fixture(1);
            fixture.input.unitLinks[0].tile = {0, 2};
            auto wait = moveCandidate(0, {0, 2}, {0, 2}, 0);
            auto capture = wait;
            capture.generationIndex = 1;
            capture.bundle.components.push_back(
                captureComponent({.income = {CAPTURE_INCOME, CAPTURE_INCOME}, .turnsUntilOwned = 0}));
            fixture.input.actors[0].candidates = {wait, capture};
            fixture.seat();
            fixture.selections[0].stockRow = 0;
            result.push_back(planPricingCase("capture", std::move(fixture),
                [](ContinuationCase & entry, const std::vector<std::int32_t> & choices)
                {
                    if (choices[0] == 0)
                    {
                        entry.friendly.rows[0].ownedTurn[0] = 1;
                        entry.friendly.rows[0].weight[0] = columnStreamValue(entry.friendly.columns[0], 1,
                                                                             entry.friendly.horizon);
                        return;
                    }
                    entry.capturedColumns = {0};
                    entry.friendly.capturedNodes[0] = true;
                    removeCapturedNode(entry.friendly.rows[0], 0);
                    entry.owned += ownershipFlipSwing(entry.friendly.columns[0], entry.friendly.horizon);
                }));
            // The capture plan's witness is no better than its cheap lower, so the session declines it.
            result.back().seededOutcome = MasterPlanPricingOutcome::WitnessDeclined;
        }
        {
            Fixture fixture(LARGE_ACTORS);
            for (std::int32_t actor = 0; actor < LARGE_ACTORS; ++actor)
            {
                fixture.input.actors[actor].candidates.push_back(
                    moveCandidate(actor, {actor, 0}, {actor, 1}, -2 * actor, 1));
                fixture.input.actors[actor].candidates.push_back(
                    moveCandidate(actor, {actor, 0}, {actor, FAR_ROW}, -MOVE_COST * actor, FAR_ROW));
            }
            fixture.seat();
            for (std::int32_t actor = 0; actor < LARGE_ACTORS; ++actor)
            {
                fixture.selections[actor].stockRow = actor;
            }
            result.push_back(planPricingCase("four actors", std::move(fixture)));
        }
        return result;
    }

    MasterAssignmentSchedule allocationSchedule(MasterRefinementAllocation allocation)
    {
        MasterAssignmentSchedule schedule;
        if (allocation == MasterRefinementAllocation::OperationService)
        {
            schedule.initialIncumbentAlternatives = true;
            schedule.initialContributionCompletion = true;
            schedule.allocation = allocation;
        }
        return schedule;
    }

    MasterAssignmentSchedule planPricingSchedule(MasterRefinementAllocation allocation, std::int32_t rounds,
                                                 std::int64_t work = PLAN_PRICING_TEST_WORK,
                                                 std::int64_t witnessWork = PLAN_PRICING_TEST_WITNESS_WORK)
    {
        auto schedule = allocationSchedule(allocation);
        schedule.planPricingRounds = rounds;
        schedule.planPricingWork = rounds == 0 ? 0 : work;
        schedule.planPricingWitnessWork = rounds == 0 ? 0 : witnessWork;
        return schedule;
    }

    PlanPricingRun runPlanPricing(const PlanPricingCase & test, const MasterAssignmentSchedule & schedule,
                                  std::int64_t allowance = PLAN_PRICING_TEST_LEDGER,
                                  FriendlySearch friendly = FriendlySearch::Immediate,
                                  const PlanPricingHooks & hooks = {})
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(allowance), "plan pricing search ledger");
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger, QuoteFault::None, true,
                                   friendly == FriendlySearch::Deferred);
        session.operationServiceEnabled = schedule.allocation == MasterRefinementAllocation::OperationService;
        if (hooks.before)
        {
            hooks.before(session);
        }
        MasterAssignmentEvaluator evaluator(test.problem, ledger, session, TARIFFS);
        MasterAssignmentInstallation installation(test.problem, evaluator, ledger);
        auto incoming = installationIncoming(test.fixture, test.problem);
        PlanPricingRun run;
        if (installation.prepare(incoming) != MasterInstallationStatus::Reserved)
        {
            run.refusals = ledger.stats().refusals;
            return run;
        }
        ComparisonHook observer(session, hooks);
        run.result = searchMasterAssignment(test.problem, evaluator, ledger,
                                            {.schedule = schedule, .retainDiagnostics = true,
                                             .observer = hooks.compared ? &observer : nullptr});
        run.ledgerAfterSearch = ledger.stats();
        run.record = searchRecord(run.result);
        run.refusals = ledger.stats().refusals;
        run.certificates = session.verifiedCertificates;
        run.capContradictions = evaluator.oursCapContradictions();
        run.heldIncumbentCap = run.result.incumbent ? evaluator.oursCap(*run.result.incumbent) : std::nullopt;
        if (hooks.tamper)
        {
            hooks.tamper(run.result);
        }
        run.installed = installation.finish(run.result.improvement, incoming).status;
        run.ledgerAfterInstall = ledger.stats();
        expect(!run.result.improvement || (session.replayLowerProof(*run.result.improvement->incumbent.proof) &&
                                           session.replayLowerProof(*run.result.improvement->challenger.proof)),
               "plan pricing proofs own replayable witnesses");
        return run;
    }

    void checkImprovementIsReal(const PlanPricingCase & test, const PlanPricingRun & run)
    {
        if (run.result.improvement)
        {
            const auto & proof = *run.result.improvement;
            const auto challenger = MasterAssignmentOracle::evaluateObjective(
                MasterAssignmentOracle::constructReference(test.problem, proof.challenger.state.choices),
                test.reference.continuation, ORACLE_STATES);
            const auto incumbent = MasterAssignmentOracle::evaluateObjective(
                MasterAssignmentOracle::constructReference(test.problem, proof.incumbent.state.choices),
                test.reference.continuation, ORACLE_STATES);
            expect(proof.comparison.deltaLower > 0 && challenger.total > incumbent.total &&
                       proof.comparison.deltaLower <= challenger.total - incumbent.total,
                   "a seeded improvement is a real improvement for the oracle");
            expect(run.installed == MasterInstallationStatus::Installed,
                   "installation accepts the seeded improvement proof");
        }
    }

    void checkPlanPricingSoundness(const PlanPricingCase & test, const PlanPricingRun & run)
    {
        const auto & result = run.result;
        expect((!result.lower || *result.lower <= *test.optimum) && (!result.upper || *result.upper >= *test.optimum) &&
                   result.stats.proofFailures == 0,
               "plan pricing searches bracket the exhaustive optimum");
        expect(result.stats.planPricing.outcome != MasterPlanPricingOutcome::ValuerFailure,
               "an adapter witness never fails the valuer");
        checkImprovementIsReal(test, run);
    }

    bool noWorse(const PlanPricingRun & on, const PlanPricingRun & off)
    {
        const auto & seeded = on.result;
        const auto & plain = off.result;
        return (!plain.lower || (seeded.lower && *seeded.lower >= *plain.lower)) &&
               (!plain.upper || (seeded.upper && *seeded.upper <= *plain.upper)) &&
               (!plain.improvement || seeded.improvement);
    }

    void planPricingAdapter()
    {
        for (const auto & test : planPricingCases())
        {
            RefinementLedger ledger;
            require(ledger.open(PLAN_PRICING_TEST_LEDGER), "adapter ledger");
            PropertyStockField field;
            PropertyStockInstance enemy;
            NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(test.problem, ledger, session, TARIFFS);
            const auto & catalog = *session.futureCatalog();
            const auto & stock = *session.pricingModel();
            const auto refusals = ledger.stats().refusals;
            const auto pricing = buildMasterPlanPricingInstance(test.problem, stock, catalog, ledger);
            require(pricing.has_value() && ledger.stats().refusals == refusals && pricing->work > 0,
                    "the adapter prices the native stock fixture without a refusal");
            constexpr std::int64_t ADAPTER_RESERVE = 1000;
            for (const bool funded : {false, true})
            {
                RefinementLedger reserveLedger;
                require(reserveLedger.open(pricing->work + ADAPTER_RESERVE - (funded ? 0 : 1)), "adapter reserve ledger");
                const auto reserved = buildMasterPlanPricingInstance(test.problem, stock, catalog, reserveLedger,
                                                                     ADAPTER_RESERVE);
                expect(reserved.has_value() == funded &&
                           reserveLedger.stats().spent() == (funded ? pricing->work : 0) &&
                           reserveLedger.stats().refusals == 0,
                       "the adapter is built only when its own work and the reserve both fit");
            }
            const auto & model = pricing->model;
            std::size_t modelled = 0;
            for (const auto & actor : test.problem.actors())
            {
                const auto incumbent = test.problem.incumbentChoices()[static_cast<std::size_t>(actor.canonicalSlot)];
                if (actor.stockRow == NO_STOCK_ROW || incumbent == NO_CANDIDATE)
                {
                    continue;
                }
                const auto slot = model.rowActors[static_cast<std::size_t>(actor.stockRow)];
                require(slot >= 0 && pricing->actorSlots[static_cast<std::size_t>(slot)] == actor.canonicalSlot,
                        "every stock actor with an incumbent is a pricing actor");
                const auto & priced = model.actors[static_cast<std::size_t>(slot)];
                std::size_t expected = 0;
                for (const auto index : actor.canonicalCandidates)
                {
                    expected += actor.candidates[static_cast<std::size_t>(index)].plannable ? 1 : 0;
                }
                const auto changeable = test.problem.canChangeActor(actor.canonicalSlot);
                expect(priced.candidates.size() == (changeable ? expected : 1) &&
                           findMasterPlanCandidate(priced, incumbent) != nullptr,
                       "pricing candidates are the plannable canonical candidates bounds prepares");
                for (const auto & candidate : priced.candidates)
                {
                    const auto & source = actor.candidates[static_cast<std::size_t>(candidate.candidateIndex)];
                    const auto * terms = stock.actionTerms(candidate.key);
                    require(terms != nullptr, "pricing candidate keys are stock actions");
                    if (terms->capturedColumn < 0)
                    {
                        expect(candidate.capturedNode == NO_SEQUENTIAL_NODE &&
                                   candidate.immediateValue == source.immediateValue.economicValue,
                               "a move keeps its catalog row and immediate value");
                        continue;
                    }
                    const auto node = static_cast<std::size_t>(candidate.capturedNode);
                    expect(candidate.capturedNode >= 0 && catalog.columns[node].slot == terms->capturedColumn &&
                               candidate.row.weight[node] == 0 && candidate.row.ownedTurn[node] == NO_CAPTURE_TURNS &&
                               candidate.immediateValue == source.immediateValue.economicValue +
                                   stock.capturedColumnSwings()[static_cast<std::size_t>(terms->capturedColumn)],
                           "a completing capture removes its node from its own row and books the capture swing");
                }
                ++modelled;
            }
            expect(model.actors.size() == modelled, "no actor outside the bound basis is priced");
            const auto tuned = tuneMasterPlanPrices(model,
                {.rounds = PLAN_PRICING_TEST_ROUNDS, .maximumWork = PLAN_PRICING_TEST_WORK,
                 .witnessBuildWork = PLAN_PRICING_TEST_WITNESS_WORK}, ledger);
            require(tuned.witness.valid, "the adapter model yields a witness");
            const auto state = masterPlanWitnessState(test.problem, *pricing, tuned.witness);
            require(state && validMasterState(test.problem, *state), "the witness is a valid master state");
            const auto evaluation = evaluator.evaluate(*state);
            require(evaluation.status == MasterEvaluationStatus::Feasible && evaluation.quote(),
                    "the witness state evaluates");
            const auto friendly = masterPlanFriendlyWitness(tuned.witness);
            const auto * found = findContinuationCase(test.reference.continuation.cases, evaluation.quote()->key);
            require(friendly && found != nullptr, "the witness key has a case");
            const NativeSequentialModel instance(found->friendly);
            expect(replaySequentialWitness(instance.instance(), friendly->rows, friendly->value),
                   "the converted witness replays on the session instance of its plan");
            const auto forced = masterPlanStateChoices(test.problem, *pricing, operationIncumbent(test.problem));
            require(forced.has_value(), "the incumbent is representable by the adapter");
            const auto restricted = restrictMasterPlanModel(model, *forced);
            expect(restricted.has_value() && ledger.stats().refusals == refusals,
                   "the incumbent restriction is well formed and no adapter step records a refusal");
            std::printf("  plan pricing adapter %s: %zu actors, %zu actions, witness %lld, rows %lld\n", test.name,
                        model.actors.size(), model.catalog.actions.size(),
                        static_cast<long long>(tuned.witness.value), static_cast<long long>(friendly->value));
        }
    }

    void planPricingSearch()
    {
        for (const auto & test : planPricingCases())
        {
            for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
            {
                const auto plain = runPlanPricing(test, planPricingSchedule(allocation, 0));
                auto unrounded = allocationSchedule(allocation);
                unrounded.planPricingWork = PLAN_PRICING_TEST_WORK;
                unrounded.planPricingWitnessWork = PLAN_PRICING_TEST_WITNESS_WORK;
                const auto defaults = runPlanPricing(test, unrounded);
                const auto unset = runPlanPricing(test, allocationSchedule(allocation));
                expect(plain.record == unset.record && !plain.record.contains(QStringLiteral("planPricing")) &&
                           plain.result.stats.planPricing.outcome == MasterPlanPricingOutcome::Disabled,
                       "zero rounds is byte-identical to an unset pre-pass");
                expect(defaults.result.stats.work == plain.result.stats.work &&
                           defaults.result.lower == plain.result.lower && defaults.result.upper == plain.result.upper,
                       "work limits without rounds change nothing");
                checkPlanPricingSoundness(test, plain);
                const auto seeded = runPlanPricing(test, planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS));
                const auto repeat = runPlanPricing(test, planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS));
                checkPlanPricingSoundness(test, seeded);
                expect(seeded.result.lower && seeded.result.upper && plain.result.lower && plain.result.upper,
                       "funded searches report both bounds");
                expect(seeded.record == repeat.record, "two seeded searches export identical records");
                const auto & seededPricing = seeded.result.stats.planPricing;
                expect(seededPricing.outcome == test.seededOutcome && seededPricing.adapterWork > 0 &&
                           seededPricing.challengerWork > 0 && seededPricing.hintStored &&
                           seededPricing.adoptedValue.has_value() ==
                               (test.seededOutcome == MasterPlanPricingOutcome::Seeded),
                       "a funded pre-pass reaches its expected outcome and stores the incumbent hint");
                expect(seeded.refusals == plain.refusals && noWorse(seeded, plain),
                       "the pre-pass records no refusal and loses no bound");
                const auto & stats = seeded.result.stats;
                std::printf("  plan pricing %s %s: outcome %d hint %d improved-first %d adopted %lld | "
                            "first improvement on %lld off %lld | refinements on %llu off %llu | "
                            "incumbent refinements on %llu off %llu | certificates on %llu off %llu | "
                            "spent on %lld off %lld | tune work %lld + %lld\n",
                            test.name, allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                            static_cast<int>(stats.planPricing.outcome),
                            stats.planPricing.hintStored, stats.planPricing.firstComparisonImproved,
                            static_cast<long long>(stats.planPricing.adoptedValue.value_or(-1)),
                            static_cast<long long>(stats.firstImprovementWork.value_or(-1)),
                            static_cast<long long>(plain.result.stats.firstImprovementWork.value_or(-1)),
                            static_cast<unsigned long long>(stats.refinements),
                            static_cast<unsigned long long>(plain.result.stats.refinements),
                            static_cast<unsigned long long>(stats.incumbentRefinements),
                            static_cast<unsigned long long>(plain.result.stats.incumbentRefinements),
                            static_cast<unsigned long long>(seeded.certificates),
                            static_cast<unsigned long long>(plain.certificates),
                            static_cast<long long>(stats.work.spent()),
                            static_cast<long long>(plain.result.stats.work.spent()),
                            static_cast<long long>(stats.planPricing.challengerWork),
                            static_cast<long long>(stats.planPricing.incumbentWork));
                const auto loosePlain = runPlanPricing(test, planPricingSchedule(allocation, 0),
                                                       PLAN_PRICING_TEST_LEDGER, FriendlySearch::Deferred);
                const auto looseSeeded = runPlanPricing(test, planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS),
                                                        PLAN_PRICING_TEST_LEDGER, FriendlySearch::Deferred);
                checkPlanPricingSoundness(test, loosePlain);
                checkPlanPricingSoundness(test, looseSeeded);
                expect(looseSeeded.result.stats.planPricing.outcome == test.seededOutcome,
                       "a loose seeded search reaches its expected pre-pass outcome");
                expect(looseSeeded.refusals == loosePlain.refusals && noWorse(looseSeeded, loosePlain),
                       "a seeded loose search records no extra refusal and loses no bound");
                const auto & looseProof = looseSeeded.result.improvement;
                expect(looseProof.has_value() && looseSeeded.result.stats.planPricing.hintStored,
                       "an adopted witness and a hinted incumbent ceiling prove an improvement without friendly search");
                const auto incumbentCertified = looseProof && looseProof->incumbent.quote() &&
                    looseProof->incumbent.quote()->ourUpperCertificateId != 0;
                std::printf("    loose friendly: incumbent certified %d | improvement on %d off %d | "
                            "first improvement on %lld off %lld | "
                            "refinements on %llu off %llu | certificates on %llu off %llu | lower on %lld off %lld | "
                            "upper on %lld off %lld | outcome %d hint %d\n", incumbentCertified,
                            looseSeeded.result.improvement.has_value(), loosePlain.result.improvement.has_value(),
                            static_cast<long long>(looseSeeded.result.stats.firstImprovementWork.value_or(-1)),
                            static_cast<long long>(loosePlain.result.stats.firstImprovementWork.value_or(-1)),
                            static_cast<unsigned long long>(looseSeeded.result.stats.refinements),
                            static_cast<unsigned long long>(loosePlain.result.stats.refinements),
                            static_cast<unsigned long long>(looseSeeded.certificates),
                            static_cast<unsigned long long>(loosePlain.certificates),
                            static_cast<long long>(looseSeeded.result.lower.value_or(-1)),
                            static_cast<long long>(loosePlain.result.lower.value_or(-1)),
                            static_cast<long long>(looseSeeded.result.upper.value_or(-1)),
                            static_cast<long long>(loosePlain.result.upper.value_or(-1)),
                            static_cast<int>(looseSeeded.result.stats.planPricing.outcome),
                            looseSeeded.result.stats.planPricing.hintStored);
                struct Limited
                {
                    const char* name;
                    MasterAssignmentSchedule schedule;
                    std::int64_t allowance;
                    MasterPlanPricingOutcome expected;
                };
                for (const auto & limited : {
                         Limited{"tiny work", planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS, 2),
                                 PLAN_PRICING_TEST_LEDGER, MasterPlanPricingOutcome::NoChallenger},
                         Limited{"tiny witness work", planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS,
                                                                          PLAN_PRICING_TEST_WORK, 1),
                                 PLAN_PRICING_TEST_LEDGER, MasterPlanPricingOutcome::NoChallenger},
                         Limited{"tiny ledger", planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS),
                                 PLAN_PRICING_TEST_WORK / MASTER_PLAN_PRICING_CHALLENGER_SHARE - 1,
                                 MasterPlanPricingOutcome::Unavailable}})
                {
                    const auto bounded = runPlanPricing(test, limited.schedule, limited.allowance);
                    const auto reference = limited.allowance == PLAN_PRICING_TEST_LEDGER
                        ? plain : runPlanPricing(test, planPricingSchedule(allocation, 0), limited.allowance);
                    checkPlanPricingSoundness(test, bounded);
                    const auto & pricing = bounded.result.stats.planPricing;
                    expect(pricing.outcome == limited.expected && !pricing.adoptedValue &&
                               (limited.expected != MasterPlanPricingOutcome::Unavailable ||
                                (pricing.adapterWork == 0 && pricing.challengerWork == 0 && !pricing.hintStored)),
                           "a starved pre-pass stops at its expected gate without adopting");
                    expect(bounded.refusals == reference.refusals && noWorse(bounded, reference),
                           "a starved pre-pass records no refusal and matches or beats the plain search");
                    std::printf("    %s: outcome %d refusals %lld vs %lld\n", limited.name,
                                static_cast<int>(bounded.result.stats.planPricing.outcome),
                                static_cast<long long>(bounded.refusals), static_cast<long long>(reference.refusals));
                }
            }
        }
    }

    constexpr MilliFunds CONTRADICTION_OWNED_INFLATION = 1000000;
    constexpr auto PLAN_PRICING_RECORD = "planPricing";
    constexpr auto BOUND_CAP_RECORD = "boundCap";

    MasterAssignmentSchedule boundCapSchedule(MasterRefinementAllocation allocation)
    {
        auto schedule = planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS);
        schedule.planBoundCap = MasterPlanBoundCap::Verified;
        return schedule;
    }

    bool hasBoundCapRecord(const PlanPricingRun & run)
    {
        return run.record.value(QLatin1String(PLAN_PRICING_RECORD)).toObject().contains(QLatin1String(BOUND_CAP_RECORD));
    }

    void checkBoundAtMethod(const PlanPricingCase & test, const MasterPlanBoundCapStats & cap)
    {
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "plan bound method ledger");
        PropertyStockField field;
        PropertyStockInstance enemy;
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        MasterAssignmentBounds bounds(test.problem);
        require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                "plan bound method bounds prepare");
        const MasterAssignmentState incumbent{test.problem.identity(), std::vector<std::int32_t>(
            test.problem.incumbentChoices().begin(), test.problem.incumbentChoices().end())};
        const auto size = test.problem.resources().size() + session.futureCatalog()->columns.size();
        const auto at = [&](std::vector<MilliFunds> prices)
        {
            return bounds.boundAt(incumbent, MasterBoundFamily::CombinedCurrentFuture, std::move(prices), ledger);
        };
        const auto zero = at(std::vector<MilliFunds>(size, 0));
        expect(zero.valid() && bounds.verify(*zero.certificate, ledger).valid(),
               "the public bound at zero prices is a verifiable certificate");
        auto tuned = std::vector<MilliFunds>(test.problem.resources().size(), 0);
        tuned.insert(tuned.end(), cap.futurePrices.begin(), cap.futurePrices.end());
        const auto repriced = at(tuned);
        expect(repriced.valid() && cap.scanUpper == repriced.certificate->upper &&
                   repriced.certificate->upper <= zero.certificate->upper,
               "the public bound at the tuned prices reproduces the search's scan and is no looser than zero prices");
        auto negative = std::vector<MilliFunds>(size, 0);
        negative.back() = -1;
        expect(at(negative).status == MasterBoundStatus::InvalidCertificate,
               "the public bound rejects a negative price");
        expect(at(std::vector<MilliFunds>(size - 1, 0)).status == MasterBoundStatus::InvalidIdentity &&
                   at(std::vector<MilliFunds>(size + 1, 0)).status == MasterBoundStatus::InvalidIdentity,
               "the public bound rejects a wrongly sized price vector");
    }

    auto incumbentExact(const PlanPricingCase & test)
    {
        const auto exact = MasterAssignmentOracle::evaluateObjective(
            MasterAssignmentOracle::constructReference(test.problem, operationIncumbent(test.problem).choices),
            test.reference.continuation, ORACLE_STATES);
        require(exact.status == MasterAssignmentOracle::ObjectiveStatus::Exact, "the incumbent has an exact oracle value");
        return exact;
    }

    void planBoundCap()
    {
        std::uint64_t reliedAndInstalled = 0;
        for (const auto & test : planPricingCases())
        {
            const auto exact = incumbentExact(test);
            for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
            {
                for (const auto friendly : {FriendlySearch::Immediate, FriendlySearch::Deferred})
                {
                    const auto off = runPlanPricing(test, planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS),
                                                    PLAN_PRICING_TEST_LEDGER, friendly);
                    const auto on = runPlanPricing(test, boundCapSchedule(allocation), PLAN_PRICING_TEST_LEDGER, friendly);
                    checkPlanPricingSoundness(test, on);
                    expect(!off.result.stats.planPricing.boundCap && !hasBoundCapRecord(off) && hasBoundCapRecord(on),
                           "the cap record exists exactly when the cap is on");
                    const auto & recorded = on.result.stats.planPricing.boundCap;
                    require(recorded.has_value(), "a capped search records its cap attempt");
                    const auto & cap = *recorded;
                    require(cap.status == MasterPlanBoundCapStatus::Registered && cap.scanUpper && cap.oursCeiling &&
                                cap.incumbentOursUpperBefore && cap.work > 0,
                            "every fixture registers a verified cap");
                    expect(*cap.oursCeiling >= exact.friendly && *cap.scanUpper == *cap.oursCeiling + exact.seated + exact.owned,
                           "the registered ceiling is the scan minus the oracle's seated and owned terms and bounds its exact stock");
                    checkBoundAtMethod(test, cap);
                    const auto & proof = on.result.improvement;
                    const bool relied = proof && (proof->incumbentOursCap || proof->challengerOursCap);
                    if (relied)
                    {
                        const auto improvement = on.record.value(QStringLiteral("improvement")).toObject();
                        expect(on.installed == MasterInstallationStatus::Installed &&
                                   proof->incumbentOursCap == cap.oursCeiling &&
                                   improvement.value(QStringLiteral("incumbentOursCap")).toString() ==
                                       QString::number(*cap.oursCeiling),
                               "a proof that relies on the cap records it and installs");
                        reliedAndInstalled += on.installed == MasterInstallationStatus::Installed ? 1 : 0;
                    }
                    const auto & first = on.result.stats.firstImprovementWork;
                    const auto & firstOff = off.result.stats.firstImprovementWork;
                    expect((!firstOff || (first && *first <= *firstOff)) && noWorse(on, off) && on.refusals == off.refusals,
                           "a registered cap finds the improvement no later and loses no bound");
                    std::printf("  plan bound cap %s %s %s: status %d scan %lld ceiling %lld exact ours %lld "
                                "upper before %lld | first improvement on %lld off %lld | delta on %lld off %lld | "
                                "relied %d improved at cap %d work %lld\n",
                                test.name, allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                                friendly == FriendlySearch::Deferred ? "loose" : "immediate", static_cast<int>(cap.status),
                                static_cast<long long>(*cap.scanUpper), static_cast<long long>(*cap.oursCeiling),
                                static_cast<long long>(exact.friendly),
                                static_cast<long long>(*cap.incumbentOursUpperBefore),
                                static_cast<long long>(first.value_or(-1)), static_cast<long long>(firstOff.value_or(-1)),
                                static_cast<long long>(proof ? proof->comparison.deltaLower : -1),
                                static_cast<long long>(off.result.improvement
                                    ? off.result.improvement->comparison.deltaLower : -1),
                                relied, cap.improvedAtCap, static_cast<long long>(cap.work));
                }
            }
        }
        expect(reliedAndInstalled > 0, "some fixture installs a proof that relies on the cap");
    }

    void planBoundCapRejections()
    {
        const auto test = planPricingCases().front();
        const auto exact = MasterAssignmentOracle::enumerate(test.problem, test.reference);
        require(exact.representative.has_value(), "the rejection fixture has an exact optimum");
        const auto optimumChoices = exact.tuples[*exact.representative].choices;
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "cap rejection ledger");
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(test.problem, ledger, session, TARIFFS);
        MasterAssignmentBounds bounds(test.problem);
        require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                "cap rejection bounds prepare");
        auto incumbent = evaluator.evaluate(operationIncumbent(test.problem));
        auto challenger = evaluator.evaluate({test.problem.identity(), optimumChoices});
        require(incumbent.quote() && challenger.quote() && incumbent.state.choices != challenger.state.choices,
                "the rejection fixture evaluates two distinct plans");
        const auto size = test.problem.resources().size() + session.futureCatalog()->columns.size();
        const std::vector<MilliFunds> zero(size, 0);
        const auto good = bounds.boundAt(incumbent.state, MasterBoundFamily::CombinedCurrentFuture, zero, ledger);
        require(good.valid(), "the incumbent has a trusted scan certificate");
        const auto before = evaluator.compare(incumbent, challenger);
        require(before.valid(), "the uncapped comparison is valid");
        const auto same = [](const RelativeStockComparison & left, const RelativeStockComparison & right)
        {
            return left.reason == right.reason && left.deltaLower == right.deltaLower &&
                   left.deltaUpper == right.deltaUpper && left.economicDelta == right.economicDelta &&
                   left.ownedDelta == right.ownedDelta;
        };
        const auto rejected = [&](const MasterAssignmentEvaluation & evaluation, const MasterBoundCertificate & certificate,
                                  const MasterAssignmentBounds & verifier, const char* message)
        {
            const auto receipt = evaluator.capOursUpper(evaluation, certificate, verifier);
            expect(receipt.status == MasterOursCapStatus::Rejected && !receipt.ceiling &&
                       !evaluator.oursCap(incumbent) && evaluator.oursCapContradictions() == 0 &&
                       same(evaluator.compare(incumbent, challenger), before),
                   message);
        };
        const auto other = bounds.boundAt(challenger.state, MasterBoundFamily::CombinedCurrentFuture, zero, ledger);
        require(other.valid(), "the challenger has its own certificate");
        rejected(incumbent, *other.certificate, bounds, "a certificate for another state is rejected");
        auto partialState = incumbent.state;
        for (const auto & actor : test.problem.actors())
        {
            if (test.problem.canChangeActor(actor.canonicalSlot))
            {
                partialState.choices[static_cast<std::size_t>(actor.canonicalSlot)] = MASTER_UNASSIGNED;
                break;
            }
        }
        const auto partial = bounds.boundAt(partialState, MasterBoundFamily::CombinedCurrentFuture, zero, ledger);
        require(!partialState.complete() && partial.valid(), "an incomplete state has a scan certificate");
        rejected(incumbent, *partial.certificate, bounds, "a certificate for an incomplete state is rejected");
        rejected(evaluator.evaluate(partialState), *partial.certificate, bounds,
                 "an incomplete evaluation cannot register a cap");
        const auto endpoint = bounds.boundAt(incumbent.state, MasterBoundFamily::EndpointCoupled, zero, ledger);
        require(endpoint.valid(), "the incumbent has an endpoint certificate");
        rejected(incumbent, *endpoint.certificate, bounds, "a certificate of another family is rejected");
        auto lowered = *good.certificate;
        --lowered.upper;
        rejected(incumbent, lowered, bounds, "a lowered upper fails verification");
        auto raised = *good.certificate;
        ++raised.upper;
        rejected(incumbent, raised, bounds, "a raised upper fails verification");
        auto negative = *good.certificate;
        negative.prices.back() = -1;
        rejected(incumbent, negative, bounds, "a negative price fails verification");
        auto longer = *good.certificate;
        longer.prices.push_back(0);
        rejected(incumbent, longer, bounds, "a longer price vector fails verification");
        auto shorter = *good.certificate;
        shorter.prices.pop_back();
        rejected(incumbent, shorter, bounds, "a shorter price vector fails verification");
        if (!test.problem.resources().empty())
        {
            auto current = zero;
            current.front() = 1;
            const auto priced = bounds.boundAt(incumbent.state, MasterBoundFamily::CombinedCurrentFuture, current, ledger);
            if (priced.valid())
            {
                rejected(incumbent, *priced.certificate, bounds, "a priced current resource is outside the premise");
            }
        }
        MasterAssignmentBounds uncovered(test.problem);
        require(uncovered.prepare(session.pricingModel(), ledger) == MasterBoundStatus::Valid, "uncovered bounds prepare");
        rejected(incumbent, *good.certificate, uncovered, "bounds without the session's catalog cannot verify a cap");
        auto unreplayed = incumbent;
        unreplayed.lowerReplayed = false;
        rejected(unreplayed, *good.certificate, bounds, "an evaluation whose lower was not replayed cannot register a cap");
        auto unverified = incumbent;
        unverified.objectiveStatus = MasterObjectiveStatus::Unavailable;
        rejected(unverified, *good.certificate, bounds, "an evaluation whose objective is not verified cannot register a cap");
        {
            RefinementLedger starvedLedger;
            require(starvedLedger.open(PLAN_PRICING_TEST_LEDGER), "starved cap ledger");
            NativeStockSession starvedSession(field, enemy, test.reference.continuation.cases, starvedLedger);
            MasterAssignmentEvaluator starvedEvaluator(test.problem, starvedLedger, starvedSession, TARIFFS);
            MasterAssignmentBounds starvedBounds(test.problem);
            require(starvedBounds.prepare(starvedSession.pricingModel(), starvedLedger, starvedSession.futureCatalog()) ==
                        MasterBoundStatus::Valid, "starved cap bounds prepare");
            const auto starvedIncumbent = starvedEvaluator.evaluate(operationIncumbent(test.problem));
            const auto starvedCertificate = starvedBounds.boundAt(starvedIncumbent.state,
                MasterBoundFamily::CombinedCurrentFuture, zero, starvedLedger);
            require(starvedIncumbent.quote() && starvedCertificate.valid() &&
                        starvedLedger.consume(starvedLedger.balance(), RefinementWork::MasterValuation),
                    "the starved cap fixture drains its ledger after certifying");
            const auto refusals = starvedLedger.stats().refusals;
            const auto starvedReceipt = starvedEvaluator.capOursUpper(starvedIncumbent, *starvedCertificate.certificate,
                                                                      starvedBounds);
            expect(starvedReceipt.status == MasterOursCapStatus::Rejected && !starvedReceipt.ceiling &&
                       !starvedEvaluator.oursCap(starvedIncumbent) && starvedLedger.stats().refusals == refusals + 1,
                   "a cap on an exhausted ledger is refused, records the refusal and stores nothing");
        }
        const auto receipt = evaluator.capOursUpper(incumbent, *good.certificate, bounds);
        const auto exactIncumbent = MasterAssignmentOracle::evaluateObjective(
            MasterAssignmentOracle::constructReference(test.problem, incumbent.state.choices),
            test.reference.continuation, ORACLE_STATES);
        require(receipt.status == MasterOursCapStatus::Registered && receipt.ceiling &&
                    receipt.verifiedUpper == good.certificate->upper && *receipt.ceiling >= exactIncumbent.friendly,
                "the genuine certificate registers a sound cap");
        const auto capped = evaluator.compare(incumbent, challenger);
        expect(capped.valid() && capped.deltaLower >= before.deltaLower && capped.deltaUpper == before.deltaUpper,
               "a cap only lowers the incumbent upper");
        auto inflated = incumbent;
        inflated.replay->seatedEconomicTotal += CONTRADICTION_OWNED_INFLATION;
        const auto contradicted = evaluator.compare(inflated, challenger);
        expect(!contradicted.valid() && contradicted.reason == RelativeStockReason::InvalidInterval &&
                   evaluator.oursCapContradictions() == 1 && !evaluator.oursCap(incumbent),
               "a cap below a replayed lower invalidates the comparison and disables every cap");
        expect(same(evaluator.compare(incumbent, challenger), before) &&
                   evaluator.capOursUpper(incumbent, *good.certificate, bounds).status == MasterOursCapStatus::Rejected,
               "after a contradiction comparisons are uncapped and no new cap registers");
        RefinementLedger freshLedger;
        require(freshLedger.open(PLAN_PRICING_TEST_LEDGER), "registration contradiction ledger");
        NativeStockSession freshSession(field, enemy, test.reference.continuation.cases, freshLedger);
        MasterAssignmentEvaluator freshEvaluator(test.problem, freshLedger, freshSession, TARIFFS);
        MasterAssignmentBounds freshBounds(test.problem);
        require(freshBounds.prepare(freshSession.pricingModel(), freshLedger, freshSession.futureCatalog()) ==
                    MasterBoundStatus::Valid, "registration contradiction bounds prepare");
        auto overstated = freshEvaluator.evaluate(operationIncumbent(test.problem));
        const auto freshCertificate =
            freshBounds.boundAt(overstated.state, MasterBoundFamily::CombinedCurrentFuture, zero, freshLedger);
        require(freshCertificate.valid(), "registration contradiction certificate");
        overstated.replay->seatedEconomicTotal += CONTRADICTION_OWNED_INFLATION;
        expect(freshEvaluator.capOursUpper(overstated, *freshCertificate.certificate, freshBounds).status ==
                   MasterOursCapStatus::Contradiction && freshEvaluator.oursCapContradictions() == 1,
               "a registration whose ceiling falls below the replayed lower is a contradiction");
    }

    void planBoundCapContradiction()
    {
        const auto cases = planPricingCases();
        const auto found = std::find_if(cases.begin(), cases.end(),
            [](const PlanPricingCase & entry) { return std::string_view(entry.name) == "capture"; });
        require(found != cases.end(), "the capture fixture exists");
        auto test = *found;
        auto & continuation = test.reference.continuation.cases;
        const auto incumbentKey = test.problem.incumbentContinuationKey();
        for (auto & entry : continuation)
        {
            if (entry.key == incumbentKey)
            {
                entry.owned += CONTRADICTION_OWNED_INFLATION;
            }
        }
        // The session's owned term for the incumbent now disagrees with the stock model the scan reads.
        std::stable_partition(continuation.begin(), continuation.end(),
                              [&](const ContinuationCase & entry) { return entry.key != incumbentKey; });
        for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
        {
            const auto run = runPlanPricing(test, boundCapSchedule(allocation));
            const auto & cap = run.result.stats.planPricing.boundCap;
            expect(cap && cap->status == MasterPlanBoundCapStatus::OwnedMismatch && !cap->oursCeiling &&
                       !run.heldIncumbentCap && run.capContradictions == 0 && !run.result.improvement &&
                       run.result.stats.proofFailures > 0 && run.refusals == 0,
                   "an inflated owned term is rejected before registration and counts as a proof failure");
        }
    }

    constexpr MilliFunds OWNED_GUARD_OFFSET = 1;

    bool capped(const MasterImprovementProof & proof)
    {
        return proof.incumbentOursCap || proof.challengerOursCap;
    }

    bool registered(const PlanPricingRun & run)
    {
        const auto & cap = run.result.stats.planPricing.boundCap;
        return cap && cap->status == MasterPlanBoundCapStatus::Registered;
    }

    using CapBenchBody = std::function<void(NativeStockSession &, MasterAssignmentEvaluator &, MasterAssignmentEvaluation &,
                                            const MasterBoundCertificate &, const MasterAssignmentBounds &)>;

    // Rebuilds the search's tuned certificate for the incumbent on a fresh evaluator.
    void withTunedCapCertificate(const PlanPricingCase & test, std::span<const MilliFunds> futurePrices,
                                 MilliFunds ownedOffset, const CapBenchBody & body)
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "cap bench ledger");
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        session.ownedOffset = ownedOffset;
        MasterAssignmentEvaluator evaluator(test.problem, ledger, session, TARIFFS);
        MasterAssignmentBounds bounds(test.problem);
        require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                "cap bench bounds prepare");
        auto incumbent = evaluator.evaluate(operationIncumbent(test.problem));
        std::vector<MilliFunds> prices(test.problem.resources().size(), 0);
        prices.insert(prices.end(), futurePrices.begin(), futurePrices.end());
        const auto certificate = bounds.boundAt(incumbent.state, MasterBoundFamily::CombinedCurrentFuture,
                                                std::move(prices), ledger);
        require(incumbent.quote() && certificate.valid(), "cap bench certificate");
        body(session, evaluator, incumbent, *certificate.certificate, bounds);
    }

    void planBoundCapOwnedGuard()
    {
        std::uint64_t silent = 0;
        for (const auto & test : planPricingCases())
        {
            const auto exact = incumbentExact(test);
            for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
            {
                const auto plain = runPlanPricing(test, planPricingSchedule(allocation, PLAN_PRICING_TEST_ROUNDS));
                const auto reference = runPlanPricing(test, boundCapSchedule(allocation));
                require(registered(reference), "the owned guard reference registers a cap");
                std::optional<MilliFunds> unguarded;
                MilliFunds lower = 0;
                withTunedCapCertificate(test, reference.result.stats.planPricing.boundCap->futurePrices,
                    OWNED_GUARD_OFFSET, [&](NativeStockSession &, MasterAssignmentEvaluator & evaluator,
                                            MasterAssignmentEvaluation & incumbent,
                                            const MasterBoundCertificate & certificate,
                                            const MasterAssignmentBounds & bounds)
                    {
                        const auto & quote = *incumbent.quote();
                        lower = quote.ours.lower;
                        // The ceiling the unguarded registration would have stored for this shifted quote.
                        unguarded = certificate.upper - incumbent.replay->seatedEconomicTotal - quote.owned;
                        if (*unguarded >= exact.friendly || *unguarded < lower)
                        {
                            return;
                        }
                        const auto receipt = evaluator.capOursUpper(incumbent, certificate, bounds);
                        const auto used = evaluator.comparisonQuote(incumbent);
                        expect(receipt.status == MasterOursCapStatus::OwnedMismatch && !receipt.ceiling &&
                                   !evaluator.oursCap(incumbent) && evaluator.oursCapContradictions() == 0 &&
                                   used && used->ours.upper == quote.ours.upper,
                               "a quote whose owned term is off by one is rejected and nothing is stored");
                    });
                if (!unguarded || *unguarded >= exact.friendly || *unguarded < lower)
                {
                    continue;
                }
                ++silent;
                const auto shifted = runPlanPricing(test, boundCapSchedule(allocation), PLAN_PRICING_TEST_LEDGER,
                    FriendlySearch::Immediate, {.before = [](NativeStockSession & session)
                    {
                        session.ownedOffset = OWNED_GUARD_OFFSET;
                    }});
                const auto & cap = *shifted.result.stats.planPricing.boundCap;
                expect(cap.status == MasterPlanBoundCapStatus::OwnedMismatch && !cap.oursCeiling &&
                           !shifted.heldIncumbentCap && shifted.capContradictions == 0 &&
                           shifted.result.stats.proofFailures == 1 && shifted.refusals == reference.refusals,
                       "a quote whose owned term is off by one is rejected and nothing is stored");
                expect(!shifted.result.improvement || !capped(*shifted.result.improvement),
                       "a rejected cap leaves every proof uncapped");
                checkImprovementIsReal(test, shifted);
                expect(shifted.result.improvement.has_value() == plain.result.improvement.has_value(),
                       "the search still finds what the uncapped search finds");
                std::printf("  owned guard %s %s: unguarded ceiling %lld exact ours %lld lower %lld | status %d | "
                            "delta shifted %lld plain %lld\n", test.name,
                            allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                            static_cast<long long>(*unguarded), static_cast<long long>(exact.friendly),
                            static_cast<long long>(lower), static_cast<int>(cap.status),
                            static_cast<long long>(shifted.result.improvement
                                ? shifted.result.improvement->comparison.deltaLower : -1),
                            static_cast<long long>(plain.result.improvement
                                ? plain.result.improvement->comparison.deltaLower : -1));
            }
        }
        expect(silent > 0, "some fixture leaves room for a silently lowered ceiling");
    }

    void planBoundCapCatalogGuard()
    {
        const auto test = planPricingCases().front();
        const auto exact = incumbentExact(test).friendly;
        const auto run = runPlanPricing(test, boundCapSchedule(MasterRefinementAllocation::Baseline),
            PLAN_PRICING_TEST_LEDGER, FriendlySearch::Immediate, {.before = [](NativeStockSession & session)
            {
                session.catalogMismatch = true;
            }});
        const auto & cap = *run.result.stats.planPricing.boundCap;
        expect(cap.status == MasterPlanBoundCapStatus::CatalogMismatch && !cap.oursCeiling && !run.heldIncumbentCap &&
                   run.capContradictions == 0 && run.result.stats.proofFailures == 1 && run.refusals == 0,
               "a session whose catalog does not describe its solver registers no cap");
        expect(!run.result.improvement || !capped(*run.result.improvement), "no proof relies on a refused catalog");
        checkImprovementIsReal(test, run);
        const auto reference = runPlanPricing(test, boundCapSchedule(MasterRefinementAllocation::Baseline));
        require(registered(reference), "the catalog guard reference registers a cap");
        withTunedCapCertificate(test, reference.result.stats.planPricing.boundCap->futurePrices, 0,
            [&](NativeStockSession & session, MasterAssignmentEvaluator & evaluator, MasterAssignmentEvaluation & incumbent,
                const MasterBoundCertificate & certificate, const MasterAssignmentBounds & bounds)
            {
                session.catalogMismatch = true;
                expect(evaluator.capOursUpper(incumbent, certificate, bounds).status == MasterOursCapStatus::CatalogMismatch &&
                           !evaluator.oursCap(incumbent) && evaluator.oursCapContradictions() == 0,
                       "a disagreeing session cannot register a cap");
                session.catalogMismatch = false;
                const auto receipt = evaluator.capOursUpper(incumbent, certificate, bounds);
                require(receipt.status == MasterOursCapStatus::Registered && *receipt.ceiling >= exact &&
                            evaluator.oursCap(incumbent) == receipt.ceiling,
                        "the agreeing session registers a sound cap");
                session.catalogMismatch = true;
                expect(!evaluator.oursCap(incumbent) && !evaluator.comparisonQuote(incumbent),
                       "a catalog that stops describing the solver withdraws the stored cap at use");
                session.catalogMismatch = false;
                expect(evaluator.oursCap(incumbent) == receipt.ceiling, "the cap applies again once the session agrees");
                session.ownedOffset = OWNED_GUARD_OFFSET;
                auto shifted = evaluator.evaluate(operationIncumbent(test.problem));
                expect(evaluator.oursCapContradictions() == 0 && !evaluator.comparisonQuote(shifted) &&
                           !evaluator.oursCap(shifted),
                       "a quote whose owned term moved after registration cannot use the stored cap");
            });
    }

    // Moves profitable enough that the seeded challenger improves before the cap, and by more once it is capped.
    PlanPricingCase profitableMovesCase()
    {
        constexpr std::int32_t ACTORS = 3;
        constexpr MilliFunds MOVE_PROFIT = 100;
        return planPricingCase("profitable moves", stockMoveFixture(ACTORS, [](std::int32_t) { return MOVE_PROFIT; }));
    }

    // The pre-pass compares the seeded challenger first, uncapped, then again once the cap is registered.
    constexpr std::uint64_t CAPPED_PREPASS_COMPARISON = 2;

    void planBoundCapFallback()
    {
        const auto test = profitableMovesCase();
        for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
        {
            for (const auto friendly : {FriendlySearch::Immediate, FriendlySearch::Deferred})
            {
                std::uint64_t comparisons = 0;
                std::optional<MilliFunds> cappedDelta;
                const PlanPricingHooks hooks{.compared =
                    [&](NativeStockSession & session, const RelativeStockComparison & comparison)
                    {
                        if (++comparisons == CAPPED_PREPASS_COMPARISON)
                        {
                            cappedDelta = comparison.deltaLower;
                            session.catalogMismatch = true;
                        }
                    }};
                const auto run = runPlanPricing(test, boundCapSchedule(allocation), PLAN_PRICING_TEST_LEDGER,
                                                friendly, hooks);
                const auto & pricing = run.result.stats.planPricing;
                require(pricing.boundCap.has_value(), "the fallback search records its cap attempt");
                const auto & cap = *pricing.boundCap;
                require(pricing.firstComparisonImproved && cap.improvedAtCap && cappedDelta,
                        "the fallback fixture proves an uncapped and then a capped improvement");
                expect(run.capContradictions == 1 && cap.status == MasterPlanBoundCapStatus::Contradiction &&
                           cap.cappedProofDropped && run.result.stats.proofFailures == 1,
                       "an invalidated cap is counted and drops the proof that relied on it");
                expect(run.result.improvement && !capped(*run.result.improvement) &&
                           run.installed == MasterInstallationStatus::Installed,
                       "the best uncapped proof is reported and installs");
                checkImprovementIsReal(test, run);
                expect(run.result.stats.firstImprovementWork && run.result.improvement &&
                           *run.result.stats.firstImprovementWork <= run.result.improvement->work,
                       "the reported first improvement belongs to an uncapped proof");
                std::printf("  cap fallback %s %s: delta capped %lld reported %lld | first improvement %lld\n",
                            allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                            friendly == FriendlySearch::Deferred ? "loose" : "immediate",
                            static_cast<long long>(*cappedDelta),
                            static_cast<long long>(run.result.improvement
                                ? run.result.improvement->comparison.deltaLower : -1),
                            static_cast<long long>(run.result.stats.firstImprovementWork.value_or(-1)));
            }
        }
    }

    void planBoundCapInstallRule()
    {
        {
            const auto test = profitableMovesCase();
            const auto schedule = boundCapSchedule(MasterRefinementAllocation::Baseline);
            const auto starving = [&](const PlanPricingHooks & extra = {})
            {
                auto hooks = extra;
                hooks.before = [](NativeStockSession & session) { session.starveAfterHint = true; };
                return runPlanPricing(test, schedule, PLAN_PRICING_TEST_LEDGER, FriendlySearch::Immediate, hooks);
            };
            const auto run = starving();
            const auto & pricing = run.result.stats.planPricing;
            require(registered(run) && pricing.firstComparisonImproved && !pricing.boundCap->improvedAtCap &&
                        run.result.budgetExhausted,
                    "the search proves uncapped, registers the cap, then starves the capped re-compare");
            expect(run.heldIncumbentCap && run.result.improvement && !capped(*run.result.improvement) &&
                       run.installed == MasterInstallationStatus::Installed,
                   "an uncapped proof recorded before the cap still installs after the re-compare is starved");
            checkImprovementIsReal(test, run);
            std::printf("  cap install rule: starved re-compare, held cap %lld, delta %lld, installed %d\n",
                        static_cast<long long>(run.heldIncumbentCap.value_or(-1)),
                        static_cast<long long>(run.result.improvement ? run.result.improvement->comparison.deltaLower : -1),
                        static_cast<int>(run.installed));
            const auto refused = starving({.tamper = [&](MasterAssignmentSearchResult & result)
                {
                    result.improvement->incumbentOursCap = run.heldIncumbentCap;
                }});
            expect(refused.installed == MasterInstallationStatus::InvalidContext,
                   "a ceiling the proof's comparison never applied is refused");
        }
        for (const auto & test : planPricingCases())
        {
            const auto schedule = boundCapSchedule(MasterRefinementAllocation::Baseline);
            const auto run = runPlanPricing(test, schedule);
            if (!run.result.improvement || !run.result.improvement->incumbentOursCap)
            {
                continue;
            }
            const auto ceiling = *run.result.improvement->incumbentOursCap;
            for (const auto & recorded : {std::optional<MilliFunds>{ceiling - 1}, std::optional<MilliFunds>{ceiling + 1},
                                          std::optional<MilliFunds>{}})
            {
                const auto tampered = runPlanPricing(test, schedule, PLAN_PRICING_TEST_LEDGER, FriendlySearch::Immediate,
                    {.tamper = [&](MasterAssignmentSearchResult & result)
                    {
                        result.improvement->incumbentOursCap = recorded;
                    }});
                expect(tampered.installed == MasterInstallationStatus::InvalidContext,
                       "a proof whose recorded cap differs from the verified one is refused");
            }
            expect(run.installed == MasterInstallationStatus::Installed, "the untampered capped proof installs");
        }
    }

    enum class RowCheck : std::int8_t
    {
        None,
        ActionRowDiffers,
    };

    struct OracleCapCase
    {
        PlanPricingCase test;
        const char* property;
        RowCheck rows{RowCheck::None};
        // The unedited fixture's exhaustive optimum, which the edit must change.
        std::optional<MilliFunds> uneditedOptimum;
    };

    void addDistinctClass(ContinuationCase & entry)
    {
        entry.friendly.classes.push_back({1, {0, 1, 1, 1, 0, 1, 1, 1, 0}});
        entry.friendly.rows.front().classIndex = 1;
    }

    void addValuedNonActorRow(ContinuationCase & entry)
    {
        constexpr std::int32_t NON_ACTOR_X = 2;
        SequentialRow row{.classIndex = 0};
        for (const auto & column : entry.friendly.columns)
        {
            const auto turn = 2 + std::abs(column.tile.x - NON_ACTOR_X);
            row.ownedTurn.push_back(sequentialFirstOwnedTurn(turn, entry.friendly.horizon));
            row.weight.push_back(columnStreamValue(column, turn, entry.friendly.horizon));
        }
        entry.friendly.rows.push_back(std::move(row));
    }

    PlanPricingCase sharedCaptureCase()
    {
        constexpr std::int32_t ACTORS = 2;
        constexpr std::int32_t CAPTURE = 1;
        constexpr std::int32_t STEP_ASIDE = 2;
        constexpr MilliFunds STEP_COST = -1;
        const TilePoint property{0, 2};
        Fixture fixture(ACTORS);
        fixture.input.unitLinks[0].tile = property;
        auto wait = moveCandidate(0, property, property, 0);
        auto capture = wait;
        capture.generationIndex = CAPTURE;
        capture.bundle.components.push_back(
            captureComponent({.income = {CAPTURE_INCOME, CAPTURE_INCOME}, .turnsUntilOwned = 0}));
        fixture.input.actors[0].candidates = {wait, capture, moveCandidate(0, property, {0, 3}, STEP_COST, STEP_ASIDE)};
        auto moveCapture = moveCandidate(1, {1, 0}, property, 0, CAPTURE);
        moveCapture.bundle.components.push_back(
            captureComponent({.income = {CAPTURE_INCOME, CAPTURE_INCOME}, .turnsUntilOwned = 0}));
        fixture.input.actors[1].candidates.push_back(moveCapture);
        fixture.seat();
        for (std::int32_t actor = 0; actor < ACTORS; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        return planPricingCase("shared capture", std::move(fixture),
            [](ContinuationCase & entry, const std::vector<std::int32_t> & choices)
            {
                for (std::size_t actor = 0; actor < choices.size(); ++actor)
                {
                    const auto action = std::find_if(entry.key.begin(), entry.key.end(),
                        [&](const CanonicalPlanActionKey & key) { return key.row() == static_cast<std::int32_t>(actor); });
                    if (choices[actor] == CAPTURE && action != entry.key.end())
                    {
                        entry.capturedColumns = {0};
                        entry.friendly.capturedNodes[0] = true;
                        removeCapturedNode(entry.friendly.rows[static_cast<std::size_t>(action - entry.key.begin())], 0);
                        entry.owned += ownershipFlipSwing(entry.friendly.columns[0], entry.friendly.horizon);
                    }
                }
            });
    }

    std::vector<OracleCapCase> oracleCapCases()
    {
        std::vector<OracleCapCase> result;
        {
            auto test = planPricingCases().front();
            auto & cases = test.reference.continuation.cases;
            std::rotate(cases.begin(), cases.begin() + 1, cases.end());
            result.push_back({std::move(test), "incumbent action rows differ from the day rows",
                              RowCheck::ActionRowDiffers});
        }
        result.push_back({sharedCaptureCase(), "a captured node reachable from two rows that can both capture it"});
        const auto unedited = planPricingCases().front().optimum;
        result.push_back({planPricingCase("non-actor row", stockInstallationFixture(),
            [](ContinuationCase & entry, const std::vector<std::int32_t> &) { addValuedNonActorRow(entry); }),
            "a valued row no actor drives", RowCheck::None, unedited});
        // Fewer rows than nodes, so a row can continue to a second node and its class matters.
        constexpr std::int32_t CLASS_ACTORS = 2;
        constexpr MilliFunds CLASS_MOVE_COST = -3;
        const auto classFixture = [] { return stockMoveFixture(CLASS_ACTORS, [](std::int32_t) { return CLASS_MOVE_COST; }); };
        result.push_back({planPricingCase("two classes", classFixture(),
            [](ContinuationCase & entry, const std::vector<std::int32_t> &) { addDistinctClass(entry); }),
            "rows of two sequential classes", RowCheck::None, planPricingCase("one class", classFixture()).optimum});
        return result;
    }

    bool actionRowDiffersFromDayRow(const PlanPricingCase & test)
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "row comparison ledger");
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        const auto & model = *session.pricingModel();
        const auto key = test.problem.incumbentContinuationKey();
        return std::any_of(key.begin(), key.end(), [&](const CanonicalPlanActionKey & action)
        {
            const auto* terms = model.actionTerms(action);
            return terms != nullptr && !(terms->rowTerms == model.dayRows()[static_cast<std::size_t>(action.row())]);
        });
    }

    void planBoundCapOracleFixtures()
    {
        for (const auto & entry : oracleCapCases())
        {
            const auto & test = entry.test;
            const auto exact = incumbentExact(test).friendly;
            if (entry.rows == RowCheck::ActionRowDiffers)
            {
                expect(actionRowDiffersFromDayRow(test), "the rotated fixture's incumbent rows differ from its day rows");
            }
            expect(!entry.uneditedOptimum || entry.uneditedOptimum != test.optimum, "the fixture edit changes the optimum");
            for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
            {
                for (const auto friendly : {FriendlySearch::Immediate, FriendlySearch::Deferred})
                {
                    const auto run = runPlanPricing(test, boundCapSchedule(allocation), PLAN_PRICING_TEST_LEDGER, friendly);
                    const auto & cap = run.result.stats.planPricing.boundCap;
                    require(cap.has_value(), "an oracle fixture records its cap attempt");
                    expect(cap->status == MasterPlanBoundCapStatus::Registered && cap->oursCeiling &&
                               *cap->oursCeiling >= exact,
                           "an oracle fixture registers a ceiling no lower than the exact incumbent stock");
                    checkPlanPricingSoundness(test, run);
                    std::printf("  oracle cap %s (%s) %s %s: status %d ceiling %lld exact ours %lld optimum %lld | "
                                "relied %d delta %lld installed %d\n", test.name, entry.property,
                                allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                                friendly == FriendlySearch::Deferred ? "loose" : "immediate",
                                static_cast<int>(cap->status), static_cast<long long>(cap->oursCeiling.value_or(-1)),
                                static_cast<long long>(exact), static_cast<long long>(test.optimum.value_or(-1)),
                                run.result.improvement && capped(*run.result.improvement),
                                static_cast<long long>(run.result.improvement
                                    ? run.result.improvement->comparison.deltaLower : -1),
                                static_cast<int>(run.installed));
                }
            }
        }
    }

    constexpr std::int64_t STARVED_NEIGHBOR_AUDIT_ALLOWANCE = 1;

    MasterAssignmentSchedule neighborAuditSchedule(MasterRefinementAllocation allocation,
                                                   std::int64_t allowance = MASTER_PLAN_NEIGHBOR_AUDIT_ALLOWANCE)
    {
        auto schedule = boundCapSchedule(allocation);
        schedule.planNeighborAudit = MasterPlanNeighborAudit::Recorded;
        schedule.planNeighborAuditAllowance = allowance;
        return schedule;
    }

    std::vector<PlanPricingCase> neighborAuditCases()
    {
        auto result = planPricingCases();
        for (auto & entry : oracleCapCases())
        {
            result.push_back(std::move(entry.test));
        }
        return result;
    }

    QJsonObject withoutNeighborAudit(QJsonObject record)
    {
        auto pricing = record.value(QLatin1String(PLAN_PRICING_RECORD)).toObject();
        pricing.remove(QLatin1String(TRACE_NEIGHBOR_AUDIT));
        record.insert(QLatin1String(PLAN_PRICING_RECORD), pricing);
        return record;
    }

    bool hasNeighborAuditRecord(const PlanPricingRun & run)
    {
        return run.record.value(QLatin1String(PLAN_PRICING_RECORD)).toObject().contains(QLatin1String(TRACE_NEIGHBOR_AUDIT));
    }

    std::vector<std::int32_t> bestChoices(const MasterAssignmentSearchResult & result)
    {
        return result.best ? result.best->state.choices : std::vector<std::int32_t>{};
    }

    void expectNoNeighborInfluence(const PlanPricingRun & off, const PlanPricingRun & on)
    {
        const auto & plain = off.result;
        const auto & audited = on.result;
        expect(!plain.stats.planPricing.neighborAudit && !hasNeighborAuditRecord(off) && hasNeighborAuditRecord(on),
               "the audit record exists exactly when the audit is on");
        expect(withoutNeighborAudit(on.record) == off.record,
               "apart from its own block the audited search exports the same record, proof and cap included");
        // Installation re-verifies the cap through the bounds cache, so a disturbed cache would change its work.
        expect(on.ledgerAfterSearch == off.ledgerAfterSearch && on.ledgerAfterInstall == off.ledgerAfterInstall &&
                   on.refusals == off.refusals,
               "the real ledger's per-category statistics match after the search and after installation");
        const bool sameProof = audited.improvement.has_value() == plain.improvement.has_value() &&
            (!plain.improvement ||
             (audited.improvement->challenger.state.choices == plain.improvement->challenger.state.choices &&
              audited.improvement->comparison.deltaLower == plain.improvement->comparison.deltaLower &&
              audited.improvement->incumbentOursCap == plain.improvement->incumbentOursCap &&
              audited.improvement->challengerOursCap == plain.improvement->challengerOursCap));
        expect(sameProof && audited.lower == plain.lower && audited.upper == plain.upper &&
                   bestChoices(audited) == bestChoices(plain) && on.installed == off.installed &&
                   on.capContradictions == off.capContradictions && on.heldIncumbentCap == off.heldIncumbentCap,
               "the audit changes no proof, bound, best plan, installation or stored cap");
    }

    void checkTermTable(const PlanPricingCase & test, const MasterAssignmentState & plan,
                        const std::vector<MilliFunds> & prices)
    {
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "term table ledger");
        PropertyStockField field;
        PropertyStockInstance enemy;
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        MasterAssignmentBounds bounds(test.problem);
        require(bounds.prepare(session.pricingModel(), ledger, session.futureCatalog()) == MasterBoundStatus::Valid,
                "term table bounds prepare");
        const auto whole = bounds.boundAt(plan, MasterBoundFamily::CombinedCurrentFuture, prices, ledger);
        const auto cacheBefore = bounds.futureCacheStats();
        const auto table = bounds.termsAt(plan, prices, ledger);
        const auto cacheAfter = bounds.futureCacheStats();
        require(whole.valid() && table.status == MasterBoundStatus::Valid, "the plan has a bound and a term table");
        expect(table.upper == whole.certificate->upper, "the table's upper is the plan's bound");
        expect(cacheBefore.hits == cacheAfter.hits && cacheBefore.recomputations == cacheAfter.recomputations,
               "the term table neither reads nor fills the future price cache");
        for (std::size_t slot = 0; slot < table.actorTerms.size(); ++slot)
        {
            const auto selected = plan.choices[slot];
            const auto & terms = table.actorTerms[slot];
            expect(selected >= 0 ? terms.size() == test.problem.actors()[slot].canonicalCandidates.size() : terms.empty(),
                   "every canonical candidate of a chosen actor has an entry");
            const auto base = std::find_if(terms.begin(), terms.end(),
                [&](const MasterCandidateTerm & entry) { return entry.candidate == selected; });
            if (selected < 0)
            {
                continue;
            }
            require(base != terms.end() && base->term.has_value(), "the plan's own choice has a term");
            for (const auto & entry : terms)
            {
                if (!entry.term)
                {
                    continue;
                }
                auto changed = plan;
                changed.choices[slot] = entry.candidate;
                const auto bound = bounds.boundAt(changed, MasterBoundFamily::CombinedCurrentFuture, prices, ledger);
                expect(bound.valid() && bound.certificate->upper == table.upper + *entry.term - *base->term,
                       "the table's upper plus a change's gain is the changed plan's bound");
            }
        }
    }

    void planNeighborTermTable()
    {
        for (const auto & test : neighborAuditCases())
        {
            const auto run = runPlanPricing(test, neighborAuditSchedule(MasterRefinementAllocation::Baseline));
            require(registered(run), "the term table fixture registers a cap");
            const auto columns = run.result.stats.planPricing.boundCap->futurePrices;
            std::vector<MilliFunds> tuned(test.problem.resources().size(), 0);
            tuned.insert(tuned.end(), columns.begin(), columns.end());
            const std::vector<MilliFunds> zero(tuned.size(), 0);
            std::vector<MasterAssignmentState> plans{operationIncumbent(test.problem)};
            if (run.result.improvement)
            {
                plans.push_back(run.result.improvement->challenger.state);
            }
            for (const auto & plan : plans)
            {
                checkTermTable(test, plan, zero);
                checkTermTable(test, plan, tuned);
            }
        }
    }

    auto exactStock(const PlanPricingCase & test, const std::vector<std::int32_t> & choices)
    {
        return MasterAssignmentOracle::evaluateObjective(
            MasterAssignmentOracle::constructReference(test.problem, choices), test.reference.continuation, ORACLE_STATES);
    }

    void planNeighborAudit()
    {
        std::uint64_t provenChecked = 0;
        std::uint64_t conflictingSeen = 0;
        for (const auto & test : neighborAuditCases())
        {
            for (const auto allocation : {MasterRefinementAllocation::Baseline, MasterRefinementAllocation::OperationService})
            {
                for (const auto friendly : {FriendlySearch::Immediate, FriendlySearch::Deferred})
                {
                    const auto off = runPlanPricing(test, boundCapSchedule(allocation), PLAN_PRICING_TEST_LEDGER, friendly);
                    const auto on = runPlanPricing(test, neighborAuditSchedule(allocation), PLAN_PRICING_TEST_LEDGER,
                                                   friendly);
                    expectNoNeighborInfluence(off, on);
                    const auto & recorded = on.result.stats.planPricing.neighborAudit;
                    require(recorded.has_value() && recorded->status == MasterPlanNeighborAuditStatus::Recorded &&
                                recorded->scanUpper && recorded->trustedFloor && recorded->work > 0,
                            "every fixture records a funded audit");
                    const auto & audit = *recorded;
                    const bool challenger = on.result.improvement.has_value();
                    expect(audit.plan == (challenger ? MasterNeighborAuditedPlan::Challenger
                                                     : MasterNeighborAuditedPlan::Incumbent),
                           "the audit measures the challenger to be installed, or else the incumbent");
                    const auto planChoices = challenger ? on.result.improvement->challenger.state.choices
                                                        : operationIncumbent(test.problem).choices;
                    const auto planExact = exactStock(test, planChoices);
                    require(planExact.status == MasterAssignmentOracle::ObjectiveStatus::Exact,
                            "the audited plan has an exact oracle value");
                    const auto planStock = planExact.seated + planExact.owned + planExact.friendly;
                    expect(*audit.trustedFloor <= planStock && *audit.scanUpper >= planStock,
                           "the trusted floor and the scan bracket the audited plan's exact stock");
                    expect(audit.witnessValue && *audit.witnessValue <= planStock && *audit.witnessValue <= *audit.scanUpper,
                           "the tuner's witness, in the scan's units, is a feasible value under the ceiling");
                    expect(audit.provenNotBetter + audit.open + audit.conflicting + audit.unavailable ==
                               audit.changes.size() && audit.nanos > 0,
                           "the class counts add up to the recorded changes and the audit times itself");
                    for (const auto & change : audit.changes)
                    {
                        const bool screened = change.verdict == MasterNeighborVerdict::ProvenNotBetter ||
                                              change.verdict == MasterNeighborVerdict::Open;
                        expect((change.verdict == MasterNeighborVerdict::Conflicting) ==
                                       (change.gain.has_value() && !change.conflictFree) &&
                                   (!screened || (change.gain.has_value() && change.conflictFree)),
                               "a conflicting change keeps its gain and only conflict-free changes are screened");
                        conflictingSeen += change.verdict == MasterNeighborVerdict::Conflicting ? 1 : 0;
                        auto choices = planChoices;
                        choices[static_cast<std::size_t>(change.slot)] = change.candidate;
                        const auto changedExact = exactStock(test, choices);
                        if (!screened || changedExact.status != MasterAssignmentOracle::ObjectiveStatus::Exact)
                        {
                            continue;
                        }
                        const auto changedStock = changedExact.seated + changedExact.owned + changedExact.friendly;
                        expect(*audit.scanUpper + *change.gain >= changedStock,
                               "a change's ceiling bounds the changed plan's exact stock");
                        if (change.verdict == MasterNeighborVerdict::ProvenNotBetter)
                        {
                            ++provenChecked;
                            expect(changedStock <= planStock, "a change screened as not better is not better");
                        }
                    }
                    std::printf("  neighbor audit %s %s %s: plan %d upper %lld floor %lld witness %lld | "
                                "proven %llu open %llu conflicting %llu unavailable %llu | rounds %d work %lld\n",
                                test.name, allocation == MasterRefinementAllocation::OperationService ? "operation" : "baseline",
                                friendly == FriendlySearch::Deferred ? "loose" : "immediate", static_cast<int>(audit.plan),
                                static_cast<long long>(*audit.scanUpper), static_cast<long long>(*audit.trustedFloor),
                                static_cast<long long>(audit.witnessValue.value_or(-1)),
                                static_cast<unsigned long long>(audit.provenNotBetter),
                                static_cast<unsigned long long>(audit.open),
                                static_cast<unsigned long long>(audit.conflicting),
                                static_cast<unsigned long long>(audit.unavailable), audit.rounds,
                                static_cast<long long>(audit.work));
                }
            }
        }
        expect(provenChecked > 0, "some fixture screens a change as proven not better");
        expect(conflictingSeen > 0, "some fixture records a change that conflicts with the rest of the plan");
    }

    void expectClearedAudit(const PlanPricingRun & run, MasterPlanNeighborAuditStatus status, const char* message)
    {
        const auto & audit = run.result.stats.planPricing.neighborAudit;
        expect(audit && audit->status == status && audit->changes.empty() && audit->provenNotBetter == 0 &&
                   audit->open == 0 && audit->conflicting == 0 && audit->unavailable == 0,
               message);
    }

    void planNeighborAuditGuards()
    {
        const auto test = planPricingCases().front();
        const auto schedule = neighborAuditSchedule(MasterRefinementAllocation::Baseline);
        struct Guard
        {
            const char* name;
            std::function<void(NativeStockSession &)> before;
            MasterPlanNeighborAuditStatus expected;
        };
        for (const auto & guard : {
                 Guard{"owned mismatch", [](NativeStockSession & session) { session.ownedOffset = OWNED_GUARD_OFFSET; },
                       MasterPlanNeighborAuditStatus::OwnedMismatch},
                 Guard{"catalog mismatch", [](NativeStockSession & session) { session.catalogMismatch = true; },
                       MasterPlanNeighborAuditStatus::CatalogMismatch}})
        {
            const PlanPricingHooks hooks{.before = guard.before};
            const auto off = runPlanPricing(test, boundCapSchedule(MasterRefinementAllocation::Baseline),
                                            PLAN_PRICING_TEST_LEDGER, FriendlySearch::Immediate, hooks);
            const auto on = runPlanPricing(test, schedule, PLAN_PRICING_TEST_LEDGER, FriendlySearch::Immediate, hooks);
            expectNoNeighborInfluence(off, on);
            expectClearedAudit(on, guard.expected, "a floor the cap path would refuse stops the audit before any verdict");
            expect(!on.result.stats.planPricing.neighborAudit->trustedFloor, "a refused floor is never recorded");
            std::printf("  neighbor audit guard %s: status %d\n", guard.name,
                        static_cast<int>(on.result.stats.planPricing.neighborAudit->status));
        }
        RefinementLedger ledger;
        require(ledger.open(PLAN_PRICING_TEST_LEDGER), "basis ledger");
        PropertyStockField field;
        PropertyStockInstance enemy;
        NativeStockSession session(field, enemy, test.reference.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(test.problem, ledger, session, TARIFFS);
        const auto incumbent = evaluator.evaluate(operationIncumbent(test.problem));
        RefinementLedger scratch;
        require(scratch.open(PLAN_PRICING_TEST_LEDGER), "basis scratch ledger");
        const auto before = ledger.stats();
        expect(evaluator.oursBasis(incumbent, scratch) == MasterOursBasisStatus::Proven &&
                   ledger.stats() == before && scratch.stats().spent() == TARIFFS.quote,
               "a replayed incumbent quote is a proven floor and its check is paid on the ledger it is given");
        auto unreplayed = incumbent;
        unreplayed.lowerReplayed = false;
        auto unverified = incumbent;
        unverified.objectiveStatus = MasterObjectiveStatus::Unavailable;
        auto invalid = incumbent;
        auto snapshot = std::make_shared<RelativeStockProofSnapshot>(*incumbent.proof);
        snapshot->quote.valid = false;
        invalid.proof = snapshot;
        auto unquoted = incumbent;
        unquoted.proof.reset();
        for (const auto* evaluation : {&unreplayed, &unverified, &invalid, &unquoted})
        {
            const auto spent = scratch.stats().spent();
            expect(evaluator.oursBasis(*evaluation, scratch) == MasterOursBasisStatus::Unproven &&
                       scratch.stats().spent() == spent,
                   "an unreplayed, unverified, invalid or missing quote is no proven floor and costs nothing");
        }
        RefinementLedger empty;
        require(empty.open(0), "empty basis ledger");
        expect(evaluator.oursBasis(incumbent, empty) == MasterOursBasisStatus::BudgetExhausted,
               "an unaffordable basis check reports exhaustion");
    }

    void planNeighborAuditMidLoop()
    {
        std::uint64_t cut = 0;
        std::uint64_t cutWithSeveralChanges = 0;
        for (const auto & test : neighborAuditCases())
        {
            const auto full = runPlanPricing(test, neighborAuditSchedule(MasterRefinementAllocation::Baseline));
            const auto & funded = full.result.stats.planPricing.neighborAudit;
            require(funded && funded->status == MasterPlanNeighborAuditStatus::Recorded, "the mid-loop reference is funded");
            if (funded->changes.empty())
            {
                continue;
            }
            const auto off = runPlanPricing(test, boundCapSchedule(MasterRefinementAllocation::Baseline));
            const auto starved = runPlanPricing(test, neighborAuditSchedule(MasterRefinementAllocation::Baseline,
                                                                             funded->work - 1));
            expectNoNeighborInfluence(off, starved);
            const auto & audit = starved.result.stats.planPricing.neighborAudit;
            expectClearedAudit(starved, MasterPlanNeighborAuditStatus::Unaffordable,
                               "an audit that runs out inside the change loop keeps no partial changes");
            expect(audit->scanUpper == funded->scanUpper, "the cut lands after the term table, inside the conflict checks");
            const auto exact = runPlanPricing(test, neighborAuditSchedule(MasterRefinementAllocation::Baseline,
                                                                           funded->work));
            const auto & fitted = exact.result.stats.planPricing.neighborAudit;
            expect(fitted && fitted->status == MasterPlanNeighborAuditStatus::Recorded && fitted->work == funded->work &&
                       fitted->changes.size() == funded->changes.size(),
                   "an allowance of exactly the funded work completes the same audit");
            ++cut;
            cutWithSeveralChanges += funded->changes.size() > 1 ? 1 : 0;
        }
        expect(cut > 0 && cutWithSeveralChanges > 0, "some fixture with several changes is cut inside its change loop");
    }

    void planNeighborAuditStarved()
    {
        for (const auto & test : planPricingCases())
        {
            const auto off = runPlanPricing(test, boundCapSchedule(MasterRefinementAllocation::Baseline));
            const auto on = runPlanPricing(test, neighborAuditSchedule(MasterRefinementAllocation::Baseline,
                                                                        STARVED_NEIGHBOR_AUDIT_ALLOWANCE));
            expectNoNeighborInfluence(off, on);
            const auto & audit = on.result.stats.planPricing.neighborAudit;
            expectClearedAudit(on, MasterPlanNeighborAuditStatus::Unaffordable,
                               "a starved audit reports itself unaffordable and records no change");
            expect(audit->work <= STARVED_NEIGHBOR_AUDIT_ALLOWANCE && !audit->scanUpper,
                   "a starved audit spends at most its allowance and scans nothing");
        }
        const auto test = planPricingCases().front();
        const auto unrounded = runPlanPricing(test, [&]
        {
            auto schedule = neighborAuditSchedule(MasterRefinementAllocation::Baseline);
            schedule.planPricingRounds = 0;
            schedule.planPricingWork = 0;
            schedule.planPricingWitnessWork = 0;
            return schedule;
        }());
        expect(!unrounded.result.stats.planPricing.neighborAudit && !unrounded.record.contains(QLatin1String(PLAN_PRICING_RECORD)),
               "without the pricing pre-pass the audit neither runs nor exports");
    }

    void planNeighborAuditConfig()
    {
        expect(parseMasterPlanNeighborAudit("1", MasterStockControlRuntime::Arena) == MasterPlanNeighborAudit::Recorded &&
                   parseMasterPlanNeighborAudit("1", MasterStockControlRuntime::PlayableTrial) ==
                       MasterPlanNeighborAudit::Recorded,
               "the audit switch is honoured in the arena and the playable trial");
        expect(parseMasterPlanNeighborAudit("1", MasterStockControlRuntime::Play) == MasterPlanNeighborAudit::Off &&
                   parseMasterPlanNeighborAudit("1", MasterStockControlRuntime::None) == MasterPlanNeighborAudit::Off,
               "the audit switch is ignored in ordinary play");
        expect(parseMasterPlanNeighborAudit({}, MasterStockControlRuntime::Arena) == MasterPlanNeighborAudit::Off &&
                   parseMasterPlanNeighborAudit("0", MasterStockControlRuntime::Arena) == MasterPlanNeighborAudit::Off &&
                   parseMasterPlanNeighborAudit("true", MasterStockControlRuntime::PlayableTrial) ==
                       MasterPlanNeighborAudit::Off,
               "only the exact switch value turns the audit on");
        expect(playMasterStockControlConfig().planNeighborAudit == MasterPlanNeighborAudit::Off &&
                   MasterAssignmentSchedule{}.planNeighborAudit == MasterPlanNeighborAudit::Off,
               "the audit defaults off");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == QStringLiteral("--stock-reference-offline"))
    {
        return MasterAssignmentTest::runOfflineStockReference(arguments[2]);
    }
    if (arguments.size() != 1)
    {
        std::fprintf(stderr, "Unsupported integration test arguments\n");
        return 4;
    }
    sharedStockCertificateWithDifferentEconomics();
    nativeStockSearch();
    futurePricingBoundaries();
    warmStartOracleBoundaries();
    signedAndWideFutureDomains();
    capturedSignaturesShareEnemy();
    tenStockActors();
    shadowControlsAndCapture();
    transactionalInstallation();
    stockInstallationAuthentication();
    stockFinalizationBudgets();
    ownedCertificateSerialization();
    observationalProfile();
    reservationFailureExport();
    refinementPreflightReceipts();
    certificateReadinessAccounting();
    operationFundingBoundaries();
    operationReadinessReuse();
    operationRejectionAndProgress();
    operationPreparationCutoffs();
    operationSearchOracle();
    operationTraceWriter();
    scriptedOperationHandoffs();
    operationFundingParking();
    nativeCollectionTrace();
    ownedCancellationEligibility();
    suggestedFriendlyBounds();
    untrustedSuggestionSafety();
    planPricingAdapter();
    planPricingSearch();
    planBoundCap();
    planBoundCapRejections();
    planBoundCapContradiction();
    planBoundCapOwnedGuard();
    planBoundCapCatalogGuard();
    planBoundCapFallback();
    planBoundCapInstallRule();
    planBoundCapOracleFixtures();
    planNeighborTermTable();
    planNeighborAudit();
    planNeighborAuditStarved();
    planNeighborAuditGuards();
    planNeighborAuditMidLoop();
    planNeighborAuditConfig();
    testMasterStockReference();
    MasterAssignmentTest::testOfflineStockReference();
    std::printf("Master assignment integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
