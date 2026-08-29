#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ai/coordinator/continuationinterval.h"

namespace
{
    using Coordinator::CanonicalPlanActionKey;
    using Coordinator::CanonicalPlanActionKeyHash;
    using Coordinator::ContinuationEntry;
    using Coordinator::ContinuationKey;
    using Coordinator::ContinuationStore;
    using Coordinator::ContenderRegistry;
    using Coordinator::EnemySetBounds;
    using Coordinator::LIVE_PAIR_REFINEMENT_BUDGET;
    using Coordinator::LIVE_PAIR_SLICE_BASE;
    using Coordinator::MilliFunds;
    using Coordinator::RowWitness;
    using Coordinator::StockInterval;

    constexpr std::array<std::int32_t, 6> KEY_SAMPLES = {
        std::numeric_limits<std::int32_t>::min(),
        -1,
        0,
        1,
        65535,
        std::numeric_limits<std::int32_t>::max(),
    };

    int failures = 0;

    void expect(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    ContinuationKey key(std::int32_t row)
    {
        return {CanonicalPlanActionKey::fromAction(row, row + 1, row + 2, row % 2 != 0)};
    }

    void testCanonicalActionKey()
    {
        std::set<CanonicalPlanActionKey> ordered;
        std::unordered_set<CanonicalPlanActionKey, CanonicalPlanActionKeyHash> hashed;
        for (const std::int32_t row : KEY_SAMPLES)
        {
            for (const std::int32_t x : KEY_SAMPLES)
            {
                for (const std::int32_t y : KEY_SAMPLES)
                {
                    for (const bool captures : {false, true})
                    {
                        const CanonicalPlanActionKey action =
                            CanonicalPlanActionKey::fromAction(row, x, y, captures);
                        expect(action.row() == row && action.x() == x && action.y() == y &&
                                   action.captures() == captures,
                               "canonical action key round-trips");
                        ordered.insert(action);
                        hashed.insert(action);
                    }
                }
            }
        }
        const std::size_t expected =
            KEY_SAMPLES.size() * KEY_SAMPLES.size() * KEY_SAMPLES.size() * 2;
        expect(ordered.size() == expected, "ordered keys are injective");
        expect(hashed.size() == expected, "hashed keys are injective");
        expect(CanonicalPlanActionKey::fromAction(-1, 0, 0, false) <
                   CanonicalPlanActionKey::fromAction(0, 0, 0, false),
               "key ordering uses signed row values");
        expect(CanonicalPlanActionKey::fromAction(0, -1, 0, false) <
                   CanonicalPlanActionKey::fromAction(0, 0, 0, false),
               "key ordering uses signed coordinates");
        expect(CanonicalPlanActionKey::fromAction(0, 0, 0, false) <
                   CanonicalPlanActionKey::fromAction(0, 0, 0, true),
               "capture state orders last");
    }

    void testContinuationKeyOrder()
    {
        ContinuationKey forward = {key(1).front(), key(-1).front(), key(3).front()};
        ContinuationKey reverse = {key(3).front(), key(-1).front(), key(1).front()};
        std::sort(forward.begin(), forward.end());
        std::sort(reverse.begin(), reverse.end());
        expect(forward == reverse, "canonical continuation order ignores insertion order");
    }

    void testLivePairBudgets()
    {
        expect(LIVE_PAIR_REFINEMENT_BUDGET == 2150537, "live refinement budget is frozen");
        expect(LIVE_PAIR_SLICE_BASE == 9609, "live slice base is frozen");
        std::int64_t cap = 0;
        expect(Coordinator::livePairRefinementSliceCap(0, cap) && cap == LIVE_PAIR_SLICE_BASE,
               "rung zero uses the base slice");
        expect(Coordinator::livePairRefinementSliceCap(3, cap) &&
                   cap == LIVE_PAIR_SLICE_BASE * 8,
               "slice rungs double exactly");
        const std::int64_t priorCap = cap;
        expect(!Coordinator::livePairRefinementSliceCap(-1, cap) && cap == priorCap,
               "negative slice rung is rejected atomically");
        expect(!Coordinator::livePairRefinementSliceCap(63, cap),
               "overflowing slice rung is rejected");

        std::int32_t rung = 0;
        expect(!Coordinator::advanceLivePairRefinementRung(rung, false) && rung == 0,
               "incomplete work does not advance a rung");
        expect(Coordinator::advanceLivePairRefinementRung(rung, true) && rung == 1,
               "completed work advances one rung");
        rung = std::numeric_limits<std::int32_t>::max();
        expect(!Coordinator::advanceLivePairRefinementRung(rung, true),
               "maximum rung does not overflow");
    }

    void testStockIntervals()
    {
        const StockInterval exact{50, 50};
        expect(exact.exact() && exact.width() == 0, "exact interval has zero width");
        const StockInterval composed = Coordinator::composeStockInterval(
            100, StockInterval{10, 40}, StockInterval{20, 35});
        expect(composed.lower == 75 && composed.upper == 120,
               "signed stock bounds pair our lower with enemy upper");
    }

    void testContinuationStore()
    {
        ContinuationStore store;
        ContinuationEntry entry;
        entry.key = key(1);
        entry.capturedColumns = {2, 4};
        entry.ours = StockInterval{10, 30};
        entry.witness = {RowWitness{.row = 2, .nodes = {1, 3}, .value = 10}};
        ContinuationEntry & admitted = store.admit(std::move(entry));
        expect(admitted.initialOurs.lower == 10 && admitted.initialOurs.upper == 30,
               "admission freezes initial bounds");
        expect(admitted.initialWitness.size() == 1 &&
                   admitted.initialWitness.front().row == 2 &&
                   admitted.initialWitness.front().nodes == std::vector<std::int32_t>({1, 3}) &&
                   admitted.initialWitness.front().value == 10,
               "admission freezes the initial witness");
        expect(!ContinuationStore::tightenOurLower(admitted, 9, {}),
               "lower bound cannot loosen");
        expect(ContinuationStore::tightenOurLower(
                   admitted, 15, {RowWitness{.row = 3, .nodes = {2}, .value = 15}}),
               "larger witnessed lower tightens");
        expect(!ContinuationStore::tightenOurUpper(admitted, 31),
               "upper bound cannot loosen");
        expect(ContinuationStore::tightenOurUpper(admitted, 25),
               "smaller upper tightens");
        expect(admitted.initialOurs.lower == 10 && admitted.initialOurs.upper == 30,
               "tightening preserves initial bounds");

        ContinuationEntry duplicate;
        duplicate.key = key(1);
        duplicate.ours = StockInterval{99, 99};
        expect(&store.admit(std::move(duplicate)) == &admitted,
               "duplicate admission keeps the canonical entry");
        expect(store.find(key(1)) == &admitted && store.find(key(9)) == nullptr,
               "store lookup follows the canonical key");

        EnemySetBounds enemy;
        enemy.bounds = StockInterval{4, 12};
        EnemySetBounds & admittedEnemy = store.admitEnemy({2, 4}, enemy);
        expect(admittedEnemy.initialBounds.lower == 4 &&
                   admittedEnemy.initialBounds.upper == 12,
               "enemy admission freezes initial bounds");
        expect(store.enemyFor({2, 4}) == &admittedEnemy && store.enemyFor({4, 2}) == nullptr,
               "enemy lookup uses the captured-column set order");
    }

    void testContenderOrder()
    {
        ContenderRegistry registry;
        registry.open(2);
        registry.encounter(key(1), 10, 20);
        registry.encounter(key(2), 20, 20);
        registry.encounter(key(3), 20, 30);
        registry.encounter(key(3), 99, 99);

        std::vector<ContinuationKey> order = registry.refinementOrder();
        expect(order.size() == 2 && order[0] == key(3) && order[1] == key(2),
               "demand keeps the strongest canonical contenders");
        const auto* contender = registry.find(key(3));
        expect(contender != nullptr && contender->cheapLower == 20 &&
                   contender->cheapUpper == 30,
               "first encounter fixes contender bounds");

        registry.markAdmitted(key(1));
        order = registry.refinementOrder();
        expect(order.size() == 3 && order[0] == key(3) && order[1] == key(2) &&
                   order[2] == key(1),
               "admitted contender remains eligible outside demand capacity");
        registry.markResolved(key(3));
        order = registry.refinementOrder();
        expect(order.size() == 2 && order[0] == key(2) && order[1] == key(1),
               "resolved contender leaves refinement order");
    }
}

int main()
{
    testCanonicalActionKey();
    testContinuationKeyOrder();
    testLivePairBudgets();
    testStockIntervals();
    testContinuationStore();
    testContenderOrder();
    return failures == 0 ? 0 : 1;
}
