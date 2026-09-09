#pragma once

#include <map>
#include <set>

#include "ai/coordinator/masterassignmentsearch.h"
#include "ai/coordinator/propertystockfield.h"
#include "tests/coordinator_ai/masterassignmenttestfixture.h"

namespace MasterAssignmentTest
{
    using MasterAssignmentOracle::ContinuationCase;
    using MasterAssignmentOracle::SequentialInput;

    struct NativeSequentialModel
    {
        SequentialInput input;
        std::vector<SequentialClassTable> classes;

        explicit NativeSequentialModel(const SequentialInput & source)
            : input(source)
        {
            for (const auto & definition : input.classes)
            {
                SequentialClassTable table;
                table.build(input.columns, definition.interArrivals, definition.captureTurns, input.horizon);
                classes.push_back(std::move(table));
            }
        }

        SequentialInstance instance() const
        {
            return {input.horizon, input.columns, classes, input.rows, input.capturedNodes};
        }

        CheapRowTerms cheapRow(std::int32_t rowIndex) const
        {
            CheapRowTerms result{rowIndex, {}};
            const auto model = instance();
            const auto & row = model.rows[static_cast<std::size_t>(rowIndex)];
            for (std::int32_t node = 0; node < model.nodeCount(); ++node)
            {
                if (row.ownedTurn[static_cast<std::size_t>(node)] == NO_CAPTURE_TURNS &&
                    row.weight[static_cast<std::size_t>(node)] == 0)
                {
                    continue;
                }
                const auto realized = SequentialDetail::realizeWitness(model, row, node);
                CheapChain chain;
                for (std::size_t step = 0; step < realized.size(); ++step)
                {
                    const auto & visit = realized[step];
                    const auto gain = step == 0 ? row.weight[static_cast<std::size_t>(node)]
                        : SequentialDetail::chainPrize(model, visit.node, visit.turn);
                    chain.steps.push_back({visit.node, visit.turn, gain});
                    chain.value += gain;
                }
                if (!chain.steps.empty())
                {
                    result.chains.push_back(std::move(chain));
                }
            }
            return result;
        }
    };

    enum class QuoteFault : std::int8_t
    {
        None,
        ForeignIdentity,
        MissingWitness,
        MissingEnemyWitness,
        UnregisteredCertificate,
    };

    class NativeStockSession final : public RelativeStockSession
    {
    public:
        NativeStockSession(const PropertyStockField & field, const PropertyStockInstance & enemyIdentity,
                           const std::vector<ContinuationCase> & cases, RefinementLedger & ledger,
                           QuoteFault fault = QuoteFault::None, bool refine = true, bool deferFriendly = false)
            : RelativeStockSession(field, enemyIdentity),
              m_ledger(ledger),
              m_fault(fault),
              m_refine(refine),
              m_deferFriendly(deferFriendly)
        {
            require(!cases.empty(), "native stock cases");
            std::vector<CheapActionTerms> actions;
            std::vector<CheapRowTerms> dayRows;
            MilliFunds enemyCeiling = 0;
            for (const auto & source : cases)
            {
                NativeSequentialModel friendly(source.friendly);
                NativeSequentialModel enemy(source.enemy);
                if (!m_enemies.contains(source.capturedColumns))
                {
                    MilliFunds upper = 0;
                    for (std::size_t row = 0; row < source.enemy.rows.size(); ++row)
                    {
                        auto terms = enemy.cheapRow(static_cast<std::int32_t>(row));
                        MilliFunds maximum = 0;
                        for (const auto & chain : terms.chains)
                        {
                            maximum = std::max(maximum, chain.value);
                        }
                        upper += maximum;
                    }
                    enemyCeiling = std::max(enemyCeiling, upper);
                    m_enemies.emplace(source.capturedColumns, Enemy{std::move(enemy), {0, upper}});
                }
                for (const auto & key : source.key)
                {
                    auto terms = friendly.cheapRow(key.row());
                    const auto found = std::find_if(actions.begin(), actions.end(),
                        [&](const auto & action) { return action.key == key; });
                    if (found == actions.end())
                    {
                        const auto captured = std::find_if(source.friendly.columns.begin(),
                            source.friendly.columns.end(), [&](const auto & column)
                            {
                                return key.captures() && column.tile == TilePoint{key.x(), key.y()} &&
                                    std::find(source.capturedColumns.begin(), source.capturedColumns.end(),
                                              column.slot) != source.capturedColumns.end();
                            });
                        actions.push_back({key, key.row(), captured == source.friendly.columns.end()
                            ? -1 : captured->slot, std::move(terms)});
                        m_future.actions.push_back({key, source.friendly.rows[static_cast<std::size_t>(key.row())]});
                    }
                    else
                    {
                        expect(found->rowTerms == terms, "same endpoint has one row-domain definition");
                    }
                }
                m_cases.emplace(source.key, Entry{source, std::move(friendly)});
            }
            const auto & first = cases.front();
            NativeSequentialModel day(first.friendly);
            m_future.horizon = first.friendly.horizon;
            m_future.columns = first.friendly.columns;
            m_future.classes = day.classes;
            m_future.dayRows = first.friendly.rows;
            std::sort(m_future.actions.begin(), m_future.actions.end(),
                [](const MasterFutureAction & left, const MasterFutureAction & right) { return left.key < right.key; });
            m_future.complete = true;
            for (std::size_t row = 0; row < first.friendly.rows.size(); ++row)
            {
                dayRows.push_back(day.cheapRow(static_cast<std::int32_t>(row)));
            }
            std::vector<MilliFunds> swings;
            for (const auto & column : first.friendly.columns)
            {
                swings.push_back(ownershipFlipSwing(column, first.friendly.horizon));
            }
            const auto rowCount = static_cast<std::int32_t>(dayRows.size());
            require(m_model.install(rowCount,
                static_cast<std::int32_t>(first.friendly.columns.size()), first.owned,
                std::move(swings), std::move(dayRows), std::move(actions), enemyCeiling), "native cheap model");
        }

        const CheapPricingModel* pricingModel() const override
        {
            return &m_model;
        }

        const MasterFutureCatalog* futureCatalog() const override
        {
            return &m_future;
        }

        RelativeStockQuote quote(const TurnPlan &, MilliFunds) override
        {
            return {};
        }

        RelativeStockQuote quote(const ContinuationKey & key, MilliFunds economic) override
        {
            m_quotedKeys.insert(key);
            const auto found = m_cases.find(key);
            if (found == m_cases.end())
            {
                return {};
            }
            auto & entry = found->second;
            auto state = m_model.initialState();
            CheapInitialPrice cheap;
            if (!m_model.price(key, state, cheap))
            {
                return {};
            }
            if (!entry.initialized)
            {
                entry.ours = cheap.ours;
                entry.initialized = true;
            }
            const auto & enemy = m_enemies.at(entry.source.capturedColumns);
            RelativeStockQuote result{
                .key = key, .capturedColumns = entry.source.capturedColumns, .identity = identity(),
                .economic = economic, .owned = entry.source.owned,
                .ours = entry.ours, .enemy = enemy.bounds,
                .ourLowerWitnessReplays = true, .enemyLowerWitnessReplays = true,
                .ourSearchCompleted = entry.ours.exact(), .enemySearchCompleted = enemy.bounds.exact(), .valid = true};
            const auto certified = m_certificates.find(key);
            if (certified != m_certificates.end())
            {
                result.ourUpperCertificateId = certified->second.id;
            }
            if (m_fault == QuoteFault::ForeignIdentity)
            {
                result.identity = {};
            }
            if (m_fault == QuoteFault::MissingWitness)
            {
                result.ourLowerWitnessReplays = false;
            }
            if (m_fault == QuoteFault::MissingEnemyWitness)
            {
                result.enemyLowerWitnessReplays = false;
            }
            if (m_fault == QuoteFault::UnregisteredCertificate)
            {
                result.ourUpperCertificateId = 991;
            }
            return result;
        }

        bool replayOurWitness(const RelativeStockQuote & quote) override
        {
            if (!owns(quote) || !quote.ourLowerWitnessReplays)
            {
                return false;
            }
            const auto & entry = m_cases.at(quote.key);
            if (entry.refined)
            {
                return replaySequentialWitness(entry.model.instance(), entry.witness, quote.ours.lower);
            }
            auto state = m_model.initialState();
            CheapInitialPrice cheap;
            return m_model.price(quote.key, state, cheap) &&
                m_model.replayWitness(state, cheap.witness, quote.ours.lower);
        }

        bool replayEnemyWitness(const RelativeStockQuote & quote) override
        {
            if (!owns(quote) || !quote.enemyLowerWitnessReplays)
            {
                return false;
            }
            const auto & enemy = m_enemies.at(quote.capturedColumns);
            return replaySequentialWitness(enemy.model.instance(), enemy.witness, quote.enemy.lower);
        }

        std::shared_ptr<const RelativeStockProofSnapshot> captureProof(const RelativeStockQuote & expected) override
        {
            ++proofCaptures;
            if (!owns(expected) || !expected.ourLowerWitnessReplays || !expected.enemyLowerWitnessReplays ||
                !sameRelativeStockQuote(expected, quote(expected.key, expected.economic)))
            {
                return {};
            }
            auto proof = std::make_shared<RelativeStockProofSnapshot>();
            proof->quote = expected;
            const auto & entry = m_cases.at(expected.key);
            if (entry.refined)
            {
                proof->ourKind = RelativeStockWitnessKind::Sequential;
                proof->ourWitness = entry.witness;
            }
            else
            {
                auto state = m_model.initialState();
                CheapInitialPrice cheap;
                if (!m_model.price(expected.key, state, cheap))
                {
                    return {};
                }
                proof->cheapOurWitness = std::move(cheap.witness);
            }
            proof->enemyWitness = m_enemies.at(expected.capturedColumns).witness;
            if (expected.ourUpperCertificateId != 0)
            {
                const auto found = m_certificates.find(expected.key);
                if (found == m_certificates.end() || found->second.id != expected.ourUpperCertificateId)
                {
                    return {};
                }
                proof->ourUpperCertificate = found->second.proof;
            }
            return proof;
        }

        bool replayLowerProof(const RelativeStockProofSnapshot & proof) const override
        {
            const auto & expected = proof.quote;
            const auto found = m_cases.find(expected.key);
            if (!owns(expected) || found == m_cases.end() ||
                found->second.source.capturedColumns != expected.capturedColumns ||
                found->second.source.owned != expected.owned)
            {
                return false;
            }
            const auto & entry = found->second;
            if (proof.ourKind == RelativeStockWitnessKind::Cheap)
            {
                auto state = m_model.initialState();
                CheapInitialPrice cheap;
                if (!proof.ourWitness.empty() || !m_model.price(expected.key, state, cheap) ||
                    !m_model.replayWitness(state, proof.cheapOurWitness, expected.ours.lower))
                {
                    return false;
                }
            }
            else if (proof.ourKind == RelativeStockWitnessKind::Sequential)
            {
                if (!proof.cheapOurWitness.empty() ||
                    !replaySequentialWitness(entry.model.instance(), proof.ourWitness, expected.ours.lower))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
            const auto & enemy = m_enemies.at(expected.capturedColumns);
            return replaySequentialWitness(enemy.model.instance(), proof.enemyWitness, expected.enemy.lower);
        }

        RelativeStockRefinedTerm refineOur(const ContinuationKey & key, std::int64_t cap) override
        {
            if (!firstFriendlyRefinement)
            {
                firstFriendlyRefinement = key;
                distinctQuotesBeforeFirstRefinement = m_quotedKeys.size();
            }
            auto & entry = m_cases.at(key);
            if (m_deferFriendly)
            {
                const auto grant = m_ledger.draw(1, RefinementWork::SearchSlice);
                return {.interval = entry.ours, .workUsed = 1, .lowerWitnessReplays = true,
                        .budgetExhausted = grant == NO_GRANT, .valid = grant != NO_GRANT};
            }
            const auto result = refineModel(entry.model, entry.ours, entry.witness, cap);
            entry.refined = !entry.witness.empty();
            return result;
        }

        RelativeStockRefinedTerm refineEnemy(const std::vector<std::int32_t> & captured,
                                            std::int64_t cap) override
        {
            auto & enemy = m_enemies.at(captured);
            return refineModel(enemy.model, enemy.bounds, enemy.witness, cap);
        }

        std::int64_t remainingWork() const override
        {
            return m_ledger.balance();
        }

        std::int64_t ourRerunWork() const override
        {
            return NATIVE_SOLVE_TARIFF;
        }

        std::int64_t enemyRerunWork(const std::vector<std::int32_t> &) const override
        {
            return NATIVE_SOLVE_TARIFF;
        }

        bool supportsVerifiedDual() const override
        {
            return true;
        }

        RelativeStockCertificatePreparation prepareOurCertificate(const RelativeStockQuote & quote) override
        {
            ++readinessCalls;
            if (!owns(quote) || preparation.readinessWork < 0)
            {
                return {};
            }
            if (!m_ledger.consume(preparation.readinessWork, RefinementWork::MasterValuation))
            {
                return {.status = RelativeStockPreparationStatus::BudgetExhausted};
            }
            return preparation;
        }

        RelativeStockDualTightening tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote, std::int64_t maximumWork) override
        {
            RelativeStockDualTightening result;
            result.intervalBefore = quote.ours;
            result.intervalAfter = quote.ours;
            ++certificateCalls;
            maximumCertificateAllowance = std::max(maximumCertificateAllowance, maximumWork);
            if (!owns(quote) || maximumWork <= 0)
            {
                result.globalFailure = true;
                return result;
            }
            const auto grant = m_ledger.draw(maximumWork, RefinementWork::PackingDualCertificate);
            if (grant == NO_GRANT)
            {
                result.locallyBudgetExhausted = true;
                return result;
            }
            std::int64_t spent = 0;
            auto & entry = m_cases.at(quote.key);
            const auto model = entry.model.instance();
            auto finalize = [&]
            {
                require(m_ledger.refund(grant, maximumWork - spent), "dual unused work refunded");
                result.totalWork = spent;
                return result;
            };
            const auto tariff = static_cast<std::int64_t>((model.rows.size() + 1) *
                (model.nodeColumns.size() + 1));
            if (tariff >= maximumWork)
            {
                result.locallyBudgetExhausted = true;
                return finalize();
            }
            spent = tariff;
            result.tariffWork = tariff;
            SequentialDetail::EnumerationBudget budget{.stateCap = maximumWork - spent};
            std::int64_t itineraries = 0;
            std::vector<std::vector<SequentialDetail::RowOption>> rows;
            PackingDualProblemIdentity identity;
            identity.session = sessionIdentity();
            identity.key = quote.key;
            identity.capturedColumns = quote.capturedColumns;
            identity.immutableField.ourColumns = entry.source.friendly.columns;
            identity.nodeColumns = entry.source.friendly.columns;
            identity.usedNodes = PackingDualNodeMask(model.nodeCount());
            identity.economic = quote.economic;
            identity.owned = quote.owned;
            identity.incumbentFloor = quote.ours.lower;
            for (const auto node : quote.capturedColumns)
            {
                require(identity.usedNodes.set(node), "captured node identity");
            }
            for (std::size_t row = 0; row < model.rows.size(); ++row)
            {
                std::vector<MilliFunds> enriched;
                const auto & source = model.rows[row];
                std::vector<std::int64_t> domain{static_cast<std::int64_t>(row), source.classIndex};
                domain.insert(domain.end(), source.weight.begin(), source.weight.end());
                domain.insert(domain.end(), source.ownedTurn.begin(), source.ownedTurn.end());
                identity.rowDomains.push_back(std::move(domain));
                for (std::int32_t node = 0; node < model.nodeCount(); ++node)
                {
                    enriched.push_back(source.weight[static_cast<std::size_t>(node)] +
                        SequentialDetail::continuationGain(model, source, node));
                }
                rows.push_back(SequentialDetail::enumerateRowOptions(
                    model, static_cast<std::int32_t>(row), enriched, budget, itineraries));
            }
            spent += budget.states;
            if (budget.truncated)
            {
                result.locallyBudgetExhausted = true;
                return finalize();
            }
            std::vector<PackingDualRowSource> sources;
            for (std::size_t row = 0; row < rows.size(); ++row)
            {
                sources.push_back({static_cast<std::int32_t>(row), identity.rowDomains[row], rows[row]});
            }
            const auto catalog = buildPackingDualCatalog(sources, identity, maximumWork - spent);
            spent += catalog.constructionWork;
            result.catalogWork = budget.states + catalog.constructionWork;
            result.catalogComplete = catalog.complete;
            const auto verificationWork = packingDualVerifierWork(catalog);
            if (!catalog.complete || verificationWork < 0 || verificationWork > maximumWork - spent)
            {
                result.locallyBudgetExhausted = true;
                return finalize();
            }
            const auto optimized = optimizePackingDualPrices(catalog, identity, maximumWork - spent - verificationWork);
            spent += optimized.workUsed;
            result.optimizerWork = optimized.workUsed;
            result.optimizerCompleted = optimized.completed;
            const auto verified = verifyPackingDualCertificate(catalog, sources, identity,
                optimized.nodePrices, maximumWork - spent);
            spent += verified.workUsed;
            result.verifierWork = verified.workUsed;
            result.verifierCompleted = verified.completed;
            result.certificateValid = verified.valid;
            if (verified.valid)
            {
                ++verifiedCertificates;
                result.certificate = std::make_shared<const PackingDualCertificate>(verified.certificate);
                result.certificateId = ++m_nextCertificate;
                result.certifiedUpper = verified.certificate.dualObjective;
                require(result.certifiedUpper >= entry.ours.lower, "verified dual contains replayed lower");
                entry.ours.upper = std::min(entry.ours.upper, result.certifiedUpper);
                result.intervalAfter = entry.ours;
                m_certificates[quote.key] = {result.certificateId, result.certificate};
            }
            return finalize();
        }

        std::uint64_t certificateCalls{0};
        std::uint64_t readinessCalls{0};
        RelativeStockCertificatePreparation preparation{.status = RelativeStockPreparationStatus::Ready};
        std::int64_t maximumCertificateAllowance{0};
        std::uint64_t proofCaptures{0};
        std::uint64_t verifiedCertificates{0};
        std::optional<ContinuationKey> firstFriendlyRefinement;
        std::size_t distinctQuotesBeforeFirstRefinement{0};

    private:
        std::set<ContinuationKey> m_quotedKeys;
        bool verifiedUpperMatches(const RelativeStockQuote & quote) const override
        {
            const auto found = m_certificates.find(quote.key);
            if (found == m_certificates.end())
            {
                return quote.ourUpperCertificateId == 0;
            }
            return quote.ourUpperCertificateId == found->second.id &&
                found->second.proof->identity.session == sessionIdentity() &&
                found->second.proof->identity.key == quote.key &&
                found->second.proof->identity.capturedColumns == quote.capturedColumns &&
                found->second.proof->identity.economic == quote.economic &&
                found->second.proof->identity.owned == quote.owned &&
                quote.ours.upper <= found->second.proof->dualObjective;
        }

        RelativeStockRefinedTerm refineModel(const NativeSequentialModel & model, StockInterval & bounds,
                                            std::vector<SequentialRowWitness> & witness, std::int64_t cap)
        {
            if (!m_refine)
            {
                return {};
            }
            const auto grant = m_ledger.draw(cap + NATIVE_SOLVE_TARIFF, RefinementWork::SearchSlice);
            if (grant == NO_GRANT)
            {
                return {.budgetExhausted = true};
            }
            const auto tier = solveSequentialPacking(model.instance(), 0, cap, nullptr, true,
                SequentialWitnessPolicy::BookedValue);
            require(m_ledger.refund(grant, cap - tier.searchStates), "unused native work refunded");
            if (tier.bookedValue >= bounds.lower &&
                replaySequentialWitness(model.instance(), tier.witness, tier.bookedValue))
            {
                bounds.lower = tier.bookedValue;
                witness = tier.witness;
            }
            if (tier.continuationSeen || model.input.rows.empty())
            {
                bounds.upper = std::min(bounds.upper, tier.searchCompleted || tier.certificate
                    ? tier.bookedValue : tier.relaxedValue);
            }
            return {.interval = bounds, .statesUsed = tier.searchStates,
                    .workUsed = NATIVE_SOLVE_TARIFF + tier.searchStates,
                    .searchCompleted = bounds.exact(), .lowerWitnessReplays = true, .valid = true};
        }

        struct Entry
        {
            ContinuationCase source;
            NativeSequentialModel model;
            StockInterval ours;
            std::vector<SequentialRowWitness> witness;
            bool initialized{false};
            bool refined{false};
        };

        struct Enemy
        {
            NativeSequentialModel model;
            StockInterval bounds;
            std::vector<SequentialRowWitness> witness;
        };

        struct Certificate
        {
            std::uint64_t id{0};
            std::shared_ptr<const PackingDualCertificate> proof;
        };

        static constexpr std::int64_t NATIVE_SOLVE_TARIFF = 128;
        RefinementLedger & m_ledger;
        QuoteFault m_fault;
        bool m_refine;
        bool m_deferFriendly;
        CheapPricingModel m_model;
        MasterFutureCatalog m_future;
        std::map<ContinuationKey, Entry> m_cases;
        std::map<std::vector<std::int32_t>, Enemy> m_enemies;
        std::map<ContinuationKey, Certificate> m_certificates;
        std::uint64_t m_nextCertificate{0};
    };

    inline ContinuationCase continuationCase(const ContinuationKey & key)
    {
        ContinuationCase result;
        result.key = key;
        result.owned = -37;
        result.friendly.horizon = 7;
        result.friendly.columns = {
            {.slot = 0, .tile = {0, 2}, .income = {11, 11}},
            {.slot = 1, .tile = {1, 3}, .income = {17, 17}},
            {.slot = 2, .tile = {2, 4}, .income = {23, 23}},
        };
        result.friendly.classes.push_back({2, {0, 1, 2, 1, 0, 1, 2, 1, 0}});
        result.friendly.capturedNodes.assign(3, false);
        for (const auto & action : key)
        {
            SequentialRow row{.classIndex = 0};
            for (const auto & column : result.friendly.columns)
            {
                const auto turn = 2 + std::abs(column.tile.x - action.x()) + (action.y() == 0 ? 1 : 0);
                row.ownedTurn.push_back(sequentialFirstOwnedTurn(turn, result.friendly.horizon));
                row.weight.push_back(columnStreamValue(column, turn, result.friendly.horizon));
            }
            result.friendly.rows.push_back(std::move(row));
        }
        result.enemy = result.friendly;
        result.enemy.rows.resize(1);
        result.enemy.rows[0].ownedTurn = {4, 3, 2};
        result.enemy.rows[0].weight = {33, 68, 115};
        return result;
    }
}
