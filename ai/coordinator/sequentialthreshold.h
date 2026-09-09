#pragma once

#ifndef COW_COORDINATED_REFINEMENT_TESTING
#error "Sequential threshold search is diagnostic-only."
#endif

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/sequentialpackingcertificate.h"
#include "ai/coordinator/propertystocksequential.h"

namespace Coordinator
{
    enum class SequentialThresholdStatus : std::int8_t
    {
        ProvenBelowThreshold,
        CounterexampleWitness,
        UnresolvedAtBudget,
        Invalid,
    };

    enum class SequentialThresholdStop : std::int8_t
    {
        Proven,
        Counterexample,
        WorkBudget,
        RowOptionResidualBoundMissing,
        InvalidInput,
        InvalidCacheAccounting,
        InvalidWitness,
    };

    enum class SequentialCacheAccounting : std::int8_t
    {
        RechargeEveryLookup,
        OncePerIdentity,
    };

    struct SequentialThresholdOptions
    {
        bool pairPartitionUpper{false};
        bool nodePriceDual{false};
        bool captureResidualNodes{false};
    };

    struct SequentialOptimisticOptionAudit
    {
        std::int32_t row{NO_STOCK_ROW};
        std::int32_t optionIndex{NO_SEQUENTIAL_OPTION};
        MilliFunds value{0};
        std::vector<std::int32_t> nodes;
    };

    struct SequentialThresholdOpenNodeAudit
    {
        std::size_t depth{0};
        MilliFunds fixedValue{0};
        MilliFunds upper{0};
        MilliFunds rowSumUpper{0};
        std::vector<std::int32_t> usedNodes;
        std::vector<std::int32_t> remainingRows;
        std::vector<std::int32_t> branches;
        std::vector<SequentialOptimisticOptionAudit>
            optimisticOptions;
        std::vector<std::int32_t> overlappingNodes;
        std::int32_t overlapCount{0};
        bool optimisticOptionsCompatible{false};
    };

    struct SequentialThresholdResult
    {
        SequentialThresholdStatus status{
            SequentialThresholdStatus::Invalid
        };
        SequentialThresholdStop stop{
            SequentialThresholdStop::InvalidInput
        };
        MilliFunds threshold{0};
        MilliFunds feasibleLower{0};
        MilliFunds residualUpper{0};
        MilliFunds originalRelaxedUpper{0};
        std::int64_t statesUsed{0};
        std::int64_t workUsed{0};
        std::int64_t rowConstructionStates{0};
        std::int64_t cacheQuotedStates{0};
        std::int64_t cacheChargedStates{0};
        std::int64_t packingStates{0};
        std::int64_t packingLeaves{0};
        std::int64_t packingPrunes{0};
        std::int64_t openNodesAtStop{0};
        std::int64_t packingCatalogWork{0};
        std::int64_t packingCatalogOptionRecords{0};
        std::int64_t packingCatalogNodeIncidences{0};
        std::int64_t pairBoundWork{0};
        std::int64_t pairBoundCalls{0};
        std::int64_t pairBoundCompleted{0};
        std::int64_t pairBoundTightenings{0};
        std::int64_t pairBoundPrunes{0};
        std::int64_t dualWork{0};
        std::int64_t dualCalls{0};
        std::int64_t dualCompleted{0};
        std::int64_t dualTightenings{0};
        std::uint64_t expansionOrderHash{1469598103934665603ULL};
        MilliFunds rootPairUpper{0};
        MilliFunds rootDualUpper{0};
        bool rowOptionDomainsComplete{false};
        bool searchCompleted{false};
        bool thresholdProven{false};
        bool witnessReplays{false};
        bool packingCatalogComplete{false};
        std::vector<SequentialRowPhaseAudit> rowPhases;
        std::vector<PropertyStockColumn> nodeColumns;
        std::vector<SequentialRowCatalogAudit> rowCatalogs;
        std::vector<SequentialRowWitness> witness;
        SequentialPairPartitionResult rootPairCertificate;
        SequentialNodePriceDualResult rootDualCertificate;
        std::vector<SequentialThresholdOpenNodeAudit>
            residualNodes;
    };

    namespace SequentialThresholdDetail
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

        inline bool validInstance(const SequentialInstance & instance)
        {
            if (instance.horizonTurns < 0 ||
                instance.capturedNodes.size() !=
                    static_cast<std::size_t>(instance.nodeCount()))
            {
                return false;
            }
            for (const SequentialRow & row : instance.rows)
            {
                if (row.classIndex < 0 ||
                    row.classIndex >=
                        static_cast<std::int32_t>(
                            instance.classes.size()) ||
                    row.weight.size() !=
                        static_cast<std::size_t>(
                            instance.nodeCount()) ||
                    row.ownedTurn.size() !=
                        static_cast<std::size_t>(
                            instance.nodeCount()))
                {
                    return false;
                }
                const SequentialClassTable & table =
                    instance.classes[
                        static_cast<std::size_t>(row.classIndex)];
                if (table.nodeCount() != instance.nodeCount() ||
                    table.horizonTurns() != instance.horizonTurns)
                {
                    return false;
                }
                for (const std::int32_t turn : row.ownedTurn)
                {
                    if (turn != NO_CAPTURE_TURNS &&
                        (turn < 1 ||
                         turn >= instance.horizonTurns))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        inline bool compatible(
            std::span<const std::int32_t> nodes,
            std::span<const std::uint8_t> used)
        {
            for (const std::int32_t node : nodes)
            {
                if (node < 0 ||
                    static_cast<std::size_t>(node) >= used.size() ||
                    used[static_cast<std::size_t>(node)])
                {
                    return false;
                }
            }
            return true;
        }

        using ThresholdPackedRow = SequentialDetail::PackedRow;

        struct ThresholdNode
        {
            std::size_t depth{0};
            MilliFunds value{0};
            MilliFunds upper{0};
            std::vector<std::uint8_t> used;
            SequentialNodeMask usedMask;
            std::vector<std::int32_t> branches;
        };

        struct ThresholdNodeOrder
        {
            bool operator()(const ThresholdNode & left,
                            const ThresholdNode & right) const
            {
                if (left.upper != right.upper)
                {
                    return left.upper < right.upper;
                }
                if (left.depth != right.depth)
                {
                    return left.depth < right.depth;
                }
                return left.branches > right.branches;
            }
        };

        inline bool nodeUpper(
            std::span<const ThresholdPackedRow> rows,
            std::size_t depth,
            MilliFunds value,
            std::span<const std::uint8_t> used,
            MilliFunds globalUpper,
            MilliFunds & upper)
        {
            const auto compatibleUpper = SequentialDetail::compatibleRowUpper(
                rows, depth, value, used, [] { return true; });
            if (!compatibleUpper)
            {
                return false;
            }
            upper = std::min(*compatibleUpper, globalUpper);
            return true;
        }

        inline std::optional<std::vector<SequentialRowWitness>>
        floorWitness(const SequentialInstance & instance,
                     std::span<const std::int32_t> openNodes,
                     MilliFunds floorValue)
        {
            std::vector<MilliFunds> weights;
            weights.reserve(instance.rows.size() * openNodes.size());
            for (const SequentialRow & row : instance.rows)
            {
                for (const std::int32_t node : openNodes)
                {
                    weights.push_back(
                        row.weight[static_cast<std::size_t>(node)]);
                }
            }
            AssignmentSolver solver;
            solver.solve(
                static_cast<std::int32_t>(instance.rows.size()),
                static_cast<std::int32_t>(openNodes.size()),
                weights);
            if (solver.optimum() != floorValue)
            {
                return std::nullopt;
            }
            std::vector<SequentialRowWitness> witness;
            for (std::int32_t open = 0;
                 open < static_cast<std::int32_t>(openNodes.size());
                 ++open)
            {
                const std::int32_t row = solver.matchedRowOf(open);
                if (row == NO_STOCK_ROW)
                {
                    continue;
                }
                const std::int32_t node =
                    openNodes[static_cast<std::size_t>(open)];
                const SequentialDetail::WitnessStep step{
                    node,
                    instance.rows[static_cast<std::size_t>(row)]
                        .ownedTurn[static_cast<std::size_t>(node)]
                };
                witness.push_back(
                    SequentialDetail::ownedWitness(
                        instance,
                        instance.rows[static_cast<std::size_t>(row)],
                        row,
                        std::span<
                            const SequentialDetail::WitnessStep>(
                                &step, 1)));
            }
            SequentialDetail::sortOwnedWitness(witness);
            if (!replaySequentialWitness(
                    instance, witness, floorValue))
            {
                return std::nullopt;
            }
            return witness;
        }

        inline std::vector<SequentialRowWitness> nodeWitness(
            const SequentialInstance & instance,
            std::span<const ThresholdPackedRow> rows,
            std::span<const std::int32_t> branches)
        {
            std::vector<SequentialRowWitness> witness;
            for (std::size_t packed = 0;
                 packed < branches.size();
                 ++packed)
            {
                const std::int32_t branch = branches[packed];
                if (branch <= 0)
                {
                    continue;
                }
                const ThresholdPackedRow & packedRow = rows[packed];
                const auto & option =
                    packedRow.options[
                        static_cast<std::size_t>(branch - 1)];
                witness.push_back(
                    SequentialDetail::ownedWitness(
                        instance,
                        instance.rows[
                            static_cast<std::size_t>(packedRow.row)],
                        packedRow.row,
                        option.steps));
            }
            SequentialDetail::sortOwnedWitness(witness);
            return witness;
        }

        inline void hashNode(
            std::uint64_t & hash,
            const ThresholdNode & node)
        {
            const auto mix = [&hash](std::uint64_t value)
            {
                constexpr std::uint64_t localPrime =
                    1099511628211ULL;
                for (std::int32_t byte = 0; byte < 8; ++byte)
                {
                    hash ^= value & 0xffU;
                    hash *= localPrime;
                    value >>= 8;
                }
            };
            mix(static_cast<std::uint64_t>(node.depth));
            mix(static_cast<std::uint64_t>(node.upper));
            for (const std::int32_t branch : node.branches)
            {
                mix(static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(branch)));
            }
        }

        inline void finishBounds(
            SequentialThresholdResult & result,
            MilliFunds closedUpper,
            MilliFunds openUpper)
        {
            result.residualUpper =
                std::max({result.feasibleLower,
                          closedUpper,
                          openUpper});
            result.searchCompleted =
                result.residualUpper == result.feasibleLower;
            result.thresholdProven =
                result.residualUpper < result.threshold;
        }

        inline void captureResidualNodes(
            SequentialThresholdResult & result,
            std::priority_queue<
                ThresholdNode,
                std::vector<ThresholdNode>,
                ThresholdNodeOrder> open,
            std::span<const ThresholdPackedRow> rows)
        {
            result.residualNodes.clear();
            while (!open.empty())
            {
                ThresholdNode node = open.top();
                open.pop();
                if (node.upper != result.threshold)
                {
                    continue;
                }
                SequentialThresholdOpenNodeAudit audit{
                    .depth = node.depth,
                    .fixedValue = node.value,
                    .upper = node.upper,
                    .rowSumUpper = node.value,
                    .usedNodes = node.usedMask.nodes(),
                    .branches = node.branches,
                };
                std::vector<std::int32_t> occurrences(
                    node.used.size(), 0);
                bool arithmeticValid = true;
                for (std::size_t row = node.depth;
                     row < rows.size();
                     ++row)
                {
                    const auto & packed = rows[row];
                    audit.remainingRows.push_back(packed.row);
                    for (std::size_t optionIndex = 0;
                         optionIndex < packed.options.size();
                         ++optionIndex)
                    {
                        const auto & option =
                            packed.options[optionIndex];
                        if (!compatible(option.nodes, node.used))
                        {
                            continue;
                        }
                        audit.optimisticOptions.push_back(
                            SequentialOptimisticOptionAudit{
                                .row = packed.row,
                                .optionIndex =
                                    static_cast<std::int32_t>(
                                        optionIndex),
                                .value = option.value,
                                .nodes = option.nodes,
                            });
                        for (const std::int32_t optionNode :
                             option.nodes)
                        {
                            ++occurrences[
                                static_cast<std::size_t>(
                                    optionNode)];
                        }
                        arithmeticValid =
                            arithmeticValid &&
                            checkedAdd(
                                audit.rowSumUpper,
                                option.value,
                                audit.rowSumUpper);
                        break;
                    }
                }
                if (!arithmeticValid)
                {
                    continue;
                }
                for (std::size_t nodeIndex = 0;
                     nodeIndex < occurrences.size();
                     ++nodeIndex)
                {
                    if (occurrences[nodeIndex] > 1)
                    {
                        audit.overlappingNodes.push_back(
                            static_cast<std::int32_t>(
                                nodeIndex));
                        audit.overlapCount +=
                            occurrences[nodeIndex] - 1;
                    }
                }
                audit.optimisticOptionsCompatible =
                    audit.overlapCount == 0;
                result.residualNodes.push_back(
                    std::move(audit));
            }
        }
    }

    inline SequentialThresholdResult proveSequentialBelow(
        const SequentialInstance & instance,
        MilliFunds floorValue,
        MilliFunds threshold,
        std::int64_t workLimit,
        SequentialDetail::SequentialRowOptionSource* pRowSource =
            nullptr,
        SequentialCacheAccounting cacheAccounting =
            SequentialCacheAccounting::RechargeEveryLookup,
        SequentialThresholdOptions options = {})
    {
        using namespace SequentialDetail;
        using namespace SequentialThresholdDetail;

        SequentialThresholdResult result;
        result.threshold = threshold;
        result.feasibleLower = floorValue;
        result.residualUpper = floorValue;
        if (!validInstance(instance) || workLimit < 0)
        {
            return result;
        }
        result.nodeColumns.assign(instance.nodeColumns.begin(),
                                  instance.nodeColumns.end());
        result.rowCatalogs.reserve(instance.rows.size());

        std::vector<std::int32_t> openNodes;
        for (std::int32_t node = 0;
             node < instance.nodeCount();
             ++node)
        {
            if (!instance.capturedNodes[
                    static_cast<std::size_t>(node)])
            {
                openNodes.push_back(node);
            }
        }
        const auto initialWitness =
            floorWitness(instance, openNodes, floorValue);
        if (!initialWitness)
        {
            result.stop = SequentialThresholdStop::InvalidWitness;
            return result;
        }
        result.witness = *initialWitness;
        result.witnessReplays = true;

        std::vector<std::vector<MilliFunds>> enriched(
            instance.rows.size());
        std::vector<MilliFunds> relaxedWeights;
        relaxedWeights.reserve(
            instance.rows.size() * openNodes.size());
        bool continuationSeen = false;
        for (std::size_t rowIndex = 0;
             rowIndex < instance.rows.size();
             ++rowIndex)
        {
            const SequentialRow & row = instance.rows[rowIndex];
            auto & rowEnriched = enriched[rowIndex];
            rowEnriched.assign(
                static_cast<std::size_t>(instance.nodeCount()), 0);
            for (const std::int32_t node : openNodes)
            {
                const std::size_t slot =
                    static_cast<std::size_t>(node);
                const MilliFunds gain =
                    continuationGain(instance, row, node);
                if (!checkedAdd(
                        row.weight[slot],
                        gain,
                        rowEnriched[slot]))
                {
                    return result;
                }
                continuationSeen = continuationSeen || gain > 0;
                relaxedWeights.push_back(rowEnriched[slot]);
            }
        }
        AssignmentSolver relaxed;
        relaxed.solve(
            static_cast<std::int32_t>(instance.rows.size()),
            static_cast<std::int32_t>(openNodes.size()),
            relaxedWeights);
        result.originalRelaxedUpper =
            std::max(floorValue, relaxed.optimum());
        result.residualUpper = result.originalRelaxedUpper;
        if (!continuationSeen ||
            result.originalRelaxedUpper == floorValue)
        {
            result.searchCompleted = true;
            result.thresholdProven = floorValue < threshold;
            result.status = result.thresholdProven
                ? SequentialThresholdStatus::ProvenBelowThreshold
                : SequentialThresholdStatus::CounterexampleWitness;
            result.stop = result.thresholdProven
                ? SequentialThresholdStop::Proven
                : SequentialThresholdStop::Counterexample;
            return result;
        }
        if (result.originalRelaxedUpper < threshold)
        {
            result.thresholdProven = true;
            result.status =
                SequentialThresholdStatus::ProvenBelowThreshold;
            result.stop = SequentialThresholdStop::Proven;
            return result;
        }
        if (floorValue >= threshold)
        {
            result.status =
                SequentialThresholdStatus::CounterexampleWitness;
            result.stop = SequentialThresholdStop::Counterexample;
            return result;
        }
        if (workLimit == 0)
        {
            result.status =
                SequentialThresholdStatus::UnresolvedAtBudget;
            result.stop = SequentialThresholdStop::WorkBudget;
            return result;
        }

        EnumerationBudget budget{
            .stateCap = workLimit,
        };
        std::vector<std::vector<RowOption>> ownedOptions;
        ownedOptions.reserve(instance.rows.size());
        std::vector<ThresholdPackedRow> packedRows;
        std::int64_t itinerariesEnumerated = 0;
        result.rowPhases.reserve(instance.rows.size());
        result.rowOptionDomainsComplete = true;
        for (std::int32_t rowIndex = 0;
             rowIndex <
                 static_cast<std::int32_t>(instance.rows.size());
             ++rowIndex)
        {
            SequentialRowPhaseAudit phase{
                .row = rowIndex,
                .cacheLookup = pRowSource != nullptr,
            };
            std::span<const RowOption> options;
            std::int64_t quoted = 0;
            const bool cached =
                pRowSource != nullptr &&
                pRowSource->lookup(rowIndex, options, quoted);
            if (cached && quoted < 0)
            {
                result.stop =
                    SequentialThresholdStop::InvalidCacheAccounting;
                return result;
            }
            phase.cacheQuotedStates = cached ? quoted : 0;
            result.cacheQuotedStates += phase.cacheQuotedStates;
            std::int64_t charged = quoted;
            if (cached &&
                cacheAccounting ==
                    SequentialCacheAccounting::OncePerIdentity)
            {
                if (!pRowSource->onceCharge(
                        rowIndex, quoted, charged) ||
                    charged < 0 || charged > quoted)
                {
                    result.stop =
                        SequentialThresholdStop::
                            InvalidCacheAccounting;
                    return result;
                }
            }
            const bool cacheFits =
                cached &&
                charged <= budget.stateCap - budget.states;
            if (cacheFits)
            {
                budget.states += charged;
                phase.cacheHit = true;
                phase.cacheChargedStates = charged;
                phase.complete = true;
                result.cacheChargedStates += charged;
                pRowSource->recordCharge(rowIndex);
            }
            else
            {
                const std::int64_t before = budget.states;
                ownedOptions.push_back(enumerateRowOptions(
                    instance,
                    rowIndex,
                    enriched[static_cast<std::size_t>(rowIndex)],
                    budget,
                    itinerariesEnumerated));
                options = ownedOptions.back();
                phase.constructionStates =
                    budget.states - before;
                result.rowConstructionStates +=
                    phase.constructionStates;
                phase.complete = !budget.truncated;
                if (pRowSource != nullptr && phase.complete)
                {
                    if (!cached)
                    {
                        pRowSource->store(
                            rowIndex,
                            options,
                            phase.constructionStates);
                    }
                    pRowSource->recordCharge(rowIndex);
                }
            }
            phase.uniqueOptions =
                static_cast<std::int64_t>(options.size());
            result.rowOptionDomainsComplete =
                result.rowOptionDomainsComplete && phase.complete;
            result.rowPhases.push_back(phase);
            result.rowCatalogs.push_back(
                SequentialRowCatalogAudit{
                    .row = rowIndex,
                    .options = std::vector<RowOption>(
                        options.begin(), options.end()),
                });
            if (!options.empty())
            {
                packedRows.push_back(ThresholdPackedRow{
                    .row = rowIndex,
                    .options = options,
                    .bestValue = options.front().value,
                });
            }
        }
        result.statesUsed = budget.states;
        result.workUsed = budget.states;
        if (!result.rowOptionDomainsComplete)
        {
            result.status =
                SequentialThresholdStatus::UnresolvedAtBudget;
            result.stop =
                SequentialThresholdStop::
                    RowOptionResidualBoundMissing;
            return result;
        }

        std::stable_sort(
            packedRows.begin(),
            packedRows.end(),
            [](const ThresholdPackedRow & left,
               const ThresholdPackedRow & right)
        {
            if (left.bestValue != right.bestValue)
            {
                return left.bestValue > right.bestValue;
            }
            return left.row < right.row;
        });

        SequentialPackingCatalog packingCatalog;
        if (options.pairPartitionUpper ||
            options.nodePriceDual)
        {
            packingCatalog = buildSequentialPackingCatalog(
                result.rowCatalogs,
                instance.nodeCount(),
                workLimit - result.workUsed);
            result.packingCatalogWork =
                packingCatalog.constructionWork;
            result.packingCatalogOptionRecords =
                packingCatalog.optionRecords;
            result.packingCatalogNodeIncidences =
                packingCatalog.nodeIncidences;
            result.workUsed += result.packingCatalogWork;
            result.packingCatalogComplete =
                packingCatalog.complete;
        }
        const auto applyPairUpper =
            [&](ThresholdNode & node,
                bool root) -> bool
        {
            if (!options.pairPartitionUpper ||
                !packingCatalog.complete ||
                node.upper < threshold)
            {
                return true;
            }
            std::vector<std::int32_t> remainingRows;
            remainingRows.reserve(
                packedRows.size() - node.depth);
            for (std::size_t row = node.depth;
                 row < packedRows.size();
                 ++row)
            {
                remainingRows.push_back(
                    packedRows[row].row);
            }
            std::sort(
                remainingRows.begin(),
                remainingRows.end());
            ++result.pairBoundCalls;
            auto certificate =
                sequentialPairPartitionUpper(
                    packingCatalog,
                    remainingRows,
                    node.usedMask,
                    node.value,
                    node.upper,
                    workLimit - result.workUsed);
            result.pairBoundWork +=
                certificate.deterministicOperationCost;
            result.workUsed +=
                certificate.deterministicOperationCost;
            if (certificate.completed)
            {
                ++result.pairBoundCompleted;
            }
            if (certificate.valid &&
                certificate.certifiedUpper < node.upper)
            {
                node.upper =
                    certificate.certifiedUpper;
                ++result.pairBoundTightenings;
            }
            if (root)
            {
                result.rootPairCertificate =
                    certificate;
                result.rootPairUpper = node.upper;
            }
            return certificate.completed;
        };
        const auto applyNodePriceDual =
            [&](ThresholdNode & node,
                bool root) -> bool
        {
            if (!options.nodePriceDual ||
                !packingCatalog.complete ||
                node.upper < threshold)
            {
                return true;
            }
            std::vector<std::int32_t> remainingRows;
            remainingRows.reserve(
                packedRows.size() - node.depth);
            for (std::size_t row = node.depth;
                 row < packedRows.size();
                 ++row)
            {
                remainingRows.push_back(
                    packedRows[row].row);
            }
            std::sort(
                remainingRows.begin(),
                remainingRows.end());
            ++result.dualCalls;
            const auto certificate =
                sequentialNodePriceDualUpper(
                    packingCatalog,
                    remainingRows,
                    node.usedMask,
                    node.value,
                    node.upper,
                    workLimit - result.workUsed);
            result.dualWork +=
                certificate.deterministicOperationCost;
            result.workUsed +=
                certificate.deterministicOperationCost;
            if (certificate.completed)
            {
                ++result.dualCompleted;
            }
            if (certificate.valid &&
                certificate.certifiedUpper < node.upper)
            {
                node.upper =
                    certificate.certifiedUpper;
                ++result.dualTightenings;
            }
            if (root)
            {
                result.rootDualCertificate =
                    certificate;
                result.rootDualUpper = node.upper;
            }
            return certificate.completed;
        };

        ThresholdNode root;
        root.used.assign(
            static_cast<std::size_t>(instance.nodeCount()), false);
        root.usedMask =
            SequentialNodeMask(instance.nodeCount());
        if (!nodeUpper(
                packedRows,
                0,
                0,
                root.used,
                result.originalRelaxedUpper,
                root.upper))
        {
            return result;
        }
        root.upper = std::max(root.upper, floorValue);
        result.rootPairUpper = root.upper;
        result.rootDualUpper = root.upper;
        applyPairUpper(root, true);
        applyNodePriceDual(root, true);
        std::priority_queue<
            ThresholdNode,
            std::vector<ThresholdNode>,
            ThresholdNodeOrder> open;
        open.push(std::move(root));
        MilliFunds closedUpper = floorValue;
        const auto finishAtWorkBudget = [&]()
        {
            const MilliFunds openUpper =
                open.empty()
                ? result.feasibleLower
                : open.top().upper;
            finishBounds(
                result, closedUpper, openUpper);
            result.status =
                SequentialThresholdStatus::
                    UnresolvedAtBudget;
            result.stop =
                SequentialThresholdStop::WorkBudget;
            result.openNodesAtStop =
                static_cast<std::int64_t>(open.size());
            if (options.captureResidualNodes)
            {
                captureResidualNodes(
                    result, open, packedRows);
            }
        };

        while (!open.empty())
        {
            const MilliFunds openUpper = open.top().upper;
            if (openUpper <= result.feasibleLower)
            {
                finishBounds(
                    result, closedUpper, result.feasibleLower);
                result.status = result.thresholdProven
                    ? SequentialThresholdStatus::
                        ProvenBelowThreshold
                    : SequentialThresholdStatus::
                        CounterexampleWitness;
                result.stop = result.thresholdProven
                    ? SequentialThresholdStop::Proven
                    : SequentialThresholdStop::Counterexample;
                result.openNodesAtStop =
                    static_cast<std::int64_t>(open.size());
                return result;
            }
            if (openUpper < threshold)
            {
                finishBounds(
                    result, closedUpper, openUpper);
                result.status =
                    SequentialThresholdStatus::
                        ProvenBelowThreshold;
                result.stop = SequentialThresholdStop::Proven;
                result.openNodesAtStop =
                    static_cast<std::int64_t>(open.size());
                return result;
            }
            if (result.workUsed >= workLimit)
            {
                finishAtWorkBudget();
                return result;
            }

            ThresholdNode node = open.top();
            open.pop();
            ++result.packingStates;
            ++result.statesUsed;
            ++result.workUsed;
            hashNode(result.expansionOrderHash, node);
            if (node.depth == packedRows.size())
            {
                ++result.packingLeaves;
                if (node.value > result.feasibleLower)
                {
                    auto witness = nodeWitness(
                        instance, packedRows, node.branches);
                    if (!replaySequentialWitness(
                            instance, witness, node.value))
                    {
                        result.status =
                            SequentialThresholdStatus::Invalid;
                        result.stop =
                            SequentialThresholdStop::InvalidWitness;
                        return result;
                    }
                    result.feasibleLower = node.value;
                    result.witness = std::move(witness);
                    result.witnessReplays = true;
                }
                if (node.value >= threshold)
                {
                    finishBounds(
                        result,
                        closedUpper,
                        open.empty() ? result.feasibleLower
                                     : open.top().upper);
                    result.status =
                        SequentialThresholdStatus::
                            CounterexampleWitness;
                    result.stop =
                        SequentialThresholdStop::Counterexample;
                    result.openNodesAtStop =
                        static_cast<std::int64_t>(open.size());
                    return result;
                }
                continue;
            }

            const ThresholdPackedRow & row =
                packedRows[node.depth];
            for (std::size_t optionIndex = 0;
                 optionIndex < row.options.size();
                 ++optionIndex)
            {
                if (result.workUsed >= workLimit)
                {
                    open.push(node);
                    finishAtWorkBudget();
                    return result;
                }
                const RowOption & option =
                    row.options[optionIndex];
                if (!compatible(option.nodes, node.used))
                {
                    continue;
                }
                ThresholdNode child = node;
                ++child.depth;
                if (!checkedAdd(
                        child.value,
                        option.value,
                        child.value))
                {
                    return result;
                }
                for (const std::int32_t used : option.nodes)
                {
                    child.used[
                        static_cast<std::size_t>(used)] = true;
                    if (!child.usedMask.set(used))
                    {
                        return result;
                    }
                }
                child.branches.push_back(
                    static_cast<std::int32_t>(optionIndex + 1));
                if (!nodeUpper(
                        packedRows,
                        child.depth,
                        child.value,
                        child.used,
                        result.originalRelaxedUpper,
                        child.upper))
                {
                    return result;
                }
                const bool pairCompleted =
                    applyPairUpper(child, false);
                if (!pairCompleted &&
                    result.workUsed >= workLimit)
                {
                    open.push(node);
                    finishAtWorkBudget();
                    return result;
                }
                if (child.upper <= result.feasibleLower)
                {
                    ++result.packingPrunes;
                }
                else if (child.upper < threshold)
                {
                    ++result.packingPrunes;
                    if (options.pairPartitionUpper)
                    {
                        ++result.pairBoundPrunes;
                    }
                    closedUpper =
                        std::max(closedUpper, child.upper);
                }
                else
                {
                    open.push(std::move(child));
                }
            }

            if (result.workUsed >= workLimit)
            {
                open.push(node);
                finishAtWorkBudget();
                return result;
            }
            ThresholdNode skipped = std::move(node);
            ++skipped.depth;
            skipped.branches.push_back(0);
            if (!nodeUpper(
                    packedRows,
                    skipped.depth,
                    skipped.value,
                    skipped.used,
                    result.originalRelaxedUpper,
                    skipped.upper))
            {
                return result;
            }
            const bool skippedPairCompleted =
                applyPairUpper(skipped, false);
            if (!skippedPairCompleted &&
                result.workUsed >= workLimit)
            {
                open.push(std::move(skipped));
                finishAtWorkBudget();
                return result;
            }
            if (skipped.upper <= result.feasibleLower)
            {
                ++result.packingPrunes;
            }
            else if (skipped.upper < threshold)
            {
                ++result.packingPrunes;
                if (options.pairPartitionUpper)
                {
                    ++result.pairBoundPrunes;
                }
                closedUpper =
                    std::max(closedUpper, skipped.upper);
            }
            else
            {
                open.push(std::move(skipped));
            }
        }

        finishBounds(result, closedUpper, result.feasibleLower);
        result.status = result.thresholdProven
            ? SequentialThresholdStatus::ProvenBelowThreshold
            : SequentialThresholdStatus::CounterexampleWitness;
        result.stop = result.thresholdProven
            ? SequentialThresholdStop::Proven
            : SequentialThresholdStop::Counterexample;
        return result;
    }
}
