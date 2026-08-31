#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/propertystocksequential.h"

namespace
{
    using Coordinator::AssignmentActor;
    using Coordinator::AssignmentInput;
    using Coordinator::AssignmentResult;
    using Coordinator::CandidateBundle;
    using Coordinator::CaptureFacts;
    using Coordinator::KnownUnitLink;
    using Coordinator::MaximumValueAssignment;
    using Coordinator::MilliFunds;
    using Coordinator::PlanActionIds;
    using Coordinator::PlanBundleKind;
    using Coordinator::PlannedAction;
    using Coordinator::PlanStockValuer;
    using Coordinator::TilePoint;
    using Coordinator::TurnPlan;

    constexpr std::int32_t FIRST_INDEX = 0;
    constexpr std::int32_t SECOND_INDEX = 1;
    constexpr std::int32_t FIRST_UNIT = 31;
    constexpr std::int32_t SECOND_UNIT = 32;
    constexpr TilePoint FIRST_COLUMN{1, 1};
    constexpr TilePoint SECOND_COLUMN{4, 1};
    constexpr TilePoint FIRST_SPARE{1, 3};
    constexpr TilePoint SECOND_SPARE{4, 3};
    constexpr MilliFunds MOVE_VALUE = 15;
    constexpr MilliFunds LONE_CAPTURE_STOCK = -10;
    constexpr MilliFunds PAIRED_CAPTURE_STOCK = 50;
    constexpr MilliFunds HOSTILE_PAIRED_STOCK = -30;

    int failures = 0;

    void expect(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    class DenialStockValuer final : public PlanStockValuer
    {
    public:
        DenialStockValuer(MilliFunds loneCaptureStock, MilliFunds pairedCaptureStock)
            : m_loneCaptureStock(loneCaptureStock)
            , m_pairedCaptureStock(pairedCaptureStock)
        {
        }

        MilliFunds tableValue(bool firstCaptured, bool secondCaptured) const
        {
            if (firstCaptured && secondCaptured)
            {
                return m_pairedCaptureStock;
            }
            if (firstCaptured || secondCaptured)
            {
                return m_loneCaptureStock;
            }
            return 0;
        }

        MilliFunds planStock(const TurnPlan & plan) override
        {
            bool firstCaptured = false;
            bool secondCaptured = false;
            for (std::int32_t index = 0; index < plan.actionCount(); ++index)
            {
                const PlannedAction & action = plan.action(index);
                if (!Coordinator::isLiveState(action.state) || action.kind != PlanBundleKind::Capture)
                {
                    continue;
                }
                firstCaptured = firstCaptured || action.destination == FIRST_COLUMN;
                secondCaptured = secondCaptured || action.destination == SECOND_COLUMN;
            }
            return tableValue(firstCaptured, secondCaptured);
        }

        MilliFunds originStock() const override
        {
            return 0;
        }

        MilliFunds stockCeiling() const override
        {
            ++m_ceilingCalls;
            return std::max({MilliFunds{0}, m_loneCaptureStock, m_pairedCaptureStock});
        }

        bool affectsStock(std::int32_t engineUnitId) const override
        {
            return engineUnitId == FIRST_UNIT || engineUnitId == SECOND_UNIT;
        }

        bool livePairSwapIntervals() const override
        {
            return true;
        }

        Coordinator::LivePlanStockQuote livePlanStock(
            const TurnPlan & plan,
            MilliFunds,
            bool) override
        {
            Coordinator::LivePlanStockQuote quote;
            for (std::int32_t index = 0;
                 index < plan.actionCount();
                 ++index)
            {
                const PlannedAction & action =
                    plan.action(index);
                if (!Coordinator::isLiveState(
                        action.state))
                {
                    continue;
                }
                quote.key.push_back(
                    Coordinator::CanonicalPlanActionKey::
                        fromAction(
                            action.unitId,
                            action.destination.x,
                            action.destination.y,
                            false));
            }
            std::sort(quote.key.begin(), quote.key.end());
            const MilliFunds stock = planStock(plan);
            quote.stockAbsolute =
                Coordinator::StockInterval{stock, stock};
            quote.valid = true;
            quote.lowerWitnessReplays = true;
            return quote;
        }

        std::int32_t ceilingCalls() const
        {
            return m_ceilingCalls;
        }

    private:
        MilliFunds m_loneCaptureStock;
        MilliFunds m_pairedCaptureStock;
        mutable std::int32_t m_ceilingCalls{0};
    };

    CandidateBundle moveCandidate(std::int32_t unitIndex, TilePoint origin, TilePoint destination,
                                  MilliFunds value)
    {
        CandidateBundle candidate;
        candidate.bundle.unitId = unitIndex;
        candidate.bundle.origin = origin;
        candidate.bundle.destination = destination;
        candidate.bundle.path = {origin, destination};
        candidate.valuation.ledger.enemyCapital = value;
        candidate.valuation.valid = true;
        return candidate;
    }

    CandidateBundle captureCandidate(std::int32_t unitIndex, TilePoint column)
    {
        CandidateBundle candidate;
        candidate.bundle.unitId = unitIndex;
        candidate.bundle.origin = column;
        candidate.bundle.destination = column;
        candidate.bundle.path = {column};
        candidate.bundle.components.push_back(Coordinator::captureComponent(CaptureFacts{}));
        candidate.valuation.valid = true;
        return candidate;
    }

    AssignmentInput denialInput(PlanStockValuer & valuer, bool reversed)
    {
        AssignmentInput input;
        input.actionIds = PlanActionIds{
            .wait = QStringLiteral("ACTION_WAIT"),
            .fire = QStringLiteral("ACTION_FIRE"),
            .capture = QStringLiteral("ACTION_CAPTURE"),
        };
        input.unitLinks.push_back(KnownUnitLink{FIRST_UNIT, FIRST_COLUMN});
        input.unitLinks.push_back(KnownUnitLink{SECOND_UNIT, SECOND_COLUMN});
        input.pStockValuer = &valuer;

        AssignmentActor first;
        first.knowledgeUnitIndex = FIRST_INDEX;
        first.engineUnitId = FIRST_UNIT;
        first.candidates.push_back(captureCandidate(FIRST_INDEX, FIRST_COLUMN));
        first.candidates.push_back(moveCandidate(FIRST_INDEX, FIRST_COLUMN, FIRST_SPARE, MOVE_VALUE));

        AssignmentActor second;
        second.knowledgeUnitIndex = SECOND_INDEX;
        second.engineUnitId = SECOND_UNIT;
        second.candidates.push_back(captureCandidate(SECOND_INDEX, SECOND_COLUMN));
        second.candidates.push_back(moveCandidate(SECOND_INDEX, SECOND_COLUMN, SECOND_SPARE, MOVE_VALUE));

        if (reversed)
        {
            std::reverse(first.candidates.begin(), first.candidates.end());
            std::reverse(second.candidates.begin(), second.candidates.end());
            input.actors.push_back(std::move(second));
            input.actors.push_back(std::move(first));
        }
        else
        {
            input.actors.push_back(std::move(first));
            input.actors.push_back(std::move(second));
        }
        return input;
    }

    MilliFunds staticPlanValue(const AssignmentResult & result)
    {
        MilliFunds total = 0;
        for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
        {
            const PlannedAction & action = result.plan.action(index);
            if (Coordinator::isLiveState(action.state))
            {
                total += action.marginalValue.economicValue;
            }
        }
        return total;
    }

    MilliFunds achievedValue(const AssignmentResult & result, DenialStockValuer & valuer)
    {
        return staticPlanValue(result) + valuer.planStock(result.plan) - valuer.originStock();
    }

    MilliFunds exhaustiveOptimum(const DenialStockValuer & valuer)
    {
        MilliFunds best = std::numeric_limits<MilliFunds>::min();
        for (const bool firstCaptures : {false, true})
        {
            for (const bool secondCaptures : {false, true})
            {
                const MilliFunds staticValue = (firstCaptures ? 0 : MOVE_VALUE) +
                                               (secondCaptures ? 0 : MOVE_VALUE);
                best = std::max(best, staticValue + valuer.tableValue(firstCaptures, secondCaptures));
            }
        }
        return best;
    }

    const PlannedAction* actionOf(const AssignmentResult & result, std::int32_t engineUnitId)
    {
        for (std::int32_t index = 0; index < result.plan.actionCount(); ++index)
        {
            if (result.plan.action(index).unitId == engineUnitId)
            {
                return &result.plan.action(index);
            }
        }
        return nullptr;
    }

    void testJointCaptureBeatsAdditiveReasoning()
    {
        DenialStockValuer valuer(LONE_CAPTURE_STOCK, PAIRED_CAPTURE_STOCK);
        const AssignmentResult result = MaximumValueAssignment::assign(denialInput(valuer, false));
        const MilliFunds optimum = exhaustiveOptimum(valuer);
        expect(optimum == PAIRED_CAPTURE_STOCK, "paired capture is the exhaustive optimum");
        expect(achievedValue(result, valuer) == optimum, "assignment matches the joint exhaustive optimum");
        const PlannedAction* pFirst = actionOf(result, FIRST_UNIT);
        const PlannedAction* pSecond = actionOf(result, SECOND_UNIT);
        expect(pFirst != nullptr && pFirst->kind == PlanBundleKind::Capture,
               "first capper takes its column");
        expect(pSecond != nullptr && pSecond->kind == PlanBundleKind::Capture,
               "second capper takes its column");
        expect(valuer.ceilingCalls() > 0, "cluster search consumes the property-aware stock ceiling");
    }

    void testHostileJointStockRepelsCaptures()
    {
        DenialStockValuer valuer(LONE_CAPTURE_STOCK, HOSTILE_PAIRED_STOCK);
        const AssignmentResult result = MaximumValueAssignment::assign(denialInput(valuer, false));
        const MilliFunds optimum = exhaustiveOptimum(valuer);
        expect(optimum == MOVE_VALUE + MOVE_VALUE, "both moves are the hostile fixture optimum");
        expect(achievedValue(result, valuer) == optimum, "assignment refuses hostile captures");
        expect(valuer.planStock(result.plan) == 0, "no capture remains live");
    }

    void testSeatingOrderInvariance()
    {
        DenialStockValuer valuer(LONE_CAPTURE_STOCK, PAIRED_CAPTURE_STOCK);
        const AssignmentResult forward = MaximumValueAssignment::assign(denialInput(valuer, false));
        const AssignmentResult backward = MaximumValueAssignment::assign(denialInput(valuer, true));
        expect(achievedValue(forward, valuer) == achievedValue(backward, valuer),
               "actor and option order preserve the objective");
        expect(valuer.planStock(forward.plan) == valuer.planStock(backward.plan),
               "actor and option order preserve the selected stock state");
    }

    void testSequentialContinuationComposesWithJointStock()
    {
        constexpr std::int32_t horizonTurns = 6;
        const std::vector<Coordinator::PropertyStockColumn> columns{
            Coordinator::PropertyStockColumn{
                .slot = 0,
                .tile = FIRST_COLUMN,
                .income = Coordinator::PropertyIncome{
                    .oursPerTurn = 100,
                    .enemyPerTurn = 100,
                },
                .ownerBefore = Coordinator::OwnerSign::Neutral,
            },
            Coordinator::PropertyStockColumn{
                .slot = 1,
                .tile = SECOND_COLUMN,
                .income = Coordinator::PropertyIncome{
                    .oursPerTurn = 80,
                    .enemyPerTurn = 80,
                },
                .ownerBefore = Coordinator::OwnerSign::Neutral,
            },
        };
        const std::array<std::int32_t, 4> arrivals{
            Coordinator::UNREACHABLE,
            1,
            1,
            Coordinator::UNREACHABLE,
        };
        Coordinator::SequentialClassTable table;
        table.build(columns, arrivals, 1, horizonTurns);
        const std::array<Coordinator::SequentialClassTable, 1> classes{
            table
        };
        const Coordinator::SequentialInstance instance{
            .horizonTurns = horizonTurns,
            .nodeColumns =
                std::span<const Coordinator::PropertyStockColumn>(columns),
            .classes =
                std::span<const Coordinator::SequentialClassTable>(classes),
            .rows = {
                Coordinator::SequentialRow{
                    .classIndex = 0,
                    .weight = {100, 0},
                    .ownedTurn = {1, 1},
                },
                Coordinator::SequentialRow{
                    .classIndex = 0,
                    .weight = {0, 90},
                    .ownedTurn = {1, 1},
                },
            },
            .capturedNodes = {false, false},
        };
        const Coordinator::SequentialTierResult continuation =
            Coordinator::solveSequentialPacking(instance, 190, 100000);
        expect(continuation.bookedValue > continuation.floorValue,
               "the joint stock term retains feasible continuation value");

        DenialStockValuer valuer(LONE_CAPTURE_STOCK,
                                 continuation.bookedValue);
        const AssignmentResult result =
            MaximumValueAssignment::assign(denialInput(valuer, false));
        expect(achievedValue(result, valuer) == exhaustiveOptimum(valuer),
               "sequential continuation composes as one joint stock term");
    }
}

int main()
{
    testJointCaptureBeatsAdditiveReasoning();
    testHostileJointStockRepelsCaptures();
    testSeatingOrderInvariance();
    testSequentialContinuationComposesWithJointStock();
    return failures == 0 ? 0 : 1;
}
