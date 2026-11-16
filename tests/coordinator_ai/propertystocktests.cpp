#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ai/coordinator/masterplanpricing.h"
#include "ai/coordinator/propertystock.h"
#include "ai/coordinator/propertystockbuilder.h"
#include "ai/coordinator/replystockcontext.h"
#include "tests/coordinator_ai/propertystockdiagnosticfixture.h"

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

    std::vector<MilliFunds> referenceCeilings(const Coordinator::PropertyStockField & field,
                                              std::span<const Coordinator::ContinuationKey> keys,
                                              std::span<const MilliFunds> prices)
    {
        using namespace Coordinator;
        const auto reference = JointPlanStockValuer::diagnosticPricingModels(field, keys);
        std::vector<MilliFunds> ceilings;
        for (const auto & model : reference.models)
        {
            RefinementLedger ledger;
            ledger.open(MASTER_PLAN_PRICING_WORK);
            const auto priced = model ? priceMasterPlan(*model, prices, ledger) : MasterPlanCeiling{};
            expect(priced.status == MasterPlanPricingStatus::Valid,
                   "the diagnostic dual model prices every fixture key");
            ceilings.push_back(priced.ceiling);
        }
        return ceilings;
    }

    Coordinator::MasterPlanPricingLimits pricingLimits(std::int32_t rounds = Coordinator::MASTER_PLAN_PRICING_ROUNDS)
    {
        using namespace Coordinator;
        return MasterPlanPricingLimits{rounds, MASTER_PLAN_PRICING_WORK, MASTER_PLAN_PRICING_WITNESS_WORK};
    }

    Coordinator::MasterPlanPricingResult existingTune(const Coordinator::PropertyStockField & field,
                                                      const Coordinator::ContinuationKey & key,
                                                      std::span<const MilliFunds> seed = {},
                                                      std::int64_t* pGranted = nullptr,
                                                      const Coordinator::MasterPlanPricingLimits & limits =
                                                          pricingLimits())
    {
        using namespace Coordinator;
        const auto reference = JointPlanStockValuer::diagnosticPricingModels(field, std::span(&key, 1));
        RefinementLedger ledger;
        if (!reference.models.front() || !ledger.open(2 * MASTER_PLAN_PRICING_WORK))
        {
            return {};
        }
        auto result = tuneMasterPlanPrices(*reference.models.front(), limits, ledger, {}, seed);
        if (pGranted != nullptr)
        {
            *pGranted = ledger.stats().granted;
        }
        return result;
    }

    bool sameOutcome(const Coordinator::MasterPlanPricingResult & left,
                     const Coordinator::MasterPlanPricingResult & right)
    {
        return left.status == right.status && left.prices == right.prices && left.ceiling == right.ceiling &&
            left.floor == right.floor && left.rounds == right.rounds && left.witness.valid == right.witness.valid &&
            left.witness.candidateIndices == right.witness.candidateIndices &&
            left.witness.rows == right.witness.rows && left.witness.value == right.witness.value;
    }

    bool sameTune(const Coordinator::MasterPlanPricingResult & left, const Coordinator::MasterPlanPricingResult & right)
    {
        return sameOutcome(left, right) && left.stoppedOnTarget == right.stoppedOnTarget &&
            left.pricingWork == right.pricingWork && left.witnessWork == right.witnessWork;
    }

    std::vector<MilliFunds> tunedPrices(const Coordinator::PropertyStockField & field,
                                        const Coordinator::ContinuationKey & key)
    {
        return existingTune(field, key).prices;
    }

    void stockContextMatchesDiagnosticCeilings()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::size_t NODES = 2;
        constexpr std::array FIXED_PRICES{MilliFunds{3}, MilliFunds{1}};
        // Above every row weight and column stream, so each relaxed row maximum is the empty itinerary.
        constexpr MilliFunds PROHIBITIVE = 1000;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto waiting = CanonicalPlanActionKey::fromAction(0, 1, 0, false);
        const auto moved = CanonicalPlanActionKey::fromAction(0, 0, 0, false);
        const auto stalled = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, Fixture::RATE, false);
        const auto second = CanonicalPlanActionKey::fromAction(1, Fixture::WIDTH - 1, 0, false);
        struct Case
        {
            PropertyStockField field;
            std::vector<ContinuationKey> keys;
        };
        const std::array cases{
            Case{Fixture::make(2), {{today}, {waiting}, {moved}, {damaged, second}, {stalled, second},
                                    {dead, second}, {second}, {today, second}}},
            Case{Fixture::make(1, Fixture::RATE), {{today}, {moved}, {dead}, {damaged}}},
        };
        for (const auto & testCase : cases)
        {
            const auto & keys = testCase.keys;
            const auto tuned = tunedPrices(testCase.field, keys.front());
            expect(tuned.size() == NODES && std::any_of(tuned.begin(), tuned.end(), [](MilliFunds p) { return p > 0; }),
                   "tuning moves the fixture prices off zero");
            const auto quotes = JointPlanStockValuer::diagnosticQuotes(testCase.field, keys, SEQUENTIAL_ONLY);
            for (const auto & prices : {std::vector<MilliFunds>(NODES, 0),
                                        std::vector<MilliFunds>(FIXED_PRICES.begin(), FIXED_PRICES.end()), tuned})
            {
                const auto expected = referenceCeilings(testCase.field, keys, prices);
                ReplyStockContext forward(testCase.field);
                ReplyStockContext backward(testCase.field);
                expect(forward.installPrices(prices).status == ReplyStockStatus::Valid &&
                       backward.installPrices(prices).status == ReplyStockStatus::Valid,
                       "the context installs a nonnegative price vector over its nodes");
                std::vector<ReplyStockCeiling> later(keys.size());
                for (std::size_t index = keys.size(); index-- > 0;)
                {
                    later[index] = backward.ceiling(keys[index]);
                }
                for (std::size_t index = 0; index < keys.size(); ++index)
                {
                    const auto early = forward.ceiling(keys[index]);
                    const auto owned = forward.owned(keys[index]);
                    expect(early.status == ReplyStockStatus::Valid && later[index].status == ReplyStockStatus::Valid &&
                           early.ceiling == expected[index] && later[index].ceiling == expected[index],
                           "the context ceiling equals the diagnostic dual ceiling in either order of asking");
                    expect(owned.status == ReplyStockStatus::Valid && owned.owned == quotes.quotes[index].owned,
                           "the context owned value equals the diagnostic quote");
                }
            }
        }

        ReplyStockContext open(Fixture::make(1));
        open.installPrices(std::vector<MilliFunds>(NODES, 0));
        expect(open.ceiling({dead}).ceiling == 0 && open.ceiling({today}).ceiling > 0,
               "a destroyed actor is an explicit zero row, not its idle day row");
        ReplyStockContext completed(Fixture::make(1, Fixture::RATE));
        completed.installPrices(std::vector<MilliFunds>(NODES, PROHIBITIVE));
        expect(completed.ceiling({today}).ceiling == PROHIBITIVE &&
                   completed.ceiling({moved}).ceiling == 2 * PROHIBITIVE,
               "the ceiling is the price sum less the price of the node captured this turn");
    }

    void stockContextExtensionKeepsAnswers()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::array FIXED_PRICES{MilliFunds{3}, MilliFunds{1}};
        constexpr std::array NEW_PRICES{MilliFunds{5}, MilliFunds{2}};
        constexpr std::array REJECTED_PRICES{MilliFunds{-1}, MilliFunds{0}};
        constexpr std::size_t NEW_RATE_CLASSES = 2;
        constexpr std::int64_t KEY_ROWS = 2;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto second = CanonicalPlanActionKey::fromAction(1, Fixture::WIDTH - 1, 0, false);
        const auto field = Fixture::make(2);
        const auto before = Fixture::snapshot(field);
        ReplyStockContext context(field);
        expect(context.valid() && context.nodeCount() == FIXED_PRICES.size() && context.classCount() == 1 &&
               context.actionCount() == 0 && context.ceiling({today}).status == ReplyStockStatus::NoPrices,
               "a fresh context holds only the day rows and refuses a ceiling before prices");
        expect(context.installPrices(FIXED_PRICES).version == 1, "an installed price vector gets a version");
        const auto first = context.ceiling({today});
        const auto other = context.ceiling({second});
        const auto classes = context.classCount();
        const auto damagedCeiling = context.ceiling({damaged, second});
        const auto deadCeiling = context.ceiling({dead, second});
        expect(damagedCeiling.counters.classesAdded == 1 && deadCeiling.counters.classesAdded == 1 &&
               context.classCount() == classes + NEW_RATE_CLASSES && damagedCeiling.counters.classTablesPriced == 1,
               "a damaged rate extends the class set in place and prices only its own table");
        const auto again = context.ceiling({today});
        const auto otherAgain = context.ceiling({second});
        expect(again.status == ReplyStockStatus::Valid && again.ceiling == first.ceiling &&
               otherAgain.ceiling == other.ceiling && again.counters.rowScans == 0 &&
               again.counters.classTablesPriced == 0 && again.counters.actionsAdded == 0 && again.work > 0 &&
               again.counters.rowHits == KEY_ROWS && again.counters.tableHits == 0 &&
               first.counters.rowHits == 0 && first.counters.tableHits == 1,
               "extending the context leaves earlier answers and their cached rows unchanged");
        const std::vector<ContinuationKey> earlier{{today}, {second}, {damaged, second}, {dead, second}};
        const auto expected = referenceCeilings(field, earlier, NEW_PRICES);
        expect(context.installPrices(NEW_PRICES).version == 2, "a new price vector gets the next version");
        std::vector<ReplyStockCeiling> repriced;
        for (const auto & key : earlier)
        {
            repriced.push_back(context.ceiling(key));
        }
        bool matches = true;
        for (std::size_t index = 0; index < earlier.size(); ++index)
        {
            matches = matches && repriced[index].status == ReplyStockStatus::Valid &&
                repriced[index].ceiling == expected[index] && repriced[index].priceVersion == 2;
        }
        expect(matches && repriced.front().counters.rowScans == KEY_ROWS &&
               repriced.front().counters.classTablesPriced == 1 && repriced.front().counters.rowHits == 0 &&
               repriced[2].counters.classTablesPriced == 1,
               "new prices discard the warm caches and reprice the earlier keys from scratch");
        const auto rejected = context.installPrices(REJECTED_PRICES);
        const auto wrongSize = context.installPrices(std::span(NEW_PRICES).first(1));
        const auto kept = context.ceiling({today});
        expect(rejected.status == ReplyStockStatus::Invalid && rejected.version == 2 &&
               wrongSize.status == ReplyStockStatus::Invalid && context.priceVersion() == 2 &&
               kept.priceVersion == 2 && kept.ceiling == expected.front() && kept.counters.rowScans == 0,
               "a rejected price vector keeps the previous version and its answers");
        expect(Fixture::unchanged(field, before), "the context works on its own copy of the field");
    }

    bool ascendingWitness(const Coordinator::ReplyStockFloor & floor)
    {
        MilliFunds total = 0;
        std::int32_t previous = NO_STOCK_ROW;
        for (const auto & row : floor.witness)
        {
            if (row.row <= previous || row.steps.empty())
            {
                return false;
            }
            previous = row.row;
            total += row.value;
        }
        return total == floor.floor;
    }

    void stockContextFloorBracketsExact()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::size_t NODES = 2;
        constexpr std::array FIXED_PRICES{MilliFunds{3}, MilliFunds{1}};
        constexpr std::array EXACT{MilliFunds{20}, MilliFunds{18}, MilliFunds{12}};
        constexpr std::size_t DEAD_INDEX = 2;
        constexpr std::int32_t DEAD_HEAD_TURN = 3;
        constexpr MilliFunds DEAD_HEAD_WEIGHT = 10;
        constexpr std::int32_t DEAD_RETURN_TURN = 6;
        constexpr MilliFunds DEAD_RETURN_STREAM = 2;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const std::array keys{ContinuationKey{today}, ContinuationKey{damaged}, ContinuationKey{dead}};
        const auto field = Fixture::make(2);
        const std::vector<MilliFunds> zero(NODES, 0);
        const std::vector<MilliFunds> fixed(FIXED_PRICES.begin(), FIXED_PRICES.end());
        for (const auto & prices : {zero, fixed, tunedPrices(field, keys.front())})
        {
            ReplyStockContext context(field);
            const auto version = context.installPrices(prices).version;
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                const auto ceiling = context.ceiling(keys[index]);
                const auto floor = context.floor(keys[index]);
                const bool polished = floor.polish == ReplyStockFloorPolish::Improved ?
                    floor.counters.polishedRows > 0 :
                    floor.polish == ReplyStockFloorPolish::Unchanged && floor.counters.polishedRows == 0;
                expect(floor.status == ReplyStockStatus::Valid && ceiling.status == ReplyStockStatus::Valid &&
                           floor.floor <= EXACT[index] && EXACT[index] <= ceiling.ceiling &&
                           floor.priceVersion == version && floor.key == keys[index] && ascendingWitness(floor) &&
                           floor.counters.steps > 0 && floor.work > 0 && polished,
                       "a published floor replays and brackets the exact optimum below the ceiling");
            }
        }
        ReplyStockContext derived(field);
        derived.installPrices(zero);
        const auto deadFloor = derived.floor(keys[DEAD_INDEX]);
        const std::vector<SequentialRowWitness> deadWitness{SequentialRowWitness{1,
            {{0, DEAD_HEAD_TURN, DEAD_HEAD_WEIGHT}, {1, DEAD_RETURN_TURN, DEAD_RETURN_STREAM}},
            DEAD_HEAD_WEIGHT + DEAD_RETURN_STREAM}};
        expect(deadFloor.floor == EXACT[DEAD_INDEX] && deadFloor.witness == deadWitness &&
                   deadFloor.counters.prefixRows == 1 && deadFloor.counters.emptyRows == 1,
               "the destroyed key's floor is the second row's whole relaxed chain");

        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            const auto existing = existingTune(field, keys[index]);
            const bool usable = existing.status == MasterPlanPricingStatus::Valid && existing.floor.has_value();
            const bool closes = usable && *existing.floor == existing.ceiling;
            ReplyStockContext context(field);
            const auto tuned = context.tune(keys[index], MASTER_PLAN_PRICING_ROUNDS);
            const auto plain = context.floor(keys[index]);
            expect(usable && tuned.status == ReplyStockStatus::Valid && tuned.floor.status == ReplyStockStatus::Valid &&
                       tuned.floor.floor <= EXACT[index] && EXACT[index] <= tuned.ceiling &&
                       (!closes || (tuned.floor.floor == EXACT[index] && tuned.ceiling == EXACT[index])) &&
                       tuned.rounds > 0 && plain.status == ReplyStockStatus::Valid && tuned.floor.floor >= plain.floor &&
                       ascendingWitness(tuned.floor) && tuned.limits.limits.rounds == MASTER_PLAN_PRICING_ROUNDS &&
                       context.priceVersion() == 1 && context.ceiling(keys[index]).ceiling == tuned.ceiling,
                   "tune leaves verified prices installed and closes the gap wherever the existing tuner does");
        }
    }

    void stockContextFloorConflictsAndReference()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        // The second node costs more than any continuation through it earns, so both relaxed chains stop at node zero.
        constexpr std::array CONFLICT_PRICES{MilliFunds{0}, MilliFunds{7}};
        constexpr std::array REFERENCE_PRICES{MilliFunds{10}, MilliFunds{6}};
        constexpr std::size_t NODES = 2;
        constexpr std::int32_t ACTING_TURN = 1;
        constexpr std::int32_t IDLE_TURN = 2;
        constexpr std::int32_t FAR_TURN = 3;
        constexpr std::int32_t RETURN_TURN = 5;
        constexpr MilliFunds ACTING_WEIGHT = 14;
        constexpr MilliFunds SECOND_HEAD_WEIGHT = 10;
        constexpr MilliFunds SECOND_HOME_WEIGHT = 6;
        constexpr MilliFunds FAR_HEAD_WEIGHT = 4;
        constexpr MilliFunds FIRST_NODE_STREAM = 6;
        constexpr std::int32_t ALONE_RETURN_TURN = 6;
        constexpr MilliFunds ALONE_RETURN_STREAM = 2;
        constexpr MilliFunds DEAD_EXACT = 12;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto second = CanonicalPlanActionKey::fromAction(1, Fixture::WIDTH - 1, 0, false);
        const auto away = *CanonicalPlanActionKey::checkedFromAction(0, 1, 0, false, 1, true);
        const auto field = Fixture::make(2);
        const SequentialRowWitness actingRow{0, {{0, ACTING_TURN, ACTING_WEIGHT}}, ACTING_WEIGHT};
        const SequentialRowWitness homeRow{1, {{1, IDLE_TURN, SECOND_HOME_WEIGHT}}, SECOND_HOME_WEIGHT};

        ReplyStockContext fresh(field);
        const auto refused = fresh.floor({today});
        expect(refused.status == ReplyStockStatus::NoPrices && refused.floor == 0 && refused.witness.empty() &&
                   refused.priceVersion == 0,
               "a floor asked before any prices is refused");

        ReplyStockContext conflict(field);
        conflict.installPrices(CONFLICT_PRICES);
        const auto alone = conflict.floor({dead});
        const auto shared = conflict.floor({today});

        const std::vector<SequentialRowWitness> aloneWitness{SequentialRowWitness{1,
            {{0, FAR_TURN, SECOND_HEAD_WEIGHT}, {1, ALONE_RETURN_TURN, ALONE_RETURN_STREAM}},
            SECOND_HEAD_WEIGHT + ALONE_RETURN_STREAM}};
        const std::vector<SequentialRowWitness> sharedWitness{actingRow, homeRow};
        expect(alone.witness == aloneWitness && alone.counters.prefixRows == 1 &&
                   alone.polish == ReplyStockFloorPolish::Improved && alone.counters.polishedRows == 1,
               "alone, the relaxed chain stops at the first node and the polish extends it");
        expect(shared.status == ReplyStockStatus::Valid && shared.witness == sharedWitness &&
                   shared.counters.prefixRows == 1 && shared.counters.fallbackRows == 1 &&
                   shared.polish == ReplyStockFloorPolish::Unchanged && shared.counters.polishedRows == 0,
               "a later row whose chain starts at a used node falls back to its best free head");

        const auto freed = conflict.floor({dead}, &shared);
        expect(freed.status == ReplyStockStatus::Valid && freed.counters.reusedRows == 1 &&
                   freed.counters.prefixRows == 0 && freed.counters.fallbackRows == 0 &&
                   freed.polish == ReplyStockFloorPolish::Improved && freed.counters.polishedRows == 1 &&
                   freed.floor == DEAD_EXACT && freed.witness == aloneWitness && ascendingWitness(freed),
               "the node a dead unit frees is picked up by the polish, from the reused home row to the exact value");

        ReplyStockContext context(field);
        context.installPrices(REFERENCE_PRICES);
        const auto base = context.floor({second});
        const auto rebuilt = context.floor({today, second}, &base);
        const auto again = context.floor({today, second}, &base);
        const std::vector<SequentialRowWitness> rebuiltWitness{actingRow, homeRow};
        expect(base.status == ReplyStockStatus::Valid && rebuilt.status == ReplyStockStatus::Valid &&
                   rebuilt.witness == rebuiltWitness && rebuilt.counters.reusedRows == 1 &&
                   rebuilt.counters.adaptedRows == 1 && rebuilt.counters.prefixRows == 0 &&
                   rebuilt.counters.fallbackRows == 0 &&
                   rebuilt.polish == ReplyStockFloorPolish::Unchanged,
               "the reference serves the unchanged row while the changed row is rebuilt");
        expect(again.witness == rebuilt.witness && again.floor == rebuilt.floor &&
                   again.counters.reusedRows == rebuilt.counters.reusedRows,
               "asking twice gives the identical witness");
        auto corrupted = base;
        ++corrupted.witness.front().steps.front().turn;
        const auto rejected = context.floor({second}, &corrupted);
        expect(rejected.status == ReplyStockStatus::ReplayRejected && rejected.floor == 0 && rejected.witness.empty(),
               "a reused step the replay disagrees with withholds the whole floor");

        ReplyStockContext open(field);
        open.installPrices(std::vector<MilliFunds>(NODES, 0));
        const auto destroyed = open.floor({dead});
        auto worthless = destroyed;
        worthless.witness.insert(worthless.witness.begin(),
                                 SequentialRowWitness{0, {{0, NO_CAPTURE_TURNS, 0}}, 0});
        const auto unblocked = open.floor({dead}, &worthless);
        expect(destroyed.status == ReplyStockStatus::Valid && unblocked.status == ReplyStockStatus::Valid &&
                   unblocked.witness == destroyed.witness && unblocked.counters.emptyRows == 1,
               "a reused prefix worth nothing releases its node to the rows after it");

        auto farField = Fixture::make(2, Fixture::RATE);
        Fixture::setWeight(farField, 1, 0, FAR_HEAD_WEIGHT);
        ReplyStockContext capture(farField);
        capture.installPrices(std::vector<MilliFunds>(NODES, 0));
        const auto through = capture.floor({away});
        const auto cut = capture.floor({today}, &through);
        const std::vector<SequentialRowWitness> throughWitness{SequentialRowWitness{1,
            {{1, IDLE_TURN, SECOND_HOME_WEIGHT}, {0, RETURN_TURN, FIRST_NODE_STREAM}},
            SECOND_HOME_WEIGHT + FIRST_NODE_STREAM}};
        const std::vector<SequentialRowWitness> cutWitness{homeRow};
        expect(through.witness == throughWitness && cut.status == ReplyStockStatus::Valid &&
                   cut.witness == cutWitness && cut.counters.reusedRows == 1 && cut.counters.emptyRows == 1,
               "a reference chain is cut before the node the new key captures, keeping its turns and gains");
    }

    void stockContextAdaptsChangedRows()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::array CONFLICT_PRICES{MilliFunds{0}, MilliFunds{7}};
        constexpr std::size_t NODES = 2;
        constexpr MilliFunds TRAP_EXACT = 18;
        constexpr MilliFunds TRAP_RELAXED = 13;
        constexpr std::int32_t DAMAGED_TURN = 2;
        constexpr MilliFunds DAMAGED_WEIGHT = 12;
        constexpr std::int32_t HOME_TURN = 2;
        constexpr MilliFunds HOME_WEIGHT = 6;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto second = CanonicalPlanActionKey::fromAction(1, Fixture::WIDTH - 1, 0, false);
        const std::vector<MilliFunds> zero(NODES, 0);
        const SequentialRowWitness homeRow{1, {{1, HOME_TURN, HOME_WEIGHT}}, HOME_WEIGHT};

        const auto field = Fixture::make(2);
        ReplyStockContext context(field);
        context.installPrices(CONFLICT_PRICES);
        const auto reference = context.floor({today});
        context.installPrices(zero);
        const ContinuationKey both{damaged, second};
        const auto adapted = context.floor(both, &reference);
        const auto relaxed = context.floor(both);
        const auto exact = ReplyStockContext(field).tune(both, MASTER_PLAN_PRICING_ROUNDS);
        const std::vector<SequentialRowWitness> adaptedWitness{
            SequentialRowWitness{0, {{0, DAMAGED_TURN, DAMAGED_WEIGHT}}, DAMAGED_WEIGHT}, homeRow};
        expect(exact.status == ReplyStockStatus::Valid && exact.floor.floor == TRAP_EXACT &&
                   exact.ceiling == TRAP_EXACT && relaxed.floor == TRAP_RELAXED &&
                   relaxed.polish == ReplyStockFloorPolish::Unchanged && adapted.floor == TRAP_EXACT &&
                   adapted.witness == adaptedWitness && adapted.counters.adaptedRows == 2 &&
                   adapted.counters.reusedRows == 0 && ascendingWitness(adapted),
               "two changed rows follow their reference nodes out of the trap the relaxed chain and polish leave");

        // Only the nodes of a changed reference row are read, so its turns and gains are placeholders.
        const auto carried = Fixture::make(2, Fixture::RATE);
        ReplyStockContext capture(carried);
        capture.installPrices(zero);
        ReplyStockFloor through;
        through.status = ReplyStockStatus::Valid;
        through.key = {dead};
        through.witness = {SequentialRowWitness{1, {{0, 1, 1}, {1, 1, 1}}, 2}};
        const auto skipped = capture.floor(ContinuationKey{today, second}, &through);
        expect(skipped.status == ReplyStockStatus::Valid && skipped.witness == std::vector{homeRow} &&
                   skipped.counters.adaptedRows == 1 && skipped.counters.fallbackRows == 0,
               "a changed row skips the head the key captures and keeps the rest of its reference nodes");
    }

    void polishWitnessRejectsMalformedStarts()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t HEAD_TURN = 3;
        constexpr MilliFunds HEAD_WEIGHT = 10;
        constexpr MilliFunds DEAD_EXACT = 12;
        constexpr std::int32_t UNKNOWN_CANDIDATE = 99;
        constexpr std::int32_t UNKNOWN_NODE = 99;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const ContinuationKey key{dead};
        const auto field = Fixture::make(2);
        const auto reference = JointPlanStockValuer::diagnosticPricingModels(field, std::span(&key, 1));
        const auto tuned = existingTune(field, key);
        if (!reference.models.front() || !tuned.witness.valid || tuned.witness.candidateIndices.empty())
        {
            expect(false, "the destroyed key has a model and a tuned witness with an actor");
            return;
        }
        const MasterPlanPricingModel & model = *reference.models.front();
        const std::vector<std::int32_t> order{0, 1};
        const auto run = [&](const MasterPlanWitness & start, std::span<const std::int32_t> rows,
                             std::int64_t work = MASTER_PLAN_PRICING_WITNESS_WORK)
        {
            RefinementLedger ledger;
            ledger.open(MASTER_PLAN_PRICING_WITNESS_WORK);
            return polishMasterPlanWitness(model, start, rows, work, ledger);
        };
        auto head = tuned.witness;
        head.rows[1].steps = {{0, HEAD_TURN, HEAD_WEIGHT}};
        head.rows[1].value = HEAD_WEIGHT;
        const auto extended = run(head, order);
        expect(extended.status == MasterPlanPricingStatus::Valid && extended.witness.value == DEAD_EXACT &&
                   extended.witness.rows == tuned.witness.rows,
               "the polish extends a head-only start to the exact chain");

        const auto rejected = [&](const MasterPlanWitness & start, std::span<const std::int32_t> rows)
        {
            const auto result = run(start, rows);
            return result.status == MasterPlanPricingStatus::Invalid && !result.witness.valid;
        };
        const auto changed = [&](auto && edit)
        {
            auto start = tuned.witness;
            edit(start);
            return rejected(start, order);
        };
        const auto chain = tuned.witness.rows[1].steps;
        const std::vector<std::int32_t> repeated{1, 1};
        const std::vector<std::int32_t> outside{0, 2};
        expect(changed([&](MasterPlanWitness & start) { start.rows[0].steps = {chain.front()}; }) &&
                   changed([](MasterPlanWitness & start) { start.candidateIndices.front() = UNKNOWN_CANDIDATE; }) &&
                   changed([](MasterPlanWitness & start) { start.rows.pop_back(); }) &&
                   changed([](MasterPlanWitness & start) { start.rows[0].row = 1; }) &&
                   changed([](MasterPlanWitness & start) { start.rows[1].row = 0; }) &&
                   changed([](MasterPlanWitness & start) { start.rows[1].steps.front().node = UNKNOWN_NODE; }) &&
                   changed([&](MasterPlanWitness & start) { start.rows[1].steps = {chain.front(), chain.front()}; }) &&
                   rejected(tuned.witness, repeated) && rejected(tuned.witness, outside),
               "overlapping or out-of-range nodes, wrong row slots, an unknown candidate, short rows or a bad order "
               "are rejected");

        // A carried capture completes this turn, so the acting candidate captures its node.
        const auto carriedField = Fixture::make(2, Fixture::RATE);
        const ContinuationKey capturing{today};
        const auto captureModels =
            JointPlanStockValuer::diagnosticPricingModels(carriedField, std::span(&capturing, 1));
        const auto captureTune = existingTune(carriedField, capturing);
        const auto & captureModel = captureModels.models.front();
        const std::int32_t captured = captureModel && !captureModel->actors.empty() ?
            captureModel->actors.front().candidates.front().capturedNode : NO_SEQUENTIAL_NODE;
        if (!captureTune.witness.valid || captured == NO_SEQUENTIAL_NODE)
        {
            expect(false, "the capturing key has a capturing candidate and a tuned witness");
            return;
        }
        auto through = captureTune.witness;
        through.rows[1].steps = {{captured, HEAD_TURN, HEAD_WEIGHT}};
        RefinementLedger captureLedger;
        captureLedger.open(MASTER_PLAN_PRICING_WITNESS_WORK);
        const auto throughCaptured = polishMasterPlanWitness(*captureModel, through, order,
                                                             MASTER_PLAN_PRICING_WITNESS_WORK, captureLedger);
        expect(throughCaptured.status == MasterPlanPricingStatus::Invalid && !throughCaptured.witness.valid,
               "a chain through the node a candidate captures is rejected");
        expect(run(head, order, 1).status == MasterPlanPricingStatus::LimitReached,
               "a cap too small for one best response stops the polish");
    }

    void stockContextSeededTune()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::size_t NODES = 2;
        constexpr MilliFunds TODAY_EXACT = 20;
        constexpr MilliFunds DAMAGED_EXACT = 18;
        constexpr std::int32_t SHORT_ROUNDS = 5;
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto field = Fixture::make(2);
        const std::vector<MilliFunds> zero(NODES, 0);

        ReplyStockContext fresh(field);
        const auto refused = fresh.tune({today}, SHORT_ROUNDS, ReplyStockTuneStart::InstalledPrices);
        expect(refused.status == ReplyStockStatus::NoPrices && fresh.priceVersion() == 0 &&
                   refused.start == ReplyStockTuneStart::InstalledPrices,
               "a tune from installed prices before any install is refused and installs nothing");

        ReplyStockContext seeded(field);
        const auto base = seeded.tune({today}, MASTER_PLAN_PRICING_ROUNDS);
        const auto atSeed = seeded.ceiling({today});
        const auto oneSeeded = seeded.tune({today}, 1, ReplyStockTuneStart::InstalledPrices);
        const auto oneZero = ReplyStockContext(field).tune({today}, 1);
        expect(base.status == ReplyStockStatus::Valid && base.ceiling == TODAY_EXACT &&
                   oneSeeded.status == ReplyStockStatus::Valid && oneSeeded.rounds == 1 &&
                   oneSeeded.ceiling == TODAY_EXACT && oneSeeded.ceiling <= atSeed.ceiling &&
                   oneZero.status == ReplyStockStatus::Valid && oneZero.ceiling > TODAY_EXACT,
               "one round from the tuned prices returns the exact ceiling, one round from zero does not");

        ReplyStockContext other(field);
        other.tune({today}, MASTER_PLAN_PRICING_ROUNDS);
        const auto earlierCeiling = other.ceiling({damaged});
        const auto earlierFloor = other.floor({damaged});
        const auto retuned =
            other.tune({damaged}, SHORT_ROUNDS, ReplyStockTuneStart::InstalledPrices, &earlierFloor);
        expect(earlierFloor.status == ReplyStockStatus::Valid && retuned.status == ReplyStockStatus::Valid &&
                   retuned.start == ReplyStockTuneStart::InstalledPrices &&
                   retuned.ceiling <= earlierCeiling.ceiling && retuned.floor.floor >= earlierFloor.floor &&
                   retuned.floor.floor <= DAMAGED_EXACT && DAMAGED_EXACT <= retuned.ceiling,
               "a seeded re-tune with the key's own floor never loosens either side and brackets the exact value");

        std::int64_t unseededWork = -1;
        std::int64_t zeroSeededWork = -1;
        const auto unseeded = existingTune(field, {damaged}, {}, &unseededWork);
        const auto zeroSeeded = existingTune(field, {damaged}, zero, &zeroSeededWork);
        expect(unseeded.status == MasterPlanPricingStatus::Valid && sameTune(zeroSeeded, unseeded) &&
                   zeroSeededWork == unseededWork,
               "an explicit all-zero seed runs exactly the unseeded tune");
        const std::array wrongSize{MilliFunds{0}};
        const std::array negative{MilliFunds{-1}, MilliFunds{0}};
        const std::array tooHigh{MASTER_PLAN_PRICE_LIMIT + 1, MilliFunds{0}};
        std::int64_t wrongSizeWork = -1;
        std::int64_t negativeWork = -1;
        std::int64_t tooHighWork = -1;
        const auto shortSeed = existingTune(field, {damaged}, wrongSize, &wrongSizeWork);
        const auto negativeSeed = existingTune(field, {damaged}, negative, &negativeWork);
        const auto tooHighSeed = existingTune(field, {damaged}, tooHigh, &tooHighWork);
        expect(shortSeed.status == MasterPlanPricingStatus::Invalid && wrongSizeWork == 0 &&
                   negativeSeed.status == MasterPlanPricingStatus::Invalid && negativeWork == 0 &&
                   tooHighSeed.status == MasterPlanPricingStatus::Invalid && tooHighWork == 0,
               "a seed of the wrong size or with a price outside the limits is refused before any work");

        if (earlierFloor.status != ReplyStockStatus::Valid || earlierFloor.witness.empty() ||
            earlierFloor.witness.front().steps.empty())
        {
            expect(false, "the earlier floor has a witness step to corrupt");
            return;
        }
        auto corrupted = earlierFloor;
        ++corrupted.witness.front().steps.front().turn;
        const auto misled =
            other.tune({damaged}, SHORT_ROUNDS, ReplyStockTuneStart::InstalledPrices, &corrupted);
        expect(misled.status == ReplyStockStatus::ReplayRejected,
               "the verification floor is taken with the caller's reference");
    }

    void tunerStopsAtTargetGap()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t CAPPED_ROUNDS = 10;
        constexpr std::int32_t DEEP_ROWS = 3;
        constexpr MilliFunds WIDE_GAP = 3;
        constexpr MilliFunds NARROW_GAP = 1;
        constexpr MilliFunds TODAY_EXACT = 20;
        constexpr MilliFunds ABOVE_EVERY_CEILING = 1000;
        const auto today = Fixture::replyKeys().today;
        const auto shallow = Fixture::make(2);
        const auto deep = Fixture::make(DEEP_ROWS);
        const auto limitsWith = [](std::int32_t rounds, MilliFunds gap, std::optional<MilliFunds> known) {
            auto limits = pricingLimits(rounds);
            limits.targetGap = gap;
            limits.knownFloor = known;
            return limits;
        };

        std::int64_t plainWork = -1;
        std::int64_t zeroWork = -1;
        const auto plain = existingTune(shallow, {today}, {}, &plainWork);
        const auto zeroGap = existingTune(shallow, {today}, {}, &zeroWork,
                                          limitsWith(MASTER_PLAN_PRICING_ROUNDS, 0, std::nullopt));
        expect(plain.status == MasterPlanPricingStatus::Valid && plain.floor && *plain.floor < plain.ceiling &&
                   !plain.stoppedOnTarget && sameTune(zeroGap, plain) && zeroWork == plainWork,
               "a zero gap without a known floor is the exact-match tune on a key it does not close");
        const auto above = existingTune(shallow, {today}, {}, nullptr,
                                        limitsWith(MASTER_PLAN_PRICING_ROUNDS, 0, ABOVE_EVERY_CEILING));
        expect(!above.stoppedOnTarget && sameTune(above, plain),
               "a known floor above the best ceiling never stops the tune");

        const auto exact = existingTune(deep, {today}, {}, nullptr, limitsWith(CAPPED_ROUNDS, 0, std::nullopt));
        const auto wide = existingTune(deep, {today}, {}, nullptr, limitsWith(CAPPED_ROUNDS, WIDE_GAP, std::nullopt));
        expect(exact.status == MasterPlanPricingStatus::Valid && exact.rounds == CAPPED_ROUNDS &&
                   !exact.stoppedOnTarget && wide.status == MasterPlanPricingStatus::Valid &&
                   wide.rounds < CAPPED_ROUNDS && wide.stoppedOnTarget && wide.floor &&
                   wide.witness.value == *wide.floor && wide.ceiling - *wide.floor <= WIDE_GAP &&
                   referenceCeilings(deep, std::array{ContinuationKey{today}}, wide.prices).front() == wide.ceiling,
               "a positive gap stops before the round cap the exact rule runs into, on a repriced ceiling");

        const auto ownOnly = existingTune(deep, {today}, {}, nullptr,
                                          limitsWith(MASTER_PLAN_PRICING_ROUNDS, NARROW_GAP, std::nullopt));
        const auto known = existingTune(deep, {today}, {}, nullptr,
                                        limitsWith(MASTER_PLAN_PRICING_ROUNDS, NARROW_GAP, TODAY_EXACT));
        const auto ownAtStop = existingTune(deep, {today}, {}, nullptr,
                                            limitsWith(known.rounds, NARROW_GAP, std::nullopt));
        expect(known.status == MasterPlanPricingStatus::Valid && known.stoppedOnTarget &&
                   known.ceiling - TODAY_EXACT <= NARROW_GAP && ownOnly.stoppedOnTarget &&
                   known.rounds < ownOnly.rounds && !ownAtStop.stoppedOnTarget,
               "a known floor stops the tune where the tuner's own floor has not reached the target");
        expect(sameOutcome(known, ownAtStop),
               "a stop on the known floor returns the tuner's own floor and witness, never the known floor");

        for (const auto * field : {&shallow, &deep})
        {
            const auto alone = existingTune(*field, {today}, {}, nullptr, pricingLimits());
            const auto belowExact = existingTune(*field, {today}, {}, nullptr,
                                                 limitsWith(MASTER_PLAN_PRICING_ROUNDS, 0, TODAY_EXACT - 1));
            expect(alone.status == MasterPlanPricingStatus::Valid && sameTune(belowExact, alone),
                   "a known floor that never meets the target leaves the price path unchanged");
        }

        std::int64_t negativeWork = -1;
        const auto negative = existingTune(shallow, {today}, {}, &negativeWork,
                                           limitsWith(MASTER_PLAN_PRICING_ROUNDS, -1, std::nullopt));
        ReplyStockContext refusing(shallow);
        const auto refused = refusing.tune({today}, MASTER_PLAN_PRICING_ROUNDS, ReplyStockTuneStart::Zero, nullptr, -1);
        expect(negative.status == MasterPlanPricingStatus::Invalid && negativeWork == 0 &&
                   refused.status == ReplyStockStatus::Invalid && refusing.priceVersion() == 0,
               "a negative gap is refused before any work");
    }

    bool provenTune(Coordinator::ReplyStockContext & context, const Coordinator::ContinuationKey & key,
                    const Coordinator::ReplyStockTune & tuned, MilliFunds exact)
    {
        using namespace Coordinator;
        // Taking the returned floor as a reference replays its witness on the key's own instance.
        const auto replayed = context.floor(key, &tuned.floor);
        return tuned.status == ReplyStockStatus::Valid && tuned.floor.status == ReplyStockStatus::Valid &&
            replayed.status == ReplyStockStatus::Valid && replayed.floor >= tuned.floor.floor &&
            tuned.floor.floor <= exact && exact <= tuned.ceiling && ascendingWitness(tuned.floor) &&
            context.ceiling(key).ceiling == tuned.ceiling &&
            (!tuned.stoppedOnTarget || tuned.ceiling - tuned.floor.floor <= tuned.targetGap);
    }

    void stockContextTargetTune()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t CAPPED_ROUNDS = 10;
        constexpr std::int32_t SEED_ROUNDS = 2;
        constexpr std::int32_t DEEP_ROWS = 3;
        constexpr MilliFunds WIDE_GAP = 3;
        constexpr MilliFunds NARROW_GAP = 1;
        constexpr MilliFunds FOREIGN_GAP = 5;
        constexpr std::array EXACT{MilliFunds{20}, MilliFunds{18}, MilliFunds{12}};
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const std::array keys{ContinuationKey{today}, ContinuationKey{damaged}, ContinuationKey{dead}};
        const auto field = Fixture::make(2);

        ReplyStockContext unseeded(Fixture::make(DEEP_ROWS));
        const auto fromZero = unseeded.tune({today}, CAPPED_ROUNDS, ReplyStockTuneStart::Zero, nullptr, WIDE_GAP);
        expect(fromZero.stoppedOnTarget && fromZero.rounds < CAPPED_ROUNDS && fromZero.targetGap == WIDE_GAP &&
                   provenTune(unseeded, {today}, fromZero, EXACT.front()),
               "an unseeded tune with a target stops early on proven bounds");

        struct SeededRun
        {
            ReplyStockTune tuned;
            ReplyStockFloor before;
            MilliFunds seedCeiling{0};
            bool proven{false};
        };
        // Two rounds on the damaged key leave prices at which the today key's own floor is already exact.
        const auto seededTune = [&](MilliFunds gap, bool ownReference) {
            ReplyStockContext context(field);
            context.tune({damaged}, SEED_ROUNDS);
            SeededRun run;
            run.before = context.floor({today});
            run.seedCeiling = context.ceiling({today}).ceiling;
            run.tuned = context.tune({today}, MASTER_PLAN_PRICING_ROUNDS, ReplyStockTuneStart::InstalledPrices,
                                     ownReference ? &run.before : nullptr, gap);
            run.proven = provenTune(context, {today}, run.tuned, EXACT.front());
            return run;
        };
        const auto targeted = seededTune(NARROW_GAP, false);
        const auto plain = seededTune(0, false);
        expect(targeted.tuned.stoppedOnTarget && targeted.proven && targeted.tuned.rounds < plain.tuned.rounds &&
                   plain.proven && targeted.before.status == ReplyStockStatus::Valid &&
                   targeted.tuned.floor.floor >= targeted.before.floor,
               "a seeded tune stops on its own floor at the seed and verifies against it");
        const auto owned = seededTune(NARROW_GAP, true);
        expect(owned.proven && targeted.tuned.ceiling <= targeted.seedCeiling &&
                   owned.tuned.ceiling <= owned.seedCeiling && owned.tuned.floor.floor >= owned.before.floor,
               "a seeded target tune never raises the seed ceiling nor drops below the key's own reference");

        // One round on the today key: its seed floor stays 18, while its floor without that reference reaches 20.
        ReplyStockContext oneRound(field);
        oneRound.tune({today}, 1);
        const auto seedFloor = oneRound.floor({today});
        const auto unstopped = oneRound.tune({today}, MASTER_PLAN_PRICING_ROUNDS,
                                             ReplyStockTuneStart::InstalledPrices, nullptr, NARROW_GAP);
        const auto callerAlone = oneRound.floor({today});
        expect(!unstopped.stoppedOnTarget && provenTune(oneRound, {today}, unstopped, EXACT.front()) &&
                   callerAlone.status == ReplyStockStatus::Valid && callerAlone.floor > seedFloor.floor &&
                   unstopped.floor.floor >= callerAlone.floor && unstopped.floor.floor >= seedFloor.floor,
               "a target tune keeps the caller's floor when it beats the one referenced to the seed");

        // After one round on the today key its floor sits above anything the damaged key's floor reaches there.
        for (const std::int32_t baseRounds : {1, MASTER_PLAN_PRICING_ROUNDS})
        {
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                ReplyStockContext context(field);
                context.tune(keys.front(), baseRounds);
                const auto foreign = context.floor(keys.front());
                const auto tuned = context.tune(keys[index], MASTER_PLAN_PRICING_ROUNDS,
                                                ReplyStockTuneStart::InstalledPrices, &foreign, FOREIGN_GAP);
                expect(provenTune(context, keys[index], tuned, EXACT[index]),
                       "a reference from another key never ends the tune on a gap its own floor does not close");
            }
        }
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

    void stockRowsCarryRawApproach()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        constexpr std::int32_t MOVEMENT = 1;
        constexpr TilePoint FIRST{0, 0};
        constexpr TilePoint MIDDLE{1, 0};
        constexpr TilePoint LAST{Fixture::WIDTH - 1, 0};
        MobilityCostGrid grid(Fixture::WIDTH, 1);
        for (std::int32_t x = 0; x < Fixture::WIDTH; ++x)
        {
            grid.setCost(x, 0, EntryDirection::East, 1);
            grid.setCost(x, 0, EntryDirection::West, 1);
        }
        const std::vector<TilePoint> columns{FIRST, LAST};
        const auto raw = [&](TilePoint from)
        {
            ActivationField reach;
            reach.build(grid, from, MOVEMENT, Fixture::HORIZON, columns);
            std::vector<SequentialApproach> result;
            for (const auto & column : columns)
            {
                result.push_back({reach.activations(column.x, column.y), reach.spent(column.x, column.y)});
            }
            return result;
        };
        const auto [today, damaged, dead] = Fixture::replyKeys();
        const auto stay = CanonicalPlanActionKey::fromAction(0, MIDDLE.x, MIDDLE.y, false);
        // A damaged unit that steps onto the property without capturing owns it after the healthy one.
        const auto slowed = *CanonicalPlanActionKey::checkedFromAction(0, FIRST.x, FIRST.y, false, 1, false);
        const std::array keys{ContinuationKey{stay}, ContinuationKey{slowed}};
        const auto field = Fixture::make(2);
        const auto models = JointPlanStockValuer::diagnosticPricingModels(field, keys);
        const auto carriedField = Fixture::make(2, Fixture::RATE);
        const ContinuationKey capturing{today};
        const auto captureModels =
            JointPlanStockValuer::diagnosticPricingModels(carriedField, std::span(&capturing, 1));
        if (!models.models[0] || !models.models[1] || !captureModels.models[0])
        {
            expect(false, "the approach keys have pricing models");
            return;
        }
        const auto & stayRow = models.models[0]->actors.front().candidates.front().row;
        const auto & slowedRow = models.models[1]->actors.front().candidates.front().row;
        const auto & dayRow = models.models[0]->catalog.dayRows[1];
        const std::vector<SequentialApproach> stayExpected{{1, 1}, {1, 1}};
        const std::vector<SequentialApproach> slowedExpected{{1, 0}, {2, 1}};
        const std::vector<SequentialApproach> dayExpected{{2, 1}, {1, 0}};
        expect(stayRow.approach == stayExpected && stayRow.approach == raw(MIDDLE) &&
                   slowedRow.approach == slowedExpected && slowedRow.approach == raw(FIRST) &&
                   dayRow.approach == dayExpected && dayRow.approach == raw(LAST),
               "mover and day rows carry the field's own movement label for every node");
        expect(slowedRow.approach[0] < stayRow.approach[0] && slowedRow.ownedTurn[0] > stayRow.ownedTurn[0],
               "the approach is raw movement even where a slower capture rate reverses the ownership order");
        const auto & captured = captureModels.models[0]->actors.front().candidates.front();
        expect(captured.capturedNode == 0 && captured.row.approach.size() == captured.row.ownedTurn.size() &&
                   !captured.row.approach[0].reachable() && captured.row.ownedTurn[0] == NO_CAPTURE_TURNS &&
                   captured.row.approach[1] == raw(FIRST)[1],
               "removing a captured node clears its approach and keeps the rest aligned");
        // Owning slot 0 leaves slot 1 as the only node, so node and column indices differ.
        auto ownedField = Fixture::make(2);
        Fixture::ownColumn(ownedField, 0);
        const auto onto = CanonicalPlanActionKey::fromAction(0, FIRST.x, FIRST.y, false);
        const ContinuationKey ontoKey{onto};
        const auto ownedModels = JointPlanStockValuer::diagnosticPricingModels(ownedField, std::span(&ontoKey, 1));
        if (!ownedModels.models[0])
        {
            expect(false, "the owned-column key has a pricing model");
            return;
        }
        const auto & ownedMover = ownedModels.models[0]->actors.front().candidates.front().row;
        const auto & ownedDay = ownedModels.models[0]->catalog.dayRows[1];
        const std::vector<SequentialApproach> moverToLast{{2, 1}};
        const std::vector<SequentialApproach> dayToLast{{1, 0}};
        expect(ownedMover.approach == moverToLast && ownedMover.approach.front() == raw(FIRST)[1] &&
                   ownedMover.approach.size() == ownedMover.ownedTurn.size() && ownedDay.approach == dayToLast &&
                   ownedDay.approach.front() == raw(LAST)[1] && ownedDay.approach.size() == ownedDay.ownedTurn.size(),
               "with a filtered column the approach follows each node's own column for mover and day rows");
    }

    constexpr std::int32_t TIE_NODES = 2;
    constexpr std::int32_t TIE_HORIZON = 8;
    constexpr std::int32_t TIE_TURN = 3;
    constexpr MilliFunds TIE_WEIGHT = 50;
    constexpr MilliFunds TIE_SIDE_WEIGHT = 5;
    constexpr Coordinator::SequentialApproach FAR_APPROACH{3, 1};
    constexpr Coordinator::SequentialApproach NEAR_APPROACH{1, 1};
    constexpr Coordinator::SequentialApproach NO_APPROACH{};
    constexpr Coordinator::SequentialApproach MORE_SPENT{2, 3};
    constexpr Coordinator::SequentialApproach LESS_SPENT{2, 1};

    struct TieCandidate
    {
        MilliFunds immediate{0};
        std::vector<MilliFunds> weight;
        std::vector<std::int32_t> ownedTurn;
        std::vector<Coordinator::SequentialApproach> approach;
        std::vector<Coordinator::MasterResourceUsage> resources;
        bool follows{false};
    };

    TieCandidate headZero(Coordinator::SequentialApproach approach, MilliFunds immediate = 0)
    {
        return TieCandidate{immediate, {TIE_WEIGHT, 0}, {TIE_TURN, NO_CAPTURE_TURNS}, {approach, NO_APPROACH}, {}};
    }

    TieCandidate idleCandidate(MilliFunds immediate)
    {
        return TieCandidate{immediate, {0, 0}, {NO_CAPTURE_TURNS, NO_CAPTURE_TURNS}, {}, {}};
    }

    std::optional<Coordinator::MasterPlanPricingModel> tieModel(const std::vector<std::vector<TieCandidate>> & actors)
    {
        using namespace Coordinator;
        MasterPlanPricingModel model;
        auto & catalog = model.catalog;
        catalog.horizon = TIE_HORIZON;
        catalog.complete = true;
        for (std::int32_t node = 0; node < TIE_NODES; ++node)
        {
            catalog.columns.push_back(PropertyStockColumn{.slot = node, .tile = {node, 0}, .income = {1, 1}});
        }
        // Without continuations every itinerary is a single head.
        SequentialClassTable table;
        const std::vector<MilliFunds> prizes(static_cast<std::size_t>(TIE_NODES * TIE_HORIZON), 0);
        const std::vector<std::int32_t> interArrivals(static_cast<std::size_t>(TIE_NODES * TIE_NODES), 0);
        table.buildFromPrizes(TIE_NODES, prizes, interArrivals, NO_CAPTURE_TURNS, TIE_HORIZON);
        catalog.classes.push_back(std::move(table));
        const auto idle = idleCandidate(0);
        for (std::size_t row = 0; row < actors.size(); ++row)
        {
            const auto rowIndex = static_cast<std::int32_t>(row);
            catalog.dayRows.push_back(
                SequentialRow{.classIndex = 0, .weight = idle.weight, .ownedTurn = idle.ownedTurn});
            model.rowActors.push_back(rowIndex);
            MasterPlanPricingActor actor{rowIndex, {}};
            for (std::size_t index = 0; index < actors[row].size(); ++index)
            {
                const auto & source = actors[row][index];
                MasterPlanPricingCandidate candidate;
                candidate.candidateIndex = static_cast<std::int32_t>(index);
                candidate.key =
                    CanonicalPlanActionKey::fromAction(rowIndex, static_cast<std::int32_t>(index), 0, false);
                candidate.resources = source.resources;
                candidate.immediateValue = source.immediate;
                candidate.follows = source.follows;
                candidate.row = SequentialRow{.classIndex = 0, .weight = source.weight,
                                              .ownedTurn = source.ownedTurn, .approach = source.approach};
                actor.candidates.push_back(std::move(candidate));
            }
            model.actors.push_back(std::move(actor));
        }
        if (!indexMasterPlanActions(model))
        {
            return std::nullopt;
        }
        return model;
    }

    bool sameWitness(const Coordinator::MasterPlanWitness & left, const Coordinator::MasterPlanWitness & right)
    {
        return left.valid && right.valid && left.candidateIndices == right.candidateIndices &&
            left.rows == right.rows && left.value == right.value;
    }

    Coordinator::MasterPlanPricingResult tuneInMode(const Coordinator::MasterPlanPricingModel & model,
                                                    Coordinator::MasterPlanSearchMode mode)
    {
        using namespace Coordinator;
        RefinementLedger ledger;
        ledger.open(2 * MASTER_PLAN_PRICING_WORK);
        return tuneMasterPlanPrices(model, pricingLimits(), ledger, {.mode = mode});
    }

    void expectTieChoice(const std::vector<TieCandidate> & candidates, std::int32_t expected, std::string_view message)
    {
        using namespace Coordinator;
        const auto model = tieModel({candidates});
        if (!model)
        {
            expect(false, message);
            return;
        }
        const auto pruned = tuneInMode(*model, MasterPlanSearchMode::Pruned);
        const auto exhaustive = tuneInMode(*model, MasterPlanSearchMode::Exhaustive);
        expect(sameWitness(pruned.witness, exhaustive.witness) && pruned.witness.candidateIndices.size() == 1 &&
                   pruned.witness.candidateIndices.front() == expected,
               message);
    }

    void tieKeepsOrMovesCloser()
    {
        const auto hold = headZero(FAR_APPROACH);
        expectTieChoice({hold, headZero(NEAR_APPROACH)}, 1, "an equal choice closer to the same head is preferred");
        expectTieChoice({hold, headZero(FAR_APPROACH)}, 0, "an equal approach keeps the first choice");
        const TieCandidate otherHead{0, {TIE_SIDE_WEIGHT, TIE_WEIGHT}, {TIE_TURN, TIE_TURN},
                                     {NEAR_APPROACH, FAR_APPROACH}, {}};
        expectTieChoice({hold, otherHead}, 0, "an equal choice with a different head keeps the first choice");
        expectTieChoice({hold, headZero(NEAR_APPROACH, -1)}, 0, "a closer choice one milli-fund lower never wins");
        expectTieChoice({idleCandidate(TIE_WEIGHT), headZero(NEAR_APPROACH)}, 0,
                        "a first choice without a head keeps its place on an equal score");
        auto bareFar = headZero(FAR_APPROACH);
        auto bareNear = headZero(NEAR_APPROACH);
        bareFar.approach.clear();
        bareNear.approach.clear();
        expectTieChoice({bareFar, bareNear}, 0, "rows without movement detail keep the first choice");
        expectTieChoice({headZero(MORE_SPENT), headZero(LESS_SPENT)}, 1,
                        "with equal activations the choice spending fewer points in the arrival activation wins");
        // The reduced threshold reaches below zero, yet an objective of zero is the empty itinerary, which has no head.
        const TieCandidate zeroObjective{TIE_WEIGHT, {0, 0}, {TIE_TURN, NO_CAPTURE_TURNS}, {NEAR_APPROACH, NO_APPROACH},
                                         {}};
        expectTieChoice({hold, zeroObjective}, 0, "a tie at a zero threshold is the headless empty itinerary");
        auto unowned = headZero(NEAR_APPROACH);
        unowned.ownedTurn.front() = NO_CAPTURE_TURNS;
        expectTieChoice({hold, unowned}, 0, "a closer choice that never owns the head does not take the tie");
        expectTieChoice({hold, headZero(NO_APPROACH)}, 0, "an unreachable candidate label does not take the tie");
        expectTieChoice({headZero(NO_APPROACH), headZero(NEAR_APPROACH)}, 0,
                        "a reachable label does not replace a held unreachable one");
        constexpr Coordinator::SequentialApproach PARTLY_UNREACHABLE{FAR_APPROACH.activations,
                                                                     Coordinator::UNREACHABLE};
        expectTieChoice({headZero(PARTLY_UNREACHABLE), headZero(NEAR_APPROACH)}, 0,
                        "a partly unreachable held label is never compared, though it orders after the candidate");
    }

    void expectBothModesFail(const Coordinator::MasterPlanPricingModel & model,
                             const Coordinator::MasterPlanWitness & start, std::string_view message);

    void exhaustiveArithmeticMatchesHead()
    {
        using namespace Coordinator;
        constexpr MilliFunds LIMIT = std::numeric_limits<MilliFunds>::min();
        const auto model = tieModel({{idleCandidate(0), idleCandidate(LIMIT)}});
        if (!model)
        {
            expect(false, "the overflow model indexes");
            return;
        }
        const auto pruned = tuneInMode(*model, MasterPlanSearchMode::Pruned);
        const auto exhaustive = tuneInMode(*model, MasterPlanSearchMode::Exhaustive);
        expect(!pruned.witness.valid && !exhaustive.witness.valid && pruned.status == exhaustive.status,
               "an unseeded pick whose threshold overflows fails in both modes");
        const MasterPlanWitness start{true, {0}, {{0, {}, 0}}, 0};
        expectBothModesFail(*model, start, "a polish whose scan threshold overflows fails in both modes");
    }

    Coordinator::MasterPlanPolishResult polishTie(const Coordinator::MasterPlanPricingModel & model,
                                                  const Coordinator::MasterPlanWitness & start,
                                                  Coordinator::MasterPlanSearchMode mode, bool audit = false)
    {
        using namespace Coordinator;
        std::vector<std::int32_t> order(model.rowActors.size());
        std::iota(order.begin(), order.end(), 0);
        RefinementLedger ledger;
        ledger.open(MASTER_PLAN_PRICING_WITNESS_WORK);
        return polishMasterPlanWitness(model, start, order, MASTER_PLAN_PRICING_WITNESS_WORK, ledger,
                                       {.mode = mode, .auditSkips = audit});
    }

    void expectBothModesFail(const Coordinator::MasterPlanPricingModel & model,
                             const Coordinator::MasterPlanWitness & start, std::string_view message)
    {
        using namespace Coordinator;
        const auto pruned = polishTie(model, start, MasterPlanSearchMode::Pruned);
        const auto exhaustive = polishTie(model, start, MasterPlanSearchMode::Exhaustive);
        expect(pruned.status == MasterPlanPricingStatus::Invalid &&
                   exhaustive.status == MasterPlanPricingStatus::Invalid,
               message);
    }

    Coordinator::SequentialRowWitness heldHead(std::int32_t row)
    {
        return {row, {{0, TIE_TURN, TIE_WEIGHT}}, TIE_WEIGHT};
    }

    void polishSwitchesToCloserTie()
    {
        using namespace Coordinator;
        constexpr MilliFunds LIMIT = std::numeric_limits<MilliFunds>::min();
        const auto limitModel = tieModel({{headZero(FAR_APPROACH, LIMIT), headZero(NEAR_APPROACH, TIE_WEIGHT)}});
        constexpr std::int32_t RESOURCE = 0;
        constexpr MilliFunds HELD_IMMEDIATE = 10;
        constexpr MilliFunds FREED_IMMEDIATE = 20;
        constexpr std::int64_t SWITCHED_RESPONSES = 4;
        constexpr std::int64_t SETTLED_SKIPS = 2;
        const MasterResourceUsage resource{RESOURCE, 1, 1};
        auto near = headZero(NEAR_APPROACH);
        near.resources = {resource};
        auto held = idleCandidate(HELD_IMMEDIATE);
        held.resources = {resource};
        const auto freedModel =
            tieModel({{headZero(FAR_APPROACH), near}, {held, idleCandidate(FREED_IMMEDIATE)}});
        if (!limitModel || !freedModel)
        {
            expect(false, "the polish tie models index");
            return;
        }
        const MasterPlanWitness limitStart{true, {0}, {heldHead(0)}, LIMIT + TIE_WEIGHT};
        const MasterPlanWitness freedStart{true, {0, 0}, {heldHead(0), {1, {}, 0}}, TIE_WEIGHT + HELD_IMMEDIATE};
        const auto exhaustiveLimit = polishTie(*limitModel, limitStart, MasterPlanSearchMode::Exhaustive);
        const auto exhaustiveFreed = polishTie(*freedModel, freedStart, MasterPlanSearchMode::Exhaustive);
        const auto auditedFreed = polishTie(*freedModel, freedStart, MasterPlanSearchMode::Pruned, true);
        expect(sameWitness(auditedFreed.witness, exhaustiveFreed.witness) &&
                   auditedFreed.witnessStats.skipViolations == 0 && auditedFreed.witnessStats.tieSwitches == 1,
               "the audited pruned polish matches exhaustive and no skipped seat would have switched");
        for (const auto mode : {MasterPlanSearchMode::Pruned, MasterPlanSearchMode::Exhaustive})
        {
            const auto limit = polishTie(*limitModel, limitStart, mode);
            expect(limit.status == MasterPlanPricingStatus::Valid && limit.witness.candidateIndices.front() == 1 &&
                       limit.witness.value == 2 * TIE_WEIGHT && limit.witnessStats.tieSwitches == 0 &&
                       sameWitness(limit.witness, exhaustiveLimit.witness),
                   "a negative threshold at the arithmetic limit is not reduced and a strictly better choice wins");
            const auto freed = polishTie(*freedModel, freedStart, mode);
            const std::vector<std::int32_t> switched{1, 1};
            expect(freed.status == MasterPlanPricingStatus::Valid && freed.witness.candidateIndices == switched &&
                       freed.witnessStats.tieSwitches == 1 && sameWitness(freed.witness, exhaustiveFreed.witness),
                   "a seat moves to the closer tied choice once the resource it needs is freed");
            if (mode == MasterPlanSearchMode::Pruned)
            {
                expect(freed.witnessStats.responses == SWITCHED_RESPONSES &&
                           freed.witnessStats.skippedResponses == SETTLED_SKIPS,
                       "a tie switch moves the version stamp, so the other seat answers again before both settle");
            }
        }
    }

    void polishRefusesASuppliedFollower()
    {
        using namespace Coordinator;
        constexpr MilliFunds FOLLOWER_IMMEDIATE = 100;
        constexpr std::int32_t FOLLOWER = 1;
        auto follower = idleCandidate(FOLLOWER_IMMEDIATE);
        follower.follows = true;
        auto cleared = follower;
        cleared.follows = false;
        const auto flagged = tieModel({{idleCandidate(0), follower}});
        const auto unflagged = tieModel({{idleCandidate(0), cleared}});
        if (!flagged || !unflagged)
        {
            expect(false, "the supplied follower models index");
            return;
        }
        const MasterPlanWitness start{true, {FOLLOWER}, {{0, {}, 0}}, FOLLOWER_IMMEDIATE};
        for (const auto mode : {MasterPlanSearchMode::Pruned, MasterPlanSearchMode::Exhaustive})
        {
            const auto refused = polishTie(*flagged, start, mode);
            expect(refused.status == MasterPlanPricingStatus::Invalid && !refused.witness.valid,
                   "a supplied seating that holds a follower is refused before it is occupied");
            const auto kept = polishTie(*unflagged, start, mode);
            expect(kept.status == MasterPlanPricingStatus::Valid && kept.witness.candidateIndices.front() == FOLLOWER,
                   "the same seating without the flag is accepted and keeps its choice");
        }
    }

    void productionRowsPreferTheCloserApproach()
    {
        using namespace Coordinator;
        using Fixture = PropertyStockDiagnosticFixture;
        // Both arrive on the first activation; stepping onto the property spends no points there.
        const auto stay = CanonicalPlanActionKey::fromAction(0, 1, 0, false);
        const auto onto = CanonicalPlanActionKey::fromAction(0, 0, 0, false);
        const std::array keys{ContinuationKey{stay}, ContinuationKey{onto}};
        const auto field = Fixture::make(1);
        const auto models = JointPlanStockValuer::diagnosticPricingModels(field, keys);
        if (!models.models[0] || !models.models[1])
        {
            expect(false, "the stay and approach keys have pricing models");
            return;
        }
        const auto & stayCandidate = models.models[0]->actors.front().candidates.front();
        const auto & ontoCandidate = models.models[1]->actors.front().candidates.front();
        const auto build = [&](const SequentialRow & ontoRow, bool withOnto)
        {
            MasterPlanPricingModel model;
            model.catalog = models.models[0]->catalog;
            model.catalog.actions.clear();
            model.rowActors = {0};
            MasterPlanPricingActor actor{0, {stayCandidate}};
            actor.candidates.front().candidateIndex = 0;
            if (withOnto)
            {
                actor.candidates.push_back(ontoCandidate);
                actor.candidates.back().candidateIndex = 1;
                actor.candidates.back().row = ontoRow;
            }
            model.actors.push_back(std::move(actor));
            return indexMasterPlanActions(model) ? std::optional<MasterPlanPricingModel>(std::move(model))
                                                 : std::nullopt;
        };
        auto sameLabels = ontoCandidate.row;
        sameLabels.approach = stayCandidate.row.approach;
        const auto alone = build(ontoCandidate.row, false);
        const auto closer = build(ontoCandidate.row, true);
        const auto equal = build(sameLabels, true);
        if (!alone || !closer || !equal)
        {
            expect(false, "the production-row models index");
            return;
        }
        expect(stayCandidate.row.approach[0] == SequentialApproach{1, 1} &&
                   ontoCandidate.row.approach[0] == SequentialApproach{1, 0},
               "the stay and the approach arrive on the same activation, the approach with fewer points spent");
        for (const auto mode : {MasterPlanSearchMode::Pruned, MasterPlanSearchMode::Exhaustive})
        {
            const auto stayOnly = tuneInMode(*alone, mode);
            const auto moved = tuneInMode(*closer, mode);
            const auto kept = tuneInMode(*equal, mode);
            expect(stayOnly.witness.valid && moved.witness.valid && kept.witness.valid &&
                       moved.witness.value == stayOnly.witness.value && kept.witness.value == stayOnly.witness.value &&
                       moved.witness.candidateIndices.front() == 1 && kept.witness.candidateIndices.front() == 0,
                   "production rows of equal value take the approach, and the stay when the labels are equal");
        }
    }
}

int main()
{
    testSolverMatchesBruteForce();
    polishRefusesASuppliedFollower();
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
    stockContextMatchesDiagnosticCeilings();
    stockContextExtensionKeepsAnswers();
    stockContextFloorBracketsExact();
    stockContextFloorConflictsAndReference();
    stockContextSeededTune();
    tunerStopsAtTargetGap();
    stockContextTargetTune();
    polishWitnessRejectsMalformedStarts();
    stockContextAdaptsChangedRows();
    stockRowsCarryRawApproach();
    tieKeepsOrMovesCloser();
    polishSwitchesToCloserTie();
    exhaustiveArithmeticMatchesHead();
    productionRowsPreferTheCloserApproach();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
