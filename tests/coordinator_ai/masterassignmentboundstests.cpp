#include <random>

#include <QCoreApplication>

#include "ai/coordinator/masterassignmentbounds.h"
#include "ai/coordinator/masterassignmentevaluator.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentTest;

    void checkNode(const MasterAssignmentProblem & problem, const MasterAssignmentState & state,
                   MasterAssignmentBounds & bounds, RefinementLedger & ledger)
    {
        auto reference = request();
        reference.domain = MasterAssignmentOracle::fullDomain(problem);
        reference.restriction = QStringLiteral("partial-state completion domain");
        for (std::size_t actor = 0; actor < state.choices.size(); ++actor)
        {
            if (state.choices[actor] != MASTER_UNASSIGNED)
            {
                reference.domain[actor] = {state.choices[actor]};
            }
        }
        const auto exact = MasterAssignmentOracle::enumerate(problem, reference);
        for (const auto family : {MasterBoundFamily::Immediate, MasterBoundFamily::CurrentResource,
                                   MasterBoundFamily::EndpointCoupled})
        {
            const auto upper = bounds.bound(state, family, ledger);
            require(upper.valid(), "complete immediate bound");
            expect(!exact.exactOptimum || upper.certificate->upper >= *exact.exactOptimum,
                   "partial-state upper covers independent exact optimum");
            const auto verified = bounds.verify(*upper.certificate, ledger);
            require(verified.valid(), "independent full-domain verification");
            const auto tight = bounds.tighten(*upper.certificate, 8, ledger);
            require(tight.valid(), "dual optimization verifies");
            expect(!exact.exactOptimum || tight.certificate->upper >= *exact.exactOptimum,
                   "optimized price certificate covers independent exact optimum");
            expect(tight.certificate->upper <= upper.certificate->upper, "minimum independently valid bound");
            auto corrupt = *upper.certificate;
            --corrupt.upper;
            expect(!bounds.verify(corrupt, ledger).valid(), "forged scalar rejected");
            if (!corrupt.prices.empty())
            {
                corrupt = *upper.certificate;
                corrupt.prices.front() = -1;
                expect(!bounds.verify(corrupt, ledger).valid(), "negative price rejected");
            }
        }
    }

    void generatedDomains()
    {
        for (std::uint32_t seed = 1101; seed <= 1148; ++seed)
        {
            std::mt19937 random(seed);
            Fixture fixture(3);
            for (std::int32_t actor = 0; actor < 3; ++actor)
            {
                for (std::int32_t choice = 1; choice <= 2; ++choice)
                {
                    fixture.input.actors[actor].candidates.push_back(moveCandidate(
                        actor, {actor, 0}, {static_cast<std::int32_t>(random() % 4), 1},
                        static_cast<MilliFunds>(random() % 81) - 40, choice));
                }
            }
            fixture.seat();
            if (seed % 3 == 0)
            {
                fixture.selections[0].role = MasterActorRole::Fixed;
            }
            auto problem = fixture.extract();
            RefinementLedger ledger;
            require(ledger.open(10000000), "bound ledger");
            MasterAssignmentBounds bounds(problem);
            require(bounds.prepare(nullptr, ledger) == MasterBoundStatus::Valid, "bound preparation");
            auto root = masterRootState(problem, ledger);
            require(root.has_value(), "root state");
            checkNode(problem, *root, bounds, ledger);
            for (std::size_t actor = 0; actor < problem.actors().size(); ++actor)
            {
                if (!problem.canChangeActor(static_cast<std::int32_t>(actor)))
                {
                    continue;
                }
                for (const auto choice : problem.actors()[actor].canonicalCandidates)
                {
                    auto partial = *root;
                    partial.choices[actor] = choice;
                    checkNode(problem, partial, bounds, ledger);
                }
            }
            MasterAssignmentEvaluator evaluator(problem, ledger);
            const auto exact = MasterAssignmentOracle::enumerate(problem, request());
            for (const auto & tuple : exact.tuples)
            {
                const auto evaluation = evaluator.evaluate({problem.identity(), tuple.choices});
                expect((evaluation.status == MasterEvaluationStatus::Feasible) == tuple.reference.complete,
                       "production evaluator feasibility agrees with independent assembly");
                if (evaluation.objective)
                {
                    expect(evaluation.lowerReplayed && evaluation.upperVerified && evaluation.exact,
                           "immediate objective is replayed and exact");
                    expect(evaluation.objective->lower == tuple.objective.total &&
                           evaluation.objective->upper == tuple.objective.total,
                           "seated objective equals independent oracle");
                }
            }
        }
    }

    void boundaries()
    {
        Fixture fixture(1);
        for (std::int32_t choice = 1; choice <= 80; ++choice)
        {
            fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {choice, 1}, choice, choice));
        }
        fixture.seat();
        const auto problem = fixture.extract();
        expect(problem.resources().size() > 64, "large resource domain retained");
        RefinementLedger ledger;
        require(ledger.open(10000000), "large bound ledger");
        MasterAssignmentBounds bounds(problem);
        require(bounds.prepare(nullptr, ledger) == MasterBoundStatus::Valid, "large preparation");
        const auto root = masterRootState(problem, ledger);
        require(root.has_value(), "large root");
        checkNode(problem, *root, bounds, ledger);
        const auto certificate = bounds.bound(*root, MasterBoundFamily::EndpointCoupled, ledger);
        auto other = fixture.extract();
        auto stale = *certificate.certificate;
        stale.state.identity = other.identity();
        expect(!bounds.verify(stale, ledger).valid(), "equal session number cannot alias problem identity");
        stale = *certificate.certificate;
        stale.state.choices[0] = 900;
        expect(!bounds.verify(stale, ledger).valid(), "invalid partial choice rejected");
        for (std::int64_t budget = 0; budget < 100; ++budget)
        {
            RefinementLedger limited;
            require(limited.open(budget), "tiny ledger");
            MasterAssignmentBounds interrupted(problem);
            const auto status = interrupted.prepare(nullptr, limited);
            expect(status == MasterBoundStatus::BudgetExhausted && !interrupted.ready(),
                   "incomplete setup never publishes a full-domain bound");
            expect(limited.stats().spent() <= budget, "setup never overspends");
        }
        for (std::int64_t budget = 0; budget < 100; ++budget)
        {
            RefinementLedger limited;
            require(limited.open(budget), "verifier ledger");
            const auto result = bounds.verify(*certificate.certificate, limited);
            expect(result.status == MasterBoundStatus::BudgetExhausted && !result.valid(),
                   "partial verification is not a valid certificate");
            expect(limited.stats().spent() <= budget, "verification never overspends");
        }
        auto reassigned = fixture.extract();
        MasterAssignmentBounds boundBeforeAssignment(reassigned);
        require(boundBeforeAssignment.prepare(nullptr, ledger) == MasterBoundStatus::Valid,
                "replacement identity fixture prepares");
        MasterAssignmentEvaluator evaluatorBeforeAssignment(reassigned, ledger);
        reassigned = fixture.extract();
        const auto replacedRoot = masterRootState(reassigned, ledger);
        require(replacedRoot.has_value(), "replacement root identity");
        expect(!boundBeforeAssignment.ready() &&
               !boundBeforeAssignment.bound(*replacedRoot, MasterBoundFamily::Immediate, ledger).valid() &&
               !evaluatorBeforeAssignment.matches(reassigned, ledger),
               "reassigning the referenced problem cannot silently reuse old valuation state");
    }

    void clippedDamage()
    {
        Fixture fixture(2);
        fixture.input.unitLinks.push_back({900, {4, 1}});
        for (std::int32_t actor = 0; actor < 2; ++actor)
        {
            auto fire = moveCandidate(actor, {actor, 0}, {actor, 0}, 100, 1);
            fire.bundle.components.push_back(fireComponent({
                .targetUnitId = 2,
                .targetReplacementCost = 100,
                .targetHpSteps = 10,
                .damageSteps = 8,
                .targetBestShotBefore = 300,
                .targetBestShotAfter = 100,
            }));
            fixture.input.actors[actor].candidates.push_back(fire);
        }
        fixture.seat();
        const auto problem = fixture.extract();
        RefinementLedger ledger;
        require(ledger.open(1000000), "damage ledger");
        MasterAssignmentBounds bounds(problem);
        require(bounds.prepare(nullptr, ledger) == MasterBoundStatus::Valid, "damage bound prepare");
        auto root = masterRootState(problem, ledger);
        require(root.has_value(), "damage root");
        checkNode(problem, *root, bounds, ledger);
        root->choices[0] = 1;
        checkNode(problem, *root, bounds, ledger);
        expect(!problem.incompatible({0, 1}, {1, 1}), "clipped damage is not additive capacity conflict");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    generatedDomains();
    boundaries();
    clippedDamage();
    std::printf("Master assignment bounds: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
