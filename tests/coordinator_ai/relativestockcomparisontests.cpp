#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>

#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/relativestocksession.h"

namespace
{
    using namespace Coordinator;

    constexpr MilliFunds MILLION = 1000000;
    int checks = 0;
    int failures = 0;

    void expect(bool condition, std::string_view message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
    }

    class FixtureSession final : public RelativeStockSession
    {
    public:
        FixtureSession(const PropertyStockField & field, const PropertyStockInstance & enemy)
            : RelativeStockSession(field, enemy)
        {
        }

        RelativeStockQuote makeQuote(StockInterval ours, StockInterval enemy) const
        {
            RelativeStockQuote result;
            result.key = {CanonicalPlanActionKey::fromAction(0, 1, 1, false)};
            result.identity = identity();
            result.ours = ours;
            result.enemy = enemy;
            result.ourLowerWitnessReplays = true;
            result.enemyLowerWitnessReplays = true;
            result.valid = true;
            return result;
        }

        RelativeStockQuote quote(const TurnPlan &, MilliFunds) override { return {}; }
        RelativeStockQuote quote(const ContinuationKey &, MilliFunds) override { return {}; }
        RelativeStockRefinedTerm refineOur(const ContinuationKey &, std::int64_t) override { return {}; }
        RelativeStockRefinedTerm refineEnemy(const std::vector<std::int32_t> &, std::int64_t) override
        {
            return {};
        }
        bool replayOurWitness(const RelativeStockQuote &) override { return false; }
        bool replayEnemyWitness(const RelativeStockQuote &) override { return false; }
        std::int64_t remainingWork() const override { return 0; }
        std::int64_t ourRerunWork() const override { return 0; }
        std::int64_t enemyRerunWork(const std::vector<std::int32_t> &) const override { return 0; }
    };

    void testSharedEnemy(FixtureSession & session)
    {
        auto incumbent = session.makeQuote({112 * MILLION, 112 * MILLION}, {99 * MILLION, 110 * MILLION});
        auto challenger = session.makeQuote({117 * MILLION, 117 * MILLION}, incumbent.enemy);
        incumbent.ourSearchCompleted = true;
        challenger.ourSearchCompleted = true;
        incumbent.enemyLowerWitnessReplays = false;
        challenger.enemyLowerWitnessReplays = false;
        const auto beforeIncumbent = incumbent;
        const auto beforeChallenger = challenger;
        auto result = session.compare(incumbent, challenger);
        expect(result.improves() && result.commonEnemyCancelled, "common unresolved enemy cancels");
        expect(result.deltaLower == 5 * MILLION && result.deltaUpper == 5 * MILLION,
               "112m versus 117m proves exactly +5m");
        expect(result.exact(), "relative result can be exact");
        expect(!incumbent.absoluteExact() && !challenger.absoluteExact(), "absolute values remain unknown");
        expect(112 * MILLION - incumbent.enemy.lower >= 117 * MILLION - challenger.enemy.upper,
               "absolute intervals overlap in the preserved fixture");
        expect(sameRelativeStockQuote(beforeIncumbent, incumbent) &&
                   sameRelativeStockQuote(beforeChallenger, challenger),
               "comparison does not mutate quotes or exactness");

        incumbent = session.makeQuote({74 * MILLION, 74 * MILLION}, {0, 60 * MILLION});
        challenger = session.makeQuote({75 * MILLION, 75 * MILLION}, incumbent.enemy);
        result = compareRelativeStock(incumbent, challenger);
        expect(result.improves() && result.exact() && result.deltaLower == MILLION,
               "actual Day-2 our continuations prove exactly +1m");
        expect(!incumbent.absoluteExact() && !challenger.absoluteExact(), "Day-2 enemy remains unresolved");

        challenger.enemy = {20 * MILLION, 70 * MILLION};
        expect(compareRelativeStock(incumbent, challenger).commonEnemyCancelled,
               "compatible tightened common enemy bounds still cancel");
        challenger.enemy = {61 * MILLION, 70 * MILLION};
        expect(compareRelativeStock(incumbent, challenger).reason ==
                   RelativeStockReason::ContradictoryCommonEnemy,
               "contradictory common enemy bounds fail closed");
    }

    void testIndependentEnemy(FixtureSession & session)
    {
        auto incumbent = session.makeQuote({10, 15}, {20, 30});
        auto challenger = session.makeQuote({12, 19}, {18, 27});
        incumbent.economic = 4;
        incumbent.owned = 6;
        challenger.economic = 9;
        challenger.owned = 8;
        challenger.capturedColumns = {1};
        const auto result = compareRelativeStock(incumbent, challenger);
        expect(result.valid() && !result.commonEnemyCancelled, "different captured keys refuse cancellation");
        expect(result.economicDelta == 5 && result.ownedDelta == 2, "economic and owned deltas retained");
        expect(result.deltaLower == -3 && result.deltaUpper == 28, "independent conservative subtraction");
        expect(result.unresolved() && !result.improves(), "relative overlap does not authorize replacement");
        for (const bool challengerSide : {false, true})
        {
            auto left = incumbent;
            auto right = challenger;
            auto & changed = challengerSide ? right : left;
            changed.enemyLowerWitnessReplays = false;
            expect(compareRelativeStock(left, right).reason == RelativeStockReason::MissingEnemyWitness,
                   "each independent enemy lower bound needs its own witness");
            changed.enemyLowerWitnessReplays = true;
            changed.ourLowerWitnessReplays = false;
            expect(compareRelativeStock(left, right).reason == RelativeStockReason::MissingOurWitness,
                   "each our lower bound needs its own witness");
        }
        challenger.capturedColumns.clear();
        challenger.enemy = incumbent.enemy;
        incumbent.enemyLowerWitnessReplays = false;
        challenger.enemyLowerWitnessReplays = false;
        expect(compareRelativeStock(incumbent, challenger).valid(), "cancelled enemy needs no lower witness");
        challenger.ourLowerWitnessReplays = false;
        expect(compareRelativeStock(incumbent, challenger).reason == RelativeStockReason::MissingOurWitness,
               "enemy cancellation never removes our witness obligation");
    }

    void testIdentity()
    {
        static_assert(!std::is_constructible_v<RelativeStockIdentity,
                                             const PropertyStockField*, const PropertyStockInstance*>);
        static_assert(!std::is_copy_constructible_v<FixtureSession>);
        static_assert(!std::is_move_constructible_v<FixtureSession>);
        PropertyStockField field;
        PropertyStockField otherField;
        PropertyStockInstance enemy;
        PropertyStockInstance otherEnemy;
        FixtureSession first(field, enemy);
        FixtureSession second(field, enemy);
        FixtureSession changedField(otherField, enemy);
        FixtureSession changedEnemy(field, otherEnemy);
        auto incumbent = first.makeQuote({10, 10}, {0, 10});
        auto challenger = first.makeQuote({11, 11}, {0, 10});
        expect(first.owns(incumbent) && !second.owns(incumbent), "identity belongs only to its issuing session");
        for (auto* incompatible : {&second, &changedField, &changedEnemy})
        {
            auto other = incompatible->makeQuote({11, 11}, {0, 10});
            expect(compareRelativeStock(incumbent, other).reason == RelativeStockReason::InvalidIdentity,
                   "incompatible session, snapshot or enemy model fails closed");
            expect(incompatible->compare(incumbent, challenger).reason == RelativeStockReason::InvalidIdentity,
                   "a foreign session cannot compare borrowed quotes");
        }
        challenger.identity = {};
        expect(compareRelativeStock(incumbent, challenger).reason == RelativeStockReason::InvalidIdentity,
               "missing identity fails closed");
        RelativeStockQuote expiredIncumbent;
        RelativeStockQuote expiredChallenger;
        {
            FixtureSession temporary(field, enemy);
            expiredIncumbent = temporary.makeQuote({10, 10}, {0, 10});
            expiredChallenger = temporary.makeQuote({11, 11}, {0, 10});
            expect(compareRelativeStock(expiredIncumbent, expiredChallenger).improves(),
                   "live session identity permits comparison");
        }
        expect(!expiredIncumbent.identity.established(), "quote does not keep its session alive");
        expect(compareRelativeStock(expiredIncumbent, expiredChallenger).reason ==
                   RelativeStockReason::InvalidIdentity,
               "expired matching identities cannot cancel");
    }

    void testValidation(FixtureSession & session)
    {
        const auto incumbent = session.makeQuote({10, 10}, {0, 10});
        const auto challenger = session.makeQuote({11, 11}, {0, 10});
        auto changed = challenger;
        changed.valid = false;
        expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::InvalidQuote,
               "invalid quote fails closed");
        changed = challenger;
        changed.ours = {12, 11};
        expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::InvalidInterval,
               "inverted our interval fails closed");
        changed = challenger;
        changed.enemy = {11, 10};
        expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::InvalidInterval,
               "inverted enemy interval fails closed");
        changed = challenger;
        changed.ours = {10, 11};
        changed.ourSearchCompleted = true;
        expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::InvalidInterval,
               "completed our search cannot retain uncertain bounds");
        changed = challenger;
        changed.enemySearchCompleted = true;
        expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::InvalidInterval,
               "completed enemy search cannot retain uncertain bounds");
        const std::array<std::vector<std::int32_t>, 3> badCaptured{{{2, 1}, {1, 1}, {-1}}};
        for (const auto & columns : badCaptured)
        {
            changed = challenger;
            changed.capturedColumns = columns;
            expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::NonCanonicalKey,
                   "captured columns must be sorted, unique and nonnegative");
        }
        const std::array<ContinuationKey, 3> badKeys{{
            {CanonicalPlanActionKey::fromAction(1, 1, 1, false),
             CanonicalPlanActionKey::fromAction(0, 1, 1, false)},
            {CanonicalPlanActionKey::fromAction(0, 1, 1, false),
             CanonicalPlanActionKey::fromAction(0, 2, 1, false)},
            {CanonicalPlanActionKey::fromAction(-1, 1, 1, false)},
        }};
        for (const auto & key : badKeys)
        {
            changed = challenger;
            changed.key = key;
            expect(compareRelativeStock(incumbent, changed).reason == RelativeStockReason::NonCanonicalKey,
                   "canonical plan requires sorted distinct nonnegative rows");
        }
    }

    void testThresholdsAndOverflow(FixtureSession & session)
    {
        const auto incumbent = session.makeQuote({10, 10}, {0, 10});
        expect(compareRelativeStock(incumbent, session.makeQuote({11, 11}, {0, 10})).improves(),
               "strictly positive lower bound accepts");
        expect(compareRelativeStock(incumbent, session.makeQuote({10, 11}, {0, 10})).unresolved(),
               "zero lower bound does not accept");
        const auto upper103 = session.makeQuote(
            {0, 103 * MILLION},
            {0, 10});
        expect(
            compareRelativeStock(
                upper103,
                session.makeQuote(
                    {103 * MILLION, 103 * MILLION},
                    {0, 10}))
                .unresolved(),
            "STRICT_EQUALITY_REMAINS_UNRESOLVED");
        expect(
            compareRelativeStock(
                upper103,
                session.makeQuote(
                    {104 * MILLION, 104 * MILLION},
                    {0, 10}))
                .improves(),
            "LOWER_104_UPPER_103_ACCEPTS");
        expect(compareRelativeStock(incumbent, session.makeQuote({9, 10}, {0, 10})).reason ==
                   RelativeStockReason::CertifiedNonImprovement,
               "zero upper bound rejects");
        expect(compareRelativeStock(incumbent, session.makeQuote({9, 9}, {0, 10})).reason ==
                   RelativeStockReason::CertifiedNonImprovement,
               "negative upper bound rejects");
        auto left = incumbent;
        auto right = incumbent;
        left.economic = std::numeric_limits<MilliFunds>::min();
        right.economic = 1;
        expect(compareRelativeStock(left, right).reason == RelativeStockReason::ArithmeticOverflow,
               "economic subtraction overflow fails closed");
        left = incumbent;
        right = incumbent;
        left.owned = std::numeric_limits<MilliFunds>::max();
        right.owned = -2;
        expect(compareRelativeStock(left, right).reason == RelativeStockReason::ArithmeticOverflow,
               "owned subtraction overflow fails closed");
        left = incumbent;
        right = incumbent;
        right.economic = std::numeric_limits<MilliFunds>::max();
        right.owned = 1;
        expect(compareRelativeStock(left, right).reason == RelativeStockReason::ArithmeticOverflow,
               "constant addition overflow fails closed");
        right.owned = 0;
        expect(compareRelativeStock(left, right).reason == RelativeStockReason::ArithmeticOverflow,
               "continuation addition overflow fails closed");
        MilliFunds value = std::numeric_limits<MilliFunds>::min();
        expect(!RelativeStockDetail::checkedSubtract(value, 1), "checked subtract detects underflow");
        value = std::numeric_limits<MilliFunds>::min();
        expect(!RelativeStockDetail::checkedAdd(value, -1), "checked add detects underflow");
    }

    void testQueueAndQuoteReplay(FixtureSession & session)
    {
        struct Entry
        {
            RelativeStockComparison comparison;
            std::vector<std::int32_t> tuple;
        };
        const std::vector<Entry> original{
            {{.deltaLower = 8, .deltaUpper = 20}, {2, 0}},
            {{.deltaLower = 8, .deltaUpper = 20}, {1, 1}},
            {{.deltaLower = -5, .deltaUpper = 21}, {8, 8}},
            {{.deltaLower = 7, .deltaUpper = 20}, {0, 0}},
        };
        std::vector<std::vector<std::int32_t>> firstOrder;
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            auto entries = original;
            std::sort(entries.begin(), entries.end(), [](const Entry & left, const Entry & right)
            {
                return relativeStockUpperFirst(left.comparison, left.tuple, right.comparison, right.tuple);
            });
            std::vector<std::vector<std::int32_t>> order;
            for (const auto & entry : entries)
            {
                order.push_back(entry.tuple);
            }
            if (repeat == 0)
            {
                firstOrder = order;
            }
            expect(order == firstOrder, "upper-guided queue ordering repeats exactly");
        }
        expect(firstOrder == std::vector<std::vector<std::int32_t>>{{8, 8}, {1, 1}, {2, 0}, {0, 0}},
               "queue sorts upper first, lower second, canonical tuple last");
        const auto originalQuote = session.makeQuote({74, 74}, {0, 60});
        expect(sameRelativeStockQuote(originalQuote, originalQuote), "identical quote replays");
        auto changed = originalQuote;
        ++changed.ours.upper;
        expect(!sameRelativeStockQuote(originalQuote, changed), "replay matches both endpoints");
        changed = originalQuote;
        changed.key = {CanonicalPlanActionKey::fromAction(0, 2, 1, false)};
        expect(!sameRelativeStockQuote(originalQuote, changed), "replay matches exact continuation key");
        changed = originalQuote;
        changed.enemyLowerWitnessReplays = false;
        expect(!sameRelativeStockQuote(originalQuote, changed), "replay retains separate enemy witness state");
        changed = originalQuote;
        changed.ourSearchCompleted = true;
        expect(!sameRelativeStockQuote(originalQuote, changed), "replay retains actual search completion");
    }
}

int main()
{
    PropertyStockField field;
    PropertyStockInstance enemy;
    FixtureSession session(field, enemy);
    testSharedEnemy(session);
    testIndependentEnemy(session);
    testIdentity();
    testValidation(session);
    testThresholdsAndOverflow(session);
    testQueueAndQuoteReplay(session);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
