#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/ownershipschedule.h"
#include "ai/coordinator/propertyeconomics.h"

namespace Coordinator
{
    constexpr std::int32_t NO_STOCK_ROW = -1;
    constexpr std::int32_t NO_STOCK_COLUMN = -1;
    constexpr std::int32_t NO_BUILDING_INDEX = -1;
    constexpr std::int32_t UNLIMITED_FUEL = -1;

    enum class FuelProjectionMode : std::int8_t
    {
        Relaxed,
        Projected,
    };

    constexpr OwnerSign STOCK_OWNER_AFTER = OwnerSign::Ours;

    struct PropertyStockHolding
    {
        PropertyIncome income{};
        OwnerSign owner{OwnerSign::Neutral};
    };

    struct PropertyStockColumn
    {
        std::int32_t slot{NO_STOCK_COLUMN};
        TilePoint tile{INVALID_TILE};
        PropertyIncome income{};
        OwnerSign ownerBefore{OwnerSign::Neutral};
        std::int32_t buildingIndex{NO_BUILDING_INDEX};

        friend constexpr bool operator==(const PropertyStockColumn &, const PropertyStockColumn &) = default;
    };

    struct PropertyStockRow
    {
        std::int32_t knowledgeIndex{NO_UNIT};
        std::int32_t gridIdentity{NO_STOCK_ROW};
        std::int32_t movementPoints{0};
        std::int32_t capturePoints{0};
        std::int32_t captureRate{0};
        TilePoint tile{INVALID_TILE};

        friend constexpr bool operator==(const PropertyStockRow &, const PropertyStockRow &) = default;
    };

    struct ArrivalFacts
    {
        std::int32_t arrivalActivations{UNREACHABLE};
        std::int32_t carriedPoints{0};
        std::int32_t ratePerTurn{0};
    };

    constexpr std::int32_t ownedTurnsUntil(std::int32_t arrivalActivations, std::int32_t carriedPoints,
                                           std::int32_t ratePerTurn, std::int32_t pointsToCapture,
                                           std::int32_t horizonTurns)
    {
        if (arrivalActivations <= 0 || ratePerTurn <= 0)
        {
            return NO_CAPTURE_TURNS;
        }
        const std::int32_t captureTurns = captureTurnsFor(carriedPoints, ratePerTurn, pointsToCapture);
        if (captureTurns == NO_CAPTURE_TURNS)
        {
            return NO_CAPTURE_TURNS;
        }
        const std::int32_t ownedTurns = arrivalActivations + captureTurns - 1;
        if (ownedTurns >= horizonTurns)
        {
            return NO_CAPTURE_TURNS;
        }
        return ownedTurns;
    }

    constexpr MilliFunds columnStreamValue(const PropertyStockColumn & column, std::int32_t ownedTurns,
                                           std::int32_t horizonTurns)
    {
        const CaptureFacts facts{
            .income = column.income,
            .ownerBefore = column.ownerBefore,
            .ownerAfter = STOCK_OWNER_AFTER,
            .turnsUntilOwned = ownedTurns,
        };
        return capturePropertyContinuation(facts, horizonTurns);
    }

    constexpr MilliFunds columnWeight(const PropertyStockColumn & column, const ArrivalFacts & arrival,
                                      std::int32_t pointsToCapture, std::int32_t horizonTurns)
    {
        const std::int32_t ownedTurns = ownedTurnsUntil(arrival.arrivalActivations, arrival.carriedPoints,
                                                        arrival.ratePerTurn, pointsToCapture, horizonTurns);
        return columnStreamValue(column, ownedTurns, horizonTurns);
    }

    constexpr MilliFunds ownedBaseline(std::span<const PropertyStockHolding> holdings, std::int32_t horizonTurns)
    {
        MilliFunds total = 0;
        for (const PropertyStockHolding & holding : holdings)
        {
            total += holding.income.valuePerTurn(holding.owner) * horizonTurns;
        }
        return total;
    }

    constexpr MilliFunds ownershipFlipSwing(const PropertyStockColumn & column, std::int32_t horizonTurns)
    {
        return incomeSwing(column.income, horizonTurns, column.ownerBefore, STOCK_OWNER_AFTER);
    }

    constexpr std::int32_t COLUMN_CEILING_TERMS = 2;

    constexpr MilliFunds propertyStockCeiling(std::span<const PropertyStockColumn> columns,
                                              MilliFunds ownedBaseline, std::int32_t horizonTurns)
    {
        MilliFunds total = ownedBaseline;
        for (const PropertyStockColumn & column : columns)
        {
            const MilliFunds swing = ownershipFlipSwing(column, horizonTurns);
            if (swing > 0)
            {
                total += COLUMN_CEILING_TERMS * swing;
            }
        }
        return total;
    }

    inline PropertyStockColumn mirroredColumn(const PropertyStockColumn & column)
    {
        PropertyStockColumn mirrored = column;
        mirrored.income = column.income.mirrored();
        mirrored.ownerBefore = mirroredSign(column.ownerBefore);
        return mirrored;
    }

    inline PropertyStockColumn capturedColumn(const PropertyStockColumn & column)
    {
        PropertyStockColumn captured = column;
        captured.ownerBefore = STOCK_OWNER_AFTER;
        return captured;
    }

    constexpr MilliFunds ASSIGNMENT_INFINITY_HEADROOM = 4;
    constexpr MilliFunds ASSIGNMENT_INFINITY = std::numeric_limits<MilliFunds>::max() / ASSIGNMENT_INFINITY_HEADROOM;

    class AssignmentSolver
    {
    public:
        void solve(std::int32_t rowCount, std::int32_t columnCount, std::span<const MilliFunds> weights)
        {
            m_rowCount = rowCount;
            m_columnCount = columnCount;
            m_paddedColumnCount = columnCount + rowCount;
            m_weights.assign(weights.begin(), weights.end());
            m_rowPotential.assign(static_cast<std::size_t>(m_rowCount) + 1, 0);
            m_columnPotential.assign(static_cast<std::size_t>(m_paddedColumnCount) + 1, 0);
            m_columnRow.assign(static_cast<std::size_t>(m_paddedColumnCount) + 1, FREE_COLUMN);
            m_columnPredecessor.assign(static_cast<std::size_t>(m_paddedColumnCount) + 1, 0);
            m_columnAvailable.assign(static_cast<std::size_t>(m_paddedColumnCount) + 1, true);
            m_augmentations = 0;
            for (std::int32_t row = 1; row <= m_rowCount; ++row)
            {
                augmentFrom(row);
            }
        }

        MilliFunds optimum() const
        {
            MilliFunds total = 0;
            for (std::int32_t column = 1; column <= m_columnCount; ++column)
            {
                const std::int32_t row = m_columnRow[static_cast<std::size_t>(column)];
                if (row != FREE_COLUMN)
                {
                    total += weightOf(row, column);
                }
            }
            return total;
        }

        bool isColumnMatched(std::int32_t columnSlot) const
        {
            return m_columnRow[static_cast<std::size_t>(columnSlot) + 1] != FREE_COLUMN;
        }

        std::int32_t matchedRowOf(std::int32_t columnSlot) const
        {
            const std::int32_t row = m_columnRow[static_cast<std::size_t>(columnSlot) + 1];
            if (row == FREE_COLUMN)
            {
                return NO_STOCK_ROW;
            }
            return row - 1;
        }

        MilliFunds optimumWithoutColumn(std::int32_t columnSlot) const
        {
            if (!isColumnMatched(columnSlot))
            {
                return optimum();
            }
            AssignmentSolver reduced = *this;
            reduced.withdrawColumn(columnSlot + 1);
            return reduced.optimum();
        }

        std::int64_t augmentationCount() const
        {
            return m_augmentations;
        }

    private:
        static constexpr std::int32_t FREE_COLUMN = 0;

        MilliFunds weightOf(std::int32_t row, std::int32_t column) const
        {
            if (column > m_columnCount)
            {
                return 0;
            }
            const std::size_t offset = static_cast<std::size_t>(row - 1) * static_cast<std::size_t>(m_columnCount) +
                                       static_cast<std::size_t>(column - 1);
            return m_weights[offset];
        }

        MilliFunds costOf(std::int32_t row, std::int32_t column) const
        {
            return -weightOf(row, column);
        }

        void withdrawColumn(std::int32_t column)
        {
            const std::int32_t freedRow = m_columnRow[static_cast<std::size_t>(column)];
            m_columnAvailable[static_cast<std::size_t>(column)] = false;
            m_columnRow[static_cast<std::size_t>(column)] = FREE_COLUMN;
            augmentFrom(freedRow);
        }

        void augmentFrom(std::int32_t row)
        {
            ++m_augmentations;
            std::vector<MilliFunds> reduced(static_cast<std::size_t>(m_paddedColumnCount) + 1, ASSIGNMENT_INFINITY);
            std::vector<bool> visited(static_cast<std::size_t>(m_paddedColumnCount) + 1, false);
            m_columnRow[0] = row;
            std::int32_t current = 0;
            do
            {
                visited[static_cast<std::size_t>(current)] = true;
                const std::int32_t sourceRow = m_columnRow[static_cast<std::size_t>(current)];
                MilliFunds step = ASSIGNMENT_INFINITY;
                std::int32_t next = 0;
                for (std::int32_t column = 1; column <= m_paddedColumnCount; ++column)
                {
                    const std::size_t slot = static_cast<std::size_t>(column);
                    if (visited[slot] || !m_columnAvailable[slot])
                    {
                        continue;
                    }
                    const MilliFunds candidate = costOf(sourceRow, column) -
                                                 m_rowPotential[static_cast<std::size_t>(sourceRow)] -
                                                 m_columnPotential[slot];
                    if (candidate < reduced[slot])
                    {
                        reduced[slot] = candidate;
                        m_columnPredecessor[slot] = current;
                    }
                    if (reduced[slot] < step)
                    {
                        step = reduced[slot];
                        next = column;
                    }
                }
                for (std::int32_t column = 0; column <= m_paddedColumnCount; ++column)
                {
                    const std::size_t slot = static_cast<std::size_t>(column);
                    if (visited[slot])
                    {
                        m_rowPotential[static_cast<std::size_t>(m_columnRow[slot])] += step;
                        m_columnPotential[slot] -= step;
                    }
                    else if (reduced[slot] != ASSIGNMENT_INFINITY)
                    {
                        reduced[slot] -= step;
                    }
                }
                current = next;
            }
            while (m_columnRow[static_cast<std::size_t>(current)] != FREE_COLUMN);
            while (current != 0)
            {
                const std::int32_t predecessor = m_columnPredecessor[static_cast<std::size_t>(current)];
                m_columnRow[static_cast<std::size_t>(current)] = m_columnRow[static_cast<std::size_t>(predecessor)];
                current = predecessor;
            }
        }

        std::int32_t m_rowCount{0};
        std::int32_t m_columnCount{0};
        std::int32_t m_paddedColumnCount{0};
        std::vector<MilliFunds> m_weights;
        std::vector<MilliFunds> m_rowPotential;
        std::vector<MilliFunds> m_columnPotential;
        std::vector<std::int32_t> m_columnRow;
        std::vector<std::int32_t> m_columnPredecessor;
        std::vector<bool> m_columnAvailable;
        std::int64_t m_augmentations{0};
    };

    struct PropertyStockInstance
    {
        std::vector<PropertyStockRow> rows;
        std::vector<PropertyStockColumn> columns;
        std::vector<MilliFunds> weights;

        std::int32_t rowCount() const
        {
            return static_cast<std::int32_t>(rows.size());
        }

        std::int32_t columnCount() const
        {
            return static_cast<std::int32_t>(columns.size());
        }

        std::span<const MilliFunds> weightRow(std::int32_t row) const
        {
            const std::size_t offset = static_cast<std::size_t>(row) * columns.size();
            return std::span<const MilliFunds>(weights.data() + offset, columns.size());
        }
    };

    struct MarginalTable
    {
        MilliFunds unmatched{0};
        std::vector<MilliFunds> withoutColumn;
        std::int64_t solves{0};
        std::int64_t augmentations{0};
    };

    struct AssignmentSubInstance
    {
        std::vector<MilliFunds> weights;
        std::vector<std::int32_t> columnSlots;
        std::int32_t rowCount{0};
        std::int32_t columnCount{0};
    };

    inline AssignmentSubInstance subInstanceWithout(const PropertyStockInstance & instance, std::int32_t excludedRow,
                                                    std::int32_t excludedColumn)
    {
        AssignmentSubInstance sub;
        for (std::int32_t column = 0; column < instance.columnCount(); ++column)
        {
            if (column != excludedColumn)
            {
                sub.columnSlots.push_back(column);
            }
        }
        sub.columnCount = static_cast<std::int32_t>(sub.columnSlots.size());
        for (std::int32_t row = 0; row < instance.rowCount(); ++row)
        {
            if (row == excludedRow)
            {
                continue;
            }
            const std::span<const MilliFunds> source = instance.weightRow(row);
            for (const std::int32_t column : sub.columnSlots)
            {
                sub.weights.push_back(source[static_cast<std::size_t>(column)]);
            }
            ++sub.rowCount;
        }
        return sub;
    }

    inline MarginalTable buildMarginalTable(const PropertyStockInstance & instance, std::int32_t actingRow,
                                            std::int32_t excludedColumn)
    {
        const AssignmentSubInstance sub = subInstanceWithout(instance, actingRow, excludedColumn);
        AssignmentSolver solver;
        solver.solve(sub.rowCount, sub.columnCount, sub.weights);
        MarginalTable table;
        table.unmatched = solver.optimum();
        table.solves = 1;
        table.withoutColumn.assign(static_cast<std::size_t>(instance.columnCount()), table.unmatched);
        for (std::int32_t column = 0; column < sub.columnCount; ++column)
        {
            if (!solver.isColumnMatched(column))
            {
                continue;
            }
            const std::size_t slot = static_cast<std::size_t>(sub.columnSlots[static_cast<std::size_t>(column)]);
            table.withoutColumn[slot] = solver.optimumWithoutColumn(column);
        }
        table.augmentations = solver.augmentationCount();
        return table;
    }

    inline MarginalTable buildMarginalTableFromScratch(const PropertyStockInstance & instance,
                                                       std::int32_t actingRow, std::int32_t excludedColumn)
    {
        const AssignmentSubInstance sub = subInstanceWithout(instance, actingRow, excludedColumn);
        AssignmentSolver solver;
        solver.solve(sub.rowCount, sub.columnCount, sub.weights);
        MarginalTable table;
        table.unmatched = solver.optimum();
        table.solves = 1;
        table.withoutColumn.assign(static_cast<std::size_t>(instance.columnCount()), table.unmatched);
        for (std::int32_t column = 0; column < sub.columnCount; ++column)
        {
            const std::int32_t fullSlot = sub.columnSlots[static_cast<std::size_t>(column)];
            const AssignmentSubInstance reduced = subInstanceWithout(instance, actingRow, fullSlot);
            AssignmentSolver reducedSolver;
            reducedSolver.solve(reduced.rowCount, reduced.columnCount, reduced.weights);
            table.withoutColumn[static_cast<std::size_t>(fullSlot)] = reducedSolver.optimum();
            ++table.solves;
        }
        return table;
    }

    inline MilliFunds instanceOptimum(const PropertyStockInstance & instance)
    {
        AssignmentSolver solver;
        solver.solve(instance.rowCount(), instance.columnCount(), instance.weights);
        return solver.optimum();
    }

    inline MilliFunds positionalOptimum(const MarginalTable & marginal, std::span<const MilliFunds> actingWeights,
                                        std::int32_t excludedColumn)
    {
        MilliFunds best = marginal.unmatched;
        for (std::size_t slot = 0; slot < actingWeights.size(); ++slot)
        {
            if (static_cast<std::int32_t>(slot) == excludedColumn || actingWeights[slot] == 0)
            {
                continue;
            }
            best = std::max(best, actingWeights[slot] + marginal.withoutColumn[slot]);
        }
        return best;
    }
}
