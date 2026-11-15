#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/masterplanpricing.h"
#include "ai/coordinator/relativestockcomparison.h"
#include "ai/coordinator/replystockcontext.h"

namespace
{
    using namespace Coordinator;

    constexpr RefinementWork REPLY_STOCK_WORK = RefinementWork::MasterDualOptimization;
    constexpr RefinementWork REPLY_FLOOR_WORK = RefinementWork::MasterWitness;
    // Stock keys carry no immediate value, exactly as in the diagnostic dual model.
    constexpr MilliFunds KEY_IMMEDIATE_VALUE = 0;
    // The tuner's price step charges one unit per demand entry and one per price.
    constexpr std::size_t PRICE_STEP_TERMS_PER_NODE = 2;
    // One pick search and then up to the polish passes, each sized as rows times nodes times turns.
    constexpr std::size_t WITNESS_SEARCH_PASSES = 1 + MASTER_PLAN_POLISH_PASSES;
    // The tuner refuses a zero witness budget, which a model without rows or classes would otherwise get.
    constexpr std::int64_t MIN_TUNE_BUILD_WORK = 1;
    // Each consideration makes an enforced build and, until a floor exists, a relaxed one.
    constexpr std::size_t WITNESS_BUILDS_PER_CONSIDERATION = 2;

    ReplyStockStatus replyStatusOf(MasterFutureStatus status)
    {
        switch (status)
        {
            case MasterFutureStatus::Valid:
                return ReplyStockStatus::Valid;
            case MasterFutureStatus::ArithmeticFailure:
                return ReplyStockStatus::ArithmeticFailure;
            case MasterFutureStatus::BudgetExhausted:
                return ReplyStockStatus::LimitReached;
            case MasterFutureStatus::Unavailable:
            case MasterFutureStatus::Invalid:
                break;
        }
        return ReplyStockStatus::Invalid;
    }

    ReplyStockStatus replyStatusOf(MasterPlanPricingStatus status)
    {
        switch (status)
        {
            case MasterPlanPricingStatus::Valid:
                return ReplyStockStatus::Valid;
            case MasterPlanPricingStatus::ArithmeticFailure:
                return ReplyStockStatus::ArithmeticFailure;
            case MasterPlanPricingStatus::LimitReached:
                return ReplyStockStatus::LimitReached;
            case MasterPlanPricingStatus::Unavailable:
            case MasterPlanPricingStatus::Invalid:
            case MasterPlanPricingStatus::Inconsistent:
                break;
        }
        return ReplyStockStatus::Invalid;
    }

    bool dualFailure(MasterPlanPricingStatus status)
    {
        return status == MasterPlanPricingStatus::ArithmeticFailure || status == MasterPlanPricingStatus::Invalid ||
            status == MasterPlanPricingStatus::Inconsistent;
    }

    std::int32_t capturedNodeOf(std::span<const PropertyStockColumn> columns, std::int32_t capturedColumn)
    {
        return capturedColumn == NO_STOCK_COLUMN ? NO_SEQUENTIAL_NODE : futureNodeOfColumn(columns, capturedColumn);
    }

    // The ceiling refunds each captured node price once, so a second capturer of it would be unsound.
    bool claimCapturedNode(std::vector<bool> & capturedNodes, std::int32_t node)
    {
        const auto slot = static_cast<std::size_t>(node);
        if (capturedNodes[slot])
        {
            return false;
        }
        capturedNodes[slot] = true;
        return true;
    }

    // A relaxed chain stops at its first blocked node; a reference sequence skips it and keeps its tail.
    enum class BlockedNode : std::int8_t
    {
        Stop,
        Skip,
    };

    struct ForcedAction
    {
        const SequentialRow* pRow{nullptr};
        std::int32_t capturedNode{NO_SEQUENTIAL_NODE};
    };

    // Removing a captured node is idempotent, so a lookup may hand over a row that already lost it.
    template <typename Lookup>
    std::optional<MasterPlanPricingModel> forcedPricingModel(
        std::int32_t horizon, std::span<const PropertyStockColumn> columns,
        std::span<const SequentialClassTable> classes, std::span<const SequentialRow> dayRows,
        const ContinuationKey & key, Lookup && lookup, const std::vector<bool>* pExpectedCaptured)
    {
        MasterPlanPricingModel model;
        model.catalog.horizon = horizon;
        model.catalog.columns.assign(columns.begin(), columns.end());
        model.catalog.classes.assign(classes.begin(), classes.end());
        model.catalog.dayRows.assign(dayRows.begin(), dayRows.end());
        model.catalog.complete = true;
        model.rowActors.assign(dayRows.size(), MASTER_PLAN_NO_ACTOR);
        std::vector<bool> capturedNodes(columns.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const ForcedAction forced = lookup(action);
            if (forced.pRow == nullptr)
            {
                return std::nullopt;
            }
            MasterPlanPricingCandidate candidate;
            candidate.key = action;
            candidate.row = *forced.pRow;
            candidate.capturedNode = forced.capturedNode;
            if (candidate.capturedNode != NO_SEQUENTIAL_NODE)
            {
                if (!claimCapturedNode(capturedNodes, candidate.capturedNode))
                {
                    return std::nullopt;
                }
                removeCapturedNode(candidate.row, candidate.capturedNode);
            }
            model.rowActors[static_cast<std::size_t>(action.row())] = static_cast<std::int32_t>(model.actors.size());
            model.actors.push_back(MasterPlanPricingActor{action.row(), {std::move(candidate)}});
        }
        if ((pExpectedCaptured != nullptr && capturedNodes != *pExpectedCaptured) || !indexMasterPlanActions(model))
        {
            return std::nullopt;
        }
        return model;
    }

    std::optional<MasterPlanPricingModel> dualModel(const MasterFutureCatalog & catalog,
                                                    const CheapPricingModel & cheap, const ContinuationKey & key,
                                                    const std::vector<bool> & expectedCaptured)
    {
        return forcedPricingModel(catalog.horizon, catalog.columns, catalog.classes, catalog.dayRows, key,
            [&](const CanonicalPlanActionKey & action)
        {
            const MasterFutureAction* pFuture = findMasterFutureAction(catalog, action);
            const CheapActionTerms* pTerms = cheap.actionTerms(action);
            return pFuture == nullptr || pTerms == nullptr ? ForcedAction{} :
                ForcedAction{&pFuture->row, capturedNodeOf(catalog.columns, pTerms->capturedColumn)};
        }, &expectedCaptured);
    }

    bool compactWitness(std::span<const SequentialRowWitness> source, std::vector<SequentialRowWitness> & rows,
                        MilliFunds & value)
    {
        bool summed = true;
        for (const SequentialRowWitness & row : source)
        {
            if (row.steps.empty())
            {
                continue;
            }
            summed = summed && RelativeStockDetail::checkedAdd(value, row.value);
            rows.push_back(row);
        }
        return summed;
    }

    std::size_t witnessSteps(std::span<const SequentialRowWitness> rows)
    {
        std::size_t steps = 0;
        for (const SequentialRowWitness & row : rows)
        {
            steps += row.steps.size();
        }
        return steps;
    }

    std::vector<const CanonicalPlanActionKey*> rowVariants(const ContinuationKey & key, std::size_t rowCount)
    {
        std::vector<const CanonicalPlanActionKey*> variants(rowCount, nullptr);
        for (const CanonicalPlanActionKey & action : key)
        {
            variants[static_cast<std::size_t>(action.row())] = &action;
        }
        return variants;
    }

    bool sameVariant(const CanonicalPlanActionKey* pLeft, const CanonicalPlanActionKey* pRight)
    {
        return pLeft == nullptr ? pRight == nullptr : pRight != nullptr && *pLeft == *pRight;
    }

    std::optional<std::int64_t> rowSearchWork(std::size_t rows, std::size_t nodes, std::int32_t horizon)
    {
        const auto cells = masterWorkProduct(nodes, static_cast<std::size_t>(std::max(horizon, 1)));
        return cells ? masterWorkProduct(rows, static_cast<std::size_t>(*cells)) : std::nullopt;
    }

    std::optional<std::int64_t> addWork(std::int64_t left, std::optional<std::int64_t> right)
    {
        if (!right || !RelativeStockDetail::checkedAdd(left, *right))
        {
            return std::nullopt;
        }
        return left;
    }

    bool tightenWithDual(const MasterFutureCatalog & catalog, const CheapPricingModel & cheap,
                         const ContinuationKey & key, const SequentialInstance & instance,
                         const DiagnosticRefinementLimits & limits, DiagnosticStockQuote & quote)
    {
        quote.dualStatus = MasterPlanPricingStatus::Invalid;
        const auto model = dualModel(catalog, cheap, key, instance.capturedNodes);
        std::int64_t ledgerTotal = limits.dualWork;
        RefinementLedger ledger;
        // The final evaluation draws on what tuning leaves, so it gets a reserve as large as the tuning budget.
        if (!model || !RelativeStockDetail::checkedAdd(ledgerTotal, limits.dualWork) || !ledger.open(ledgerTotal))
        {
            return false;
        }
        const MasterPlanPricingResult tuned = tuneMasterPlanPrices(
            *model, MasterPlanPricingLimits{limits.dualRounds, limits.dualWork, limits.dualWitnessWork}, ledger);
        quote.dualRounds = tuned.rounds;
        quote.dualStatus = tuned.status;
        if (dualFailure(tuned.status))
        {
            return false;
        }
        StockInterval interval = quote.ours;
        if (!tuned.prices.empty())
        {
            const MasterPlanCeiling checked = priceMasterPlan(*model, tuned.prices, ledger);
            quote.dualStatus = checked.status;
            if (dualFailure(checked.status))
            {
                return false;
            }
            if (checked.status == MasterPlanPricingStatus::Valid)
            {
                quote.dualCeiling = checked.ceiling;
                interval.upper = std::min(interval.upper, checked.ceiling);
            }
        }
        if (tuned.witness.valid)
        {
            std::vector<SequentialRowWitness> rows;
            MilliFunds value = 0;
            const bool summed = compactWitness(tuned.witness.rows, rows, value);
            quote.dualWitnessValue = value;
            quote.dualWitnessReplays = summed && replaySequentialWitness(instance, rows, value);
            if (quote.dualWitnessReplays && value > interval.lower)
            {
                interval.lower = value;
            }
        }
        if (interval.upper < interval.lower)
        {
            return false;
        }
        quote.ours = interval;
        return true;
    }
}

namespace Coordinator
{
    std::int32_t PropertyStockField::tileSlot(std::int32_t width, std::int32_t height, std::int32_t x, std::int32_t y)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
        {
            return NO_STOCK_COLUMN;
        }
        return y * width + x;
    }

    std::int32_t PropertyStockField::columnSlotAt(const TilePoint & tile) const
    {
        const std::int32_t slot = tileSlot(m_width, m_height, tile.x, tile.y);
        if (slot == NO_STOCK_COLUMN)
        {
            return NO_STOCK_COLUMN;
        }
        return m_columnAtTile[static_cast<std::size_t>(slot)];
    }

    std::int32_t PropertyStockField::carriedFor(const PropertyStockRow & row, std::int32_t slot) const
    {
        const std::size_t index = static_cast<std::size_t>(slot);
        if (m_columnCapturer[index] != row.knowledgeIndex || m_columnTiles[index] != row.tile)
        {
            return 0;
        }
        return m_columnCapturePoints[index];
    }

    const PropertyStockField::ArrivalVectors & PropertyStockField::arrivalsFrom(std::int32_t gridIdentity,
                                                                                std::int32_t movementPoints,
                                                                                const TilePoint & tile)
    {
        const ArrivalKey key{gridIdentity, movementPoints, tile.x, tile.y};
        const auto known = m_arrivals.find(key);
        if (known != m_arrivals.end())
        {
            return known->second;
        }
        const MobilityCostGrid & grid = m_grids[static_cast<std::size_t>(gridIdentity)];
        m_reach.build(grid, tile, movementPoints, m_horizonTurns, m_columnTiles);
        ArrivalVectors arrivals;
        arrivals.activations.reserve(m_columnTiles.size());
        for (const TilePoint & column : m_columnTiles)
        {
            arrivals.activations.push_back(m_reach.activations(column.x, column.y));
        }
        return m_arrivals.emplace(key, std::move(arrivals)).first->second;
    }

    std::vector<MilliFunds> PropertyStockField::actingWeights(std::int32_t row, const PropertyStockActor & actor)
    {
        const PropertyStockRow & source = m_ours.rows[static_cast<std::size_t>(row)];
        if (actor.destroyed || (actor.stepsLost > 0 && actor.effectiveCaptureRate(source.captureRate) == 0))
        {
            return std::vector<MilliFunds>(m_ours.columns.size(), 0);
        }
        const ArrivalVectors & arrivals = arrivalsFrom(source.gridIdentity, actor.movementPoints, actor.tile);
        std::vector<MilliFunds> weights(m_ours.columns.size(), 0);
        for (std::size_t slot = 0; slot < m_ours.columns.size(); ++slot)
        {
            const ArrivalFacts arrival = planRowArrival(actor, source.captureRate, m_columnTiles[slot],
                                                         arrivals.activations[slot]);
            weights[slot] = columnWeight(m_ours.columns[slot], arrival, m_capturePointsToCapture, m_horizonTurns);
        }
        return weights;
    }

    PropertyStockActor PropertyStockField::actionMover(
        const PlanRowAction & action, std::int32_t & capturedColumn) const
    {
        const PropertyStockRow & row = m_ours.rows[static_cast<std::size_t>(action.row)];
        const std::int32_t slot = columnSlotAt(action.destination);
        const bool onColumn = slot != NO_STOCK_COLUMN;
        const auto mover = planRowMover(row, action, onColumn ? carriedFor(row, slot) : 0, onColumn);
        capturedColumn = onColumn && action.captures && mover.carriedCapturePoints >= m_capturePointsToCapture
            ? slot : NO_STOCK_COLUMN;
        return mover;
    }

    PropertyStockField::JointActionFacts PropertyStockField::jointActionFacts(
        std::span<const PlanRowAction> actions) const
    {
        JointActionFacts facts;
        for (const PlanRowAction & entry : actions)
        {
            std::int32_t capturedColumn = NO_STOCK_COLUMN;
            facts.movers.push_back(actionMover(entry, capturedColumn));
            facts.moverRows.push_back(entry.row);
            if (capturedColumn != NO_STOCK_COLUMN)
            {
                facts.capturedColumns.push_back(capturedColumn);
            }
        }
        std::sort(facts.capturedColumns.begin(), facts.capturedColumns.end());
        facts.capturedColumns.erase(
            std::unique(facts.capturedColumns.begin(), facts.capturedColumns.end()),
            facts.capturedColumns.end());
        return facts;
    }

    MilliFunds PropertyStockField::ourOpenOptimum(const JointActionFacts & facts)
    {
        std::vector<std::int32_t> openColumns;
        for (std::int32_t column = 0; column < m_ours.columnCount(); ++column)
        {
            if (std::find(facts.capturedColumns.begin(),
                          facts.capturedColumns.end(),
                          column) == facts.capturedColumns.end())
            {
                openColumns.push_back(column);
            }
        }
        std::vector<MilliFunds> weights;
        weights.reserve(static_cast<std::size_t>(m_ours.rowCount()) * openColumns.size());
        for (std::int32_t row = 0; row < m_ours.rowCount(); ++row)
        {
            const auto mover =
                std::find(facts.moverRows.begin(), facts.moverRows.end(), row);
            if (mover == facts.moverRows.end())
            {
                const std::span<const MilliFunds> source = m_ours.weightRow(row);
                for (const std::int32_t column : openColumns)
                {
                    weights.push_back(source[static_cast<std::size_t>(column)]);
                }
                continue;
            }
            const std::size_t moverSlot =
                static_cast<std::size_t>(mover - facts.moverRows.begin());
            const std::vector<MilliFunds> acting =
                actingWeights(row, facts.movers[moverSlot]);
            for (const std::int32_t column : openColumns)
            {
                weights.push_back(acting[static_cast<std::size_t>(column)]);
            }
        }
        AssignmentSolver solver;
        solver.solve(m_ours.rowCount(), static_cast<std::int32_t>(openColumns.size()), weights);
        return solver.optimum();
    }

    std::vector<PlanRowAction> JointPlanStockValuer::planActions(
        const ContinuationKey & key)
    {
        std::vector<PlanRowAction> actions;
        actions.reserve(key.size());
        for (const CanonicalPlanActionKey & action : key)
        {
            actions.push_back(PlanRowAction{
                .row = action.row(),
                .destination = TilePoint{action.x(), action.y()},
                .captures = action.captures(),
                .stepsLost = action.stepsLost(),
                .destroyed = action.destroyed(),
            });
        }
        return actions;
    }

    CheapRowTerms JointPlanStockValuer::cheapRowTerms(
        const SequentialInstance & instance,
        const SequentialRow & row,
        std::int32_t rowIndex,
        std::span<const std::int32_t> nodeSlots)
    {
        CheapRowTerms terms;
        terms.row = rowIndex;
        for (std::int32_t head = 0;
             head < instance.nodeCount();
             ++head)
        {
            const std::size_t slot = static_cast<std::size_t>(head);
            const MilliFunds enriched =
                row.weight[slot] +
                SequentialDetail::continuationGain(instance, row, head);
            if (enriched <= 0)
            {
                continue;
            }
            const std::vector<SequentialDetail::WitnessStep> realized =
                SequentialDetail::realizeWitness(instance, row, head);
            CheapChain chain;
            chain.steps.reserve(realized.size());
            for (std::size_t step = 0;
                 step < realized.size();
                 ++step)
            {
                const SequentialDetail::WitnessStep & source =
                    realized[step];
                const MilliFunds gain =
                    step == 0
                        ? row.weight[
                              static_cast<std::size_t>(source.node)]
                        : SequentialDetail::chainPrize(
                              instance, source.node, source.turn);
                chain.steps.push_back(CheapChainStep{
                    .column =
                        nodeSlots[static_cast<std::size_t>(source.node)],
                    .turn = source.turn,
                    .gain = gain,
                });
                chain.value += gain;
            }
            if (chain.value != enriched)
            {
                return CheapRowTerms{};
            }
            terms.chains.push_back(std::move(chain));
        }
        return terms;
    }

    DiagnosticStockQuotes JointPlanStockValuer::diagnosticQuotes(
        const PropertyStockField & field, std::span<const ContinuationKey> keys,
        const DiagnosticRefinementLimits & limits)
    {
        const auto started = std::chrono::steady_clock::now();
        DiagnosticStockQuotes result;
        result.limits = limits;
        result.quotes.resize(keys.size());
        const DiagnosticCatalogKeys selected = diagnosticCatalogKeys(field, keys);
        const std::vector<std::size_t> & validIndices = selected.validIndices;
        const std::vector<ContinuationKey> & validKeys = selected.validKeys;
        for (const std::size_t index : validIndices)
        {
            result.quotes[index].status = DiagnosticStockStatus::PreparationFailed;
        }
        result.actions = selected.catalog.size();
        auto scratch = validKeys.empty() ? std::nullopt :
            buildCheapModelFor(field, planActions(selected.catalog), true, &result.classesBuilt, nullptr);
        result.preparationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (scratch)
        {
            auto quotes = diagnosticStockPrices(scratch->model, validKeys);
            for (std::size_t index = 0; index < quotes.size(); ++index)
            {
                auto & quote = quotes[index];
                if (quote.valid)
                {
                    const auto refinementStarted = std::chrono::steady_clock::now();
                    quote.valid = refineDiagnosticQuote(*scratch, validKeys[index], limits, quote);
                    quote.refinementNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - refinementStarted).count();
                    if (!quote.valid)
                    {
                        quote.status = DiagnosticStockStatus::RefinementFailed;
                    }
                }
                result.quotes[validIndices[index]] = std::move(quote);
            }
        }
        result.nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    }

    bool JointPlanStockValuer::refineDiagnosticQuote(
        CheapModelBuild & build, const ContinuationKey & key, const DiagnosticRefinementLimits & limits,
        DiagnosticStockQuote & quote)
    {
        const std::int64_t stateCap = limits.stateCap;
        if (!build.future || stateCap <= 0)
        {
            return false;
        }
        const MasterFutureCatalog & catalog = *build.future;
        PropertyStockField & field = *build.field;
        const std::vector<PlanRowAction> actions = planActions(key);
        const PropertyStockField::JointActionFacts facts = field.jointActionFacts(actions);
        if (facts.capturedColumns != quote.capturedColumns)
        {
            return false;
        }
        SequentialInstance instance;
        instance.horizonTurns = catalog.horizon;
        instance.nodeColumns = catalog.columns;
        instance.classes = catalog.classes;
        instance.rows = catalog.dayRows;
        for (const CanonicalPlanActionKey & action : key)
        {
            const MasterFutureAction* pAction = findMasterFutureAction(catalog, action);
            if (pAction == nullptr)
            {
                return false;
            }
            instance.rows[static_cast<std::size_t>(action.row())] = pAction->row;
        }
        instance.capturedNodes = capturedNodesOf(catalog.columns, facts.capturedColumns);
        const SequentialTierResult tier = solveSequentialPacking(
            instance, field.ourOpenOptimum(facts), stateCap, nullptr, true, SequentialWitnessPolicy::BookedValue,
            SequentialTierAuditMode::CountersOnly, nullptr, nullptr, SequentialPackingPolicy::CompatibleRows);
        quote.searchStates = tier.searchStates;
        quote.refinementCompleted = tier.searchCompleted || tier.certificate;
        const SequentialIntervalRefinement refined = refineSequentialInterval(
            quote.ours, tier, replaySequentialWitness(instance, tier.witness, tier.bookedValue), stateCap);
        if (refined.status != SequentialIntervalStatus::Tightened)
        {
            return false;
        }
        quote.ours = refined.interval;
        if (limits.dualRounds <= 0)
        {
            return true;
        }
        const auto dualStarted = std::chrono::steady_clock::now();
        const bool dualSound = tightenWithDual(catalog, build.model, key, instance, limits, quote);
        quote.dualNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - dualStarted).count();
        return dualSound;
    }

    bool JointPlanStockValuer::validDiagnosticKey(const PropertyStockField & field, const ContinuationKey & key)
    {
        std::set<std::int32_t> rows;
        return std::all_of(key.begin(), key.end(), [&](const auto & action)
        {
            return action.row() >= 0 && action.row() < field.m_ours.rowCount() &&
                action.x() >= 0 && action.x() < field.m_width && action.y() >= 0 && action.y() < field.m_height &&
                rows.insert(action.row()).second;
        });
    }

    JointPlanStockValuer::DiagnosticCatalogKeys JointPlanStockValuer::diagnosticCatalogKeys(
        const PropertyStockField & field, std::span<const ContinuationKey> keys)
    {
        DiagnosticCatalogKeys result;
        std::set<CanonicalPlanActionKey> uniqueActions;
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            if (!validDiagnosticKey(field, keys[index]))
            {
                continue;
            }
            result.validIndices.push_back(index);
            result.validKeys.push_back(keys[index]);
            uniqueActions.insert(keys[index].begin(), keys[index].end());
        }
        result.catalog.assign(uniqueActions.begin(), uniqueActions.end());
        return result;
    }

    std::vector<bool> JointPlanStockValuer::capturedNodesOf(
        std::span<const PropertyStockColumn> nodeColumns, std::span<const std::int32_t> capturedColumns)
    {
        std::vector<bool> captured(nodeColumns.size(), false);
        for (const std::int32_t column : capturedColumns)
        {
            const std::int32_t node = futureNodeOfColumn(nodeColumns, column);
            if (node != NO_SEQUENTIAL_NODE)
            {
                captured[static_cast<std::size_t>(node)] = true;
            }
        }
        return captured;
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    DiagnosticPricingModels JointPlanStockValuer::diagnosticPricingModels(
        const PropertyStockField & field, std::span<const ContinuationKey> keys)
    {
        DiagnosticPricingModels result;
        result.models.resize(keys.size());
        const DiagnosticCatalogKeys selected = diagnosticCatalogKeys(field, keys);
        auto build = selected.validKeys.empty() ? std::nullopt :
            buildCheapModelFor(field, planActions(selected.catalog), true, &result.classes, nullptr);
        if (!build)
        {
            return result;
        }
        for (std::size_t valid = 0; valid < selected.validKeys.size(); ++valid)
        {
            const ContinuationKey & key = selected.validKeys[valid];
            const PropertyStockField::JointActionFacts facts = build->field->jointActionFacts(planActions(key));
            result.models[selected.validIndices[valid]] = dualModel(*build->future, build->model, key,
                capturedNodesOf(build->future->columns, facts.capturedColumns));
        }
        return result;
    }
#endif

    void JointPlanStockValuer::ourStockNodes(const PropertyStockField & field, std::vector<std::int32_t> & nodeSlots,
                                             std::vector<PropertyStockColumn> & nodeColumns)
    {
        for (std::size_t slot = 0;
             slot < field.m_ours.columns.size();
             ++slot)
        {
            const PropertyStockColumn & column =
                field.m_ours.columns[slot];
            if (column.ownerBefore == STOCK_OWNER_AFTER)
            {
                continue;
            }
            nodeSlots.push_back(static_cast<std::int32_t>(slot));
            nodeColumns.push_back(column);
        }
    }

    std::vector<std::int32_t> JointPlanStockValuer::dayClassIndices(
        PropertyStockField & field, const std::vector<std::int32_t> & nodeSlots,
        const std::vector<PropertyStockColumn> & nodeColumns, std::vector<SequentialClassKey> & classKeys,
        std::vector<SequentialClassTable> & classTables)
    {
        std::vector<std::int32_t> classOfRow(
            field.m_ours.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0; rowIndex < field.m_ours.rows.size(); ++rowIndex)
        {
            classOfRow[rowIndex] = cheapClassIndexFor(field, field.m_ours.rows[rowIndex],
                nodeSlots, nodeColumns, classKeys, classTables);
        }
        return classOfRow;
    }

    SequentialRow JointPlanStockValuer::daySequentialRow(PropertyStockField & field, std::int32_t rowIndex,
                                                         const std::vector<std::int32_t> & nodeSlots,
                                                         std::int32_t classIndex)
    {
        return sequentialRowFor(
            field,
            field.m_ours,
            field.m_ours.rows[static_cast<std::size_t>(rowIndex)],
            rowIndex,
            nullptr,
            nodeSlots,
            classIndex);
    }

    std::int32_t JointPlanStockValuer::actionClassIndexFor(
        PropertyStockField & field, const PlanRowAction & action, std::span<const std::int32_t> classOfRow,
        const std::vector<std::int32_t> & nodeSlots, const std::vector<PropertyStockColumn> & nodeColumns,
        std::vector<SequentialClassKey> & classKeys, std::vector<SequentialClassTable> & classTables)
    {
        if (action.row < 0 || action.row >= field.m_ours.rowCount() ||
            action.stepsLost < 0 || action.stepsLost > CanonicalPlanActionKey::MAX_STEPS_LOST)
        {
            return NO_SEQUENTIAL_CLASS;
        }
        if (action.stepsLost == 0 && !action.destroyed)
        {
            return classOfRow[static_cast<std::size_t>(action.row)];
        }
        PropertyStockRow row = field.m_ours.rows[static_cast<std::size_t>(action.row)];
        row.captureRate = replyCaptureRate(row.captureRate, action.stepsLost, action.destroyed);
        return cheapClassIndexFor(field, row, nodeSlots, nodeColumns, classKeys, classTables);
    }

    SequentialRow JointPlanStockValuer::actionSequentialRow(PropertyStockField & field, const PlanRowAction & action,
                                                            const std::vector<std::int32_t> & nodeSlots,
                                                            std::int32_t classIndex, std::int32_t & capturedColumn)
    {
        const PropertyStockRow & source =
            field.m_ours.rows[
                static_cast<std::size_t>(action.row)];
        capturedColumn = NO_STOCK_COLUMN;
        const PropertyStockActor mover = field.actionMover(action, capturedColumn);
        return sequentialRowFor(
            field,
            field.m_ours,
            source,
            action.row,
            &mover,
            nodeSlots,
            classIndex);
    }

    SequentialClassTable JointPlanStockValuer::buildOurClassTable(
        PropertyStockField & field, const PropertyStockRow & row,
        const std::vector<std::int32_t> & nodeSlots, const std::vector<PropertyStockColumn> & nodeColumns)
    {
        const std::vector<std::int32_t> arrivals =
            nodeArrivalsFor(field,
                            row.gridIdentity,
                            row.movementPoints,
                            nodeSlots);
        SequentialClassTable table;
        table.build(
            nodeColumns,
            arrivals,
            captureTurnsFor(
                0,
                row.captureRate,
                field.m_capturePointsToCapture),
            field.m_horizonTurns);
        return table;
    }

    std::int32_t JointPlanStockValuer::cheapClassIndexFor(
        PropertyStockField & field, const PropertyStockRow & row,
        const std::vector<std::int32_t> & nodeSlots, const std::vector<PropertyStockColumn> & nodeColumns,
        std::vector<SequentialClassKey> & classKeys, std::vector<SequentialClassTable> & classTables)
    {
        const SequentialClassKey key{
            row.gridIdentity,
            row.movementPoints,
            row.captureRate,
        };
        const auto known =
            std::find(classKeys.begin(), classKeys.end(), key);
        if (known != classKeys.end())
        {
            return static_cast<std::int32_t>(known - classKeys.begin());
        }
        auto table = buildOurClassTable(field, row, nodeSlots, nodeColumns);
        classKeys.push_back(key);
        classTables.push_back(std::move(table));
        return static_cast<std::int32_t>(classKeys.size()) - 1;
    }

    std::optional<JointPlanStockValuer::CheapModelBuild> JointPlanStockValuer::buildCheapModelFor(
        const PropertyStockField & source, std::span<const PlanRowAction> catalogActions,
        bool retainSequentialRows, std::size_t* classesBuilt, EnemyCeiling priceEnemy)
    {
        std::unique_ptr<PropertyStockField> pricingField =
            std::make_unique<PropertyStockField>(source);
        std::vector<std::int32_t> nodeSlots;
        std::vector<PropertyStockColumn> nodeColumns;
        ourStockNodes(*pricingField, nodeSlots, nodeColumns);

        std::vector<SequentialClassKey> classKeys;
        std::vector<SequentialClassTable> classTables;
        const std::vector<std::int32_t> classOfRow =
            dayClassIndices(*pricingField, nodeSlots, nodeColumns, classKeys, classTables);
        std::vector<std::int32_t> classOfAction;
        classOfAction.reserve(catalogActions.size());
        for (const PlanRowAction & action : catalogActions)
        {
            const std::int32_t classIndex =
                actionClassIndexFor(*pricingField, action, classOfRow, nodeSlots, nodeColumns, classKeys, classTables);
            if (classIndex == NO_SEQUENTIAL_CLASS)
            {
                return std::nullopt;
            }
            classOfAction.push_back(classIndex);
        }
        if (classesBuilt != nullptr)
        {
            *classesBuilt = classTables.size();
        }

        SequentialInstance templateInstance;
        templateInstance.horizonTurns =
            pricingField->m_horizonTurns;
        templateInstance.nodeColumns = nodeColumns;
        templateInstance.classes = classTables;
        templateInstance.capturedNodes.assign(
            nodeSlots.size(), false);
        std::shared_ptr<MasterFutureCatalog> future;
        if (retainSequentialRows)
        {
            future = std::make_shared<MasterFutureCatalog>();
            future->horizon = pricingField->m_horizonTurns;
            future->columns = nodeColumns;
            future->classes = classTables;
        }
        std::vector<CheapRowTerms> dayRows;
        dayRows.reserve(pricingField->m_ours.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < pricingField->m_ours.rows.size();
             ++rowIndex)
        {
            const SequentialRow row = daySequentialRow(
                *pricingField, static_cast<std::int32_t>(rowIndex), nodeSlots, classOfRow[rowIndex]);
            if (future)
            {
                future->dayRows.push_back(row);
            }
            CheapRowTerms terms = cheapRowTerms(
                templateInstance,
                row,
                static_cast<std::int32_t>(rowIndex),
                nodeSlots);
            if (terms.row != static_cast<std::int32_t>(rowIndex))
            {
                return std::nullopt;
            }
            dayRows.push_back(std::move(terms));
        }

        std::vector<CheapActionTerms> actions;
        actions.reserve(catalogActions.size());
        std::size_t actionIndex = 0;
        for (const PlanRowAction & action : catalogActions)
        {
            std::int32_t capturedColumn = NO_STOCK_COLUMN;
            const SequentialRow row =
                actionSequentialRow(*pricingField, action, nodeSlots, classOfAction[actionIndex++], capturedColumn);
            if (future)
            {
                future->actions.push_back({canonicalPlanActionKey(action), row});
            }
            CheapRowTerms terms =
                cheapRowTerms(templateInstance,
                              row,
                              action.row,
                              nodeSlots);
            if (terms.row != action.row)
            {
                return std::nullopt;
            }
            actions.push_back(CheapActionTerms{
                .key = canonicalPlanActionKey(action),
                .row = action.row,
                .capturedColumn = capturedColumn,
                .rowTerms = std::move(terms),
            });
        }
        std::vector<MilliFunds> capturedSwing;
        capturedSwing.reserve(pricingField->m_ours.columns.size());
        for (const PropertyStockColumn & column :
             pricingField->m_ours.columns)
        {
            capturedSwing.push_back(
                ownershipFlipSwing(
                    column,
                    pricingField->m_horizonTurns));
        }
        const MilliFunds enemyCeiling = priceEnemy == nullptr ? 0 : priceEnemy(*pricingField);
        CheapPricingModel model;
        if (!model.install(
                pricingField->m_ours.rowCount(),
                pricingField->m_ours.columnCount(),
                pricingField->m_ownedBaseline,
                std::move(capturedSwing),
                std::move(dayRows),
                std::move(actions),
                enemyCeiling))
        {
            return std::nullopt;
        }
        if (future)
        {
            future->complete = true;
        }
        return CheapModelBuild{std::move(pricingField), std::move(model), std::move(future)};
    }

    std::vector<std::int32_t> JointPlanStockValuer::nodeArrivalsFor(
        PropertyStockField & field,
        std::int32_t gridIdentity,
        std::int32_t movementPoints,
        const std::vector<std::int32_t> & nodeSlots)
    {
        const std::size_t nodeCount = nodeSlots.size();
        std::vector<std::int32_t> arrivals(
            nodeCount * nodeCount,
            UNREACHABLE);
        for (std::size_t from = 0; from < nodeCount; ++from)
        {
            const PropertyStockField::ArrivalVectors & anchored =
                field.arrivalsFrom(
                    gridIdentity,
                    movementPoints,
                    field.m_columnTiles[
                        static_cast<std::size_t>(nodeSlots[from])]);
            for (std::size_t to = 0; to < nodeCount; ++to)
            {
                arrivals[from * nodeCount + to] =
                    anchored.activations[
                        static_cast<std::size_t>(nodeSlots[to])];
            }
        }
        return arrivals;
    }

    SequentialRow JointPlanStockValuer::sequentialRowFor(
        PropertyStockField & field,
        const PropertyStockInstance & instance,
        const PropertyStockRow & row,
        std::int32_t rowIndex,
        const PropertyStockActor* pMover,
        const std::vector<std::int32_t> & nodeSlots,
        std::int32_t classIndex)
    {
        SequentialRow result;
        result.classIndex = classIndex;
        const std::size_t nodeCount = nodeSlots.size();
        result.weight.assign(nodeCount, 0);
        result.ownedTurn.assign(nodeCount, NO_CAPTURE_TURNS);
        const std::int32_t horizon = field.m_horizonTurns;
        const std::int32_t points = field.m_capturePointsToCapture;
        if (pMover == nullptr)
        {
            const PropertyStockField::ArrivalVectors & arrivals =
                field.arrivalsFrom(row.gridIdentity,
                                     row.movementPoints,
                                     row.tile);
            const std::span<const MilliFunds> weights =
                instance.weightRow(rowIndex);
            for (std::size_t node = 0; node < nodeCount; ++node)
            {
                const std::size_t slot =
                    static_cast<std::size_t>(nodeSlots[node]);
                result.weight[node] = weights[slot];
                const std::int32_t owned = ownedTurnsUntil(
                    arrivals.activations[slot],
                    field.carriedFor(row, nodeSlots[node]),
                    row.captureRate,
                    points,
                    horizon);
                result.ownedTurn[node] =
                    sequentialFirstOwnedTurn(owned, horizon);
            }
            return result;
        }

        if (pMover->destroyed || (pMover->stepsLost > 0 && pMover->effectiveCaptureRate(row.captureRate) == 0))
        {
            return result;
        }

        const std::vector<MilliFunds> acting =
            field.actingWeights(rowIndex, *pMover);
        const PropertyStockField::ArrivalVectors & arrivals =
            field.arrivalsFrom(row.gridIdentity,
                                 pMover->movementPoints,
                                 pMover->tile);
        for (std::size_t node = 0; node < nodeCount; ++node)
        {
            const std::size_t slot =
                static_cast<std::size_t>(nodeSlots[node]);
            result.weight[node] = acting[slot];
            const ArrivalFacts arrival = planRowArrival(*pMover, row.captureRate, field.m_columnTiles[slot],
                                                         arrivals.activations[slot]);
            const std::int32_t owned = ownedTurnsUntil(
                arrival.arrivalActivations, arrival.carriedPoints, arrival.ratePerTurn, points, horizon);
            result.ownedTurn[node] =
                sequentialFirstOwnedTurn(owned, horizon);
        }
        return result;
    }

    ReplyStockContext::ReplyStockContext(const PropertyStockField & field)
        : m_field(std::make_unique<PropertyStockField>(field))
    {
        JointPlanStockValuer::ourStockNodes(*m_field, m_nodeSlots, m_nodeColumns);
        m_classOfRow =
            JointPlanStockValuer::dayClassIndices(*m_field, m_nodeSlots, m_nodeColumns, m_classKeys, m_classTables);
        m_dayRows.reserve(m_classOfRow.size());
        for (std::size_t row = 0; row < m_classOfRow.size(); ++row)
        {
            m_dayRows.push_back(JointPlanStockValuer::daySequentialRow(
                *m_field, static_cast<std::int32_t>(row), m_nodeSlots, m_classOfRow[row]));
        }
        m_pricedDayRows.resize(m_dayRows.size());
        m_valid = m_meter.open(std::numeric_limits<std::int64_t>::max());
    }

    ReplyStockPriceInstall ReplyStockContext::installPrices(std::span<const MilliFunds> prices)
    {
        ReplyStockPriceInstall result{ReplyStockStatus::Invalid, m_priceVersion};
        const std::int32_t horizon = m_field->m_horizonTurns;
        if (!m_valid || horizon < 0 || prices.size() != m_nodeColumns.size())
        {
            return result;
        }
        if (!chargeMasterWork(m_meter, prices.size() + 1, 1, REPLY_STOCK_WORK))
        {
            result.status = ReplyStockStatus::LimitReached;
            return result;
        }
        if (!MasterFutureDetail::validMasterFuturePrices(m_nodeColumns, prices))
        {
            return result;
        }
        if (!masterWorkProduct(prices.size(), static_cast<std::size_t>(horizon)))
        {
            result.status = ReplyStockStatus::ArithmeticFailure;
            return result;
        }
        m_prices.assign(prices.begin(), prices.end());
        m_pricedClasses.assign(m_classTables.size(), {});
        m_classPriced.assign(m_classTables.size(), false);
        m_pricedDayRows.assign(m_dayRows.size(), std::nullopt);
        for (auto & [key, entry] : m_actions)
        {
            entry.priced.reset();
        }
        result.version = ++m_priceVersion;
        result.status = ReplyStockStatus::Valid;
        return result;
    }

    ReplyStockCeiling ReplyStockContext::ceiling(const ContinuationKey & key)
    {
        ReplyStockCeiling result;
        const std::int64_t workBefore = work();
        result.priceVersion = m_priceVersion;
        result.status = m_priceVersion == 0 ? ReplyStockStatus::NoPrices : extend(key, result.counters);
        if (result.status == ReplyStockStatus::Valid)
        {
            result.status = composeCeiling(key, result);
        }
        result.work = work() - workBefore;
        return result;
    }

    ReplyStockOwned ReplyStockContext::owned(const ContinuationKey & key)
    {
        ReplyStockOwned result;
        ReplyStockCounters counters;
        result.status = extend(key, counters);
        if (result.status != ReplyStockStatus::Valid)
        {
            return result;
        }
        const std::vector<PropertyStockColumn> & columns = m_field->m_ours.columns;
        std::vector<bool> captured(columns.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const std::int32_t column = m_actions.at(action).capturedColumn;
            if (column != NO_STOCK_COLUMN)
            {
                captured[static_cast<std::size_t>(column)] = true;
            }
        }
        MilliFunds owned = m_field->m_ownedBaseline;
        for (std::size_t column = 0; column < columns.size(); ++column)
        {
            if (captured[column] &&
                !RelativeStockDetail::checkedAdd(owned, ownershipFlipSwing(columns[column], m_field->m_horizonTurns)))
            {
                result.status = ReplyStockStatus::ArithmeticFailure;
                return result;
            }
        }
        result.owned = owned;
        return result;
    }

    ReplyStockStatus ReplyStockContext::extend(const ContinuationKey & key, ReplyStockCounters & counters)
    {
        if (!m_valid)
        {
            return ReplyStockStatus::PreparationFailed;
        }
        if (!JointPlanStockValuer::validDiagnosticKey(*m_field, key))
        {
            return ReplyStockStatus::InvalidKey;
        }
        const std::vector<PlanRowAction> actions = JointPlanStockValuer::planActions(key);
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            if (m_actions.contains(key[index]))
            {
                continue;
            }
            const std::size_t classesBefore = m_classTables.size();
            const std::int32_t classIndex = JointPlanStockValuer::actionClassIndexFor(
                *m_field, actions[index], m_classOfRow, m_nodeSlots, m_nodeColumns, m_classKeys, m_classTables);
            if (classIndex == NO_SEQUENTIAL_CLASS)
            {
                return ReplyStockStatus::PreparationFailed;
            }
            counters.classesAdded += static_cast<std::int64_t>(m_classTables.size() - classesBefore);
            ActionEntry entry;
            entry.row = JointPlanStockValuer::actionSequentialRow(
                *m_field, actions[index], m_nodeSlots, classIndex, entry.capturedColumn);
            entry.capturedNode = capturedNodeOf(m_nodeColumns, entry.capturedColumn);
            if (entry.capturedNode != NO_SEQUENTIAL_NODE)
            {
                removeCapturedNode(entry.row, entry.capturedNode);
            }
            m_actions.emplace(key[index], std::move(entry));
            ++counters.actionsAdded;
        }
        m_pricedClasses.resize(m_classTables.size());
        m_classPriced.resize(m_classTables.size(), false);
        return ReplyStockStatus::Valid;
    }

    ReplyStockFloor ReplyStockContext::floor(const ContinuationKey & key, const ReplyStockFloor * reference)
    {
        ReplyStockFloor result;
        const std::int64_t workBefore = work();
        result.key = key;
        result.priceVersion = m_priceVersion;
        result.status = m_priceVersion == 0 ? ReplyStockStatus::NoPrices : extend(key, result.pricing);
        if (result.status == ReplyStockStatus::Valid)
        {
            result.status = composeFloor(key, reference, result);
        }
        if (result.status != ReplyStockStatus::Valid)
        {
            result.floor = 0;
            result.witness.clear();
        }
        result.work = work() - workBefore;
        return result;
    }

    ReplyStockTune ReplyStockContext::tune(const ContinuationKey & key, std::int32_t rounds, ReplyStockTuneStart start,
                                           const ReplyStockFloor * reference, MilliFunds targetGap)
    {
        ReplyStockTune result;
        result.start = start;
        result.targetGap = targetGap;
        const std::int64_t workBefore = work();
        result.status = start == ReplyStockTuneStart::InstalledPrices && m_priceVersion == 0 ?
            ReplyStockStatus::NoPrices : tuneKey(key, rounds, reference, result);
        result.work = work() - workBefore;
        return result;
    }

    ReplyStockStatus ReplyStockContext::claimKey(const ContinuationKey & key, std::vector<bool> & capturedNodes,
                                                 std::vector<bool> & acting) const
    {
        capturedNodes.assign(m_nodeColumns.size(), false);
        acting.assign(m_dayRows.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const ActionEntry & entry = m_actions.at(action);
            if (entry.capturedNode != NO_SEQUENTIAL_NODE && !claimCapturedNode(capturedNodes, entry.capturedNode))
            {
                return ReplyStockStatus::Invalid;
            }
            acting[static_cast<std::size_t>(action.row())] = true;
        }
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::keyInstance(const ContinuationKey & key, SequentialInstance & instance) const
    {
        std::vector<bool> acting;
        const ReplyStockStatus status = claimKey(key, instance.capturedNodes, acting);
        if (status != ReplyStockStatus::Valid)
        {
            return status;
        }
        instance.horizonTurns = m_field->m_horizonTurns;
        instance.nodeColumns = m_nodeColumns;
        instance.classes = m_classTables;
        instance.rows = m_dayRows;
        for (const CanonicalPlanActionKey & action : key)
        {
            instance.rows[static_cast<std::size_t>(action.row())] = m_actions.at(action).row;
        }
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::composeCeiling(const ContinuationKey & key, ReplyStockCeiling & result)
    {
        std::vector<bool> capturedNodes;
        std::vector<bool> acting;
        const ReplyStockStatus claimed = claimKey(key, capturedNodes, acting);
        if (claimed != ReplyStockStatus::Valid)
        {
            return claimed;
        }
        for (std::size_t row = 0; row < m_dayRows.size(); ++row)
        {
            const ReplyStockStatus status = acting[row] ? ReplyStockStatus::Valid :
                rowUpper(m_dayRows[row], m_pricedDayRows[row], result.counters);
            if (status != ReplyStockStatus::Valid)
            {
                return status;
            }
        }
        for (const CanonicalPlanActionKey & action : key)
        {
            ActionEntry & entry = m_actions.at(action);
            if (!chargeMasterWork(m_meter, 1, 1, REPLY_STOCK_WORK))
            {
                return ReplyStockStatus::LimitReached;
            }
            const ReplyStockStatus status = rowUpper(entry.row, entry.priced, result.counters);
            if (status != ReplyStockStatus::Valid)
            {
                return status;
            }
        }
        MilliFunds ceiling = 0;
        if (!MasterPlanPricingDetail::addMasterPlanTerms(ceiling, m_prices))
        {
            return ReplyStockStatus::ArithmeticFailure;
        }
        for (std::size_t row = 0; row < m_dayRows.size(); ++row)
        {
            if (!acting[row] && !RelativeStockDetail::checkedAdd(ceiling, m_pricedDayRows[row]->upper))
            {
                return ReplyStockStatus::ArithmeticFailure;
            }
        }
        for (const CanonicalPlanActionKey & action : key)
        {
            const ActionEntry & entry = m_actions.at(action);
            const auto value = MasterPlanPricingDetail::masterPlanCandidateValue(
                KEY_IMMEDIATE_VALUE, entry.capturedNode, entry.priced->upper, m_prices);
            if (!value || !RelativeStockDetail::checkedAdd(ceiling, *value))
            {
                return ReplyStockStatus::ArithmeticFailure;
            }
        }
        result.ceiling = ceiling;
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::composeFloor(const ContinuationKey & key, const ReplyStockFloor * reference,
                                                     ReplyStockFloor & result)
    {
        ReplyStockCeiling priced;
        priced.counters = result.pricing;
        ReplyStockStatus status = composeCeiling(key, priced);
        result.pricing = priced.counters;
        SequentialInstance instance;
        if (status == ReplyStockStatus::Valid)
        {
            status = keyInstance(key, instance);
        }
        if (status != ReplyStockStatus::Valid)
        {
            return status;
        }
        const std::size_t rowCount = instance.rows.size();
        const std::size_t nodeCount = m_nodeColumns.size();
        std::vector<bool> used(nodeCount, false);
        const auto freeNode = [&](std::int32_t node)
        {
            return node >= 0 && static_cast<std::size_t>(node) < nodeCount &&
                !instance.capturedNodes[static_cast<std::size_t>(node)] && !used[static_cast<std::size_t>(node)];
        };
        // A prefix worth nothing only blocks nodes, so its row goes on to the relaxed chain and fallback instead.
        const auto releaseWorthless = [&](auto & steps, MilliFunds value)
        {
            if (value > 0)
            {
                return false;
            }
            for (const auto & step : steps)
            {
                used[static_cast<std::size_t>(step.node)] = false;
            }
            steps.clear();
            return true;
        };
        const auto walk = [&](std::span<const std::int32_t> nodes, const SequentialRow & row, BlockedNode blocked,
                              std::vector<SequentialDetail::WitnessStep> & steps)
        {
            const SequentialClassTable & table = m_classTables[static_cast<std::size_t>(row.classIndex)];
            for (const std::int32_t node : nodes)
            {
                if (!chargeFloor(1))
                {
                    return false;
                }
                const bool free = freeNode(node);
                const std::int32_t turn = !free ? NO_CAPTURE_TURNS :
                    steps.empty() ? row.ownedTurn[static_cast<std::size_t>(node)] :
                                    table.continuationOwnedTurn(steps.back().node, steps.back().turn, node);
                // A relaxed chain keeps a free untimed head, a legal one-step witness when its weight is positive.
                if (!free || (turn == NO_CAPTURE_TURNS && (!steps.empty() || blocked == BlockedNode::Skip)))
                {
                    if (blocked == BlockedNode::Stop)
                    {
                        break;
                    }
                    continue;
                }
                used[static_cast<std::size_t>(node)] = true;
                steps.push_back({node, turn});
            }
            return true;
        };
        std::vector<std::optional<SequentialRowWitness>> chosen(rowCount);
        std::vector<bool> reused(rowCount, false);
        std::vector<const SequentialRowWitness*> adapting;
        const std::vector<const CanonicalPlanActionKey*> variants = rowVariants(key, rowCount);
        ReplyStockFloorCounters & counters = result.counters;
        const auto keepChain = [&](std::size_t slot, std::vector<SequentialDetail::WitnessStep> & steps,
                                   std::int64_t & counter)
        {
            SequentialRowWitness witness =
                SequentialDetail::ownedWitness(instance, instance.rows[slot], static_cast<std::int32_t>(slot), steps);
            releaseWorthless(steps, witness.value);
            if (steps.empty())
            {
                return false;
            }
            ++counter;
            chosen[slot] = std::move(witness);
            return true;
        };
        if (reference != nullptr && reference->status == ReplyStockStatus::Valid &&
            JointPlanStockValuer::validDiagnosticKey(*m_field, reference->key))
        {
            const std::vector<const CanonicalPlanActionKey*> referenceVariants =
                rowVariants(reference->key, rowCount);
            std::int32_t previousRow = NO_STOCK_ROW;
            for (const SequentialRowWitness & source : reference->witness)
            {
                const auto slot = static_cast<std::size_t>(source.row);
                if (source.row <= previousRow || slot >= rowCount)
                {
                    continue;
                }
                previousRow = source.row;
                if (!sameVariant(variants[slot], referenceVariants[slot]))
                {
                    adapting.push_back(&source);
                    continue;
                }
                SequentialRowWitness kept;
                kept.row = source.row;
                for (const SequentialWitnessStep & step : source.steps)
                {
                    if (!chargeFloor(1))
                    {
                        return ReplyStockStatus::LimitReached;
                    }
                    if (!freeNode(step.node))
                    {
                        break;
                    }
                    if (!RelativeStockDetail::checkedAdd(kept.value, step.gain))
                    {
                        return ReplyStockStatus::ArithmeticFailure;
                    }
                    used[static_cast<std::size_t>(step.node)] = true;
                    kept.steps.push_back(step);
                }
                if (!releaseWorthless(kept.steps, kept.value) && !kept.steps.empty())
                {
                    chosen[slot] = std::move(kept);
                    reused[slot] = true;
                    ++counters.reusedRows;
                }
            }
        }
        // A changed row re-times its own reference nodes, which no other reference row holds.
        for (const SequentialRowWitness* source : adapting)
        {
            const auto slot = static_cast<std::size_t>(source->row);
            std::vector<std::int32_t> nodes;
            for (const SequentialWitnessStep & step : source->steps)
            {
                nodes.push_back(step.node);
            }
            std::vector<SequentialDetail::WitnessStep> steps;
            if (!walk(nodes, instance.rows[slot], BlockedNode::Skip, steps))
            {
                return ReplyStockStatus::LimitReached;
            }
            keepChain(slot, steps, counters.adaptedRows);
        }
        for (std::size_t slot = 0; slot < rowCount; ++slot)
        {
            if (chosen[slot])
            {
                continue;
            }
            const SequentialRow & row = instance.rows[slot];
            const std::optional<PricedRow> & pricedRow =
                variants[slot] == nullptr ? m_pricedDayRows[slot] : m_actions.at(*variants[slot]).priced;
            std::vector<SequentialDetail::WitnessStep> steps;
            if (!walk(pricedRow->chain, row, BlockedNode::Stop, steps))
            {
                return ReplyStockStatus::LimitReached;
            }
            if (keepChain(slot, steps, counters.prefixRows))
            {
                continue;
            }
            if (!chargeFloor(nodeCount))
            {
                return ReplyStockStatus::LimitReached;
            }
            std::int32_t best = NO_SEQUENTIAL_NODE;
            for (std::int32_t node = 0; node < static_cast<std::int32_t>(nodeCount); ++node)
            {
                const MilliFunds weight = row.weight[static_cast<std::size_t>(node)];
                if (freeNode(node) && weight > 0 &&
                    (best == NO_SEQUENTIAL_NODE || weight > row.weight[static_cast<std::size_t>(best)]))
                {
                    best = node;
                }
            }
            if (best == NO_SEQUENTIAL_NODE)
            {
                ++counters.emptyRows;
                continue;
            }
            used[static_cast<std::size_t>(best)] = true;
            steps.push_back({best, row.ownedTurn[static_cast<std::size_t>(best)]});
            chosen[slot] = SequentialDetail::ownedWitness(instance, row, static_cast<std::int32_t>(slot), steps);
            ++counters.fallbackRows;
        }
        std::vector<SequentialRowWitness> rows;
        for (std::optional<SequentialRowWitness> & row : chosen)
        {
            if (row)
            {
                rows.push_back(std::move(*row));
            }
        }
        const ReplyStockStatus published = publishFloor(instance, rows, result);
        if (published == ReplyStockStatus::Valid)
        {
            polishFloor(key, instance, reused, result);
        }
        return published;
    }

    // Changed rows go first so they reclaim their natural nodes before unchanged rows extend onto them.
    void ReplyStockContext::polishFloor(const ContinuationKey & key, const SequentialInstance & instance,
                                        const std::vector<bool> & reused, ReplyStockFloor & result)
    {
        const std::size_t rowCount = instance.rows.size();
        const std::optional<MasterPlanPricingModel> model = forcedModel(key);
        if (!model || model->rowActors.size() != rowCount)
        {
            result.polish = ReplyStockFloorPolish::Rejected;
            return;
        }
        MasterPlanWitness start;
        start.rows.resize(rowCount);
        for (std::size_t slot = 0; slot < rowCount; ++slot)
        {
            start.rows[slot].row = static_cast<std::int32_t>(slot);
        }
        for (const SequentialRowWitness & row : result.witness)
        {
            start.rows[static_cast<std::size_t>(row.row)] = row;
        }
        for (const MasterPlanPricingActor & actor : model->actors)
        {
            start.candidateIndices.push_back(actor.candidates.front().candidateIndex);
        }
        std::vector<std::int32_t> order;
        for (const bool wasReused : {false, true})
        {
            for (std::size_t slot = 0; slot < rowCount; ++slot)
            {
                if (reused[slot] == wasReused)
                {
                    order.push_back(static_cast<std::int32_t>(slot));
                }
            }
        }
        const auto searchPass = rowSearchWork(rowCount, m_nodeColumns.size(), m_field->m_horizonTurns);
        const auto cap = searchPass ? masterWorkProduct(static_cast<std::size_t>(MASTER_PLAN_POLISH_PASSES),
                                                        static_cast<std::size_t>(*searchPass))
                                    : std::nullopt;
        const std::int64_t allowed = cap ? std::min(std::max(*cap, MIN_TUNE_BUILD_WORK), m_meter.balance()) : 0;
        RefinementLedger scratch;
        if (allowed <= 0 || !scratch.open(allowed))
        {
            result.polish = ReplyStockFloorPolish::LimitReached;
            return;
        }
        const MasterPlanPolishResult polished = polishMasterPlanWitness(*model, start, order, allowed, scratch);
        if (!m_meter.consume(scratch.stats().granted, REPLY_FLOOR_WORK) ||
            polished.status == MasterPlanPricingStatus::LimitReached)
        {
            result.polish = ReplyStockFloorPolish::LimitReached;
            return;
        }
        ReplyStockFloor candidate;
        const ReplyStockStatus published = polished.status == MasterPlanPricingStatus::Valid ?
            publishFloor(instance, polished.witness.rows, candidate) : ReplyStockStatus::Invalid;
        if (published != ReplyStockStatus::Valid)
        {
            result.polish = published == ReplyStockStatus::LimitReached ? ReplyStockFloorPolish::LimitReached
                                                                         : ReplyStockFloorPolish::Rejected;
            return;
        }
        if (candidate.floor <= result.floor)
        {
            result.polish = ReplyStockFloorPolish::Unchanged;
            return;
        }
        for (std::size_t slot = 0; slot < rowCount; ++slot)
        {
            if (polished.witness.rows[slot].steps != start.rows[slot].steps)
            {
                ++result.counters.polishedRows;
            }
        }
        result.floor = candidate.floor;
        result.witness = std::move(candidate.witness);
        result.counters.steps = candidate.counters.steps;
        result.polish = ReplyStockFloorPolish::Improved;
    }

    ReplyStockStatus ReplyStockContext::publishFloor(const SequentialInstance & instance,
                                                     std::span<const SequentialRowWitness> source,
                                                     ReplyStockFloor & result)
    {
        std::vector<SequentialRowWitness> rows;
        MilliFunds value = 0;
        if (!compactWitness(source, rows, value))
        {
            return ReplyStockStatus::ArithmeticFailure;
        }
        const std::size_t steps = witnessSteps(rows);
        if (!chargeFloor(rows.size() + m_nodeColumns.size() + steps))
        {
            return ReplyStockStatus::LimitReached;
        }
        if (!replaySequentialWitness(instance, rows, value))
        {
            return ReplyStockStatus::ReplayRejected;
        }
        result.floor = value;
        result.witness = std::move(rows);
        result.counters.steps = static_cast<std::int64_t>(steps);
        return ReplyStockStatus::Valid;
    }

    bool ReplyStockContext::chargeFloor(std::size_t work)
    {
        return chargeMasterWork(m_meter, work, 1, REPLY_FLOOR_WORK);
    }

    std::optional<MasterPlanPricingModel> ReplyStockContext::forcedModel(const ContinuationKey & key) const
    {
        return forcedPricingModel(m_field->m_horizonTurns, m_nodeColumns, m_classTables, m_dayRows, key,
            [&](const CanonicalPlanActionKey & action)
        {
            const auto found = m_actions.find(action);
            return found == m_actions.end() ? ForcedAction{} :
                ForcedAction{&found->second.row, found->second.capturedNode};
        }, nullptr);
    }

    ReplyStockStatus ReplyStockContext::tuneLimits(const MasterPlanPricingModel & model, std::int32_t rounds,
                                                   ReplyStockTuneLimits & result)
    {
        const MasterFutureCatalog & catalog = model.catalog;
        const std::size_t nodes = catalog.columns.size();
        const auto turns = static_cast<std::size_t>(std::max(catalog.horizon, 1));
        const std::size_t transitionTurns = turns - 1;
        RefinementLedger probe;
        const std::vector<MilliFunds> zeroPrices(nodes, 0);
        if (rounds <= 0 || !probe.open(std::numeric_limits<std::int64_t>::max()))
        {
            return ReplyStockStatus::Invalid;
        }
        const MasterPlanPricingStatus probed = priceMasterPlan(model, zeroPrices, probe).status;
        if (!m_meter.consume(probe.stats().granted, REPLY_STOCK_WORK))
        {
            return ReplyStockStatus::LimitReached;
        }
        if (probed != MasterPlanPricingStatus::Valid)
        {
            return replyStatusOf(probed);
        }
        // The probe ran at one price vector; every row chain may reach the horizon at another.
        const auto chains = masterWorkProduct(catalog.dayRows.size() + catalog.actions.size(), turns);
        const auto evaluation = chains ?
            addWork(probe.stats().granted, addWork(*chains, masterWorkProduct(nodes, PRICE_STEP_TERMS_PER_NODE))) :
            std::nullopt;
        const auto squared = masterWorkProduct(nodes, nodes);
        const auto transitions = squared ? masterWorkProduct(static_cast<std::size_t>(*squared), transitionTurns)
                                         : std::nullopt;
        const auto cells = masterWorkProduct(nodes, turns);
        const auto table = cells ? addWork(*cells + 1, transitions) : std::nullopt;
        const auto tables = table ? masterWorkProduct(catalog.classes.size(), static_cast<std::size_t>(*table))
                                  : std::nullopt;
        const auto searchPass = rowSearchWork(catalog.dayRows.size(), nodes, catalog.horizon);
        const auto search = searchPass ? masterWorkProduct(WITNESS_SEARCH_PASSES, static_cast<std::size_t>(*searchPass))
                                       : std::nullopt;
        const auto summed = tables ? addWork(*tables, search) : std::nullopt;
        const auto build = summed ? std::optional(std::max(*summed, MIN_TUNE_BUILD_WORK)) : std::nullopt;
        const auto interval = static_cast<std::size_t>(MASTER_PLAN_WITNESS_INTERVAL);
        const std::size_t considerations = (static_cast<std::size_t>(rounds) + interval - 1) / interval + 1;
        const auto builds = masterWorkProduct(WITNESS_BUILDS_PER_CONSIDERATION, considerations);
        if (!evaluation || !build || !builds)
        {
            return ReplyStockStatus::ArithmeticFailure;
        }
        const auto pricing =
            masterWorkProduct(static_cast<std::size_t>(*evaluation), static_cast<std::size_t>(rounds) + 1);
        const auto witness = masterWorkProduct(static_cast<std::size_t>(*build), static_cast<std::size_t>(*builds));
        const auto maximum = pricing ? addWork(*pricing, witness) : std::nullopt;
        if (!maximum)
        {
            return ReplyStockStatus::ArithmeticFailure;
        }
        result.evaluationWork = *evaluation;
        result.pricingWork = *pricing;
        result.buildWork = *build;
        result.builds = *builds;
        result.limits = MasterPlanPricingLimits{rounds, *maximum, *build};
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::tuneKey(const ContinuationKey & key, std::int32_t rounds,
                                                const ReplyStockFloor * reference, ReplyStockTune & result)
    {
        ReplyStockCounters counters;
        const ReplyStockStatus extended =
            rounds <= 0 || result.targetGap < 0 ? ReplyStockStatus::Invalid : extend(key, counters);
        if (extended != ReplyStockStatus::Valid)
        {
            return extended;
        }
        // The key's own floor at the seed prices lets the tuner stop early and, as the reference, bounds the result.
        std::optional<ReplyStockFloor> seeded;
        if (result.targetGap > 0 && result.start == ReplyStockTuneStart::InstalledPrices)
        {
            seeded = floor(key, reference);
            if (seeded->status != ReplyStockStatus::Valid)
            {
                seeded.reset();
            }
        }
        const ReplyStockFloor * verification = seeded ? &*seeded : reference;
        const std::optional<MasterPlanPricingModel> model = forcedModel(key);
        if (!model)
        {
            return ReplyStockStatus::Invalid;
        }
        const ReplyStockStatus limited = tuneLimits(*model, rounds, result.limits);
        if (limited != ReplyStockStatus::Valid)
        {
            return limited;
        }
        result.limits.limits.targetGap = result.targetGap;
        if (seeded)
        {
            result.limits.limits.knownFloor = seeded->floor;
        }
        RefinementLedger ledger;
        if (!ledger.open(result.limits.limits.maximumWork))
        {
            return ReplyStockStatus::ArithmeticFailure;
        }
        const std::span<const MilliFunds> seed =
            result.start == ReplyStockTuneStart::InstalledPrices ? std::span<const MilliFunds>(m_prices) :
                                                                   std::span<const MilliFunds>();
        const MasterPlanPricingResult tuned = tuneMasterPlanPrices(*model, result.limits.limits, ledger, {}, seed);
        result.tunerStatus = tuned.status;
        result.rounds = tuned.rounds;
        result.stoppedOnTarget = tuned.stoppedOnTarget;
        result.tunerWork = ledger.stats().granted;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        result.tunerPricingWork = tuned.pricingWork;
        result.tunerWitnessWork = tuned.witnessWork;
        result.witnessStats = tuned.witnessStats;
#endif
        if (!m_meter.consume(result.tunerWork, REPLY_STOCK_WORK))
        {
            return ReplyStockStatus::LimitReached;
        }
        if (dualFailure(tuned.status))
        {
            return replyStatusOf(tuned.status);
        }
        // Only an evaluated round leaves prices, and an empty vector is correct for a field without stock nodes.
        if (tuned.rounds <= 0 || tuned.prices.size() != m_nodeColumns.size())
        {
            return ReplyStockStatus::LimitReached;
        }
        const ReplyStockPriceInstall installed = installPrices(tuned.prices);
        if (installed.status != ReplyStockStatus::Valid)
        {
            return installed.status;
        }
        const ReplyStockCeiling checked = ceiling(key);
        if (checked.status != ReplyStockStatus::Valid)
        {
            return checked.status;
        }
        result.ceiling = checked.ceiling;
        result.floor = floor(key, verification);
        if (seeded)
        {
            // The seed floor pins the result from below, but the caller's reference alone can replay higher.
            ReplyStockFloor callers = floor(key, reference);
            if (callers.status == ReplyStockStatus::Valid &&
                (result.floor.status != ReplyStockStatus::Valid || callers.floor > result.floor.floor))
            {
                result.floor = std::move(callers);
            }
        }
        if (result.floor.status != ReplyStockStatus::Valid)
        {
            return result.floor.status;
        }
        result.floorSource = ReplyStockFloorSource::Prefix;
        SequentialInstance instance;
        ReplyStockFloor tunerFloor;
        tunerFloor.key = key;
        tunerFloor.priceVersion = m_priceVersion;
        if (tuned.witness.valid && keyInstance(key, instance) == ReplyStockStatus::Valid &&
            publishFloor(instance, tuned.witness.rows, tunerFloor) == ReplyStockStatus::Valid &&
            tunerFloor.floor >= result.floor.floor)
        {
            tunerFloor.status = ReplyStockStatus::Valid;
            result.floor = std::move(tunerFloor);
            result.floorSource = ReplyStockFloorSource::Tuner;
        }
        return result.floor.floor > result.ceiling ? ReplyStockStatus::Inverted : ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::rowUpper(const SequentialRow & row, std::optional<PricedRow> & cached,
                                                 ReplyStockCounters & counters)
    {
        if (cached)
        {
            ++counters.rowHits;
            return ReplyStockStatus::Valid;
        }
        const ReplyStockStatus priced = priceClass(row.classIndex, counters);
        if (priced != ReplyStockStatus::Valid)
        {
            return priced;
        }
        MasterFutureRowPrice price;
        const MasterFutureStatus status = MasterFutureDetail::priceMasterFutureRow(row, m_classTables,
            m_pricedClasses, m_field->m_horizonTurns, m_prices, m_meter, REPLY_STOCK_WORK, price);
        if (status != MasterFutureStatus::Valid)
        {
            return replyStatusOf(status);
        }
        cached = PricedRow{price.upper, std::move(price.nodes)};
        ++counters.rowScans;
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::priceClass(std::int32_t classIndex, ReplyStockCounters & counters)
    {
        if (classIndex < 0 || static_cast<std::size_t>(classIndex) >= m_classTables.size())
        {
            return ReplyStockStatus::Invalid;
        }
        const auto slot = static_cast<std::size_t>(classIndex);
        if (m_classPriced[slot])
        {
            ++counters.tableHits;
            return ReplyStockStatus::Valid;
        }
        // The share of priceMasterFuture's per-pricing tariff that each class adds.
        if (!chargeMasterWork(m_meter, 1, 1, REPLY_STOCK_WORK))
        {
            return ReplyStockStatus::LimitReached;
        }
        const MasterFutureStatus status = MasterFutureDetail::priceMasterFutureClass(m_classTables[slot],
            m_nodeColumns, m_field->m_horizonTurns, m_prices, m_meter, REPLY_STOCK_WORK, m_pricedClasses[slot]);
        if (status != MasterFutureStatus::Valid)
        {
            return replyStatusOf(status);
        }
        m_classPriced[slot] = true;
        ++counters.classTablesPriced;
        return ReplyStockStatus::Valid;
    }
}
