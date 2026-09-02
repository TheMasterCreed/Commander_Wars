#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/propertystocksequential.h"

namespace Coordinator
{
    constexpr std::int32_t NO_PACKING_DUAL_OPTION = -1;

    class PackingDualWorkBudget
    {
    public:
        explicit PackingDualWorkBudget(std::int64_t limit)
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

        bool exhausted() const
        {
            return m_exhausted;
        }

    private:
        std::int64_t m_limit{0};
        std::int64_t m_used{0};
        bool m_exhausted{false};
    };

    class PackingDualNodeMask
    {
    public:
        explicit PackingDualNodeMask(std::int32_t nodeCount = 0)
            : m_nodeCount(nodeCount),
              m_words(
                  nodeCount >= 0
                      ? (static_cast<std::size_t>(nodeCount) + 63U) / 64U
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

        bool disjoint(const PackingDualNodeMask & other) const
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

        friend bool operator==(const PackingDualNodeMask &,
                               const PackingDualNodeMask &) = default;

    private:
        std::int32_t m_nodeCount{0};
        std::vector<std::uint64_t> m_words;
    };

    struct PackingDualFieldIdentity
    {
        std::int32_t horizonTurns{0};
        std::int32_t width{0};
        std::int32_t height{0};
        std::int32_t capturePointsToCapture{0};
        MilliFunds ownedBaseline{0};
        std::vector<PropertyStockRow> ourRows;
        std::vector<PropertyStockColumn> ourColumns;
        std::vector<MilliFunds> ourWeights;
        std::vector<PropertyStockRow> enemyRows;
        std::vector<PropertyStockColumn> enemyColumns;
        std::vector<MilliFunds> enemyWeights;
        std::vector<TilePoint> columnTiles;
        std::vector<std::int32_t> columnCapturers;
        std::vector<std::int32_t> columnCapturePoints;
        std::vector<std::int32_t> enemyArrivals;
        std::vector<MobilityCostGrid> mobilityGrids;

        friend bool operator==(const PackingDualFieldIdentity &,
                               const PackingDualFieldIdentity &) = default;
    };

    struct PackingDualProblemIdentity
    {
        std::uint64_t session{0};
        PackingDualFieldIdentity immutableField;
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        std::vector<std::vector<std::int64_t>> rowDomains;
        std::vector<PropertyStockColumn> nodeColumns;
        PackingDualNodeMask usedNodes;
        MilliFunds economic{0};
        MilliFunds owned{0};
        MilliFunds incumbentFloor{0};
        MilliFunds fixedValue{0};

        friend bool operator==(const PackingDualProblemIdentity &,
                               const PackingDualProblemIdentity &) = default;
    };

    struct PackingDualRowSource
    {
        std::int32_t row{NO_STOCK_ROW};
        std::span<const std::int64_t> domain;
        std::span<const SequentialDetail::RowOption> options;
    };

    struct PackingDualOption
    {
        std::int32_t index{NO_PACKING_DUAL_OPTION};
        MilliFunds value{0};
        std::vector<std::int32_t> nodes;
        PackingDualNodeMask nodeMask;

        friend bool operator==(const PackingDualOption &,
                               const PackingDualOption &) = default;
    };

    struct PackingDualRow
    {
        std::int32_t row{NO_STOCK_ROW};
        std::vector<std::int64_t> domain;
        std::vector<PackingDualOption> options;

        friend bool operator==(const PackingDualRow &,
                               const PackingDualRow &) = default;
    };

    struct PackingDualCatalog
    {
        PackingDualProblemIdentity identity;
        std::int64_t constructionWork{0};
        std::int64_t optionRecords{0};
        std::int64_t nodeIncidences{0};
        std::vector<PackingDualRow> rows;
        bool complete{false};

        const PackingDualRow* find(std::int32_t row) const
        {
            const auto found = std::lower_bound(
                rows.begin(),
                rows.end(),
                row,
                [](const PackingDualRow & entry,
                   std::int32_t key)
            {
                return entry.row < key;
            });
            return found != rows.end() && found->row == row
                ? &*found
                : nullptr;
        }
    };

    namespace PackingDualDetail
    {
        inline bool checkedAdd(
            MilliFunds left,
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

        inline bool checkedAddWork(
            std::int64_t & total,
            std::int64_t amount)
        {
            if (amount < 0 ||
                total > std::numeric_limits<std::int64_t>::max() -
                    amount)
            {
                return false;
            }
            total += amount;
            return true;
        }

        inline bool canonicalOption(
            const SequentialDetail::RowOption & option,
            const SequentialDetail::RowOption* pPrevious,
            std::int32_t nodeCount)
        {
            if (option.value <= 0 ||
                !std::is_sorted(option.nodes.begin(),
                                option.nodes.end()) ||
                std::adjacent_find(option.nodes.begin(),
                                   option.nodes.end()) !=
                    option.nodes.end() ||
                std::any_of(
                    option.nodes.begin(),
                    option.nodes.end(),
                    [nodeCount](std::int32_t node)
            {
                return node < 0 || node >= nodeCount;
            }))
            {
                return false;
            }
            if (pPrevious == nullptr)
            {
                return true;
            }
            return pPrevious->value > option.value ||
                (pPrevious->value == option.value &&
                 pPrevious->nodes < option.nodes);
        }

        inline bool canonicalKey(
            std::span<const CanonicalPlanActionKey> key)
        {
            for (std::size_t index = 0;
                 index < key.size();
                 ++index)
            {
                if (key[index].row() < 0 ||
                    (index > 0 &&
                     key[index - 1].row() >=
                         key[index].row()))
                {
                    return false;
                }
            }
            return true;
        }

        inline bool canonicalNodeColumns(
            std::span<const PropertyStockColumn> columns)
        {
            for (std::size_t index = 0;
                 index < columns.size();
                 ++index)
            {
                if (columns[index].slot < 0 ||
                    (index > 0 &&
                     columns[index - 1].slot >=
                         columns[index].slot))
                {
                    return false;
                }
            }
            return true;
        }

        inline bool canonicalIdentity(
            const PackingDualProblemIdentity & identity,
            std::size_t rowCount)
        {
            if (identity.session == 0 ||
                identity.immutableField.ourColumns.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<
                            std::int32_t>::max()) ||
                identity.nodeColumns.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<
                            std::int32_t>::max()) ||
                rowCount >
                    static_cast<std::size_t>(
                        std::numeric_limits<
                            std::int32_t>::max()) ||
                !identity.usedNodes.valid() ||
                identity.usedNodes.nodeCount() !=
                    static_cast<std::int32_t>(
                        identity.nodeColumns.size()) ||
                identity.rowDomains.size() != rowCount ||
                !canonicalKey(identity.key) ||
                !canonicalNodeColumns(
                    identity.nodeColumns) ||
                !std::is_sorted(
                    identity.capturedColumns.begin(),
                    identity.capturedColumns.end()) ||
                std::adjacent_find(
                    identity.capturedColumns.begin(),
                    identity.capturedColumns.end()) !=
                    identity.capturedColumns.end() ||
                std::any_of(
                    identity.capturedColumns.begin(),
                    identity.capturedColumns.end(),
                    [&identity](std::int32_t column)
            {
                return column < 0 ||
                    static_cast<std::size_t>(column) >=
                        identity.immutableField.ourColumns.size();
            }))
            {
                return false;
            }
            std::size_t node = 0;
            for (std::size_t column = 0;
                 column <
                     identity.immutableField.ourColumns.size();
                 ++column)
            {
                const PropertyStockColumn & fieldColumn =
                    identity.immutableField.ourColumns[column];
                if (fieldColumn.slot !=
                    static_cast<std::int32_t>(column))
                {
                    return false;
                }
                if (fieldColumn.ownerBefore ==
                    STOCK_OWNER_AFTER)
                {
                    continue;
                }
                if (node >=
                        identity.nodeColumns.size() ||
                    identity.nodeColumns[node] !=
                        fieldColumn ||
                    identity.usedNodes.contains(
                        static_cast<std::int32_t>(node)) !=
                        std::binary_search(
                            identity.capturedColumns.begin(),
                            identity.capturedColumns.end(),
                            fieldColumn.slot))
                {
                    return false;
                }
                ++node;
            }
            if (node != identity.nodeColumns.size())
            {
                return false;
            }
            for (std::size_t row = 0;
                 row < identity.rowDomains.size();
                 ++row)
            {
                if (identity.rowDomains[row].empty() ||
                    identity.rowDomains[row].front() !=
                        static_cast<std::int64_t>(row))
                {
                    return false;
                }
            }
            return true;
        }
    }

    inline PackingDualCatalog buildPackingDualCatalog(
        std::span<const PackingDualRowSource> source,
        PackingDualProblemIdentity identity,
        std::int64_t operationLimit)
    {
        PackingDualCatalog result;
        result.identity = std::move(identity);
        PackingDualWorkBudget budget(operationLimit);
        if (operationLimit < 0 ||
            !PackingDualDetail::canonicalIdentity(
                result.identity,
                source.size()))
        {
            return result;
        }
        const std::int32_t nodeCount =
            static_cast<std::int32_t>(
                result.identity.nodeColumns.size());

        result.rows.reserve(source.size());
        for (std::size_t rowIndex = 0;
             rowIndex < source.size();
             ++rowIndex)
        {
            const PackingDualRowSource & sourceRow =
                source[rowIndex];
            if (sourceRow.row !=
                    static_cast<std::int32_t>(rowIndex) ||
                sourceRow.row < 0 ||
                sourceRow.domain.empty() ||
                sourceRow.domain.front() !=
                    static_cast<std::int64_t>(sourceRow.row) ||
                sourceRow.domain.size() !=
                    result.identity.rowDomains[rowIndex].size() ||
                !std::equal(
                    sourceRow.domain.begin(),
                    sourceRow.domain.end(),
                    result.identity.rowDomains[rowIndex].begin(),
                    result.identity.rowDomains[rowIndex].end()) ||
                (!result.rows.empty() &&
                 result.rows.back().row >= sourceRow.row))
            {
                result.constructionWork = budget.used();
                return result;
            }
            PackingDualRow row;
            row.row = sourceRow.row;
            row.domain.assign(
                sourceRow.domain.begin(),
                sourceRow.domain.end());
            row.options.reserve(sourceRow.options.size());
            const SequentialDetail::RowOption* pPrevious =
                nullptr;
            for (std::size_t optionIndex = 0;
                 optionIndex < sourceRow.options.size();
                 ++optionIndex)
            {
                const auto & sourceOption =
                    sourceRow.options[optionIndex];
                if (!PackingDualDetail::canonicalOption(
                        sourceOption,
                        pPrevious,
                        nodeCount) ||
                    optionIndex >
                        static_cast<std::size_t>(
                            std::numeric_limits<
                                std::int32_t>::max()))
                {
                    result.constructionWork = budget.used();
                    return result;
                }
                const std::int64_t optionWork =
                    std::max<std::int64_t>(
                        1,
                        static_cast<std::int64_t>(
                            sourceOption.nodes.size()));
                if (!budget.spend(optionWork))
                {
                    result.constructionWork = budget.used();
                    return result;
                }
                PackingDualNodeMask mask(nodeCount);
                for (const std::int32_t node :
                     sourceOption.nodes)
                {
                    if (!mask.set(node))
                    {
                        result.constructionWork = budget.used();
                        return result;
                    }
                }
                row.options.push_back(PackingDualOption{
                    .index =
                        static_cast<std::int32_t>(optionIndex),
                    .value = sourceOption.value,
                    .nodes = sourceOption.nodes,
                    .nodeMask = std::move(mask),
                });
                if (!PackingDualDetail::checkedAddWork(
                        result.optionRecords,
                        1) ||
                    !PackingDualDetail::checkedAddWork(
                        result.nodeIncidences,
                        static_cast<std::int64_t>(
                            sourceOption.nodes.size())))
                {
                    result.constructionWork = budget.used();
                    return result;
                }
                pPrevious = &sourceOption;
            }
            result.rows.push_back(std::move(row));
        }
        result.constructionWork = budget.used();
        result.complete = !budget.exhausted();
        return result;
    }

    inline std::int64_t packingDualVerifierWork(
        const PackingDualCatalog & catalog)
    {
        if (!catalog.complete ||
            !PackingDualDetail::canonicalIdentity(
                catalog.identity,
                catalog.rows.size()))
        {
            return -1;
        }
        std::int64_t result =
            static_cast<std::int64_t>(
                catalog.identity.nodeColumns.size());
        if (!PackingDualDetail::checkedAddWork(
                result,
                catalog.optionRecords) ||
            !PackingDualDetail::checkedAddWork(
                result,
                catalog.nodeIncidences))
        {
            return -1;
        }
        return result;
    }

    struct PackingDualCertificate
    {
        PackingDualProblemIdentity identity;
        std::vector<std::int32_t> rows;
        std::vector<MilliFunds> nodePrices;
        std::vector<MilliFunds> rowReducedMaxima;
        MilliFunds dualObjective{0};

        friend bool operator==(const PackingDualCertificate &,
                               const PackingDualCertificate &) = default;
    };

    struct PackingDualVerification
    {
        PackingDualCertificate certificate;
        MilliFunds totalPriceSum{0};
        std::int64_t nodePricesChecked{0};
        std::int64_t optionRecordsScanned{0};
        std::int64_t optionIncidencesScanned{0};
        std::int64_t workUsed{0};
        bool catalogIdentityMatches{false};
        bool pricesNonnegative{false};
        bool exactRowMaxima{false};
        bool objectiveMatches{false};
        bool checkedArithmetic{true};
        bool completed{false};
        bool valid{false};
    };

    inline PackingDualVerification verifyPackingDualCertificate(
        const PackingDualCatalog & catalog,
        std::span<const PackingDualRowSource> source,
        const PackingDualProblemIdentity & expectedIdentity,
        std::span<const MilliFunds> nodePrices,
        std::int64_t operationLimit,
        const PackingDualCertificate* pClaimed = nullptr)
    {
        using namespace PackingDualDetail;

        PackingDualVerification result;
        PackingDualWorkBudget budget(operationLimit);
        result.catalogIdentityMatches =
            catalog.complete &&
            catalog.rows.size() == source.size() &&
            catalog.identity == expectedIdentity &&
            canonicalIdentity(
                expectedIdentity,
                catalog.rows.size()) &&
            (pClaimed == nullptr ||
             pClaimed->identity == expectedIdentity);
        if (!result.catalogIdentityMatches ||
            operationLimit < 0)
        {
            return result;
        }
        const std::int32_t nodeCount =
            static_cast<std::int32_t>(
                catalog.identity.nodeColumns.size());
        if (nodePrices.size() !=
                static_cast<std::size_t>(nodeCount) ||
            catalog.rows.size() !=
                expectedIdentity.rowDomains.size() ||
            (pClaimed != nullptr &&
             pClaimed->nodePrices !=
                 std::vector<MilliFunds>(
                     nodePrices.begin(),
                     nodePrices.end())))
        {
            return result;
        }

        result.certificate.identity = expectedIdentity;
        result.certificate.nodePrices.assign(
            nodePrices.begin(),
            nodePrices.end());
        result.pricesNonnegative = true;
        for (std::int32_t node = 0;
             node < nodeCount;
             ++node)
        {
            if (!budget.spend())
            {
                result.workUsed = budget.used();
                return result;
            }
            ++result.nodePricesChecked;
            const MilliFunds price =
                nodePrices[static_cast<std::size_t>(node)];
            if (price < 0 ||
                (expectedIdentity.usedNodes.contains(node) &&
                 price != 0))
            {
                result.pricesNonnegative = false;
                result.workUsed = budget.used();
                result.completed = true;
                return result;
            }
            if (!expectedIdentity.usedNodes.contains(node) &&
                !checkedAdd(
                    result.totalPriceSum,
                    price,
                    result.totalPriceSum))
            {
                result.checkedArithmetic = false;
                result.workUsed = budget.used();
                return result;
            }
        }

        MilliFunds rowSum = 0;
        result.certificate.rows.reserve(catalog.rows.size());
        result.certificate.rowReducedMaxima.reserve(
            catalog.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < catalog.rows.size();
             ++rowIndex)
        {
            const PackingDualRow & row =
                catalog.rows[rowIndex];
            const PackingDualRowSource & sourceRow =
                source[rowIndex];
            if (row.row != static_cast<std::int32_t>(rowIndex) ||
                sourceRow.row != row.row ||
                row.domain !=
                    expectedIdentity.rowDomains[rowIndex] ||
                sourceRow.domain.size() !=
                    row.domain.size() ||
                !std::equal(
                    sourceRow.domain.begin(),
                    sourceRow.domain.end(),
                    row.domain.begin(),
                    row.domain.end()) ||
                row.domain.empty() ||
                row.domain.front() !=
                    static_cast<std::int64_t>(row.row) ||
                row.options.size() !=
                    sourceRow.options.size() ||
                (!result.certificate.rows.empty() &&
                 result.certificate.rows.back() >= row.row))
            {
                result.workUsed = budget.used();
                return result;
            }
            result.certificate.rows.push_back(row.row);
            MilliFunds maximum = 0;
            const PackingDualOption* pPrevious = nullptr;
            for (std::size_t optionIndex = 0;
                 optionIndex < row.options.size();
                 ++optionIndex)
            {
                const PackingDualOption & option =
                    row.options[optionIndex];
                const SequentialDetail::RowOption &
                    sourceOption =
                        sourceRow.options[optionIndex];
                if (!budget.spend())
                {
                    result.workUsed = budget.used();
                    return result;
                }
                ++result.optionRecordsScanned;
                const bool canonical =
                    option.index ==
                        static_cast<std::int32_t>(
                            optionIndex) &&
                    option.value == sourceOption.value &&
                    option.value > 0 &&
                    option.nodes.size() ==
                        sourceOption.nodes.size() &&
                    std::is_sorted(
                        option.nodes.begin(),
                        option.nodes.end()) &&
                    std::adjacent_find(
                        option.nodes.begin(),
                        option.nodes.end()) ==
                        option.nodes.end() &&
                    (pPrevious == nullptr ||
                     pPrevious->value > option.value ||
                     (pPrevious->value == option.value &&
                      pPrevious->nodes < option.nodes));
                if (!canonical)
                {
                    result.workUsed = budget.used();
                    result.completed = true;
                    return result;
                }
                const bool compatible =
                    expectedIdentity.usedNodes.disjoint(
                        option.nodeMask);
                MilliFunds price = 0;
                PackingDualNodeMask rebuilt(nodeCount);
                for (std::size_t incidence = 0;
                     incidence < option.nodes.size();
                     ++incidence)
                {
                    const std::int32_t node =
                        option.nodes[incidence];
                    if (!budget.spend())
                    {
                        result.workUsed = budget.used();
                        return result;
                    }
                    ++result.optionIncidencesScanned;
                    if (node !=
                            sourceOption.nodes[incidence] ||
                        node < 0 || node >= nodeCount ||
                        rebuilt.contains(node) ||
                        !rebuilt.set(node) ||
                        !checkedAdd(
                            price,
                            nodePrices[
                                static_cast<std::size_t>(
                                    node)],
                            price))
                    {
                        result.checkedArithmetic = false;
                        result.workUsed = budget.used();
                        return result;
                    }
                }
                if (rebuilt != option.nodeMask)
                {
                    result.workUsed = budget.used();
                    result.completed = true;
                    return result;
                }
                if (!compatible)
                {
                    pPrevious = &option;
                    continue;
                }
                MilliFunds reduced = 0;
                if (!checkedAdd(
                        option.value,
                        -price,
                        reduced))
                {
                    result.checkedArithmetic = false;
                    result.workUsed = budget.used();
                    return result;
                }
                maximum = std::max<MilliFunds>(
                    maximum,
                    reduced);
                pPrevious = &option;
            }
            result.certificate.rowReducedMaxima.push_back(
                maximum);
            if (!checkedAdd(rowSum, maximum, rowSum))
            {
                result.checkedArithmetic = false;
                result.workUsed = budget.used();
                return result;
            }
        }

        MilliFunds objective = result.totalPriceSum;
        if (!checkedAdd(
                objective,
                expectedIdentity.fixedValue,
                objective) ||
            !checkedAdd(objective, rowSum, objective))
        {
            result.checkedArithmetic = false;
            result.workUsed = budget.used();
            return result;
        }
        result.certificate.dualObjective = objective;
        result.exactRowMaxima =
            pClaimed == nullptr ||
            pClaimed->rows == result.certificate.rows &&
            pClaimed->rowReducedMaxima ==
                result.certificate.rowReducedMaxima;
        result.objectiveMatches =
            pClaimed == nullptr ||
            pClaimed->dualObjective == objective;
        result.workUsed = budget.used();
        result.completed = true;
        result.valid =
            result.pricesNonnegative &&
            result.checkedArithmetic &&
            result.exactRowMaxima &&
            result.objectiveMatches &&
            objective >= expectedIdentity.incumbentFloor &&
            result.optionRecordsScanned ==
                catalog.optionRecords &&
            result.optionIncidencesScanned ==
                catalog.nodeIncidences &&
            result.workUsed ==
                packingDualVerifierWork(catalog);
        return result;
    }

    struct PackingDualOptimizerResult
    {
        std::vector<MilliFunds> nodePrices;
        MilliFunds priceStep{0};
        std::int32_t iterations{0};
        std::int64_t priceUpdates{0};
        std::int64_t optionRecordsScanned{0};
        std::int64_t optionIncidencesScanned{0};
        std::int64_t coordinateWork{0};
        std::int64_t averagingWork{0};
        std::int64_t workUsed{0};
        bool checkedArithmetic{true};
        bool completed{false};
    };

    inline PackingDualOptimizerResult optimizePackingDualPrices(
        const PackingDualCatalog & catalog,
        const PackingDualProblemIdentity & expectedIdentity,
        std::int64_t operationLimit)
    {
        using namespace PackingDualDetail;

        constexpr std::int32_t ITERATIONS = 7;
        constexpr std::int32_t AVERAGING_WINDOW = 3;

        PackingDualOptimizerResult result;
        if (!catalog.complete ||
            catalog.identity != expectedIdentity ||
            !canonicalIdentity(
                expectedIdentity,
                catalog.rows.size()) ||
            operationLimit < 0)
        {
            return result;
        }
        const std::int32_t nodeCount =
            static_cast<std::int32_t>(
                catalog.identity.nodeColumns.size());
        result.nodePrices.assign(
            static_cast<std::size_t>(nodeCount),
            0);

        PackingDualWorkBudget budget(operationLimit);
        std::vector<std::vector<MilliFunds>> reducedValues;
        reducedValues.reserve(catalog.rows.size());
        MilliFunds valueGcd = 0;
        for (const PackingDualRow & row : catalog.rows)
        {
            auto & reduced = reducedValues.emplace_back();
            reduced.reserve(row.options.size());
            for (const PackingDualOption & option :
                 row.options)
            {
                if (!budget.spend())
                {
                    result.workUsed = budget.used();
                    return result;
                }
                ++result.optionRecordsScanned;
                if (expectedIdentity.usedNodes.disjoint(
                        option.nodeMask))
                {
                    reduced.push_back(option.value);
                    valueGcd = std::gcd(
                        valueGcd,
                        option.value);
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
                valueGcd > 1
                    ? valueGcd / 2
                    : valueGcd);

        std::vector<std::vector<MilliFunds>> priceHistory;
        priceHistory.reserve(ITERATIONS);
        std::vector<MilliFunds> priceDeltas(
            result.nodePrices.size(),
            0);
        for (std::int32_t iteration = 0;
             iteration < ITERATIONS;
             ++iteration)
        {
            std::vector<std::int64_t> nodeUseCounts(
                result.nodePrices.size(),
                0);
            for (std::size_t rowIndex = 0;
                 rowIndex < catalog.rows.size();
                 ++rowIndex)
            {
                MilliFunds maximum = 0;
                std::int32_t selected =
                    NO_PACKING_DUAL_OPTION;
                for (std::size_t optionIndex = 0;
                     optionIndex <
                         reducedValues[rowIndex].size();
                     ++optionIndex)
                {
                    if (!budget.spend())
                    {
                        result.workUsed = budget.used();
                        return result;
                    }
                    ++result.optionRecordsScanned;
                    if (reducedValues[rowIndex][optionIndex] >
                        maximum)
                    {
                        maximum =
                            reducedValues[rowIndex][optionIndex];
                        selected =
                            static_cast<std::int32_t>(
                                optionIndex);
                    }
                }
                if (selected == NO_PACKING_DUAL_OPTION)
                {
                    continue;
                }
                for (const std::int32_t node :
                     catalog.rows[rowIndex]
                         .options[
                             static_cast<std::size_t>(
                                 selected)]
                         .nodes)
                {
                    if (!budget.spend() ||
                        node < 0 || node >= nodeCount)
                    {
                        result.workUsed = budget.used();
                        return result;
                    }
                    ++result.optionIncidencesScanned;
                    ++nodeUseCounts[
                        static_cast<std::size_t>(node)];
                }
            }

            std::fill(
                priceDeltas.begin(),
                priceDeltas.end(),
                0);
            for (std::int32_t node = 0;
                 node < nodeCount;
                 ++node)
            {
                if (!budget.spend())
                {
                    result.workUsed = budget.used();
                    return result;
                }
                ++result.coordinateWork;
                const std::size_t slot =
                    static_cast<std::size_t>(node);
                const MilliFunds previous =
                    result.nodePrices[slot];
                MilliFunds next = previous;
                const std::int64_t useCount =
                    nodeUseCounts[slot];
                if (expectedIdentity.usedNodes.contains(node))
                {
                    if (useCount != 0 || previous != 0)
                    {
                        result.workUsed = budget.used();
                        return result;
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
                            result.workUsed = budget.used();
                            return result;
                        }
                        ++result.coordinateWork;
                        if (!checkedAdd(
                                next,
                                result.priceStep,
                                next))
                        {
                            result.checkedArithmetic = false;
                            result.workUsed = budget.used();
                            return result;
                        }
                    }
                }
                if (!checkedAdd(
                        next,
                        -previous,
                        priceDeltas[slot]))
                {
                    result.checkedArithmetic = false;
                    result.workUsed = budget.used();
                    return result;
                }
                if (next != previous)
                {
                    ++result.priceUpdates;
                }
                result.nodePrices[slot] = next;
            }
            priceHistory.push_back(result.nodePrices);
            ++result.iterations;
            if (iteration + 1 == ITERATIONS)
            {
                continue;
            }
            for (std::size_t rowIndex = 0;
                 rowIndex < catalog.rows.size();
                 ++rowIndex)
            {
                for (std::size_t optionIndex = 0;
                     optionIndex <
                         catalog.rows[rowIndex].options.size();
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
                         catalog.rows[rowIndex]
                             .options[optionIndex]
                             .nodes)
                    {
                        if (!budget.spend())
                        {
                            result.workUsed = budget.used();
                            return result;
                        }
                        ++result.optionIncidencesScanned;
                        if (!checkedAdd(
                                delta,
                                priceDeltas[
                                    static_cast<std::size_t>(
                                        node)],
                                delta))
                        {
                            result.checkedArithmetic = false;
                            result.workUsed = budget.used();
                            return result;
                        }
                    }
                    if (!checkedAdd(
                            reduced,
                            -delta,
                            reduced))
                    {
                        result.checkedArithmetic = false;
                        result.workUsed = budget.used();
                        return result;
                    }
                }
            }
        }

        for (std::int32_t node = 0;
             node < nodeCount;
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
                    result.workUsed = budget.used();
                    return result;
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
                    result.workUsed = budget.used();
                    return result;
                }
            }
            if (!budget.spend())
            {
                result.workUsed = budget.used();
                return result;
            }
            ++result.averagingWork;
            result.nodePrices[
                static_cast<std::size_t>(node)] =
                total / AVERAGING_WINDOW;
        }
        result.workUsed = budget.used();
        result.completed = true;
        return result;
    }
}
