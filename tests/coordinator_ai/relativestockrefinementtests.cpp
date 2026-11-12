#include <iostream>
#include <limits>
#include <string_view>

#include "tests/coordinator_ai/relativestocktestfixture.h"

namespace
{
    using namespace Coordinator;
    using RelativeStockTests::Problem;
    using RelativeStockTests::SearchPrefix;
    using RelativeStockTests::Session;
    using RelativeStockTests::Term;

    int failures = 0;
    int checks = 0;

    void expect(bool condition, std::string_view name)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << name << '\n';
        }
    }

    const ContinuationKey INCUMBENT{CanonicalPlanActionKey::fromAction(0, 1, 1, false)};
    const ContinuationKey CHALLENGER{CanonicalPlanActionKey::fromAction(0, 2, 1, false)};
    const ContinuationKey SECOND_CHALLENGER{
        CanonicalPlanActionKey::fromAction(
            0, 3, 1, false)};

    struct Observer
    {
        std::vector<RelativeStockStep> steps;
        void operator()(const RelativeStockStep & step, const RelativeStockQuote &, const RelativeStockQuote &)
        {
            steps.push_back(step);
        }
    };

    void testTargeting()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const auto slice : {RelativeStockSlice::Direct, RelativeStockSlice::Doubling})
        {
            Session session(field, enemy, 100000);
            session.plans[INCUMBENT].ours = {{76, 116}, 112, 15000};
            session.plans[CHALLENGER].ours = {{78, 123}, 117, 20000};
            auto incumbent = session.quote(INCUMBENT, 0);
            auto challenger = session.quote(CHALLENGER, 0);
            Observer observer;
            const auto result = refineRelativeStock(session, incumbent, challenger,
                {.workLimit = 100000, .maximumStateCap = 100000, .slice = slice}, observer);
            expect(result.comparison.improves() && result.comparison.commonEnemyCancelled,
                   "targeted our-side refinement proves an improvement");
            expect(session.enemyCalls == 0 && result.enemyStatesUsed == 0,
                   "cancelled enemy receives no refinement even with its larger absolute width");
            expect(observer.steps.front().target == RelativeStockTarget::ChallengerOurs,
                   "widest contributing term is refined first");
            expect(observer.steps.front().requestedCap ==
                       (slice == RelativeStockSlice::Direct ? 99997 : LIVE_PAIR_SLICE_BASE),
                   "requested slice includes the normal rerun tariff in its allowance");
            expect(result.comparison.deltaLower == 1 && result.comparison.deltaUpper == 41,
                   "comparison stops as soon as a conservative positive delta is proved");
            expect(session.ourCalls == static_cast<std::int32_t>(observer.steps.size()) &&
                       incumbent.ours.lower == 76,
                   "unneeded incumbent work is not performed");
            expect(result.workUsed == session.ledger.stats().spent() &&
                       result.workUsed == result.statesUsed + session.ourCalls * session.ourTariff,
                   "actual states plus rerun work match the one shared ledger");
            expect(!incumbent.absoluteExact() && !challenger.absoluteExact(),
                   "relative refinement does not turn cancelled enemy uncertainty into absolute proof");
        }
    }

    void testTieAndBudget()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 20);
        session.plans[INCUMBENT].ours = {{10, 20}, 15, 4};
        session.plans[CHALLENGER].ours = {{10, 20}, 16, 4};
        auto incumbent = session.quote(INCUMBENT, 0);
        auto challenger = session.quote(CHALLENGER, 0);
        Observer observer;
        auto result = refineRelativeStock(session, incumbent, challenger, {.workLimit = 20}, observer);
        expect(result.steps == 2 && observer.steps.front().target == RelativeStockTarget::IncumbentOurs,
               "equal widths have stable incumbent-first ordering");
        expect(result.comparison.exact() && result.comparison.deltaLower == 1, "two refined terms prove exact +1");
        expect(result.statesUsed == 8 && result.workUsed == 14 && session.remainingWork() == 6,
               "unused state grants are refunded without inventing another allowance");

        Session exhausted(field, enemy, 17);
        exhausted.plans[INCUMBENT].ours = {{10, 20}, 15, 1000};
        exhausted.plans[CHALLENGER].ours = {{10, 20}, 16, 1000};
        incumbent = exhausted.quote(INCUMBENT, 0);
        challenger = exhausted.quote(CHALLENGER, 0);
        observer.steps.clear();
        result = refineRelativeStock(exhausted, incumbent, challenger, {.workLimit = 17}, observer);
        expect(result.workUsed == 17 && result.statesUsed == 14 && result.budgetExhausted,
               "deterministic work budget includes tariff and stops at its exact ceiling");
        expect(result.comparison.unresolved() && !result.comparison.improves(), "exhaustion cannot install an incumbent scalar");

        Session capped(field, enemy, 100);
        capped.plans[INCUMBENT].ours = {{10, 20}, 15, 1000};
        capped.plans[CHALLENGER].ours = {{10, 20}, 16, 1000};
        incumbent = capped.quote(INCUMBENT, 0);
        challenger = capped.quote(CHALLENGER, 0);
        observer.steps.clear();
        result = refineRelativeStock(capped, incumbent, challenger,
                                    {.workLimit = 100, .maximumStateCap = 10}, observer);
        expect(result.steps == 2 && result.statesUsed == 20 && result.stop == RelativeStockStop::TermCap,
               "a direct per-term cap does not rerun identical incomplete searches");
        expect(result.comparison.unresolved(), "capped continuation uncertainty is retained");
    }

    void testCheckedThresholdArithmeticAndCommonEnemyWork()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 100);
        auto incumbent = session.quote(INCUMBENT, 11);
        auto challenger = session.quote(CHALLENGER, 29);
        incumbent.owned = 7;
        challenger.owned = 13;
        challenger.ours = {17, 200};
        const auto threshold =
            commonEnemySequentialThreshold(incumbent, challenger);
        expect(
            threshold.has_value() && *threshold == 41,
            "CHECKED_THRESHOLD_ARITHMETIC");
        expect(
            session.enemyCalls == 0 &&
                session.enemyReplayCalls == 0,
            "COMMON_ENEMY_REQUIRES_ZERO_ENEMY_WORK");

        challenger.economic =
            std::numeric_limits<MilliFunds>::max();
        incumbent.economic = -1;
        expect(
            !commonEnemySequentialThreshold(
                 incumbent, challenger).has_value(),
            "checked threshold addition rejects overflow");
        challenger.economic =
            std::numeric_limits<MilliFunds>::min();
        incumbent.economic = 1;
        expect(
            !commonEnemySequentialThreshold(
                 incumbent, challenger).has_value(),
            "checked threshold subtraction rejects underflow");
    }

    void testResumedRungs()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, DEFAULT_RELATIVE_STOCK_WORK_LIMIT);
        session.plans[INCUMBENT].ours = {{100, 100}, 100, 1, true};
        session.plans[CHALLENGER].ours = {{0, 150}, 101, 20000};
        auto incumbent = session.quote(INCUMBENT, 0);
        auto challenger = session.quote(CHALLENGER, 0);
        Observer observer;
        RelativeStockRefinementCursor cursor;
        std::int64_t spent = 0;
        for (int step = 0; step < 3; ++step)
        {
            const auto result = refineRelativeStock(session, incumbent, challenger,
                {.workLimit = DEFAULT_RELATIVE_STOCK_WORK_LIMIT - spent,
                 .slice = RelativeStockSlice::Doubling, .maximumSteps = 1}, cursor, observer);
            spent += result.workUsed;
            expect(result.steps == 1 && result.stop == (step < 2 ? RelativeStockStop::StepLimit :
                RelativeStockStop::Certified), "one-step resumption yields without marking an unresolved term exhausted");
        }
        expect(observer.steps[0].requestedCap == LIVE_PAIR_SLICE_BASE &&
            observer.steps[1].requestedCap == 2 * LIVE_PAIR_SLICE_BASE &&
            observer.steps[2].requestedCap == 4 * LIVE_PAIR_SLICE_BASE,
            "resumed work advances existing deterministic rungs instead of repeating the first cap");
        expect(session.compare(incumbent, challenger).deltaLower == 1 && session.enemyCalls == 0,
            "resumed comparison preserves strict proof and shared-enemy cancellation");
        expect(spent == session.ledger.stats().spent() && spent <= DEFAULT_RELATIVE_STOCK_WORK_LIMIT,
            "all resumed steps charge one shared allocation");
    }

    void testInvalidRefinement()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (int fault = 0; fault < 3; ++fault)
        {
            Session session(field, enemy, 100);
            session.plans[INCUMBENT].ours = {{10, 20}, 15, 4};
            session.plans[CHALLENGER].ours = {{10, 20}, 16, 4};
            session.failWitness = fault == 0;
            session.widen = fault == 1;
            session.exceedCap = fault == 2;
            auto incumbent = session.quote(INCUMBENT, 0);
            auto challenger = session.quote(CHALLENGER, 0);
            const auto original = incumbent;
            Observer observer;
            const auto result = refineRelativeStock(session, incumbent, challenger, {.workLimit = 100}, observer);
            expect(result.stop == RelativeStockStop::InvalidRefinement && !result.comparison.improves(),
                   "failed witness, widening interval or exceeded state cap fails closed");
            expect(sameRelativeStockQuote(original, incumbent), "failed adoption preserves the prior quote");
        }
    }

    void testRetainedWitness()
    {
        RelativeStockRefinedTerm chessboard{
            .interval = {88000000, 294000000},
            .statesUsed = 156006,
            .workUsed = 160000,
            .searchCompleted = false,
            .lowerWitnessReplays = true,
            .valid = true,
        };
        expect(RelativeStockDetail::monotone({88000000, 344000000}, chessboard),
               "Chessboard can tighten an upper while retaining its witnessed 88m lower");
        chessboard.interval.lower = 102000000;
        chessboard.lowerWitnessReplays = false;
        expect(!RelativeStockDetail::monotone({88000000, 344000000}, chessboard),
               "Chessboard cannot adopt the unwitnessed 102m solver floor");

        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 20);
        session.plans[INCUMBENT].ours = {{10, 20}, 15, 1000};
        session.plans[CHALLENGER].ours = {{10, 20}, 16, 1000};
        auto incumbent = session.quote(INCUMBENT, 0);
        auto challenger = session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result = refineRelativeStock(session, incumbent, challenger, {.workLimit = 20}, observer);
        expect(observer.steps.size() == 1 && observer.steps.front().adopted &&
                   observer.steps.front().refined.interval.lower == observer.steps.front().before.lower,
               "an unchanged lower with its retained witness remains a valid refinement");
        expect(!incumbent.ourSearchCompleted && incumbent.ourLowerWitnessReplays &&
                   result.stop == RelativeStockStop::WorkBudget && result.comparison.unresolved(),
               "a retained witnessed floor is neither an exact search nor an improvement");
    }

    void testCapturedKeysShareEnemyAndReplay()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 100);
        session.plans[INCUMBENT] = {{{10, 10}, 10, 0}, {}, 0};
        session.plans[CHALLENGER] = {{{11, 11}, 11, 0}, {1}, 0};
        session.enemies[{}] = {{0, 200}, 100, 3};
        session.enemies[{1}] = {{0, 100}, 40, 4};
        auto incumbent = session.quote(INCUMBENT, 0);
        auto challenger = session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result = refineRelativeStock(session, incumbent, challenger, {.workLimit = 100}, observer);
        expect(result.comparison.improves() && result.comparison.commonEnemyCancelled &&
                   result.comparison.deltaLower == 1,
               "different captured keys cancel the enemy and prove exactly +1");
        expect(observer.steps.empty() && result.enemyStatesUsed == 0 && session.enemyCalls == 0 &&
                   session.ourCalls == 0,
               "uncertain enemy bounds under different captured keys receive no search");
        expect(replayRelativeStockProof(session, incumbent, challenger, incumbent, challenger),
               "complete matching decomposition and witnesses permit replay");
        expect(session.ourReplayCalls == 2 && session.enemyReplayCalls == 0,
               "replay verifies both our witnesses and no cancelled enemy witness");
        auto changed = challenger;
        changed.key = INCUMBENT;
        expect(!replayRelativeStockProof(session, incumbent, challenger, incumbent, changed), "replay matches the key");
        changed = challenger;
        ++changed.ours.upper;
        expect(!replayRelativeStockProof(session, incumbent, challenger, incumbent, changed), "replay matches both bounds");
        session.failReplay = true;
        expect(!replayRelativeStockProof(session, incumbent, challenger, incumbent, challenger),
               "an adoption cannot bypass a failing witness replay");

        Session shared(field, enemy, 100);
        shared.plans[INCUMBENT].ours = {{74, 74}, 74, 0};
        shared.plans[CHALLENGER].ours = {{75, 75}, 75, 0};
        shared.enemies[{}].witness = false;
        incumbent = shared.quote(INCUMBENT, 0);
        challenger = shared.quote(CHALLENGER, 0);
        expect(replayRelativeStockProof(shared, incumbent, challenger, incumbent, challenger),
               "shared-enemy proof replays without solving its cancelled lower witness");
        expect(shared.enemyCalls == 0 && shared.enemyReplayCalls == 0, "cancelled enemy has zero replay and solve work");
    }

    void testBoundedMixedRefundsAndLaterFittingKey()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 1000);
        RelativeStockTests::Valuer valuer(session);
        valuer.boundaryEnabled = true;
        session.plans[INCUMBENT].ours = {{0, 200}, 100, 1};
        valuer.boundaryCaps[INCUMBENT] = 100;
        valuer.registered.encounter(INCUMBENT, -1000, 200);
        const auto result = valuer.refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, 208);
        expect(result.valid && result.refined && result.keysRefined == 1 && result.workUsed == 10 &&
                   result.statesUsed == 2 && result.enemyStatesUsed == 1,
               "bounded mixed accounting reports actual states and tariffs after both normal refunds");
        expect(session.ledger.stats().granted == 208 && session.ledger.stats().refunded == 198 &&
                   session.ledger.stats().spent() == result.workUsed,
               "whole scheduled admission is preflighted but unused state grants are not counted as spent");

        Session later(field, enemy, 1000);
        RelativeStockTests::Valuer laterValuer(later);
        laterValuer.boundaryEnabled = true;
        later.enemies.at({}) = {{0, 0}, 0, 1, true};
        later.plans[INCUMBENT].ours = {{0, 9000}, 100, 1};
        later.plans[CHALLENGER].ours = {{0, 200}, 100, 1};
        laterValuer.boundaryCaps[INCUMBENT] = 100;
        laterValuer.registered.encounter(INCUMBENT, 0, 9000);
        laterValuer.registered.encounter(CHALLENGER, 0, 200);
        const auto unchanged = later.quote(INCUMBENT, 0);
        const auto order = laterValuer.registered.refinementOrder();
        expect(!order.empty() && order.front() == INCUMBENT,
               "the non-fitting key precedes the fitting key in the deterministic registry order");
        const auto fitting = laterValuer.refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, 4);
        expect(fitting.valid && fitting.budgetExhausted && fitting.refined &&
                   fitting.keysRefined == 1 && fitting.workUsed == 4 && later.ourCalls == 1,
               "a non-fitting ordered request does not prevent a later fitting key from refining");
        expect(sameRelativeStockQuote(unchanged, later.quote(INCUMBENT, 0)) &&
                   later.quote(CHALLENGER, 0).ours.exact() && later.ledger.stats().refusals == 0,
               "the skipped key is unchanged and makes no partial draw or global ledger refusal");
        expect(!laterValuer.globalClosed && !laterValuer.pricingFailure,
               "local round exhaustion leaves the shared pricing session open and valid");
    }

    void testMixedBoundaryYieldsAfterOneFittingKey()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const bool tightens : {true, false})
        {
            Session session(field, enemy, 1000);
            RelativeStockTests::Valuer valuer(session);
            valuer.boundaryEnabled = true;
            session.enemies.at({}) = {{0, 0}, 0, 1, true};
            session.plans[INCUMBENT].ours = {{0, 9000}, 100, tightens ? 1 : 100};
            session.plans[CHALLENGER].ours = {{0, 200}, 100, 1};
            valuer.registered.encounter(INCUMBENT, 0, 9000);
            valuer.registered.encounter(CHALLENGER, 0, 200);
            const auto laterBefore = session.quote(CHALLENGER, 0);
            const auto result = valuer.refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, 1000);
            expect(result.valid && result.refined == tightens && result.keysRefined == (tightens ? 1 : 0),
                   "one fitting mixed request reports actual tightening, not merely work spent");
            expect(result.workUsed == 4 && result.statesUsed == 1 && session.ourCalls == 1,
                   "one mixed key yields with work available for the relative scheduler");
            expect(sameRelativeStockQuote(laterBefore, session.quote(CHALLENGER, 0)) &&
                       !result.budgetExhausted && !valuer.globalClosed && !valuer.pricingFailure,
                   "yielding preserves later keys without fabricating local exhaustion or global closure");
            if (tightens)
            {
                const auto next = valuer.refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, 996);
                expect(next.valid && next.refined && session.quote(CHALLENGER, 0).ours.exact() &&
                           session.ledger.stats().spent() == 8,
                       "a later boundary deterministically advances to the next unresolved key");
            }
        }
    }

    void testVerifiedDualIntegration()
    {
        constexpr MilliFunds MILLION = 1000000;
        constexpr MilliFunds VERIFIED_UPPER =
            101666666;
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(
            field,
            enemy,
            DEFAULT_RELATIVE_STOCK_WORK_LIMIT);
        session.dualEnabled = true;
        session.plans[INCUMBENT].ours = {
            {100 * MILLION, 200 * MILLION},
            100 * MILLION,
            1,
        };
        session.plans[CHALLENGER].ours = {
            {0, 250 * MILLION},
            103 * MILLION,
            29232,
        };
        session.plans[SECOND_CHALLENGER].ours = {
            {0, 260 * MILLION},
            104 * MILLION,
            7,
        };
        session.dualUppers[INCUMBENT] =
            VERIFIED_UPPER;
        auto incumbent =
            session.quote(INCUMBENT, 0);
        auto challenger =
            session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result =
            refineRelativeStock(
                session,
                incumbent,
                challenger,
                {
                    .workLimit =
                        DEFAULT_RELATIVE_STOCK_WORK_LIMIT,
                },
                observer);
        expect(
            result.comparison.improves() &&
                challenger.ours.lower ==
                    103 * MILLION &&
                incumbent.ours.upper ==
                    VERIFIED_UPPER &&
                result.comparison.deltaLower ==
                    1333334,
            "VERIFIED_DUAL_UPPER_ACCEPTS_STRICT_CHALLENGER");
        expect(
            observer.steps.size() == 2 &&
                observer.steps[1].verifiedDual &&
                observer.steps[1].dual.catalogComplete &&
                observer.steps[1].dual.optimizerCompleted &&
                observer.steps[1].dual.verifierCompleted &&
                observer.steps[1].dual.certificateValid,
            "verified dual is adopted only after complete production-shaped receipts");
        expect(
            incumbent.ours.lower ==
                    100 * MILLION &&
                incumbent.ourLowerWitnessReplays &&
                !incumbent.ourSearchCompleted,
            "LOWER_WITNESS_PRESERVED_WHEN_UPPER_TIGHTENS");
        expect(
            !incumbent.absoluteExact() &&
                !challenger.absoluteExact(),
            "ABSOLUTE_EXACTNESS_REMAINS_FALSE");
        expect(
            result.statesUsed == 29232 &&
                result.workUsed ==
                    session.ledger.stats().spent() &&
                session.ledger.stats()
                        .grantedByWork[
                            static_cast<std::size_t>(
                                RefinementWork::
                                    PackingDualCertificate)] ==
                    observer.steps[1].dual.totalWork,
            "dual search and certificate work use one conserved ledger");
        expect(
            replayRelativeStockProof(
                session,
                incumbent,
                challenger,
                session.quote(INCUMBENT, 0),
                session.quote(CHALLENGER, 0)),
            "verified dual comparison replays both lower witnesses");

        auto stale =
            session.quote(SECOND_CHALLENGER, 0);
        stale.ourUpperCertificateId =
            incumbent.ourUpperCertificateId;
        expect(
            !session.compare(stale, challenger).valid(),
            "STALE_CERTIFICATE_AFTER_REPLACEMENT_REJECTED");

        const auto reused =
            session.tightenOurUpperWithVerifiedDual(
                incumbent,
                session.remainingWork());
        expect(
            reused.reused &&
                reused.totalWork == 0 &&
                reused.certificateId ==
                    incumbent.ourUpperCertificateId &&
                session.dualBuilds == 1,
            "SHARED_INCUMBENT_CERTIFICATE_REUSED_ONCE");

        auto secondIncumbent =
            session.quote(INCUMBENT, 0);
        auto secondChallenger =
            session.quote(SECOND_CHALLENGER, 0);
        Observer secondObserver;
        const std::int64_t spentBefore =
            session.ledger.stats().spent();
        const auto second =
            refineRelativeStock(
                session,
                secondIncumbent,
                secondChallenger,
                {
                    .workLimit =
                        session.remainingWork(),
                },
                secondObserver);
        expect(
            second.comparison.improves() &&
                session.dualBuilds == 1 &&
                session.dualCalls == 2 &&
                secondIncumbent.ourUpperCertificateId ==
                    incumbent.ourUpperCertificateId &&
                session.ledger.stats().spent() -
                        spentBefore ==
                    second.workUsed,
            "one incumbent certificate proves a later challenger without recomputation");
    }

    void testVerifiedDualProductionSchedule()
    {
        constexpr MilliFunds MILLION = 1000000;
        constexpr std::int64_t PREFIX_WORK = 42953;
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(
            field,
            enemy,
            DEFAULT_RELATIVE_STOCK_WORK_LIMIT);
        expect(
            session.ledger.draw(
                PREFIX_WORK,
                RefinementWork::MandatoryPrecompute) !=
                NO_GRANT,
            "production schedule accounts for the existing mixed prefix");
        session.dualEnabled = true;
        session.ourTariff = 10014;
        session.dualCatalogWork = 6492;
        session.dualOptimizerWork = 59199;
        session.dualVerifierWork = 8988;
        session.plans[INCUMBENT].ours = {
            {65 * MILLION, 114 * MILLION},
            100 * MILLION,
            LIVE_PAIR_REFINEMENT_BUDGET,
        };
        session.plans[CHALLENGER].ours = {
            {72 * MILLION, 126 * MILLION},
            110 * MILLION,
            LIVE_PAIR_REFINEMENT_BUDGET,
            false,
            true,
            SearchPrefix{
                .states = 2 * LIVE_PAIR_SLICE_BASE,
                .bounds = {
                    102 * MILLION,
                    124 * MILLION,
                },
            },
        };
        session.dualUppers[INCUMBENT] =
            101666666;
        auto incumbent =
            session.quote(INCUMBENT, 0);
        auto challenger =
            session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result =
            refineRelativeStock(
                session,
                incumbent,
                challenger,
                {
                    .workLimit =
                        session.remainingWork(),
                    .slice =
                        RelativeStockSlice::Doubling,
                },
                observer);
        expect(
            result.comparison.improves() &&
                observer.steps.size() == 2 &&
                observer.steps[0].target ==
                    RelativeStockTarget::
                        ChallengerOurs &&
                observer.steps[0].requestedCap ==
                    2 * LIVE_PAIR_SLICE_BASE &&
                observer.steps[1].verifiedDual,
            "doubling production schedule reaches the verified dual");
        expect(
            result.workUsed == 113925 &&
                session.ledger.stats().spent() ==
                    156878 &&
                session.remainingWork() == 3122,
            "production challenger and dual work fit the shared allocation");
    }

    void testVerifiedDualLocalExhaustion()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 8);
        session.dualEnabled = true;
        session.plans[INCUMBENT].ours = {
            {100, 200},
            100,
            1,
        };
        session.plans[CHALLENGER].ours = {
            {0, 250},
            103,
            1,
        };
        session.dualUppers[INCUMBENT] = 101;
        auto incumbent =
            session.quote(INCUMBENT, 0);
        auto challenger =
            session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result =
            refineRelativeStock(
                session,
                incumbent,
                challenger,
                {
                    .workLimit = 8,
                    .maximumSteps = 2,
                },
                observer);
        expect(
            result.stop ==
                    RelativeStockStop::StepLimit &&
                result.comparison.unresolved() &&
                result.dualAttempts == 1 &&
                result.dualTightenings == 0 &&
                observer.steps.back()
                    .dual.locallyBudgetExhausted &&
                !observer.steps.back()
                     .dual.globalFailure &&
                session.dualCertificates.empty(),
            "LOCAL_BUDGET_EXHAUSTION_IS_NOT_GLOBAL_FAILURE");
        expect(
            session.ledger.stats().spent() == 4 &&
                session.remainingWork() == 4 &&
                session.ledger.stats()
                        .refusalsByWork[
                            static_cast<std::size_t>(
                                RefinementWork::
                                    PackingDualCertificate)] ==
                    0,
            "local verifier shortage makes no partial dual draw");
    }

    void testMismatchedDualReceiptFailsClosed()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 100);
        session.dualEnabled = true;
        session.corruptDualReceipt = true;
        session.plans[INCUMBENT].ours = {
            {100, 200}, 100, 1};
        session.plans[CHALLENGER].ours = {
            {0, 250}, 103, 1};
        session.dualUppers[INCUMBENT] = 101;
        auto incumbent =
            session.quote(INCUMBENT, 0);
        auto challenger =
            session.quote(CHALLENGER, 0);
        Observer observer;
        const auto result =
            refineRelativeStock(
                session,
                incumbent,
                challenger,
                {
                    .workLimit = 100,
                },
                observer);
        expect(
            result.stop ==
                    RelativeStockStop::
                        InvalidRefinement &&
                result.dualAttempts == 1 &&
                result.steps == 2 &&
                session.ourCalls == 1,
            "mismatched verified-dual receipt fails closed immediately");
    }

    void testDualAndMixedShareLedger()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 100);
        RelativeStockTests::Valuer valuer(session);
        valuer.boundaryEnabled = true;
        session.enemies.at({}) = {
            {0, 0}, 0, 1, true};
        session.plans[SECOND_CHALLENGER].ours = {
            {0, 20}, 10, 1};
        valuer.registered.encounter(
            SECOND_CHALLENGER,
            0,
            20);
        const auto mixed =
            valuer.refineLiveAtBoundary(
                AssignPhase::BetweenSwapSweeps,
                20);

        session.dualEnabled = true;
        session.plans[INCUMBENT].ours = {
            {100, 200}, 100, 1};
        session.plans[CHALLENGER].ours = {
            {0, 250}, 103, 1};
        session.dualUppers[INCUMBENT] = 101;
        auto incumbent =
            session.quote(INCUMBENT, 0);
        auto challenger =
            session.quote(CHALLENGER, 0);
        Observer observer;
        const auto relative =
            refineRelativeStock(
                session,
                incumbent,
                challenger,
                {
                    .workLimit =
                        session.remainingWork(),
                },
                observer);
        expect(
            mixed.workUsed > 0 &&
                relative.comparison.improves() &&
                session.ledger.stats().spent() ==
                    mixed.workUsed +
                        relative.workUsed &&
                session.remainingWork() ==
                    100 -
                        mixed.workUsed -
                        relative.workUsed,
            "DUAL_AND_MIXED_WORK_SHARE_ONE_LEDGER");
    }
}

int main()
{
    testTargeting();
    testTieAndBudget();
    testCheckedThresholdArithmeticAndCommonEnemyWork();
    testResumedRungs();
    testInvalidRefinement();
    testRetainedWitness();
    testCapturedKeysShareEnemyAndReplay();
    testBoundedMixedRefundsAndLaterFittingKey();
    testMixedBoundaryYieldsAfterOneFittingKey();
    testVerifiedDualIntegration();
    testVerifiedDualProductionSchedule();
    testVerifiedDualLocalExhaustion();
    testMismatchedDualReceiptFailsClosed();
    testDualAndMixedShareLedger();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
