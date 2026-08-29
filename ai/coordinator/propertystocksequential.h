#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
            std::vector<MilliFunds> prizes(
                nodeColumns.size() *
                    static_cast<std::size_t>(std::max(horizonTurns, 0)),
                0);
            for (std::size_t node = 0; node < nodeColumns.size(); ++node)
            {
                for (std::int32_t turn = 0; turn < horizonTurns; ++turn)
                {
                    prizes[
                        node * static_cast<std::size_t>(horizonTurns) +
                        static_cast<std::size_t>(turn)] =
                        columnStreamValue(nodeColumns[node],
                                          turn,
                                          horizonTurns);
                }
            }
            buildFromPrizes(
                static_cast<std::int32_t>(nodeColumns.size()),
                prizes,
                interArrivals,
                captureTurnsFromZero,
                horizonTurns);
        }

        void buildFromPrizes(std::int32_t nodeCount,
                             std::span<const MilliFunds> nodeTurnPrizes,
                             std::span<const std::int32_t> interArrivals,
                             std::int32_t captureTurnsFromZero,
                             std::int32_t horizonTurns)
        {
            if (nodeCount < 0 || horizonTurns < 0)
            {
                m_nodeCount = 0;
                m_horizonTurns = 0;
                m_captureTurnsFromZero = captureTurnsFromZero;
                m_interArrivals.clear();
                m_values.clear();
                m_witnesses.clear();
                return;
            }
            m_nodeCount = nodeCount;
            m_horizonTurns = horizonTurns;
            m_captureTurnsFromZero = captureTurnsFromZero;
            m_interArrivals.assign(interArrivals.begin(), interArrivals.end());
            const std::size_t cells = static_cast<std::size_t>(m_nodeCount) *
                                      static_cast<std::size_t>(horizonTurns);
            m_values.assign(cells, 0);
            m_witnesses.assign(cells, NO_SEQUENTIAL_NODE);
            if (captureTurnsFromZero == NO_CAPTURE_TURNS ||
                nodeTurnPrizes.size() != cells ||
                interArrivals.size() !=
                    static_cast<std::size_t>(nodeCount) *
                        static_cast<std::size_t>(nodeCount))
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
                            nodeTurnPrizes[
                                static_cast<std::size_t>(next) *
                                    static_cast<std::size_t>(horizonTurns) +
                                static_cast<std::size_t>(nextTurn)] +
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

        struct CachedRowEnumeration
        {
            std::vector<RowOption> options;
            std::int64_t statesSpent{0};
        };

        struct SequentialRowOptionSource
        {
            virtual ~SequentialRowOptionSource() = default;
            virtual bool lookup(std::int32_t rowIndex,
                                std::span<const RowOption> & options,
                                std::int64_t & statesSpent) = 0;
            virtual void store(std::int32_t rowIndex,
                               std::span<const RowOption> options,
                               std::int64_t statesSpent) = 0;
        };

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

    struct SequentialWitnessStep
    {
        std::int32_t node{NO_SEQUENTIAL_NODE};
        std::int32_t turn{NO_CAPTURE_TURNS};
        MilliFunds gain{0};

        friend constexpr bool operator==(
            const SequentialWitnessStep &,
            const SequentialWitnessStep &) = default;
    };

    struct SequentialRowWitness
    {
        std::int32_t row{NO_STOCK_ROW};
        std::vector<SequentialWitnessStep> steps;
        MilliFunds value{0};

        friend bool operator==(const SequentialRowWitness &,
                               const SequentialRowWitness &) = default;
    };

    struct SequentialTierResult
    {
        MilliFunds floorValue{0};
        MilliFunds relaxedValue{0};
        MilliFunds repairValue{0};
        MilliFunds searchValue{0};
        MilliFunds bookedValue{0};
        std::int64_t searchStates{0};
        std::int64_t itinerariesEnumerated{0};
        std::int64_t itineraryOptionsKept{0};
        std::int64_t rowsSearched{0};
        std::int64_t cacheHits{0};
        std::int64_t cacheFallbacks{0};
        bool continuationSeen{false};
        bool certificate{false};
        bool searchCompleted{false};
        std::vector<SequentialRowWitness> witness;
    };

    namespace SequentialDetail
    {
        inline MilliFunds continuationGain(const SequentialInstance & instance,
                                           const SequentialRow & row,
                                           std::int32_t node)
        {
            const std::int32_t turn =
                row.ownedTurn[static_cast<std::size_t>(node)];
            if (turn == NO_CAPTURE_TURNS)
            {
                return 0;
            }
            const MilliFunds value =
                instance.classes[static_cast<std::size_t>(row.classIndex)]
                    .valueAt(node, turn);
            return std::max<MilliFunds>(value, 0);
        }

        struct RelaxedMatch
        {
            std::int32_t row{NO_STOCK_ROW};
            std::int32_t node{NO_SEQUENTIAL_NODE};
            MilliFunds enrichedWeight{0};
        };

        inline std::vector<WitnessStep> realizeWitness(
            const SequentialInstance & instance,
            const SequentialRow & row,
            std::int32_t head)
        {
            std::vector<WitnessStep> chain;
            std::int32_t turn =
                row.ownedTurn[static_cast<std::size_t>(head)];
            chain.push_back(WitnessStep{head, turn});
            if (turn == NO_CAPTURE_TURNS)
            {
                return chain;
            }
            const SequentialClassTable & table =
                instance.classes[static_cast<std::size_t>(row.classIndex)];
            std::int32_t node = head;
            while (table.valueAt(node, turn) > 0)
            {
                const std::int32_t next = table.witnessAt(node, turn);
                if (next == NO_SEQUENTIAL_NODE)
                {
                    break;
                }
                const std::int32_t nextTurn =
                    table.continuationOwnedTurn(node, turn, next);
                if (nextTurn == NO_CAPTURE_TURNS)
                {
                    break;
                }
                chain.push_back(WitnessStep{next, nextTurn});
                node = next;
                turn = nextTurn;
            }
            return chain;
        }

        inline SequentialRowWitness ownedWitness(
            const SequentialInstance & instance,
            const SequentialRow & row,
            std::int32_t rowIndex,
            std::span<const WitnessStep> steps)
        {
            SequentialRowWitness witness;
            witness.row = rowIndex;
            witness.steps.reserve(steps.size());
            for (std::size_t step = 0; step < steps.size(); ++step)
            {
                const WitnessStep & source = steps[step];
                const MilliFunds gain =
                    step == 0
                        ? row.weight[static_cast<std::size_t>(source.node)]
                        : chainPrize(instance, source.node, source.turn);
                witness.steps.push_back(
                    SequentialWitnessStep{source.node,
                                          source.turn,
                                          gain});
                witness.value += gain;
            }
            return witness;
        }

        inline void sortOwnedWitness(
            std::vector<SequentialRowWitness> & witness)
        {
            std::sort(witness.begin(),
                      witness.end(),
                      [](const SequentialRowWitness & lhs,
                         const SequentialRowWitness & rhs)
            {
                return lhs.row < rhs.row;
            });
        }

        struct PackedRow
        {
            std::int32_t row{NO_STOCK_ROW};
            std::span<const RowOption> options;
            MilliFunds bestValue{0};
        };

        struct PackSearch
        {
            std::span<const PackedRow> rows;
            std::vector<MilliFunds> suffixBound;
            std::vector<bool> used;
            EnumerationBudget* pBudget{nullptr};
            MilliFunds incumbent{0};
            MilliFunds relaxedValue{0};
            std::vector<const RowOption*>* pCurrentChoices{nullptr};
            std::vector<const RowOption*>* pBestChoices{nullptr};
            bool exact{false};
        };

        inline void packOptionRows(PackSearch & pack,
                                   std::size_t depth,
                                   MilliFunds value)
        {
            if (pack.exact || pack.pBudget->truncated)
            {
                return;
            }
            if (depth == pack.rows.size())
            {
                if (value > pack.incumbent)
                {
                    pack.incumbent = value;
                    if (pack.pCurrentChoices != nullptr)
                    {
                        *pack.pBestChoices = *pack.pCurrentChoices;
                    }
                    if (pack.incumbent == pack.relaxedValue)
                    {
                        pack.exact = true;
                    }
                }
                return;
            }
            if (value + pack.suffixBound[depth] <= pack.incumbent)
            {
                return;
            }
            for (const RowOption & option : pack.rows[depth].options)
            {
                if (value + option.value + pack.suffixBound[depth + 1] <=
                    pack.incumbent)
                {
                    break;
                }
                bool free = true;
                for (const std::int32_t node : option.nodes)
                {
                    if (pack.used[static_cast<std::size_t>(node)])
                    {
                        free = false;
                        break;
                    }
                }
                if (!free)
                {
                    continue;
                }
                if (!pack.pBudget->spend())
                {
                    return;
                }
                for (const std::int32_t node : option.nodes)
                {
                    pack.used[static_cast<std::size_t>(node)] = true;
                }
                if (pack.pCurrentChoices != nullptr)
                {
                    (*pack.pCurrentChoices)[depth] = &option;
                }
                packOptionRows(pack, depth + 1, value + option.value);
                for (const std::int32_t node : option.nodes)
                {
                    pack.used[static_cast<std::size_t>(node)] = false;
                }
                if (pack.exact)
                {
                    return;
                }
            }
            if (pack.pCurrentChoices != nullptr)
            {
                (*pack.pCurrentChoices)[depth] = nullptr;
            }
            packOptionRows(pack, depth + 1, value);
        }
    }

    inline bool replaySequentialWitness(
        const SequentialInstance & instance,
        std::span<const SequentialRowWitness> witness,
        MilliFunds expectedValue)
    {
        std::vector<bool> usedRows(instance.rows.size(), false);
        std::vector<bool> usedNodes(
            static_cast<std::size_t>(instance.nodeCount()), false);
        MilliFunds total = 0;
        std::int32_t previousRow = NO_STOCK_ROW;
        for (const SequentialRowWitness & rowWitness : witness)
        {
            if (rowWitness.row < 0 ||
                rowWitness.row >=
                    static_cast<std::int32_t>(instance.rows.size()) ||
                rowWitness.row <= previousRow ||
                rowWitness.steps.empty())
            {
                return false;
            }
            previousRow = rowWitness.row;
            const std::size_t rowSlot =
                static_cast<std::size_t>(rowWitness.row);
            if (usedRows[rowSlot])
            {
                return false;
            }
            usedRows[rowSlot] = true;
            const SequentialRow & row = instance.rows[rowSlot];
            const SequentialClassTable & table =
                instance.classes[static_cast<std::size_t>(row.classIndex)];
            MilliFunds rowTotal = 0;
            std::int32_t previousNode = NO_SEQUENTIAL_NODE;
            std::int32_t previousTurn = NO_CAPTURE_TURNS;
            for (std::size_t index = 0;
                 index < rowWitness.steps.size();
                 ++index)
            {
                const SequentialWitnessStep & step =
                    rowWitness.steps[index];
                if (step.node < 0 ||
                    step.node >= instance.nodeCount())
                {
                    return false;
                }
                const std::size_t nodeSlot =
                    static_cast<std::size_t>(step.node);
                if (instance.capturedNodes[nodeSlot] ||
                    usedNodes[nodeSlot])
                {
                    return false;
                }
                usedNodes[nodeSlot] = true;
                MilliFunds expectedGain = 0;
                if (index == 0)
                {
                    if (step.turn != row.ownedTurn[nodeSlot])
                    {
                        return false;
                    }
                    expectedGain = row.weight[nodeSlot];
                }
                else
                {
                    const std::int32_t expectedTurn =
                        table.continuationOwnedTurn(previousNode,
                                                    previousTurn,
                                                    step.node);
                    if (expectedTurn == NO_CAPTURE_TURNS ||
                        step.turn != expectedTurn ||
                        step.turn <= previousTurn)
                    {
                        return false;
                    }
                    expectedGain = SequentialDetail::chainPrize(
                        instance, step.node, step.turn);
                }
                if (step.gain != expectedGain)
                {
                    return false;
                }
                rowTotal += step.gain;
                previousNode = step.node;
                previousTurn = step.turn;
            }
            if (rowTotal != rowWitness.value)
            {
                return false;
            }
            total += rowTotal;
        }
        return total == expectedValue;
    }

    inline SequentialTierResult solveSequentialPacking(
        const SequentialInstance & instance,
        MilliFunds floorValue,
        std::int64_t stateCap,
        SequentialDetail::SequentialRowOptionSource* pRowSource = nullptr,
        bool captureWitness = false)
    {
        using namespace SequentialDetail;

        SequentialTierResult result;
        result.floorValue = floorValue;
        result.relaxedValue = floorValue;
        result.bookedValue = floorValue;
        const std::int32_t nodeCount = instance.nodeCount();
        const std::int32_t rowCount =
            static_cast<std::int32_t>(instance.rows.size());
        std::vector<std::int32_t> openNodes;
        for (std::int32_t node = 0; node < nodeCount; ++node)
        {
            if (!instance.capturedNodes[static_cast<std::size_t>(node)])
            {
                openNodes.push_back(node);
            }
        }

        std::vector<std::vector<MilliFunds>> enriched(
            static_cast<std::size_t>(rowCount));
        for (std::int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            const SequentialRow & row =
                instance.rows[static_cast<std::size_t>(rowIndex)];
            std::vector<MilliFunds> & rowEnriched =
                enriched[static_cast<std::size_t>(rowIndex)];
            rowEnriched.assign(static_cast<std::size_t>(nodeCount), 0);
            for (const std::int32_t node : openNodes)
            {
                const std::size_t slot = static_cast<std::size_t>(node);
                const MilliFunds gain =
                    continuationGain(instance, row, node);
                rowEnriched[slot] = row.weight[slot] + gain;
                if (gain > 0)
                {
                    result.continuationSeen = true;
                }
            }
        }
        if (!result.continuationSeen)
        {
            return result;
        }

        std::vector<MilliFunds> relaxedWeights;
        relaxedWeights.reserve(static_cast<std::size_t>(rowCount) *
                               openNodes.size());
        for (std::int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            for (const std::int32_t node : openNodes)
            {
                relaxedWeights.push_back(
                    enriched[static_cast<std::size_t>(rowIndex)]
                            [static_cast<std::size_t>(node)]);
            }
        }
        AssignmentSolver relaxed;
        relaxed.solve(rowCount,
                      static_cast<std::int32_t>(openNodes.size()),
                      relaxedWeights);
        result.relaxedValue = relaxed.optimum();
        if (result.relaxedValue <= floorValue)
        {
            result.relaxedValue =
                std::max(result.relaxedValue, floorValue);
            return result;
        }

        std::vector<RelaxedMatch> matches;
        for (std::int32_t open = 0;
             open < static_cast<std::int32_t>(openNodes.size());
             ++open)
        {
            const std::int32_t rowIndex = relaxed.matchedRowOf(open);
            if (rowIndex == NO_STOCK_ROW)
            {
                continue;
            }
            const std::int32_t node =
                openNodes[static_cast<std::size_t>(open)];
            const MilliFunds weight =
                enriched[static_cast<std::size_t>(rowIndex)]
                        [static_cast<std::size_t>(node)];
            if (weight > 0)
            {
                matches.push_back(RelaxedMatch{rowIndex, node, weight});
            }
        }

        std::vector<bool> touched(static_cast<std::size_t>(nodeCount),
                                  false);
        std::optional<std::vector<SequentialRowWitness>>
            relaxedWitness;
        if (captureWitness)
        {
            relaxedWitness.emplace();
            relaxedWitness->reserve(matches.size());
        }
        bool certified = true;
        MilliFunds certifiedTotal = 0;
        for (const RelaxedMatch & match : matches)
        {
            const SequentialRow & row =
                instance.rows[static_cast<std::size_t>(match.row)];
            const std::vector<WitnessStep> chain =
                realizeWitness(instance, row, match.node);
            MilliFunds replay =
                row.weight[static_cast<std::size_t>(match.node)];
            for (std::size_t step = 0; step < chain.size(); ++step)
            {
                const std::size_t slot =
                    static_cast<std::size_t>(chain[step].node);
                if (touched[slot] || instance.capturedNodes[slot])
                {
                    certified = false;
                    break;
                }
                touched[slot] = true;
                if (step > 0)
                {
                    replay += chainPrize(instance,
                                         chain[step].node,
                                         chain[step].turn);
                }
            }
            if (!certified || replay != match.enrichedWeight)
            {
                certified = false;
                break;
            }
            certifiedTotal += replay;
            if (captureWitness)
            {
                relaxedWitness->push_back(
                    ownedWitness(instance,
                                 row,
                                 match.row,
                                 chain));
            }
        }
        if (certified && certifiedTotal == result.relaxedValue)
        {
            result.certificate = true;
            result.bookedValue = result.relaxedValue;
            if (captureWitness)
            {
                sortOwnedWitness(*relaxedWitness);
                result.witness = std::move(*relaxedWitness);
            }
            return result;
        }

        std::vector<RelaxedMatch> repairOrder = matches;
        std::stable_sort(repairOrder.begin(),
                         repairOrder.end(),
                         [](const RelaxedMatch & lhs,
                            const RelaxedMatch & rhs)
        {
            if (lhs.enrichedWeight != rhs.enrichedWeight)
            {
                return lhs.enrichedWeight > rhs.enrichedWeight;
            }
            return lhs.row < rhs.row;
        });
        std::vector<bool> used(instance.capturedNodes.begin(),
                               instance.capturedNodes.end());
        std::optional<std::vector<SequentialRowWitness>>
            repairWitness;
        if (captureWitness)
        {
            repairWitness.emplace();
            repairWitness->reserve(repairOrder.size());
        }
        MilliFunds repairTotal = 0;
        for (const RelaxedMatch & match : repairOrder)
        {
            const SequentialRow & row =
                instance.rows[static_cast<std::size_t>(match.row)];
            const std::size_t headSlot =
                static_cast<std::size_t>(match.node);
            if (!used[headSlot])
            {
                const std::vector<WitnessStep> chain =
                    realizeWitness(instance, row, match.node);
                std::optional<std::vector<WitnessStep>> acceptedSteps;
                if (captureWitness)
                {
                    acceptedSteps.emplace();
                    acceptedSteps->reserve(chain.size());
                }
                for (const WitnessStep & step : chain)
                {
                    const std::size_t slot =
                        static_cast<std::size_t>(step.node);
                    if (used[slot])
                    {
                        break;
                    }
                    used[slot] = true;
                    if (captureWitness)
                    {
                        acceptedSteps->push_back(step);
                    }
                    if (step.node == match.node)
                    {
                        repairTotal += row.weight[slot];
                    }
                    else
                    {
                        repairTotal +=
                            chainPrize(instance, step.node, step.turn);
                    }
                }
                if (captureWitness && !acceptedSteps->empty())
                {
                    repairWitness->push_back(
                        ownedWitness(instance,
                                     row,
                                     match.row,
                                     *acceptedSteps));
                }
                continue;
            }

            std::int32_t fallback = NO_SEQUENTIAL_NODE;
            MilliFunds fallbackWeight = 0;
            for (std::int32_t node = 0; node < nodeCount; ++node)
            {
                const std::size_t slot = static_cast<std::size_t>(node);
                if (used[slot] || row.weight[slot] <= fallbackWeight)
                {
                    continue;
                }
                fallback = node;
                fallbackWeight = row.weight[slot];
            }
            if (fallback != NO_SEQUENTIAL_NODE)
            {
                used[static_cast<std::size_t>(fallback)] = true;
                repairTotal += fallbackWeight;
                if (captureWitness)
                {
                    const WitnessStep step{
                        fallback,
                        row.ownedTurn[static_cast<std::size_t>(fallback)]
                    };
                    repairWitness->push_back(
                        ownedWitness(
                            instance,
                            row,
                            match.row,
                            std::span<const WitnessStep>(&step, 1)));
                }
            }
        }
        result.repairValue = repairTotal;
        if (repairTotal == result.relaxedValue)
        {
            result.certificate = true;
            result.bookedValue = result.relaxedValue;
            if (captureWitness)
            {
                sortOwnedWitness(*repairWitness);
                result.witness = std::move(*repairWitness);
            }
            return result;
        }

        EnumerationBudget budget{
            .stateCap = stateCap,
        };
        std::vector<std::vector<RowOption>> ownedOptions;
        ownedOptions.reserve(static_cast<std::size_t>(rowCount));
        std::vector<PackedRow> packedRows;
        for (std::int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            std::span<const RowOption> options;
            std::int64_t cachedCost = 0;
            const bool cached =
                pRowSource != nullptr &&
                pRowSource->lookup(rowIndex, options, cachedCost);
            if (cached && budget.states + cachedCost < budget.stateCap)
            {
                budget.states += cachedCost;
                ++result.cacheHits;
            }
            else
            {
                if (cached)
                {
                    ++result.cacheFallbacks;
                }
                const std::int64_t statesBefore = budget.states;
                ownedOptions.push_back(enumerateRowOptions(
                    instance,
                    rowIndex,
                    enriched[static_cast<std::size_t>(rowIndex)],
                    budget,
                    result.itinerariesEnumerated));
                options = std::span<const RowOption>(ownedOptions.back());
                if (!cached && pRowSource != nullptr && !budget.truncated)
                {
                    pRowSource->store(rowIndex,
                                      options,
                                      budget.states - statesBefore);
                }
            }
            if (options.empty())
            {
                continue;
            }
            result.itineraryOptionsKept +=
                static_cast<std::int64_t>(options.size());
            packedRows.push_back(PackedRow{
                .row = rowIndex,
                .options = options,
                .bestValue = options.front().value,
            });
        }
        result.rowsSearched =
            static_cast<std::int64_t>(packedRows.size());
        std::stable_sort(packedRows.begin(),
                         packedRows.end(),
                         [](const PackedRow & lhs, const PackedRow & rhs)
        {
            if (lhs.bestValue != rhs.bestValue)
            {
                return lhs.bestValue > rhs.bestValue;
            }
            return lhs.row < rhs.row;
        });

        PackSearch pack;
        pack.rows = std::span<const PackedRow>(packedRows);
        pack.suffixBound.assign(packedRows.size() + 1, 0);
        for (std::size_t index = packedRows.size(); index > 0; --index)
        {
            pack.suffixBound[index - 1] =
                pack.suffixBound[index] + packedRows[index - 1].bestValue;
        }
        pack.used.assign(static_cast<std::size_t>(nodeCount), false);
        pack.pBudget = &budget;
        pack.incumbent = std::max(floorValue, repairTotal);
        pack.relaxedValue = result.relaxedValue;
        std::optional<std::vector<const RowOption*>> currentChoices;
        std::optional<std::vector<const RowOption*>> bestChoices;
        if (captureWitness)
        {
            currentChoices.emplace(packedRows.size(), nullptr);
            bestChoices.emplace(packedRows.size(), nullptr);
            pack.pCurrentChoices = &*currentChoices;
            pack.pBestChoices = &*bestChoices;
        }
        packOptionRows(pack, 0, 0);
        result.searchStates = budget.states;
        result.searchCompleted = pack.exact || !budget.truncated;
        result.searchValue = pack.incumbent;
        result.certificate = pack.exact;
        result.bookedValue =
            std::max({floorValue, repairTotal, pack.incumbent});
        if (captureWitness &&
            pack.incumbent > std::max(floorValue, repairTotal))
        {
            for (std::size_t packed = 0;
                 packed < packedRows.size();
                 ++packed)
            {
                const RowOption* pOption = (*bestChoices)[packed];
                if (pOption == nullptr)
                {
                    continue;
                }
                const std::int32_t rowIndex = packedRows[packed].row;
                const SequentialRow & row =
                    instance.rows[static_cast<std::size_t>(rowIndex)];
                result.witness.push_back(
                    ownedWitness(instance,
                                 row,
                                 rowIndex,
                                 pOption->steps));
            }
            sortOwnedWitness(result.witness);
        }
        else if (captureWitness && repairTotal > floorValue)
        {
            sortOwnedWitness(*repairWitness);
            result.witness = std::move(*repairWitness);
        }
        return result;
    }
}
