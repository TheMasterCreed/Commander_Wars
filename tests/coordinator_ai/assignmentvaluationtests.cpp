#include <cstdio>
#include <limits>
#include <optional>

#include "ai/coordinator/assignmentvaluation.h"

namespace
{
    using namespace Coordinator;

    int checks = 0;
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAIL: %s\n", message);
        }
    }

    CandidateBundle candidate(MilliFunds value)
    {
        CandidateBundle result;
        result.valuation.valid = true;
        result.valuation.ledger.enemyCapital = value;
        return result;
    }

    CandidateBundle fireCandidate(MilliFunds value = 3900)
    {
        CandidateBundle result = candidate(value);
        result.bundle.components.push_back(fireComponent({
            .targetUnitId = 1,
            .targetReplacementCost = 10000,
            .targetHpSteps = 3,
            .damageSteps = 3,
            .targetBestShotBefore = 900,
        }));
        return result;
    }

    void testSeatedValues()
    {
        auto shot = fireCandidate();
        expect(checkedSeatedCandidateValue(shot, 3) == 3900,
               "full damage preserves the static value");
        expect(checkedSeatedCandidateValue(shot, 2) == 2600,
               "a two-of-three damage share seats at 2600");
        expect(checkedSeatedCandidateValue(shot, 1) == 1300,
               "a one-of-three damage share seats at 1300");
        expect(checkedSeatedCandidateValue(shot, 0) == 0,
               "zero damage removes target capital and positive credit");
        shot.valuation.ledger.enemyCapital = -1000;
        expect(checkedSeatedCandidateValue(shot, 2) == -2300,
               "negative static economics retain the same damage adjustment");
        shot = fireCandidate(2800);
        auto & fire = shot.bundle.components.front().fire;
        fire.targetHpSteps = 4;
        fire.targetBestShotBefore = 100;
        fire.targetBestShotAfter = 300;
        expect(checkedSeatedCandidateValue(shot, 2) == 1800,
               "negative continuation credit is not prorated");
        fire.targetReplacementCost = 0;
        fire.targetBestShotBefore = std::numeric_limits<MilliFunds>::min();
        fire.targetBestShotAfter = 0;
        shot.valuation.ledger.enemyCapital = 42;
        expect(checkedSeatedCandidateValue(shot, 0) == 42,
               "the minimum negative credit is representable when no negation is needed");
    }

    void testClippingAndZeroDamage()
    {
        auto shot = fireCandidate();
        auto & fire = shot.bundle.components.front().fire;
        fire.damageSteps = 10;
        expect(checkedSeatedCandidateValue(shot, 3) == 3900,
               "full share is clipped to target hp");
        expect(checkedSeatedCandidateValue(shot, 2) == 2600,
               "partial share uses dealt damage rather than requested damage");
        expect(!checkedSeatedCandidateValue(shot, 4),
               "a share above target hp is rejected");
        fire.damageSteps = 0;
        shot.valuation.ledger.enemyCapital = 17;
        expect(checkedSeatedCandidateValue(shot, 0) == 17,
               "a zero-damage full share does not divide by zero");
        expect(!checkedSeatedCandidateValue(shot, 1),
               "a share above zero dealt damage is rejected");
        shot = fireCandidate(19);
        auto & extreme = shot.bundle.components.front().fire;
        extreme.targetReplacementCost = std::numeric_limits<MilliFunds>::max();
        extreme.targetBestShotBefore = std::numeric_limits<MilliFunds>::min();
        extreme.targetBestShotAfter = std::numeric_limits<MilliFunds>::max();
        extreme.targetHpSteps = 2;
        extreme.damageSteps = 1;
        expect(checkedSeatedCandidateValue(shot, 1) == 19,
               "full grant preserves the legacy early return without unused arithmetic");
        expect(!checkedSeatedCandidateValue(shot, 0),
               "partial grant rejects overflowing arithmetic skipped by a full grant");
    }

    void testStaticTotals()
    {
        constexpr MilliFunds maximum = std::numeric_limits<MilliFunds>::max();
        constexpr MilliFunds minimum = std::numeric_limits<MilliFunds>::min();
        expect(checkedSeatedCandidateValue(candidate(maximum), 0) == maximum,
               "maximum static total is retained exactly");
        expect(checkedSeatedCandidateValue(candidate(minimum), 0) == minimum,
               "minimum static total is retained exactly");
        auto value = candidate(0);
        value.valuation.ledger.income = 100;
        value.valuation.ledger.friendlyCapital = -70;
        value.valuation.continuation.repositioning = 19;
        value.valuation.continuation.exposure = -9;
        expect(checkedSeatedCandidateValue(value, 0) == 40,
               "ledger and continuation use signed exact accumulation");
        value.valuation.ledger.income = maximum;
        value.valuation.ledger.friendlyCapital = 1;
        value.valuation.ledger.enemyCapital = -1;
        expect(!checkedSeatedCandidateValue(value, 0),
               "ledger intermediate overflow cannot be hidden by a later cancellation");
        value = candidate(0);
        value.valuation.continuation.repositioning = minimum;
        value.valuation.continuation.exposure = -1;
        value.valuation.continuation.targetContinuationRemoved = 1;
        expect(!checkedSeatedCandidateValue(value, 0),
               "continuation intermediate underflow cannot be hidden by cancellation");
        value = candidate(maximum);
        value.valuation.continuation.repositioning = 1;
        expect(!checkedSeatedCandidateValue(value, 0),
               "combined ledger and continuation overflow is rejected");
        value = candidate(minimum);
        value.valuation.continuation.exposure = -1;
        expect(!checkedSeatedCandidateValue(value, 0),
               "combined ledger and continuation underflow is rejected");
        value = candidate(0);
        value.valuation.valid = false;
        expect(!checkedSeatedCandidateValue(value, 0),
               "invalid valuation is not presented as a valid zero");
        expect(!checkedSeatedCandidateValue(candidate(7), 1),
               "a non-fire candidate cannot carry reserved damage");
    }

    void testRoundingAndOverflow()
    {
        constexpr MilliFunds maximum = std::numeric_limits<MilliFunds>::max();
        constexpr MilliFunds minimum = std::numeric_limits<MilliFunds>::min();
        auto shot = fireCandidate(1);
        auto & fire = shot.bundle.components.front().fire;
        fire.targetReplacementCost = 5;
        fire.targetHpSteps = 2;
        fire.damageSteps = 1;
        fire.targetBestShotBefore = 0;
        expect(checkedSeatedCandidateValue(shot, 0) == 0,
               "booked capital rounds a half away from zero");
        fire.targetReplacementCost = 0;
        fire.damageSteps = 2;
        fire.targetBestShotBefore = 1;
        expect(checkedSeatedCandidateValue(shot, 1) == 1,
               "positive continuation credit rounds a half away from zero");
        shot.valuation.ledger.enemyCapital = 922337203685477580;
        fire.targetReplacementCost = 4611686018427387901;
        fire.targetHpSteps = 3;
        fire.damageSteps = 2;
        fire.targetBestShotBefore = 0;
        expect(checkedSeatedCandidateValue(shot, 1) == 461168601842738790,
               "booked-capital product plus rounding may reach the exact maximum");
        ++fire.targetReplacementCost;
        expect(!checkedSeatedCandidateValue(shot, 1),
               "booked-capital rounding overflow fails closed");
        fire.targetReplacementCost = maximum / 2 + 1;
        expect(!checkedSeatedCandidateValue(shot, 1),
               "booked-capital multiplication overflow fails closed");
        fire.targetReplacementCost = 0;
        fire.targetHpSteps = 3;
        fire.damageSteps = 3;
        fire.targetBestShotBefore = 4611686018427387903;
        shot.valuation.ledger.enemyCapital = fire.targetBestShotBefore;
        expect(checkedSeatedCandidateValue(shot, 2) == 3074457345618258602,
               "credit product plus rounding may reach the exact maximum");
        ++fire.targetBestShotBefore;
        shot.valuation.ledger.enemyCapital = fire.targetBestShotBefore;
        expect(!checkedSeatedCandidateValue(shot, 2),
               "credit multiplication overflow fails closed");
        fire.targetHpSteps = 4;
        fire.damageSteps = 4;
        fire.targetBestShotBefore = 3074457345618258602;
        shot.valuation.ledger.enemyCapital = fire.targetBestShotBefore;
        expect(!checkedSeatedCandidateValue(shot, 3),
               "credit rounding overflow fails closed");
        --fire.targetBestShotBefore;
        shot.valuation.ledger.enemyCapital = fire.targetBestShotBefore;
        expect(checkedSeatedCandidateValue(shot, 3) == 2305843009213693951,
               "credit immediately below the rounding overflow boundary is exact");
        fire.damageSteps = 3;
        fire.targetBestShotBefore = maximum;
        fire.targetBestShotAfter = -1;
        shot.valuation.ledger.enemyCapital = 0;
        expect(!checkedSeatedCandidateValue(shot, 2),
               "best-shot subtraction overflow fails closed");
        fire.targetBestShotBefore = minimum;
        fire.targetBestShotAfter = 1;
        expect(!checkedSeatedCandidateValue(shot, 2),
               "best-shot subtraction underflow fails closed");
        fire.targetBestShotBefore = 0;
        fire.targetBestShotAfter = 0;
        fire.targetReplacementCost = 10;
        fire.damageSteps = 1;
        shot.valuation.ledger.enemyCapital = minimum;
        expect(!checkedSeatedCandidateValue(shot, 0),
               "capital subtraction underflow fails closed");
        fire.targetReplacementCost = 0;
        fire.targetBestShotBefore = 1;
        expect(!checkedSeatedCandidateValue(shot, 0),
               "continuation-credit subtraction underflow fails closed");
    }

    void testMalformedFire()
    {
        auto shot = fireCandidate();
        expect(!checkedSeatedCandidateValue(shot, -1), "negative grants are rejected");
        expect(!checkedSeatedCandidateValue(shot, 4), "excess grants are rejected");
        auto & fire = shot.bundle.components.front().fire;
        fire.targetReplacementCost = -1;
        expect(!checkedSeatedCandidateValue(shot, 3), "negative target cost is rejected");
        fire.targetReplacementCost = 10000;
        fire.targetHpSteps = 0;
        expect(!checkedSeatedCandidateValue(shot, 0), "dead targets are rejected");
        fire.targetHpSteps = 11;
        expect(!checkedSeatedCandidateValue(shot, 3), "out-of-range target hp is rejected");
        fire.targetHpSteps = 3;
        fire.damageSteps = 11;
        expect(!checkedSeatedCandidateValue(shot, 3), "out-of-range requested damage is rejected");
        fire.damageSteps = 3;
        fire.counterSteps = 11;
        expect(!checkedSeatedCandidateValue(shot, 3), "out-of-range counterdamage is rejected");
        fire.counterSteps = 0;
        fire.targetUnitId = NO_UNIT;
        expect(!checkedSeatedCandidateValue(shot, 3), "missing targets are rejected");
    }
}

int main()
{
    testSeatedValues();
    testClippingAndZeroDamage();
    testStaticTotals();
    testRoundingAndOverflow();
    testMalformedFire();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
