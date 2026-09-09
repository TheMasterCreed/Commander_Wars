#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "ai/coordinator/refinementledger.h"
#include "ai/coordinator/sequentialthreshold.h"

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
    using Coordinator::SequentialCacheAccounting;
    using Coordinator::SequentialInstance;
    using Coordinator::SequentialRow;
    using Coordinator::SequentialThresholdResult;
    using Coordinator::SequentialThresholdStatus;
    using Coordinator::SequentialThresholdStop;
    using Coordinator::SequentialTierResult;
    using Coordinator::TilePoint;
    using Coordinator::solveSequentialPacking;
    using Coordinator::proveSequentialBelow;
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
    int checks = 0;

    void expect(bool condition, std::string_view message)
    {
        ++checks;
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

    class IdentityRowSource final : public SequentialRowOptionSource
    {
    public:
        explicit IdentityRowSource(std::int32_t rowCount)
        {
            setGeneration(rowCount, 0);
        }

        void setGeneration(
            std::int32_t rowCount,
            std::int32_t generation)
        {
            keys.clear();
            for (std::int32_t row = 0; row < rowCount; ++row)
            {
                keys.push_back({generation, row});
            }
        }

        bool lookup(
            std::int32_t rowIndex,
            std::span<const RowOption> & options,
            std::int64_t & statesSpent) override
        {
            const auto known = entries.find(
                keys[static_cast<std::size_t>(rowIndex)]);
            if (known == entries.end())
            {
                return false;
            }
            options = known->second.options;
            statesSpent = known->second.statesSpent;
            return true;
        }

        void store(
            std::int32_t rowIndex,
            std::span<const RowOption> options,
            std::int64_t statesSpent) override
        {
            entries.emplace(
                keys[static_cast<std::size_t>(rowIndex)],
                CachedRowEnumeration{
                    .options = std::vector<RowOption>(
                        options.begin(), options.end()),
                    .statesSpent = statesSpent,
                });
        }

        bool onceCharge(
            std::int32_t rowIndex,
            std::int64_t statesSpent,
            std::int64_t & chargedStates) const override
        {
            const auto & key =
                keys[static_cast<std::size_t>(rowIndex)];
            chargedStates =
                paid.contains(key) ? 0 : statesSpent;
            return true;
        }

        void recordCharge(std::int32_t rowIndex) override
        {
            paid.insert(
                keys[static_cast<std::size_t>(rowIndex)]);
        }

        std::int64_t quotedStates() const
        {
            std::int64_t result = 0;
            for (const auto & entry : entries)
            {
                result += entry.second.statesSpent;
            }
            return result;
        }

        std::vector<std::vector<std::int64_t>> keys;
        std::map<std::vector<std::int64_t>, CachedRowEnumeration>
            entries;
        std::set<std::vector<std::int64_t>> paid;
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

    void testCappedRetainedFloor()
    {
        SequentialFixture fixture;
        constexpr MilliFunds ROW_FLOOR = 10000;
        fixture.instance.rows = {
            SequentialRow{.classIndex = 0, .weight = {ROW_FLOOR, 0, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, ROW_FLOOR, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, 0, ROW_FLOOR}, .ownedTurn = {1, 1, 1}},
        };
        const MilliFunds floor = assignmentFloor(fixture.instance);
        const auto result = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true);
        expect(result.continuationSeen && !result.searchCompleted && result.searchStates == 1,
               "a capped continuation search remains incomplete");
        expect(result.bookedValue == floor && result.relaxedValue > floor && result.witness.empty(),
               "retaining the supplied floor does not emit a new packing witness");
        std::vector<Coordinator::SequentialRowWitness> floorWitness;
        for (std::int32_t row = 0; row < NODE_COUNT; ++row)
        {
            const WitnessStep step{row, 1};
            floorWitness.push_back(Coordinator::SequentialDetail::ownedWitness(
                fixture.instance, fixture.instance.rows[static_cast<std::size_t>(row)], row,
                std::span<const WitnessStep>(&step, 1)));
        }
        expect(replaySequentialWitness(fixture.instance, floorWitness, floor),
               "the retained floor has an independent replayable witness");
        const Coordinator::SequentialWarmStart floorSeed{floorWitness, floor};
        const auto warmFloor = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly, &floorSeed);
        expect(warmFloor.warmStartAccepted && !warmFloor.searchCompleted &&
                   warmFloor.bookedValue == floor && warmFloor.witness == floorWitness &&
                   warmFloor.relaxedValue == result.relaxedValue,
               "an equal-floor seed replaces missing witness ownership without closing its open bound");
        expect(!replaySequentialWitness(fixture.instance, result.witness, floor),
               "an absent new witness cannot substitute for the retained witness");
        const auto legacy = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true);
        const auto retained = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue);
        expect(legacy.searchCompleted && legacy.bookedValue == floor && legacy.repairValue < floor &&
                   legacy.witness.empty(),
               "legacy equality pruning retains its original scalar-only floor result");
        expect(retained.searchCompleted && retained.bookedValue == floor &&
                   replaySequentialWitness(fixture.instance, retained.witness, floor),
               "booked-value policy finds the missing equal-floor witness");
        for (std::int64_t cap = 1; cap <= retained.searchStates; ++cap)
        {
            const auto bounded = solveSequentialPacking(fixture.instance, floor, cap, nullptr, true,
                Coordinator::SequentialWitnessPolicy::BookedValue);
            expect(bounded.searchStates <= cap && bounded.bookedValue == floor && bounded.relaxedValue >= floor,
                   "equal-floor witness recovery respects every native work boundary");
            expect(!bounded.searchCompleted ||
                       replaySequentialWitness(fixture.instance, bounded.witness, bounded.bookedValue),
                   "a completed booked-value search owns a valid witness");
        }
        for (auto & row : fixture.instance.rows)
        {
            row.ownedTurn.assign(NODE_COUNT, NO_CAPTURE_TURNS);
        }
        const auto direct = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue);
        expect(direct.bookedValue == floor && replaySequentialWitness(fixture.instance, direct.witness, floor),
               "booked-value policy also witnesses a no-continuation floor");
        const Coordinator::SequentialWarmStart directSeed{direct.witness, floor};
        const auto warmDirect = solveSequentialPacking(fixture.instance, floor, 0, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly, &directSeed);
        expect(warmDirect.warmStartAccepted && warmDirect.certificate && !warmDirect.searchCompleted &&
                   warmDirect.witness == direct.witness && warmDirect.bookedValue == floor,
               "a nonzero sentinel first leg can certify its matching upper without claiming search completion");
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

    auto coreTierResult(const SequentialTierResult & result)
    {
        return std::tie(
            result.floorValue, result.relaxedValue, result.repairValue,
            result.searchValue, result.bookedValue, result.searchStates,
            result.itinerariesEnumerated, result.itineraryOptionsKept,
            result.rowsSearched, result.cacheHits, result.cacheFallbacks,
            result.rowCacheLookups, result.rowCacheQuotedStates,
            result.rowCacheChargedStates, result.rowConstructionStates,
            result.totalUniqueRowOptions, result.packingStates,
            result.packingLeaves, result.packingPrunes,
            result.packingIncumbentImprovements,
            result.unexploredPackingBranches,
            result.maximumUnexploredPackingBound, result.continuationSeen,
            result.certificate, result.searchCompleted, result.warmStartAccepted, result.warmStartSelected,
            result.rowOptionDomainsComplete, result.packingAttempted,
            result.packingResidualUpperValid, result.witness);
    }

    SequentialTierResult solveWithWarmStart(
        const SequentialInstance & instance, MilliFunds floor, std::int64_t cap,
        const Coordinator::SequentialWarmStart & seed,
        Coordinator::SequentialTierAuditMode audit = Coordinator::SequentialTierAuditMode::CountersOnly,
        SequentialRowOptionSource* source = nullptr)
    {
        return solveSequentialPacking(instance, floor, cap, source, true,
            Coordinator::SequentialWitnessPolicy::BookedValue, audit, &seed);
    }

    bool checkImprovedWarmStart(const SequentialInstance & instance, MilliFunds floor,
                               std::int64_t cap, MilliFunds exact)
    {
        const auto partial = solveSequentialPacking(instance, floor, cap, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly);
        if (partial.bookedValue <= std::max(floor, partial.repairValue) ||
            partial.bookedValue >= exact ||
            !replaySequentialWitness(instance, partial.witness, partial.bookedValue))
        {
            return false;
        }
        const Coordinator::SequentialWarmStart seed{partial.witness, partial.bookedValue};
        const auto capped = solveWithWarmStart(instance, floor, 1, seed);
        const auto advanced = solveWithWarmStart(instance, floor, 100000, seed);
        expect(capped.warmStartAccepted && capped.warmStartSelected && capped.bookedValue == partial.bookedValue &&
                   capped.witness == partial.witness && !capped.searchCompleted,
               "an interrupted retry retains a suboptimal seed above its repair floor");
        expect(advanced.warmStartAccepted && advanced.warmStartSelected && advanced.bookedValue == exact &&
                   advanced.witness != partial.witness &&
                   replaySequentialWitness(instance, advanced.witness, advanced.bookedValue),
               "funded packing improves past the warm seed and owns its new winning witness");
        return true;
    }

    void testWarmStartWitnessRetention()
    {
        std::int32_t strongerThanRepair = 0;
        std::int32_t openBounds = 0;
        std::int32_t improvedSeed = 0;
        std::int32_t acceptedRepair = 0;
        for (std::int32_t mask = 0; mask < 64; ++mask)
        {
            SequentialFixture fixture;
            fixture.instance.rows = {
                SequentialRow{.classIndex = 0,
                    .weight = {100 + (mask & 3) * 90, 40, 0},
                    .ownedTurn = {1 + ((mask >> 2) & 1), 2, 3}},
                SequentialRow{.classIndex = 0,
                    .weight = {0, 90 + ((mask >> 3) & 3) * 100, 50},
                    .ownedTurn = {2, 1, (mask & 32) ? NO_CAPTURE_TURNS : 2}},
            };
            const MilliFunds floor = assignmentFloor(fixture.instance);
            const auto exact = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true,
                Coordinator::SequentialWitnessPolicy::BookedValue,
                Coordinator::SequentialTierAuditMode::CountersOnly);
            expect((exact.certificate || exact.searchCompleted) &&
                       exact.bookedValue == exhaustivePacking(fixture.instance, floor) &&
                       replaySequentialWitness(fixture.instance, exact.witness, exact.bookedValue),
                   "warm-start fixture has an exact independently packed owned witness");
            const Coordinator::SequentialWarmStart seed{exact.witness, exact.bookedValue};
            strongerThanRepair += exact.bookedValue > std::max(floor, exact.repairValue);
            openBounds += exact.bookedValue < exact.relaxedValue;
            const auto repair = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
                Coordinator::SequentialWitnessPolicy::BookedValue,
                Coordinator::SequentialTierAuditMode::CountersOnly);
            if (repair.bookedValue == repair.repairValue && repair.bookedValue < repair.relaxedValue &&
                replaySequentialWitness(fixture.instance, repair.witness, repair.bookedValue))
            {
                const Coordinator::SequentialWarmStart repairSeed{repair.witness, repair.bookedValue};
                const auto retry = solveWithWarmStart(fixture.instance, floor, 1, repairSeed);
                expect(retry.warmStartAccepted && !retry.warmStartSelected &&
                           retry.bookedValue == repair.bookedValue &&
                           replaySequentialWitness(fixture.instance, retry.witness, retry.bookedValue),
                       "accepting a seed does not select it over an equally valuable fresh repair");
                ++acceptedRepair;
            }
            for (std::int64_t cap = 0; cap <= exact.searchStates + 1; ++cap)
            {
                if (improvedSeed == 0 && cap > 0)
                {
                    improvedSeed += checkImprovedWarmStart(fixture.instance, floor, cap, exact.bookedValue);
                }
                const auto warmed = solveWithWarmStart(fixture.instance, floor, cap, seed);
                expect(warmed.warmStartAccepted && warmed.floorValue == floor &&
                           warmed.relaxedValue == exact.relaxedValue && warmed.bookedValue == exact.bookedValue &&
                           warmed.searchStates <= cap &&
                           replaySequentialWitness(fixture.instance, warmed.witness, warmed.bookedValue),
                       "every interruption preserves the verified seed, original floor, upper and owned witness");
                expect(!warmed.certificate || warmed.bookedValue == warmed.relaxedValue,
                       "a warm lower alone cannot certify an open relaxed bound");
                if (cap == 0)
                {
                    expect(!warmed.searchCompleted && warmed.witness == exact.witness,
                           "a zero-state retry owns the seed without claiming completed search");
                }
                const auto full = solveWithWarmStart(
                    fixture.instance, floor, cap, seed, Coordinator::SequentialTierAuditMode::Full);
                expect(coreTierResult(full) == coreTierResult(warmed),
                       "warm-start proof, pruning and work are independent of diagnostic retention");
            }
            MemoryRowSource cached;
            const auto cold = solveSequentialPacking(fixture.instance, floor, 100000, &cached, true,
                Coordinator::SequentialWitnessPolicy::BookedValue,
                Coordinator::SequentialTierAuditMode::CountersOnly);
            const auto warm = solveWithWarmStart(fixture.instance, floor, 100000, seed,
                Coordinator::SequentialTierAuditMode::CountersOnly, &cached);
            expect(warm.bookedValue == cold.bookedValue && (warm.searchCompleted || warm.certificate) &&
                       warm.searchStates <= cold.searchStates,
                   "a complete cached warm search preserves exact value without adding packing states");
        }
        expect(strongerThanRepair > 0 && openBounds > 0,
               "warm-start sweep exercises improved incumbents and non-exact relaxed bounds");
        expect(improvedSeed > 0, "warm-start sweep exercises the seed-to-packing witness transition");
        expect(acceptedRepair > 0, "warm-start sweep distinguishes seed acceptance from incumbent selection");
    }

    void testWarmStartWork()
    {
        using Coordinator::sequentialWarmStartWork;
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        constexpr auto largestRecordCount = maximum / 3 - 1;
        expect(sequentialWarmStartWork(0, 0) == 3 && sequentialWarmStartWork(4, 7) == 36,
               "warm witness overhead funds initialization, replay and owned-copy passes");
        expect(sequentialWarmStartWork(4, 7) == sequentialWarmStartWork(7, 4),
               "warm witness charging includes both row and node records");
        expect(sequentialWarmStartWork(-1, 0) < 0 && sequentialWarmStartWork(0, -1) < 0,
               "negative warm witness dimensions fail closed");
        expect(sequentialWarmStartWork(largestRecordCount, 0) == maximum - maximum % 3 &&
                   sequentialWarmStartWork(largestRecordCount - 1, 1) == maximum - maximum % 3,
               "the largest representable warm witness tariff remains exact");
        expect(sequentialWarmStartWork(largestRecordCount, 1) < 0 &&
                   sequentialWarmStartWork(maximum, maximum) < 0,
               "warm witness tariff overflow cannot become a cheap reservation");
    }

    void testWarmStartRejectionAndOwnership()
    {
        enum class SeedFault
        {
            Value,
            Row,
            Node,
            Turn,
            EmptyRow,
            OutOfRange,
        };
        SequentialFixture fixture;
        fixture.instance.rows = {
            SequentialRow{.classIndex = 0, .weight = {100, 0, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, 90, 0}, .ownedTurn = {1, 1, 1}},
        };
        const MilliFunds floor = assignmentFloor(fixture.instance);
        const auto complete = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly);
        const auto cold = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly);
        expect(!complete.witness.empty(), "rejection fixture owns a sequential witness");
        if (complete.witness.empty())
        {
            return;
        }
        for (const auto fault : {SeedFault::Value, SeedFault::Row, SeedFault::Node,
                                 SeedFault::Turn, SeedFault::EmptyRow, SeedFault::OutOfRange})
        {
            auto rows = complete.witness;
            MilliFunds value = complete.bookedValue;
            switch (fault)
            {
                case SeedFault::Value:
                    ++value;
                    break;
                case SeedFault::Row:
                    rows.front().row = static_cast<std::int32_t>(fixture.instance.rows.size());
                    break;
                case SeedFault::Node:
                    rows.front().steps.front().node = fixture.instance.nodeCount();
                    break;
                case SeedFault::Turn:
                    ++rows.front().steps.front().turn;
                    break;
                case SeedFault::EmptyRow:
                    rows.front().steps.clear();
                    break;
                case SeedFault::OutOfRange:
                    value = std::numeric_limits<MilliFunds>::max();
                    break;
            }
            const Coordinator::SequentialWarmStart invalid{rows, value};
            const auto rejected = solveWithWarmStart(fixture.instance, floor, 1, invalid);
            expect(!rejected.warmStartAccepted && coreTierResult(rejected) == coreTierResult(cold),
                   "a stale or malformed seed cannot change cold search, work, bounds or witness");
        }
        auto seedRows = complete.witness;
        const Coordinator::SequentialWarmStart seed{seedRows, complete.bookedValue};
        auto owned = solveWithWarmStart(fixture.instance, floor, 0, seed);
        seedRows.clear();
        expect(owned.warmStartAccepted && owned.witness == complete.witness &&
                   replaySequentialWitness(fixture.instance, owned.witness, owned.bookedValue),
               "the returned warm witness outlives the caller's seed storage");
        const Coordinator::SequentialWarmStart preserved{complete.witness, complete.bookedValue};
        const auto legacy = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::ImprovementOnly,
            Coordinator::SequentialTierAuditMode::CountersOnly);
        const auto ignored = solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::ImprovementOnly,
            Coordinator::SequentialTierAuditMode::CountersOnly, &preserved);
        expect(coreTierResult(legacy) == coreTierResult(ignored),
               "legacy witness policy never consumes an optional warm seed");
        fixture.instance.capturedNodes[complete.witness.front().steps.front().node] = true;
        expect(!solveWithWarmStart(fixture.instance, 0, 1, preserved).warmStartAccepted,
               "a witness from a different captured-column mask is rejected");
    }

    void expectAuditEquivalent(const SequentialInstance & instance,
                              const SequentialTierResult & full,
                              const SequentialTierResult & counters,
                              Coordinator::SequentialWitnessPolicy policy = Coordinator::SequentialWitnessPolicy::BookedValue)
    {
        expect(coreTierResult(full) == coreTierResult(counters),
               "audit retention preserves every core result, counter and owned witness");
        expect(counters.nodeColumns.empty() && counters.rowPhases.empty() &&
                   counters.rowCatalogs.empty(),
               "counters-only mode retains no optional tier catalogs or row records");
        if (!counters.witness.empty() ||
            (policy == Coordinator::SequentialWitnessPolicy::BookedValue && counters.searchCompleted))
        {
            expect(replaySequentialWitness(instance, counters.witness, counters.bookedValue),
                   "reduced diagnostics preserve the replayable booked-value witness");
        }
    }

    void checkCachedAuditRetention(const SequentialInstance & instance, MilliFunds floor, std::int64_t cap,
                                  IdentityRowSource & fullSource, IdentityRowSource & countersSource,
                                  Coordinator::SequentialWitnessPolicy policy)
    {
        using Coordinator::SequentialTierAuditMode;
        const auto full = solveSequentialPacking(instance, floor, cap, &fullSource, true,
            policy, SequentialTierAuditMode::Full);
        const auto counters = solveSequentialPacking(instance, floor, cap, &countersSource, true,
            policy, SequentialTierAuditMode::CountersOnly);
        expectAuditEquivalent(instance, full, counters, policy);
        expect(fullSource.paid == countersSource.paid &&
                   fullSource.quotedStates() == countersSource.quotedStates() &&
                   fullSource.entries.size() == countersSource.entries.size(),
               "audit retention preserves cache storage, historical charges and paid keys");
    }

    bool checkAuditRetention(const SequentialInstance & instance,
                             Coordinator::SequentialWitnessPolicy policy = Coordinator::SequentialWitnessPolicy::BookedValue)
    {
        using Coordinator::SequentialTierAuditMode;
        using Coordinator::SequentialWitnessPolicy;
        constexpr std::int64_t COMPLETION_ALLOWANCE = 100000;
        const MilliFunds floor = assignmentFloor(instance);
        const auto full = solveSequentialPacking(instance, floor, COMPLETION_ALLOWANCE, nullptr, true,
            policy, SequentialTierAuditMode::Full);
        const auto counters = solveSequentialPacking(instance, floor, COMPLETION_ALLOWANCE, nullptr, true,
            policy, SequentialTierAuditMode::CountersOnly);
        expect(full.searchCompleted || full.certificate ||
                   (policy == SequentialWitnessPolicy::ImprovementOnly && full.relaxedValue == floor),
               "audit fixture completes or retains its legacy floor");
        expectAuditEquivalent(instance, full, counters, policy);
        for (std::int64_t cap = 0; cap <= full.searchStates + 1; ++cap)
        {
            const auto boundedFull = solveSequentialPacking(instance, floor, cap, nullptr, true,
                policy, SequentialTierAuditMode::Full);
            const auto boundedCounters = solveSequentialPacking(instance, floor, cap, nullptr, true,
                policy, SequentialTierAuditMode::CountersOnly);
            expectAuditEquivalent(instance, boundedFull, boundedCounters, policy);
        }

        const auto rowCount = static_cast<std::int32_t>(instance.rows.size());
        IdentityRowSource fullSource(rowCount);
        IdentityRowSource countersSource(rowCount);
        checkCachedAuditRetention(instance, floor, COMPLETION_ALLOWANCE, fullSource, countersSource, policy);
        checkCachedAuditRetention(instance, floor, COMPLETION_ALLOWANCE, fullSource, countersSource, policy);
        for (std::int64_t cap = 0; cap <= full.searchStates + 1; ++cap)
        {
            checkCachedAuditRetention(instance, floor, cap, fullSource, countersSource, policy);
        }
        return !full.rowCatalogs.empty() && !full.rowPhases.empty() &&
            !fullSource.entries.empty() && !fullSource.paid.empty();
    }

    void testAuditRetentionTransparency()
    {
        constexpr auto LEGACY_POLICY = Coordinator::SequentialWitnessPolicy::ImprovementOnly;
        SequentialFixture fixture;
        fixture.instance.rows = {
            SequentialRow{.classIndex = 0, .weight = {100, 0, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, 90, 0}, .ownedTurn = {1, 1, 1}},
        };
        bool copiedCatalog = checkAuditRetention(fixture.instance);
        bool copiedLegacyCatalog = checkAuditRetention(fixture.instance, LEGACY_POLICY);
        constexpr MilliFunds ROW_FLOOR = 10000;
        fixture.instance.rows = {
            SequentialRow{.classIndex = 0, .weight = {ROW_FLOOR, 0, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, ROW_FLOOR, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, 0, ROW_FLOOR}, .ownedTurn = {1, 1, 1}},
        };
        copiedCatalog = checkAuditRetention(fixture.instance) || copiedCatalog;
        copiedLegacyCatalog = checkAuditRetention(fixture.instance, LEGACY_POLICY) || copiedLegacyCatalog;
        fixture.instance.capturedNodes[1] = true;
        fixture.instance.rows[1].weight[1] = 0;
        copiedCatalog = checkAuditRetention(fixture.instance) || copiedCatalog;
        copiedLegacyCatalog = checkAuditRetention(fixture.instance, LEGACY_POLICY) || copiedLegacyCatalog;
        fixture.instance.capturedNodes[1] = false;
        fixture.instance.rows[1].weight[1] = ROW_FLOOR;
        for (auto & row : fixture.instance.rows)
        {
            row.ownedTurn.assign(NODE_COUNT, NO_CAPTURE_TURNS);
        }
        checkAuditRetention(fixture.instance);
        checkAuditRetention(fixture.instance, LEGACY_POLICY);
        expect(copiedCatalog, "audit equivalence exercises actual catalog copies and warm cache entries");
        expect(copiedLegacyCatalog, "legacy audit equivalence exercises catalog copies and warm cache entries");
    }

    void testSparsePackingOccupancy(
        Coordinator::SequentialPackingPolicy packingPolicy = Coordinator::SequentialPackingPolicy::LegacySuffix)
    {
        constexpr std::array<std::int32_t, NODE_COUNT> NODE_MAPPING{0, 64, 128};
        constexpr std::int32_t SPARSE_NODE_COUNT = NODE_MAPPING.back() + 1;
        constexpr std::int64_t COMPLETION_ALLOWANCE = 100000;
        SequentialFixture compact;
        compact.instance.rows = {
            SequentialRow{.classIndex = 0, .weight = {100, 0, 0}, .ownedTurn = {1, 1, 1}},
            SequentialRow{.classIndex = 0, .weight = {0, 90, 0}, .ownedTurn = {1, 1, 1}},
        };
        std::vector<PropertyStockColumn> columns;
        for (std::int32_t node = 0; node < SPARSE_NODE_COUNT; ++node)
        {
            columns.push_back(column(node, 0));
        }
        std::vector<std::int32_t> arrivals(SPARSE_NODE_COUNT * SPARSE_NODE_COUNT, Coordinator::UNREACHABLE);
        for (std::int32_t from = 0; from < NODE_COUNT; ++from)
        {
            columns[NODE_MAPPING[from]] = column(NODE_MAPPING[from], compact.columns[from].income.oursPerTurn);
            for (std::int32_t to = 0; to < NODE_COUNT; ++to)
            {
                arrivals[NODE_MAPPING[from] * SPARSE_NODE_COUNT + NODE_MAPPING[to]] =
                    compact.arrivals[from * NODE_COUNT + to];
            }
        }
        std::array<SequentialClassTable, 1> classes;
        classes[0].build(columns, arrivals, 1, HORIZON_TURNS);
        SequentialInstance sparse{
            .horizonTurns = HORIZON_TURNS,
            .nodeColumns = columns,
            .classes = classes,
            .capturedNodes = std::vector<bool>(SPARSE_NODE_COUNT, true),
        };
        for (const auto mapped : NODE_MAPPING)
        {
            sparse.capturedNodes[mapped] = false;
        }
        for (const auto & compactRow : compact.instance.rows)
        {
            SequentialRow row{
                .classIndex = compactRow.classIndex,
                .weight = std::vector<MilliFunds>(SPARSE_NODE_COUNT, 0),
                .ownedTurn = std::vector<std::int32_t>(SPARSE_NODE_COUNT, NO_CAPTURE_TURNS),
            };
            for (std::int32_t node = 0; node < NODE_COUNT; ++node)
            {
                row.weight[NODE_MAPPING[node]] = compactRow.weight[node];
                row.ownedTurn[NODE_MAPPING[node]] = compactRow.ownedTurn[node];
            }
            sparse.rows.push_back(std::move(row));
        }
        const auto floor = assignmentFloor(compact.instance);
        const auto exact = exhaustivePacking(compact.instance, floor);
        expect(assignmentFloor(sparse) == floor, "sparse occupancy fixture preserves the feasible floor");
        for (const auto policy : {Coordinator::SequentialWitnessPolicy::ImprovementOnly,
                                  Coordinator::SequentialWitnessPolicy::BookedValue})
        {
            const auto complete = solveSequentialPacking(compact.instance, floor, COMPLETION_ALLOWANCE, nullptr,
                true, policy, Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, nullptr, packingPolicy);
            expect(complete.packingAttempted && complete.packingStates > 0 && complete.bookedValue == exact,
                   "sparse occupancy fixture exercises conflicting packing branches against the oracle");
            for (std::int64_t cap = 0; cap <= complete.searchStates + 1; ++cap)
            {
                auto expected = solveSequentialPacking(compact.instance, floor, cap, nullptr, true,
                    policy, Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, nullptr, packingPolicy);
                const auto actual = solveSequentialPacking(sparse, floor, cap, nullptr, true,
                    policy, Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, nullptr, packingPolicy);
                for (auto & witness : expected.witness)
                {
                    for (auto & step : witness.steps)
                    {
                        step.node = NODE_MAPPING[step.node];
                    }
                }
                expect(coreTierResult(actual) == coreTierResult(expected),
                       "sparse occupancy preserves witnesses, backtracking, residuals and every cap boundary");
                if (!actual.witness.empty())
                {
                    expect(replaySequentialWitness(sparse, actual.witness, actual.bookedValue),
                           "sparse occupancy witness replays without overlapping resources");
                }
            }
        }
    }

    void configureThresholdFixture(SequentialFixture & fixture)
    {
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
            SequentialRow{
                .classIndex = 0,
                .weight = {0, 0, 80},
                .ownedTurn = {1, 1, 1},
            },
        };
    }

    MilliFunds populateThresholdCache(
        SequentialFixture & fixture,
        IdentityRowSource & source)
    {
        const MilliFunds floor = assignmentFloor(fixture.instance);
        const auto exact = solveSequentialPacking(
            fixture.instance, floor, 100000, &source, true);
        expect(exact.searchCompleted &&
                   exact.witness.size() > 0 &&
                   replaySequentialWitness(
                       fixture.instance,
                       exact.witness,
                       exact.bookedValue),
               "threshold fixture cache is complete and replayable");
        return exact.bookedValue;
    }

    void testTruncatedResidualUpper()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        const std::int64_t cacheWork = source.quotedStates();
        bool found = false;
        for (std::int64_t extra = 1;
             extra <= 100 && !found;
             ++extra)
        {
            const auto result = proveSequentialBelow(
                fixture.instance,
                assignmentFloor(fixture.instance),
                exact,
                cacheWork + extra,
                &source);
            found =
                result.status ==
                    SequentialThresholdStatus::
                        UnresolvedAtBudget &&
                result.rowOptionDomainsComplete &&
                result.residualUpper <
                    result.originalRelaxedUpper &&
                result.residualUpper >= exact;
        }
        expect(
            found,
            "TRUNCATED_SEARCH_RETURNS_RESIDUAL_UPPER");
    }

    void testThresholdProvenBeforeExactness()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        const auto relaxedProof = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            std::numeric_limits<MilliFunds>::max(),
            0,
            &source,
            SequentialCacheAccounting::OncePerIdentity);
        bool found = false;
        for (std::int64_t work = 1;
             work <= 100 && !found;
             ++work)
        {
            const auto result = proveSequentialBelow(
                fixture.instance,
                assignmentFloor(fixture.instance),
                exact + 1,
                work,
                &source,
                SequentialCacheAccounting::OncePerIdentity);
            found =
                result.status ==
                    SequentialThresholdStatus::
                        ProvenBelowThreshold &&
                result.thresholdProven &&
                !result.searchCompleted &&
                result.residualUpper < exact + 1;
        }
        expect(
            relaxedProof.status ==
                SequentialThresholdStatus::
                    ProvenBelowThreshold &&
                relaxedProof.thresholdProven &&
                !relaxedProof.searchCompleted &&
                relaxedProof.workUsed == 0 &&
                found,
            "THRESHOLD_PROVEN_BEFORE_EXACTNESS");
    }

    SequentialThresholdResult thresholdCounterexample(
        SequentialFixture & fixture,
        IdentityRowSource & source,
        MilliFunds exact)
    {
        return proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            100000,
            &source,
            SequentialCacheAccounting::OncePerIdentity);
    }

    void testThresholdCounterexample()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        const auto result =
            thresholdCounterexample(fixture, source, exact);
        expect(
            result.status ==
                SequentialThresholdStatus::
                    CounterexampleWitness &&
                result.feasibleLower >= exact &&
                result.witnessReplays &&
                replaySequentialWitness(
                    fixture.instance,
                    result.witness,
                    result.feasibleLower),
            "THRESHOLD_COUNTEREXAMPLE");
    }

    void testIncompleteRowCatalogFailsClosed()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        const MilliFunds floor =
            assignmentFloor(fixture.instance);
        const auto result = proveSequentialBelow(
            fixture.instance,
            floor,
            floor + 1,
            1);
        expect(
            result.status ==
                SequentialThresholdStatus::
                    UnresolvedAtBudget &&
                result.stop ==
                    SequentialThresholdStop::
                        RowOptionResidualBoundMissing &&
                !result.rowOptionDomainsComplete &&
                result.residualUpper ==
                    result.originalRelaxedUpper,
            "INCOMPLETE_ROW_CATALOG_FAILS_CLOSED");
    }

    void testResidualUpperSoundness()
    {
        bool sound = true;
        for (std::int32_t mask = 0;
             mask < 16 && sound;
             ++mask)
        {
            SequentialFixture fixture;
            configureThresholdFixture(fixture);
            fixture.instance.rows[0].weight[0] += mask & 3;
            fixture.instance.rows[1].weight[1] +=
                (mask >> 2) & 3;
            IdentityRowSource source(
                static_cast<std::int32_t>(
                    fixture.instance.rows.size()));
            const MilliFunds exact =
                populateThresholdCache(fixture, source);
            for (std::int64_t work = 0;
                 work <= 16;
                 ++work)
            {
                const auto result = proveSequentialBelow(
                    fixture.instance,
                    assignmentFloor(fixture.instance),
                    exact,
                    work,
                    &source,
                    SequentialCacheAccounting::
                        OncePerIdentity);
                sound =
                    result.status !=
                        SequentialThresholdStatus::Invalid &&
                    result.feasibleLower <= exact &&
                    result.residualUpper >= exact &&
                    result.witnessReplays &&
                    replaySequentialWitness(
                        fixture.instance,
                        result.witness,
                        result.feasibleLower);
                if (!sound)
                {
                    break;
                }
            }
        }
        expect(sound, "RESIDUAL_UPPER_SOUNDNESS");
    }

    void testMonotoneAnytimeBounds()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        MilliFunds previousLower =
            std::numeric_limits<MilliFunds>::min();
        MilliFunds previousUpper =
            std::numeric_limits<MilliFunds>::max();
        bool monotone = true;
        for (std::int64_t work = 0;
             work <= 100;
             ++work)
        {
            const auto result = proveSequentialBelow(
                fixture.instance,
                assignmentFloor(fixture.instance),
                exact,
                work,
                &source,
                SequentialCacheAccounting::OncePerIdentity);
            monotone =
                monotone &&
                result.feasibleLower >= previousLower &&
                result.residualUpper <= previousUpper;
            previousLower = result.feasibleLower;
            previousUpper = result.residualUpper;
        }
        expect(monotone, "MONOTONE_ANYTIME_BOUNDS");
    }

    void testDeterministicBestBoundOrder()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource firstSource(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        IdentityRowSource secondSource(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, firstSource);
        populateThresholdCache(fixture, secondSource);
        const auto first = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            8,
            &firstSource,
            SequentialCacheAccounting::OncePerIdentity);
        const auto second = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            8,
            &secondSource,
            SequentialCacheAccounting::OncePerIdentity);
        expect(
            first.status == second.status &&
                first.feasibleLower == second.feasibleLower &&
                first.residualUpper == second.residualUpper &&
                first.statesUsed == second.statesUsed &&
                first.expansionOrderHash ==
                    second.expansionOrderHash &&
                first.witness == second.witness,
            "DETERMINISTIC_BEST_BOUND_ORDER");
    }

    void testCacheIdentityAndOnceCharge()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        const std::int64_t quoted = source.quotedStates();
        const auto once = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            1,
            &source,
            SequentialCacheAccounting::OncePerIdentity);
        source.paid.clear();
        const auto recharge = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            quoted + 1,
            &source,
            SequentialCacheAccounting::
                RechargeEveryLookup);
        source.setGeneration(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()),
            1);
        const auto incompatible = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            1,
            &source,
            SequentialCacheAccounting::OncePerIdentity);
        expect(
            once.cacheQuotedStates == quoted &&
                once.cacheChargedStates == 0 &&
                recharge.cacheChargedStates == quoted &&
                incompatible.stop ==
                    SequentialThresholdStop::
                        RowOptionResidualBoundMissing &&
                incompatible.rowConstructionStates > 0,
            "CACHE_IDENTITY_AND_ONCE_CHARGE");
    }

    void testNoActionKindSemantics()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        const std::array<std::string_view, 4> labels{
            "WAIT", "MOVE", "FIRE", "CAPTURE"
        };
        std::optional<SequentialThresholdResult> baseline;
        bool identical = true;
        for (const auto label : labels)
        {
            (void)label;
            const auto result = proveSequentialBelow(
                fixture.instance,
                assignmentFloor(fixture.instance),
                exact,
                8,
                &source,
                SequentialCacheAccounting::OncePerIdentity);
            if (!baseline)
            {
                baseline = result;
            }
            else
            {
                identical =
                    identical &&
                    result.status == baseline->status &&
                    result.feasibleLower ==
                        baseline->feasibleLower &&
                    result.residualUpper ==
                        baseline->residualUpper &&
                    result.expansionOrderHash ==
                        baseline->expansionOrderHash;
            }
        }
        expect(identical, "NO_ACTION_KIND_SEMANTICS");
    }

    void testCostAuditTransparency()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        const auto floor = assignmentFloor(fixture.instance);
        const auto complete = solveSequentialPacking(fixture.instance, floor, 100000, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue, Coordinator::SequentialTierAuditMode::CountersOnly);
        bool conflicts = false;
        bool truncatedTail = false;
        for (std::int64_t cap = 0; cap <= complete.searchStates + 1; ++cap)
        {
            const auto expected = solveSequentialPacking(fixture.instance, floor, cap, nullptr, true,
                Coordinator::SequentialWitnessPolicy::BookedValue, Coordinator::SequentialTierAuditMode::CountersOnly);
            for (const bool countOperations : {false, true})
            {
                Coordinator::SequentialCostAudit audit{.countOperations = countOperations};
                const auto measured = solveSequentialPacking(fixture.instance, floor, cap, nullptr, true,
                    Coordinator::SequentialWitnessPolicy::BookedValue,
                    Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, &audit);
                expect(coreTierResult(expected) == coreTierResult(measured),
                       "native cost instrumentation preserves every result, witness and work boundary");
                expect(audit.solves == 1 && !audit.overflow &&
                           audit.relaxationRepairNanos >= 0 && audit.rowOptionsNanos >= 0 &&
                           audit.packingNanos >= 0 && audit.finalWitnessNanos >= 0,
                       "cost phases remain nonnegative without assuming timer resolution");
                expect(audit.optionsVisited == audit.boundRejectedOptions + audit.conflictingOptions +
                           audit.admittedOptions + audit.refusedOptions,
                       "every tested packing option has exactly one recorded outcome");
                expect(countOperations || (audit.packingCalls == 0 && audit.optionsVisited == 0 &&
                           audit.occupancyProbes == 0 && audit.optionsAfterTruncation == 0),
                       "timing-only measurement does not collect inner-loop operations");
                conflicts = conflicts || audit.conflictingOptions > 0;
                truncatedTail = truncatedTail || audit.optionsAfterTruncation > 0;
            }
        }
        expect(conflicts && truncatedTail, "cost fixture exposes conflict rejection and work after truncation");
        Coordinator::SequentialCostAudit accumulated;
        solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, &accumulated);
        solveSequentialPacking(fixture.instance, floor, 1, nullptr, true,
            Coordinator::SequentialWitnessPolicy::BookedValue,
            Coordinator::SequentialTierAuditMode::CountersOnly, nullptr, &accumulated);
        expect(accumulated.solves == 2 && !accumulated.overflow, "cost records accumulate across native retries");
        accumulated.solves = std::numeric_limits<std::uint64_t>::max();
        accumulated.count(accumulated.solves);
        expect(accumulated.overflow && accumulated.solves == std::numeric_limits<std::uint64_t>::max(),
               "diagnostic counter overflow is reported without wrapping");
    }

    void testCompatibleRowBound()
    {
        using Coordinator::SequentialDetail::compatibleRowUpper;
        using Coordinator::SequentialDetail::PackedRow;
        std::array<std::vector<RowOption>, 2> options{{
            {{{0, 64}, 100, {}}, {{128}, 40, {}}},
            {{{64}, 90, {}}, {{129}, 30, {}}},
        }};
        const std::array<PackedRow, 2> rows{{
            {0, options[0], 100}, {1, options[1], 90},
        }};
        std::vector<std::uint8_t> used(130, 0);
        auto unlimited = [] { return true; };
        expect(compatibleRowUpper(rows, 0, 10, used, unlimited) == 200,
               "compatible row upper permits conflicts among still-unselected rows");
        used[64] = 1;
        expect(compatibleRowUpper(rows, 0, 10, used, unlimited) == 80,
               "compatible row upper removes every option conflicting with the fixed prefix");
        used[128] = 1;
        expect(compatibleRowUpper(rows, 0, 10, used, unlimited) == 40,
               "an exhausted row still permits its empty choice beyond resource 64");
        expect(compatibleRowUpper(rows, 2, 10, used, unlimited) == 10,
               "a fully selected prefix needs no remaining-row contribution");
        std::int64_t fullWork = 0;
        const auto complete = compatibleRowUpper(rows, 0, 10, used, [&] {
            ++fullWork;
            return true;
        });
        expect(fullWork == 11 && complete == 40,
               "bound tariff counts two rows, four options and five occupancy probes");
        for (std::int64_t cap = 0; cap <= fullWork; ++cap)
        {
            std::int64_t paid = 0;
            const auto partial = compatibleRowUpper(rows, 0, 10, used, [&] {
                if (paid == cap)
                {
                    return false;
                }
                ++paid;
                return true;
            });
            expect(paid == cap && partial.has_value() == (cap == fullWork),
                   "a partially scanned compatible bound cannot authorize pruning");
        }
        options[0][1].value = std::numeric_limits<MilliFunds>::max();
        options[0][0].value = std::numeric_limits<MilliFunds>::max();
        used[128] = 0;
        expect(!compatibleRowUpper(rows, 0, 10, used, unlimited),
               "compatible upper addition overflow produces no bound");
        expect(!compatibleRowUpper(rows, 3, 0, used, unlimited) &&
                   !compatibleRowUpper(rows, 0, -1, used, unlimited),
               "invalid compatible-bound prefix is unavailable");
        options[0][0].nodes = {130};
        expect(!compatibleRowUpper(rows, 0, 0, used, unlimited),
               "out-of-domain occupancy cannot produce a compatible upper");
    }

    void testCompatiblePackingBoundaries()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        const auto floor = assignmentFloor(fixture.instance);
        const auto exact = exhaustivePacking(fixture.instance, floor);
        Coordinator::SequentialCostAudit audit{.countOperations = true};
        MemoryRowSource source;
        const auto complete = solveSequentialPacking(fixture.instance, floor, 100000, &source, true,
            Coordinator::SequentialWitnessPolicy::BookedValue, Coordinator::SequentialTierAuditMode::CountersOnly,
            nullptr, &audit, Coordinator::SequentialPackingPolicy::CompatibleRows);
        expect(complete.searchCompleted && complete.bookedValue == exact && audit.compatibleBoundCalls > 0 &&
                   audit.compatibleBoundPrunes > 0 && audit.compatibleBoundWork > 0,
               "charged compatible-row pruning preserves the complete packing optimum");
        const Coordinator::SequentialWarmStart seed{complete.witness, complete.bookedValue};
        for (std::int64_t cap = 0; cap <= complete.searchStates + 1; ++cap)
        {
            for (const auto* warm : {static_cast<const Coordinator::SequentialWarmStart*>(nullptr), &seed})
            {
                Coordinator::SequentialCostAudit measured{.countOperations = true};
                const auto result = solveSequentialPacking(fixture.instance, floor, cap, &source, true,
                    Coordinator::SequentialWitnessPolicy::BookedValue,
                    Coordinator::SequentialTierAuditMode::CountersOnly, warm, &measured,
                    Coordinator::SequentialPackingPolicy::CompatibleRows);
                const auto timing = solveSequentialPacking(fixture.instance, floor, cap, &source, true,
                    Coordinator::SequentialWitnessPolicy::BookedValue,
                    Coordinator::SequentialTierAuditMode::CountersOnly, warm, nullptr,
                    Coordinator::SequentialPackingPolicy::CompatibleRows);
                expect(result.searchStates <= cap && result.bookedValue <= exact && result.relaxedValue >= exact &&
                           (!result.searchCompleted || result.bookedValue == exact) &&
                           coreTierResult(result) == coreTierResult(timing),
                       "compatible packing interruptions and instrumentation preserve proof boundaries");
                expect((result.witness.empty() && !result.searchCompleted && !warm) ||
                           replaySequentialWitness(fixture.instance, result.witness, result.bookedValue),
                       "cold and warm compatible packing retain owned replayable lower witnesses");
                expect(!result.packingResidualUpperValid ||
                           std::max(result.bookedValue, result.maximumUnexploredPackingBound) >= exact,
                       "interrupted compatible pruning leaves every unexplored branch covered");
            }
        }
    }

    void testShared160000Accounting()
    {
        SequentialFixture fixture;
        configureThresholdFixture(fixture);
        IdentityRowSource source(
            static_cast<std::int32_t>(
                fixture.instance.rows.size()));
        const MilliFunds exact =
            populateThresholdCache(fixture, source);
        source.paid.clear();
        constexpr std::int64_t total = 160000;
        constexpr std::int64_t tariff = 10014;
        Coordinator::RefinementLedger ledger;
        ledger.open(total);
        const auto grant = ledger.draw(
            total, Coordinator::RefinementWork::SearchSlice);
        const auto result = proveSequentialBelow(
            fixture.instance,
            assignmentFloor(fixture.instance),
            exact,
            total - tariff,
            &source,
            SequentialCacheAccounting::OncePerIdentity,
            Coordinator::SequentialThresholdOptions{
                .pairPartitionUpper = true,
                .nodePriceDual = true,
            });
        const bool refunded = ledger.refund(
            grant,
            total - tariff - result.workUsed);
        expect(
            refunded &&
                ledger.stats().spent() ==
                    tariff + result.workUsed &&
                ledger.stats().spent() <= total,
            "SHARED_160000_ACCOUNTING");
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
    testCappedRetainedFloor();
    testRowCacheTransparencyAndBudgetFallback();
    testWarmStartWitnessRetention();
    testWarmStartWork();
    testWarmStartRejectionAndOwnership();
    testAuditRetentionTransparency();
    testSparsePackingOccupancy();
    testTruncatedResidualUpper();
    testThresholdProvenBeforeExactness();
    testThresholdCounterexample();
    testIncompleteRowCatalogFailsClosed();
    testResidualUpperSoundness();
    testMonotoneAnytimeBounds();
    testDeterministicBestBoundOrder();
    testCacheIdentityAndOnceCharge();
    testNoActionKindSemantics();
    testCostAuditTransparency();
    testCompatibleRowBound();
    testCompatiblePackingBoundaries();
    testSparsePackingOccupancy(Coordinator::SequentialPackingPolicy::CompatibleRows);
    testShared160000Accounting();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
