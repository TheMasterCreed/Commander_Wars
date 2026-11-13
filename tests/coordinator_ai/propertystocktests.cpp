#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/masterplanpricing.h"
#include "ai/coordinator/propertystock.h"
#include "ai/coordinator/propertystockbuilder.h"

namespace Coordinator
{
    struct PropertyStockDiagnosticFixture
    {
        static constexpr std::int32_t HORIZON = 8;
        static constexpr std::int32_t WIDTH = 3;
        static constexpr std::int32_t POINTS = 4;
        static constexpr std::int32_t RATE = 2;

        struct ReplyKeys
        {
            CanonicalPlanActionKey today;
            CanonicalPlanActionKey damaged;
            CanonicalPlanActionKey dead;
        };

        static ReplyKeys replyKeys()
        {
            return ReplyKeys{
                .today = CanonicalPlanActionKey::fromAction(0, 0, 0, true),
                .damaged = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, 1, false),
                // Destruction also removes a capture-rate bonus that would remain after hp loss.
                .dead = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, 1, true),
            };
        }

        struct Snapshot
        {
            PropertyStockField value;
            std::array<unsigned char, sizeof(PropertyStockField)> bytes;
        };

        static PropertyStockField make(std::int32_t rowCount = 1, std::int32_t carried = 0)
        {
            PropertyStockField field;
            field.m_horizonTurns = HORIZON;
            field.m_width = WIDTH;
            field.m_height = 1;
            field.m_capturePointsToCapture = POINTS;
            field.m_columnAtTile = {0, NO_STOCK_COLUMN, 1};
            field.m_propertyAtTile = {0, NO_PROPERTY_INDEX, 1};
            field.m_columnTiles = {{0, 0}, {WIDTH - 1, 0}};
            field.m_columnCapturer = {0, NO_UNIT};
            field.m_columnCapturePoints = {carried, 0};
            field.m_ours.columns = {
                PropertyStockColumn{.slot = 0, .tile = field.m_columnTiles[0], .income = {2, 2}},
                PropertyStockColumn{.slot = 1, .tile = field.m_columnTiles[1], .income = {1, 1}},
            };
            MobilityCostGrid grid(WIDTH, 1);
            for (std::int32_t x = 0; x < WIDTH; ++x)
            {
                grid.setCost(x, 0, EntryDirection::East, 1);
                grid.setCost(x, 0, EntryDirection::West, 1);
            }
            field.m_grids.push_back(std::move(grid));
            for (std::int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
            {
                const TilePoint tile{rowIndex == 0 ? (carried > 0 ? 0 : 1) : WIDTH - 1, 0};
                field.m_ours.rows.push_back(PropertyStockRow{
                    .knowledgeIndex = rowIndex, .gridIdentity = 0, .movementPoints = 1,
                    .capturePoints = carried, .captureRate = RATE, .tile = tile,
                });
                field.arrivalsFrom(0, 1, tile);
                constexpr std::array<MilliFunds, 2> CENTER_WEIGHTS{12, 6};
                constexpr std::array<MilliFunds, 2> CARRIED_WEIGHTS{14, 5};
                constexpr std::array<MilliFunds, 2> SECOND_WEIGHTS{10, 6};
                const auto & weights = rowIndex == 0 ? (carried > 0 ? CARRIED_WEIGHTS : CENTER_WEIGHTS) : SECOND_WEIGHTS;
                field.m_ours.weights.insert(field.m_ours.weights.end(), weights.begin(), weights.end());
            }
            field.m_marginals.emplace(PropertyStockField::MarginalKey{0, NO_STOCK_COLUMN},
                                      buildMarginalTable(field.m_ours, 0, NO_STOCK_COLUMN));
            return field;
        }

        static Snapshot snapshot(const PropertyStockField & field)
        {
            Snapshot result{field, {}};
            std::memcpy(result.bytes.data(), &field, sizeof(field));
            return result;
        }

        static bool unchanged(const PropertyStockField & field, const Snapshot & before)
        {
            const auto & old = before.value;
            if (std::memcmp(before.bytes.data(), &field, sizeof(field)) != 0 ||
                field.m_ours.rows != old.m_ours.rows || field.m_ours.columns != old.m_ours.columns ||
                field.m_ours.weights != old.m_ours.weights || field.m_theirs.rows != old.m_theirs.rows ||
                field.m_theirs.columns != old.m_theirs.columns || field.m_theirs.weights != old.m_theirs.weights ||
                field.m_grids != old.m_grids || field.m_columnAtTile != old.m_columnAtTile ||
                field.m_propertyAtTile != old.m_propertyAtTile || field.m_columnTiles != old.m_columnTiles ||
                field.m_columnCapturer != old.m_columnCapturer || field.m_columnCapturePoints != old.m_columnCapturePoints ||
                field.m_enemyArrivals != old.m_enemyArrivals || field.m_enemyOptimaWithoutRow != old.m_enemyOptimaWithoutRow ||
                field.m_arrivals.size() != old.m_arrivals.size() || field.m_marginals.size() != old.m_marginals.size())
            {
                return false;
            }
            for (const auto & [key, value] : field.m_arrivals)
            {
                const auto found = old.m_arrivals.find(key);
                if (found == old.m_arrivals.end() || value.activations != found->second.activations)
                {
                    return false;
                }
            }
            for (const auto & [key, value] : field.m_marginals)
            {
                const auto found = old.m_marginals.find(key);
                if (found == old.m_marginals.end() || value.unmatched != found->second.unmatched ||
                    value.withoutColumn != found->second.withoutColumn || value.solves != found->second.solves ||
                    value.augmentations != found->second.augmentations)
                {
                    return false;
                }
            }
            for (std::int32_t x = 0; x < WIDTH; ++x)
            {
                if (field.m_reach.activations(x, 0) != old.m_reach.activations(x, 0) ||
                    field.m_reach.routeCost(x, 0) != old.m_reach.routeCost(x, 0))
                {
                    return false;
                }
            }
            return true;
        }
    };
}

namespace
{
    using Coordinator::AssignmentSolver;
    using Coordinator::MarginalTable;
    using Coordinator::MilliFunds;
    using Coordinator::NO_CAPTURE_TURNS;
    using Coordinator::NO_STOCK_COLUMN;
    using Coordinator::NO_STOCK_ROW;
    using Coordinator::OwnerSign;
    using Coordinator::PropertyIncome;
    using Coordinator::PropertyStockColumn;
    using Coordinator::PropertyStockHolding;
    using Coordinator::PropertyStockInstance;
    using Coordinator::PropertyStockRow;
    using Coordinator::TilePoint;

    constexpr std::int32_t HORIZON_TURNS = 10;
    constexpr std::int32_t POINTS_TO_CAPTURE = 20;
    constexpr std::int32_t FULL_RATE = 10;
    constexpr MilliFunds INCOME_PER_TURN = Coordinator::toMilliFunds(1000);
    constexpr PropertyIncome SYMMETRIC_INCOME{
        .oursPerTurn = INCOME_PER_TURN,
        .enemyPerTurn = INCOME_PER_TURN,
    };
    constexpr std::uint64_t RANDOM_MULTIPLIER = 6364136223846793005ULL;
    constexpr std::uint64_t RANDOM_INCREMENT = 1442695040888963407ULL;
    constexpr Coordinator::DiagnosticRefinementLimits SEQUENTIAL_ONLY{
        .stateCap = Coordinator::SEQUENTIAL_SEARCH_STATE_CAP,
    };
    constexpr std::int64_t TRUNCATING_STATE_CAP = 1;
    constexpr Coordinator::DiagnosticRefinementLimits TRUNCATED_SEQUENTIAL_ONLY{.stateCap = TRUNCATING_STATE_CAP};
    constexpr Coordinator::DiagnosticRefinementLimits TRUNCATED_WITH_DUAL{
        .stateCap = TRUNCATING_STATE_CAP,
        .dualRounds = Coordinator::MASTER_PLAN_PRICING_ROUNDS,
        .dualWork = Coordinator::MASTER_PLAN_PRICING_WORK,
        .dualWitnessWork = Coordinator::MASTER_PLAN_PRICING_WITNESS_WORK,
    };

    int failures = 0;
    int checks = 0;

    void expect(bool condition, std::string_view message)
    {
        ++checks;
        if (!condition)
        {
            std::printf("FAILED: %.*s\n", static_cast<int>(message.size()), message.data());
            ++failures;
        }
    }

    class SeededRandom
    {
    public:
        explicit SeededRandom(std::uint64_t seed)
            : m_state(seed)
        {
        }

        std::int32_t bounded(std::int32_t bound)
        {
            m_state = m_state * RANDOM_MULTIPLIER + RANDOM_INCREMENT;
            return static_cast<std::int32_t>((m_state >> 33U) % static_cast<std::uint64_t>(bound));
        }

    private:
        std::uint64_t m_state;
    };

    MilliFunds bruteForce(std::span<const MilliFunds> weights, std::int32_t rowCount, std::int32_t columnCount,
                          std::int32_t row, std::vector<bool> & used)
    {
        if (row == rowCount)
        {
            return 0;
        }
        MilliFunds best = bruteForce(weights, rowCount, columnCount, row + 1, used);
        for (std::int32_t column = 0; column < columnCount; ++column)
        {
            if (used[static_cast<std::size_t>(column)])
            {
                continue;
            }
            used[static_cast<std::size_t>(column)] = true;
            const std::size_t offset = static_cast<std::size_t>(row * columnCount + column);
            best = std::max(best, weights[offset] +
                                     bruteForce(weights, rowCount, columnCount, row + 1, used));
            used[static_cast<std::size_t>(column)] = false;
        }
        return best;
    }

    MilliFunds bruteForce(std::span<const MilliFunds> weights, std::int32_t rowCount, std::int32_t columnCount)
    {
        std::vector<bool> used(static_cast<std::size_t>(columnCount), false);
        return bruteForce(weights, rowCount, columnCount, 0, used);
    }

    PropertyStockInstance stockInstance(std::int32_t rowCount, std::int32_t columnCount,
                                        const std::vector<MilliFunds> & weights)
    {
        PropertyStockInstance instance;
        instance.weights = weights;
        for (std::int32_t row = 0; row < rowCount; ++row)
        {
            instance.rows.push_back(PropertyStockRow{.knowledgeIndex = row});
        }
        for (std::int32_t column = 0; column < columnCount; ++column)
        {
            instance.columns.push_back(PropertyStockColumn{
                .slot = column,
                .tile = TilePoint{column, 0},
            });
        }
        return instance;
    }

    std::vector<MilliFunds> withoutColumn(std::span<const MilliFunds> weights, std::int32_t rowCount,
                                          std::int32_t columnCount, std::int32_t removed)
    {
        std::vector<MilliFunds> reduced;
        reduced.reserve(static_cast<std::size_t>(rowCount * std::max(columnCount - 1, 0)));
        for (std::int32_t row = 0; row < rowCount; ++row)
        {
            for (std::int32_t column = 0; column < columnCount; ++column)
            {
                if (column != removed)
                {
                    reduced.push_back(weights[static_cast<std::size_t>(row * columnCount + column)]);
                }
            }
        }
        return reduced;
    }

    void testSolverMatchesBruteForce()
    {
        SeededRandom random(0x5cf1a2b3d4e5f607ULL);
        for (std::int32_t sample = 0; sample < 400; ++sample)
        {
            const std::int32_t rows = random.bounded(5);
            const std::int32_t columns = random.bounded(5);
            std::vector<MilliFunds> weights(static_cast<std::size_t>(rows * columns));
            for (MilliFunds & weight : weights)
            {
                weight = random.bounded(17) - 4;
            }
            AssignmentSolver solver;
            solver.solve(rows, columns, weights);
            expect(solver.optimum() == bruteForce(weights, rows, columns),
                   "assignment solver matches exhaustive search");
        }
    }

    void testColumnWithdrawalIsExact()
    {
        const std::vector<MilliFunds> weights{
            11, 10, 0,
            10, 0, 9,
            0, 8, 7,
        };
        AssignmentSolver solver;
        solver.solve(3, 3, weights);
        for (std::int32_t column = 0; column < 3; ++column)
        {
            const std::vector<MilliFunds> reduced = withoutColumn(weights, 3, 3, column);
            AssignmentSolver oracle;
            oracle.solve(3, 2, reduced);
            expect(solver.optimumWithoutColumn(column) == oracle.optimum(),
                   "column withdrawal restores the exact reduced optimum");
        }
    }

    void testMarginalTableMatchesFreshSolves()
    {
        SeededRandom random(0x7fdc91a430e2b865ULL);
        for (std::int32_t sample = 0; sample < 80; ++sample)
        {
            constexpr std::int32_t ROWS = 4;
            constexpr std::int32_t COLUMNS = 4;
            std::vector<MilliFunds> weights(ROWS * COLUMNS);
            for (MilliFunds & weight : weights)
            {
                weight = random.bounded(23);
            }
            const PropertyStockInstance instance = stockInstance(ROWS, COLUMNS, weights);
            for (std::int32_t actingRow = 0; actingRow < ROWS; ++actingRow)
            {
                const MarginalTable incremental =
                    Coordinator::buildMarginalTable(instance, actingRow, NO_STOCK_COLUMN);
                const MarginalTable oracle =
                    Coordinator::buildMarginalTableFromScratch(instance, actingRow, NO_STOCK_COLUMN);
                expect(incremental.unmatched == oracle.unmatched &&
                           incremental.withoutColumn == oracle.withoutColumn,
                       "incremental marginal table matches fresh solves");
            }
        }
    }

    void testCaptureClockAndHorizon()
    {
        const PropertyStockColumn column{
            .slot = 0,
            .tile = TilePoint{1, 1},
            .income = SYMMETRIC_INCOME,
            .ownerBefore = OwnerSign::Neutral,
        };
        expect(Coordinator::ownedTurnsUntil(1, 0, FULL_RATE, POINTS_TO_CAPTURE, HORIZON_TURNS) == 2,
               "arrival one plus two capture activations owns from turn two");
        expect(Coordinator::ownedTurnsUntil(3, 0, FULL_RATE, POINTS_TO_CAPTURE, HORIZON_TURNS) == 4,
               "walking activations advance the ownership clock");
        expect(Coordinator::ownedTurnsUntil(1, 10, FULL_RATE, POINTS_TO_CAPTURE, HORIZON_TURNS) == 1,
               "carried capture points shorten the clock");
        expect(Coordinator::ownedTurnsUntil(Coordinator::UNREACHABLE, 0, FULL_RATE,
                                            POINTS_TO_CAPTURE, HORIZON_TURNS) == NO_CAPTURE_TURNS,
               "unreachable arrivals never fabricate ownership");
        expect(Coordinator::ownedTurnsUntil(1, 0, 0, POINTS_TO_CAPTURE, HORIZON_TURNS) == NO_CAPTURE_TURNS,
               "zero capture rate never fabricates ownership");
        expect(Coordinator::columnWeight(column, {1, 0, FULL_RATE}, POINTS_TO_CAPTURE, HORIZON_TURNS) ==
                   INCOME_PER_TURN * 8,
               "column value spans every turn after ownership");
        expect(Coordinator::columnWeight(column, {9, 0, FULL_RATE}, POINTS_TO_CAPTURE, HORIZON_TURNS) == 0,
               "ownership at the horizon has no continuation");
    }

    void testOwnershipAndMirrorIdentities()
    {
        constexpr PropertyIncome asymmetric{
            .oursPerTurn = Coordinator::toMilliFunds(1200),
            .enemyPerTurn = Coordinator::toMilliFunds(800),
        };
        const std::vector<PropertyStockHolding> holdings{
            {asymmetric, OwnerSign::Ours},
            {asymmetric, OwnerSign::Enemy},
            {asymmetric, OwnerSign::Neutral},
        };
        expect(Coordinator::ownedBaseline(holdings, HORIZON_TURNS) ==
                   (asymmetric.oursPerTurn - asymmetric.enemyPerTurn) * HORIZON_TURNS,
               "owned baseline prices the whole horizon at each current owner");
        const PropertyStockColumn enemy{
            .slot = 0,
            .tile = TilePoint{2, 2},
            .income = asymmetric,
            .ownerBefore = OwnerSign::Enemy,
        };
        const PropertyStockColumn mirrored = Coordinator::mirroredColumn(enemy);
        expect(mirrored.income == asymmetric.mirrored() && mirrored.ownerBefore == OwnerSign::Ours,
               "column mirroring swaps income and owner sign together");
        expect(Coordinator::ownershipFlipSwing(enemy, HORIZON_TURNS) ==
                   (asymmetric.oursPerTurn + asymmetric.enemyPerTurn) * HORIZON_TURNS,
               "ownership flip moves the full horizon baseline");
        expect(Coordinator::ownershipFlipSwing(Coordinator::capturedColumn(enemy), HORIZON_TURNS) == 0,
               "an already captured column has no remaining ownership swing");
    }

    void testInjectivityAndPositionalDelta()
    {
        const PropertyStockInstance contested = stockInstance(2, 1, {100, 90});
        expect(Coordinator::instanceOptimum(contested) == 100,
               "one property column cannot be claimed by two rows");
        const PropertyStockInstance instance = stockInstance(3, 3, {
            100, 0, 0,
            0, 90, 0,
            0, 0, 80,
        });
        const MarginalTable marginal = Coordinator::buildMarginalTable(instance, 0, NO_STOCK_COLUMN);
        expect(marginal.unmatched == 170, "removing the acting row leaves the other assignments standing");
        expect(Coordinator::positionalOptimum(marginal, std::vector<MilliFunds>{0, 95, 0}, NO_STOCK_COLUMN) ==
                   175,
               "acting row is priced against the matching margin");
        expect(Coordinator::positionalOptimum(marginal, std::vector<MilliFunds>{0, 0, 0}, NO_STOCK_COLUMN) ==
                   marginal.unmatched,
               "a dominated acting row does not lower the answer");
    }

    Coordinator::MobilityCostGrid straightCorridor(std::int32_t length, std::int32_t stepCost)
    {
        Coordinator::MobilityCostGrid grid(length + 1, 1);
        for (std::int32_t x = 1; x <= length; ++x)
        {
            grid.setCost(x, 0, Coordinator::EntryDirection::East, stepCost);
        }
        return grid;
    }

    void testActivationScheduleUsesTurnBoundaries()
    {
        constexpr TilePoint origin{0, 0};
        constexpr TilePoint near{1, 0};
        constexpr TilePoint target{3, 0};
        const Coordinator::MobilityCostGrid grid = straightCorridor(3, 3);
        Coordinator::ActivationField field;
        field.build(grid, origin, 5, Coordinator::UNBOUNDED_ACTIVATIONS, {origin, near, target});
        expect(field.activations(origin.x, origin.y) == 1, "origin is reachable on the current activation");
        expect(field.activations(near.x, near.y) == 1, "one affordable step stays on the current activation");
        expect(field.activations(target.x, target.y) == 3,
               "movement allowance resets determine arrival activations");

        const Coordinator::MobilityCostGrid blocked = straightCorridor(1, 6);
        Coordinator::ActivationField unreachable;
        unreachable.build(blocked, origin, 5, Coordinator::UNBOUNDED_ACTIVATIONS, {near});
        expect(unreachable.activations(near.x, near.y) == Coordinator::UNREACHABLE,
               "a tile costing more than one allowance is unreachable");
    }

    void stockFieldDiagnosticChecks()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto zero = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, Fixture::RATE, false);
        const std::array keys{ContinuationKey{today}, ContinuationKey{damaged}, ContinuationKey{dead},
                              ContinuationKey{zero}, ContinuationKey{today}};
        const auto field = Fixture::make();
        const auto before = Fixture::snapshot(field);
        const auto batch = JointPlanStockValuer::diagnosticQuotes(field, keys, SEQUENTIAL_ONLY);
        constexpr std::size_t UNIQUE_ACTIONS = 4;
        constexpr std::size_t UNIQUE_CLASSES = 3;
        constexpr MilliFunds TODAY_FLOOR = 18;
        constexpr MilliFunds TODAY_CEILING = 20;
        constexpr MilliFunds DAMAGED_VALUE = 13;
        expect(batch.quotes.size() == keys.size() && batch.actions == UNIQUE_ACTIONS &&
               batch.classesBuilt == UNIQUE_CLASSES && batch.nanos >= 0,
               "field diagnostic builds only the union of actions and effective-rate classes");
        expect(batch.limits.stateCap == SEQUENTIAL_SEARCH_STATE_CAP && batch.limits.dualRounds == 0 &&
               batch.quotes[0].cheapOurs.lower == TODAY_FLOOR && batch.quotes[0].cheapOurs.upper == TODAY_CEILING &&
               batch.quotes[1].cheapOurs.lower == DAMAGED_VALUE && batch.quotes[1].cheapOurs.upper == DAMAGED_VALUE,
               "field diagnostic decodes damage and slows both the unfinished and second capture");
        for (const auto & quote : batch.quotes)
        {
            expect(quote.valid && quote.witnessReplays, "field diagnostic witnesses replay");
        }
        expect(batch.quotes[2].ours.upper == 0 && batch.quotes[2].witness.empty() &&
               batch.quotes[2].owned == 0 && batch.quotes[3].ours.upper == 0,
               "destroyed unfinished and zero-rate rows have no future contribution");
        auto reversed = keys;
        std::reverse(reversed.begin(), reversed.end());
        const auto other = JointPlanStockValuer::diagnosticQuotes(field, reversed, SEQUENTIAL_ONLY);
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const auto & quote = other.quotes[keys.size() - 1 - index];
            expect(batch.quotes[index].ours.lower == quote.ours.lower &&
                   batch.quotes[index].ours.upper == quote.ours.upper && batch.quotes[index].owned == quote.owned,
                   "field diagnostic batch is independent of key order including duplicate keys");
        }
        constexpr auto invalidIndependent = "invalid batch keys cannot invalidate valid quotes or enter the catalog";
        const auto invalid = CanonicalPlanActionKey::fromAction(1, 0, 0, false);
        const std::array partialKeys{ContinuationKey{today}, ContinuationKey{invalid}, ContinuationKey{damaged},
                                     ContinuationKey{today, damaged}};
        const auto partial = JointPlanStockValuer::diagnosticQuotes(field, partialKeys, SEQUENTIAL_ONLY);
        expect(partial.quotes[0].valid && partial.quotes[2].valid && partial.actions == 2 &&
               partial.quotes[1].status == DiagnosticStockStatus::InvalidKey &&
               partial.quotes.back().status == DiagnosticStockStatus::InvalidKey &&
               partial.quotes[0].cheapOurs.lower == TODAY_FLOOR && partial.quotes[2].cheapOurs.lower == DAMAGED_VALUE &&
               partial.quotes[0].refinementCompleted && partial.quotes[2].refinementCompleted &&
               !partial.quotes[1].refinementCompleted && partial.quotes[1].searchStates == 0 &&
               partial.quotes[1].refinementNanos == 0 && partial.quotes[0].refinementNanos >= 0 &&
               partial.quotes[1].pricingNanos == 0 && partial.quotes[1].replayNanos == 0 &&
               partial.preparationNanos >= 0 && partial.quotes[0].pricingNanos >= 0 &&
               partial.quotes[0].replayNanos >= 0, invalidIndependent);
        expect(Fixture::unchanged(field, before), "field diagnostic leaves the field and warmed caches unchanged");
        const auto completedField = Fixture::make(1, Fixture::RATE);
        const std::array completedKeys{ContinuationKey{today}, ContinuationKey{dead}};
        const auto completed =
            JointPlanStockValuer::diagnosticQuotes(completedField, completedKeys, SEQUENTIAL_ONLY);
        expect(completed.quotes[0].valid && completed.quotes[1].valid &&
               completed.quotes[1].owned == Fixture::HORIZON * 2 &&
               completed.quotes[0].owned == completed.quotes[1].owned && completed.quotes[1].ours.upper == 0,
               "field diagnostic preserves a capture completed this turn when its capturer is destroyed");
        const auto mixedField = Fixture::make(2);
        const auto mixedBefore = Fixture::snapshot(mixedField);
        const auto second = CanonicalPlanActionKey::fromAction(1, Fixture::WIDTH - 1, 0, false);
        const std::array mixedKeys{ContinuationKey{damaged, second}, ContinuationKey{dead, second}};
        const auto mixed = JointPlanStockValuer::diagnosticQuotes(mixedField, mixedKeys, SEQUENTIAL_ONLY);
        expect(mixed.quotes[0].valid && mixed.quotes[1].valid && mixed.quotes[1].ours.lower > 0 &&
               std::none_of(mixed.quotes[1].witness.begin(), mixed.quotes[1].witness.end(),
                   [](const RowWitness & witness) { return witness.row == 0; }) && Fixture::unchanged(mixedField, mixedBefore),
               "mixed field diagnostic keeps an undamaged row and never revives its destroyed neighbour");
    }

    void stockFieldRefinementIsExact()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t ROWS = 2;
        constexpr MilliFunds TODAY_CHEAP_LOWER = 18;
        constexpr MilliFunds TODAY_CHEAP_UPPER = 32;
        constexpr MilliFunds TODAY_EXACT = 20;
        constexpr MilliFunds DAMAGED_CHEAP_LOWER = 13;
        constexpr MilliFunds DAMAGED_CHEAP_UPPER = 25;
        constexpr MilliFunds DAMAGED_EXACT = 18;
        constexpr MilliFunds DEAD_EXACT = 12;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const std::array keys{ContinuationKey{today}, ContinuationKey{damaged}, ContinuationKey{dead}};
        const std::array cheap{
            StockInterval{TODAY_CHEAP_LOWER, TODAY_CHEAP_UPPER},
            StockInterval{DAMAGED_CHEAP_LOWER, DAMAGED_CHEAP_UPPER},
            StockInterval{DEAD_EXACT, DEAD_EXACT},
        };
        const std::array exact{TODAY_EXACT, DAMAGED_EXACT, DEAD_EXACT};
        const auto field = Fixture::make(ROWS);
        const auto batch = JointPlanStockValuer::diagnosticQuotes(field, keys, SEQUENTIAL_ONLY);
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const auto & quote = batch.quotes[index];
            expect(quote.valid && quote.status == DiagnosticStockStatus::Valid && quote.refinementCompleted &&
                       quote.cheapOurs.lower == cheap[index].lower && quote.cheapOurs.upper == cheap[index].upper &&
                       quote.ours.lower == exact[index] && quote.ours.upper == exact[index] &&
                       quote.searchStates >= 0 && quote.searchStates <= SEQUENTIAL_SEARCH_STATE_CAP &&
                       !quote.dualStatus && !quote.dualCeiling,
                   "a generous cap closes the sequential optimum inside the cheap interval");
        }

        const std::array truncatedKeys{ContinuationKey{today}};
        const auto truncated = JointPlanStockValuer::diagnosticQuotes(field, truncatedKeys, TRUNCATED_SEQUENTIAL_ONLY);
        const auto & partial = truncated.quotes.front();
        expect(truncated.limits.stateCap == TRUNCATING_STATE_CAP && partial.valid && !partial.refinementCompleted &&
                   partial.searchStates == TRUNCATING_STATE_CAP && partial.ours.lower == TODAY_CHEAP_LOWER &&
                   partial.ours.upper == TODAY_CHEAP_UPPER,
               "an unfinished refinement keeps the witnessed lower and the relaxed upper");
    }

    void stockFieldDualCeilingIsSound()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t ROWS = 2;
        constexpr std::array CHEAP_LOWER{MilliFunds{18}, MilliFunds{13}, MilliFunds{12}};
        constexpr std::array CHEAP_UPPER{MilliFunds{32}, MilliFunds{25}, MilliFunds{12}};
        constexpr std::array EXACT{MilliFunds{20}, MilliFunds{18}, MilliFunds{12}};
        constexpr std::size_t DEAD_INDEX = 2;
        constexpr MilliFunds COMPLETED_EXACT = 5;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const std::array keys{ContinuationKey{today}, ContinuationKey{damaged}, ContinuationKey{dead}};
        const auto batch = JointPlanStockValuer::diagnosticQuotes(Fixture::make(ROWS), keys, TRUNCATED_WITH_DUAL);
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const auto & quote = batch.quotes[index];
            expect(quote.valid && quote.dualCeiling.has_value() &&
                       quote.dualStatus == MasterPlanPricingStatus::Valid && quote.dualRounds > 0 &&
                       quote.cheapOurs.lower == CHEAP_LOWER[index] && quote.cheapOurs.upper == CHEAP_UPPER[index] &&
                       quote.ours.lower >= CHEAP_LOWER[index] && quote.ours.lower <= EXACT[index] &&
                       quote.ours.upper >= EXACT[index] &&
                       (index == DEAD_INDEX || (!quote.refinementCompleted && quote.ours.upper < CHEAP_UPPER[index])),
                   "the dual ceiling tightens an unfinished search and still brackets the exact optimum");
        }
        const std::array completedKeys{ContinuationKey{today}};
        const auto completed =
            JointPlanStockValuer::diagnosticQuotes(Fixture::make(1, Fixture::RATE), completedKeys, TRUNCATED_WITH_DUAL);
        const auto & captured = completed.quotes.front();
        expect(completed.limits.dualRounds == MASTER_PLAN_PRICING_ROUNDS && captured.valid &&
                   captured.dualCeiling.has_value() && captured.ours.lower <= COMPLETED_EXACT &&
                   captured.ours.upper >= COMPLETED_EXACT,
               "a capture completed this turn keeps the dual ceiling above the exact optimum");
    }

    void testReadSetOnlyBoundsWork()
    {
        constexpr TilePoint origin{0, 0};
        constexpr TilePoint near{1, 0};
        constexpr TilePoint far{4, 0};
        const Coordinator::MobilityCostGrid grid = straightCorridor(4, 3);
        Coordinator::ActivationField narrow;
        Coordinator::ActivationField wide;
        narrow.build(grid, origin, 5, Coordinator::UNBOUNDED_ACTIVATIONS, {near});
        wide.build(grid, origin, 5, Coordinator::UNBOUNDED_ACTIVATIONS, {near, far});
        expect(narrow.activations(near.x, near.y) == wide.activations(near.x, near.y),
               "read set size does not change a requested answer");
        expect(narrow.activations(far.x, far.y) == Coordinator::UNREACHABLE,
               "an unrequested tile is not exposed as settled");
        expect(wide.activations(far.x, far.y) == 4, "requested distant arrival remains exact");
    }
}

int main()
{
    testSolverMatchesBruteForce();
    testColumnWithdrawalIsExact();
    testMarginalTableMatchesFreshSolves();
    testCaptureClockAndHorizon();
    testOwnershipAndMirrorIdentities();
    testInjectivityAndPositionalDelta();
    testActivationScheduleUsesTurnBoundaries();
    testReadSetOnlyBoundsWork();
    stockFieldDiagnosticChecks();
    stockFieldRefinementIsExact();
    stockFieldDualCeilingIsSound();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
