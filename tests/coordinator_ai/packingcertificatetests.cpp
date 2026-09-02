#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/sequentialpackingcertificate.h"

namespace
{
    using namespace Coordinator;
    using SequentialDetail::RowOption;
    using SequentialDetail::WitnessStep;

    constexpr MilliFunds MILLION = 1000000;
    int checks = 0;
    int failures = 0;

    void expect(bool condition, std::string_view message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAILED: " << message << '\n';
        }
    }

    PropertyStockColumn column(std::int32_t slot)
    {
        return PropertyStockColumn{
            .slot = slot,
            .tile = TilePoint{slot, 0},
            .income = PropertyIncome{},
            .ownerBefore = OwnerSign::Neutral,
        };
    }

    RowOption option(
        MilliFunds value,
        std::initializer_list<std::int32_t> nodes)
    {
        RowOption result{
            .nodes = nodes,
            .value = value,
        };
        std::sort(result.nodes.begin(), result.nodes.end());
        for (const std::int32_t node : result.nodes)
        {
            result.steps.push_back(WitnessStep{node, 1});
        }
        return result;
    }

    SequentialRowCatalogAudit row(
        std::int32_t index,
        std::initializer_list<RowOption> options)
    {
        SequentialRowCatalogAudit result{
            .row = index,
            .options = options,
        };
        std::sort(
            result.options.begin(),
            result.options.end(),
            [](const RowOption & left,
               const RowOption & right)
        {
            if (left.value != right.value)
            {
                return left.value > right.value;
            }
            return left.nodes < right.nodes;
        });
        return result;
    }

    SequentialPackingCatalog catalogOf(
        std::span<const SequentialRowCatalogAudit> source,
        std::int32_t nodeCount)
    {
        return buildSequentialPackingCatalog(
            source,
            nodeCount,
            std::numeric_limits<std::int64_t>::max());
    }

    MilliFunds rowSumUpper(
        const SequentialPackingCatalog & catalog,
        std::span<const std::int32_t> rows)
    {
        MilliFunds result = 0;
        for (const std::int32_t rowIndex : rows)
        {
            const auto* pRow = catalog.find(rowIndex);
            if (pRow != nullptr && !pRow->options.empty())
            {
                result += pRow->options.front().pOption->value;
            }
        }
        return result;
    }

    MilliFunds exactPacking(
        const SequentialPackingCatalog & catalog,
        std::span<const std::int32_t> rows,
        const SequentialNodeMask & initiallyUsed =
            SequentialNodeMask())
    {
        SequentialNodeMask used =
            initiallyUsed.nodeCount() == catalog.nodeCount
            ? initiallyUsed
            : SequentialNodeMask(catalog.nodeCount);
        MilliFunds best = 0;
        const auto visit =
            [&](const auto & self,
                std::size_t depth,
                MilliFunds value,
                const SequentialNodeMask & occupied) -> void
        {
            if (depth == rows.size())
            {
                best = std::max(best, value);
                return;
            }
            self(self, depth + 1, value, occupied);
            const auto* pRow = catalog.find(rows[depth]);
            if (pRow == nullptr)
            {
                return;
            }
            for (const auto & candidate : pRow->options)
            {
                if (!occupied.disjoint(candidate.nodes))
                {
                    continue;
                }
                SequentialNodeMask next = occupied;
                if (next.merge(candidate.nodes))
                {
                    self(
                        self,
                        depth + 1,
                        value + candidate.pOption->value,
                        next);
                }
            }
        };
        visit(visit, 0, 0, used);
        return best;
    }

    struct RepairFixture
    {
        std::vector<PropertyStockColumn> columns{
            column(0),
            column(1),
            column(2),
        };
        std::array<std::int32_t, 9> arrivals{
            UNREACHABLE, UNREACHABLE, UNREACHABLE,
            UNREACHABLE, UNREACHABLE, UNREACHABLE,
            UNREACHABLE, UNREACHABLE, UNREACHABLE,
        };
        SequentialClassTable table;
        std::array<SequentialClassTable, 1> classes;
        SequentialInstance instance;
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {option(10, {0}), option(9, {1})}),
            row(1, {option(10, {0}), option(8, {2})}),
        };
        SequentialPackingCatalog catalog;
        std::vector<SequentialRowWitness> initialWitness;

        RepairFixture()
        {
            table.build(columns, arrivals, 1, 3);
            classes[0] = table;
            instance.horizonTurns = 3;
            instance.nodeColumns = columns;
            instance.classes = classes;
            instance.rows = {
                SequentialRow{
                    .classIndex = 0,
                    .weight = {10, 9, 0},
                    .ownedTurn = {1, 1, 1},
                },
                SequentialRow{
                    .classIndex = 0,
                    .weight = {10, 0, 8},
                    .ownedTurn = {1, 1, 1},
                },
            };
            instance.capturedNodes.assign(columns.size(), false);
            catalog = catalogOf(source, 3);
            const std::array<WitnessStep, 1> first{
                WitnessStep{1, 1},
            };
            const std::array<WitnessStep, 1> second{
                WitnessStep{2, 1},
            };
            initialWitness = {
                SequentialDetail::ownedWitness(
                    instance,
                    instance.rows[0],
                    0,
                    first),
                SequentialDetail::ownedWitness(
                    instance,
                    instance.rows[1],
                    1,
                    second),
            };
        }
    };

    void testPrimalRepair()
    {
        RepairFixture fixture;
        const auto result = repairSequentialPackingWitness(
            fixture.instance,
            fixture.catalog,
            fixture.initialWitness,
            17,
            3,
            1000);
        expect(
            result.valid &&
                result.replayValid &&
                result.lowerBefore == 17 &&
                result.lowerAfter == 19 &&
                result.witnessImprovements > 0 &&
                replaySequentialWitness(
                    fixture.instance,
                    result.witness,
                    result.lowerAfter),
            "PRIMAL_REPAIR_IMPROVES_REPLAYABLE_LOWER");

        const std::array<std::int32_t, 2> rows{0, 1};
        const MilliFunds exact =
            exactPacking(fixture.catalog, rows);
        expect(
            result.valid &&
                result.lowerAfter <= exact &&
                result.lowerAfter != 20,
            "PRIMAL_REPAIR_CANNOT_CREATE_FALSE_LOWER");
    }

    void testActionLabelInvariance()
    {
        RepairFixture fixture;
        const std::array<std::int32_t, 2> rows{0, 1};
        const SequentialNodeMask used(3);
        const std::array<std::string_view, 4> labels{
            "WAIT",
            "MOVE",
            "FIRE",
            "CAPTURE",
        };
        std::vector<MilliFunds> repairValues;
        std::vector<MilliFunds> pairValues;
        std::vector<MilliFunds> dualValues;
        for (const auto label : labels)
        {
            (void)label;
            repairValues.push_back(
                repairSequentialPackingWitness(
                    fixture.instance,
                    fixture.catalog,
                    fixture.initialWitness,
                    17,
                    3,
                    1000)
                    .lowerAfter);
            pairValues.push_back(
                sequentialPairPartitionUpper(
                    fixture.catalog,
                    rows,
                    used,
                    0,
                    20,
                    1000)
                    .certifiedUpper);
            dualValues.push_back(
                sequentialNodePriceDualUpper(
                    fixture.catalog,
                    rows,
                    used,
                    0,
                    20,
                    100000)
                    .certifiedUpper);
        }
        expect(
            std::adjacent_find(
                repairValues.begin(),
                repairValues.end(),
                std::not_equal_to<>()) ==
                    repairValues.end() &&
                std::adjacent_find(
                    pairValues.begin(),
                    pairValues.end(),
                    std::not_equal_to<>()) ==
                    pairValues.end() &&
                std::adjacent_find(
                    dualValues.begin(),
                    dualValues.end(),
                    std::not_equal_to<>()) ==
                    dualValues.end(),
            "ACTION_LABEL_INVARIANCE");
    }

    void testPairPartitionSoundness()
    {
        bool sound = true;
        for (std::int32_t mask = 0; mask < 16; ++mask)
        {
            std::vector<SequentialRowCatalogAudit> source{
                row(0, {
                    option(10 + (mask & 1), {0}),
                    option(7, {1}),
                }),
                row(1, {
                    option(9 + ((mask >> 1) & 1), {0}),
                    option(6, {2}),
                }),
                row(2, {
                    option(12 + ((mask >> 2) & 1), {1, 2}),
                    option(5 + ((mask >> 3) & 1), {2}),
                }),
            };
            const auto catalog = catalogOf(source, 3);
            const std::array<std::int32_t, 3> rows{0, 1, 2};
            const MilliFunds relaxed =
                rowSumUpper(catalog, rows);
            const MilliFunds exact =
                exactPacking(catalog, rows);
            const auto pair =
                sequentialPairPartitionUpper(
                    catalog,
                    rows,
                    SequentialNodeMask(3),
                    0,
                    relaxed,
                    100000);
            sound =
                sound &&
                pair.valid &&
                exact <= pair.pairPartitionUpper &&
                pair.pairPartitionUpper <=
                    pair.rowSumUpper;
        }

        std::vector<SequentialRowCatalogAudit> wideSource{
            row(0, {option(10, {65})}),
            row(1, {option(9, {65})}),
        };
        const auto wideCatalog =
            catalogOf(wideSource, 70);
        const std::array<std::int32_t, 2> wideRows{0, 1};
        const auto widePair =
            sequentialPairPartitionUpper(
                wideCatalog,
                wideRows,
                SequentialNodeMask(70),
                0,
                19,
                10000);
        sound =
            sound &&
            widePair.valid &&
            widePair.certifiedUpper == 10;
        expect(sound, "PAIR_PARTITION_UPPER_SOUNDNESS");
    }

    MilliFunds exactMatchingLoss(
        const SequentialPairPartitionResult & certificate)
    {
        MilliFunds best = 0;
        std::vector<bool> used(certificate.singleRows.size(), false);
        const auto lossOf =
            [&](std::int32_t first,
                std::int32_t second)
        {
            for (const auto & loss : certificate.losses)
            {
                if (loss.firstRow == first &&
                    loss.secondRow == second)
                {
                    return loss.loss;
                }
            }
            return MilliFunds{0};
        };
        const auto visit =
            [&](const auto & self,
                std::size_t first,
                MilliFunds value) -> void
        {
            while (first < used.size() && used[first])
            {
                ++first;
            }
            if (first == used.size())
            {
                best = std::max(best, value);
                return;
            }
            used[first] = true;
            self(self, first + 1, value);
            for (std::size_t second = first + 1;
                 second < used.size();
                 ++second)
            {
                if (used[second])
                {
                    continue;
                }
                used[second] = true;
                self(
                    self,
                    first + 1,
                    value + lossOf(
                        certificate.singleRows[first],
                        certificate.singleRows[second]));
                used[second] = false;
            }
            used[first] = false;
        };
        visit(visit, 0, 0);
        return best;
    }

    void testPairLossMatching()
    {
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {option(10, {0}), option(5, {2})}),
            row(1, {option(9, {0}), option(4, {3})}),
            row(2, {option(8, {1}), option(3, {2})}),
            row(3, {option(7, {1}), option(2, {3})}),
        };
        const auto catalog = catalogOf(source, 4);
        const std::array<std::int32_t, 4> rows{0, 1, 2, 3};
        const auto pair = sequentialPairPartitionUpper(
            catalog,
            rows,
            SequentialNodeMask(4),
            0,
            rowSumUpper(catalog, rows),
            100000);
        const bool lossesNonnegative =
            std::all_of(
                pair.losses.begin(),
                pair.losses.end(),
                [](const SequentialPairLoss & loss)
        {
            return loss.loss >= 0 &&
                loss.pairOptimum <=
                    loss.firstSingle + loss.secondSingle;
        });
        expect(
            pair.valid &&
                lossesNonnegative &&
                pair.matchingLoss ==
                    exactMatchingLoss(pair),
            "PAIR_LOSS_MATCHING_SOUNDNESS");
    }

    void testPairStrictThreshold()
    {
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {option(10, {0})}),
            row(1, {option(10, {0})}),
        };
        const auto catalog = catalogOf(source, 1);
        const std::array<std::int32_t, 2> rows{0, 1};
        const auto result = sequentialPairPartitionUpper(
            catalog,
            rows,
            SequentialNodeMask(1),
            0,
            20,
            10000);
        expect(
            result.valid &&
                result.rowSumUpper == 20 &&
                result.certifiedUpper == 10 &&
                result.certifiedUpper < 20,
            "PAIR_BOUND_STRICT_THRESHOLD_PROOF");
    }

    void testNodePriceDualSoundness()
    {
        bool sound = true;
        for (std::int32_t mask = 0; mask < 16; ++mask)
        {
            std::vector<SequentialRowCatalogAudit> source{
                row(0, {
                    option(10 + (mask & 1), {0, 1}),
                    option(6, {2}),
                }),
                row(1, {
                    option(9 + ((mask >> 1) & 1), {0}),
                    option(7, {1, 2}),
                }),
                row(2, {
                    option(8 + ((mask >> 2) & 1), {1}),
                    option(5 + ((mask >> 3) & 1), {2}),
                }),
            };
            const auto catalog = catalogOf(source, 3);
            const std::array<std::int32_t, 3> rows{0, 1, 2};
            const MilliFunds relaxed =
                rowSumUpper(catalog, rows);
            const auto dual = sequentialNodePriceDualUpper(
                catalog,
                rows,
                SequentialNodeMask(3),
                0,
                relaxed,
                1000000);
            const MilliFunds exact =
                exactPacking(catalog, rows);
            sound =
                sound &&
                dual.valid &&
                dual.dualCertificateValid &&
                exact <= dual.finalDualUpper &&
                dual.certifiedUpper <= relaxed;
        }
        expect(sound, "NODE_PRICE_DUAL_SOUNDNESS");
    }

    void testInvalidDualCertificates()
    {
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {option(10, {0}), option(6, {1})}),
            row(1, {option(9, {0}), option(5, {1})}),
        };
        const auto catalog = catalogOf(source, 2);
        const std::array<std::int32_t, 2> rows{0, 1};
        const auto dual = sequentialNodePriceDualUpper(
            catalog,
            rows,
            SequentialNodeMask(2),
            0,
            19,
            100000);
        auto falseMaxima = dual.rowReducedMaxima;
        falseMaxima[0] =
            std::max<MilliFunds>(0, falseMaxima[0] - 1);
        const auto invalid = verifySequentialNodePriceDual(
            catalog,
            rows,
            SequentialNodeMask(2),
            0,
            dual.nodePrices,
            100000,
            falseMaxima,
            &dual.finalDualUpper);
        expect(
            invalid.completed && !invalid.valid,
            "INVALID_DUAL_CERTIFICATE_FAILS_CLOSED");

        auto negativePrices = dual.nodePrices;
        negativePrices[0] = -1;
        const auto negative = verifySequentialNodePriceDual(
            catalog,
            rows,
            SequentialNodeMask(2),
            0,
            negativePrices,
            100000);
        expect(
            negative.completed &&
                !negative.valid &&
                !negative.pricesNonnegative,
            "NONNEGATIVE_PRICE_REQUIREMENT");

        const auto capped = sequentialNodePriceDualUpper(
            catalog,
            rows,
            SequentialNodeMask(2),
            0,
            19,
            1);
        expect(
            !capped.valid &&
                !capped.completed &&
                capped.deterministicOperationCost ==
                    capped.optimizerWork &&
                capped.deterministicOperationCost <= 1,
            "dual budget exhaustion fails closed");
    }

    void testCombinedUpperAndResidualFixture()
    {
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {
                option(40 * MILLION, {0}),
                option(15 * MILLION, {1}),
            }),
            row(1, {
                option(35 * MILLION, {0}),
                option(10 * MILLION, {2}),
            }),
            row(2, {
                option(28 * MILLION, {0}),
                option(5 * MILLION, {3}),
            }),
        };
        const auto catalog = catalogOf(source, 4);
        const std::array<std::int32_t, 3> rows{0, 1, 2};
        constexpr MilliFunds current = 103 * MILLION;
        const auto pair = sequentialPairPartitionUpper(
            catalog,
            rows,
            SequentialNodeMask(4),
            0,
            current,
            100000);
        const auto dual = sequentialNodePriceDualUpper(
            catalog,
            rows,
            SequentialNodeMask(4),
            0,
            current,
            1000000);
        const MilliFunds combined =
            std::min({
                current,
                pair.certifiedUpper,
                dual.certifiedUpper,
            });
        expect(
            combined <= current &&
                combined <= pair.certifiedUpper &&
                combined <= dual.certifiedUpper,
            "MONOTONE_COMBINED_UPPER");
        expect(
            rowSumUpper(catalog, rows) == current &&
                exactPacking(catalog, rows) <=
                    pair.certifiedUpper &&
                pair.certifiedUpper < current,
            "RESIDUAL_103_CONFLICT_FIXTURE");
    }

    void testDeterministicRepeat()
    {
        std::vector<SequentialRowCatalogAudit> source{
            row(0, {option(12, {0, 1}), option(7, {2})}),
            row(1, {option(11, {0}), option(8, {1, 2})}),
            row(2, {option(9, {1}), option(6, {2})}),
        };
        const auto catalog = catalogOf(source, 3);
        const std::array<std::int32_t, 3> rows{0, 1, 2};
        const auto first = sequentialNodePriceDualUpper(
            catalog,
            rows,
            SequentialNodeMask(3),
            0,
            32,
            1000000);
        bool identical = first.valid;
        for (std::int32_t repeat = 0; repeat < 3; ++repeat)
        {
            const auto next = sequentialNodePriceDualUpper(
                catalog,
                rows,
                SequentialNodeMask(3),
                0,
                32,
                1000000);
            identical =
                identical &&
                next.valid &&
                next.finalDualUpper ==
                    first.finalDualUpper &&
                next.nodePrices == first.nodePrices &&
                next.rowReducedMaxima ==
                    first.rowReducedMaxima &&
                next.deterministicOperationCost ==
                    first.deterministicOperationCost;
        }
        expect(identical, "DETERMINISTIC_REPEAT");
    }
}

int main()
{
    testPrimalRepair();
    testActionLabelInvariance();
    testPairPartitionSoundness();
    testPairLossMatching();
    testPairStrictThreshold();
    testNodePriceDualSoundness();
    testInvalidDualCertificates();
    testCombinedUpperAndResidualFixture();
    testDeterministicRepeat();
    std::cout << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
