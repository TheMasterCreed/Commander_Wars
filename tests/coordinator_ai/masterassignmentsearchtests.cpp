#include <random>

#include <QCoreApplication>

#include "ai/coordinator/masterassignmentsearch.h"
#include "ai/coordinator/masteroperationservice.h"
#include "ai/coordinator/propertystockfield.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"
#include "tests/coordinator_ai/masterassignmenttestexport.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;

    class RegistrySession final : public RelativeStockSession
    {
    public:
        RegistrySession(const PropertyStockField & field, const PropertyStockInstance & enemy)
            : RelativeStockSession(field, enemy)
        {
        }

        RelativeStockIdentity scope() const { return identity(); }
        RelativeStockQuote quote(const TurnPlan &, MilliFunds) override { return {}; }
        RelativeStockQuote quote(const ContinuationKey &, MilliFunds) override { return {}; }
        RelativeStockRefinedTerm refineOur(const ContinuationKey &, std::int64_t) override { return {}; }
        RelativeStockRefinedTerm refineEnemy(const std::vector<std::int32_t> &, std::int64_t) override { return {}; }
        bool replayOurWitness(const RelativeStockQuote &) override { return false; }
        bool replayEnemyWitness(const RelativeStockQuote &) override { return false; }
        std::int64_t remainingWork() const override { return 0; }
        std::int64_t ourRerunWork() const override { return 0; }
        std::int64_t enemyRerunWork(const std::vector<std::int32_t> &) const override { return 0; }
    };

    std::shared_ptr<MasterAssignmentEvaluation> registryEvaluation(
        const MasterAssignmentProblem & problem, const RelativeStockIdentity & stock,
        ContinuationKey key, std::vector<std::int32_t> mask)
    {
        auto result = std::make_shared<MasterAssignmentEvaluation>();
        result->state.identity = problem.identity();
        auto proof = std::make_shared<RelativeStockProofSnapshot>();
        proof->quote.identity = stock;
        proof->quote.key = std::move(key);
        proof->quote.capturedColumns = std::move(mask);
        result->proof = std::move(proof);
        return result;
    }

    void operationRegistry()
    {
        Fixture fixture(1);
        fixture.seat();
        const auto problem = fixture.extract();
        PropertyStockField field;
        PropertyStockInstance enemy;
        auto session = std::make_unique<RegistrySession>(field, enemy);
        auto incoming = registryEvaluation(
            problem, session->scope(), {CanonicalPlanActionKey::fromAction(0, 0, 0, false)}, {});
        auto challenger = registryEvaluation(
            problem, session->scope(), {CanonicalPlanActionKey::fromAction(0, 1, 0, false)}, {});
        std::array<MasterOperationDescriptor, 5> roles{{
            {incoming, MasterRefinementTerm::Friendly},
            {challenger, MasterRefinementTerm::Friendly},
            {incoming, MasterRefinementTerm::Enemy},
            {challenger, MasterRefinementTerm::Enemy},
            {incoming, MasterRefinementTerm::FutureDual}}};
        RefinementLedger ledger;
        require(ledger.open(1000000), "operation registry ledger");
        MasterOperationRegistry registry;
        std::array<std::size_t, 5> indices{};
        require(registry.prepare(problem.identity(), roles, ledger, 128, 4096, indices),
                "canonical registry preparation");
        expect(registry.size() == 4 && indices[2] == indices[3] && indices[0] != indices[4],
               "shared mask deduplicates but friendly search and dual remain separate");
        expect(MasterOperationRegistry::credit(registry.at(indices[0]), 8440000) &&
               registry.at(indices[2]).paidWork == 0 && registry.at(indices[4]).paidWork == 0,
               "friendly debt never services enemy or dual");
        expect(MasterOperationRegistry::credit(registry.at(indices[2]), 178165) &&
               registry.at(indices[3]).paidWork == 178165,
               "same enemy operation reached from another plan retains its paid history");
        const auto beforeCredit = ledger.stats().spent();
        MasterOperationRegistry::credit(registry.at(indices[1]), 100);
        expect(ledger.stats().spent() == beforeCredit, "service attribution never debits the ledger again");
        auto & friendly = registry.at(indices[0]);
        require(MasterOperationRegistry::grow(friendly, ledger, 4, false, false, true),
                "unadmitted operation leaves grant unchanged");
        expect(friendly.allowance == 128, "no growth for unavailable or unfunded operation");
        require(MasterOperationRegistry::grow(friendly, ledger, 4, true, false, true),
                "admitted valid unfinished operation grows");
        expect(friendly.allowance == 512 && ledger.stats().spent() - beforeCredit == MASTER_OPERATION_QUOTA_WORK,
               "admitted no-progress work grows once with separately charged quota arithmetic");
        require(MasterOperationRegistry::grow(friendly, ledger, 4, true, true, true), "completed native operation");
        expect(friendly.allowance == 512, "completed native operation does not grow");
        friendly.allowance = std::numeric_limits<std::int64_t>::max();
        require(MasterOperationRegistry::grow(friendly, ledger, 4, true, false, true), "quota overflow parks operation");
        expect(friendly.growthExhausted, "overflow does not wrap or clamp a grant");

        auto sameKey = registryEvaluation(problem, session->scope(), incoming->proof->quote.key, {70});
        const std::array<MasterOperationDescriptor, 1> sameFriendly{{{sameKey, MasterRefinementTerm::Friendly}}};
        require(registry.prepare(problem.identity(), sameFriendly, ledger, 128, 4096, indices),
                "friendly identity ignores independently owned enemy mask");
        expect(registry.at(indices[0]).paidWork == 8440000 && registry.at(indices[0]).growthExhausted,
               "full friendly key shares service and canonical grant");
        std::vector<std::int32_t> wideMask;
        for (std::int32_t column = 0; column < 71; ++column)
        {
            wideMask.push_back(column);
        }
        auto wide = registryEvaluation(problem, session->scope(), {}, wideMask);
        wideMask.back() = 72;
        auto distinct = registryEvaluation(problem, session->scope(), {}, wideMask);
        const std::array<MasterOperationDescriptor, 2> wideRoles{{
            {wide, MasterRefinementTerm::Enemy}, {distinct, MasterRefinementTerm::Enemy}}};
        require(registry.prepare(problem.identity(), wideRoles, ledger, 128, 4096, indices), "wide enemy identities");
        expect(indices[0] != indices[1], "columns beyond 64 participate in the complete mask identity");

        session.reset();
        expect(!incoming->proof->quote.identity.established(), "old operation context expires");
        session = std::make_unique<RegistrySession>(field, enemy);
        auto fresh = registryEvaluation(problem, session->scope(), {}, {});
        const std::array<MasterOperationDescriptor, 1> freshRoles{{{fresh, MasterRefinementTerm::Enemy}}};
        require(registry.prepare(problem.identity(), freshRoles, ledger, 128, 4096, indices), "replacement live context");
        expect(registry.size() == 1 && registry.at(indices[0]).paidWork == 0 &&
               registry.at(indices[0]).allowance == 128, "new context never inherits old service or grants");

        const std::array<std::size_t, 5> callbackSizes{36, 36, 0, 3, 36};
        expect(MasterOperationRegistry::tariff(5, callbackSizes) == 5356, "saved callback-51 registry arithmetic");
        expect(17081 + 128 + 5 * 6699 + 4 * 31823 + 169 == 178165 &&
               16171317 - 178165 - 5356 == 15987796, "saved displacement affordability, excluding new inspection fees");
        const std::array<std::size_t, 1> huge{std::numeric_limits<std::size_t>::max()};
        expect(!MasterOperationRegistry::tariff(1, huge), "registry size arithmetic fails closed");
        const std::array<std::size_t, 1> emptyPayload{0};
        const auto exactFee = *MasterOperationRegistry::tariff(0, emptyPayload) +
            MASTER_OPERATION_REGISTRY_QUOTE_WORK;
        for (const auto funding : {exactFee - 1, exactFee})
        {
            RefinementLedger limited;
            require(limited.open(funding), "registry threshold ledger");
            MasterOperationRegistry limitedRegistry;
            const bool prepared = limitedRegistry.prepare(problem.identity(), freshRoles, limited, 128, 4096, indices);
            expect(prepared == (funding == exactFee) && limited.stats().spent() <= funding,
                   "exact registry funding threshold and safe refusal");
            expect(prepared || limitedRegistry.size() == 0, "unfunded construction retains no partially inserted identity");
        }
    }

    MasterAssignmentSearchResult solve(const MasterAssignmentProblem & problem, std::int64_t work,
                                      std::int32_t dualRounds = 4, MasterAssignmentSearchObserver* observer = nullptr)
    {
        RefinementLedger ledger;
        require(ledger.open(work), "search ledger");
        MasterAssignmentEvaluator evaluator(problem, ledger);
        return searchMasterAssignment(problem, evaluator, ledger,
            {.schedule = {.currentDualRounds = dualRounds}, .retainDiagnostics = true, .observer = observer});
    }

    class SearchObserver final : public MasterAssignmentSearchObserver
    {
    public:
        void evaluated(const MasterAssignmentEvaluation & evaluation, std::int64_t work) override
        {
            ++evaluations;
            choices.push_back(evaluation.state.choices);
            expect(evaluation.state.complete() && work >= lastWork, "observer sees a complete evaluated tuple");
            lastWork = work;
        }

        void refined(const MasterAssignmentEvaluation &, MasterRefinementTerm,
                     std::int64_t, MasterRefinementStatus, std::int64_t, std::int64_t) override
        {
            expect(false, "immediate-only search has no continuation refinement event");
        }

        void compared(const MasterAssignmentEvaluation & challenger,
                      const RelativeStockComparison & comparison, std::int64_t work) override
        {
            ++comparisons;
            expect(challenger.lowerReplayed && comparison.valid() && work >= lastWork,
                   "observer sees authenticated complete comparisons in work order");
            lastWork = work;
        }

        std::uint64_t evaluations{0};
        std::uint64_t comparisons{0};
        std::int64_t lastWork{0};
        std::vector<std::vector<std::int32_t>> choices;
    };

    bool contains(const MasterAssignmentState & state, std::span<const std::int32_t> tuple)
    {
        if (state.choices.size() != tuple.size())
        {
            return false;
        }
        for (std::size_t actor = 0; actor < tuple.size(); ++actor)
        {
            if (state.choices[actor] != MASTER_UNASSIGNED && state.choices[actor] != tuple[actor])
            {
                return false;
            }
        }
        return true;
    }

    void check(const MasterAssignmentProblem & problem, const MasterAssignmentOracle::Result & exact,
               const MasterAssignmentSearchResult & result, std::int64_t allowance)
    {
        expect(result.fullDomain && result.domainAccounted && !result.invalidInput, "full applicable search domain");
        expect(result.stats.proofFailures == 0, "supported exact search has no proof failures");
        expect(result.stats.work.spent() <= allowance && result.stats.work.refundClamps == 0,
               "single ledger never overspends");
        if (allowance == 0)
        {
            expect(result.implicitRoot && result.stats.residualRegions == 1 && !result.upper,
                   "zero budget explicitly retains the entire implicit root domain");
        }
        if (exact.exactOptimum)
        {
            expect(!result.lower || *result.lower <= *exact.exactOptimum, "search lower <= exact optimum");
            expect(!result.upper || *result.upper >= *exact.exactOptimum, "search upper >= exact optimum");
            expect(!result.globalOptimal || (result.lower && *result.lower == *exact.exactOptimum),
                   "completed search equals exact optimum");
            expect(!result.noFeasibleAssignment, "never discard a feasible domain");
        }
        if (result.best)
        {
            const auto independent = MasterAssignmentOracle::constructReference(problem, result.best->state.choices);
            expect(independent.complete && independent.seatedTotal == *result.lower,
                   "best lower owns an independently replayable full plan");
            expect(result.best->lowerReplayed, "no lower without witness replay");
        }
        if (result.improvement)
        {
            const auto & proof = *result.improvement;
            const auto original = MasterAssignmentOracle::constructReference(problem, proof.incumbent.state.choices);
            const auto improved = MasterAssignmentOracle::constructReference(problem, proof.challenger.state.choices);
            expect(original.complete && improved.complete && improved.seatedTotal > original.seatedTotal &&
                   improved.seatedTotal - original.seatedTotal == proof.comparison.deltaLower,
                   "strict improvement independently verified, not lower rank");
        }
        for (const auto & prune : result.prunes)
        {
            for (const auto & tuple : exact.tuples)
            {
                if (tuple.reference.complete && contains(prune.state, tuple.choices))
                {
                    expect(prune.upper >= tuple.objective.total && prune.feasibleLower >= tuple.objective.total,
                           "pruning never discards a better completion");
                }
            }
        }
        if (result.lower)
        {
            for (const auto & tuple : exact.tuples)
            {
                if (!tuple.reference.complete || tuple.objective.total <= *result.lower)
                {
                    continue;
                }
                bool covered = false;
                for (const auto & region : result.residual)
                {
                    if (contains(region.state, tuple.choices))
                    {
                        expect(!region.upper || *region.upper >= tuple.objective.total,
                               "residual region upper covers each feasible completion");
                        covered = true;
                    }
                }
                expect(covered, "every unresolved better alternative remains in residual coverage");
            }
        }
    }

    void generatedSearches()
    {
        for (std::uint32_t seed = 2101; seed <= 2164; ++seed)
        {
            std::mt19937 random(seed);
            Fixture fixture(3);
            for (std::int32_t actor = 0; actor < 3; ++actor)
            {
                for (std::int32_t choice = 1; choice <= 2; ++choice)
                {
                    fixture.input.actors[actor].candidates.push_back(moveCandidate(
                        actor, {actor, 0}, {static_cast<std::int32_t>(random() % 4), 1},
                        static_cast<MilliFunds>(random() % 81) - 40, 3 - choice));
                }
            }
            fixture.seat();
            if (seed % 4 == 0)
            {
                fixture.selections[0].role = MasterActorRole::Fixed;
            }
            const auto problem = fixture.extract();
            auto input = request();
            input.seed = seed;
            input.fixture = QStringLiteral("master-search-%1").arg(seed);
            const auto exact = MasterAssignmentOracle::enumerate(problem, input);
            require(exact.exactOptimum.has_value(), "generated exact domain");
            std::optional<MilliFunds> priorLower;
            std::optional<MilliFunds> priorUpper;
            QJsonArray searches;
            for (const std::int64_t allowance : {0, 1, 4, 30, 100, 300, 800, 2000, 5000, 12000, 50000})
            {
                const auto result = solve(problem, allowance);
                check(problem, exact, result, allowance);
                const auto repeat = solve(problem, allowance);
                searches.append(searchRecord(result));
                expect(searchRecord(result) == searchRecord(repeat), "complete search receipt repeats byte-for-byte");
                expect(result.lower == repeat.lower && result.upper == repeat.upper &&
                       result.globalOptimal == repeat.globalOptimal &&
                       result.stats.work.spent() == repeat.stats.work.spent() &&
                       result.stats.work.grantedByWork == repeat.stats.work.grantedByWork &&
                       result.stats.expanded == repeat.stats.expanded &&
                       result.stats.residualRegions == repeat.stats.residualRegions,
                       "frozen schedule repeats deterministically");
                expect(!priorLower || (result.lower && *result.lower >= *priorLower),
                       "larger allowance preserves previously verified lower");
                expect(!priorUpper || (result.upper && *result.upper <= *priorUpper),
                       "larger allowance preserves previously verified upper");
                priorLower = result.lower;
                priorUpper = result.upper;
            }
            const auto final = solve(problem, 1000000);
            require(final.globalOptimal, "supported small search completes");
            check(problem, exact, final, 1000000);
            searches.append(searchRecord(final));
            exportSearchCase(input.fixture, problem, input, exact, searches);
        }
    }

    void coordinatedAndEquality()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {(actor + 1) % 3, 0}, actor + 5, 1));
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor, 1}, actor + 4, 2));
        }
        fixture.seat();
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        auto result = solve(problem, 1000000);
        check(problem, exact, result, 1000000);
        expect(result.globalOptimal && result.improvement.has_value(), "three-actor domain finds certified improvement");
        Fixture equal(2);
        for (std::int32_t actor = 0; actor < 2; ++actor)
        {
            equal.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor, 1}, actor + 1, 1));
        }
        equal.seat();
        const auto equalProblem = equal.extract();
        result = solve(equalProblem, 1000000);
        expect(result.globalOptimal && result.incumbentPreserved && result.canonicalRepresentative &&
               !result.improvement, "exact equality keeps incoming incumbent");
    }

    void exhaustiveInterruption()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {0, 1}, 9, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        const auto complete = solve(problem, 100000, 1);
        require(complete.globalOptimal, "interruption control completes");
        for (std::int64_t allowance = 0; allowance <= complete.stats.work.spent(); ++allowance)
        {
            check(problem, exact, solve(problem, allowance, 1), allowance);
        }
    }

    void jointVacateChain()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates[0].valuation.ledger.enemyCapital = 0;
            fixture.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {actor + 1, 0}, actor == 0 ? 10 : -3, 1));
        }
        fixture.seat();
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        require(exact.exactOptimum == 4, "joint-only fixture exact optimum");
        const auto result = solve(problem, 1000000);
        check(problem, exact, result, 1000000);
        require(result.improvement.has_value(), "joint vacate chain has strict proof");
        expect(result.stats.incumbentCompletions > 0 && result.stats.infeasible > 0,
               "failed incumbent completions do not discard a feasible three-actor branch");
        expect(result.improvement->challenger.state.choices == std::vector<std::int32_t>({1, 1, 1}),
               "all three actors must change for the positive plan");
        for (const auto & tuple : exact.tuples)
        {
            if (tuple.reference.complete && tuple.objective.total > 0)
            {
                expect(tuple.choices == std::vector<std::int32_t>({1, 1, 1}),
                       "no single or pair replacement explains the improvement");
            }
        }
        fixture.input.actionIds = {QStringLiteral("RENAMED_WAIT"), QStringLiteral("RENAMED_FIRE"),
                                  QStringLiteral("RENAMED_CAPTURE")};
        fixture.seat();
        const auto renamed = solve(fixture.extract(), 1000000);
        expect(searchRecord(result) == searchRecord(renamed), "engine action labels do not affect search");
        exportSearchCase(QStringLiteral("master-search-joint-vacate"), problem, request(), exact,
                         {searchRecord(result)});
    }

    void vacateCycleAndMissingIncumbent()
    {
        Fixture cycle(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            cycle.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {(actor + 1) % 3, 0}, 100, 1));
        }
        cycle.seat();
        const auto cycleProblem = cycle.extract();
        const auto cycleExact = MasterAssignmentOracle::enumerate(cycleProblem, request());
        const auto cycleSearch = solve(cycleProblem, 1000000);
        check(cycleProblem, cycleExact, cycleSearch, 1000000);
        expect(cycleSearch.globalOptimal && cycleSearch.incumbentPreserved &&
               cycleSearch.stats.infeasible > 0 && !cycleSearch.improvement,
               "individually compatible moves cannot install a cyclic vacate dependency");

        Fixture missing(2);
        missing.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {1, 0}, 10, 1));
        missing.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {2, 0}, 10, 1));
        for (const bool abandoned : {false, true})
        {
            missing.seat();
            missing.plan.markFailed(1);
            if (!abandoned)
            {
                TurnPlan onlyFirst;
                onlyFirst.addAction(missing.plan.action(0));
                require(claimFor(onlyFirst, 0, missing.input.actors[0].candidates[0]) ==
                        ReservationResult::Granted, "unassigned fixture preserves first actor");
                missing.plan = std::move(onlyFirst);
            }
            missing.selections[1].actionIndex = abandoned ? 1 : NO_ACTION;
            missing.selections[1].candidateIndex = NO_CANDIDATE;
            const auto problem = missing.extract();
            const auto exact = MasterAssignmentOracle::enumerate(problem, request());
            const auto result = solve(problem, 1000000);
            check(problem, exact, result, 1000000);
            expect(result.globalOptimal && result.lower == 20 && result.best &&
                   result.best->state.choices == std::vector<std::int32_t>({1, 1}) &&
                   !result.improvement && !result.noFeasibleAssignment,
                   "search revives unassigned actors without claiming improvement over an incomplete incumbent");
        }
    }

    void unsupportedAndOverflow()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 90, 1));
        fixture.seat();
        require(fixture.plan.addEdge(0, 1, PlanEdgeKind::Screen), "screen fixture edge");
        auto problem = fixture.extract();
        auto result = solve(problem, 1000000);
        expect(result.stats.unsupported > 0 && result.stats.residualRegions > 0 && !result.globalOptimal &&
               !result.noFeasibleAssignment && result.incumbentPreserved,
               "unsupported changed dependencies are not proven infeasible");
        fixture.input.actors[0].candidates[1].valuation.ledger.enemyCapital =
            std::numeric_limits<MilliFunds>::max();
        fixture.seat();
        problem = fixture.extract();
        result = solve(problem, 1000000);
        expect(result.stats.arithmeticFailures > 0 && !result.globalOptimal && !result.upper &&
               !result.noFeasibleAssignment && result.incumbentPreserved,
               "unrepresentable objective remains uncovered by a numeric upper, not discarded");
    }

    void scopedIdentitiesAndLargeResources()
    {
        Fixture fixture(130);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {129, 0}, 999, 1));
        fixture.seat();
        for (std::size_t actor = 1; actor < fixture.selections.size(); ++actor)
        {
            fixture.selections[actor].role = MasterActorRole::Fixed;
        }
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        const auto result = solve(problem, 10000000);
        check(problem, exact, result, 10000000);
        expect(problem.resources().size() > 64 && result.globalOptimal && result.incumbentPreserved,
               "large resource domain preserves fixed claims");
        RefinementLedger first;
        RefinementLedger second;
        require(first.open(100000) && second.open(100000), "identity ledgers");
        MasterAssignmentEvaluator evaluator(problem, first);
        expect(searchMasterAssignment(problem, evaluator, second).invalidInput,
               "evaluator cannot hide work in another ledger");
        const auto unrelated = fixture.extract();
        expect(searchMasterAssignment(unrelated, evaluator, first).invalidInput,
               "equal-content independent problem cannot reuse a bound identity");
        for (const std::int64_t growth : {-1, 0, 1})
        {
            const auto before = first.stats();
            const auto invalid = searchMasterAssignment(problem, evaluator, first,
                {.schedule = {.refinementGrowthFactor = growth}, .retainDiagnostics = true});
            expect(invalid.invalidInput && invalid.implicitRoot && !invalid.globalOptimal &&
                       first.stats() == before,
                   "non-growing continuation policies fail before work while retaining the full domain");
        }
    }

    void improvementBeforeGlobalProof()
    {
        Fixture fixture(3);
        fixture.input.unitLinks.push_back({900, {5, 1}});
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 10, 1));
        auto shot = moveCandidate(1, {1, 0}, {1, 0}, 100, 1);
        shot.bundle.components.push_back(fireComponent({
            .targetUnitId = 3, .targetReplacementCost = 100, .targetHpSteps = 10, .damageSteps = 10}));
        fixture.input.actors[1].candidates.push_back(shot);
        auto fixed = moveCandidate(2, {2, 0}, {2, 0}, 0, 0);
        fixed.bundle.components.push_back(fireComponent({
            .targetUnitId = 3, .targetReplacementCost = 100, .targetHpSteps = 10, .damageSteps = 5}));
        fixture.input.actors[2].candidates[0] = fixed;
        fixture.seat();
        require(fixture.plan.commit(2), "fixed damage share is already committed");
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        const auto complete = solve(problem, 1000000);
        require(complete.globalOptimal && complete.stats.firstImprovementWork.has_value(), "damage search completes");
        const auto prefix = solve(problem, *complete.stats.firstImprovementWork);
        check(problem, exact, prefix, *complete.stats.firstImprovementWork);
        expect(prefix.improvement && !prefix.globalOptimal && prefix.gap && *prefix.gap > 0,
               "strict improvement is certified before the remaining damage-allocation gap closes");
        expect(prefix.best && prefix.best->replay->plan.action(2) == fixture.plan.action(2) &&
               prefix.best->replay->plan.actionReservations(2) == fixture.plan.actionReservations(2),
               "global search preserves committed actions and their exact damage share");
        exportSearchCase(QStringLiteral("master-search-improvement-before-optimal"), problem, request(), exact,
                         {searchRecord(prefix), searchRecord(complete)});
    }

    void completionWitnessMemo()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        const auto result = solve(problem, 1000000);
        check(problem, exact, result, 1000000);
        expect(result.globalOptimal && result.lower == 16 && result.stats.evaluationCacheHits > 0,
               "an earlier completion witness is reused when best-upper traversal reaches that exact tuple");
    }

    MasterAssignmentSearchResult solveContributionCompletion(
        const MasterAssignmentProblem & problem, std::int64_t allowance)
    {
        RefinementLedger ledger;
        require(ledger.open(allowance), "contribution completion ledger");
        MasterAssignmentEvaluator evaluator(problem, ledger);
        MasterAssignmentSearchOptions options;
        options.schedule.initialContributionCompletion = true;
        options.retainDiagnostics = true;
        return searchMasterAssignment(problem, evaluator, ledger, options);
    }

    void initialContributionCompletion()
    {
        Fixture independent(2);
        for (auto & actor : independent.input.actors)
        {
            actor.candidates[0].valuation.ledger.enemyCapital = 0;
        }
        independent.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        independent.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9, 1));
        independent.seat();
        const auto problem = independent.extract();
        const auto exact = MasterAssignmentOracle::enumerate(problem, request());
        const auto completed = solveContributionCompletion(problem, 1000000);
        check(problem, exact, completed, 1000000);
        expect(completed.globalOptimal && completed.lower == 16 &&
                   completed.stats.firstImprovementLower == 16 && completed.stats.contributionCompletions == 1,
               "the initial contribution descent produces one complete joint witness");
        require(completed.stats.firstImprovementWork.has_value(), "contribution witness has a paid proof boundary");
        for (const auto allowance : {std::int64_t{0}, std::int64_t{64},
                                     *completed.stats.firstImprovementWork / 2,
                                     *completed.stats.firstImprovementWork - 1})
        {
            const auto interrupted = solveContributionCompletion(problem, allowance);
            check(problem, exact, interrupted, allowance);
            expect(interrupted.domainAccounted && interrupted.stats.work.spent() <= allowance,
                   "interrupted proposal construction preserves the original search region");
        }

        Fixture conflict(2);
        for (auto & actor : conflict.input.actors)
        {
            actor.candidates[0].valuation.ledger.enemyCapital = 0;
        }
        conflict.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {1, 1}, 10, 1));
        conflict.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 20, 1));
        conflict.seat();
        const auto conflictProblem = conflict.extract();
        const auto conflictExact = MasterAssignmentOracle::enumerate(conflictProblem, request());
        const auto improved = solveContributionCompletion(conflictProblem, 1000000);
        check(conflictProblem, conflictExact, improved, 1000000);
        expect(improved.globalOptimal && improved.lower == 20 && improved.stats.firstImprovementLower == 10,
               "ordinary exploration can improve beyond the greedy witness");

        Fixture cycle(3);
        for (auto & actor : cycle.input.actors)
        {
            actor.candidates[0].valuation.ledger.enemyCapital = 0;
        }
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            cycle.input.actors[actor].candidates.push_back(
                moveCandidate(actor, {actor, 0}, {(actor + 1) % 3, 0}, 100, 1));
        }
        cycle.seat();
        const auto cycleProblem = cycle.extract();
        const auto cycleExact = MasterAssignmentOracle::enumerate(cycleProblem, request());
        const auto rejected = solveContributionCompletion(cycleProblem, 1000000);
        check(cycleProblem, cycleExact, rejected, 1000000);
        expect(rejected.globalOptimal && rejected.lower == 0 && !rejected.improvement &&
                   rejected.stats.contributionCompletions == 1 && rejected.stats.infeasible > 0,
               "full replay rejects the greedy vacate cycle without discarding legal alternatives");
    }

    void priorAttemptRefusal()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        for (const bool alternatives : {false, true})
        {
            std::optional<MasterAssignmentSearchResult> baseline;
            for (const bool refused : {false, true})
            {
                RefinementLedger ledger;
                require(ledger.open(1000000), "shared attempt ledger");
                if (refused)
                {
                    RefinementLedger::WorkLimit limit(ledger, 0);
                    require(!ledger.consume(1, RefinementWork::MasterPreparation), "earlier attempt refused");
                }
                MasterAssignmentEvaluator evaluator(problem, ledger);
                MasterAssignmentSearchOptions options;
                options.schedule.initialIncumbentAlternatives = alternatives;
                const auto result = searchMasterAssignment(problem, evaluator, ledger, options);
                expect(result.globalOptimal && result.improvement && result.lower == 16 &&
                           ledger.stats().refusals == static_cast<int>(refused),
                       "a previous attempt refusal preserves later search and aggregate accounting");
                expect(!alternatives || result.stats.initialIncumbentAlternatives == 2,
                       "initial alternatives visit each mutable actor's alternate candidate");
                if (baseline)
                {
                    expect(result.best->state.choices == baseline->best->state.choices &&
                               result.stats.work.spent() == baseline->stats.work.spent() &&
                               result.stats.firstImprovementWork == baseline->stats.firstImprovementWork,
                           "prior refusal changes neither discovery nor paid search work");
                }
                else
                {
                    baseline = result;
                }
            }
        }
    }

    void observerIsolation()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        const auto original = solve(problem, 1000000);
        SearchObserver observer;
        const auto observed = solve(problem, 1000000, 4, &observer);
        expect(observer.evaluations == observed.stats.evaluated && observer.comparisons > 0,
               "diagnostic observer receives every fresh evaluation and comparison");
        expect(searchRecord(original) == searchRecord(observed),
               "observation changes no selection, proof, bound, residual, or work receipt");
    }

    void firstOwnedProofTiming()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 7, 1));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        const auto complete = solve(problem, 1000000);
        require(complete.globalOptimal && complete.stats.firstImprovementWork &&
                    *complete.stats.firstImprovementWork > 0, "timing fixture retains a strict proof");
        const auto firstWork = *complete.stats.firstImprovementWork;
        for (const auto allowance : {std::int64_t{0}, firstWork - 1, firstWork,
                                     complete.stats.work.spent(), std::int64_t{1000000}})
        {
            MasterAssignmentProfile profile;
            profile.recordFirstImprovement();
            expect(!profile.firstImprovementNanos, "inactive profiling cannot invent a proof time");
            RefinementLedger ledger;
            require(ledger.open(allowance), "proof timing ledger");
            MasterAssignmentEvaluator evaluator(problem, ledger, &profile);
            MasterAssignmentSearchResult result;
            {
                MasterProfileScope outer(&profile, MasterPhase::Shadow);
                {
                    MasterProfileScope setup(&profile, MasterPhase::Catalog);
                }
                result = searchMasterAssignment(problem, evaluator, ledger,
                    {.retainDiagnostics = true, .profile = &profile});
                if (result.improvement)
                {
                    const auto retained = profile.firstImprovementNanos;
                    profile.recordFirstImprovement();
                    expect(retained == profile.firstImprovementNanos,
                           "later observations cannot reset the first retained proof time");
                }
            }
            expect(searchRecord(result) == searchRecord(solve(problem, allowance)),
                   "proof timestamps preserve all deterministic search results and work");
            expect(profile.firstImprovementNanos.has_value() == result.improvement.has_value() &&
                       profile.firstImprovementNanos.has_value() == result.stats.firstImprovementWork.has_value(),
                   "a timestamp exists exactly when a complete strict proof was retained");
            if (profile.firstImprovementNanos)
            {
                const auto elapsed = profile.samples[static_cast<std::size_t>(MasterPhase::Shadow)].inclusiveNanos;
                const auto setup = profile.samples[static_cast<std::size_t>(MasterPhase::Catalog)].inclusiveNanos;
                expect(*profile.firstImprovementNanos >= setup && *profile.firstImprovementNanos <= elapsed,
                       "first proof uses the enclosing computation origin, including setup");
            }
            if (allowance == firstWork - 1)
            {
                expect(!result.improvement && !profile.firstImprovementNanos && ledger.lastRefusal() &&
                           ledger.lastRefusal()->work == RefinementWork::MasterWitness,
                       "an unpaid improvement witness does not receive a retained-proof timestamp");
            }
        }
        Fixture equal(2);
        equal.seat();
        const auto equalProblem = equal.extract();
        RefinementLedger equalLedger;
        require(equalLedger.open(100000), "equality timing ledger");
        MasterAssignmentProfile equalProfile;
        MasterAssignmentEvaluator equalEvaluator(equalProblem, equalLedger, &equalProfile);
        const auto unchanged = searchMasterAssignment(equalProblem, equalEvaluator, equalLedger,
            {.profile = &equalProfile});
        expect(unchanged.globalOptimal && !unchanged.improvement && !equalProfile.firstImprovementNanos,
               "exact equality is not timed as a useful strict improvement");
    }

    void missingGenerationOrdering()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            for (std::int32_t choice = 1; choice <= 3; ++choice)
            {
                fixture.input.actors[actor].candidates.push_back(
                    moveCandidate(actor, {actor, 0}, {actor, choice}, 5, choice));
            }
        }
        fixture.seat();
        const auto indexed = fixture.extract();
        for (auto & actor : fixture.input.actors)
        {
            for (auto & candidate : actor.candidates)
            {
                candidate.generationIndex = -1;
            }
        }
        const auto unindexed = fixture.extract();
        SearchObserver indexedObserver;
        SearchObserver unindexedObserver;
        const auto first = solve(indexed, 1000000, 4, &indexedObserver);
        const auto second = solve(unindexed, 1000000, 4, &unindexedObserver);
        expect(indexedObserver.choices == unindexedObserver.choices,
               "missing generation ordinals preserve canonical original-index discovery order");
        expect(searchRecord(first) == searchRecord(second),
               "equivalent canonical candidate orders preserve search proof and work");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    generatedSearches();
    coordinatedAndEquality();
    exhaustiveInterruption();
    jointVacateChain();
    vacateCycleAndMissingIncumbent();
    unsupportedAndOverflow();
    scopedIdentitiesAndLargeResources();
    improvementBeforeGlobalProof();
    completionWitnessMemo();
    initialContributionCompletion();
    priorAttemptRefusal();
    observerIsolation();
    firstOwnedProofTiming();
    missingGenerationOrdering();
    operationRegistry();
    std::printf("Master assignment search: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
