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

    constexpr MilliFunds ORIGIN_BEST = 50;
    constexpr MilliFunds DESTINATION_BEST = 120;
    constexpr MilliFunds EXPOSURE = -60;
    constexpr MilliFunds TARGET_CREDIT = 900;
    constexpr MilliFunds PROPERTY = 400;
    constexpr MilliFunds PARTNER = 25;
    // Ledger 2940 plus property and partner continuation minus the origin best shot.
    constexpr MilliFunds ACTING_STATIC = 3315;
    constexpr MilliFunds BOOKED_STEP = 1000;
    constexpr std::int32_t FULL_GRANT = 3;

    CandidateBundle actingCandidate()
    {
        constexpr MilliFunds SECOND_ORIGIN_SHOT = 30;
        CandidateBundle result = fireCandidate(0);
        auto & fire = result.bundle.components.front().fire;
        fire.targetBestShotBefore = TARGET_CREDIT;
        EconomicDelta & ledger = result.valuation.ledger;
        ledger = EconomicDelta{.income = 100, .friendlyCapital = -200, .enemyCapital = 3000, .actionCost = -10,
                               .resourceCost = -20, .scriptedCapital = 30, .revaluation = 40};
        result.valuation.continuation = ContinuationDelta{
            .repositioning = DESTINATION_BEST - ORIGIN_BEST, .exposure = EXPOSURE,
            .targetContinuationRemoved = TARGET_CREDIT, .propertyContinuation = PROPERTY,
            .partnerContinuation = PARTNER};
        result.actorNextShotsAtOrigin = {SECOND_ORIGIN_SHOT, ORIGIN_BEST};
        result.actorNextShotsAtDestination = {DESTINATION_BEST};
        return result;
    }

    CandidateBundle rebuilt(CandidateBundle candidate)
    {
        candidate.valuation =
            valueBundle(candidate.bundle, candidate.actor, positionFacts(candidate), ValuationContext{});
        return candidate;
    }

    CandidateBundle builtFire(std::int32_t actorHp, std::int32_t damage, std::int32_t counter)
    {
        constexpr std::int32_t ACTOR_ID = 2;
        constexpr MilliFunds ACTOR_COST = 7000;
        CandidateBundle result = fireCandidate(0);
        auto & fire = result.bundle.components.front().fire;
        fire.damageSteps = damage;
        fire.counterSteps = counter;
        fire.targetBestShotAfter = TARGET_CREDIT / 2;
        result.bundle.unitId = ACTOR_ID;
        result.bundle.origin = TilePoint{0, 0};
        result.bundle.destination = TilePoint{1, 0};
        result.actor = ActorFacts{ACTOR_COST, actorHp};
        result.actorNextShotsAtOrigin = {ORIGIN_BEST};
        result.actorNextShotsAtDestination = {DESTINATION_BEST};
        result.enemyShotsOnActorAtOrigin = {-EXPOSURE};
        result.enemyShotsOnActorAtDestination = {DESTINATION_BEST};
        return rebuilt(result);
    }

    void testActingFireGrants()
    {
        const auto shot = actingCandidate();
        const auto legacy = checkedSeatedCandidateValue(shot, FULL_GRANT);
        expect(checkedActingSeatedCandidateValue(shot, FULL_GRANT) == ACTING_STATIC && legacy &&
                   *legacy - EXPOSURE - TARGET_CREDIT - DESTINATION_BEST == ACTING_STATIC,
               "full grant: legacy minus exposure, target continuation and destination best is the acting value");
        for (std::int32_t granted = 0; granted < FULL_GRANT; ++granted)
        {
            const auto acting = checkedActingSeatedCandidateValue(shot, granted);
            const auto seated = checkedSeatedCandidateValue(shot, granted);
            const MilliFunds credit = proRatedCredit(TARGET_CREDIT, granted, FULL_GRANT) - TARGET_CREDIT;
            expect(acting == ACTING_STATIC - (FULL_GRANT - granted) * BOOKED_STEP && seated &&
                       *seated - EXPOSURE - TARGET_CREDIT - DESTINATION_BEST - credit == acting,
                   "a partial grant moves only booked capital and drops the prorated credit the legacy value keeps");
        }

        auto positive = actingCandidate();
        auto & kept = positive.bundle.components.front().fire;
        kept.targetHpSteps = FULL_GRANT + 1;
        kept.targetBestShotAfter = 0;
        auto negative = positive;
        negative.bundle.components.front().fire.targetBestShotAfter = TARGET_CREDIT * 2;
        expect(checkedActingSeatedCandidateValue(positive, 1) == checkedActingSeatedCandidateValue(negative, 1) &&
                   checkedActingSeatedCandidateValue(negative, 1) == ACTING_STATIC - 2 * BOOKED_STEP &&
                   checkedSeatedCandidateValue(positive, 1) != checkedSeatedCandidateValue(negative, 1),
               "the acting value does not depend on the sign of the target credit");

        auto huge = actingCandidate();
        huge.bundle.components.front().fire.targetBestShotBefore = std::numeric_limits<MilliFunds>::max();
        expect(!checkedSeatedCandidateValue(huge, 1) &&
                   checkedActingSeatedCandidateValue(huge, 1) == ACTING_STATIC - 2 * BOOKED_STEP,
               "a credit that overflows the legacy partial grant never touches the acting value");

        auto surviving = actingCandidate();
        auto & survivor = surviving.bundle.components.front().fire;
        survivor.targetHpSteps = FULL_GRANT + 1;
        survivor.targetBestShotBefore = std::numeric_limits<MilliFunds>::max();
        survivor.targetBestShotAfter = -1;
        expect(!checkedSeatedCandidateValue(surviving, 1) &&
                   checkedActingSeatedCandidateValue(surviving, 1) == ACTING_STATIC - 2 * BOOKED_STEP,
               "a credit subtraction that overflows the legacy partial grant never touches the acting value");
    }

    void testActingOriginSource()
    {
        constexpr MilliFunds STEP = 7;
        constexpr MilliFunds FAR_DESTINATION = 200;
        const auto base = actingCandidate();
        auto origin = base;
        origin.actorNextShotsAtOrigin = {ORIGIN_BEST + STEP};
        auto destination = base;
        destination.actorNextShotsAtDestination = {FAR_DESTINATION};
        expect(checkedActingSeatedCandidateValue(origin, 1) ==
                       checkedActingSeatedCandidateValue(base, 1).value() - STEP &&
                   checkedActingSeatedCandidateValue(destination, 1) == checkedActingSeatedCandidateValue(base, 1),
               "the acting value reads the origin spans themselves, never the stored repositioning");
        auto empty = base;
        empty.actorNextShotsAtOrigin.clear();
        expect(checkedActingSeatedCandidateValue(empty, FULL_GRANT) == ACTING_STATIC + ORIGIN_BEST,
               "an empty origin span is a zero baseline");
    }

    void testActingTerms()
    {
        constexpr MilliFunds STEP = 7;
        const auto base = actingCandidate();
        for (MilliFunds EconomicDelta::* term : {&EconomicDelta::income, &EconomicDelta::friendlyCapital,
             &EconomicDelta::enemyCapital, &EconomicDelta::actionCost, &EconomicDelta::resourceCost,
             &EconomicDelta::scriptedCapital, &EconomicDelta::revaluation})
        {
            auto moved = base;
            moved.valuation.ledger.*term += STEP;
            expect(checkedActingSeatedCandidateValue(moved, 1) ==
                       checkedActingSeatedCandidateValue(base, 1).value() + STEP,
                   "every ledger term moves the acting value one for one");
        }
        for (MilliFunds ContinuationDelta::* term : {&ContinuationDelta::propertyContinuation,
             &ContinuationDelta::partnerContinuation})
        {
            auto moved = base;
            moved.valuation.continuation.*term += STEP;
            expect(checkedActingSeatedCandidateValue(moved, 0) ==
                       checkedActingSeatedCandidateValue(base, 0).value() + STEP,
                   "property and partner continuation stay in the acting value");
        }

        constexpr std::int32_t ALIVE_HP = 10;
        const auto built = builtFire(ALIVE_HP, FULL_GRANT, 0);
        const auto acting = checkedActingSeatedCandidateValue(built, 1);
        auto origin = built;
        origin.actorNextShotsAtOrigin = {ORIGIN_BEST + STEP};
        expect(acting && checkedActingSeatedCandidateValue(rebuilt(origin), 1) == *acting - STEP,
               "the origin best shot stays in the acting value one for one");
        for (std::vector<MilliFunds> CandidateBundle::* span : {&CandidateBundle::actorNextShotsAtDestination,
             &CandidateBundle::enemyShotsOnActorAtOrigin, &CandidateBundle::enemyShotsOnActorAtDestination})
        {
            auto moved = built;
            (moved.*span).push_back(DESTINATION_BEST + STEP);
            moved = rebuilt(moved);
            expect(moved.valuation.valid &&
                       checkedSeatedCandidateValue(moved, 1) != checkedSeatedCandidateValue(built, 1) &&
                       checkedActingSeatedCandidateValue(moved, 1) == acting,
                   "destination and enemy shot spans move the legacy value but not the acting value");
        }

        constexpr std::int32_t DYING_HP = 2;
        constexpr std::int32_t FULL_COUNTER = 10;
        auto dead = builtFire(DYING_HP, 1, FULL_COUNTER);
        const auto deadActing = checkedActingSeatedCandidateValue(dead, 1);
        const auto deadLegacy = checkedSeatedCandidateValue(dead, 1);
        dead.actorNextShotsAtDestination = {DESTINATION_BEST * 2};
        expect(deadActing && deadLegacy && dead.valuation.continuation.repositioning == -ORIGIN_BEST &&
                   *deadActing == dead.valuation.ledger.total() - ORIGIN_BEST &&
                   checkedActingSeatedCandidateValue(rebuilt(dead), 1) == deadActing,
               "a dead actor still carries minus the origin best shot and ignores its destination");
    }

    void testActingRejections()
    {
        auto shot = actingCandidate();
        expect(!checkedActingSeatedCandidateValue(shot, -1) && !checkedActingSeatedCandidateValue(shot, FULL_GRANT + 1),
               "a grant outside zero to the dealt damage is rejected");
        shot.bundle.components.front().fire.targetUnitId = NO_UNIT;
        expect(!checkedActingSeatedCandidateValue(shot, FULL_GRANT), "a malformed fire is rejected");
        auto still = actingCandidate();
        still.bundle.components.clear();
        expect(checkedActingSeatedCandidateValue(still, 0) == ACTING_STATIC &&
                   !checkedActingSeatedCandidateValue(still, 1),
               "a non-fire candidate takes no damage grant");
        auto invalid = actingCandidate();
        invalid.valuation.valid = false;
        expect(!checkedActingSeatedCandidateValue(invalid, 0), "an invalid valuation is rejected");
        auto overflow = actingCandidate();
        overflow.valuation.ledger.income = std::numeric_limits<MilliFunds>::max();
        expect(!checkedActingSeatedCandidateValue(overflow, 0), "ledger overflow is rejected");
        auto underflow = candidate(std::numeric_limits<MilliFunds>::min());
        underflow.actorNextShotsAtOrigin = {ORIGIN_BEST};
        expect(!checkedActingSeatedCandidateValue(underflow, 0), "subtracting the origin best shot fails closed");
    }
}

int main()
{
    testSeatedValues();
    testClippingAndZeroDamage();
    testStaticTotals();
    testRoundingAndOverflow();
    testMalformedFire();
    testActingFireGrants();
    testActingTerms();
    testActingOriginSource();
    testActingRejections();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
