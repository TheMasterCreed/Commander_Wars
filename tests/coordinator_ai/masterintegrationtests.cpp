#include <array>

#include <QCoreApplication>
#include <QDir>

#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "tests/coordinator_ai/masterassignmentstockfixture.h"
#include "tests/coordinator_ai/masterassignmenttestexport.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;

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
        for (const std::uint64_t burst : {1, 24})
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
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "certificate integration ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger, QuoteFault::None, true, true);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger,
                {.schedule = {.futureDualAllowance = 1}, .retainDiagnostics = true});
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
        const auto disabled = parseMasterShadowConfig({}, "invalid", {});
        expect(!disabled.enabled() && disabled.error == MasterShadowConfigError::None,
               "default disabled mode ignores inactive diagnostic settings");
        expect(parseMasterShadowConfig("stockControl", "160000", output).error ==
               MasterShadowConfigError::UnknownMode, "Phase 3 cannot activate gameplay control");
        expect(parseMasterShadowConfig("shadow", "-1", output).error ==
               MasterShadowConfigError::InvalidAllowance, "negative work fails closed");
        expect(parseMasterShadowConfig("shadow", "9223372036854775808", output).error ==
               MasterShadowConfigError::InvalidAllowance, "overflowed work fails closed");
        expect(parseMasterShadowConfig("shadow", {}, "relative.jsonl").error ==
               MasterShadowConfigError::MissingAbsoluteOutput, "diagnostics require an explicit absolute destination");
        const auto enabled = parseMasterShadowConfig("shadow", {}, output);
        expect(enabled.enabled() && enabled.allowance == MASTER_ASSIGNMENT_INITIAL_ALLOWANCE,
               "explicit shadow starts at the configurable initial allowance");

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
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    sharedStockCertificateWithDifferentEconomics();
    nativeStockSearch();
    futurePricingBoundaries();
    signedAndWideFutureDomains();
    differentEnemySignatures();
    tenStockActors();
    shadowControlsAndCapture();
    observationalProfile();
    std::printf("Master assignment integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
