#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/propertystockfield.h"
#include "tests/coordinator_ai/masterassignmentoracle.h"

namespace
{
    using namespace Coordinator;
    using namespace MasterAssignmentOracle;

    int checks = 0;
    int failures = 0;
    std::uint64_t totalTuples = 0;
    std::uint64_t totalContinuationWork = 0;
    std::uint64_t totalNanos = 0;
    std::uint64_t maximumExportBytes = 0;
    std::uint64_t caseCount = 0;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }

    void require(bool condition, const char* message)
    {
        expect(condition, message);
        if (!condition)
        {
            std::exit(4);
        }
    }

    CandidateBundle moveCandidate(std::int32_t actor, TilePoint origin, TilePoint destination,
                                  MilliFunds value, std::int32_t generation = 0)
    {
        CandidateBundle result;
        result.generationIndex = generation;
        result.bundle.unitId = actor;
        result.bundle.origin = origin;
        result.bundle.destination = destination;
        result.bundle.path = {origin};
        if (origin != destination)
        {
            result.bundle.path.push_back(destination);
        }
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        return result;
    }

    struct Fixture
    {
        AssignmentInput input;
        TurnPlan plan;
        std::vector<MasterActorSelection> selections;

        explicit Fixture(std::int32_t count)
        {
            input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"),
                               QStringLiteral("ACTION_CAPTURE")};
            for (std::int32_t slot = 0; slot < count; ++slot)
            {
                const TilePoint origin{slot, 0};
                input.unitLinks.push_back({100 + slot, origin});
                input.actors.push_back({slot, 100 + slot, {moveCandidate(slot, origin, origin, slot + 1)}});
            }
        }

        void seat(std::span<const std::int32_t> choices = {}, bool reverseDamage = false)
        {
            plan = TurnPlan{};
            selections.clear();
            for (std::size_t slot = 0; slot < input.actors.size(); ++slot)
            {
                const auto & actor = input.actors[slot];
                const auto choice = choices.empty() ? 0 : choices[slot];
                const auto & candidate = actor.candidates[choice];
                const auto index = plan.addAction(plannedActionFrom(
                    input.actionIds, input.unitLinks, actor.engineUnitId, candidate));
                require(index != NO_ACTION, "fixture action construction");
                selections.push_back({actor.engineUnitId, index, choice, MasterActorRole::Mutable});
            }
            for (std::size_t offset = 0; offset < input.actors.size(); ++offset)
            {
                const auto slot = reverseDamage ? input.actors.size() - offset - 1 : offset;
                require(claimFor(plan, static_cast<std::int32_t>(slot),
                                 input.actors[slot].candidates[selections[slot].candidateIndex]) ==
                            ReservationResult::Granted,
                        "fixture native claims");
            }
            std::vector<OccupancyEntry> occupancy;
            for (const auto & link : input.unitLinks)
            {
                occupancy.push_back({link.engineUnitId, link.tile});
            }
            require(plan.rebuildVacateEdges(occupancy).empty(), "fixture occupancy");
        }

        MasterAssignmentProblem extract() const
        {
            const auto order = plan.executionOrder().order;
            const auto result = buildMasterAssignmentProblem(input, {
                .sessionIdentity = 71,
                .selections = selections,
                .pIncumbentPlan = &plan,
                .incumbentExecutionOrder = order,
            });
            if (!result.valid())
            {
                std::printf("EXTRACTION: error=%d actor=%d candidate=%d action=%d\n",
                            static_cast<int>(result.error), result.actorSlot, result.candidateIndex, result.actionIndex);
            }
            require(result.valid(), "fixture extracts into accepted production model");
            return *result.problem;
        }
    };

    Request requestFor(QString name, std::uint64_t seed = 0)
    {
        Request request;
        request.fixture = std::move(name);
        request.seed = seed;
        request.sourceRevision = QString::fromLocal8Bit(qgetenv("COW_ORACLE_SOURCE_REVISION"));
        if (request.sourceRevision.isEmpty())
        {
            request.sourceRevision = QStringLiteral("43bbd8d4f44b3100d14247dc56ba5b9dae666aca+phase2-working");
        }
        return request;
    }

    Result run(const MasterAssignmentProblem & problem, const Request & request)
    {
        auto result = enumerate(problem, request);
        const auto repeat = enumerate(problem, request);
        const auto bytes = exportDiagnostic(problem, request, result);
        expect(bytes == exportDiagnostic(problem, request, repeat), "deterministic complete diagnostic bytes");
        expect(result.falsePositives == 0 && result.falseNegatives == 0 && result.reconstructionMismatches == 0,
               "independent reference has no production feasibility or state mismatch");
        if (result.reconstructionMismatches != 0)
        {
            for (const auto & tuple : result.tuples)
            {
                if (tuple.comparisonApplicable && !tuple.comparisonEqual)
                {
                    std::printf("MISMATCH %s tuple:", qPrintable(request.fixture));
                    for (const auto choice : tuple.choices)
                    {
                        std::printf(" %d", choice);
                    }
                    std::printf(" reference=%d complete=%d production=%d complete=%d reason=%s\n",
                                static_cast<int>(tuple.reference.status), tuple.reference.complete,
                                static_cast<int>(tuple.productionError), tuple.productionComplete,
                                qPrintable(tuple.reference.reason));
                }
            }
        }
        const auto directory = QString::fromLocal8Bit(qgetenv("COW_MASTER_ORACLE_OUTPUT"));
        if (!directory.isEmpty())
        {
            require(QDir().mkpath(directory), "create isolated diagnostic output directory");
            QFile output(QDir(directory).filePath(request.fixture + QStringLiteral(".json")));
            require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.flush(),
                    "write complete deterministic diagnostic");
        }
        ++caseCount;
        totalTuples += result.evaluated;
        totalContinuationWork += result.continuationWork;
        totalNanos += result.elapsedNanos;
        maximumExportBytes = std::max(maximumExportBytes, static_cast<std::uint64_t>(bytes.size()));
        std::printf("CASE %s seed=%llu product=%llu evaluated=%llu feasible=%llu unsupported=%llu unresolved=%llu status=%d ns=%llu bytes=%llu\n",
                    qPrintable(request.fixture), static_cast<unsigned long long>(request.seed),
                    static_cast<unsigned long long>(result.tupleProduct.value_or(0)),
                    static_cast<unsigned long long>(result.evaluated), static_cast<unsigned long long>(result.feasible),
                    static_cast<unsigned long long>(result.unsupported), static_cast<unsigned long long>(result.unresolved),
                    static_cast<int>(result.status), static_cast<unsigned long long>(result.elapsedNanos),
                    static_cast<unsigned long long>(bytes.size()));
        return result;
    }

    void testSingletonFixedAndEquality()
    {
        Fixture fixture(3);
        fixture.seat();
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto singleton = run(fixture.extract(), requestFor(QStringLiteral("singleton-fixed")));
        expect(singleton.status == ResultStatus::ExactOptimum && singleton.evaluated == 1 &&
               singleton.exactOptimum == 6 && singleton.incumbentRetained, "singleton exact optimum");
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 1, -10));
        fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {1, 1}, 9999, -10));
        const auto equal = run(fixture.extract(), requestFor(QStringLiteral("equality-keeps-incumbent")));
        expect(equal.evaluated == 2 && equal.incumbentRetained &&
               equal.tuples[*equal.representative].choices == std::vector<std::int32_t>({0, 0, 0}),
               "fixed alternative excluded and optimal incoming incumbent retained");
        fixture.input.actors[0].candidates[0].valuation.ledger.enemyCapital = -5;
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 2}, 1, 20));
        fixture.seat();
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto canonical = run(fixture.extract(), requestFor(QStringLiteral("canonical-equal-improvement")));
        expect(canonical.status == ResultStatus::ExactOptimum && !canonical.incumbentRetained &&
               canonical.tuples[*canonical.representative].choices[0] == 1,
               "canonical generation order selects representative separately from equal optimum");
        std::reverse(fixture.input.actors[0].candidates.begin(), fixture.input.actors[0].candidates.end());
        fixture.seat(std::vector<std::int32_t>{2, 0, 0});
        fixture.selections[1].role = MasterActorRole::Fixed;
        const auto reorderedProblem = fixture.extract();
        const auto reordered = enumerate(reorderedProblem, requestFor(QStringLiteral("reordered-candidates")));
        const auto selected = reordered.tuples[*reordered.representative].choices[0];
        expect(reordered.exactOptimum == canonical.exactOptimum &&
               reorderedProblem.actors()[0].candidates[selected].identity.generationIndex == -10,
               "original indices may change but canonical candidate identity determines equal representative");
    }

    void testThreeActorImprovement()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.input.actors[actor].candidates = {
                moveCandidate(actor, {actor, 0}, {actor, 1}, 0),
                moveCandidate(actor, {actor, 0}, {(actor + 1) % 3, 1}, 10, 1)};
        }
        fixture.seat();
        const auto result = run(fixture.extract(), requestFor(QStringLiteral("three-actor-only-improvement")));
        expect(result.status == ResultStatus::ExactOptimum && result.evaluated == 8 && result.feasible == 2 &&
               result.exactOptimum == 30 &&
               result.tuples[*result.representative].choices == std::vector<std::int32_t>({1, 1, 1}),
               "improvement requires all three mutable assignments to change jointly");
        expect(checkSearchClaim(result, 0, 30, 30) == SearchClaimCheck::Consistent &&
               checkSearchClaim(result, 31, 40) == SearchClaimCheck::LowerAboveOptimum &&
               checkSearchClaim(result, 0, 29) == SearchClaimCheck::UpperBelowOptimum &&
               checkSearchClaim(result, 0, 30, 0) == SearchClaimCheck::CompletedValueMismatch,
               "future search comparison interface rejects synthetic false bounds and completion");
        const std::vector<std::vector<std::int32_t>> retained{{1, 1, 1}};
        const std::vector<std::vector<std::int32_t>> prunedWinner{{0, 0, 0}};
        expect(retainsOptimum(result, retained) && !retainsOptimum(result, prunedWinner),
               "future pruning comparison detects loss of the only optimum");
    }

    void testVacatesAndProduction()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates = {
            moveCandidate(0, {0, 0}, {-1, 0}, 1), moveCandidate(0, {0, 0}, {1, 0}, 2, 1)};
        fixture.input.actors[1].candidates = {
            moveCandidate(1, {1, 0}, {0, 0}, 1), moveCandidate(1, {1, 0}, {2, 0}, 2, 1)};
        fixture.seat();
        const auto result = run(fixture.extract(), requestFor(QStringLiteral("vacate-chain-reversal-cycle")));
        expect(result.evaluated == 4 && result.feasible == 3, "cyclic leaf alone is infeasible");
        expect(result.tuples.back().reference.ordering.order == std::vector<std::int32_t>({1, 0}),
               "reverse chain independently orders departing actor first");
        fixture.input.actors[0].candidates[0].vacatesActiveFriendlyProduction = true;
        fixture.seat();
        fixture.selections[0].role = MasterActorRole::Fixed;
        const auto fixed = run(fixture.extract(), requestFor(QStringLiteral("fixed-production-vacate")));
        expect(fixed.evaluated == 2 && fixed.feasible == 2, "fixed production vacate preserves both mutable choices");
        expect(fixed.tuples[0].reference.plan.action(0) == fixture.plan.action(0), "fixed production actor unchanged");
    }

    void testRevival()
    {
        for (const bool retainedSlot : {false, true})
        {
            Fixture fixture(2);
            fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {1, 0}, 4, 1));
            fixture.input.actors[1].candidates.push_back(moveCandidate(1, {1, 0}, {2, 0}, 3, 1));
            fixture.plan.addAction(plannedActionFrom(fixture.input.actionIds, fixture.input.unitLinks, 100,
                                                     fixture.input.actors[0].candidates[0]));
            fixture.plan.claimDestination(0, {0, 0});
            if (retainedSlot)
            {
                auto abandoned = plannedActionFrom(fixture.input.actionIds, fixture.input.unitLinks, 101,
                                                    fixture.input.actors[1].candidates[0]);
                abandoned.state = PlanActionState::Abandoned;
                fixture.plan.addAction(abandoned);
            }
            fixture.selections = {{100, 0, 0, MasterActorRole::Mutable},
                {101, retainedSlot ? 1 : NO_ACTION, NO_CANDIDATE, MasterActorRole::Mutable}};
            const auto problem = fixture.extract();
            const auto incumbent = constructReference(problem, problem.incumbentChoices());
            expect(incumbent.representationValid && !incumbent.complete, "unassigned incumbent is not complete");
            const auto result = run(problem, requestFor(retainedSlot ? QStringLiteral("revived-occupancy")
                                                                    : QStringLiteral("previously-unassigned")));
            expect(result.evaluated == 4 && result.feasible == 3 && result.exactOptimum == 7,
                   "complete enumeration includes revived actors and retains occupied origins");
        }
    }

    void addAttack(Fixture & fixture, std::int32_t actor, std::int32_t choice, std::int32_t damage)
    {
        fixture.input.actors[actor].candidates[choice].bundle.components.push_back(fireComponent({
            .targetUnitId = static_cast<std::int32_t>(fixture.input.unitLinks.size() - 1),
            .targetReplacementCost = 1000, .targetHpSteps = 10, .damageSteps = damage}));
    }

    void testDamageAndDependencies()
    {
        Fixture fixed(2);
        fixed.input.unitLinks.push_back({999, {5, 5}});
        fixed.input.actors[0].candidates.push_back(fixed.input.actors[0].candidates[0]);
        fixed.input.actors[0].candidates[1].generationIndex = 1;
        addAttack(fixed, 0, 1, 8);
        addAttack(fixed, 1, 0, 8);
        fixed.seat();
        fixed.selections[1].role = MasterActorRole::Fixed;
        auto result = run(fixed.extract(), requestFor(QStringLiteral("fixed-damage-share")));
        expect(result.tuples.back().reference.plan.action(0).plannedDamageSteps == 2 &&
               result.tuples.back().reference.plan.action(1).plannedDamageSteps == 8,
               "fresh mutable attacker cannot steal fixed eight damage");
        expect(result.tuples.back().reference.seatedTotal != 3, "seated value does not substitute static total");
        fixed.plan.commit(1);
        fixed.selections[1].role = MasterActorRole::Mutable;
        result = run(fixed.extract(), requestFor(QStringLiteral("committed-damage-share")));
        expect(result.tuples.back().reference.plan.action(1).state == PlanActionState::Committed,
               "committed actor immutable despite mutable role");

        Fixture mutableDamage(2);
        mutableDamage.input.unitLinks.push_back({999, {5, 5}});
        mutableDamage.input.actors[1].candidates.push_back(mutableDamage.input.actors[1].candidates[0]);
        mutableDamage.input.actors[1].candidates[1].generationIndex = 1;
        addAttack(mutableDamage, 0, 0, 8);
        addAttack(mutableDamage, 1, 0, 8);
        mutableDamage.seat({}, true);
        result = run(mutableDamage.extract(), requestFor(QStringLiteral("fresh-mutable-damage")));
        expect(result.tuples.front().reference.plan.action(0).plannedDamageSteps == 2 &&
               result.tuples.back().reference.plan.action(0).plannedDamageSteps == 8,
               "incumbent historical partial share and changed fresh share differ correctly");

        Fixture wall(2);
        wall.input.unitLinks.push_back({999, {5, 5}});
        wall.input.actors[0].candidates.push_back(wall.input.actors[0].candidates[0]);
        wall.input.actors[0].candidates.push_back(wall.input.actors[0].candidates[0]);
        wall.input.actors[0].candidates[1].generationIndex = 1;
        wall.input.actors[0].candidates[2].generationIndex = 2;
        addAttack(wall, 0, 0, 10);
        addAttack(wall, 0, 2, 8);
        wall.seat();
        require(wall.plan.addWallbreakEdge(0, 1), "fixture lethal dependency");
        result = run(wall.extract(), requestFor(QStringLiteral("wallbreak-prerequisite")));
        expect(result.evaluated == 3 && result.feasible == 1 && result.infeasible == 2,
               "removed or weakened lethal prerequisite cannot remain executable");
    }

    void testUnsupportedAndPartial()
    {
        for (const auto kind : {PlanEdgeKind::Screen, PlanEdgeKind::Coverage, PlanEdgeKind::Service})
        {
            Fixture fixture(2);
            fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 1, 1));
            fixture.seat();
            fixture.plan.addEdge(0, 1, kind);
            const auto result = run(fixture.extract(), requestFor(QStringLiteral("unsupported-edge-%1").arg(static_cast<int>(kind))));
            expect(result.status == ResultStatus::Unsupported && result.evaluated == 2 && result.unsupported == 1 &&
                   !result.exactOptimum, "changed unproved dependency does not become infeasibility or an optimum");
        }
        Fixture service(1);
        service.input.unitLinks.push_back({999, {5, 5}});
        auto alternative = service.input.actors[0].candidates[0];
        alternative.generationIndex = 1;
        alternative.bundle.components.push_back(serviceComponent({
            .partnerUnitId = 1, .partnerHpStepsBefore = 5, .partnerHpStepsAfter = 6}));
        service.input.actors[0].candidates.push_back(alternative);
        service.seat();
        const auto unresolved = run(service.extract(), requestFor(QStringLiteral("unsupported-service-action")));
        expect(unresolved.status == ResultStatus::Unsupported && unresolved.unsupported == 1,
               "service metadata is not execution support");
        Fixture failed(1);
        failed.seat();
        failed.selections[0].role = MasterActorRole::Fixed;
        failed.plan.markFailed(0);
        const auto noPlan = run(failed.extract(), requestFor(QStringLiteral("fixed-inactive-no-complete-plan")));
        expect(noPlan.status == ResultStatus::NoFeasibleAssignment && noPlan.feasible == 0,
               "incomplete fixed action does not count as executable");
        Fixture blocked(3);
        blocked.input.actors[0].candidates[0] = moveCandidate(0, {0, 0}, {1, 0}, 1);
        blocked.input.actors[1].candidates[0] = moveCandidate(1, {1, 0}, {0, 0}, 2);
        blocked.seat();
        blocked.plan.addEdge(1, 2, PlanEdgeKind::Screen);
        const auto cyclic = run(blocked.extract(), requestFor(QStringLiteral("cycle-blocked-dependent")));
        expect(cyclic.status == ResultStatus::NoFeasibleAssignment &&
               cyclic.tuples[0].reference.ordering.blockedByCycle == std::vector<std::int32_t>{2},
               "cyclic actors and their blocked dependent cannot advertise completeness");
        Fixture mixed(3);
        mixed.input.actors[0].candidates[0] = moveCandidate(0, {0, 0}, {-1, 0}, 1);
        mixed.input.actors[1].candidates[0] = moveCandidate(1, {1, 0}, {0, 0}, 2);
        mixed.input.actors[2].candidates.push_back(moveCandidate(2, {2, 0}, {2, 0}, 4, 1));
        mixed.seat();
        mixed.plan.addEdge(2, 1, PlanEdgeKind::Screen);
        const auto preserved = run(mixed.extract(), requestFor(QStringLiteral("mixed-edge-semantic-preservation")));
        expect(preserved.status == ResultStatus::ExactOptimum && preserved.feasible == 2 &&
               preserved.tuples[0].reference.plan.equivalentState(mixed.plan),
               "mixed edge order and unchanged non-Vacate intents remain supported");
    }

    void testLimitsAndOverflow()
    {
        Fixture fixture(2);
        fixture.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {0, 1}, 1, 1));
        fixture.seat();
        const auto problem = fixture.extract();
        auto request = requestFor(QStringLiteral("incomplete-tuple-limit"));
        request.tupleLimit = 1;
        auto result = run(problem, request);
        expect(result.status == ResultStatus::Incomplete && result.evaluated == 1 && result.tupleProduct == 2 &&
               !result.exactOptimum, "tuple limit never claims complete-domain optimum");
        request = requestFor(QStringLiteral("interrupted-before-first-tuple"));
        request.interrupted = [] { return true; };
        result = run(problem, request);
        expect(result.status == ResultStatus::Incomplete && result.evaluated == 0, "interruption is explicit");
        request = requestFor(QStringLiteral("unavailable-exact-continuation"));
        request.continuation.mode = ContinuationMode::Unavailable;
        result = run(problem, request);
        expect(result.status == ResultStatus::NonExactObjective && result.feasible == 2 &&
               result.unresolved == 2 && !result.exactOptimum, "feasibility is not full-objective exactness");
        request = requestFor(QStringLiteral("explicit-reduced-domain"));
        request.domain = {{1}, {0}};
        request.restriction = QStringLiteral("actor 100 restricted to original candidate 1");
        result = run(problem, request);
        expect(result.status == ResultStatus::ExactOptimum && result.reducedDomain &&
               result.fullTupleProduct == 2 && result.tupleProduct == 1, "reduced optimum retains original domain metadata");
        request.restriction.clear();
        expect(enumerate(problem, request).status == ResultStatus::InvalidInput, "unlabeled reduction rejected");
        std::vector<std::vector<std::int32_t>> enormous(65, {0, 1});
        expect(!tupleProduct(enormous), "checked tuple-product overflow");
        enormous[0].clear();
        expect(tupleProduct(enormous) == 0, "empty factor proves empty domain without intermediate overflow");
        Fixture overflow(2);
        overflow.input.actors[0].candidates.push_back(moveCandidate(
            0, {0, 0}, {0, 1}, std::numeric_limits<MilliFunds>::max(), 1));
        overflow.seat();
        result = run(overflow.extract(), requestFor(QStringLiteral("seated-total-overflow")));
        expect(result.status == ResultStatus::ArithmeticFailure && !result.exactOptimum,
               "overflowing challenger cannot be silently omitted");
    }

    void testGeneratedAndOrdering()
    {
        for (std::uint32_t seed = 101; seed < 165; ++seed)
        {
            std::mt19937 random(seed);
            Fixture fixture(3);
            for (std::int32_t actor = 0; actor < 3; ++actor)
            {
                for (std::int32_t choice = 1; choice < 3; ++choice)
                {
                    const auto destination = static_cast<std::int32_t>(random() % 4);
                    const auto value = static_cast<MilliFunds>(random() % 21) - 10;
                    fixture.input.actors[actor].candidates.push_back(
                        moveCandidate(actor, {actor, 0}, {destination, 1}, value, choice * 10));
                }
            }
            fixture.seat();
            const auto first = run(fixture.extract(), requestFor(QStringLiteral("generated-%1").arg(seed), seed));
            expect(first.status == ResultStatus::ExactOptimum && first.evaluated == 27 &&
                   first.feasible + first.infeasible == 27, "generated full domain completely accounted");
            std::reverse(fixture.input.actors.begin(), fixture.input.actors.end());
            std::reverse(fixture.selections.begin(), fixture.selections.end());
            const auto reorderedProblem = fixture.extract();
            const auto sameRequest = requestFor(QStringLiteral("generated-%1").arg(seed), seed);
            const auto second = enumerate(reorderedProblem, sameRequest);
            expect(second.exactOptimum == first.exactOptimum && second.evaluated == first.evaluated &&
                   second.tuples[*second.representative].choices == first.tuples[*first.representative].choices,
                   "equivalent actor input order preserves optimum and representative");
            expect(exportDiagnostic(reorderedProblem, sameRequest, second) ==
                   exportDiagnostic(fixture.extract(), sameRequest, first),
                   "portable export ignores session identity and original actor ordering");
        }
        Fixture many(130);
        many.input.actors[0].candidates.push_back(moveCandidate(0, {0, 0}, {129, 0}, 999, 1));
        many.seat();
        const auto result = run(many.extract(), requestFor(QStringLiteral("resources-130")));
        expect(result.evaluated == 2 && result.feasible == 1 && result.infeasible == 1,
               "more than 64 resources with tractable tuple count");
    }

    SequentialClassInput travelClass(std::span<const PropertyStockColumn> columns, std::int32_t movement)
    {
        SequentialClassInput result;
        result.captureTurns = captureTurnsFor(0, 10, 20);
        for (const auto & from : columns)
        {
            for (const auto & to : columns)
            {
                const auto distance = std::abs(from.tile.x - to.tile.x) + std::abs(from.tile.y - to.tile.y);
                result.interArrivals.push_back(distance == 0 ? UNREACHABLE : (distance + movement - 1) / movement);
            }
        }
        return result;
    }

    SequentialRow stockRow(const SequentialInput & model, TilePoint endpoint, std::int32_t movement,
                           std::int32_t classIndex, std::int32_t carried)
    {
        SequentialRow row;
        row.classIndex = classIndex;
        for (const auto & column : model.columns)
        {
            const auto distance = std::abs(endpoint.x - column.tile.x) + std::abs(endpoint.y - column.tile.y);
            const ArrivalFacts arrival{
                .arrivalActivations = std::max(1, (distance + movement - 1) / movement),
                .carriedPoints = endpoint == column.tile ? carried : 0,
                .ratePerTurn = 10};
            row.weight.push_back(columnWeight(column, arrival, 20, model.horizon));
            row.ownedTurn.push_back(sequentialFirstOwnedTurn(
                ownedTurnsUntil(arrival.arrivalActivations, arrival.carriedPoints, 10, 20, model.horizon),
                model.horizon));
        }
        return row;
    }

    ContinuationCase smallStock(const ContinuationKey & key, std::int32_t carried = 0,
                               TilePoint enemyEndpoint = {7, 2})
    {
        ContinuationCase result;
        result.key = key;
        result.friendly.columns = {
            {.slot = 0, .tile = {-3, 0}, .income = {100, 100}},
            {.slot = 1, .tile = {3, 2}, .income = {100, 100}},
            {.slot = 2, .tile = {3, 4}, .income = {100, 100}}};
        result.friendly.capturedNodes.assign(3, false);
        const std::vector<PropertyStockHolding> holdings{{{50, 50}, OwnerSign::Ours}};
        const std::vector<std::int32_t> movementByRow{3, 3, 2};
        result.owned = ownedBaseline(holdings, result.friendly.horizon);
        for (const auto & action : key)
        {
            const TilePoint endpoint{action.x(), action.y()};
            const auto movement = movementByRow.at(static_cast<std::size_t>(action.row()));
            const auto progress = carried + (action.captures() ? 10 : 0);
            const auto classIndex = static_cast<std::int32_t>(result.friendly.classes.size());
            result.friendly.classes.push_back(travelClass(result.friendly.columns, movement));
            result.friendly.rows.push_back(stockRow(result.friendly, endpoint, movement, classIndex, progress));
            for (std::size_t node = 0; node < result.friendly.columns.size(); ++node)
            {
                if (action.captures() && endpoint == result.friendly.columns[node].tile && progress >= 20)
                {
                    result.capturedColumns.push_back(static_cast<std::int32_t>(node));
                    result.friendly.capturedNodes[node] = true;
                    result.owned += ownershipFlipSwing(result.friendly.columns[node], result.friendly.horizon);
                }
            }
        }
        std::sort(result.capturedColumns.begin(), result.capturedColumns.end());
        for (const auto & column : result.friendly.columns)
        {
            result.enemy.columns.push_back(mirroredColumn(column));
        }
        result.enemy.capturedNodes.assign(3, false);
        result.enemy.classes.push_back(travelClass(result.enemy.columns, 2));
        result.enemy.rows.push_back(stockRow(result.enemy, enemyEndpoint, 2, 0, 0));
        return result;
    }

    void compareSequentialAuthority(const SequentialInput & input, const SequentialExactResult & exact)
    {
        std::vector<SequentialClassTable> classes;
        for (const auto & definition : input.classes)
        {
            SequentialClassTable table;
            table.build(input.columns, definition.interArrivals, definition.captureTurns, input.horizon);
            classes.push_back(std::move(table));
        }
        const SequentialInstance instance{
            .horizonTurns = input.horizon, .nodeColumns = input.columns,
            .classes = classes, .rows = input.rows, .capturedNodes = input.capturedNodes};
        const auto production = solveSequentialPacking(instance, 0, 1000000, nullptr, true);
        expect(exact.status == ObjectiveStatus::Exact && replaySequentialWitness(instance, exact.witness, exact.value),
               "independent exhaustive continuation witness replays");
        expect((production.searchCompleted || production.certificate) && production.bookedValue == exact.value &&
               replaySequentialWitness(instance, production.witness, production.bookedValue),
               "verified completed stock path equals independent exact continuation optimum");
        expect(production.floorValue <= exact.value && production.relaxedValue >= exact.value,
               "existing exercised continuation lower and upper contain exact optimum");
    }

    class IdentityFixture final : public RelativeStockSession
    {
    public:
        IdentityFixture(const PropertyStockField & field, const PropertyStockInstance & enemy)
            : RelativeStockSession(field, enemy)
        {
        }

        RelativeStockQuote bind(const TupleReceipt & receipt)
        {
            require(receipt.objective.status == ObjectiveStatus::Exact, "only exact reference results bind quotes");
            const auto & value = receipt.objective;
            return {.key = receipt.reference.key, .capturedColumns = value.capturedColumns, .identity = identity(),
                    .economic = value.seated, .owned = value.owned,
                    .ours = {value.friendly, value.friendly}, .enemy = {value.enemy, value.enemy},
                    .ourLowerWitnessReplays = value.friendlyProof.status == ObjectiveStatus::Exact,
                    .enemyLowerWitnessReplays = value.enemyProof.status == ObjectiveStatus::Exact,
                    .ourSearchCompleted = true, .enemySearchCompleted = true, .valid = true};
        }

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

    void testStockObjective()
    {
        Fixture fixture(3);
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            const TilePoint origin{0, actor * 2};
            fixture.input.unitLinks[actor].tile = origin;
            fixture.input.actors[actor].candidates = {
                moveCandidate(actor, origin, {-1, origin.y}, 0),
                moveCandidate(actor, origin, {1, origin.y}, 0, 1)};
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto problem = fixture.extract();
        auto request = requestFor(QStringLiteral("shared-property-mixed-mobility"));
        request.continuation.mode = ContinuationMode::Sequential;
        for (int mask = 0; mask < 8; ++mask)
        {
            ContinuationKey key;
            for (int actor = 0; actor < 3; ++actor)
            {
                key.push_back(CanonicalPlanActionKey::fromAction(actor, (mask & (1 << actor)) ? 1 : -1, actor * 2, false));
            }
            request.continuation.cases.push_back(smallStock(key));
        }
        const auto result = run(problem, request);
        expect(result.status == ResultStatus::ExactOptimum && result.feasible == 8 && result.continuationWork > 0,
               "small complete stock domain has an exact full objective");
        expect(result.tuples[*result.representative].choices == std::vector<std::int32_t>({0, 1, 1}),
               "shared property demand favors distributed routing with mixed movement capabilities");
        for (const auto & receipt : result.tuples)
        {
            const auto input = std::find_if(request.continuation.cases.begin(), request.continuation.cases.end(),
                [&](const auto & entry) { return entry.key == receipt.reference.key; });
            require(input != request.continuation.cases.end(), "exact stock fixture key");
            compareSequentialAuthority(input->friendly, receipt.objective.friendlyProof);
            compareSequentialAuthority(input->enemy, receipt.objective.enemyProof);
            expect(receipt.objective.total == receipt.reference.seatedTotal + receipt.objective.owned +
                   receipt.objective.friendly - receipt.objective.enemy,
                   "each objective term is counted once on existing stock accounting basis");
        }
        PropertyStockField field;
        PropertyStockInstance enemy;
        IdentityFixture identity(field, enemy);
        IdentityFixture different(field, enemy);
        const auto first = identity.bind(result.tuples.front());
        const auto best = identity.bind(result.tuples[*result.representative]);
        const auto comparison = identity.compare(first, best);
        expect(comparison.exact() && comparison.commonEnemyCancelled &&
               comparison.deltaLower == result.tuples[*result.representative].objective.total -
                                        result.tuples.front().objective.total,
               "formal same-model common enemy cancellation matches exact absolute difference");
        expect(!compareRelativeStock(first, different.bind(result.tuples.back())).valid(),
               "identical values from different session identities cannot cancel");
        auto truncated = request;
        truncated.fixture = QStringLiteral("incomplete-continuation");
        truncated.continuationLimit = 1;
        const auto incomplete = run(problem, truncated);
        expect(incomplete.status == ResultStatus::NonExactObjective && !incomplete.exactOptimum &&
               incomplete.unresolved == 8, "truncated continuation cannot establish full-objective optimum");
        for (std::size_t index = 0; index < fixture.input.actors.size(); ++index)
        {
            fixture.input.actors[index].engineUnitId += 1000;
            fixture.input.unitLinks[index].engineUnitId += 1000;
        }
        fixture.seat();
        for (std::int32_t actor = 0; actor < 3; ++actor)
        {
            fixture.selections[actor].stockRow = actor;
        }
        const auto renamed = enumerate(fixture.extract(), request);
        expect(renamed.exactOptimum == result.exactOptimum &&
               renamed.tuples[*renamed.representative].choices == result.tuples[*result.representative].choices,
               "mixed capability fixture has no engine unit-ID policy");
    }

    void testCapturesAndEnemyIdentities()
    {
        for (const auto carried : {0, 10})
        {
            Fixture fixture(1);
            fixture.input.unitLinks[0].tile = {-3, 0};
            fixture.input.actors[0].candidates = {moveCandidate(0, {-3, 0}, {-3, 0}, 0)};
            auto capture = fixture.input.actors[0].candidates[0];
            capture.generationIndex = 1;
            capture.bundle.components.push_back(captureComponent({
                .income = {100, 100}, .turnsUntilOwned = carried == 0 ? 1 : 0}));
            fixture.input.actors[0].candidates.push_back(capture);
            fixture.seat();
            fixture.selections[0].stockRow = 0;
            auto request = requestFor(QStringLiteral("capture-progress-%1").arg(carried));
            request.continuation.mode = ContinuationMode::Sequential;
            request.continuation.cases = {
                smallStock({CanonicalPlanActionKey::fromAction(0, -3, 0, false)}, carried),
                smallStock({CanonicalPlanActionKey::fromAction(0, -3, 0, true)}, carried)};
            const auto result = run(fixture.extract(), request);
            expect(result.status == ResultStatus::ExactOptimum && result.feasible == 2,
                   "same-destination WAIT and CAPTURE both evaluated with actual consequences");
            expect(result.tuples[0].reference.key != result.tuples[1].reference.key,
                   "actual capture progress changes endpoint identity without an action bonus");
            const auto & left = result.tuples[0];
            const auto & right = result.tuples[1];
            expect(left.objective.seated == right.objective.seated && left.objective.seated == 0,
                   "capture stock consequence is not double-counted in immediate seating");
            PropertyStockField field;
            PropertyStockInstance enemy;
            IdentityFixture identity(field, enemy);
            const auto comparison = identity.compare(identity.bind(left), identity.bind(right));
            expect(comparison.exact() && comparison.commonEnemyCancelled &&
                   comparison.deltaLower == right.objective.total - left.objective.total,
                   "every captured-column signature shares the start-of-turn enemy value");
            if (carried == 0)
            {
                expect(right.objective.owned == left.objective.owned &&
                       request.continuation.cases[1].friendly.rows[0].ownedTurn[0] == 1,
                       "partial capture advances continuation without an ownership flip");
            }
            else
            {
                expect(right.objective.capturedColumns == std::vector<std::int32_t>{0} &&
                       right.objective.owned - left.objective.owned == 1000 &&
                       request.continuation.cases[0].enemy.rows[0].weight ==
                           request.continuation.cases[1].enemy.rows[0].weight,
                       "completed capture changes ownership and leaves enemy inputs at start-of-turn ownership");
                auto missingWitness = identity.bind(right);
                missingWitness.enemyLowerWitnessReplays = false;
                expect(identity.compare(identity.bind(left), missingWitness).valid(),
                       "cancelled enemy needs no replayed witness under any captured signature");
                auto nearby = request;
                nearby.fixture = QStringLiteral("capture-keeps-enemy-optimum");
                nearby.continuation.cases = {
                    smallStock(left.reference.key, carried, {-1, 0}),
                    smallStock(right.reference.key, carried, {-1, 0})};
                const auto changedEnemy = run(fixture.extract(), nearby);
                expect(changedEnemy.tuples[0].objective.enemy == changedEnemy.tuples[1].objective.enemy &&
                       changedEnemy.tuples[0].objective.enemy > 0,
                       "nearby enemy exact optimum is unchanged by our capture this turn");
                const auto nearbyComparison = identity.compare(identity.bind(changedEnemy.tuples[0]),
                                                               identity.bind(changedEnemy.tuples[1]));
                expect(nearbyComparison.exact() && nearbyComparison.commonEnemyCancelled &&
                       nearbyComparison.deltaLower == changedEnemy.tuples[1].objective.total -
                                                      changedEnemy.tuples[0].objective.total &&
                       nearbyComparison.deltaLower == comparison.deltaLower,
                       "a reachable enemy cannot change what finishing the capture is worth");
            }
            fixture.input.actionIds.wait = QStringLiteral("LABEL_Z");
            fixture.input.actionIds.capture = QStringLiteral("LABEL_A");
            fixture.seat();
            fixture.selections[0].stockRow = 0;
            const auto relabeled = enumerate(fixture.extract(), request);
            expect(relabeled.exactOptimum == result.exactOptimum &&
                   relabeled.tuples[*relabeled.representative].choices == result.tuples[*result.representative].choices,
                   "action labels cannot change objective or winner when semantic facts stay identical");
        }
        Fixture conflict(2);
        for (auto & actor : conflict.input.actors)
        {
            auto capture = moveCandidate(actor.knowledgeUnitIndex, actor.candidates[0].bundle.origin, {3, 3}, 10, 1);
            capture.bundle.components.push_back(captureComponent({.turnsUntilOwned = 1}));
            actor.candidates.push_back(capture);
        }
        conflict.seat();
        const auto result = run(conflict.extract(), requestFor(QStringLiteral("shared-capture-conflict")));
        expect(result.evaluated == 4 && result.feasible == 3 && result.infeasible == 1,
               "two actors cannot claim the same capture and destination");
    }

    void testSequentialFailuresAndGeneratedCases()
    {
        for (std::uint32_t seed = 701; seed < 733; ++seed)
        {
            std::mt19937 random(seed);
            Fixture fixture(2);
            for (std::int32_t actor = 0; actor < 2; ++actor)
            {
                const auto value = -static_cast<MilliFunds>(random() % 50);
                fixture.input.actors[actor].candidates[0].valuation.ledger.enemyCapital = value;
                fixture.input.actors[actor].candidates.push_back(
                    moveCandidate(actor, {actor, 0}, {actor, 1}, value - 1, 1));
            }
            fixture.seat();
            fixture.selections[0].stockRow = 0;
            fixture.selections[1].stockRow = 1;
            const auto problem = fixture.extract();
            auto request = requestFor(QStringLiteral("generated-stock-%1").arg(seed), seed);
            request.continuation.mode = ContinuationMode::Sequential;
            for (int mask = 0; mask < 4; ++mask)
            {
                ContinuationKey key;
                for (int actor = 0; actor < 2; ++actor)
                {
                    key.push_back(CanonicalPlanActionKey::fromAction(actor, actor, (mask >> actor) & 1, false));
                }
                auto stock = smallStock(key);
                for (auto & row : stock.friendly.rows)
                {
                    for (std::size_t node = 0; node < row.weight.size(); ++node)
                    {
                        const auto turn = 1 + static_cast<std::int32_t>(random() % 7);
                        row.ownedTurn[node] = turn;
                        row.weight[node] = columnStreamValue(stock.friendly.columns[node], turn, stock.friendly.horizon);
                    }
                }
                request.continuation.cases.push_back(stock);
            }
            const auto result = run(problem, request);
            expect(result.status == ResultStatus::ExactOptimum && result.evaluated == 4,
                   "seeded exact sequential domain completes");
            for (const auto & receipt : result.tuples)
            {
                const auto stock = std::find_if(request.continuation.cases.begin(), request.continuation.cases.end(),
                    [&](const auto & entry) { return entry.key == receipt.reference.key; });
                require(stock != request.continuation.cases.end(), "generated exact stock fixture key");
                compareSequentialAuthority(stock->friendly, receipt.objective.friendlyProof);
                compareSequentialAuthority(stock->enemy, receipt.objective.enemyProof);
            }
        }
        auto model = smallStock({CanonicalPlanActionKey::fromAction(0, 0, 0, false)}).friendly;
        model.rows[0].weight[0] = std::numeric_limits<MilliFunds>::max();
        expect(exhaustSequential(model, 100).status == ObjectiveStatus::ArithmeticFailure,
               "unsafe sequential arithmetic fails closed before legacy arithmetic executes");
        model.rows[0].weight.clear();
        expect(exhaustSequential(model, 100).status == ObjectiveStatus::InvalidInput, "malformed row dimensions rejected");
        Fixture negative(1);
        negative.input.actors[0].candidates[0].valuation.ledger.enemyCapital = -17;
        negative.seat();
        const auto result = run(negative.extract(), requestFor(QStringLiteral("negative-exact-optimum")));
        expect(result.status == ResultStatus::ExactOptimum && result.exactOptimum == -17,
               "negative feasible optimum is not replaced with an invented zero action");
        SequentialInput signedModel;
        signedModel.horizon = 4;
        signedModel.columns = {
            {.slot = 0, .tile = {0, 0}, .income = {0, 0}},
            {.slot = 1, .tile = {1, 0}, .income = {10, 10}}};
        signedModel.classes = {{1, {UNREACHABLE, 1, UNREACHABLE, UNREACHABLE}}};
        signedModel.rows = {{0, {-1, 0}, {1, NO_CAPTURE_TURNS}}};
        signedModel.capturedNodes = {false, false};
        const auto signedExact = exhaustSequential(signedModel, 1000);
        expect(signedExact.status == ObjectiveStatus::Exact && signedExact.value == 19,
               "negative first-leg gain can enable a positive exact continuation");
        compareSequentialAuthority(signedModel, signedExact);
        signedModel.columns[0].income.oursPerTurn = -1;
        const auto signedIncome = exhaustSequential(signedModel, 1000);
        expect(signedIncome.status == ObjectiveStatus::Exact, "abstract signed property mathematics is not malformed");
        compareSequentialAuthority(signedModel, signedIncome);
        auto signedRequest = requestFor(QStringLiteral("signed-continuation-objective"));
        signedRequest.continuation.mode = ContinuationMode::Sequential;
        ContinuationCase signedCase;
        signedCase.key = negative.extract().incumbentContinuationKey();
        signedCase.friendly = signedModel;
        signedRequest.continuation.cases.push_back(signedCase);
        const auto signedObjective = run(negative.extract(), signedRequest);
        expect(signedObjective.status == ResultStatus::ExactOptimum && signedObjective.exactOptimum == 2,
               "signed seating and continuation aggregate without a fallback or clamp");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    testSingletonFixedAndEquality();
    testThreeActorImprovement();
    testVacatesAndProduction();
    testRevival();
    testDamageAndDependencies();
    testUnsupportedAndPartial();
    testLimitsAndOverflow();
    testGeneratedAndOrdering();
    testStockObjective();
    testCapturesAndEnemyIdentities();
    testSequentialFailuresAndGeneratedCases();
    std::printf("ORACLE cases=%llu tuples=%llu continuationWork=%llu elapsedNs=%llu maximumExportBytes=%llu\n",
                static_cast<unsigned long long>(caseCount), static_cast<unsigned long long>(totalTuples),
                static_cast<unsigned long long>(totalContinuationWork), static_cast<unsigned long long>(totalNanos),
                static_cast<unsigned long long>(maximumExportBytes));
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 4;
}
