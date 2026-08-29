#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/propertystocksequential.h"

namespace
{
    using Coordinator::MilliFunds;
    using Coordinator::NO_CAPTURE_TURNS;
    using Coordinator::NO_SEQUENTIAL_NODE;
    using Coordinator::OwnerSign;
    using Coordinator::PropertyIncome;
    using Coordinator::PropertyStockColumn;
    using Coordinator::replaySequentialWitness;
    using Coordinator::SequentialClassTable;
    using Coordinator::SequentialInstance;
    using Coordinator::SequentialRow;
    using Coordinator::SequentialTierResult;
    using Coordinator::TilePoint;
    using Coordinator::solveSequentialPacking;
    using Coordinator::SequentialDetail::CachedRowEnumeration;
    using Coordinator::SequentialDetail::continuationGain;
    using Coordinator::SequentialDetail::EnumerationBudget;
    using Coordinator::SequentialDetail::realizeWitness;
    using Coordinator::SequentialDetail::RowOption;
    using Coordinator::SequentialDetail::RowOptionMap;
    using Coordinator::SequentialDetail::SequentialRowOptionSource;
    using Coordinator::SequentialDetail::WitnessStep;

    constexpr std::int32_t HORIZON_TURNS = 6;
    constexpr std::int32_t NODE_COUNT = 3;

    int failures = 0;

    void expect(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    PropertyStockColumn column(std::int32_t slot, MilliFunds income)
    {
        return PropertyStockColumn{
            .slot = slot,
            .tile = TilePoint{slot, 0},
            .income = PropertyIncome{
                .oursPerTurn = income,
                .enemyPerTurn = income,
            },
            .ownerBefore = OwnerSign::Neutral,
        };
    }

    std::array<std::int32_t, NODE_COUNT * NODE_COUNT> unitArrivals()
    {
        std::array<std::int32_t, NODE_COUNT * NODE_COUNT> arrivals{};
        for (std::int32_t from = 0; from < NODE_COUNT; ++from)
        {
            for (std::int32_t to = 0; to < NODE_COUNT; ++to)
            {
                arrivals[static_cast<std::size_t>(from * NODE_COUNT + to)] =
                    from == to ? Coordinator::UNREACHABLE : 1;
            }
        }
        return arrivals;
    }

    struct SequentialFixture
    {
        std::vector<PropertyStockColumn> columns{
            column(0, 100),
            column(1, 90),
            column(2, 80),
        };
        std::array<std::int32_t, NODE_COUNT * NODE_COUNT> arrivals{
            unitArrivals()
        };
        SequentialClassTable table;
        std::array<SequentialClassTable, 1> classes;
        SequentialInstance instance;

        SequentialFixture()
        {
            table.build(columns, arrivals, 1, HORIZON_TURNS);
            classes[0] = table;
            instance.horizonTurns = HORIZON_TURNS;
            instance.nodeColumns = columns;
            instance.classes = classes;
            instance.rows.push_back(SequentialRow{
                .classIndex = 0,
                .weight = {50, 40, 30},
                .ownedTurn = {1, 1, 1},
            });
            instance.capturedNodes.assign(columns.size(), false);
        }
    };

    class MemoryRowSource final : public SequentialRowOptionSource
    {
    public:
        bool lookup(std::int32_t rowIndex,
                    std::span<const RowOption> & options,
                    std::int64_t & statesSpent) override
        {
            ++lookups;
            const auto known = entries.find(rowIndex);
            if (known == entries.end())
            {
                return false;
            }
            options = std::span<const RowOption>(known->second.options);
            statesSpent = known->second.statesSpent;
            return true;
        }

        void store(std::int32_t rowIndex,
                   std::span<const RowOption> options,
                   std::int64_t statesSpent) override
        {
            ++stores;
            entries.emplace(
                rowIndex,
                CachedRowEnumeration{
                    .options =
                        std::vector<RowOption>(options.begin(), options.end()),
                    .statesSpent = statesSpent,
                });
        }

        std::map<std::int32_t, CachedRowEnumeration> entries;
        std::int32_t lookups{0};
        std::int32_t stores{0};
    };

    void testClassTableAndFirstLegBounds()
    {
        SequentialFixture fixture;
        expect(fixture.table.continuationOwnedTurn(0, 1, 1) == 2,
               "a unit arrival advances the owned turn exactly once");
        expect(fixture.table.continuationOwnedTurn(0, 1, 0) ==
                   NO_CAPTURE_TURNS,
               "a node cannot continue to itself");
        expect(fixture.table.continuationOwnedTurn(0, HORIZON_TURNS - 1, 1) ==
                   NO_CAPTURE_TURNS,
               "a continuation outside the horizon is excluded");
        expect(fixture.table.witnessAt(0, HORIZON_TURNS - 2) == 1,
               "equal successor order settles on the lowest eligible node");
        expect(Coordinator::sequentialFirstOwnedTurn(0, HORIZON_TURNS) ==
                   NO_CAPTURE_TURNS,
               "a carried-full first leg stays in the floor");
        expect(Coordinator::sequentialFirstOwnedTurn(1, HORIZON_TURNS) == 1,
               "a live first leg keeps its exact owned turn");
        expect(Coordinator::sequentialFirstOwnedTurn(HORIZON_TURNS,
                                                     HORIZON_TURNS) ==
                   NO_CAPTURE_TURNS,
               "a first leg outside the horizon has no continuation");

        constexpr std::int32_t PRIZE_HORIZON = 4;
        const std::array<MilliFunds, 8> prizes{
            0, 0, 0, 0,
            0, 0, 77, 0,
        };
        const std::array<std::int32_t, 4> arrivals{
            Coordinator::UNREACHABLE, 1,
            1, Coordinator::UNREACHABLE,
        };
        SequentialClassTable prizeTable;
        prizeTable.buildFromPrizes(2,
                                   prizes,
                                   arrivals,
                                   1,
                                   PRIZE_HORIZON);
        expect(prizeTable.valueAt(0, 1) == 77 &&
                   prizeTable.witnessAt(0, 1) == 1,
               "explicit prize tables retain the exact continuation");
    }

    void testCanonicalOptionDeduplication()
    {
        RowOptionMap options;
        std::int64_t enumerated = 0;
        const std::vector<WitnessStep> first{
            WitnessStep{2, 1},
            WitnessStep{0, 2},
        };
        const std::vector<WitnessStep> better{
            WitnessStep{0, 1},
            WitnessStep{2, 2},
        };
        Coordinator::SequentialDetail::recordOption(options,
                                                    first,
                                                    70,
                                                    enumerated);
        Coordinator::SequentialDetail::recordOption(options,
                                                    better,
                                                    90,
                                                    enumerated);
        Coordinator::SequentialDetail::recordOption(options,
                                                    first,
                                                    90,
                                                    enumerated);
        expect(options.size() == 1,
               "ordered itineraries sharing one node set deduplicate");
        expect(options.begin()->first == std::vector<std::int32_t>({0, 2}),
               "the option key is the sorted consumed node set");
        const RowOption & selected = options.begin()->second;
        expect(selected.value == 90 && selected.steps.size() == better.size() &&
                   selected.steps[0].node == better[0].node &&
                   selected.steps[1].node == better[1].node,
               "higher value replaces the representative and ties keep it");
        expect(enumerated == 3,
               "every ordered itinerary is counted before deduplication");
    }

    void testEnumerationOrderAndCapturedExclusion()
    {
        SequentialFixture fixture;
        fixture.instance.capturedNodes[1] = true;
        EnumerationBudget budget{
            .stateCap = 1000,
        };
        std::int64_t enumerated = 0;
        const std::vector<MilliFunds> enriched{1, 1, 1};
        const std::vector<RowOption> options =
            Coordinator::SequentialDetail::enumerateRowOptions(
                fixture.instance,
                0,
                enriched,
                budget,
                enumerated);
        expect(!options.empty(), "the open nodes produce itineraries");
        for (std::size_t index = 0; index < options.size(); ++index)
        {
            expect(std::is_sorted(options[index].nodes.begin(),
                                  options[index].nodes.end()),
                   "each itinerary uses a canonical node-set key");
            expect(std::find(options[index].nodes.begin(),
                             options[index].nodes.end(),
                             1) == options[index].nodes.end(),
                   "a captured node never enters an itinerary");
            if (index == 0)
            {
                continue;
            }
            const RowOption & previous = options[index - 1];
            const RowOption & current = options[index];
            expect(previous.value > current.value ||
                       (previous.value == current.value &&
                        previous.nodes < current.nodes),
                   "options are value ordered with canonical tie breaks");
        }
        expect(enumerated >= static_cast<std::int64_t>(options.size()),
               "deduplication never increases the option count");
    }

    void testBudgetBoundary()
    {
        EnumerationBudget budget{
            .stateCap = 1,
        };
        expect(!budget.spend() && budget.states == 1 && budget.truncated,
               "the state cap is charged before a capped branch expands");
        expect(!budget.spend() && budget.states == 1,
               "a truncated budget remains closed");
    }

    MilliFunds assignmentFloor(const SequentialInstance & instance)
    {
        std::vector<MilliFunds> weights;
        weights.reserve(instance.rows.size() *
                        static_cast<std::size_t>(instance.nodeCount()));
        for (const SequentialRow & row : instance.rows)
        {
            weights.insert(weights.end(), row.weight.begin(), row.weight.end());
        }
        Coordinator::AssignmentSolver solver;
        solver.solve(static_cast<std::int32_t>(instance.rows.size()),
                     instance.nodeCount(),
                     weights);
        return solver.optimum();
    }

    MilliFunds exhaustivePacking(const SequentialInstance & instance,
                                 MilliFunds floorValue)
    {
        std::vector<std::vector<RowOption>> optionsByRow;
        optionsByRow.reserve(instance.rows.size());
        for (std::int32_t rowIndex = 0;
             rowIndex < static_cast<std::int32_t>(instance.rows.size());
             ++rowIndex)
        {
            const SequentialRow & row =
                instance.rows[static_cast<std::size_t>(rowIndex)];
            std::vector<MilliFunds> enriched(
                static_cast<std::size_t>(instance.nodeCount()),
                0);
            for (std::int32_t node = 0; node < instance.nodeCount(); ++node)
            {
                const std::size_t slot = static_cast<std::size_t>(node);
                if (!instance.capturedNodes[slot])
                {
                    enriched[slot] =
                        row.weight[slot] +
                        continuationGain(instance, row, node);
                }
            }
            EnumerationBudget budget{
                .stateCap = 100000,
            };
            std::int64_t enumerated = 0;
            optionsByRow.push_back(
                Coordinator::SequentialDetail::enumerateRowOptions(
                    instance,
                    rowIndex,
                    enriched,
                    budget,
                    enumerated));
            expect(!budget.truncated,
                   "the exhaustive test option enumeration completes");
        }

        MilliFunds best = floorValue;
        std::vector<bool> used(
            static_cast<std::size_t>(instance.nodeCount()),
            false);
        std::function<void(std::size_t, MilliFunds)> visit =
            [&](std::size_t rowIndex, MilliFunds value)
        {
            if (rowIndex == optionsByRow.size())
            {
                best = std::max(best, value);
                return;
            }
            visit(rowIndex + 1, value);
            for (const RowOption & option : optionsByRow[rowIndex])
            {
                bool free = true;
                for (const std::int32_t node : option.nodes)
                {
                    free = free &&
                           !used[static_cast<std::size_t>(node)];
                }
                if (!free)
                {
                    continue;
                }
                for (const std::int32_t node : option.nodes)
                {
                    used[static_cast<std::size_t>(node)] = true;
                }
                visit(rowIndex + 1, value + option.value);
                for (const std::int32_t node : option.nodes)
                {
                    used[static_cast<std::size_t>(node)] = false;
                }
            }
        };
        visit(0, 0);
        return best;
    }

    void testWitnessRecoveryAndExactPacking()
    {
        SequentialFixture fixture;
        fixture.instance.rows = {
            SequentialRow{
                .classIndex = 0,
                .weight = {100, 0, 0},
                .ownedTurn = {1, 1, 1},
            },
            SequentialRow{
                .classIndex = 0,
                .weight = {0, 90, 0},
                .ownedTurn = {1, 1, 1},
            },
        };
        const MilliFunds floorValue = assignmentFloor(fixture.instance);
        const MilliFunds exhaustive =
            exhaustivePacking(fixture.instance, floorValue);
        const SequentialTierResult result =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000);
        const SequentialTierResult captured =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000,
                                   nullptr,
                                   true);
        expect(result.continuationSeen,
               "the fixture exposes a continuation");
        expect(result.searchStates > 0,
               "conflicting optimistic witnesses reach feasible search");
        expect(result.searchCompleted,
               "the bounded search completes for the compact fixture");
        expect(result.bookedValue == exhaustive,
               "witness recovery books the exact feasible packing");
        expect(result.bookedValue <= result.relaxedValue,
               "the optimistic assignment remains an upper bound");
        expect(captured.floorValue == result.floorValue &&
                   captured.relaxedValue == result.relaxedValue &&
                   captured.repairValue == result.repairValue &&
                   captured.searchValue == result.searchValue &&
                   captured.bookedValue == result.bookedValue &&
                   captured.searchStates == result.searchStates &&
                   captured.searchCompleted == result.searchCompleted,
               "witness capture does not change the exact solve");
        expect(!captured.witness.empty() &&
                   replaySequentialWitness(fixture.instance,
                                           captured.witness,
                                           captured.bookedValue),
               "captured exact witness replays");

        const std::vector<WitnessStep> first =
            realizeWitness(fixture.instance,
                           fixture.instance.rows[0],
                           0);
        const std::vector<WitnessStep> second =
            realizeWitness(fixture.instance,
                           fixture.instance.rows[1],
                           1);
        bool overlap = false;
        for (const WitnessStep & lhs : first)
        {
            for (const WitnessStep & rhs : second)
            {
                overlap = overlap || lhs.node == rhs.node;
            }
        }
        expect(overlap,
               "the relaxed witnesses conflict before feasible recovery");
    }

    void testCapturedNodeAndFloorCompatibility()
    {
        SequentialFixture fixture;
        fixture.instance.rows = {
            SequentialRow{
                .classIndex = 0,
                .weight = {100, 80, 60},
                .ownedTurn = {1, 1, 1},
            },
        };
        const MilliFunds floorValue = assignmentFloor(fixture.instance);
        const SequentialTierResult open =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000);
        fixture.instance.capturedNodes[1] = true;
        const SequentialTierResult captured =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000);
        expect(captured.bookedValue ==
                   exhaustivePacking(fixture.instance, floorValue),
               "a completed node is absent from the exact domain");
        expect(captured.bookedValue <= open.bookedValue,
               "completing a node cannot add another itinerary use");

        fixture.instance.rows[0].ownedTurn.assign(
            fixture.columns.size(),
            NO_CAPTURE_TURNS);
        const SequentialTierResult floorOnly =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000,
                                   nullptr,
                                   true);
        expect(!floorOnly.continuationSeen &&
                   floorOnly.bookedValue == floorValue &&
                   floorOnly.searchStates == 0 &&
                   floorOnly.witness.empty(),
               "no continuation reproduces the scalar floor exactly");
    }

    void testExhaustiveCeilingEquivalence()
    {
        for (std::int32_t mask = 0; mask < 32; ++mask)
        {
            std::vector<PropertyStockColumn> columns{
                column(0, 20 + (mask & 1)),
                column(1, 15 + ((mask >> 1) & 1)),
            };
            const std::array<std::int32_t, 4> arrivals{
                Coordinator::UNREACHABLE,
                1,
                1,
                Coordinator::UNREACHABLE,
            };
            SequentialClassTable table;
            table.build(columns, arrivals, 1, 5);
            const std::array<SequentialClassTable, 1> classes{table};
            SequentialInstance instance{
                .horizonTurns = 5,
                .nodeColumns =
                    std::span<const PropertyStockColumn>(columns),
                .classes =
                    std::span<const SequentialClassTable>(classes),
                .rows = {
                    SequentialRow{
                        .classIndex = 0,
                        .weight = {
                            (mask & 4) ? 30 : 0,
                            (mask & 8) ? 25 : 0,
                        },
                        .ownedTurn = {1, 1},
                    },
                    SequentialRow{
                        .classIndex = 0,
                        .weight = {
                            (mask & 16) ? 28 : 0,
                            22,
                        },
                        .ownedTurn = {1, 1},
                    },
                },
                .capturedNodes = {false, false},
            };
            const MilliFunds floorValue = assignmentFloor(instance);
            const MilliFunds exhaustive =
                exhaustivePacking(instance, floorValue);
            const SequentialTierResult result =
                solveSequentialPacking(instance,
                                       floorValue,
                                       100000);
            expect(result.bookedValue == exhaustive,
                   "the completed search matches exhaustive packing");
            expect(result.relaxedValue >= result.bookedValue,
                   "the relaxed continuation value is a valid ceiling");
            expect(result.certificate || result.searchCompleted,
                   "every exhaustive sweep case closes its proof");
        }
    }

    void testRowCacheTransparencyAndBudgetFallback()
    {
        SequentialFixture fixture;
        fixture.instance.rows = {
            SequentialRow{
                .classIndex = 0,
                .weight = {100, 0, 0},
                .ownedTurn = {1, 1, 1},
            },
            SequentialRow{
                .classIndex = 0,
                .weight = {0, 90, 0},
                .ownedTurn = {1, 1, 1},
            },
        };
        const MilliFunds floorValue = assignmentFloor(fixture.instance);
        MemoryRowSource source;
        const SequentialTierResult cold =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000,
                                   &source);
        expect(source.stores > 0 && cold.cacheHits == 0,
               "a cold complete solve stores row options");
        const std::int32_t storesAfterCold = source.stores;
        const SequentialTierResult warm =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   100000,
                                   &source);
        expect(warm.bookedValue == cold.bookedValue &&
                   warm.relaxedValue == cold.relaxedValue &&
                   warm.searchCompleted == cold.searchCompleted,
               "cached continuation preserves the exact result");
        expect(warm.cacheHits > 0 &&
                   source.stores == storesAfterCold,
               "a warm solve reuses complete row enumerations");

        const SequentialTierResult coldCapped =
            solveSequentialPacking(fixture.instance, floorValue, 1);
        const SequentialTierResult warmCapped =
            solveSequentialPacking(fixture.instance,
                                   floorValue,
                                   1,
                                   &source);
        expect(warmCapped.cacheFallbacks > 0,
               "a cached row that cannot fit the cold budget recomputes");
        expect(warmCapped.bookedValue == coldCapped.bookedValue &&
                   warmCapped.searchCompleted ==
                       coldCapped.searchCompleted &&
                   warmCapped.searchStates == coldCapped.searchStates,
               "cache budget fallback is replay transparent");
    }
}

int main()
{
    testClassTableAndFirstLegBounds();
    testCanonicalOptionDeduplication();
    testEnumerationOrderAndCapturedExclusion();
    testBudgetBoundary();
    testWitnessRecoveryAndExactPacking();
    testCapturedNodeAndFloorCompatibility();
    testExhaustiveCeilingEquivalence();
    testRowCacheTransparencyAndBudgetFallback();
    return failures == 0 ? 0 : 1;
}
