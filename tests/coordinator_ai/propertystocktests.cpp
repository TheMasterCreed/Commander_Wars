#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/propertystock.h"

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

    int failures = 0;

    void expect(bool condition, std::string_view message)
    {
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
}

int main()
{
    testSolverMatchesBruteForce();
    testColumnWithdrawalIsExact();
    testMarginalTableMatchesFreshSolves();
    testCaptureClockAndHorizon();
    testOwnershipAndMirrorIdentities();
    testInjectivityAndPositionalDelta();
    return failures == 0 ? 0 : 1;
}
