#include <array>
#include <map>

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>

#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentinstallation.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "tests/coordinator_ai/masterassignmentstockfixture.h"
#include "tests/coordinator_ai/masterassignmenttestexport.h"

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

    void differentEnemySignatures()
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
                entry.enemy.columns[0].ownerBefore = OwnerSign::Enemy;
                entry.enemy.rows[0].weight[0] = columnStreamValue(
                    entry.enemy.columns[0], entry.enemy.rows[0].ownedTurn[0], entry.enemy.horizon);
            }
            input.continuation.cases.push_back(std::move(entry));
        }
        const auto exact = MasterAssignmentOracle::enumerate(problem, input);
        require(exact.exactOptimum.has_value(), "different enemy signatures have exact reference");
        PropertyStockField field;
        PropertyStockInstance enemy;
        RefinementLedger ledger;
        require(ledger.open(1000000), "different enemy ledger");
        NativeStockSession session(field, enemy, input.continuation.cases, ledger);
        MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
        const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
        expect(result.lower && *result.lower <= *exact.exactOptimum &&
               result.upper && *result.upper >= *exact.exactOptimum,
               "different enemy signatures preserve absolute search bounds");
        expect(result.stats.cancelledEnemyRefinements == 0,
               "different capture signatures cannot defer their enemy terms through common-enemy cancellation");
        auto left = evaluator.evaluate({problem.identity(), {0}});
        auto right = evaluator.evaluate({problem.identity(), {1}});
        expect(!evaluator.unresolvedCommonEnemy(left, right),
               "owned quotes with distinct capture signatures cannot defer enemy refinement");
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
                   "distinct enemy signatures retain exact replayed witnesses");
        }
        const auto comparison = evaluator.compare(left, right);
        const auto delta = exact.tuples[1].objective.total - exact.tuples[0].objective.total;
        expect(comparison.valid() && !comparison.commonEnemyCancelled &&
               comparison.deltaLower <= delta && comparison.deltaUpper >= delta,
               "distinct signatures cannot use common-enemy cancellation");
        exportSearchCase(QStringLiteral("master-different-enemy"), problem, input, exact, {searchRecord(result)});
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
        const auto control = parseMasterShadowConfig("stockControl", "60000000", output, {}, {}, true);
        expect(control.controlsStock() && !control.enabled() && control.allowance == 60000000,
               "StockControl requires an explicit mode and allowance without enabling Shadow");
        expect(parseMasterShadowConfig("stockControl", {}, output, {}, {}, true).error ==
               MasterShadowConfigError::MissingExplicitAllowance,
               "StockControl cannot inherit the global initial allowance implicitly");
        expect(parseMasterShadowConfig("stockControl", "60000000", output).error ==
               MasterShadowConfigError::StockControlOutsideArena,
               "StockControl cannot activate outside the explicit isolated arena");
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
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    sharedStockCertificateWithDifferentEconomics();
    nativeStockSearch();
    futurePricingBoundaries();
    warmStartOracleBoundaries();
    signedAndWideFutureDomains();
    differentEnemySignatures();
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
    ownedCancellationEligibility();
    std::printf("Master assignment integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
