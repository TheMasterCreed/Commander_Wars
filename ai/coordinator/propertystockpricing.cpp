#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "ai/coordinator/masterassignmentfuture.h"

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
        const PropertyStockField & field, std::span<const ContinuationKey> keys)
    {
        const auto started = std::chrono::steady_clock::now();
        DiagnosticStockQuotes result;
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
        const auto scratch = validKeys.empty() ? std::nullopt :
            buildCheapModelFor(field, planActions(catalog), false, &result.classesBuilt, nullptr);
        result.preparationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (scratch)
        {
            auto quotes = diagnosticStockPrices(scratch->model, validKeys);
            for (std::size_t index = 0; index < quotes.size(); ++index)
            {
                result.quotes[validIndices[index]] = std::move(quotes[index]);
            }
        }
        result.nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        return result;
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
