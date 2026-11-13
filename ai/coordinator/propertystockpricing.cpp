#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/masterplanpricing.h"
#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace Coordinator;

    bool dualFailure(MasterPlanPricingStatus status)
    {
        return status == MasterPlanPricingStatus::ArithmeticFailure || status == MasterPlanPricingStatus::Invalid ||
            status == MasterPlanPricingStatus::Inconsistent;
    }

    std::optional<MasterPlanPricingModel> dualModel(const MasterFutureCatalog & catalog,
                                                    const CheapPricingModel & cheap, const ContinuationKey & key,
                                                    const SequentialInstance & instance)
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
            if (pTerms->capturedColumn != NO_STOCK_COLUMN)
            {
                candidate.capturedNode = futureNodeOfColumn(catalog.columns, pTerms->capturedColumn);
            }
            if (candidate.capturedNode != NO_SEQUENTIAL_NODE)
            {
                const auto node = static_cast<std::size_t>(candidate.capturedNode);
                // The ceiling refunds each captured node price once, so a second capturer of it would be unsound.
                if (capturedNodes[node])
                {
                    return std::nullopt;
                }
                capturedNodes[node] = true;
                removeCapturedNode(candidate.row, candidate.capturedNode);
            }
            model.rowActors[static_cast<std::size_t>(action.row())] = static_cast<std::int32_t>(model.actors.size());
            model.actors.push_back(MasterPlanPricingActor{action.row(), {std::move(candidate)}});
        }
        if (capturedNodes != instance.capturedNodes || !indexMasterPlanActions(model))
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
        const auto model = dualModel(catalog, cheap, key, instance);
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
        std::set<CanonicalPlanActionKey> uniqueActions;
        std::vector<ContinuationKey> validKeys;
        std::vector<std::size_t> validIndices;
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            std::set<std::int32_t> rows;
            const bool valid = std::all_of(keys[index].begin(), keys[index].end(), [&](const auto & action)
            {
                return action.row() >= 0 && action.row() < field.m_ours.rowCount() &&
                    action.x() >= 0 && action.x() < field.m_width && action.y() >= 0 && action.y() < field.m_height &&
                    rows.insert(action.row()).second;
            });
            if (!valid)
            {
                continue;
            }
            result.quotes[index].status = DiagnosticStockStatus::PreparationFailed;
            validIndices.push_back(index);
            validKeys.push_back(keys[index]);
            uniqueActions.insert(keys[index].begin(), keys[index].end());
        }
        const ContinuationKey catalog(uniqueActions.begin(), uniqueActions.end());
        result.actions = catalog.size();
        auto scratch = validKeys.empty() ? std::nullopt :
            buildCheapModelFor(field, planActions(catalog), true, &result.classesBuilt, nullptr);
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
        instance.capturedNodes.assign(catalog.columns.size(), false);
        for (const CanonicalPlanActionKey & action : key)
        {
            const MasterFutureAction* pAction = findMasterFutureAction(catalog, action);
            if (pAction == nullptr)
            {
                return false;
            }
            instance.rows[static_cast<std::size_t>(action.row())] = pAction->row;
        }
        for (const std::int32_t column : facts.capturedColumns)
        {
            const std::int32_t node = futureNodeOfColumn(catalog.columns, column);
            if (node != NO_SEQUENTIAL_NODE)
            {
                instance.capturedNodes[static_cast<std::size_t>(node)] = true;
            }
        }
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
        for (std::size_t slot = 0;
             slot < pricingField->m_ours.columns.size();
             ++slot)
        {
            const PropertyStockColumn & column =
                pricingField->m_ours.columns[slot];
            if (column.ownerBefore == STOCK_OWNER_AFTER)
            {
                continue;
            }
            nodeSlots.push_back(static_cast<std::int32_t>(slot));
            nodeColumns.push_back(column);
        }

        std::vector<SequentialClassKey> classKeys;
        std::vector<SequentialClassTable> classTables;
        std::vector<std::int32_t> classOfRow(
            pricingField->m_ours.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0; rowIndex < pricingField->m_ours.rows.size(); ++rowIndex)
        {
            classOfRow[rowIndex] = cheapClassIndexFor(*pricingField, pricingField->m_ours.rows[rowIndex],
                nodeSlots, nodeColumns, classKeys, classTables);
        }
        std::vector<std::int32_t> classOfAction;
        classOfAction.reserve(catalogActions.size());
        for (const PlanRowAction & action : catalogActions)
        {
            if (action.row < 0 || action.row >= pricingField->m_ours.rowCount() ||
                action.stepsLost < 0 || action.stepsLost > CanonicalPlanActionKey::MAX_STEPS_LOST)
            {
                return std::nullopt;
            }
            if (action.stepsLost == 0 && !action.destroyed)
            {
                classOfAction.push_back(classOfRow[static_cast<std::size_t>(action.row)]);
                continue;
            }
            PropertyStockRow row = pricingField->m_ours.rows[static_cast<std::size_t>(action.row)];
            row.captureRate = replyCaptureRate(row.captureRate, action.stepsLost, action.destroyed);
            classOfAction.push_back(cheapClassIndexFor(*pricingField, row, nodeSlots, nodeColumns, classKeys, classTables));
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
            const SequentialRow row = sequentialRowFor(
                *pricingField,
                pricingField->m_ours,
                pricingField->m_ours.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                nullptr,
                nodeSlots,
                classOfRow[rowIndex]);
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
            const PropertyStockRow & source =
                pricingField->m_ours.rows[
                    static_cast<std::size_t>(action.row)];
            std::int32_t capturedColumn = NO_STOCK_COLUMN;
            const PropertyStockActor mover = pricingField->actionMover(action, capturedColumn);
            const SequentialRow row = sequentialRowFor(
                *pricingField,
                pricingField->m_ours,
                source,
                action.row,
                &mover,
                nodeSlots,
                classOfAction[actionIndex++]);
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

}
