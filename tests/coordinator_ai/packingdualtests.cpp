#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/packingdualcertificate.h"

namespace
{
    using namespace Coordinator;
    using SequentialDetail::RowOption;

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
            .tile = TilePoint{slot, slot + 1},
            .income = PropertyIncome{
                .oursPerTurn = 1000,
                .enemyPerTurn = 1000,
            },
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
        std::sort(
            result.nodes.begin(),
            result.nodes.end());
        return result;
    }

    struct Fixture
    {
        std::vector<std::vector<RowOption>> options{
            {
                option(10, {0}),
                option(7, {1}),
            },
            {
                option(9, {0}),
                option(6, {2}),
            },
        };
        PackingDualProblemIdentity identity;
        std::vector<PackingDualRowSource> sources;
        PackingDualCatalog catalog;

        Fixture()
        {
            identity.session = 7;
            identity.key = {
                CanonicalPlanActionKey::fromAction(
                    0, 1, 1, false),
            };
            identity.rowDomains = {{0}, {1}};
            identity.nodeColumns = {
                column(0),
                column(1),
                column(2),
            };
            identity.immutableField.ourColumns =
                identity.nodeColumns;
            identity.usedNodes =
                PackingDualNodeMask(3);
            for (std::size_t row = 0;
                 row < options.size();
                 ++row)
            {
                sources.push_back(PackingDualRowSource{
                    .row =
                        static_cast<std::int32_t>(row),
                    .domain = identity.rowDomains[row],
                    .options = options[row],
                });
            }
            catalog = buildPackingDualCatalog(
                sources,
                identity,
                1000);
        }
    };

    void testCatalogAndVerification()
    {
        Fixture fixture;
        expect(
            fixture.catalog.complete &&
                fixture.catalog.optionRecords == 4 &&
                fixture.catalog.nodeIncidences == 4 &&
                fixture.catalog.constructionWork == 4,
            "COMPLETE_CATALOG_REQUIRED");

        const std::array<MilliFunds, 3> prices{
            5, 0, 0,
        };
        const auto verified =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                prices,
                packingDualVerifierWork(
                    fixture.catalog));
        expect(
            verified.valid &&
                verified.certificate.dualObjective == 18 &&
                verified.certificate.rowReducedMaxima ==
                    std::vector<MilliFunds>{7, 6},
            "VERIFIED_DUAL_UPPER_ACCEPTS_STRICT_CHALLENGER");
        expect(
            verified.nodePricesChecked == 3 &&
                verified.optionRecordsScanned == 4 &&
                verified.optionIncidencesScanned == 4 &&
                verified.workUsed == 11,
            "EVERY_OPTION_AND_INCIDENCE_SCANNED");

        const auto replayed =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                prices,
                packingDualVerifierWork(
                    fixture.catalog),
                &verified.certificate);
        expect(
            replayed.valid &&
                replayed.certificate ==
                    verified.certificate,
            "EXACT_REDUCED_MAXIMA_AND_OBJECTIVE");

        auto falseMaximum =
            verified.certificate;
        --falseMaximum.rowReducedMaxima[0];
        expect(
            !verifyPackingDualCertificate(
                 fixture.catalog,
                 fixture.sources,
                 fixture.identity,
                 prices,
                 packingDualVerifierWork(
                     fixture.catalog),
                 &falseMaximum)
                 .valid,
            "FALSE_ROW_MAXIMUM_REJECTED");

        auto falseObjective =
            verified.certificate;
        --falseObjective.dualObjective;
        expect(
            !verifyPackingDualCertificate(
                 fixture.catalog,
                 fixture.sources,
                 fixture.identity,
                 prices,
                 packingDualVerifierWork(
                     fixture.catalog),
                 &falseObjective)
                 .valid,
            "FALSE_DUAL_OBJECTIVE_REJECTED");
    }

    void testFailureBoundaries()
    {
        Fixture fixture;
        const std::array<MilliFunds, 3> prices{
            5, 0, 0,
        };
        const auto incomplete =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                prices,
                packingDualVerifierWork(
                    fixture.catalog) -
                    1);
        expect(
            !incomplete.completed &&
                !incomplete.valid,
            "INCOMPLETE_VERIFIER_FAILS_CLOSED");

        const auto incompleteCatalog =
            buildPackingDualCatalog(
                fixture.sources,
                fixture.identity,
                fixture.catalog.constructionWork -
                    1);
        expect(
            !incompleteCatalog.complete &&
                !verifyPackingDualCertificate(
                     incompleteCatalog,
                     fixture.sources,
                     fixture.identity,
                     prices,
                     1000)
                     .valid,
            "INCOMPLETE_CATALOG_HAS_NO_AUTHORITY");

        auto mismatchedSources =
            fixture.sources;
        mismatchedSources[0].domain =
            fixture.identity.rowDomains[1];
        expect(
            !buildPackingDualCatalog(
                 mismatchedSources,
                 fixture.identity,
                 1000)
                 .complete,
            "CATALOG_ROW_DOMAIN_IDENTITY_REQUIRED");

        auto longerIdentityDomain = fixture.identity;
        longerIdentityDomain.rowDomains[0].push_back(99);
        expect(
            !buildPackingDualCatalog(
                 fixture.sources,
                 longerIdentityDomain,
                 1000)
                 .complete,
            "CATALOG_ROW_DOMAIN_LENGTH_REQUIRED");

        auto falseCounts =
            fixture.catalog;
        ++falseCounts.optionRecords;
        expect(
            !verifyPackingDualCertificate(
                 falseCounts,
                 fixture.sources,
                 fixture.identity,
                 prices,
                 packingDualVerifierWork(
                     falseCounts))
                 .valid,
            "CATALOG_COUNTERS_RECOMPUTED_BY_VERIFIER");

        auto falseOptionIndex =
            fixture.catalog;
        ++falseOptionIndex.rows[0].options[0].index;
        expect(
            !verifyPackingDualCertificate(
                 falseOptionIndex,
                 fixture.sources,
                 fixture.identity,
                 prices,
                 packingDualVerifierWork(
                     falseOptionIndex))
                 .valid,
            "CATALOG_OPTION_IDENTITY_RECOMPUTED_BY_VERIFIER");

        auto omittedOption =
            fixture.catalog;
        omittedOption.rows[0].options.erase(
            omittedOption.rows[0].options.begin());
        omittedOption.rows[0].options[0].index = 0;
        --omittedOption.optionRecords;
        --omittedOption.nodeIncidences;
        expect(
            !verifyPackingDualCertificate(
                 omittedOption,
                 fixture.sources,
                 fixture.identity,
                 prices,
                 packingDualVerifierWork(
                     omittedOption))
                 .valid,
            "CATALOG_SOURCE_OMISSION_REJECTED");

        std::array<MilliFunds, 3> negative{
            -1, 0, 0,
        };
        const auto negativeResult =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                negative,
                1000);
        expect(
            negativeResult.completed &&
                !negativeResult.valid &&
                !negativeResult.pricesNonnegative,
            "NEGATIVE_PRICE_REJECTED");

        auto usedIdentity = fixture.identity;
        usedIdentity.capturedColumns = {0};
        usedIdentity.usedNodes.set(0);
        auto usedCatalog =
            buildPackingDualCatalog(
                fixture.sources,
                usedIdentity,
                1000);
        const auto usedPrice =
            verifyPackingDualCertificate(
                usedCatalog,
                fixture.sources,
                usedIdentity,
                prices,
                1000);
        expect(
            usedPrice.completed &&
                !usedPrice.valid &&
                !usedPrice.pricesNonnegative,
            "USED_NODE_PRICE_RULE");

        auto falseUsedIdentity =
            fixture.identity;
        falseUsedIdentity.usedNodes.set(1);
        auto falseUsedCatalog =
            fixture.catalog;
        falseUsedCatalog.identity =
            falseUsedIdentity;
        expect(
            !verifyPackingDualCertificate(
                 falseUsedCatalog,
                 fixture.sources,
                 falseUsedIdentity,
                 prices,
                 1000)
                 .valid,
            "FIXED_USED_NODE_STATE_REQUIRED");

        auto staleIdentity = fixture.identity;
        staleIdentity.key = {
            CanonicalPlanActionKey::fromAction(
                0, 2, 1, false),
        };
        expect(
            !verifyPackingDualCertificate(
                 fixture.catalog,
                 fixture.sources,
                 staleIdentity,
                 prices,
                 1000)
                 .valid,
            "CERTIFICATE_PLAN_IDENTITY");

        auto staleField = fixture.identity;
        staleField.immutableField.horizonTurns = 11;
        expect(
            !verifyPackingDualCertificate(
                 fixture.catalog,
                 fixture.sources,
                 staleField,
                 prices,
                 1000)
                 .valid,
            "CERTIFICATE_FIELD_IDENTITY");

        auto staleEconomic = fixture.identity;
        staleEconomic.economic = 1;
        expect(
            !verifyPackingDualCertificate(
                 fixture.catalog,
                 fixture.sources,
                 staleEconomic,
                 prices,
                 1000)
                 .valid,
            "CERTIFICATE_ECONOMIC_IDENTITY");

        auto contradictoryIdentity =
            fixture.identity;
        contradictoryIdentity.incumbentFloor = 19;
        const auto contradictoryCatalog =
            buildPackingDualCatalog(
                fixture.sources,
                contradictoryIdentity,
                1000);
        const auto contradictory =
            verifyPackingDualCertificate(
                contradictoryCatalog,
                fixture.sources,
                contradictoryIdentity,
                prices,
                packingDualVerifierWork(
                    contradictoryCatalog));
        expect(
            contradictory.completed &&
                !contradictory.valid &&
                contradictory.certificate.dualObjective == 18,
            "DUAL_BELOW_FEASIBLE_FLOOR_FAILS_CLOSED");
    }

    void testOptimizer()
    {
        Fixture fixture;
        const auto capped =
            optimizePackingDualPrices(
                fixture.catalog,
                fixture.identity,
                1);
        expect(
            !capped.completed &&
                capped.workUsed <= 1,
            "OPTIMIZER_OUTPUT_HAS_NO_AUTHORITY_BEFORE_VERIFICATION");

        const auto first =
            optimizePackingDualPrices(
                fixture.catalog,
                fixture.identity,
                100000);
        bool deterministic = first.completed;
        for (std::int32_t repeat = 0;
             repeat < 3;
             ++repeat)
        {
            const auto next =
                optimizePackingDualPrices(
                    fixture.catalog,
                    fixture.identity,
                    100000);
            deterministic =
                deterministic &&
                next.completed &&
                next.nodePrices ==
                    first.nodePrices &&
                next.workUsed ==
                    first.workUsed &&
                next.iterations ==
                    first.iterations;
        }
        expect(
            deterministic,
            "DETERMINISTIC_PRICE_OPTIMIZER");

        const auto verified =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                first.nodePrices,
                packingDualVerifierWork(
                    fixture.catalog));
        expect(
            verified.valid,
            "OPTIMIZER_PRICES_REQUIRE_FULL_VERIFIER");
        const std::int64_t verifierWork =
            packingDualVerifierWork(
                fixture.catalog);
        const std::int64_t sharedAllowance =
            first.workUsed + verifierWork;
        const auto reservedOptimizer =
            optimizePackingDualPrices(
                fixture.catalog,
                fixture.identity,
                sharedAllowance - verifierWork);
        const auto reservedVerification =
            verifyPackingDualCertificate(
                fixture.catalog,
                fixture.sources,
                fixture.identity,
                reservedOptimizer.nodePrices,
                sharedAllowance -
                    reservedOptimizer.workUsed);
        expect(
            reservedOptimizer.completed &&
                reservedVerification.valid &&
                reservedOptimizer.workUsed +
                        reservedVerification.workUsed ==
                    sharedAllowance &&
                reservedVerification.workUsed ==
                    verifierWork,
            "VERIFIER_WORK_RESERVED");

    }

    void testArithmeticAndWideMask()
    {
        Fixture fixture;
        auto overflowIdentity =
            fixture.identity;
        overflowIdentity.fixedValue =
            std::numeric_limits<MilliFunds>::max();
        auto overflowCatalog =
            buildPackingDualCatalog(
                fixture.sources,
                overflowIdentity,
                1000);
        const std::array<MilliFunds, 3> zero{};
        const auto overflow =
            verifyPackingDualCertificate(
                overflowCatalog,
                fixture.sources,
                overflowIdentity,
                zero,
                1000);
        expect(
            !overflow.valid &&
                !overflow.checkedArithmetic,
            "CHECKED_DUAL_ARITHMETIC");
        expect(
            !overflow.valid,
            "OVERFLOW_FAILS_CLOSED");

        std::vector<std::vector<RowOption>>
            wideOptions{
                {option(10, {65})},
                {option(9, {65})},
            };
        PackingDualProblemIdentity wideIdentity;
        wideIdentity.session = 9;
        wideIdentity.rowDomains = {{0}, {1}};
        for (std::int32_t node = 0;
             node < 70;
             ++node)
        {
            wideIdentity.nodeColumns.push_back(
                column(node));
        }
        wideIdentity.immutableField.ourColumns =
            wideIdentity.nodeColumns;
        wideIdentity.usedNodes =
            PackingDualNodeMask(70);
        std::vector<PackingDualRowSource>
            wideSources{
                {0, wideIdentity.rowDomains[0], wideOptions[0]},
                {1, wideIdentity.rowDomains[1], wideOptions[1]},
            };
        const auto wideCatalog =
            buildPackingDualCatalog(
                wideSources,
                wideIdentity,
                1000);
        std::vector<MilliFunds> prices(70, 0);
        prices[65] = 9;
        const auto wide =
            verifyPackingDualCertificate(
                wideCatalog,
                wideSources,
                wideIdentity,
                prices,
                packingDualVerifierWork(
                    wideCatalog));
        expect(
            wide.valid &&
                wide.certificate.dualObjective == 10,
            "MORE_THAN_64_PROPERTY_NODES");
    }
}

int main()
{
    testCatalogAndVerification();
    testFailureBoundaries();
    testOptimizer();
    testArithmeticAndWideMask();
    std::cout << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
