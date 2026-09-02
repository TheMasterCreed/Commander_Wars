#pragma once

#ifndef COW_COORDINATED_REFINEMENT_TESTING
#error "Sequential packing certificates are diagnostic-only."
#endif

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/propertystocksequential.h"

namespace Coordinator
{
    constexpr std::int32_t NO_SEQUENTIAL_OPTION = -1;

    class SequentialOperationBudget
    {
    public:
        explicit SequentialOperationBudget(std::int64_t limit)
            : m_limit(std::max<std::int64_t>(limit, 0))
        {
        }

        bool spend(std::int64_t amount = 1)
        {
            if (amount < 0 || amount > m_limit - m_used)
            {
                m_exhausted = true;
                return false;
            }
            m_used += amount;
            return true;
        }

        std::int64_t used() const
        {
            return m_used;
        }

        std::int64_t remaining() const
        {
            return m_limit - m_used;
        }

        bool exhausted() const
        {
            return m_exhausted;
        }

    private:
        std::int64_t m_limit{0};
        std::int64_t m_used{0};
        bool m_exhausted{false};
    };

    class SequentialNodeMask
    {
    public:
        explicit SequentialNodeMask(std::int32_t nodeCount = 0)
            : m_nodeCount(nodeCount),
              m_words(nodeCount >= 0
                          ? (static_cast<std::size_t>(nodeCount) + 63U) /
                                64U
                          : 0U,
                      0)
        {
        }

        bool valid() const
        {
            return m_nodeCount >= 0;
        }

        std::int32_t nodeCount() const
        {
            return m_nodeCount;
        }

        bool set(std::int32_t node)
        {
            if (node < 0 || node >= m_nodeCount)
            {
                return false;
            }
            m_words[static_cast<std::size_t>(node) / 64U] |=
                std::uint64_t{1}
                << (static_cast<std::uint32_t>(node) % 64U);
            return true;
        }

        bool contains(std::int32_t node) const
        {
            return node >= 0 && node < m_nodeCount &&
                (m_words[static_cast<std::size_t>(node) / 64U] &
                 (std::uint64_t{1}
                  << (static_cast<std::uint32_t>(node) % 64U))) != 0;
        }

        bool disjoint(const SequentialNodeMask & other) const
        {
            if (m_nodeCount != other.m_nodeCount)
            {
                return false;
            }
            for (std::size_t word = 0; word < m_words.size(); ++word)
            {
                if ((m_words[word] & other.m_words[word]) != 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool merge(const SequentialNodeMask & other)
        {
            if (!disjoint(other))
            {
                return false;
            }
            for (std::size_t word = 0; word < m_words.size(); ++word)
            {
                m_words[word] |= other.m_words[word];
            }
            return true;
        }

        std::vector<std::int32_t> nodes() const
        {
            std::vector<std::int32_t> result;
            for (std::int32_t node = 0; node < m_nodeCount; ++node)
            {
                if (contains(node))
                {
                    result.push_back(node);
                }
            }
            return result;
        }

        friend bool operator==(const SequentialNodeMask &,
                               const SequentialNodeMask &) = default;

    private:
        std::int32_t m_nodeCount{0};
        std::vector<std::uint64_t> m_words;
    };

    struct SequentialIndexedOption
    {
        std::int32_t index{NO_SEQUENTIAL_OPTION};
        const SequentialDetail::RowOption* pOption{nullptr};
        SequentialNodeMask nodes;
    };

    struct SequentialIndexedRow
    {
        std::int32_t row{NO_STOCK_ROW};
        std::vector<SequentialIndexedOption> options;
    };

    struct SequentialPackingCatalog
    {
        std::int32_t nodeCount{0};
        std::int64_t constructionWork{0};
        std::int64_t optionRecords{0};
        std::int64_t nodeIncidences{0};
        std::vector<SequentialIndexedRow> rows;
        bool complete{false};

        const SequentialIndexedRow* find(std::int32_t row) const
        {
            const auto found = std::lower_bound(
                rows.begin(),
                rows.end(),
                row,
                [](const SequentialIndexedRow & entry,
                   std::int32_t key)
                {
                    return entry.row < key;
                });
            return found != rows.end() && found->row == row
                ? &*found
                : nullptr;
        }
    };

    inline SequentialPackingCatalog buildSequentialPackingCatalog(
        std::span<const SequentialRowCatalogAudit> source,
        std::int32_t nodeCount,
        std::int64_t operationLimit)
    {
        SequentialPackingCatalog result;
        result.nodeCount = nodeCount;
        SequentialOperationBudget budget(operationLimit);
        if (nodeCount < 0)
        {
            return result;
        }
        result.rows.reserve(source.size());
        for (const SequentialRowCatalogAudit & sourceRow : source)
        {
            if (sourceRow.row < 0 ||
                (!result.rows.empty() &&
                 result.rows.back().row >= sourceRow.row))
            {
                result.constructionWork = budget.used();
                return result;
            }
            SequentialIndexedRow row;
            row.row = sourceRow.row;
            row.options.reserve(sourceRow.options.size());
            MilliFunds previous =
                std::numeric_limits<MilliFunds>::max();
            for (std::size_t index = 0;
                 index < sourceRow.options.size();
                 ++index)
            {
                const auto & option = sourceRow.options[index];
                if (option.value <= 0 || option.value > previous)
                {
                    result.constructionWork = budget.used();
                    return result;
                }
                previous = option.value;
                SequentialNodeMask nodes(nodeCount);
                if (option.nodes.empty() && !budget.spend())
                {
                    result.constructionWork = budget.used();
                    return result;
                }
                for (const std::int32_t node : option.nodes)
                {
                    if (!budget.spend() || nodes.contains(node) ||
                        !nodes.set(node))
                    {
                        result.constructionWork = budget.used();
                        return result;
                    }
                    ++result.nodeIncidences;
                }
                row.options.push_back(SequentialIndexedOption{
                    .index = static_cast<std::int32_t>(index),
                    .pOption = &option,
                    .nodes = std::move(nodes),
                });
                ++result.optionRecords;
            }
            result.rows.push_back(std::move(row));
        }
        result.constructionWork = budget.used();
        result.complete = !budget.exhausted();
        return result;
    }

    struct SequentialPrimalRepairResult
    {
        std::vector<std::int32_t> optionIndices;
        std::vector<SequentialRowWitness> witness;
        MilliFunds lowerBefore{0};
        MilliFunds lowerAfter{0};
        std::int32_t repairPasses{0};
        std::int64_t oneRowTrials{0};
        std::int64_t twoRowTrials{0};
        std::int64_t compatibilityChecks{0};
        std::int64_t witnessImprovements{0};
        std::int64_t deterministicOperationCost{0};
        bool completed{false};
        bool replayValid{false};
        bool valid{false};
    };

    namespace SequentialPackingCertificateDetail
    {
        inline bool checkedAdd(MilliFunds left,
                               MilliFunds right,
                               MilliFunds & result)
        {
            constexpr auto minimum =
                std::numeric_limits<MilliFunds>::min();
            constexpr auto maximum =
                std::numeric_limits<MilliFunds>::max();
            if ((right > 0 && left > maximum - right) ||
                (right < 0 && left < minimum - right))
            {
                return false;
            }
            result = left + right;
            return true;
        }

        inline bool sameOption(
            const SequentialDetail::RowOption & option,
            const SequentialRowWitness & witness)
        {
            if (option.value != witness.value ||
                option.steps.size() != witness.steps.size())
            {
                return false;
            }
            for (std::size_t step = 0;
                 step < option.steps.size();
                 ++step)
            {
                if (option.steps[step].node !=
                        witness.steps[step].node ||
                    option.steps[step].turn !=
                        witness.steps[step].turn)
                {
                    return false;
                }
            }
            return true;
        }

        inline SequentialNodeMask selectedMask(
            const SequentialPackingCatalog & catalog,
            std::span<const std::int32_t> selected,
            std::int32_t exceptFirst,
            std::int32_t exceptSecond)
        {
            SequentialNodeMask result(catalog.nodeCount);
            for (std::size_t row = 0;
                 row < selected.size();
                 ++row)
            {
                if (static_cast<std::int32_t>(row) == exceptFirst ||
                    static_cast<std::int32_t>(row) == exceptSecond ||
                    selected[row] == NO_SEQUENTIAL_OPTION)
                {
                    continue;
                }
                const auto* pRow =
                    catalog.find(static_cast<std::int32_t>(row));
                if (pRow == nullptr ||
                    selected[row] < 0 ||
                    static_cast<std::size_t>(selected[row]) >=
                        pRow->options.size() ||
                    !result.merge(
                        pRow->options[
                            static_cast<std::size_t>(
                                selected[row])]
                            .nodes))
                {
                    return SequentialNodeMask(-1);
                }
            }
            return result;
        }

        inline bool selectionValue(
            const SequentialPackingCatalog & catalog,
            std::span<const std::int32_t> selected,
            MilliFunds & value)
        {
            value = 0;
            for (std::size_t row = 0;
                 row < selected.size();
                 ++row)
            {
                if (selected[row] == NO_SEQUENTIAL_OPTION)
                {
                    continue;
                }
                const auto* pRow =
                    catalog.find(static_cast<std::int32_t>(row));
                if (pRow == nullptr ||
                    selected[row] < 0 ||
                    static_cast<std::size_t>(selected[row]) >=
                        pRow->options.size() ||
                    !checkedAdd(
                        value,
                        pRow->options[
                            static_cast<std::size_t>(
                                selected[row])]
                            .pOption->value,
                        value))
                {
                    return false;
                }
            }
            return true;
        }

        inline std::vector<SequentialRowWitness> selectionWitness(
            const SequentialInstance & instance,
            const SequentialPackingCatalog & catalog,
            std::span<const std::int32_t> selected)
        {
            std::vector<SequentialRowWitness> witness;
            for (std::size_t row = 0;
                 row < selected.size();
                 ++row)
            {
                if (selected[row] == NO_SEQUENTIAL_OPTION)
                {
                    continue;
                }
                const auto* pRow =
                    catalog.find(static_cast<std::int32_t>(row));
                if (pRow == nullptr ||
                    selected[row] < 0 ||
                    static_cast<std::size_t>(selected[row]) >=
                        pRow->options.size())
                {
                    return {};
                }
                const auto & option =
                    *pRow->options[
                         static_cast<std::size_t>(selected[row])]
                         .pOption;
                witness.push_back(
                    SequentialDetail::ownedWitness(
                        instance,
                        instance.rows[row],
                        static_cast<std::int32_t>(row),
                        option.steps));
            }
            SequentialDetail::sortOwnedWitness(witness);
            return witness;
        }
    }

    inline SequentialPrimalRepairResult repairSequentialPackingWitness(
        const SequentialInstance & instance,
        const SequentialPackingCatalog & catalog,
        std::span<const SequentialRowWitness> initialWitness,
        MilliFunds lower,
        std::int32_t passLimit,
        std::int64_t operationLimit)
    {
        using namespace SequentialPackingCertificateDetail;

        SequentialPrimalRepairResult result;
        result.lowerBefore = lower;
        result.lowerAfter = lower;
        if (!catalog.complete ||
            catalog.nodeCount != instance.nodeCount() ||
            catalog.rows.size() != instance.rows.size() ||
            passLimit < 0 || operationLimit < 0 ||
            !replaySequentialWitness(instance, initialWitness, lower))
        {
            return result;
        }
        result.optionIndices.assign(
            instance.rows.size(), NO_SEQUENTIAL_OPTION);
        for (const SequentialRowWitness & witness : initialWitness)
        {
            const auto* pRow = catalog.find(witness.row);
            if (pRow == nullptr ||
                result.optionIndices[
                    static_cast<std::size_t>(witness.row)] !=
                    NO_SEQUENTIAL_OPTION)
            {
                return result;
            }
            std::int32_t match = NO_SEQUENTIAL_OPTION;
            for (const auto & option : pRow->options)
            {
                if (sameOption(*option.pOption, witness))
                {
                    if (match != NO_SEQUENTIAL_OPTION)
                    {
                        return result;
                    }
                    match = option.index;
                }
            }
            if (match == NO_SEQUENTIAL_OPTION)
            {
                return result;
            }
            result.optionIndices[
                static_cast<std::size_t>(witness.row)] = match;
        }
        MilliFunds selectedValue = 0;
        if (!selectionValue(
                catalog, result.optionIndices, selectedValue) ||
            selectedValue != lower ||
            !selectedMask(
                 catalog,
                 result.optionIndices,
                 NO_STOCK_ROW,
                 NO_STOCK_ROW)
                 .valid())
        {
            return result;
        }

        SequentialOperationBudget budget(operationLimit);
        const auto finish =
            [&]() -> SequentialPrimalRepairResult
        {
            result.deterministicOperationCost = budget.used();
            if (!selectionValue(
                    catalog,
                    result.optionIndices,
                    result.lowerAfter))
            {
                return result;
            }
            result.witness = selectionWitness(
                instance,
                catalog,
                result.optionIndices);
            result.replayValid = replaySequentialWitness(
                instance,
                result.witness,
                result.lowerAfter);
            result.valid =
                result.replayValid &&
                result.lowerAfter >= result.lowerBefore;
            return result;
        };
        for (std::int32_t pass = 0;
             pass < passLimit;
             ++pass)
        {
            bool improved = false;
            ++result.repairPasses;
            for (std::int32_t row = 0;
                 row < static_cast<std::int32_t>(
                           result.optionIndices.size());
                 ++row)
            {
                const auto* pRow = catalog.find(row);
                const SequentialNodeMask fixed =
                    selectedMask(
                        catalog,
                        result.optionIndices,
                        row,
                        NO_STOCK_ROW);
                if (pRow == nullptr || !fixed.valid())
                {
                    return result;
                }
                const std::int32_t currentIndex =
                    result.optionIndices[
                        static_cast<std::size_t>(row)];
                const MilliFunds currentValue =
                    currentIndex == NO_SEQUENTIAL_OPTION
                    ? 0
                    : pRow->options[
                          static_cast<std::size_t>(currentIndex)]
                          .pOption->value;
                for (const auto & option : pRow->options)
                {
                    if (option.pOption->value <= currentValue)
                    {
                        break;
                    }
                    ++result.oneRowTrials;
                    ++result.compatibilityChecks;
                    if (!budget.spend())
                    {
                        return finish();
                    }
                    if (fixed.disjoint(option.nodes))
                    {
                        result.optionIndices[
                            static_cast<std::size_t>(row)] =
                            option.index;
                        ++result.witnessImprovements;
                        improved = true;
                        break;
                    }
                }
            }
            for (std::int32_t first = 0;
                 first < static_cast<std::int32_t>(
                             result.optionIndices.size());
                 ++first)
            {
                for (std::int32_t second = first + 1;
                     second < static_cast<std::int32_t>(
                                  result.optionIndices.size());
                     ++second)
                {
                    const auto* pFirst = catalog.find(first);
                    const auto* pSecond = catalog.find(second);
                    const SequentialNodeMask fixed =
                        selectedMask(
                            catalog,
                            result.optionIndices,
                            first,
                            second);
                    if (pFirst == nullptr || pSecond == nullptr ||
                        !fixed.valid())
                    {
                        return result;
                    }
                    const auto currentValue =
                        [&](const SequentialIndexedRow & row,
                            std::int32_t index)
                    {
                        return index == NO_SEQUENTIAL_OPTION
                            ? MilliFunds{0}
                            : row.options[
                                  static_cast<std::size_t>(index)]
                                  .pOption->value;
                    };
                    MilliFunds current = 0;
                    if (!checkedAdd(
                            currentValue(
                                *pFirst,
                                result.optionIndices[
                                    static_cast<std::size_t>(
                                        first)]),
                            currentValue(
                                *pSecond,
                                result.optionIndices[
                                    static_cast<std::size_t>(
                                        second)]),
                            current))
                    {
                        return result;
                    }
                    MilliFunds best = current;
                    std::int32_t bestFirst =
                        result.optionIndices[
                            static_cast<std::size_t>(first)];
                    std::int32_t bestSecond =
                        result.optionIndices[
                            static_cast<std::size_t>(second)];
                    const MilliFunds secondMaximum =
                        pSecond->options.empty()
                        ? 0
                        : pSecond->options.front().pOption->value;
                    for (const auto & firstOption :
                         pFirst->options)
                    {
                        MilliFunds optimistic = 0;
                        if (!checkedAdd(
                                firstOption.pOption->value,
                                secondMaximum,
                                optimistic))
                        {
                            return result;
                        }
                        if (optimistic <= best)
                        {
                            break;
                        }
                        ++result.compatibilityChecks;
                        if (!budget.spend())
                        {
                            return finish();
                        }
                        if (!fixed.disjoint(firstOption.nodes))
                        {
                            continue;
                        }
                        for (const auto & secondOption :
                             pSecond->options)
                        {
                            MilliFunds combined = 0;
                            if (!checkedAdd(
                                    firstOption.pOption->value,
                                    secondOption.pOption->value,
                                    combined))
                            {
                                return result;
                            }
                            if (combined <= best)
                            {
                                break;
                            }
                            ++result.twoRowTrials;
                            ++result.compatibilityChecks;
                            if (!budget.spend())
                            {
                                return finish();
                            }
                            if (fixed.disjoint(
                                    secondOption.nodes) &&
                                firstOption.nodes.disjoint(
                                    secondOption.nodes))
                            {
                                best = combined;
                                bestFirst = firstOption.index;
                                bestSecond = secondOption.index;
                                break;
                            }
                        }
                    }
                    if (best > current)
                    {
                        result.optionIndices[
                            static_cast<std::size_t>(first)] =
                            bestFirst;
                        result.optionIndices[
                            static_cast<std::size_t>(second)] =
                            bestSecond;
                        ++result.witnessImprovements;
                        improved = true;
                    }
                }
            }
            if (!selectionValue(
                    catalog,
                    result.optionIndices,
                    result.lowerAfter))
            {
                return result;
            }
            if (!improved)
            {
                result.completed = true;
                break;
            }
        }
        return finish();
    }

    struct SequentialPairLoss
    {
        std::int32_t firstRow{NO_STOCK_ROW};
        std::int32_t secondRow{NO_STOCK_ROW};
        std::int32_t firstOptionIndex{NO_SEQUENTIAL_OPTION};
        std::int32_t secondOptionIndex{NO_SEQUENTIAL_OPTION};
        MilliFunds firstSingle{0};
        MilliFunds secondSingle{0};
        MilliFunds pairOptimum{0};
        MilliFunds loss{0};
    };

    struct SequentialPairPartitionResult
    {
        MilliFunds currentUpper{0};
        MilliFunds rowSumUpper{0};
        MilliFunds pairPartitionUpper{0};
        MilliFunds certifiedUpper{0};
        MilliFunds matchingLoss{0};
        SequentialNodeMask usedNodes;
        std::vector<std::int32_t> singleRows;
        std::vector<std::int32_t> singleOptionIndices;
        std::vector<SequentialPairLoss> losses;
        std::vector<std::pair<std::int32_t, std::int32_t>>
            matching;
        std::int64_t singleCompatibilityChecks{0};
        std::int64_t pairCompatibilityChecks{0};
        std::int64_t matchingTransitions{0};
        std::int64_t deterministicOperationCost{0};
        bool checkedArithmetic{true};
        bool completed{false};
        bool valid{false};
    };

    inline SequentialPairPartitionResult
    sequentialPairPartitionUpper(
        const SequentialPackingCatalog & catalog,
        std::span<const std::int32_t> remainingRows,
        const SequentialNodeMask & used,
        MilliFunds fixedValue,
        MilliFunds currentUpper,
        std::int64_t operationLimit)
    {
        using namespace SequentialPackingCertificateDetail;

        SequentialPairPartitionResult result;
        result.currentUpper = currentUpper;
        result.certifiedUpper = currentUpper;
        result.rowSumUpper = fixedValue;
        result.pairPartitionUpper = currentUpper;
        result.usedNodes = used;
        if (!catalog.complete || !used.valid() ||
            used.nodeCount() != catalog.nodeCount ||
            operationLimit < 0)
        {
            return result;
        }
        SequentialOperationBudget budget(operationLimit);
        std::vector<const SequentialIndexedRow*> rows;
        std::vector<const SequentialIndexedOption*> singles;
        rows.reserve(remainingRows.size());
        singles.reserve(remainingRows.size());
        result.singleRows.reserve(remainingRows.size());
        result.singleOptionIndices.reserve(remainingRows.size());
        for (std::size_t index = 0;
             index < remainingRows.size();
             ++index)
        {
            if ((index > 0 &&
                 remainingRows[index - 1] >= remainingRows[index]) ||
                catalog.find(remainingRows[index]) == nullptr)
            {
                return result;
            }
            const auto* pRow = catalog.find(remainingRows[index]);
            rows.push_back(pRow);
            const SequentialIndexedOption* pBest = nullptr;
            if (pBest == nullptr)
            {
                for (const auto & option : pRow->options)
                {
                    ++result.singleCompatibilityChecks;
                    if (!budget.spend())
                    {
                        result.deterministicOperationCost =
                            budget.used();
                        return result;
                    }
                    if (used.disjoint(option.nodes))
                    {
                        pBest = &option;
                        break;
                    }
                }
            }
            singles.push_back(pBest);
            result.singleRows.push_back(
                remainingRows[index]);
            result.singleOptionIndices.push_back(
                pBest != nullptr
                    ? pBest->index
                    : NO_SEQUENTIAL_OPTION);
            if (pBest != nullptr &&
                !checkedAdd(
                    result.rowSumUpper,
                    pBest->pOption->value,
                    result.rowSumUpper))
            {
                result.checkedArithmetic = false;
                return result;
            }
        }

        const std::size_t count = rows.size();
        std::vector<std::vector<MilliFunds>> losses(
            count, std::vector<MilliFunds>(count, 0));
        for (std::size_t first = 0;
             first < count;
             ++first)
        {
            const MilliFunds firstSingle =
                singles[first] != nullptr
                ? singles[first]->pOption->value
                : 0;
            for (std::size_t second = first + 1;
                 second < count;
                 ++second)
            {
                const MilliFunds secondSingle =
                    singles[second] != nullptr
                    ? singles[second]->pOption->value
                    : 0;
                MilliFunds singleSum = 0;
                if (!checkedAdd(
                        firstSingle,
                        secondSingle,
                        singleSum))
                {
                    result.checkedArithmetic = false;
                    return result;
                }
                MilliFunds pairBest =
                    std::max(firstSingle, secondSingle);
                std::int32_t bestFirst =
                    firstSingle >= secondSingle &&
                        singles[first] != nullptr
                    ? singles[first]->index
                    : NO_SEQUENTIAL_OPTION;
                std::int32_t bestSecond =
                    secondSingle > firstSingle &&
                        singles[second] != nullptr
                    ? singles[second]->index
                    : NO_SEQUENTIAL_OPTION;
                if (singles[first] != nullptr &&
                    singles[second] != nullptr &&
                    singles[first]->nodes.disjoint(
                        singles[second]->nodes))
                {
                    pairBest = singleSum;
                    bestFirst = singles[first]->index;
                    bestSecond = singles[second]->index;
                }
                else
                {
                    for (const auto & firstOption :
                         rows[first]->options)
                    {
                        MilliFunds optimistic = 0;
                        if (!checkedAdd(
                                firstOption.pOption->value,
                                secondSingle,
                                optimistic))
                        {
                            result.checkedArithmetic = false;
                            return result;
                        }
                        if (optimistic <= pairBest)
                        {
                            break;
                        }
                        ++result.pairCompatibilityChecks;
                        if (!budget.spend())
                        {
                            result.deterministicOperationCost =
                                budget.used();
                            return result;
                        }
                        if (!used.disjoint(firstOption.nodes))
                        {
                            continue;
                        }
                        for (const auto & secondOption :
                             rows[second]->options)
                        {
                            MilliFunds combined = 0;
                            if (!checkedAdd(
                                    firstOption.pOption->value,
                                    secondOption.pOption->value,
                                    combined))
                            {
                                result.checkedArithmetic = false;
                                return result;
                            }
                            if (combined <= pairBest)
                            {
                                break;
                            }
                            ++result.pairCompatibilityChecks;
                            if (!budget.spend())
                            {
                                result.deterministicOperationCost =
                                    budget.used();
                                return result;
                            }
                            if (used.disjoint(secondOption.nodes) &&
                                firstOption.nodes.disjoint(
                                    secondOption.nodes))
                            {
                                pairBest = combined;
                                bestFirst =
                                    firstOption.index;
                                bestSecond =
                                    secondOption.index;
                                break;
                            }
                        }
                    }
                }
                if (pairBest > singleSum)
                {
                    return result;
                }
                const MilliFunds loss = singleSum - pairBest;
                losses[first][second] = loss;
                losses[second][first] = loss;
                result.losses.push_back(SequentialPairLoss{
                    .firstRow = rows[first]->row,
                    .secondRow = rows[second]->row,
                    .firstOptionIndex = bestFirst,
                    .secondOptionIndex = bestSecond,
                    .firstSingle = firstSingle,
                    .secondSingle = secondSingle,
                    .pairOptimum = pairBest,
                    .loss = loss,
                });
            }
        }

        MilliFunds bestLoss = 0;
        std::vector<std::pair<std::int32_t, std::int32_t>>
            bestMatching;
        std::vector<bool> matched(count, false);
        std::vector<std::pair<std::int32_t, std::int32_t>>
            currentMatching;
        const auto search =
            [&](const auto & self,
                MilliFunds currentLoss) -> bool
        {
            ++result.matchingTransitions;
            if (!budget.spend())
            {
                return false;
            }
            std::size_t first = 0;
            while (first < count && matched[first])
            {
                ++first;
            }
            if (first == count)
            {
                if (currentLoss > bestLoss)
                {
                    bestLoss = currentLoss;
                    bestMatching = currentMatching;
                }
                return true;
            }
            matched[first] = true;
            if (!self(self, currentLoss))
            {
                matched[first] = false;
                return false;
            }
            for (std::size_t second = first + 1;
                 second < count;
                 ++second)
            {
                if (matched[second])
                {
                    continue;
                }
                if (losses[first][second] == 0)
                {
                    continue;
                }
                MilliFunds nextLoss = 0;
                if (!checkedAdd(
                        currentLoss,
                        losses[first][second],
                        nextLoss))
                {
                    result.checkedArithmetic = false;
                    matched[first] = false;
                    return false;
                }
                matched[second] = true;
                currentMatching.push_back(
                    {rows[first]->row, rows[second]->row});
                if (!self(self, nextLoss))
                {
                    currentMatching.pop_back();
                    matched[second] = false;
                    matched[first] = false;
                    return false;
                }
                currentMatching.pop_back();
                matched[second] = false;
            }
            matched[first] = false;
            return true;
        };
        if (!search(search, 0))
        {
            result.deterministicOperationCost = budget.used();
            return result;
        }
        if (bestLoss < 0 ||
            bestLoss > result.rowSumUpper - fixedValue)
        {
            return result;
        }
        result.matchingLoss = bestLoss;
        result.matching = std::move(bestMatching);
        result.pairPartitionUpper =
            result.rowSumUpper - bestLoss;
        result.certifiedUpper = std::min(
            currentUpper, result.pairPartitionUpper);
        result.deterministicOperationCost = budget.used();
        result.completed = true;
        result.valid = result.checkedArithmetic;
        return result;
    }

    struct SequentialNodePriceVerification
    {
        MilliFunds totalPriceSum{0};
        MilliFunds dualObjective{0};
        std::vector<MilliFunds> computedRowReducedMaxima;
        std::vector<std::int32_t> rowOptionIndices;
        std::int64_t nodePricesChecked{0};
        std::int64_t optionRecordsScanned{0};
        std::int64_t optionIncidencesScanned{0};
        std::int64_t deterministicOperationCost{0};
        bool pricesNonnegative{false};
        bool checkedArithmetic{true};
        bool completed{false};
        bool valid{false};
    };

    inline SequentialNodePriceVerification
    verifySequentialNodePriceDual(
        const SequentialPackingCatalog & catalog,
        std::span<const std::int32_t> remainingRows,
        const SequentialNodeMask & used,
        MilliFunds fixedValue,
        std::span<const MilliFunds> nodePrices,
        std::int64_t operationLimit,
        std::span<const MilliFunds> claimedRowReducedMaxima = {},
        const MilliFunds* pClaimedDualObjective = nullptr)
    {
        using namespace SequentialPackingCertificateDetail;

        SequentialNodePriceVerification result;
        SequentialOperationBudget budget(operationLimit);
        const bool claimsPresent =
            pClaimedDualObjective != nullptr;
        if (!catalog.complete || !used.valid() ||
            used.nodeCount() != catalog.nodeCount ||
            nodePrices.size() !=
                static_cast<std::size_t>(catalog.nodeCount) ||
            operationLimit < 0 ||
            (claimsPresent &&
             claimedRowReducedMaxima.size() !=
                 remainingRows.size()) ||
            (!claimsPresent &&
             !claimedRowReducedMaxima.empty()))
        {
            return result;
        }

        result.pricesNonnegative = true;
        for (std::int32_t node = 0;
             node < catalog.nodeCount;
             ++node)
        {
            if (!budget.spend())
            {
                result.deterministicOperationCost =
                    budget.used();
                return result;
            }
            ++result.nodePricesChecked;
            const MilliFunds price =
                nodePrices[static_cast<std::size_t>(node)];
            if (price < 0 ||
                (used.contains(node) && price != 0))
            {
                result.pricesNonnegative = false;
                result.deterministicOperationCost =
                    budget.used();
                result.completed = true;
                return result;
            }
            if (!used.contains(node) &&
                !checkedAdd(
                    result.totalPriceSum,
                    price,
                    result.totalPriceSum))
            {
                result.checkedArithmetic = false;
                result.deterministicOperationCost =
                    budget.used();
                return result;
            }
        }

        MilliFunds rowSum = 0;
        result.computedRowReducedMaxima.reserve(
            remainingRows.size());
        result.rowOptionIndices.reserve(
            remainingRows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < remainingRows.size();
             ++rowIndex)
        {
            if ((rowIndex > 0 &&
                 remainingRows[rowIndex - 1] >=
                     remainingRows[rowIndex]))
            {
                result.deterministicOperationCost =
                    budget.used();
                return result;
            }
            const auto* pRow =
                catalog.find(remainingRows[rowIndex]);
            if (pRow == nullptr)
            {
                result.deterministicOperationCost =
                    budget.used();
                return result;
            }
            MilliFunds actualMaximum = 0;
            std::int32_t actualOption =
                NO_SEQUENTIAL_OPTION;
            for (const auto & option : pRow->options)
            {
                if (!budget.spend())
                {
                    result.deterministicOperationCost =
                        budget.used();
                    return result;
                }
                ++result.optionRecordsScanned;
                if (!used.disjoint(option.nodes))
                {
                    continue;
                }
                MilliFunds optionPrice = 0;
                for (const std::int32_t node :
                     option.pOption->nodes)
                {
                    if (!budget.spend())
                    {
                        result.deterministicOperationCost =
                            budget.used();
                        return result;
                    }
                    ++result.optionIncidencesScanned;
                    if (node < 0 ||
                        node >= catalog.nodeCount ||
                        used.contains(node) ||
                        !checkedAdd(
                            optionPrice,
                            nodePrices[
                                static_cast<std::size_t>(
                                    node)],
                            optionPrice))
                    {
                        result.checkedArithmetic = false;
                        result.deterministicOperationCost =
                            budget.used();
                        return result;
                    }
                }
                MilliFunds reduced = 0;
                if (!checkedAdd(
                        option.pOption->value,
                        -optionPrice,
                        reduced))
                {
                    result.checkedArithmetic = false;
                    result.deterministicOperationCost =
                        budget.used();
                    return result;
                }
                if (reduced > actualMaximum)
                {
                    actualMaximum = reduced;
                    actualOption = option.index;
                }
            }
            result.computedRowReducedMaxima.push_back(
                actualMaximum);
            result.rowOptionIndices.push_back(
                actualOption);
            const MilliFunds certifiedMaximum =
                claimsPresent
                ? claimedRowReducedMaxima[rowIndex]
                : actualMaximum;
            if (certifiedMaximum < actualMaximum ||
                certifiedMaximum < 0)
            {
                result.deterministicOperationCost =
                    budget.used();
                result.completed = true;
                return result;
            }
            if (!checkedAdd(
                    rowSum,
                    certifiedMaximum,
                    rowSum))
            {
                result.checkedArithmetic = false;
                result.deterministicOperationCost =
                    budget.used();
                return result;
            }
        }

        MilliFunds objective = fixedValue;
        if (!checkedAdd(
                objective,
                result.totalPriceSum,
                objective) ||
            !checkedAdd(
                objective,
                rowSum,
                objective))
        {
            result.checkedArithmetic = false;
            result.deterministicOperationCost =
                budget.used();
            return result;
        }
        result.dualObjective = objective;
        result.deterministicOperationCost = budget.used();
        result.completed = true;
        result.valid =
            result.pricesNonnegative &&
            result.checkedArithmetic &&
            (!claimsPresent ||
             *pClaimedDualObjective == objective);
        return result;
    }

    struct SequentialNodePriceDualResult
    {
        MilliFunds currentUpper{0};
        MilliFunds initialDualUpper{0};
        MilliFunds finalDualUpper{0};
        MilliFunds certifiedUpper{0};
        MilliFunds priceStep{0};
        MilliFunds totalPriceSum{0};
        SequentialNodeMask usedNodes;
        std::vector<std::int32_t> rows;
        std::vector<std::int32_t> rowOptionIndices;
        std::vector<MilliFunds> nodePrices;
        std::vector<MilliFunds> rowReducedMaxima;
        std::int32_t dualIterations{0};
        std::int64_t priceUpdates{0};
        std::int64_t optimizerOptionRecordsScanned{0};
        std::int64_t optimizerOptionIncidencesScanned{0};
        std::int64_t priceCoordinateWork{0};
        std::int64_t averagingWork{0};
        std::int64_t verificationOptionRecordsScanned{0};
        std::int64_t verificationOptionIncidencesScanned{0};
        std::int64_t optimizerWork{0};
        std::int64_t verificationWork{0};
        std::int64_t deterministicOperationCost{0};
        bool checkedArithmetic{true};
        bool optimizerCompleted{false};
        bool verificationCompleted{false};
        bool dualCertificateValid{false};
        bool completed{false};
        bool valid{false};
    };

    inline SequentialNodePriceDualResult
    sequentialNodePriceDualUpper(
        const SequentialPackingCatalog & catalog,
        std::span<const std::int32_t> remainingRows,
        const SequentialNodeMask & used,
        MilliFunds fixedValue,
        MilliFunds currentUpper,
        std::int64_t operationLimit)
    {
        using namespace SequentialPackingCertificateDetail;

        constexpr std::int32_t ITERATIONS = 7;
        constexpr std::int32_t AVERAGING_WINDOW = 3;

        SequentialNodePriceDualResult result;
        result.currentUpper = currentUpper;
        result.initialDualUpper = currentUpper;
        result.finalDualUpper = currentUpper;
        result.certifiedUpper = currentUpper;
        result.usedNodes = used;
        result.rows.assign(
            remainingRows.begin(),
            remainingRows.end());
        result.nodePrices.assign(
            static_cast<std::size_t>(
                std::max(catalog.nodeCount, 0)),
            0);
        if (!catalog.complete || !used.valid() ||
            used.nodeCount() != catalog.nodeCount ||
            operationLimit < 0)
        {
            return result;
        }

        std::vector<const SequentialIndexedRow*> rows;
        rows.reserve(remainingRows.size());
        std::vector<std::vector<MilliFunds>> reducedValues;
        reducedValues.reserve(remainingRows.size());
        SequentialOperationBudget budget(operationLimit);
        const auto stopOptimizer =
            [&]() -> SequentialNodePriceDualResult
        {
            result.optimizerWork = budget.used();
            result.deterministicOperationCost =
                result.optimizerWork;
            return result;
        };
        MilliFunds valueGcd = 0;
        for (std::size_t rowIndex = 0;
             rowIndex < remainingRows.size();
             ++rowIndex)
        {
            if ((rowIndex > 0 &&
                 remainingRows[rowIndex - 1] >=
                     remainingRows[rowIndex]))
            {
                return stopOptimizer();
            }
            const auto* pRow =
                catalog.find(remainingRows[rowIndex]);
            if (pRow == nullptr)
            {
                return stopOptimizer();
            }
            rows.push_back(pRow);
            auto & reduced = reducedValues.emplace_back();
            reduced.reserve(pRow->options.size());
            for (const auto & option : pRow->options)
            {
                if (!budget.spend())
                {
                    return stopOptimizer();
                }
                ++result.optimizerOptionRecordsScanned;
                if (used.disjoint(option.nodes))
                {
                    reduced.push_back(
                        option.pOption->value);
                    valueGcd = std::gcd(
                        valueGcd,
                        option.pOption->value);
                }
                else
                {
                    reduced.push_back(
                        std::numeric_limits<
                            MilliFunds>::min());
                }
            }
        }
        result.priceStep =
            std::max<MilliFunds>(
                1,
                valueGcd > 1 ? valueGcd / 2 : valueGcd);

        std::vector<std::vector<MilliFunds>> priceHistory;
        priceHistory.reserve(ITERATIONS);
        std::vector<MilliFunds> priceDeltas(
            result.nodePrices.size(), 0);
        for (std::int32_t iteration = 0;
             iteration < ITERATIONS;
             ++iteration)
        {
            std::vector<std::int64_t> nodeUseCounts(
                result.nodePrices.size(), 0);
            MilliFunds rowMaximumSum = 0;
            for (std::size_t rowIndex = 0;
                 rowIndex < rows.size();
                 ++rowIndex)
            {
                MilliFunds maximum = 0;
                std::int32_t selected =
                    NO_SEQUENTIAL_OPTION;
                for (std::size_t optionIndex = 0;
                     optionIndex <
                         reducedValues[rowIndex].size();
                     ++optionIndex)
                {
                    if (!budget.spend())
                    {
                        return stopOptimizer();
                    }
                    ++result.optimizerOptionRecordsScanned;
                    if (reducedValues[rowIndex][optionIndex] >
                        maximum)
                    {
                        maximum =
                            reducedValues[rowIndex][
                                optionIndex];
                        selected =
                            static_cast<std::int32_t>(
                                optionIndex);
                    }
                }
                if (!checkedAdd(
                        rowMaximumSum,
                        maximum,
                        rowMaximumSum))
                {
                    result.checkedArithmetic = false;
                    return stopOptimizer();
                }
                if (selected == NO_SEQUENTIAL_OPTION)
                {
                    continue;
                }
                for (const std::int32_t node :
                     rows[rowIndex]
                         ->options[
                             static_cast<std::size_t>(
                                 selected)]
                         .pOption->nodes)
                {
                    if (!budget.spend() ||
                        node < 0 ||
                        node >= catalog.nodeCount)
                    {
                        return stopOptimizer();
                    }
                    ++result.optimizerOptionIncidencesScanned;
                    ++nodeUseCounts[
                        static_cast<std::size_t>(node)];
                }
            }
            if (iteration == 0)
            {
                result.initialDualUpper = fixedValue;
                if (!checkedAdd(
                        result.initialDualUpper,
                        rowMaximumSum,
                        result.initialDualUpper))
                {
                    result.checkedArithmetic = false;
                    return stopOptimizer();
                }
            }

            std::fill(
                priceDeltas.begin(),
                priceDeltas.end(),
                0);
            for (std::int32_t node = 0;
                 node < catalog.nodeCount;
                 ++node)
            {
                if (!budget.spend())
                {
                    return stopOptimizer();
                }
                ++result.priceCoordinateWork;
                const std::size_t slot =
                    static_cast<std::size_t>(node);
                const MilliFunds previous =
                    result.nodePrices[slot];
                MilliFunds next = previous;
                const std::int64_t useCount =
                    nodeUseCounts[slot];
                if (used.contains(node))
                {
                    if (useCount != 0 || previous != 0)
                    {
                        return stopOptimizer();
                    }
                }
                else if (useCount == 0)
                {
                    next =
                        previous > result.priceStep
                        ? previous - result.priceStep
                        : 0;
                }
                else
                {
                    for (std::int64_t extra = 1;
                         extra < useCount;
                         ++extra)
                    {
                        if (!budget.spend())
                        {
                            return stopOptimizer();
                        }
                        ++result.priceCoordinateWork;
                        if (!checkedAdd(
                                next,
                                result.priceStep,
                                next))
                        {
                            result.checkedArithmetic = false;
                            return stopOptimizer();
                        }
                    }
                }
                if (!checkedAdd(
                        next,
                        -previous,
                        priceDeltas[slot]))
                {
                    result.checkedArithmetic = false;
                    return stopOptimizer();
                }
                if (next != previous)
                {
                    ++result.priceUpdates;
                }
                result.nodePrices[slot] = next;
            }
            priceHistory.push_back(result.nodePrices);
            ++result.dualIterations;
            if (iteration + 1 == ITERATIONS)
            {
                continue;
            }
            for (std::size_t rowIndex = 0;
                 rowIndex < rows.size();
                 ++rowIndex)
            {
                for (std::size_t optionIndex = 0;
                     optionIndex <
                         rows[rowIndex]->options.size();
                     ++optionIndex)
                {
                    auto & reduced =
                        reducedValues[rowIndex][optionIndex];
                    if (reduced ==
                        std::numeric_limits<
                            MilliFunds>::min())
                    {
                        continue;
                    }
                    MilliFunds delta = 0;
                    for (const std::int32_t node :
                         rows[rowIndex]
                             ->options[optionIndex]
                             .pOption->nodes)
                    {
                        if (!budget.spend())
                        {
                            return stopOptimizer();
                        }
                        ++result.optimizerOptionIncidencesScanned;
                        if (!checkedAdd(
                                delta,
                                priceDeltas[
                                    static_cast<std::size_t>(
                                        node)],
                                delta))
                        {
                            result.checkedArithmetic = false;
                            return stopOptimizer();
                        }
                    }
                    if (!checkedAdd(
                            reduced,
                            -delta,
                            reduced))
                    {
                        result.checkedArithmetic = false;
                        return stopOptimizer();
                    }
                }
            }
        }

        for (std::int32_t node = 0;
             node < catalog.nodeCount;
             ++node)
        {
            MilliFunds total = 0;
            for (std::int32_t sample =
                     ITERATIONS - AVERAGING_WINDOW;
                 sample < ITERATIONS;
                 ++sample)
            {
                if (!budget.spend())
                {
                    return stopOptimizer();
                }
                ++result.averagingWork;
                if (!checkedAdd(
                        total,
                        priceHistory[
                            static_cast<std::size_t>(
                                sample)]
                            [static_cast<std::size_t>(
                                node)],
                        total))
                {
                    result.checkedArithmetic = false;
                    return stopOptimizer();
                }
            }
            if (!budget.spend())
            {
                return stopOptimizer();
            }
            ++result.averagingWork;
            result.nodePrices[
                static_cast<std::size_t>(node)] =
                total / AVERAGING_WINDOW;
        }
        result.optimizerWork = budget.used();
        result.optimizerCompleted = true;

        const auto verification =
            verifySequentialNodePriceDual(
                catalog,
                remainingRows,
                used,
                fixedValue,
                result.nodePrices,
                operationLimit - result.optimizerWork);
        result.verificationOptionRecordsScanned =
            verification.optionRecordsScanned;
        result.verificationOptionIncidencesScanned =
            verification.optionIncidencesScanned;
        result.verificationWork =
            verification.deterministicOperationCost;
        result.deterministicOperationCost =
            result.optimizerWork +
            result.verificationWork;
        result.verificationCompleted =
            verification.completed;
        result.dualCertificateValid =
            verification.valid;
        result.checkedArithmetic =
            result.checkedArithmetic &&
            verification.checkedArithmetic;
        if (!verification.valid)
        {
            return result;
        }
        result.totalPriceSum =
            verification.totalPriceSum;
        result.finalDualUpper =
            verification.dualObjective;
        result.certifiedUpper = std::min(
            currentUpper,
            result.finalDualUpper);
        result.rowReducedMaxima =
            verification.computedRowReducedMaxima;
        result.rowOptionIndices =
            verification.rowOptionIndices;
        result.completed = true;
        result.valid = true;
        return result;
    }
}
