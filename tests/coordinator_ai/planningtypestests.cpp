#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/turnplan.h"

namespace
{
    using Coordinator::CandidateBundle;
    using Coordinator::MilliFunds;
    using Coordinator::PlanStockValuer;
    using Coordinator::PositionFacts;
    using Coordinator::TerminalClass;
    using Coordinator::TerminalValue;
    using Coordinator::TilePoint;
    using Coordinator::TurnPlan;
    using Coordinator::toMilliFunds;

    constexpr std::int32_t FIRST_UNIT = 7;
    constexpr std::int32_t SECOND_UNIT = 11;
    constexpr TilePoint ORIGIN{2, 3};
    constexpr TilePoint DESTINATION{4, 3};
    constexpr MilliFunds ORIGIN_STOCK = toMilliFunds(3000);
    constexpr MilliFunds PLAN_STOCK = toMilliFunds(4500);
    constexpr MilliFunds STOCK_CEILING = toMilliFunds(9000);
    constexpr MilliFunds LOWER_VALUE = toMilliFunds(1200);
    constexpr MilliFunds HIGHER_VALUE = toMilliFunds(1800);

    int failures = 0;

    void expect(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAILED: %s\n", description);
            ++failures;
        }
    }

    class FixedStockValuer final : public PlanStockValuer
    {
    public:
        MilliFunds planStock(const TurnPlan &) override
        {
            ++calls;
            return PLAN_STOCK;
        }

        MilliFunds originStock() const override
        {
            return ORIGIN_STOCK;
        }

        MilliFunds stockCeiling() const override
        {
            return STOCK_CEILING;
        }

        bool affectsStock(std::int32_t engineUnitId) const override
        {
            return engineUnitId == FIRST_UNIT;
        }

        std::int32_t calls{0};
    };

    CandidateBundle candidate(std::int32_t unitId, MilliFunds value)
    {
        CandidateBundle result;
        result.bundle.unitId = unitId;
        result.bundle.origin = ORIGIN;
        result.bundle.destination = DESTINATION;
        result.bundle.path = {ORIGIN, DESTINATION};
        result.actor.replacementCost = toMilliFunds(7000);
        result.actor.hpSteps = 8;
        result.movementCost = 3;
        result.actorNextShotsAtOrigin = {toMilliFunds(300), toMilliFunds(600)};
        result.actorNextShotsAtDestination = {toMilliFunds(900)};
        result.enemyShotsOnActorAtOrigin = {toMilliFunds(200)};
        result.enemyShotsOnActorAtDestination = {toMilliFunds(500), toMilliFunds(700)};
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        return result;
    }

    void testCandidateOwnsPositionFacts()
    {
        CandidateBundle original = candidate(FIRST_UNIT, HIGHER_VALUE);
        const PositionFacts originalFacts = Coordinator::positionFacts(original);
        expect(originalFacts.actorNextShotsAtOrigin.size() == 2, "origin shot facts keep both entries");
        expect(originalFacts.enemyShotsOnActorAtDestination.back() == toMilliFunds(700),
               "destination exposure facts preserve their values");
        expect(originalFacts.actorNextShotsAtOrigin.data() == original.actorNextShotsAtOrigin.data(),
               "the view addresses the candidate-owned storage");

        CandidateBundle copy = original;
        const PositionFacts copyFacts = Coordinator::positionFacts(copy);
        expect(copyFacts.actorNextShotsAtOrigin.data() == copy.actorNextShotsAtOrigin.data(),
               "a copied candidate produces views over the copy");
        expect(copyFacts.actorNextShotsAtOrigin.data() != originalFacts.actorNextShotsAtOrigin.data(),
               "candidate copies do not alias owned fact buffers");
    }

    void testCandidateIdentityAndValueSurviveCopy()
    {
        const CandidateBundle original = candidate(FIRST_UNIT, HIGHER_VALUE);
        const CandidateBundle copy = original;
        expect(copy.bundle.unitId == FIRST_UNIT, "the engine unit identity survives a copy");
        expect(copy.bundle.origin == ORIGIN && copy.bundle.destination == DESTINATION,
               "the action geometry survives a copy");
        expect(copy.valuation.valid, "valuation validity survives a copy");
        expect(copy.valuation.value() == TerminalValue{TerminalClass::Unresolved, HIGHER_VALUE},
               "the candidate value survives a copy");
    }

    void testTerminalOrderingIsDeterministic()
    {
        std::vector<CandidateBundle> candidates{
            candidate(SECOND_UNIT, LOWER_VALUE),
            candidate(FIRST_UNIT, HIGHER_VALUE),
            candidate(FIRST_UNIT, LOWER_VALUE),
        };
        std::sort(candidates.begin(), candidates.end(), [](const CandidateBundle & lhs, const CandidateBundle & rhs)
        {
            if (lhs.valuation.value() != rhs.valuation.value())
            {
                return lhs.valuation.value() > rhs.valuation.value();
            }
            return lhs.bundle.unitId < rhs.bundle.unitId;
        });
        expect(candidates[0].bundle.unitId == FIRST_UNIT, "higher value sorts first");
        expect(candidates[1].bundle.unitId == FIRST_UNIT, "equal values tie to the lower unit id");
        expect(candidates[2].bundle.unitId == SECOND_UNIT, "higher unit id sorts last among equal values");
    }

    void testScalarStockBoundaryDispatchesVirtually()
    {
        TurnPlan plan;
        FixedStockValuer concrete;
        PlanStockValuer & valuer = concrete;
        expect(valuer.planStock(plan) == PLAN_STOCK, "plan stock dispatches through the scalar interface");
        expect(concrete.calls == 1, "plan stock is evaluated once");
        expect(valuer.originStock() == ORIGIN_STOCK, "origin stock crosses the interface");
        expect(valuer.stockCeiling() == STOCK_CEILING, "the admissible ceiling crosses the interface");
        expect(valuer.affectsStock(FIRST_UNIT), "the affected unit is reported");
        expect(!valuer.affectsStock(SECOND_UNIT), "an unrelated unit is not stock coupled");
    }
}

int main()
{
    testCandidateOwnsPositionFacts();
    testCandidateIdentityAndValueSurviveCopy();
    testTerminalOrderingIsDeterministic();
    testScalarStockBoundaryDispatchesVirtually();
    return failures == 0 ? 0 : 1;
}
