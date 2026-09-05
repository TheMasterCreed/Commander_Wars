#include <array>

#include <QCoreApplication>

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
        {
            RefinementLedger ledger;
            require(ledger.open(1000000), "certificate integration ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger, QuoteFault::None, true, true);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            const auto result = searchMasterAssignment(problem, evaluator, ledger, {.retainDiagnostics = true});
            expect(session.certificateCalls > 0 && session.verifiedCertificates > 0 &&
                   result.stats.work.grantedByWork[static_cast<std::size_t>(RefinementWork::PackingDualCertificate)] > 0,
                   "actual search exercises complete catalog, native node dual optimizer, and verifier");
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
            require(ledger.open(100000), "owned witness ledger");
            NativeStockSession session(field, enemy, reference.continuation.cases, ledger);
            MasterAssignmentEvaluator evaluator(problem, ledger, session, {.quote = 32, .witness = 32});
            auto evaluated = evaluator.evaluate({problem.identity(),
                std::vector<std::int32_t>(problem.incumbentChoices().begin(), problem.incumbentChoices().end())});
            require(evaluated.proof != nullptr, "initial evaluation owns lower proof");
            const auto recorded = evaluated.proof;
            require(evaluator.refine(evaluated, MasterRefinementTerm::Friendly, 4096) == MasterRefinementStatus::Updated &&
                    evaluator.refine(evaluated, MasterRefinementTerm::Enemy, 4096) == MasterRefinementStatus::Updated,
                    "mutable session replaces continuation witnesses");
            expect(!sameRelativeStockQuote(recorded->quote, *evaluated.quote()) &&
                   session.replayLowerProof(*recorded), "prior lower witness remains replayable after cache refinement");
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
        auto left = evaluator.evaluate({problem.identity(), {0}});
        auto right = evaluator.evaluate({problem.identity(), {1}});
        for (auto* value : {&left, &right})
        {
            require(evaluator.refine(*value, MasterRefinementTerm::Friendly, 4096) == MasterRefinementStatus::Updated &&
                    evaluator.refine(*value, MasterRefinementTerm::Enemy, 4096) == MasterRefinementStatus::Updated,
                    "distinct enemy lower witnesses refine and replay");
        }
        const auto comparison = evaluator.compare(left, right);
        const auto delta = exact.tuples[1].objective.total - exact.tuples[0].objective.total;
        expect(comparison.valid() && !comparison.commonEnemyCancelled &&
               comparison.deltaLower <= delta && comparison.deltaUpper >= delta,
               "distinct signatures cannot use common-enemy cancellation");
        exportSearchCase(QStringLiteral("master-different-enemy"), problem, input, exact, {searchRecord(result)});
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
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    nativeStockSearch();
    futurePricingBoundaries();
    signedAndWideFutureDomains();
    differentEnemySignatures();
    tenStockActors();
    std::printf("Master assignment integration: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
