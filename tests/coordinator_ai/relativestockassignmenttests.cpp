#include <iostream>
#include <string_view>

#include <QStringList>

#include "tests/coordinator_ai/relativestocktestfixture.h"

namespace
{
    using namespace Coordinator;
    using RelativeStockTests::Session;
    using RelativeStockTests::Valuer;

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

    QString quoteReceipt(const RelativeStockQuote & quote)
    {
        QString result = QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10;")
            .arg(quote.economic).arg(quote.owned).arg(quote.ours.lower).arg(quote.ours.upper)
            .arg(quote.enemy.lower).arg(quote.enemy.upper).arg(quote.ourLowerWitnessReplays)
            .arg(quote.enemyLowerWitnessReplays).arg(quote.ourSearchCompleted).arg(quote.enemySearchCompleted);
        for (const auto & key : quote.key)
        {
            result += QStringLiteral("%1,%2,%3,%4;").arg(key.row()).arg(key.x()).arg(key.y()).arg(key.captures());
        }
        return result + QLatin1Char('|') + traceIndices(quote.capturedColumns);
    }

    class Recorder final : public DecisionTrace, public RelativeStockObserver
    {
    public:
        bool candidateDetailsEnabled() const override { return details; }
        bool stockDetailsEnabled() const override { return details; }
        bool flush() override { return true; }
        void record(const QString & category, const QString & fields) override
        {
            trace.push_back(category + QLatin1Char(' ') + fields);
        }
        void domain(const RelativeStockDomainAudit & audit) override
        {
            domains.push_back(audit);
            receipts.push_back(QStringLiteral("D %1 %2 %3 %4 %5 %6 %7")
                .arg(static_cast<int>(audit.phase)).arg(static_cast<int>(audit.status))
                .arg(traceIndices(audit.actors)).arg(audit.product).arg(audit.states)
                .arg(audit.claimable).arg(audit.contenders));
        }
        void step(RelativeStockPhase phase, const RelativeStockStep & step,
                  const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger) override
        {
            steps.push_back(step);
            receipts.push_back(QStringLiteral("S %1 %2 %3 %4 %5 %6 %7 %8 %9")
                .arg(static_cast<int>(phase)).arg(static_cast<int>(step.target)).arg(step.requestedCap)
                .arg(step.refined.statesUsed).arg(step.refined.workUsed).arg(step.adopted)
                .arg(step.comparison.deltaLower).arg(quoteReceipt(incumbent)).arg(quoteReceipt(challenger)));
        }
        void comparison(RelativeStockPhase phase, std::span<const std::int32_t> tuple,
                        const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
                        const RelativeStockRefinementResult & result, bool accepted) override
        {
            receipts.push_back(QStringLiteral("C %1 %2 %3 %4 %5 %6 %7 %8 %9 %10")
                .arg(static_cast<int>(phase)).arg(traceIndices(tuple)).arg(result.comparison.deltaLower)
                .arg(result.comparison.deltaUpper).arg(result.statesUsed).arg(result.enemyStatesUsed)
                .arg(result.workUsed).arg(accepted).arg(quoteReceipt(incumbent)).arg(quoteReceipt(challenger)));
            if (accepted)
            {
                acceptedProofs.push_back(result.comparison);
                acceptedIncumbents.push_back(incumbent.key);
                acceptedChallengers.push_back(challenger.key);
            }
        }
        void mixedBoundary(std::int64_t allowance, const LiveRefinementResult & result) override
        {
            receipts.push_back(QStringLiteral("M %1 %2 %3 %4 %5 %6 %7")
                .arg(allowance).arg(result.keysRefined).arg(result.statesUsed).arg(result.enemyStatesUsed)
                .arg(result.workUsed).arg(result.budgetExhausted).arg(result.valid));
        }

        bool details{true};
        QStringList trace;
        QStringList receipts;
        std::vector<RelativeStockStep> steps;
        std::vector<RelativeStockDomainAudit> domains;
        std::vector<RelativeStockComparison> acceptedProofs;
        std::vector<ContinuationKey> acceptedIncumbents;
        std::vector<ContinuationKey> acceptedChallengers;
    };

    AssignmentInput makeInput(std::int32_t actors, std::int32_t candidates = 2)
    {
        AssignmentInput input;
        input.actionIds = {QStringLiteral("ACTION_WAIT"), QStringLiteral("ACTION_FIRE"), QStringLiteral("ACTION_CAPTURE")};
        for (std::int32_t slot = 0; slot < actors; ++slot)
        {
            AssignmentActor actor;
            actor.knowledgeUnitIndex = slot;
            actor.engineUnitId = slot + 1;
            const TilePoint origin{0, slot * 2};
            input.unitLinks.push_back({slot + 1, origin});
            for (std::int32_t choice = 0; choice < candidates; ++choice)
            {
                CandidateBundle candidate;
                candidate.bundle.unitId = slot;
                candidate.bundle.origin = origin;
                candidate.bundle.destination = {choice, slot * 2};
                candidate.bundle.path.push_back(origin);
                if (choice != 0)
                {
                    candidate.bundle.path.push_back(candidate.bundle.destination);
                }
                candidate.valuation.ledger.enemyCapital = 10;
                candidate.valuation.valid = true;
                actor.candidates.push_back(std::move(candidate));
            }
            input.actors.push_back(std::move(actor));
        }
        input.relativeStockWorkLimit = 1000;
        return input;
    }

    void configureStock(Session & session, std::int32_t actors, std::int32_t jointSize)
    {
        for (std::int32_t id = 1; id <= actors; ++id)
        {
            session.stockActors.insert(id);
        }
        session.evaluate = [jointSize](const ContinuationKey & key)
        {
            const bool allMoved = key.size() == static_cast<std::size_t>(jointSize) &&
                std::all_of(key.begin(), key.end(), [](const auto & action) { return action.x() == 1; });
            return allMoved ? MilliFunds{101} : MilliFunds{100};
        };
    }

    bool samePlan(const AssignmentResult & left, const AssignmentResult & right)
    {
        if (left.executionOrder != right.executionOrder || left.plan.actionCount() != right.plan.actionCount() ||
            left.plan.edgeCount() != right.plan.edgeCount())
        {
            return false;
        }
        for (std::int32_t index = 0; index < left.plan.actionCount(); ++index)
        {
            if (left.plan.action(index) != right.plan.action(index))
            {
                return false;
            }
            const auto & action = left.plan.action(index);
            if (left.plan.destinationClaimant(action.destination) != right.plan.destinationClaimant(action.destination) ||
                left.plan.captureClaimant(action.destination) != right.plan.captureClaimant(action.destination) ||
                left.plan.serviceClaimant(action.unitId) != right.plan.serviceClaimant(action.unitId) ||
                left.plan.plannedDamageOn(action.targetUnitId) != right.plan.plannedDamageOn(action.targetUnitId) ||
                left.plan.remainingHpStepsOn(action.targetUnitId) != right.plan.remainingHpStepsOn(action.targetUnitId))
            {
                return false;
            }
        }
        for (std::int32_t index = 0; index < left.plan.edgeCount(); ++index)
        {
            const auto & a = left.plan.edge(index);
            const auto & b = right.plan.edge(index);
            if (a.from != b.from || a.to != b.to || a.kind != b.kind)
            {
                return false;
            }
        }
        return true;
    }

    std::int32_t actionOfUnit(const TurnPlan & plan, std::int32_t id)
    {
        for (std::int32_t index = 0; index < plan.actionCount(); ++index)
        {
            if (plan.action(index).unitId == id)
            {
                return index;
            }
        }
        return NO_ACTION;
    }

    bool allAt(const AssignmentResult & result, std::int32_t count, std::int32_t x)
    {
        for (std::int32_t id = 1; id <= count; ++id)
        {
            const auto index = actionOfUnit(result.plan, id);
            if (index == NO_ACTION || result.plan.action(index).destination.x != x)
            {
                return false;
            }
        }
        return true;
    }

    void testPairAndStockMembership()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy);
        configureStock(session, 2, 2);
        Valuer valuer(session);
        Recorder recorder;
        auto input = makeInput(3);
        input.pStockValuer = &valuer;
        input.pRelativeStockObserver = &recorder;
        const auto result = MaximumValueAssignment::assign(input);
        expect(allAt(result, 2, 1) && result.stats.stockPairs.improvements == 1,
               "existing pair stage installs the certified stock-stock replacement");
        const auto nonStock = actionOfUnit(result.plan, 3);
        expect(nonStock != NO_ACTION && result.plan.action(nonStock).destination == TilePoint{0, 4},
               "unrelated non-stock actor cannot move through stock-key deduplication");
        expect(valuer.scalarCalls == 0 && valuer.absoluteRefinements == 0,
               "relative pairs replace the absolute refinement drain without scalar fallback");
        expect(session.enemyCalls == 0 && result.stats.stockPairs.enemyStatesUsed == 0,
               "shared enemy is never refined by the stock pair stage");
        expect(result.stats.mixedPairs.calls == 0 && result.stats.mixedPairs.domains == 0 &&
                   result.stats.mixedPairs.workUsed == 0,
               "PURE_STOCK_RELATIVE_UNCHANGED uses no mixed boundary work");
        expect(result.stats.replayFailures == 0 && result.executionOrder.size() == 3,
               "pair replacement fully replays and remains orderable");
        expect(std::all_of(recorder.acceptedProofs.begin(), recorder.acceptedProofs.end(),
                           [](const auto & proof) { return proof.improves() && proof.deltaLower > 0; }),
               "every accepted pair carries a strictly positive certificate");
        expect(std::all_of(recorder.domains.begin(), recorder.domains.end(), [](const auto & domain)
        {
            return domain.phase != RelativeStockPhase::Pair ||
                (domain.actors.size() == 2 && domain.actors[0] <= 2 && domain.actors[1] <= 2);
        }), "relative pair membership explicitly excludes non-stock actors");
    }

    void testJointAndRepeat()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        AssignmentResult first;
        QStringList firstTrace;
        QStringList firstReceipts;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            Session session(field, enemy);
            configureStock(session, 3, 3);
            Valuer valuer(session);
            Recorder recorder;
            auto input = makeInput(3);
            input.pStockValuer = &valuer;
            input.pRelativeStockObserver = &recorder;
            input.pTrace = &recorder;
            auto result = MaximumValueAssignment::assign(input);
            expect(result.stats.stockPairs.improvements == 0 && result.stats.stockJoints.improvements == 1,
                   "one joint improvement occurs only at the existing stock cluster stage");
            expect(allAt(result, 3, 1), "the certified three-member plan is installed");
            expect(result.stats.stockJoints.candidateProduct == 27 &&
                       result.stats.stockJoints.enumerationStates == 39 &&
                       result.stats.stockJoints.claimableLeaves == 8,
                   "complete candidate product includes the no-action sentinel and rejects incomplete leaves");
            expect(result.stats.replayFailures == 0 && result.stats.assignedUnits == 3 &&
                       result.stats.stockJoints.improvements <= 1,
                   "at most one fully replayed joint replacement is installed");
            expect(!recorder.acceptedProofs.empty() && recorder.acceptedProofs.back().deltaLower == 1,
                   "joint proof compares against the actual post-pair incumbent");
            expect(result.stats.stockPairs.workUsed + result.stats.stockJoints.workUsed ==
                       session.ledger.stats().spent() && session.ledger.stats().spent() <= input.relativeStockWorkLimit,
                   "pair and joint work share the existing bounded ledger");
            expect(session.enemyCalls == 0 && valuer.scalarCalls == 0 && valuer.absoluteRefinements == 0,
                   "joint refinement adds no enemy solve, scalar fallback or duplicate optimizer");
            if (repeat == 0)
            {
                first = result;
                firstTrace = recorder.trace;
                firstReceipts = recorder.receipts;
            }
            expect(samePlan(first, result) && first.stats == result.stats,
                   "deterministic repeat has identical complete plan, order and all stats");
            expect(firstTrace == recorder.trace && firstReceipts == recorder.receipts,
                   "deterministic repeat has identical trace and non-timing refinement schedule");
        }
    }

    void testRollback()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const std::int32_t count : {2, 3})
        {
            Session baselineSession(field, enemy);
            configureStock(baselineSession, count, count);
            Valuer baselineValuer(baselineSession);
            baselineValuer.relativeEnabled = false;
            auto baselineInput = makeInput(count);
            baselineInput.pStockValuer = &baselineValuer;
            const auto baseline = MaximumValueAssignment::assign(baselineInput);

            Session session(field, enemy);
            configureStock(session, count, count);
            session.failReplay = true;
            Valuer valuer(session);
            auto input = makeInput(count);
            input.pStockValuer = &valuer;
            const auto result = MaximumValueAssignment::assign(input);
            expect(result.stats.replayFailures == 1, "forced pair or joint witness replay failure is recorded");
            expect(samePlan(baseline, result) && allAt(result, count, 0),
                   "TRANSACTIONAL_INSTALLATION_ROLLBACK");
            expect(result.stats.stockPairs.improvements == 0 && result.stats.stockJoints.improvements == 0,
                   "failed replay never reports an installed improvement");
        }
    }

    void testBoundedSkips()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session invalidSession(field, enemy);
        configureStock(invalidSession, 2, 2);
        invalidSession.invalidateOnRefinement = true;
        Valuer invalidValuer(invalidSession);
        auto invalidInput = makeInput(2);
        invalidInput.pStockValuer = &invalidValuer;
        const auto invalid = MaximumValueAssignment::assign(invalidInput);
        expect(allAt(invalid, 2, 0) && invalid.stats.stockPairs.improvements == 0 &&
                   invalid.stats.stockPairs.unresolved > 0,
               "invalid refinement leaves the incumbent intact and contenders unresolved");

        Session productSession(field, enemy);
        configureStock(productSession, 4, 100);
        Valuer productValuer(productSession);
        Recorder recorder;
        auto productInput = makeInput(4, 15);
        productInput.pStockValuer = &productValuer;
        productInput.pRelativeStockObserver = &recorder;
        productInput.relativeStockWorkLimit = 1;
        const auto product = MaximumValueAssignment::assign(productInput);
        expect(product.stats.stockJoints.enumerationStates == 0 && product.stats.stockJoints.claimableLeaves == 0 &&
                   product.stats.stockJoints.skippedDomains == 1,
               "over-product joint component performs zero partial enumeration");
        expect(std::any_of(recorder.domains.begin(), recorder.domains.end(), [](const auto & domain)
        {
            return domain.phase == RelativeStockPhase::Joint &&
                domain.status == RelativeStockDomainStatus::ProductLimit && domain.states == 0;
        }), "over-product skip has an explicit zero-state receipt");

        Session largeSession(field, enemy);
        configureStock(largeSession, 5, 100);
        Valuer largeValuer(largeSession);
        auto largeInput = makeInput(5);
        largeInput.pStockValuer = &largeValuer;
        const auto large = MaximumValueAssignment::assign(largeInput);
        expect(large.stats.stockJoints.enumerationStates == 0 && large.stats.stockJoints.skippedDomains == 1 &&
                   large.stats.clustersSkippedStockBudget == 1,
               "more than four stock actors preserve the whole-component skip");

        Session exhaustedSession(field, enemy);
        configureStock(exhaustedSession, 3, 3);
        Valuer exhaustedValuer(exhaustedSession);
        auto exhaustedInput = makeInput(3);
        exhaustedInput.pStockValuer = &exhaustedValuer;
        exhaustedInput.relativeStockWorkLimit = 0;
        const auto exhausted = MaximumValueAssignment::assign(exhaustedInput);
        expect(allAt(exhausted, 3, 0) && exhausted.stats.stockJoints.enumerationStates == 0 &&
                   exhausted.stats.stockJoints.budgetExhaustions == 1 && exhaustedSession.ourCalls == 0,
               "exhausted work allowance cannot install an unproven pair or start a joint search");
    }

    void testNonStockExact()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy);
        Valuer valuer(session);
        auto input = makeInput(2, 3);
        for (auto & actor : input.actors)
        {
            actor.candidates[1].bundle.destination = {5, 5};
            actor.candidates[1].bundle.path.back() = {5, 5};
            actor.candidates[1].valuation.ledger.enemyCapital = 30;
            actor.candidates[2].valuation.ledger.enemyCapital = 20;
        }
        const auto baseline = MaximumValueAssignment::assign(input);
        input.pStockValuer = &valuer;
        const auto relativeAvailable = MaximumValueAssignment::assign(input);
        expect(baseline.stats.swapStates > 0 && samePlan(baseline, relativeAvailable) &&
                   baseline.stats == relativeAvailable.stats,
               "active exact non-stock pair and cluster behavior is unchanged");
        expect(session.ourCalls == 0 && session.enemyCalls == 0 &&
                   relativeAvailable.stats.stockPairs.domains == 0,
               "non-stock exact work never enters relative stock enumeration or refinement");
    }

    void addChoiceActor(AssignmentInput & input, std::int32_t id, TilePoint origin,
                        std::initializer_list<std::pair<TilePoint, MilliFunds>> choices)
    {
        AssignmentActor actor;
        actor.engineUnitId = id;
        actor.knowledgeUnitIndex = static_cast<std::int32_t>(input.actors.size());
        for (const auto & [destination, value] : choices)
        {
            CandidateBundle candidate;
            candidate.bundle.unitId = actor.knowledgeUnitIndex;
            candidate.bundle.origin = origin;
            candidate.bundle.destination = destination;
            candidate.bundle.path.push_back(origin);
            if (origin != destination)
            {
                candidate.bundle.path.push_back(destination);
            }
            candidate.valuation.ledger.enemyCapital = value;
            candidate.valuation.valid = true;
            actor.candidates.push_back(std::move(candidate));
        }
        input.unitLinks.push_back({id, origin});
        input.actors.push_back(std::move(actor));
    }

    ContinuationKey replacementKey(const AssignmentInput & input, std::size_t firstChoice, std::size_t secondChoice,
                                   std::size_t firstSlot = 0, std::size_t secondSlot = 1)
    {
        const auto & first = input.actors[firstSlot];
        const auto & second = input.actors[secondSlot];
        const auto a = first.candidates[firstChoice].bundle.destination;
        const auto b = second.candidates[secondChoice].bundle.destination;
        return {CanonicalPlanActionKey::fromAction(first.engineUnitId, a.x, a.y, false),
                CanonicalPlanActionKey::fromAction(second.engineUnitId, b.x, b.y, false)};
    }

    ContinuationKey selectionKey(
        const AssignmentInput & input,
        std::span<const std::size_t> choices)
    {
        ContinuationKey result;
        result.reserve(choices.size());
        for (std::size_t slot = 0;
             slot < choices.size();
             ++slot)
        {
            const auto & actor = input.actors[slot];
            const auto & bundle =
                actor.candidates[choices[slot]].bundle;
            const PlanBundleKind kind =
                planBundleKindOf(bundle);
            result.push_back(
                CanonicalPlanActionKey::fromAction(
                    actor.engineUnitId,
                    bundle.destination.x,
                    bundle.destination.y,
                    kind == PlanBundleKind::Capture ||
                        kind ==
                            PlanBundleKind::
                                MoveAndCapture));
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    void configureReplacementChain(Session & session, const AssignmentInput & input,
                                   std::size_t firstSlot = 0, std::size_t secondSlot = 1)
    {
        session.stockActors = {input.actors[firstSlot].engineUnitId, input.actors[secondSlot].engineUnitId};
        session.enemies.at({}) = {{0, 0}, 0, 1, true};
        for (std::size_t first = 0; first < input.actors[firstSlot].candidates.size(); ++first)
        {
            for (std::size_t second = 0; second < input.actors[secondSlot].candidates.size(); ++second)
            {
                session.plans[replacementKey(input, first, second, firstSlot, secondSlot)].ours = {{0, 90}, 80, 1};
            }
        }
        session.plans[replacementKey(input, 0, 0, firstSlot, secondSlot)].ours = {{100, 100}, 100, 1, true};
        session.plans[replacementKey(input, 1, 1, firstSlot, secondSlot)].ours = {{110, 110}, 110, 1, true};
        session.plans[replacementKey(input, 2, 2, firstSlot, secondSlot)].ours = {{90, 200}, 140, 1};
    }

    AssignmentInput reversalInput(Session & session)
    {
        auto input = makeInput(0);
        addChoiceActor(input, 1, {0, 0}, {{{0, 0}, 10}, {{0, 1}, 10}, {{1, 0}, 10}});
        addChoiceActor(input, 2, {1, 0}, {{{1, 0}, 10}, {{0, 0}, 10}, {{1, 1}, 10}});
        configureReplacementChain(session, input);
        return input;
    }

    bool sameSelection(const AssignmentResult::Selection & left, const AssignmentResult::Selection & right)
    {
        return left.knowledgeUnitIndex == right.knowledgeUnitIndex && left.engineUnitId == right.engineUnitId &&
            left.actionIndex == right.actionIndex && left.candidateIndex == right.candidateIndex &&
            left.greedyCandidateIndex == right.greedyCandidateIndex && left.seatedValue == right.seatedValue &&
            left.stockCoupled == right.stockCoupled && left.phase == right.phase &&
            left.completeValues.size() == right.completeValues.size() &&
            std::equal(left.completeValues.begin(), left.completeValues.end(), right.completeValues.begin(),
                [](const auto & a, const auto & b) { return a.value == b.value && a.known == b.known; });
    }

    void testSuccessiveDependencyReversal()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy);
        Valuer valuer(session);
        auto input = reversalInput(session);
        input.pStockValuer = &valuer;
        Recorder recorder;
        input.pRelativeStockObserver = &recorder;
        const auto result = MaximumValueAssignment::assign(input);
        const auto first = actionOfUnit(result.plan, 1);
        const auto second = actionOfUnit(result.plan, 2);
        expect(result.stats.stockPairs.improvements == 2, "SUCCESSIVE_DEPENDENCY_REVERSAL accepts both certified replacements");
        expect(result.plan.action(first).destination == TilePoint{1, 0} &&
                   result.plan.action(second).destination == TilePoint{1, 1},
               "the second replacement takes the reversed-dependency destinations");
        expect(result.plan.edgeCount() == 1 && result.plan.edge(0).kind == PlanEdgeKind::Vacate &&
                   result.plan.edge(0).from == second && result.plan.edge(0).to == first,
               "only the current B-before-A Vacate edge reaches the final plan");
        const auto order = result.plan.executionOrder();
        expect(result.executionOrder == std::vector<std::int32_t>{second, first} &&
                   order.cycleMembers.empty() && order.blockedByCycle.empty() && result.stats.replayFailures == 0,
               "successive replacements replay without a false cycle or blocked-by-cycle action");
        expect(session.quotedVacateEdges == 0, "relative pricing never sees validation-copy Vacate edges");
        expect(
            recorder.acceptedIncumbents.size() == 2 &&
                recorder.acceptedChallengers.size() == 2 &&
                recorder.acceptedChallengers[0] ==
                    recorder.acceptedIncumbents[1] &&
                recorder.acceptedIncumbents[0] !=
                    recorder.acceptedIncumbents[1],
            "SUCCESSIVE_REPLACEMENTS_REQUIRE_FRESH_CERTIFICATES");
    }

    AssignmentInput nonMemberInput(Session & session)
    {
        auto input = makeInput(0);
        addChoiceActor(input, 1, {0, 0}, {{{0, 1}, 10}, {{0, 2}, 10}, {{0, 3}, 10}});
        addChoiceActor(input, 2, {1, 0}, {{{0, 0}, 10}});
        input.actors[1].candidates[0].bundle.components.push_back(captureComponent(CaptureFacts{}));
        addChoiceActor(input, 3, {2, 0}, {{{2, 0}, 10}, {{2, 1}, 10}, {{2, 2}, 10}});
        configureReplacementChain(session, input, 0, 2);
        return input;
    }

    void testNonMemberDependentSurvivesTrial()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session baselineSession(field, enemy);
        Valuer baselineValuer(baselineSession);
        baselineValuer.relativeEnabled = false;
        auto baselineInput = nonMemberInput(baselineSession);
        baselineInput.pStockValuer = &baselineValuer;
        Recorder baselineTrace;
        baselineTrace.details = false;
        baselineInput.pTrace = &baselineTrace;
        const auto baseline = MaximumValueAssignment::assign(baselineInput);

        Session session(field, enemy);
        Valuer valuer(session);
        auto input = nonMemberInput(session);
        input.pStockValuer = &valuer;
        Recorder trace;
        trace.details = false;
        input.pTrace = &trace;
        const auto result = MaximumValueAssignment::assign(input);
        const auto nonMember = actionOfUnit(result.plan, 2);
        const auto oldNonMember = actionOfUnit(baseline.plan, 2);
        expect(result.stats.stockPairs.improvements == 2, "NON_MEMBER_DEPENDENT_SURVIVES_TRIAL accepts the later replacement");
        expect(result.plan.action(nonMember) == baseline.plan.action(oldNonMember) &&
                   result.plan.destinationClaimant({0, 0}) == baseline.plan.destinationClaimant({0, 0}) &&
                   result.plan.captureClaimant({0, 0}) == baseline.plan.captureClaimant({0, 0}),
               "a non-member's complete action, state, destination and capture claims remain equal");
        expect(result.selections.size() == 3 && baseline.selections.size() == 3 &&
                   sameSelection(result.selections[1], baseline.selections[1]),
               "non-member choice, seated value, phase and complete-value cache remain equal");
        expect(result.stats.unorderedActions == 0 && result.stats.vacateConflicts == 0 &&
                   result.plan.edgeCount() == 1 && result.plan.edge(0).to == nonMember,
               "fresh validation derives the real current dependency without a stale cascade");
    }

    void testRebuiltOrderRollback()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session baselineSession(field, enemy);
        Valuer baselineValuer(baselineSession);
        auto baselineInput = reversalInput(baselineSession);
        baselineSession.plans[replacementKey(baselineInput, 2, 2)].ours = {{0, 90}, 80, 1};
        baselineInput.pStockValuer = &baselineValuer;
        Recorder baselineTrace;
        baselineTrace.details = false;
        baselineInput.pTrace = &baselineTrace;
        const auto baseline = MaximumValueAssignment::assign(baselineInput);

        Session session(field, enemy);
        Valuer valuer(session);
        auto input = reversalInput(session);
        session.failReplayKey = replacementKey(input, 2, 2);
        input.pStockValuer = &valuer;
        Recorder trace;
        trace.details = false;
        input.pTrace = &trace;
        const auto result = MaximumValueAssignment::assign(input);
        expect(result.stats.replayFailures == 1 && result.stats.stockPairs.improvements == 1,
               "ROLLBACK forces replay failure only after the earlier dependency-bearing replacement");
        expect(samePlan(result, baseline), "failed rebuilt-order replay restores all actions, edges, claims and final order");
        expect(result.selections.size() == baseline.selections.size() &&
                   std::equal(result.selections.begin(), result.selections.end(), baseline.selections.begin(), sameSelection),
               "failed rebuilt-order replay restores choices, indexes, values, phases and complete-value caches");
    }

    void testLaterNonRelativeStage()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy);
        Valuer valuer(session);
        auto input = reversalInput(session);
        addChoiceActor(input, 3, {8, 10}, {{{10, 10}, 100}, {{11, 10}, 99}});
        addChoiceActor(input, 4, {8, 11}, {{{11, 10}, 90}, {{12, 10}, 89}});
        addChoiceActor(input, 5, {8, 12}, {{{12, 10}, 80}, {{10, 10}, 83}});
        input.pStockValuer = &valuer;
        const auto result = MaximumValueAssignment::assign(input);
        expect(result.stats.stockPairs.improvements == 2 && result.stats.clustersEnumerated == 1,
               "LATER_NON_RELATIVE_STAGE runs an eligible exact cluster after relative pair installations");
        expect(result.plan.action(actionOfUnit(result.plan, 3)).destination == TilePoint{11, 10} &&
                   result.plan.action(actionOfUnit(result.plan, 4)).destination == TilePoint{12, 10} &&
                   result.plan.action(actionOfUnit(result.plan, 5)).destination == TilePoint{10, 10},
               "the non-stock exact cluster installs its three-actor improvement");
        expect(session.quotedVacateEdges == 0 && result.stats.assignedUnits == 5 &&
                   result.stats.unorderedActions == 0 && result.stats.replayFailures == 0,
               "temporary relative ordering edges never enter later mutable assignment stages");
    }

    AssignmentInput mixedInput(Session & session, bool withStockPair = false)
    {
        auto input = makeInput(0);
        addChoiceActor(input, 1, {0, 0}, {{{1, 0}, 10000}, {{2, 0}, 9800}});
        addChoiceActor(input, 2, {0, 2}, {{{1, 0}, 8500}, {{0, 2}, 8100}});
        session.stockActors = {1};
        if (withStockPair)
        {
            addChoiceActor(input, 3, {0, 4}, {{{0, 4}, 10}, {{1, 4}, 10}});
            session.stockActors.insert(3);
            session.evaluate = [](const ContinuationKey & key)
            {
                return std::any_of(key.begin(), key.end(),
                    [](const auto & action) { return action.row() == 3 && action.x() == 1; }) ? 101 : 100;
            };
            for (std::size_t choice = 0; choice < input.actors[0].candidates.size(); ++choice)
            {
                session.plans[replacementKey(input, choice, 1, 0, 2)].ours = {{0, 201}, 101, 2};
            }
        }
        return input;
    }

    void testConflictingMixedPairRefinesWithRelativeSession()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy);
        Valuer valuer(session);
        valuer.boundaryEnabled = true;
        auto input = mixedInput(session);
        input.pStockValuer = &valuer;
        const auto result = MaximumValueAssignment::assign(input);
        expect(result.stats.mixedPairs.calls > 0 && result.stats.mixedPairs.keysRefined > 0 &&
                   result.stats.stockPairs.domains == 0,
               "CONFLICTING_MIXED_PAIR_REFINES_WITH_RELATIVE_SESSION uses only the bounded legacy domain");
        expect(result.plan.action(actionOfUnit(result.plan, 1)).destination == TilePoint{2, 0} &&
                   result.plan.action(actionOfUnit(result.plan, 2)).destination == TilePoint{1, 0},
               "a later sweep installs the now-certified conflicting mixed result");
        expect(valuer.scalarCalls == 0 && result.stats.replayFailures == 0 && result.stats.mixedPairs.failures == 0 &&
                   !valuer.globalClosed && !valuer.pricingFailure,
               "mixed refinement succeeds without scalar fallback, replay failure or global closure");
        expect(result.stats.mixedPairs.workUsed == session.ledger.stats().spent() &&
                   result.stats.mixedPairs.workUsed > 0 && result.stats.mixedPairs.workUsed <= input.relativeStockWorkLimit,
               "all mixed work is reported as a single bounded ledger delta");
    }

    void testMixedAndStockPairShareBudget()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        AssignmentResult first;
        QStringList receipts;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            Session session(field, enemy);
            Valuer valuer(session);
            valuer.boundaryEnabled = true;
            auto input = mixedInput(session, true);
            input.pStockValuer = &valuer;
            Recorder recorder;
            input.pRelativeStockObserver = &recorder;
            const auto result = MaximumValueAssignment::assign(input);
            std::cout << "MIXED_SHARED_CONTROL repeat=" << repeat
                      << " mixedWork=" << result.stats.mixedPairs.workUsed
                      << " relativeWork=" << result.stats.stockPairs.workUsed
                      << " jointWork=" << result.stats.stockJoints.workUsed
                      << " ledgerSpent=" << session.ledger.stats().spent()
                      << " mixedStates=" << result.stats.mixedPairs.statesUsed
                      << " relativeStates=" << result.stats.stockPairs.statesUsed
                      << " enemyStates=" << result.stats.mixedPairs.enemyStatesUsed + result.stats.stockPairs.enemyStatesUsed
                      << " improvements=" << result.stats.stockPairs.improvements << '\n';
            expect(result.stats.mixedPairs.workUsed > 0 && result.stats.stockPairs.workUsed > 0 &&
                       result.stats.stockPairs.improvements > 0,
                   "MIXED_AND_STOCK_PAIR_SHARE_BUDGET gives both schedulers real work");
            expect(!recorder.steps.empty() && !valuer.boundaryResults.empty() &&
                       recorder.steps.front().requestedCap ==
                           input.relativeStockWorkLimit - valuer.boundaryResults.front().workUsed - session.ourTariff,
                   "relative work sees the exact remaining pair allowance after mixed refunds");
            expect(result.stats.mixedPairs.workUsed + result.stats.stockPairs.workUsed + result.stats.stockJoints.workUsed ==
                       session.ledger.stats().spent() && session.ledger.stats().spent() <= input.relativeStockWorkLimit,
                   "relative, mixed and joint work equal one bounded assignment ledger delta");
            if (repeat == 0)
            {
                first = result;
                receipts = recorder.receipts;
            }
            expect(samePlan(first, result) && first.stats == result.stats && receipts == recorder.receipts,
                   "mixed/relative scheduling repeats with identical results, work split and non-timing receipts");
        }
    }

    void testMixedLocalExhaustion()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const std::int64_t allowance : {0, 9})
        {
            Session session(field, enemy);
            Valuer valuer(session);
            valuer.boundaryEnabled = true;
            auto input = mixedInput(session);
            input.relativeStockWorkLimit = allowance;
            input.pStockValuer = &valuer;
            const auto result = MaximumValueAssignment::assign(input);
            expect(session.ourCalls == 0 && session.enemyCalls == 0 &&
                       session.ledger.stats().granted == 0 && session.ledger.stats().refusals == 0,
                   "ZERO_LOCAL_ALLOWANCE and NEXT_REQUEST_DOES_NOT_FIT perform no search, draw or partial request");
            expect(result.stats.mixedPairs.budgetExhaustions > 0 && result.stats.mixedPairs.failures == 0 &&
                       result.stats.mixedPairs.workUsed == 0 && !valuer.globalClosed && !valuer.pricingFailure,
                   "local exhaustion is reported without closing or failing the global session");
            expect(result.plan.action(actionOfUnit(result.plan, 1)).destination == TilePoint{1, 0} &&
                       result.plan.action(actionOfUnit(result.plan, 2)).destination == TilePoint{0, 2},
                   "an overlapping mixed incumbent survives insufficient local work");
            const auto resumed = valuer.refineLiveAtBoundary(AssignPhase::BetweenSwapSweeps, 1000);
            expect(resumed.valid && resumed.refined && resumed.workUsed > 0,
                   "the same global session can refine after a prior local refusal");
        }
        Session shared(field, enemy);
        Valuer sharedValuer(shared);
        sharedValuer.boundaryEnabled = true;
        auto sharedInput = mixedInput(shared, true);
        sharedInput.pStockValuer = &sharedValuer;
        sharedInput.relativeStockWorkLimit = 0;
        const auto empty = MaximumValueAssignment::assign(sharedInput);
        expect(empty.stats.mixedPairs.calls > 0 && empty.stats.stockPairs.domains > 0 &&
                   empty.stats.mixedPairs.workUsed == 0 && empty.stats.stockPairs.workUsed == 0 &&
                   shared.ourCalls == 0 && shared.enemyCalls == 0 && shared.ledger.stats().spent() == 0,
               "ZERO_LOCAL_ALLOWANCE prevents search in both eligible mixed and relative domains");
        expect(empty.stats.mixedPairs.failures == 0 && !sharedValuer.globalClosed && !sharedValuer.pricingFailure &&
                   empty.plan.action(actionOfUnit(empty.plan, 1)).destination == TilePoint{1, 0} &&
                   empty.plan.action(actionOfUnit(empty.plan, 2)).destination == TilePoint{0, 2} &&
                   empty.plan.action(actionOfUnit(empty.plan, 3)).destination == TilePoint{0, 4},
               "zero shared allowance retains the whole incumbent without a global pricing failure");
    }

    void testMixedPairPrefixCertificatesShareBudget()
    {
        constexpr std::int64_t SHARED_ALLOWANCE = 50000;
        constexpr std::int64_t FULL_SEARCH_STATES = 2 * SHARED_ALLOWANCE;
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const bool mixed : {false, true})
        {
            AssignmentResult first;
            QStringList receipts;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                Session session(field, enemy, SHARED_ALLOWANCE);
                auto input = reversalInput(session);
                session.enemies.at({}) = {{0, 1000}, 100, 1};
                session.plans[replacementKey(input, 1, 1)].ours =
                    {{0, 300}, 120, FULL_SEARCH_STATES, false, true,
                     RelativeStockTests::SearchPrefix{LIVE_PAIR_SLICE_BASE, {110, 250}}};
                session.plans[replacementKey(input, 2, 2)].ours =
                    {{0, 290}, 270, FULL_SEARCH_STATES, false, true,
                     RelativeStockTests::SearchPrefix{LIVE_PAIR_SLICE_BASE, {260, 280}}};
                if (mixed)
                {
                    addChoiceActor(input, 3, {9, 10}, {{{10, 10}, 10}, {{0, 0}, 10}});
                }
                Valuer valuer(session);
                valuer.boundaryEnabled = mixed;
                Recorder recorder;
                input.pStockValuer = &valuer;
                input.pRelativeStockObserver = &recorder;
                input.relativeStockWorkLimit = SHARED_ALLOWANCE;
                const auto result = MaximumValueAssignment::assign(input);
                std::cout << "PREFIX_SHARED_CONTROL mixed=" << mixed << " repeat=" << repeat
                          << " improvements=" << result.stats.stockPairs.improvements
                          << " mixedWork=" << result.stats.mixedPairs.workUsed
                          << " relativeWork=" << result.stats.stockPairs.workUsed
                          << " ledgerSpent=" << session.ledger.stats().spent() << '\n';
                expect(result.stats.stockPairs.improvements == (mixed ? 2 : 1),
                       "mixed contention checks prefix certificates early; pure-stock Direct remains unchanged");
                expect(!recorder.steps.empty() && recorder.steps.front().requestedCap ==
                           (mixed ? LIVE_PAIR_SLICE_BASE : SHARED_ALLOWANCE - session.ourTariff),
                       "only an actual mixed live domain selects the existing incremental relative schedule");
                expect((result.stats.mixedPairs.workUsed > 0) == mixed &&
                           result.stats.stockPairs.workUsed + result.stats.mixedPairs.workUsed ==
                               session.ledger.stats().spent() &&
                           session.ledger.stats().spent() <= SHARED_ALLOWANCE,
                       "mixed and prefix work share one allowance with every tariff accounted");
                expect(result.stats.replayFailures == 0 && valuer.scalarCalls == 0 &&
                           std::all_of(recorder.acceptedProofs.begin(), recorder.acceptedProofs.end(),
                               [](const auto & proof) { return proof.improves() && proof.deltaLower > 0; }),
                       "partial searches install only positive witnessed certificates with successful replay");
                if (mixed)
                {
                    const auto key = replacementKey(input, 2, 2);
                    const auto quote = session.quote(key, 0);
                    expect(result.plan.action(actionOfUnit(result.plan, 1)).destination == TilePoint{1, 0} &&
                               result.plan.action(actionOfUnit(result.plan, 2)).destination == TilePoint{1, 1} &&
                               quote.ours.lower == 260 && quote.ours.upper == 280 &&
                               !quote.ourSearchCompleted && quote.ourLowerWitnessReplays,
                           "the second replacement retains genuine interval uncertainty and its lower witness");
                }
                if (repeat == 0)
                {
                    first = result;
                    receipts = recorder.receipts;
                }
                expect(samePlan(first, result) && first.stats == result.stats && receipts == recorder.receipts,
                       "prefix scheduling repeats with identical plans, work and non-timing receipts");
            }
        }
    }

    void testSevenStockActorsWithExpensiveDecoy()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        for (const auto schedule : {RelativeStockSchedule::Current, RelativeStockSchedule::CertifiedFirst,
                                   RelativeStockSchedule::Requeue, RelativeStockSchedule::FairDomains})
        {
            AssignmentResult first;
            QStringList receipts;
            for (int repeat = 0; repeat < 3; ++repeat)
            {
                Session session(field, enemy);
                configureStock(session, 7, 7);
                for (int mask = 0; mask < 128; ++mask)
                {
                    ContinuationKey key;
                    for (int row = 0; row < 7; ++row)
                    {
                        key.push_back(CanonicalPlanActionKey::fromAction(
                            row + 1, (mask >> row) & 1, row * 2, false));
                    }
                    auto & term = session.plans[key].ours;
                    term = {{100, 100}, 100, 1, true};
                    if (mask == 3)
                    {
                        term = {{0, 1000}, 100, 1000000};
                        term.prefix = RelativeStockTests::SearchPrefix{LIVE_PAIR_SLICE_BASE, {0, 800}};
                    }
                    if (mask == 48)
                    {
                        term = {{100, 900}, 105, 1};
                    }
                }
                Valuer valuer(session);
                valuer.boundaryEnabled = true;
                auto input = makeInput(8);
                auto & mixed = input.actors.back().candidates.back().bundle;
                mixed.destination = {0, 12};
                mixed.path.back() = mixed.destination;
                input.pStockValuer = &valuer;
                input.relativeStockWorkLimit = DEFAULT_RELATIVE_STOCK_WORK_LIMIT;
                input.relativeStockSchedule = schedule;
                Recorder recorder;
                input.pRelativeStockObserver = &recorder;
                const auto result = MaximumValueAssignment::assign(input);
                const auto cheap = actionOfUnit(result.plan, 5);
                const bool reached = result.plan.action(cheap).destination.x == 1;
                const bool bounded = schedule == RelativeStockSchedule::Requeue ||
                    schedule == RelativeStockSchedule::FairDomains;
                expect(reached == bounded && result.stats.stockPairs.improvements == (bounded ? 1 : 0),
                    "bounded scheduling reaches the lower-ranked cheap pair while exclusive scheduling starves it");
                expect(result.stats.replayFailures == 0 && valuer.scalarCalls == 0 &&
                    std::all_of(recorder.acceptedProofs.begin(), recorder.acceptedProofs.end(),
                        [](const auto & proof) { return proof.deltaLower > 0; }),
                    "the decoy regression never installs an unproven or scalar replacement");
                const auto spent = result.stats.mixedPairs.workUsed + result.stats.stockPairs.workUsed +
                    result.stats.stockJoints.workUsed;
                expect(spent == session.ledger.stats().spent() && spent <= DEFAULT_RELATIVE_STOCK_WORK_LIMIT &&
                    result.stats.mixedPairs.domains > 0,
                    "seven stock rows and a non-stock conflict retain one shared 160000 allocation");
                if (repeat == 0)
                {
                    first = result;
                    receipts = recorder.receipts;
                }
                expect(samePlan(first, result) && first.stats == result.stats && receipts == recorder.receipts,
                    "decoy plans, schedules and accounting repeat identically");
            }
        }
    }

    void testCertifiedWinnerAndSuccessiveDuals()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        AssignmentResult first;
        QStringList firstReceipts;
        for (std::int32_t repeat = 0;
             repeat < 3;
             ++repeat)
        {
            Session session(field, enemy, 32);
            Valuer valuer(session);
            auto input = reversalInput(session);
            input.pStockValuer = &valuer;
            input.relativeStockWorkLimit = 32;
            Recorder recorder;
            input.pRelativeStockObserver = &recorder;

            for (std::size_t left = 0;
                 left < 3;
                 ++left)
            {
                for (std::size_t right = 0;
                     right < 3;
                     ++right)
                {
                    session.plans[
                        replacementKey(
                            input,
                            left,
                            right)]
                        .ours = {
                            {0, 90}, 80, 1};
                }
            }
            const ContinuationKey incumbentKey =
                replacementKey(input, 0, 0);
            const ContinuationKey triggerKey =
                replacementKey(input, 1, 1);
            const ContinuationKey firstWinnerKey =
                replacementKey(input, 2, 2);
            const ContinuationKey secondWinnerKey =
                replacementKey(input, 1, 2);
            session.plans[incumbentKey].ours = {
                {100, 200}, 100, 1000};
            session.plans[triggerKey].ours = {
                {0, 300}, 103, 1};
            session.plans[firstWinnerKey].ours = {
                {105, 260}, 105, 1000};
            session.plans[secondWinnerKey].ours = {
                {0, 190}, 107, 1};
            session.dualEnabled = true;
            session.dualUppers[incumbentKey] = 101;
            session.dualUppers[firstWinnerKey] = 106;

            const auto result =
                MaximumValueAssignment::assign(input);
            const auto firstAction =
                actionOfUnit(result.plan, 1);
            const auto secondAction =
                actionOfUnit(result.plan, 2);
            expect(
                result.stats.stockPairs.improvements == 2 &&
                    result.stats.replayFailures == 0 &&
                    result.plan.action(firstAction).destination ==
                        TilePoint{0, 1} &&
                    result.plan.action(secondAction).destination ==
                        TilePoint{1, 1},
                "DETERMINISTIC_CERTIFIED_WINNER");
            expect(
                recorder.acceptedChallengers.size() == 2 &&
                    recorder.acceptedChallengers[0] ==
                        firstWinnerKey &&
                    recorder.acceptedChallengers[1] ==
                        secondWinnerKey,
                "certified winner uses lower, upper, support and tuple proof ordering");
            expect(
                session.dualBuilds == 2 &&
                    session.dualCertificates.size() == 2 &&
                    session.dualCertificates.at(
                        incumbentKey)
                            .id !=
                        session.dualCertificates.at(
                            firstWinnerKey)
                            .id &&
                    recorder.acceptedIncumbents[0] ==
                        incumbentKey &&
                    recorder.acceptedIncumbents[1] ==
                        firstWinnerKey,
                "SUCCESSIVE_REPLACEMENTS_REQUIRE_NEW_CERTIFICATES");
            expect(
                result.stats.stockPairs.workUsed == 32 &&
                    session.ledger.stats().spent() == 32 &&
                    session.ledger.stats()
                            .grantedByWork[
                                static_cast<std::size_t>(
                                    RefinementWork::
                                        PackingDualCertificate)] ==
                        24,
                "successive duals remain inside one 32-work assignment allocation");
            if (repeat == 0)
            {
                first = result;
                firstReceipts = recorder.receipts;
            }
            expect(
                samePlan(first, result) &&
                    first.stats == result.stats &&
                    firstReceipts == recorder.receipts,
                "certified winner and successive certificate schedule repeat identically");
        }
    }

    void testDualPairAndJointShareLedger()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 1000);
        configureStock(session, 3, 3);
        session.dualEnabled = true;
        Valuer valuer(session);
        Recorder recorder;
        auto input = makeInput(3);
        input.pStockValuer = &valuer;
        input.pRelativeStockObserver = &recorder;
        const std::array<std::size_t, 3> incumbentChoices{
            0, 0, 0};
        const ContinuationKey incumbent =
            selectionKey(input, incumbentChoices);
        session.plans[incumbent].ours = {
            {100, 150}, 100, 1000};
        session.dualUppers[incumbent] = 100;
        const auto result =
            MaximumValueAssignment::assign(input);
        expect(
            result.stats.stockJoints.improvements == 1 &&
                session.dualBuilds == 1 &&
                result.stats.stockPairs.workUsed +
                        result.stats.stockJoints.workUsed ==
                    session.ledger.stats().spent() &&
                session.ledger.stats().spent() <=
                    input.relativeStockWorkLimit,
            "DUAL_AND_PAIR_AND_JOINT_SHARE_ONE_LEDGER");
        expect(
            session.ledger.stats()
                    .grantedByWork[
                        static_cast<std::size_t>(
                            RefinementWork::
                                PackingDualCertificate)] ==
                    session.ourTariff +
                        session.dualCatalogWork +
                        session.dualOptimizerWork +
                        session.dualVerifierWork &&
                result.stats.replayFailures == 0,
            "one verified incumbent certificate is shared across pair and joint phases");
    }

    void testSharedDualRefreshesOtherContenders()
    {
        PropertyStockField field;
        PropertyStockInstance enemy;
        Session session(field, enemy, 16);
        Valuer valuer(session);
        auto input = reversalInput(session);
        input.pStockValuer = &valuer;
        input.relativeStockWorkLimit = 16;
        Recorder recorder;
        input.pRelativeStockObserver = &recorder;

        for (std::size_t left = 0; left < 3; ++left)
        {
            for (std::size_t right = 0; right < 3; ++right)
            {
                session.plans[
                    replacementKey(input, left, right)]
                    .ours = {{0, 90}, 80, 1};
            }
        }
        const ContinuationKey incumbent =
            replacementKey(input, 0, 0);
        const ContinuationKey trigger =
            replacementKey(input, 1, 0);
        const ContinuationKey winner =
            replacementKey(input, 2, 2);
        session.plans[incumbent].ours = {
            {100, 200}, 100, 1000};
        session.plans[trigger].ours = {
            {0, 300}, 101, 1};
        session.plans[winner].ours = {
            {105, 105}, 105, 1};
        session.dualEnabled = true;
        session.dualUppers[incumbent] = 101;

        const auto result =
            MaximumValueAssignment::assign(input);
        const auto first =
            actionOfUnit(result.plan, 1);
        const auto second =
            actionOfUnit(result.plan, 2);
        const bool selected =
            first != NO_ACTION &&
            second != NO_ACTION &&
            result.plan.hasAction(first) &&
            result.plan.hasAction(second);
        expect(
            selected &&
                result.stats.stockPairs.improvements == 1 &&
                result.stats.stockPairs.workUsed == 16 &&
                session.ledger.stats().spent() == 16 &&
                session.dualBuilds == 1 &&
                recorder.acceptedChallengers ==
                    std::vector<ContinuationKey>{winner} &&
                result.plan.action(first).destination ==
                    TilePoint{1, 0} &&
                result.plan.action(second).destination ==
                    TilePoint{1, 1},
            "shared incumbent dual refresh selects another certified-positive contender after local exhaustion");
    }

    void testCanonicalCertifiedTieOrder()
    {
        auto input = makeInput(0);
        addChoiceActor(
            input,
            30,
            {0, 0},
            {{{0, 0}, 10}, {{1, 0}, 10}});
        addChoiceActor(
            input,
            10,
            {0, 1},
            {{{0, 1}, 10}, {{1, 1}, 10}});
        addChoiceActor(
            input,
            20,
            {0, 2},
            {{{0, 2}, 10}, {{1, 2}, 10}});
        std::vector<ActorProgress> actors;
        for (const auto & actor : input.actors)
        {
            ActorProgress progress;
            progress.pActor = &actor;
            actors.push_back(progress);
        }
        const std::vector<std::int32_t> actor30{0};
        const std::vector<std::int32_t> actor10{1};
        expect(
            canonicalActorSupportFirst(
                actors,
                actor10,
                actor30) &&
                !canonicalActorSupportFirst(
                    actors,
                    actor30,
                    actor10),
            "certified support ties use canonical actor IDs");

        std::vector<std::int32_t> actorOrder{
            0, 1, 2};
        std::sort(
            actorOrder.begin(),
            actorOrder.end(),
            ActorIdOrder{&actors});
        const std::vector<std::int32_t> left{
            0, 1, 0};
        const std::vector<std::int32_t> right{
            1, 0, 0};
        expect(
            canonicalCandidateTupleFirst(
                actorOrder,
                right,
                left) &&
                !canonicalCandidateTupleFirst(
                    actorOrder,
                    left,
                    right),
            "certified candidate ties use actor-ID tuple order");
    }

    void testActionLabelInvariance()
    {
        std::vector<PlanBundleKind> winners;
        for (const bool captureFirst :
             {false, true})
        {
            PropertyStockField field;
            PropertyStockInstance enemy;
            Session session(field, enemy, 16);
            Valuer valuer(session);
            auto input = reversalInput(session);
            auto & first =
                input.actors[0].candidates[1].bundle;
            auto & second =
                input.actors[0].candidates[2].bundle;
            (captureFirst ? first : second)
                .components.push_back(
                    captureComponent({}));
            session.stockActors = {1, 2};
            session.enemies.at({}) = {
                {0, 0}, 0, 1, true};
            for (std::size_t left = 0;
                 left < 3;
                 ++left)
            {
                for (std::size_t right = 0;
                     right < 3;
                     ++right)
                {
                    const std::array<std::size_t, 2>
                        choices{left, right};
                    session.plans[
                        selectionKey(
                            input,
                            choices)]
                        .ours = {
                            {0, 90}, 80, 1};
                }
            }
            const std::array<std::size_t, 2>
                incumbentChoices{0, 0};
            const std::array<std::size_t, 2>
                firstChoices{1, 0};
            const std::array<std::size_t, 2>
                secondChoices{2, 0};
            const auto incumbent =
                selectionKey(
                    input,
                    incumbentChoices);
            const auto canonicalWinner =
                selectionKey(
                    input,
                    firstChoices);
            session.plans[incumbent].ours = {
                {100, 200}, 100, 1000};
            session.plans[canonicalWinner].ours = {
                {105, 105}, 105, 1};
            session.plans[
                selectionKey(
                    input,
                    secondChoices)]
                .ours = {
                    {105, 105}, 105, 1};
            session.dualEnabled = true;
            session.dualUppers[incumbent] = 101;
            input.pStockValuer = &valuer;
            input.relativeStockWorkLimit = 16;
            Recorder recorder;
            input.pTrace = &recorder;
            input.pRelativeStockObserver = &recorder;

            const auto result =
                MaximumValueAssignment::assign(input);
            const auto action =
                actionOfUnit(result.plan, 1);
            const bool selected =
                !result.selections.empty() &&
                action != NO_ACTION &&
                result.plan.hasAction(action);
            expect(
                selected &&
                    result.stats.stockPairs.improvements == 1 &&
                    result.stats.replayFailures == 0 &&
                    recorder.acceptedChallengers ==
                        std::vector<ContinuationKey>{
                            canonicalWinner} &&
                    result.selections[0].candidateIndex == 1,
                "proof ties select the canonical tuple regardless of action label");
            winners.push_back(
                selected
                    ? result.plan.action(action).kind
                    : PlanBundleKind::Wait);
        }
        expect(
            winners ==
                std::vector<PlanBundleKind>{
                    PlanBundleKind::Move,
                    PlanBundleKind::MoveAndCapture},
            "ACTION_LABEL_INVARIANCE");
    }
}

int main()
{
    testPairAndStockMembership();
    testJointAndRepeat();
    testRollback();
    testBoundedSkips();
    testNonStockExact();
    testSuccessiveDependencyReversal();
    testNonMemberDependentSurvivesTrial();
    testRebuiltOrderRollback();
    testLaterNonRelativeStage();
    testConflictingMixedPairRefinesWithRelativeSession();
    testMixedAndStockPairShareBudget();
    testMixedLocalExhaustion();
    testMixedPairPrefixCertificatesShareBudget();
    testSevenStockActorsWithExpensiveDecoy();
    testCertifiedWinnerAndSuccessiveDuals();
    testDualPairAndJointShareLedger();
    testSharedDualRefreshesOtherContenders();
    testCanonicalCertifiedTieOrder();
    testActionLabelInvariance();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
