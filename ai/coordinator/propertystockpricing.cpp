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
    // Stock keys carry no immediate value, exactly as in the diagnostic dual model.
    constexpr MilliFunds KEY_IMMEDIATE_VALUE = 0;

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

    std::optional<MasterPlanPricingModel> dualModel(const MasterFutureCatalog & catalog,
                                                    const CheapPricingModel & cheap, const ContinuationKey & key,
                                                    const std::vector<bool> & expectedCaptured)
    {
        MasterPlanPricingModel model;
        model.catalog.horizon = catalog.horizon;
        model.catalog.columns = catalog.columns;
        model.catalog.classes = catalog.classes;
        model.catalog.dayRows = catalog.dayRows;
        model.catalog.complete = true;
        model.rowActors.assign(catalog.dayRows.size(), MASTER_PLAN_NO_ACTOR);
        std::vector<bool> capturedNodes(catalog.columns.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const MasterFutureAction* pFuture = findMasterFutureAction(catalog, action);
            const CheapActionTerms* pTerms = cheap.actionTerms(action);
            if (pFuture == nullptr || pTerms == nullptr)
            {
                return std::nullopt;
            }
            MasterPlanPricingCandidate candidate;
            candidate.key = action;
            candidate.row = pFuture->row;
            candidate.capturedNode = capturedNodeOf(catalog.columns, pTerms->capturedColumn);
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
        if (capturedNodes != expectedCaptured || !indexMasterPlanActions(model))
        {
            return std::nullopt;
        }
        return model;
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
            bool summed = true;
            for (const SequentialRowWitness & row : tuned.witness.rows)
            {
                if (row.steps.empty())
                {
                    continue;
                }
                summed = summed && RelativeStockDetail::checkedAdd(value, row.value);
                rows.push_back(row);
            }
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
        m_dayUppers.resize(m_dayRows.size());
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
        m_dayUppers.assign(m_dayRows.size(), std::nullopt);
        for (auto & [key, entry] : m_actions)
        {
            entry.upper.reset();
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

    ReplyStockStatus ReplyStockContext::composeCeiling(const ContinuationKey & key, ReplyStockCeiling & result)
    {
        std::vector<bool> capturedNodes(m_nodeColumns.size(), false);
        std::vector<bool> acting(m_dayRows.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const ActionEntry & entry = m_actions.at(action);
            if (entry.capturedNode != NO_SEQUENTIAL_NODE && !claimCapturedNode(capturedNodes, entry.capturedNode))
            {
                return ReplyStockStatus::Invalid;
            }
            acting[static_cast<std::size_t>(action.row())] = true;
        }
        for (std::size_t row = 0; row < m_dayRows.size(); ++row)
        {
            const ReplyStockStatus status =
                acting[row] ? ReplyStockStatus::Valid : rowUpper(m_dayRows[row], m_dayUppers[row], result.counters);
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
            const ReplyStockStatus status = rowUpper(entry.row, entry.upper, result.counters);
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
            if (!acting[row] && !RelativeStockDetail::checkedAdd(ceiling, *m_dayUppers[row]))
            {
                return ReplyStockStatus::ArithmeticFailure;
            }
        }
        for (const CanonicalPlanActionKey & action : key)
        {
            const ActionEntry & entry = m_actions.at(action);
            const auto value = MasterPlanPricingDetail::masterPlanCandidateValue(
                KEY_IMMEDIATE_VALUE, entry.capturedNode, *entry.upper, m_prices);
            if (!value || !RelativeStockDetail::checkedAdd(ceiling, *value))
            {
                return ReplyStockStatus::ArithmeticFailure;
            }
        }
        result.ceiling = ceiling;
        return ReplyStockStatus::Valid;
    }

    ReplyStockStatus ReplyStockContext::rowUpper(const SequentialRow & row, std::optional<MilliFunds> & cached,
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
        cached = price.upper;
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
