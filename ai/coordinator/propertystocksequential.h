#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/propertystock.h"

namespace Coordinator
{
    constexpr std::int32_t NO_SEQUENTIAL_NODE = -1;
    constexpr std::int32_t NO_SEQUENTIAL_CLASS = -1;
    constexpr std::int64_t SEQUENTIAL_SEARCH_STATE_CAP = 100000;

    struct SequentialClassKey
    {
        std::int32_t gridIdentity{NO_STOCK_ROW};
        std::int32_t movementPoints{0};
        std::int32_t ratePerTurn{0};

        friend constexpr auto operator<=>(const SequentialClassKey &, const SequentialClassKey &) = default;
    };

    class SequentialClassTable
    {
    public:
        void build(std::span<const PropertyStockColumn> nodeColumns,
                   std::span<const std::int32_t> interArrivals,
                   std::int32_t captureTurnsFromZero,
                   std::int32_t horizonTurns)
        {
            m_nodeCount = static_cast<std::int32_t>(nodeColumns.size());
            m_horizonTurns = horizonTurns;
            m_captureTurnsFromZero = captureTurnsFromZero;
            m_interArrivals.assign(interArrivals.begin(), interArrivals.end());
            const std::size_t cells = static_cast<std::size_t>(m_nodeCount) *
                                      static_cast<std::size_t>(horizonTurns);
            m_values.assign(cells, 0);
            m_witnesses.assign(cells, NO_SEQUENTIAL_NODE);
            if (captureTurnsFromZero == NO_CAPTURE_TURNS)
            {
                return;
            }
            for (std::int32_t turn = horizonTurns - 1; turn >= 1; --turn)
            {
                for (std::int32_t node = 0; node < m_nodeCount; ++node)
                {
                    MilliFunds best = 0;
                    std::int32_t bestSuccessor = NO_SEQUENTIAL_NODE;
                    for (std::int32_t next = 0; next < m_nodeCount; ++next)
                    {
                        const std::int32_t nextTurn = continuationOwnedTurn(node, turn, next);
                        if (nextTurn == NO_CAPTURE_TURNS)
                        {
                            continue;
                        }
                        const MilliFunds tail = valueAt(next, nextTurn);
                        const MilliFunds candidate =
                            columnStreamValue(nodeColumns[static_cast<std::size_t>(next)],
                                              nextTurn, horizonTurns) +
                            std::max<MilliFunds>(tail, 0);
                        if (bestSuccessor == NO_SEQUENTIAL_NODE || candidate > best)
                        {
                            best = candidate;
                            bestSuccessor = next;
                        }
                    }
                    if (bestSuccessor != NO_SEQUENTIAL_NODE)
                    {
                        const std::size_t cell = cellOf(node, turn);
                        m_values[cell] = best;
                        m_witnesses[cell] = bestSuccessor;
                    }
                }
            }
        }

        std::int32_t continuationOwnedTurn(std::int32_t node,
                                           std::int32_t turn,
                                           std::int32_t next) const
        {
            if (next == node || m_captureTurnsFromZero == NO_CAPTURE_TURNS)
            {
                return NO_CAPTURE_TURNS;
            }
            const std::int32_t arrival =
                m_interArrivals[static_cast<std::size_t>(node) *
                                    static_cast<std::size_t>(m_nodeCount) +
                                static_cast<std::size_t>(next)];
            if (arrival <= 0)
            {
                return NO_CAPTURE_TURNS;
            }
            const std::int32_t nextTurn = turn + arrival + m_captureTurnsFromZero - 1;
            if (nextTurn <= turn || nextTurn >= m_horizonTurns)
            {
                return NO_CAPTURE_TURNS;
            }
            return nextTurn;
        }

        MilliFunds valueAt(std::int32_t node, std::int32_t turn) const
        {
            return m_values[cellOf(node, turn)];
        }

        std::int32_t witnessAt(std::int32_t node, std::int32_t turn) const
        {
            return m_witnesses[cellOf(node, turn)];
        }

        std::int32_t nodeCount() const
        {
            return m_nodeCount;
        }

    private:
        std::size_t cellOf(std::int32_t node, std::int32_t turn) const
        {
            return static_cast<std::size_t>(node) *
                       static_cast<std::size_t>(m_horizonTurns) +
                   static_cast<std::size_t>(turn);
        }

        std::int32_t m_nodeCount{0};
        std::int32_t m_horizonTurns{0};
        std::int32_t m_captureTurnsFromZero{NO_CAPTURE_TURNS};
        std::vector<std::int32_t> m_interArrivals;
        std::vector<MilliFunds> m_values;
        std::vector<std::int32_t> m_witnesses;
    };

    struct SequentialRow
    {
        std::int32_t classIndex{NO_SEQUENTIAL_CLASS};
        std::vector<MilliFunds> weight;
        std::vector<std::int32_t> ownedTurn;
    };

    struct SequentialInstance
    {
        std::int32_t horizonTurns{0};
        std::span<const PropertyStockColumn> nodeColumns;
        std::span<const SequentialClassTable> classes;
        std::vector<SequentialRow> rows;
        std::vector<bool> capturedNodes;

        std::int32_t nodeCount() const
        {
            return static_cast<std::int32_t>(nodeColumns.size());
        }
    };

    constexpr std::int32_t sequentialFirstOwnedTurn(std::int32_t ownedTurns,
                                                     std::int32_t horizonTurns)
    {
        if (ownedTurns < 1 || ownedTurns >= horizonTurns)
        {
            return NO_CAPTURE_TURNS;
        }
        return ownedTurns;
    }

    namespace SequentialDetail
    {
        struct WitnessStep
        {
            std::int32_t node{NO_SEQUENTIAL_NODE};
            std::int32_t turn{NO_CAPTURE_TURNS};
        };

        inline MilliFunds chainPrize(const SequentialInstance & instance,
                                     std::int32_t node,
                                     std::int32_t turn)
        {
            return columnStreamValue(
                instance.nodeColumns[static_cast<std::size_t>(node)],
                turn,
                instance.horizonTurns);
        }

        struct RowOption
        {
            std::vector<std::int32_t> nodes;
            MilliFunds value{0};
            std::vector<WitnessStep> steps;
        };

        struct EnumerationBudget
        {
            std::int64_t states{0};
            std::int64_t stateCap{0};
            bool truncated{false};

            bool spend()
            {
                if (truncated)
                {
                    return false;
                }
                ++states;
                if (states >= stateCap)
                {
                    truncated = true;
                    return false;
                }
                return true;
            }
        };

        using RowOptionMap = std::map<std::vector<std::int32_t>, RowOption>;

        inline void recordOption(RowOptionMap & options,
                                 const std::vector<WitnessStep> & steps,
                                 MilliFunds value,
                                 std::int64_t & itinerariesEnumerated)
        {
            ++itinerariesEnumerated;
            std::vector<std::int32_t> key;
            key.reserve(steps.size());
            for (const WitnessStep & step : steps)
            {
                key.push_back(step.node);
            }
            std::sort(key.begin(), key.end());
            const auto found = options.find(key);
            if (found == options.end())
            {
                RowOption option;
                option.nodes = key;
                option.value = value;
                option.steps = steps;
                options.emplace(std::move(key), std::move(option));
            }
            else if (value > found->second.value)
            {
                found->second.value = value;
                found->second.steps = steps;
            }
        }

        inline void enumerateChain(const SequentialInstance & instance,
                                   const SequentialClassTable & table,
                                   EnumerationBudget & budget,
                                   RowOptionMap & options,
                                   std::vector<WitnessStep> & steps,
                                   std::vector<bool> & visited,
                                   std::int32_t node,
                                   std::int32_t turn,
                                   MilliFunds value,
                                   std::int64_t & itinerariesEnumerated)
        {
            recordOption(options, steps, value, itinerariesEnumerated);
            for (std::int32_t next = 0; next < instance.nodeCount(); ++next)
            {
                const std::size_t slot = static_cast<std::size_t>(next);
                if (visited[slot] || instance.capturedNodes[slot])
                {
                    continue;
                }
                const std::int32_t nextTurn =
                    table.continuationOwnedTurn(node, turn, next);
                if (nextTurn == NO_CAPTURE_TURNS)
                {
                    continue;
                }
                if (!budget.spend())
                {
                    return;
                }
                visited[slot] = true;
                steps.push_back(WitnessStep{next, nextTurn});
                enumerateChain(instance,
                               table,
                               budget,
                               options,
                               steps,
                               visited,
                               next,
                               nextTurn,
                               value + chainPrize(instance, next, nextTurn),
                               itinerariesEnumerated);
                steps.pop_back();
                visited[slot] = false;
            }
        }

        inline std::vector<RowOption> enumerateRowOptions(
            const SequentialInstance & instance,
            std::int32_t rowIndex,
            const std::vector<MilliFunds> & enriched,
            EnumerationBudget & budget,
            std::int64_t & itinerariesEnumerated)
        {
            const SequentialRow & row =
                instance.rows[static_cast<std::size_t>(rowIndex)];
            const SequentialClassTable & table =
                instance.classes[static_cast<std::size_t>(row.classIndex)];
            RowOptionMap options;
            std::vector<WitnessStep> steps;
            std::vector<bool> visited(
                static_cast<std::size_t>(instance.nodeCount()), false);
            for (std::int32_t head = 0; head < instance.nodeCount(); ++head)
            {
                const std::size_t slot = static_cast<std::size_t>(head);
                if (instance.capturedNodes[slot] || enriched[slot] <= 0)
                {
                    continue;
                }
                if (!budget.spend())
                {
                    break;
                }
                const std::int32_t turn = row.ownedTurn[slot];
                visited[slot] = true;
                steps.push_back(WitnessStep{head, turn});
                if (turn == NO_CAPTURE_TURNS)
                {
                    recordOption(options,
                                 steps,
                                 row.weight[slot],
                                 itinerariesEnumerated);
                }
                else
                {
                    enumerateChain(instance,
                                   table,
                                   budget,
                                   options,
                                   steps,
                                   visited,
                                   head,
                                   turn,
                                   row.weight[slot],
                                   itinerariesEnumerated);
                }
                steps.pop_back();
                visited[slot] = false;
            }
            std::vector<RowOption> kept;
            kept.reserve(options.size());
            for (auto & entry : options)
            {
                if (entry.second.value > 0)
                {
                    kept.push_back(std::move(entry.second));
                }
            }
            std::stable_sort(kept.begin(),
                             kept.end(),
                             [](const RowOption & lhs, const RowOption & rhs)
            {
                if (lhs.value != rhs.value)
                {
                    return lhs.value > rhs.value;
                }
                return lhs.nodes < rhs.nodes;
            });
            return kept;
        }
    }
}
