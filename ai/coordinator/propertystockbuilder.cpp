#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <utility>

#include <QString>

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coreai.h"

#include "coreengine/gameconsole.h"

#include "game/gamemap.h"
#include "game/player.h"
#include "game/unit.h"

namespace
{
    std::int32_t tileSlot(std::int32_t width, std::int32_t height, std::int32_t x, std::int32_t y)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
        {
            return Coordinator::NO_STOCK_COLUMN;
        }
        return y * width + x;
    }

    bool isRowCandidate(const Coordinator::KnownUnit & known)
    {
        return known.carrier == Coordinator::KnownUnit::NO_CARRIER && known.canCapture;
    }
}

namespace Coordinator
{
    std::int32_t PropertyStockField::propertyIndexAt(const TilePoint & tile) const
    {
        const std::int32_t slot = tileSlot(m_width, m_height, tile.x, tile.y);
        if (slot == NO_STOCK_COLUMN)
        {
            return NO_PROPERTY_INDEX;
        }
        return m_propertyAtTile[static_cast<std::size_t>(slot)];
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

    std::int32_t PropertyStockField::ourRowOf(std::int32_t knowledgeIndex) const
    {
        for (std::size_t row = 0; row < m_ours.rows.size(); ++row)
        {
            if (m_ours.rows[row].knowledgeIndex == knowledgeIndex)
            {
                return static_cast<std::int32_t>(row);
            }
        }
        return NO_STOCK_ROW;
    }

    std::int32_t PropertyStockField::enemyRowOf(std::int32_t knowledgeIndex) const
    {
        for (std::size_t row = 0; row < m_theirs.rows.size(); ++row)
        {
            if (m_theirs.rows[row].knowledgeIndex == knowledgeIndex)
            {
                return static_cast<std::int32_t>(row);
            }
        }
        return NO_STOCK_ROW;
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

    std::vector<MilliFunds> PropertyStockField::rowWeights(const PropertyStockInstance & instance,
                                                           std::span<const std::int32_t> arrivals,
                                                           const PropertyStockRow & row) const
    {
        std::vector<MilliFunds> weights(instance.columns.size(), 0);
        for (std::size_t slot = 0; slot < instance.columns.size(); ++slot)
        {
            const ArrivalFacts arrival{
                .arrivalActivations = arrivals[slot],
                .carriedPoints = carriedFor(row, static_cast<std::int32_t>(slot)),
                .ratePerTurn = row.captureRate,
            };
            weights[slot] = columnWeight(instance.columns[slot], arrival, m_capturePointsToCapture, m_horizonTurns);
        }
        return weights;
    }

    std::vector<MilliFunds> PropertyStockField::actingWeights(std::int32_t row, const PropertyStockActor & actor)
    {
        const PropertyStockRow & source = m_ours.rows[static_cast<std::size_t>(row)];
        const ArrivalVectors & arrivals = arrivalsFrom(source.gridIdentity, actor.movementPoints, actor.tile);
        std::vector<MilliFunds> weights(m_ours.columns.size(), 0);
        for (std::size_t slot = 0; slot < m_ours.columns.size(); ++slot)
        {
            const std::int32_t carried = m_columnTiles[slot] == actor.tile ? actor.carriedCapturePoints : 0;
            const ArrivalFacts arrival{
                .arrivalActivations = arrivals.activations[slot],
                .carriedPoints = carried,
                .ratePerTurn = source.captureRate,
            };
            weights[slot] = columnWeight(m_ours.columns[slot], arrival, m_capturePointsToCapture, m_horizonTurns);
        }
        return weights;
    }

    const MarginalTable & PropertyStockField::marginalFor(std::int32_t row, std::int32_t excludedColumn)
    {
        const MarginalKey key{row, excludedColumn};
        const auto known = m_marginals.find(key);
        if (known != m_marginals.end())
        {
            return known->second;
        }
        MarginalTable table = buildMarginalTable(m_ours, row, excludedColumn);
        return m_marginals.emplace(key, std::move(table)).first->second;
    }

    void PropertyStockField::flipColumnForEnemy(PropertyStockInstance & instance,
                                                std::int32_t flippedColumn) const
    {
        const std::size_t columnCount = instance.columns.size();
        const std::size_t column = static_cast<std::size_t>(flippedColumn);
        instance.columns[column].ownerBefore = mirroredSign(STOCK_OWNER_AFTER);
        for (std::size_t row = 0; row < instance.rows.size(); ++row)
        {
            const std::size_t offset = row * columnCount + column;
            const ArrivalFacts arrival{
                .arrivalActivations = m_enemyArrivals[offset],
                .carriedPoints = carriedFor(instance.rows[row], flippedColumn),
                .ratePerTurn = instance.rows[row].captureRate,
            };
            instance.weights[offset] = columnWeight(instance.columns[column], arrival,
                                                    m_capturePointsToCapture, m_horizonTurns);
        }
    }

    MilliFunds PropertyStockField::enemyOptimumWith(std::int32_t flippedColumn, std::int32_t removedRow)
    {
        PropertyStockInstance instance = m_theirs;
        if (flippedColumn != NO_STOCK_COLUMN)
        {
            flipColumnForEnemy(instance, flippedColumn);
        }
        const AssignmentSubInstance sub = subInstanceWithout(instance, removedRow, NO_STOCK_COLUMN);
        AssignmentSolver solver;
        solver.solve(sub.rowCount, sub.columnCount, sub.weights);
        return solver.optimum();
    }

    MilliFunds PropertyStockField::jointEnemyOptimum(const std::vector<std::int32_t> & capturedColumns)
    {
        if (capturedColumns.empty())
        {
            return m_enemyOptimum;
        }
        const auto known = m_jointEnemyOptima.find(capturedColumns);
        if (known != m_jointEnemyOptima.end())
        {
            return known->second;
        }
        PropertyStockInstance instance = m_theirs;
        for (const std::int32_t flipped : capturedColumns)
        {
            flipColumnForEnemy(instance, flipped);
        }
        AssignmentSolver solver;
        solver.solve(instance.rowCount(), instance.columnCount(), instance.weights);
        const MilliFunds optimum = solver.optimum();
        m_jointEnemyOptima.emplace(capturedColumns, optimum);
        return optimum;
    }

    MilliFunds PropertyStockField::enemyOptimum(const PropertyStockOutcome & outcome)
    {
        const std::int32_t removedRow = enemyRowOf(outcome.destroyedKnowledgeIndex);
        if (outcome.capturedColumn == NO_STOCK_COLUMN && removedRow == NO_STOCK_ROW)
        {
            return m_enemyOptimum;
        }
        const EnemyKey key{outcome.capturedColumn, removedRow};
        const auto known = m_enemyOptima.find(key);
        if (known != m_enemyOptima.end())
        {
            return known->second;
        }
        const MilliFunds optimum = enemyOptimumWith(outcome.capturedColumn, removedRow);
        m_enemyOptima.emplace(key, optimum);
        return optimum;
    }

    MilliFunds PropertyStockField::ourPositionalStock(const PropertyStockActor & actor,
                                                      const PropertyStockOutcome & outcome)
    {
        const std::int32_t row = ourRowOf(actor.knowledgeIndex);
        const MarginalTable & marginal = marginalFor(row, outcome.capturedColumn);
        if (row == NO_STOCK_ROW || actor.survival == ActorSurvival::Destroyed)
        {
            return marginal.unmatched;
        }
        const std::vector<MilliFunds> weights = actingWeights(row, actor);
        return positionalOptimum(marginal, weights, outcome.capturedColumn);
    }

    MilliFunds PropertyStockField::destinationStock(const PropertyStockActor & actor,
                                                    const PropertyStockOutcome & outcome)
    {
        MilliFunds owned = m_ownedBaseline;
        if (outcome.capturedColumn != NO_STOCK_COLUMN)
        {
            owned += ownershipFlipSwing(m_ours.columns[static_cast<std::size_t>(outcome.capturedColumn)],
                                        m_horizonTurns);
        }
        return owned + ourPositionalStock(actor, outcome) - enemyOptimum(outcome);
    }

    PropertyStockField::JointActionFacts PropertyStockField::jointActionFacts(
        std::span<const PlanRowAction> actions) const
    {
        JointActionFacts facts;
        for (const PlanRowAction & entry : actions)
        {
            const PropertyStockRow & row = m_ours.rows[static_cast<std::size_t>(entry.row)];
            std::int32_t carried = 0;
            const std::int32_t slot = columnSlotAt(entry.destination);
            if (slot != NO_STOCK_COLUMN)
            {
                carried = carriedFor(row, slot);
                if (entry.captures)
                {
                    carried += row.captureRate;
                    if (carried >= m_capturePointsToCapture)
                    {
                        facts.capturedColumns.push_back(slot);
                    }
                }
            }
            facts.moverRows.push_back(entry.row);
            facts.movers.push_back(PropertyStockActor{
                .knowledgeIndex = row.knowledgeIndex,
                .tile = entry.destination,
                .movementPoints = row.movementPoints,
                .carriedCapturePoints = carried,
                .survival = ActorSurvival::Alive,
            });
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

    MilliFunds PropertyStockField::jointStock(std::span<const PlanRowAction> actions)
    {
        const JointActionFacts facts = jointActionFacts(actions);
        MilliFunds owned = m_ownedBaseline;
        for (const std::int32_t column : facts.capturedColumns)
        {
            owned += ownershipFlipSwing(
                m_ours.columns[static_cast<std::size_t>(column)],
                m_horizonTurns);
        }
        return owned + ourOpenOptimum(facts) -
               jointEnemyOptimum(facts.capturedColumns);
    }

    MilliFunds PropertyStockField::stockCeiling() const
    {
        return propertyStockCeiling(std::span<const PropertyStockColumn>(m_ours.columns),
                                    m_ownedBaseline, m_horizonTurns);
    }

    JointPlanStockValuer::JointPlanStockValuer(PropertyStockField & field,
                                               std::span<const KnownUnitLink> unitLinks)
        : m_field(field)
    {
        for (std::size_t index = 0; index < unitLinks.size(); ++index)
        {
            if (unitLinks[index].engineUnitId != NO_UNIT)
            {
                m_knowledgeOf.emplace(unitLinks[index].engineUnitId, static_cast<std::int32_t>(index));
            }
        }
    }

    std::int32_t JointPlanStockValuer::rowOf(std::int32_t engineUnitId) const
    {
        const auto known = m_knowledgeOf.find(engineUnitId);
        if (known == m_knowledgeOf.end())
        {
            return NO_STOCK_ROW;
        }
        return m_field.ourRowOf(known->second);
    }

    std::vector<PlanRowAction> JointPlanStockValuer::planActions(
        const TurnPlan & plan) const
    {
        std::vector<PlanRowAction> actions;
        for (std::int32_t index = 0; index < plan.actionCount(); ++index)
        {
            const PlannedAction & action = plan.action(index);
            if (!isLiveState(action.state))
            {
                continue;
            }
            const std::int32_t row = rowOf(action.unitId);
            if (row == NO_STOCK_ROW)
            {
                continue;
            }
            actions.push_back(PlanRowAction{
                .row = row,
                .destination = action.destination,
                .captures = action.kind == PlanBundleKind::Capture ||
                            action.kind == PlanBundleKind::MoveAndCapture,
            });
        }
        return actions;
    }

    MilliFunds JointPlanStockValuer::planStock(const TurnPlan & plan)
    {
        const std::vector<PlanRowAction> actions = planActions(plan);
        if (actions.empty())
        {
            return originStock();
        }
        return sequentialJointStock(actions);
    }

    MilliFunds JointPlanStockValuer::planStockFloor(const TurnPlan & plan)
    {
        return m_field.jointStock(planActions(plan));
    }

    MilliFunds JointPlanStockValuer::originStock() const
    {
        if (!m_originCached)
        {
            m_sequentialOrigin =
                sequentialJointStock(std::span<const PlanRowAction>());
            m_originCached = true;
        }
        return m_sequentialOrigin;
    }

    void JointPlanStockValuer::ensureOurSequentialState() const
    {
        if (m_ourStateBuilt)
        {
            return;
        }
        const std::vector<PropertyStockColumn> & columns =
            m_field.m_ours.columns;
        m_ourNodeOfColumn.assign(columns.size(), NO_SEQUENTIAL_NODE);
        for (std::size_t slot = 0; slot < columns.size(); ++slot)
        {
            if (columns[slot].ownerBefore == STOCK_OWNER_AFTER)
            {
                continue;
            }
            m_ourNodeOfColumn[slot] =
                static_cast<std::int32_t>(m_ourNodeSlots.size());
            m_ourNodeSlots.push_back(static_cast<std::int32_t>(slot));
            m_ourNodeColumns.push_back(columns[slot]);
        }
        m_ourStateBuilt = true;
    }

    std::vector<std::int32_t> JointPlanStockValuer::nodeArrivalsFor(
        std::int32_t gridIdentity,
        std::int32_t movementPoints,
        const std::vector<std::int32_t> & nodeSlots) const
    {
        const std::size_t nodeCount = nodeSlots.size();
        std::vector<std::int32_t> arrivals(
            nodeCount * nodeCount,
            UNREACHABLE);
        for (std::size_t from = 0; from < nodeCount; ++from)
        {
            const PropertyStockField::ArrivalVectors & anchored =
                m_field.arrivalsFrom(
                    gridIdentity,
                    movementPoints,
                    m_field.m_columnTiles[
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

    std::int32_t JointPlanStockValuer::ourClassIndexFor(
        const PropertyStockRow & row) const
    {
        const SequentialClassKey key{
            row.gridIdentity,
            row.movementPoints,
            row.captureRate,
        };
        for (std::size_t index = 0; index < m_ourClassKeys.size(); ++index)
        {
            if (m_ourClassKeys[index] == key)
            {
                return static_cast<std::int32_t>(index);
            }
        }
        const std::vector<std::int32_t> arrivals =
            nodeArrivalsFor(row.gridIdentity,
                            row.movementPoints,
                            m_ourNodeSlots);
        SequentialClassTable table;
        table.build(
            std::span<const PropertyStockColumn>(m_ourNodeColumns),
            std::span<const std::int32_t>(arrivals),
            captureTurnsFor(0,
                            row.captureRate,
                            m_field.m_capturePointsToCapture),
            m_field.m_horizonTurns);
        m_ourClassKeys.push_back(key);
        m_ourClassTables.push_back(std::move(table));
        return static_cast<std::int32_t>(m_ourClassKeys.size()) - 1;
    }

    SequentialRow JointPlanStockValuer::sequentialRowFor(
        const PropertyStockInstance & instance,
        const PropertyStockRow & row,
        std::int32_t rowIndex,
        const PropertyStockActor* pMover,
        const std::vector<std::int32_t> & nodeSlots,
        std::int32_t classIndex) const
    {
        SequentialRow result;
        result.classIndex = classIndex;
        const std::size_t nodeCount = nodeSlots.size();
        result.weight.assign(nodeCount, 0);
        result.ownedTurn.assign(nodeCount, NO_CAPTURE_TURNS);
        const std::int32_t horizon = m_field.m_horizonTurns;
        const std::int32_t points = m_field.m_capturePointsToCapture;
        if (pMover == nullptr)
        {
            const PropertyStockField::ArrivalVectors & arrivals =
                m_field.arrivalsFrom(row.gridIdentity,
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
                    m_field.carriedFor(row, nodeSlots[node]),
                    row.captureRate,
                    points,
                    horizon);
                result.ownedTurn[node] =
                    sequentialFirstOwnedTurn(owned, horizon);
            }
            return result;
        }

        const std::vector<MilliFunds> acting =
            m_field.actingWeights(rowIndex, *pMover);
        const PropertyStockField::ArrivalVectors & arrivals =
            m_field.arrivalsFrom(row.gridIdentity,
                                 pMover->movementPoints,
                                 pMover->tile);
        for (std::size_t node = 0; node < nodeCount; ++node)
        {
            const std::size_t slot =
                static_cast<std::size_t>(nodeSlots[node]);
            result.weight[node] = acting[slot];
            const std::int32_t carried =
                m_field.m_columnTiles[slot] == pMover->tile
                    ? pMover->carriedCapturePoints
                    : 0;
            const std::int32_t owned = ownedTurnsUntil(
                arrivals.activations[slot],
                carried,
                row.captureRate,
                points,
                horizon);
            result.ownedTurn[node] =
                sequentialFirstOwnedTurn(owned, horizon);
        }
        return result;
    }

    MilliFunds JointPlanStockValuer::enemySequentialOptimum(
        const std::vector<std::int32_t> & capturedColumns) const
    {
        const auto known =
            m_enemySequentialOptima.find(capturedColumns);
        if (known != m_enemySequentialOptima.end())
        {
            return known->second;
        }

        const MilliFunds enemyFloor =
            m_field.jointEnemyOptimum(capturedColumns);
        PropertyStockInstance flipped = m_field.m_theirs;
        for (const std::int32_t column : capturedColumns)
        {
            m_field.flipColumnForEnemy(flipped, column);
        }

        std::vector<std::int32_t> nodeSlots;
        std::vector<PropertyStockColumn> nodeColumns;
        for (std::size_t slot = 0; slot < flipped.columns.size(); ++slot)
        {
            if (flipped.columns[slot].ownerBefore == STOCK_OWNER_AFTER)
            {
                continue;
            }
            nodeSlots.push_back(static_cast<std::int32_t>(slot));
            nodeColumns.push_back(flipped.columns[slot]);
        }

        std::vector<SequentialClassKey> classKeys;
        std::vector<SequentialClassTable> classTables;
        std::vector<std::int32_t> classIndices(
            flipped.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0;
             rowIndex < flipped.rows.size();
             ++rowIndex)
        {
            const PropertyStockRow & row = flipped.rows[rowIndex];
            const SequentialClassKey key{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            };
            for (std::size_t index = 0; index < classKeys.size(); ++index)
            {
                if (classKeys[index] == key)
                {
                    classIndices[rowIndex] =
                        static_cast<std::int32_t>(index);
                    break;
                }
            }
            if (classIndices[rowIndex] != NO_SEQUENTIAL_CLASS)
            {
                continue;
            }
            const std::vector<std::int32_t> arrivals =
                nodeArrivalsFor(row.gridIdentity,
                                row.movementPoints,
                                nodeSlots);
            SequentialClassTable table;
            table.build(
                std::span<const PropertyStockColumn>(nodeColumns),
                std::span<const std::int32_t>(arrivals),
                captureTurnsFor(0,
                                row.captureRate,
                                m_field.m_capturePointsToCapture),
                m_field.m_horizonTurns);
            classKeys.push_back(key);
            classTables.push_back(std::move(table));
            classIndices[rowIndex] =
                static_cast<std::int32_t>(classKeys.size()) - 1;
        }

        SequentialInstance instance;
        instance.horizonTurns = m_field.m_horizonTurns;
        instance.nodeColumns =
            std::span<const PropertyStockColumn>(nodeColumns);
        instance.classes =
            std::span<const SequentialClassTable>(classTables);
        instance.capturedNodes.assign(nodeSlots.size(), false);
        instance.rows.reserve(flipped.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < flipped.rows.size();
             ++rowIndex)
        {
            instance.rows.push_back(sequentialRowFor(
                flipped,
                flipped.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                nullptr,
                nodeSlots,
                classIndices[rowIndex]));
        }
        const SequentialTierResult enemy = solveSequentialPacking(
            instance,
            enemyFloor,
            SEQUENTIAL_SEARCH_STATE_CAP);
        m_enemySequentialOptima.emplace(capturedColumns,
                                        enemy.bookedValue);
        return enemy.bookedValue;
    }

    MilliFunds JointPlanStockValuer::sequentialJointStock(
        std::span<const PlanRowAction> actions) const
    {
        const PropertyStockField::JointActionFacts facts =
            m_field.jointActionFacts(actions);
        MilliFunds owned = m_field.m_ownedBaseline;
        for (const std::int32_t column : facts.capturedColumns)
        {
            owned += ownershipFlipSwing(
                m_field.m_ours.columns[static_cast<std::size_t>(column)],
                m_field.m_horizonTurns);
        }
        const MilliFunds floorOur = m_field.ourOpenOptimum(facts);

        ensureOurSequentialState();
        std::vector<std::int32_t> classIndices(
            m_field.m_ours.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0;
             rowIndex < m_field.m_ours.rows.size();
             ++rowIndex)
        {
            classIndices[rowIndex] =
                ourClassIndexFor(m_field.m_ours.rows[rowIndex]);
        }

        SequentialInstance instance;
        instance.horizonTurns = m_field.m_horizonTurns;
        instance.nodeColumns =
            std::span<const PropertyStockColumn>(m_ourNodeColumns);
        instance.classes =
            std::span<const SequentialClassTable>(m_ourClassTables);
        instance.capturedNodes.assign(m_ourNodeSlots.size(), false);
        for (const std::int32_t column : facts.capturedColumns)
        {
            const std::int32_t node =
                m_ourNodeOfColumn[static_cast<std::size_t>(column)];
            if (node != NO_SEQUENTIAL_NODE)
            {
                instance.capturedNodes[static_cast<std::size_t>(node)] =
                    true;
            }
        }

        instance.rows.reserve(m_field.m_ours.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < m_field.m_ours.rows.size();
             ++rowIndex)
        {
            const PropertyStockActor* pMover = nullptr;
            for (std::size_t moverSlot = 0;
                 moverSlot < facts.moverRows.size();
                 ++moverSlot)
            {
                if (facts.moverRows[moverSlot] ==
                    static_cast<std::int32_t>(rowIndex))
                {
                    pMover = &facts.movers[moverSlot];
                    break;
                }
            }
            instance.rows.push_back(sequentialRowFor(
                m_field.m_ours,
                m_field.m_ours.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                pMover,
                m_ourNodeSlots,
                classIndices[rowIndex]));
        }

        const SequentialTierResult ours = solveSequentialPacking(
            instance,
            floorOur,
            SEQUENTIAL_SEARCH_STATE_CAP);
        const MilliFunds enemyBooked =
            enemySequentialOptimum(facts.capturedColumns);
        return owned + ours.bookedValue - enemyBooked;
    }

    MilliFunds JointPlanStockValuer::stockCeiling() const
    {
        return m_field.stockCeiling();
    }

    bool JointPlanStockValuer::affectsStock(std::int32_t engineUnitId) const
    {
        return rowOf(engineUnitId) != NO_STOCK_ROW;
    }

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns)
    {
        PropertyStockField field;
        field.m_horizonTurns = horizonTurns;
        field.m_width = knowledge.width();
        field.m_height = knowledge.height();
        field.m_capturePointsToCapture = Unit::MAX_CAPTURE_POINTS;
        const std::size_t tileCount = static_cast<std::size_t>(field.m_width) *
                                      static_cast<std::size_t>(field.m_height);
        field.m_columnAtTile.assign(tileCount, NO_STOCK_COLUMN);
        field.m_propertyAtTile.assign(tileCount, NO_PROPERTY_INDEX);
        std::vector<PropertyStockHolding> holdings;
        holdings.reserve(properties.size());
        for (std::size_t index = 0; index < properties.size(); ++index)
        {
            const PropertyFacts & facts = properties[index];
            const PropertyIncome income = captureIncome(facts);
            const OwnerSign owner = captureOwnerBefore(facts.ownerId, knowledge.relation(facts.ownerId));
            holdings.push_back(PropertyStockHolding{income, owner});
            const std::int32_t tile = tileSlot(field.m_width, field.m_height, facts.x, facts.y);
            if (tile == NO_STOCK_COLUMN)
            {
                continue;
            }
            field.m_propertyAtTile[static_cast<std::size_t>(tile)] = static_cast<std::int32_t>(index);
            if (!facts.capturable)
            {
                continue;
            }
            const PropertyStockColumn column{
                .slot = static_cast<std::int32_t>(field.m_columnTiles.size()),
                .tile = TilePoint{facts.x, facts.y},
                .income = income,
                .ownerBefore = owner,
                .buildingIndex = facts.buildingIndex,
            };
            field.m_columnAtTile[static_cast<std::size_t>(tile)] = column.slot;
            field.m_columnTiles.push_back(column.tile);
            field.m_columnCapturer.push_back(facts.capturerIndex);
            field.m_columnCapturePoints.push_back(facts.capturePoints);
            field.m_theirs.columns.push_back(mirroredColumn(column));
            field.m_ours.columns.push_back(column);
        }
        field.m_ownedBaseline = ownedBaseline(holdings, horizonTurns);
        const std::vector<KnownUnit> & units = knowledge.units();
        for (std::size_t index = 0; index < units.size(); ++index)
        {
            const KnownUnit & known = units[index];
            if (!isRowCandidate(known))
            {
                continue;
            }
            const Side side = sideOf(knowledge.relation(known.ownerId));
            if (side == Side::Bystander)
            {
                continue;
            }
            Player* pOwner = map.getPlayer(known.ownerId);
            if (pOwner == nullptr)
            {
                AI_CONSOLE_PRINT("Coordinator::buildPropertyStockField() no owner " +
                                     QString::number(known.ownerId),
                                 GameConsole::eERROR);
                continue;
            }
            const MobilityCostGrid & grid = mobility.grid(map, *pOwner, known.unitId);
            std::size_t gridSlot = 0;
            while (gridSlot < field.m_grids.size() && field.m_grids[gridSlot] != grid)
            {
                ++gridSlot;
            }
            if (gridSlot == field.m_grids.size())
            {
                field.m_grids.push_back(grid);
            }
            const PropertyStockRow row{
                .knowledgeIndex = static_cast<std::int32_t>(index),
                .gridIdentity = static_cast<std::int32_t>(gridSlot),
                .movementPoints = known.movementPoints,
                .capturePoints = known.capturePoints,
                .captureRate = known.captureRate,
                .tile = TilePoint{known.x, known.y},
            };
            if (side == Side::Ours)
            {
                field.m_ours.rows.push_back(row);
            }
            else
            {
                field.m_theirs.rows.push_back(row);
            }
        }
        for (const PropertyStockRow & source : field.m_ours.rows)
        {
            const PropertyStockField::ArrivalVectors & arrivals =
                field.arrivalsFrom(source.gridIdentity, source.movementPoints, source.tile);
            const std::vector<MilliFunds> weights =
                field.rowWeights(field.m_ours, arrivals.activations, source);
            field.m_ours.weights.insert(field.m_ours.weights.end(), weights.begin(), weights.end());
        }
        for (const PropertyStockRow & source : field.m_theirs.rows)
        {
            const PropertyStockField::ArrivalVectors & arrivals =
                field.arrivalsFrom(source.gridIdentity, source.movementPoints, source.tile);
            const std::vector<MilliFunds> weights =
                field.rowWeights(field.m_theirs, arrivals.activations, source);
            field.m_enemyArrivals.insert(field.m_enemyArrivals.end(), arrivals.activations.begin(),
                                         arrivals.activations.end());
            field.m_theirs.weights.insert(field.m_theirs.weights.end(), weights.begin(), weights.end());
        }
        field.m_ourOptimum = instanceOptimum(field.m_ours);
        field.m_enemyOptimum = instanceOptimum(field.m_theirs);
        field.m_originStock = field.m_ownedBaseline + field.m_ourOptimum - field.m_enemyOptimum;
        return field;
    }
}
