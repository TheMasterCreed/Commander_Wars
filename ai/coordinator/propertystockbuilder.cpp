#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <limits>
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

    MilliFunds PropertyStockField::jointStock(std::span<const PlanRowAction> actions)
    {
        std::vector<std::int32_t> captured;
        std::vector<std::int32_t> moverRows;
        std::vector<PropertyStockActor> movers;
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
                        captured.push_back(slot);
                    }
                }
            }
            moverRows.push_back(entry.row);
            movers.push_back(PropertyStockActor{
                .knowledgeIndex = row.knowledgeIndex,
                .tile = entry.destination,
                .movementPoints = row.movementPoints,
                .carriedCapturePoints = carried,
                .survival = ActorSurvival::Alive,
            });
        }
        std::sort(captured.begin(), captured.end());
        captured.erase(std::unique(captured.begin(), captured.end()), captured.end());
        MilliFunds owned = m_ownedBaseline;
        for (const std::int32_t column : captured)
        {
            owned += ownershipFlipSwing(m_ours.columns[static_cast<std::size_t>(column)], m_horizonTurns);
        }
        std::vector<std::int32_t> openColumns;
        for (std::int32_t column = 0; column < m_ours.columnCount(); ++column)
        {
            if (std::find(captured.begin(), captured.end(), column) == captured.end())
            {
                openColumns.push_back(column);
            }
        }
        std::vector<MilliFunds> weights;
        weights.reserve(static_cast<std::size_t>(m_ours.rowCount()) * openColumns.size());
        for (std::int32_t row = 0; row < m_ours.rowCount(); ++row)
        {
            const auto mover = std::find(moverRows.begin(), moverRows.end(), row);
            if (mover == moverRows.end())
            {
                const std::span<const MilliFunds> source = m_ours.weightRow(row);
                for (const std::int32_t column : openColumns)
                {
                    weights.push_back(source[static_cast<std::size_t>(column)]);
                }
                continue;
            }
            const std::size_t moverSlot = static_cast<std::size_t>(mover - moverRows.begin());
            const std::vector<MilliFunds> acting = actingWeights(row, movers[moverSlot]);
            for (const std::int32_t column : openColumns)
            {
                weights.push_back(acting[static_cast<std::size_t>(column)]);
            }
        }
        AssignmentSolver solver;
        solver.solve(m_ours.rowCount(), static_cast<std::int32_t>(openColumns.size()), weights);
        return owned + solver.optimum() - jointEnemyOptimum(captured);
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

    MilliFunds JointPlanStockValuer::planStock(const TurnPlan & plan)
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
        return m_field.jointStock(actions);
    }

    MilliFunds JointPlanStockValuer::originStock() const
    {
        return m_field.originStock();
    }

    MilliFunds JointPlanStockValuer::stockCeiling() const
    {
        return std::numeric_limits<MilliFunds>::max();
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
