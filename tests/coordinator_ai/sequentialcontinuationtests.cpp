#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
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
    using Coordinator::SequentialClassTable;
    using Coordinator::SequentialInstance;
    using Coordinator::SequentialRow;
    using Coordinator::TilePoint;
    using Coordinator::SequentialDetail::EnumerationBudget;
    using Coordinator::SequentialDetail::RowOption;
    using Coordinator::SequentialDetail::RowOptionMap;
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
}

int main()
{
    testClassTableAndFirstLegBounds();
    testCanonicalOptionDeduplication();
    testEnumerationOrderAndCapturedExclusion();
    testBudgetBoundary();
    return failures == 0 ? 0 : 1;
}
