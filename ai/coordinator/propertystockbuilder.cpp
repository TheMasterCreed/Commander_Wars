#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
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
    constexpr std::size_t ROW_ENUM_KEY_PREFIX = 5;

    bool addWork(std::int64_t & total, std::int64_t amount)
    {
        if (amount < 0 ||
            total > std::numeric_limits<std::int64_t>::max() - amount)
        {
            return false;
        }
        total += amount;
        return true;
    }

    bool addProduct(
        std::int64_t & total,
        std::initializer_list<std::int64_t> factors)
    {
        std::int64_t product = 1;
        for (const std::int64_t factor : factors)
        {
            if (factor < 0 ||
                (factor != 0 &&
                 product >
                     std::numeric_limits<std::int64_t>::max() / factor))
            {
                return false;
            }
            product *= factor;
        }
        return addWork(total, product);
    }

    bool sameWitness(
        std::span<const Coordinator::RowWitness> lhs,
        std::span<const Coordinator::RowWitness> rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }
        for (std::size_t slot = 0; slot < lhs.size(); ++slot)
        {
            if (lhs[slot].row != rhs[slot].row ||
                lhs[slot].nodes != rhs[slot].nodes ||
                lhs[slot].value != rhs[slot].value)
            {
                return false;
            }
        }
        return true;
    }

    struct ValuerRowOptionSource final
        : Coordinator::SequentialDetail::SequentialRowOptionSource
    {
        std::map<std::vector<std::int64_t>,
                 Coordinator::SequentialDetail::CachedRowEnumeration>*
            pCache{nullptr};
        const std::vector<std::vector<std::int64_t>>* pKeys{nullptr};

        bool lookup(
            std::int32_t rowIndex,
            std::span<const Coordinator::SequentialDetail::RowOption> & options,
            std::int64_t & statesSpent) override
        {
            const auto found =
                pCache->find((*pKeys)[static_cast<std::size_t>(rowIndex)]);
            if (found == pCache->end())
            {
                return false;
            }
            options = std::span<const Coordinator::SequentialDetail::RowOption>(
                found->second.options);
            statesSpent = found->second.statesSpent;
            return true;
        }

        void store(
            std::int32_t rowIndex,
            std::span<const Coordinator::SequentialDetail::RowOption> options,
            std::int64_t statesSpent) override
        {
            Coordinator::SequentialDetail::CachedRowEnumeration entry;
            entry.options.assign(options.begin(), options.end());
            entry.statesSpent = statesSpent;
            pCache->emplace(
                (*pKeys)[static_cast<std::size_t>(rowIndex)],
                std::move(entry));
        }
    };

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

    JointStockDecomposition PropertyStockField::jointStockDecomposition(
        std::span<const PlanRowAction> actions)
    {
        JointActionFacts facts = jointActionFacts(actions);
        JointStockDecomposition result;
        result.ownedBaseline = m_ownedBaseline;
        for (const std::int32_t column : facts.capturedColumns)
        {
            result.ownershipFlipSwingTotal += ownershipFlipSwing(
                m_ours.columns[static_cast<std::size_t>(column)],
                m_horizonTurns);
        }
        result.ourOpenOptimum = ourOpenOptimum(facts);
        result.jointEnemyOptimum =
            jointEnemyOptimum(facts.capturedColumns);
        result.stockAbsolute =
            result.ownedBaseline +
            result.ownershipFlipSwingTotal +
            result.ourOpenOptimum -
            result.jointEnemyOptimum;
        result.capturedColumns =
            std::move(facts.capturedColumns);
        return result;
    }

    MilliFunds PropertyStockField::jointStock(
        std::span<const PlanRowAction> actions)
    {
        return jointStockDecomposition(actions).stockAbsolute;
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

    std::int64_t JointPlanStockValuer::mandatoryTariffFor(
        std::size_t actionCount) const
    {
        const std::int64_t rows =
            static_cast<std::int64_t>(m_field.m_ours.rows.size());
        const std::int64_t enemyRows =
            static_cast<std::int64_t>(m_field.m_theirs.rows.size());
        const std::int64_t columns =
            static_cast<std::int64_t>(m_field.m_ours.columns.size());
        const std::int64_t actions =
            static_cast<std::int64_t>(actionCount);
        const std::int64_t horizon =
            std::max<std::int64_t>(m_field.m_horizonTurns, 0);
        std::int64_t ourNodes = 0;
        for (const PropertyStockColumn & column :
             m_field.m_ours.columns)
        {
            if (column.ownerBefore != STOCK_OWNER_AFTER)
            {
                ++ourNodes;
            }
        }
        std::set<SequentialClassKey> ourClasses;
        for (const PropertyStockRow & row : m_field.m_ours.rows)
        {
            ourClasses.insert(SequentialClassKey{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            });
        }
        std::set<SequentialClassKey> enemyClasses;
        for (const PropertyStockRow & row : m_field.m_theirs.rows)
        {
            enemyClasses.insert(SequentialClassKey{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            });
        }
        const std::int64_t copiedCells =
            static_cast<std::int64_t>(
                m_field.m_ours.weights.size() +
                m_field.m_theirs.weights.size() +
                m_field.m_enemyArrivals.size());
        std::int64_t tariff = 0;
        if (!addWork(tariff, 1) ||
            !addWork(tariff, copiedCells) ||
            !addProduct(tariff, {actions, actions}) ||
            !addProduct(
                tariff,
                {static_cast<std::int64_t>(ourClasses.size()),
                 ourNodes,
                 ourNodes,
                 horizon}) ||
            !addProduct(
                tariff,
                {rows + actions,
                 ourNodes,
                 std::max<std::int64_t>(ourNodes, 1)}) ||
            !addProduct(
                tariff,
                {static_cast<std::int64_t>(enemyClasses.size()),
                 columns,
                 columns,
                 horizon}) ||
            !addProduct(tariff, {enemyRows, columns, 2}))
        {
            return -1;
        }
        return tariff;
    }

    ContinuationPricingCatalog
    JointPlanStockValuer::continuationPricingCatalog(
        const AssignmentInput & input) const
    {
        ContinuationPricingCatalog catalog;
        for (const AssignmentActor & actor : input.actors)
        {
            const std::int32_t row = rowOf(actor.engineUnitId);
            if (row == NO_STOCK_ROW)
            {
                continue;
            }
            for (const CandidateBundle & candidate : actor.candidates)
            {
                if (!isPlannableCandidate(input.actionIds,
                                          input.unitLinks,
                                          candidate))
                {
                    continue;
                }
                const PlanBundleKind kind =
                    planBundleKindOf(candidate.bundle);
                catalog.actions.push_back(PlanRowAction{
                    .row = row,
                    .destination = candidate.bundle.destination,
                    .captures =
                        kind == PlanBundleKind::Capture ||
                        kind == PlanBundleKind::MoveAndCapture,
                });
            }
        }
        const auto less = [](const PlanRowAction & lhs,
                             const PlanRowAction & rhs)
        {
            return canonicalPlanActionKey(lhs) <
                   canonicalPlanActionKey(rhs);
        };
        const auto equal = [](const PlanRowAction & lhs,
                              const PlanRowAction & rhs)
        {
            return canonicalPlanActionKey(lhs) ==
                   canonicalPlanActionKey(rhs);
        };
        std::sort(catalog.actions.begin(),
                  catalog.actions.end(),
                  less);
        catalog.actions.erase(
            std::unique(catalog.actions.begin(),
                        catalog.actions.end(),
                        equal),
            catalog.actions.end());
        catalog.mandatoryTariff =
            mandatoryTariffFor(catalog.actions.size());
        catalog.refinementBudget = LIVE_PAIR_REFINEMENT_BUDGET;
        catalog.ledgerTotal = catalog.mandatoryTariff;
        catalog.valid =
            catalog.mandatoryTariff >= 0 &&
            addWork(catalog.ledgerTotal,
                    catalog.refinementBudget);
        return catalog;
    }

    bool JointPlanStockValuer::prepareContinuationPricing(
        ContinuationPricingCatalog catalog,
        RefinementLedger & ledger)
    {
        const ContinuationKey catalogKey =
            canonicalPlanKey(catalog.actions);
        if (m_pricingState == ContinuationPricingState::Prepared)
        {
            return catalog.valid &&
                   catalogKey == m_preparedCatalogKey &&
                   m_pRefinementLedger == &ledger;
        }
        if (m_pricingState == ContinuationPricingState::Failed)
        {
            return false;
        }
        const std::int64_t expectedMandatory =
            mandatoryTariffFor(catalog.actions.size());
        std::int64_t expectedTotal = expectedMandatory;
        const bool expectedValid =
            expectedMandatory >= 0 &&
            addWork(expectedTotal, LIVE_PAIR_REFINEMENT_BUDGET);
        const auto less = [](const PlanRowAction & lhs,
                             const PlanRowAction & rhs)
        {
            return canonicalPlanActionKey(lhs) <
                   canonicalPlanActionKey(rhs);
        };
        const auto equal = [](const PlanRowAction & lhs,
                              const PlanRowAction & rhs)
        {
            return canonicalPlanActionKey(lhs) ==
                   canonicalPlanActionKey(rhs);
        };
        const bool invalidRow = std::any_of(
            catalog.actions.begin(),
            catalog.actions.end(),
            [this](const PlanRowAction & action)
        {
            return action.row < 0 ||
                   action.row >= m_field.m_ours.rowCount();
        });
        const RefinementLedgerStats & stats = ledger.stats();
        if (!catalog.valid || !expectedValid ||
            catalog.mandatoryTariff != expectedMandatory ||
            catalog.refinementBudget !=
                LIVE_PAIR_REFINEMENT_BUDGET ||
            catalog.ledgerTotal != expectedTotal ||
            stats.total != expectedTotal ||
            stats.granted != 0 ||
            stats.refunded != 0 ||
            ledger.balance() != expectedTotal ||
            invalidRow ||
            !std::is_sorted(catalog.actions.begin(),
                            catalog.actions.end(),
                            less) ||
            std::adjacent_find(catalog.actions.begin(),
                               catalog.actions.end(),
                               equal) != catalog.actions.end())
        {
            m_pricingState = ContinuationPricingState::Failed;
            return false;
        }
        if (ledger.draw(expectedMandatory,
                        RefinementWork::MandatoryPrecompute) ==
            NO_GRANT)
        {
            m_pricingState = ContinuationPricingState::Failed;
            return false;
        }
        if (!buildCheapModel(catalog.actions))
        {
            m_pricingState = ContinuationPricingState::Failed;
            return false;
        }
        m_preparedCatalogKey = std::move(catalogKey);
        m_pricingState = ContinuationPricingState::Prepared;
        m_pRefinementLedger = &ledger;
        m_contenderRegistry.open(liveDemandCapacity());
        m_pricingFailure = false;
        m_refinementClosed = false;
        return true;
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
            });
        }
        return actions;
    }

    std::int64_t JointPlanStockValuer::ourRerunTariff() const
    {
        const std::int64_t rows =
            static_cast<std::int64_t>(m_field.m_ours.rows.size());
        const std::int64_t horizon =
            std::max<std::int64_t>(m_field.m_horizonTurns, 0);
        std::int64_t nodes = 0;
        for (const PropertyStockColumn & column :
             m_field.m_ours.columns)
        {
            if (column.ownerBefore != STOCK_OWNER_AFTER)
            {
                ++nodes;
            }
        }
        std::set<SequentialClassKey> classes;
        for (const PropertyStockRow & row : m_field.m_ours.rows)
        {
            classes.insert(SequentialClassKey{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            });
        }
        std::int64_t tariff = 1;
        if (!addProduct(
                tariff,
                {static_cast<std::int64_t>(classes.size()),
                 nodes,
                 nodes,
                 horizon}) ||
            !addProduct(
                tariff,
                {rows,
                 std::max<std::int64_t>(nodes, 1),
                 std::max<std::int64_t>({rows, nodes, 1})}) ||
            !addProduct(
                tariff,
                {rows,
                 std::max<std::int64_t>(nodes, 1),
                 2}))
        {
            return -1;
        }
        return tariff;
    }

    std::int64_t JointPlanStockValuer::enemyRerunTariff(
        const std::vector<std::int32_t> & capturedColumns) const
    {
        const std::int64_t rows =
            static_cast<std::int64_t>(m_field.m_theirs.rows.size());
        const std::int64_t columns =
            static_cast<std::int64_t>(
                m_field.m_theirs.columns.size());
        const std::int64_t horizon =
            std::max<std::int64_t>(m_field.m_horizonTurns, 0);
        std::set<SequentialClassKey> classes;
        for (const PropertyStockRow & row : m_field.m_theirs.rows)
        {
            classes.insert(SequentialClassKey{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            });
        }
        std::int64_t tariff = 1;
        if (!addProduct(
                tariff,
                {columns,
                 static_cast<std::int64_t>(
                     capturedColumns.size())}) ||
            !addProduct(
                tariff,
                {static_cast<std::int64_t>(classes.size()),
                 columns,
                 columns,
                 horizon}) ||
            !addProduct(
                tariff,
                {rows,
                 std::max<std::int64_t>(columns, 1),
                 std::max<std::int64_t>({rows, columns, 1})}) ||
            !addProduct(
                tariff,
                {rows,
                 std::max<std::int64_t>(columns, 1),
                 2}))
        {
            return -1;
        }
        return tariff;
    }

    std::int32_t JointPlanStockValuer::liveDemandCapacity() const
    {
        const std::int64_t ourTariff = ourRerunTariff();
        const std::int64_t enemyTariff = enemyRerunTariff({});
        std::int64_t minimumAdmission = 0;
        if (ourTariff < 0 ||
            enemyTariff < 0 ||
            !addWork(minimumAdmission, ourTariff) ||
            !addWork(minimumAdmission, enemyTariff) ||
            !addWork(minimumAdmission, LIVE_PAIR_SLICE_BASE) ||
            !addWork(minimumAdmission, LIVE_PAIR_SLICE_BASE) ||
            minimumAdmission <= 0)
        {
            return 0;
        }
        return static_cast<std::int32_t>(
            std::min<std::int64_t>(
                LIVE_PAIR_REFINEMENT_BUDGET / minimumAdmission,
                std::numeric_limits<std::int32_t>::max()));
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

    CheapRowTerms JointPlanStockValuer::cheapRowTerms(
        const SequentialInstance & instance,
        const SequentialRow & row,
        std::int32_t rowIndex,
        std::span<const std::int32_t> nodeSlots) const
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

    MilliFunds JointPlanStockValuer::enemyUnionCeiling(
        PropertyStockField & field,
        std::span<const std::int32_t> admissibleCapturedColumns) const
    {
        const std::int32_t columnCount = field.m_theirs.columnCount();
        if (columnCount == 0 || field.m_theirs.rowCount() == 0)
        {
            return 0;
        }
        std::vector<bool> canFlip(
            static_cast<std::size_t>(columnCount), false);
        for (const std::int32_t column : admissibleCapturedColumns)
        {
            if (column < 0 || column >= columnCount)
            {
                return -1;
            }
            canFlip[static_cast<std::size_t>(column)] = true;
        }

        std::vector<std::int32_t> nodeSlots;
        std::vector<std::int32_t> nodeOfColumn(
            static_cast<std::size_t>(columnCount),
            NO_SEQUENTIAL_NODE);
        for (std::int32_t column = 0; column < columnCount; ++column)
        {
            const std::size_t slot = static_cast<std::size_t>(column);
            if (field.m_theirs.columns[slot].ownerBefore ==
                    STOCK_OWNER_AFTER &&
                !canFlip[slot])
            {
                continue;
            }
            nodeOfColumn[slot] =
                static_cast<std::int32_t>(nodeSlots.size());
            nodeSlots.push_back(column);
        }
        const std::int32_t nodeCount =
            static_cast<std::int32_t>(nodeSlots.size());
        if (nodeCount == 0)
        {
            return 0;
        }

        const std::int32_t horizon = field.m_horizonTurns;
        std::vector<MilliFunds> unionPrizes(
            static_cast<std::size_t>(nodeCount) *
                static_cast<std::size_t>(std::max(horizon, 0)),
            0);
        for (std::int32_t node = 0; node < nodeCount; ++node)
        {
            const std::int32_t column =
                nodeSlots[static_cast<std::size_t>(node)];
            const std::size_t columnSlot =
                static_cast<std::size_t>(column);
            const PropertyStockColumn & base =
                field.m_theirs.columns[columnSlot];
            PropertyStockColumn flipped = base;
            flipped.ownerBefore = mirroredSign(STOCK_OWNER_AFTER);
            for (std::int32_t turn = 0; turn < horizon; ++turn)
            {
                bool hasPrize = false;
                MilliFunds prize = 0;
                if (base.ownerBefore != STOCK_OWNER_AFTER)
                {
                    prize = columnStreamValue(base, turn, horizon);
                    hasPrize = true;
                }
                if (canFlip[columnSlot])
                {
                    const MilliFunds candidate =
                        columnStreamValue(flipped, turn, horizon);
                    if (!hasPrize || candidate > prize)
                    {
                        prize = candidate;
                    }
                    hasPrize = true;
                }
                if (hasPrize)
                {
                    unionPrizes[
                        static_cast<std::size_t>(node) *
                            static_cast<std::size_t>(horizon) +
                        static_cast<std::size_t>(turn)] = prize;
                }
            }
        }

        std::vector<SequentialClassKey> classKeys;
        std::vector<SequentialClassTable> classTables;
        std::vector<std::int32_t> classOfRow(
            field.m_theirs.rows.size(), NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0;
             rowIndex < field.m_theirs.rows.size();
             ++rowIndex)
        {
            const PropertyStockRow & row =
                field.m_theirs.rows[rowIndex];
            const SequentialClassKey key{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            };
            const auto known =
                std::find(classKeys.begin(), classKeys.end(), key);
            if (known != classKeys.end())
            {
                classOfRow[rowIndex] =
                    static_cast<std::int32_t>(known -
                                              classKeys.begin());
                continue;
            }
            const std::vector<std::int32_t> arrivals =
                nodeArrivalsFor(field,
                                row.gridIdentity,
                                row.movementPoints,
                                nodeSlots);
            SequentialClassTable table;
            table.buildFromPrizes(
                nodeCount,
                unionPrizes,
                arrivals,
                captureTurnsFor(0,
                                row.captureRate,
                                field.m_capturePointsToCapture),
                horizon);
            classKeys.push_back(key);
            classTables.push_back(std::move(table));
            classOfRow[rowIndex] =
                static_cast<std::int32_t>(classKeys.size()) - 1;
        }

        MilliFunds ceiling = 0;
        for (std::size_t rowIndex = 0;
             rowIndex < field.m_theirs.rows.size();
             ++rowIndex)
        {
            const PropertyStockRow & row =
                field.m_theirs.rows[rowIndex];
            const PropertyStockField::ArrivalVectors & arrivals =
                field.arrivalsFrom(row.gridIdentity,
                                   row.movementPoints,
                                   row.tile);
            const SequentialClassTable & table =
                classTables[
                    static_cast<std::size_t>(classOfRow[rowIndex])];
            const std::span<const MilliFunds> baseWeights =
                field.m_theirs.weightRow(
                    static_cast<std::int32_t>(rowIndex));
            MilliFunds rowCeiling = 0;
            for (std::int32_t column = 0;
                 column < columnCount;
                 ++column)
            {
                const std::size_t columnSlot =
                    static_cast<std::size_t>(column);
                const std::int32_t node =
                    nodeOfColumn[columnSlot];
                if (node == NO_SEQUENTIAL_NODE)
                {
                    continue;
                }
                const std::int32_t owned =
                    sequentialFirstOwnedTurn(
                        ownedTurnsUntil(
                            arrivals.activations[columnSlot],
                            field.carriedFor(row, column),
                            row.captureRate,
                            field.m_capturePointsToCapture,
                            horizon),
                        horizon);
                const MilliFunds continuation =
                    owned == NO_CAPTURE_TURNS
                        ? 0
                        : std::max<MilliFunds>(
                              table.valueAt(node, owned), 0);
                if (field.m_theirs.columns[columnSlot].ownerBefore !=
                    STOCK_OWNER_AFTER)
                {
                    rowCeiling = std::max(
                        rowCeiling,
                        baseWeights[columnSlot] + continuation);
                }
                if (canFlip[columnSlot])
                {
                    PropertyStockColumn flipped =
                        field.m_theirs.columns[columnSlot];
                    flipped.ownerBefore =
                        mirroredSign(STOCK_OWNER_AFTER);
                    const ArrivalFacts arrival{
                        .arrivalActivations =
                            arrivals.activations[columnSlot],
                        .carriedPoints =
                            field.carriedFor(row, column),
                        .ratePerTurn = row.captureRate,
                    };
                    rowCeiling = std::max(
                        rowCeiling,
                        columnWeight(
                            flipped,
                            arrival,
                            field.m_capturePointsToCapture,
                            horizon) +
                            continuation);
                }
            }
            ceiling += std::max<MilliFunds>(rowCeiling, 0);
        }
        return ceiling;
    }

    bool JointPlanStockValuer::buildCheapModel(
        std::span<const PlanRowAction> catalogActions)
    {
        std::unique_ptr<PropertyStockField> pricingField =
            std::make_unique<PropertyStockField>(m_field);
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
        for (std::size_t rowIndex = 0;
             rowIndex < pricingField->m_ours.rows.size();
             ++rowIndex)
        {
            const PropertyStockRow & row =
                pricingField->m_ours.rows[rowIndex];
            const SequentialClassKey key{
                row.gridIdentity,
                row.movementPoints,
                row.captureRate,
            };
            const auto known =
                std::find(classKeys.begin(), classKeys.end(), key);
            if (known != classKeys.end())
            {
                classOfRow[rowIndex] =
                    static_cast<std::int32_t>(known -
                                              classKeys.begin());
                continue;
            }
            const std::vector<std::int32_t> arrivals =
                nodeArrivalsFor(*pricingField,
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
                    pricingField->m_capturePointsToCapture),
                pricingField->m_horizonTurns);
            classKeys.push_back(key);
            classTables.push_back(std::move(table));
            classOfRow[rowIndex] =
                static_cast<std::int32_t>(classKeys.size()) - 1;
        }

        SequentialInstance templateInstance;
        templateInstance.horizonTurns =
            pricingField->m_horizonTurns;
        templateInstance.nodeColumns = nodeColumns;
        templateInstance.classes = classTables;
        templateInstance.capturedNodes.assign(
            nodeSlots.size(), false);
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
            CheapRowTerms terms = cheapRowTerms(
                templateInstance,
                row,
                static_cast<std::int32_t>(rowIndex),
                nodeSlots);
            if (terms.row != static_cast<std::int32_t>(rowIndex))
            {
                return false;
            }
            dayRows.push_back(std::move(terms));
        }

        std::vector<CheapActionTerms> actions;
        std::vector<std::int32_t> admissibleCapturedColumns;
        actions.reserve(catalogActions.size());
        for (const PlanRowAction & action : catalogActions)
        {
            if (action.row < 0 ||
                action.row >= pricingField->m_ours.rowCount())
            {
                return false;
            }
            const PropertyStockRow & source =
                pricingField->m_ours.rows[
                    static_cast<std::size_t>(action.row)];
            std::int32_t capturedColumn = NO_STOCK_COLUMN;
            std::int32_t carried = 0;
            const std::int32_t column =
                pricingField->columnSlotAt(action.destination);
            if (column != NO_STOCK_COLUMN)
            {
                carried = pricingField->carriedFor(source, column);
                if (action.captures)
                {
                    carried += source.captureRate;
                    if (carried >=
                        pricingField->m_capturePointsToCapture)
                    {
                        capturedColumn = column;
                        admissibleCapturedColumns.push_back(column);
                    }
                }
            }
            const PropertyStockActor mover{
                .knowledgeIndex = source.knowledgeIndex,
                .tile = action.destination,
                .movementPoints = source.movementPoints,
                .carriedCapturePoints = carried,
                .survival = ActorSurvival::Alive,
            };
            const SequentialRow row = sequentialRowFor(
                *pricingField,
                pricingField->m_ours,
                source,
                action.row,
                &mover,
                nodeSlots,
                classOfRow[static_cast<std::size_t>(action.row)]);
            CheapRowTerms terms =
                cheapRowTerms(templateInstance,
                              row,
                              action.row,
                              nodeSlots);
            if (terms.row != action.row)
            {
                return false;
            }
            actions.push_back(CheapActionTerms{
                .key = canonicalPlanActionKey(action),
                .row = action.row,
                .capturedColumn = capturedColumn,
                .rowTerms = std::move(terms),
            });
        }
        std::sort(admissibleCapturedColumns.begin(),
                  admissibleCapturedColumns.end());
        admissibleCapturedColumns.erase(
            std::unique(admissibleCapturedColumns.begin(),
                        admissibleCapturedColumns.end()),
            admissibleCapturedColumns.end());

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
        const MilliFunds enemyCeiling =
            enemyUnionCeiling(
                *pricingField,
                admissibleCapturedColumns);
        if (enemyCeiling < 0)
        {
            return false;
        }
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
            return false;
        }
        m_pricingField = std::move(pricingField);
        m_cheapModel = std::move(model);
        m_cheapState = m_cheapModel.initialState();
        return m_cheapState.pModel == &m_cheapModel;
    }

    bool JointPlanStockValuer::priceCheap(
        const ContinuationKey & key,
        CheapInitialPrice & price) const
    {
        if (!m_cheapModel.price(key, m_cheapState, price))
        {
            return false;
        }
        ContinuationEntry* pEntry =
            m_continuationStore.find(price.key);
        if (pEntry == nullptr)
        {
            ContinuationEntry entry;
            entry.key = price.key;
            entry.capturedColumns = price.capturedColumns;
            entry.ours = price.ours;
            entry.witness = price.witness;
            entry.exact = price.ours.exact();
            entry.storeBacked = false;
            m_continuationStore.admit(std::move(entry));
        }
        else if (
            pEntry->capturedColumns != price.capturedColumns ||
            pEntry->initialOurs.lower != price.ours.lower ||
            pEntry->initialOurs.upper != price.ours.upper ||
            !sameWitness(pEntry->initialWitness, price.witness))
        {
            m_pricingFailure = true;
            return false;
        }

        EnemySetBounds* pEnemy =
            m_continuationStore.enemyFor(price.capturedColumns);
        if (pEnemy == nullptr)
        {
            EnemySetBounds enemy;
            enemy.bounds = price.enemy;
            enemy.exact = price.enemy.exact();
            enemy.storeBacked = false;
            m_continuationStore.admitEnemy(
                price.capturedColumns,
                enemy);
        }
        else if (
            pEnemy->initialBounds.lower != price.enemy.lower ||
            pEnemy->initialBounds.upper != price.enemy.upper)
        {
            m_pricingFailure = true;
            return false;
        }
        return true;
    }

    LivePlanStockQuote JointPlanStockValuer::liveQuote(
        const CheapInitialPrice & initial,
        MilliFunds economicValue) const
    {
        LivePlanStockQuote quote;
        quote.key = initial.key;
        const ContinuationEntry* pEntry =
            m_continuationStore.find(initial.key);
        const EnemySetBounds* pEnemy =
            m_continuationStore.enemyFor(
                initial.capturedColumns);
        if (pEntry == nullptr || pEnemy == nullptr ||
            pEntry->ours.lower > pEntry->ours.upper ||
            pEnemy->bounds.lower > pEnemy->bounds.upper)
        {
            return quote;
        }
        if (pEntry->ours.lower == pEntry->initialOurs.lower)
        {
            quote.lowerWitnessReplays =
                m_cheapModel.replayWitness(
                    m_cheapState,
                    pEntry->initialWitness,
                    pEntry->ours.lower);
        }
        else
        {
            const auto witness =
                m_refinedOurWitnesses.find(initial.key);
            quote.lowerWitnessReplays =
                witness != m_refinedOurWitnesses.end() &&
                witness->second.value == pEntry->ours.lower &&
                witness->second.replays;
        }
        quote.stockAbsolute =
            composeStockInterval(
                initial.owned,
                pEntry->ours,
                pEnemy->bounds);
        quote.valid =
            quote.lowerWitnessReplays &&
            quote.stockAbsolute.lower <=
                quote.stockAbsolute.upper;
        if (quote.valid)
        {
            m_contenderRegistry.encounter(
                initial.key,
                economicValue + quote.stockAbsolute.lower,
                economicValue + quote.stockAbsolute.upper);
            if (pEntry->exact && pEnemy->exact)
            {
                m_contenderRegistry.markResolved(initial.key);
            }
        }
        return quote;
    }

    bool JointPlanStockValuer::livePairSwapIntervals() const
    {
        return
            m_pricingState ==
                ContinuationPricingState::Prepared &&
            m_pRefinementLedger != nullptr &&
            !m_pricingFailure;
    }

    LivePlanStockQuote JointPlanStockValuer::livePlanStock(
        const TurnPlan & plan,
        MilliFunds economicValue,
        bool)
    {
        LivePlanStockQuote quote;
        if (!livePairSwapIntervals())
        {
            return quote;
        }
        CheapInitialPrice initial;
        if (!priceCheap(
                canonicalPlanKey(planActions(plan)),
                initial))
        {
            m_pricingFailure = true;
            return quote;
        }
        quote = liveQuote(initial, economicValue);
        if (!quote.valid)
        {
            m_pricingFailure = true;
        }
        return quote;
    }

    MilliFunds JointPlanStockValuer::planStockFloor(const TurnPlan & plan)
    {
        return m_field.jointStock(planActions(plan));
    }

    SequentialStockAudit JointPlanStockValuer::sequentialAudit(
        const SequentialTierResult & tier,
        bool witnessReplays)
    {
        return SequentialStockAudit{
            .floorValue = tier.floorValue,
            .relaxedValue = tier.relaxedValue,
            .repairValue = tier.repairValue,
            .searchValue = tier.searchValue,
            .bookedValue = tier.bookedValue,
            .searchStates = tier.searchStates,
            .certificate = tier.certificate,
            .searchCompleted = tier.searchCompleted,
            .witnessReplays = witnessReplays,
            .provenExact =
                tier.certificate ||
                tier.searchCompleted ||
                tier.bookedValue == tier.relaxedValue,
        };
    }

    std::vector<PlanStockActionAudit>
    JointPlanStockValuer::actionAudits(
        std::span<const PlanRowAction> actions) const
    {
        std::vector<PlanStockActionAudit> result;
        result.reserve(actions.size());
        for (const PlanRowAction & action : actions)
        {
            if (action.row < 0 ||
                action.row >= m_field.m_ours.rowCount())
            {
                continue;
            }
            const PropertyStockRow & row =
                m_field.m_ours.rows[
                    static_cast<std::size_t>(action.row)];
            PlanStockActionAudit audit{
                .knowledgeIndex = row.knowledgeIndex,
                .destination = action.destination,
                .captures = action.captures,
                .propertyIndex =
                    m_field.propertyIndexAt(action.destination),
                .stockColumn =
                    m_field.columnSlotAt(action.destination),
                .captureRate = row.captureRate,
            };
            if (audit.stockColumn != NO_STOCK_COLUMN)
            {
                const std::size_t column =
                    static_cast<std::size_t>(audit.stockColumn);
                audit.currentOwnerSign = static_cast<std::int32_t>(
                    m_field.m_ours.columns[column].ownerBefore);
                audit.currentCapturePoints =
                    m_field.m_columnCapturePoints[column];
                audit.currentCapturerKnowledge =
                    m_field.m_columnCapturer[column];
                audit.carriedCapturePoints =
                    m_field.carriedFor(row, audit.stockColumn);
                if (action.captures)
                {
                    audit.carriedCapturePoints += row.captureRate;
                }
                audit.turnsUntilOwned = captureTurnsFor(
                    audit.carriedCapturePoints,
                    row.captureRate,
                    m_field.m_capturePointsToCapture);
                audit.capturedColumn =
                    action.captures &&
                    audit.carriedCapturePoints >=
                        m_field.m_capturePointsToCapture;
            }
            result.push_back(audit);
        }
        return result;
    }

    PlanStockAudit JointPlanStockValuer::stockAudit(
        std::span<const PlanRowAction> actions)
    {
        const JointStockDecomposition floor =
            m_field.jointStockDecomposition(actions);
        const OurSequentialSolve ours =
            solveOurSequential(actions,
                               SEQUENTIAL_SEARCH_STATE_CAP,
                               true);
        const EnemySequentialSolve enemy =
            solveEnemySequential(ours.capturedColumns,
                                 SEQUENTIAL_SEARCH_STATE_CAP,
                                 true);
        PlanStockAudit result{
            .capturedColumns = floor.capturedColumns,
            .ownedBaseline = floor.ownedBaseline,
            .ownershipFlipSwingTotal =
                floor.ownershipFlipSwingTotal,
            .ourOpenOptimum = floor.ourOpenOptimum,
            .jointEnemyOptimum = floor.jointEnemyOptimum,
            .floorStockAbsolute = floor.stockAbsolute,
            .ours = sequentialAudit(
                ours.tier,
                ours.witnessReplays),
            .enemy = sequentialAudit(
                enemy.tier,
                enemy.witnessReplays),
            .scalarStockAbsolute =
                ours.owned +
                ours.tier.bookedValue -
                enemy.tier.bookedValue,
            .actions = actionAudits(actions),
        };
        result.available =
            ours.capturedColumns == floor.capturedColumns &&
            ours.owned ==
                floor.ownedBaseline +
                    floor.ownershipFlipSwingTotal &&
            ours.tier.floorValue ==
                floor.ourOpenOptimum &&
            enemy.tier.floorValue ==
                floor.jointEnemyOptimum;
        result.scalarExact =
            result.available &&
            result.ours.provenExact &&
            result.enemy.provenExact;
        result.scalarWitnessReplays =
            result.available &&
            result.ours.witnessReplays &&
            result.enemy.witnessReplays;
        return result;
    }

    PlanStockAudit JointPlanStockValuer::auditPlanStock(
        const TurnPlan & plan)
    {
        const MilliFunds origin = originStock();
        if (!m_auditOrigin.has_value())
        {
            m_auditOrigin =
                stockAudit(std::span<const PlanRowAction>());
        }
        const std::vector<PlanRowAction> actions =
            planActions(plan);
        PlanStockAudit result =
            actions.empty()
                ? *m_auditOrigin
                : stockAudit(actions);
        const bool originMatches =
            m_auditOrigin->available &&
            m_auditOrigin->scalarStockAbsolute == origin;
        result.liveOriginStock = origin;
        result.originScalarStock =
            m_auditOrigin->scalarStockAbsolute;
        result.originExact =
            originMatches &&
            m_auditOrigin->scalarExact;
        result.originWitnessReplays =
            originMatches &&
            m_auditOrigin->scalarWitnessReplays;
        return result;
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
        PropertyStockField & field,
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
            nodeArrivalsFor(m_field,
                            row.gridIdentity,
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
        PropertyStockField & field,
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
            const std::int32_t carried =
                field.m_columnTiles[slot] == pMover->tile
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

    JointPlanStockValuer::EnemySequentialSolve
    JointPlanStockValuer::solveEnemySequential(
        const std::vector<std::int32_t> & capturedColumns,
        std::int64_t stateCap,
        bool captureWitness) const
    {
        EnemySequentialSolve solved;
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
                nodeArrivalsFor(m_field,
                                row.gridIdentity,
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
                m_field,
                flipped,
                flipped.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                nullptr,
                nodeSlots,
                classIndices[rowIndex]));
        }
        solved.tier = solveSequentialPacking(
            instance,
            enemyFloor,
            stateCap,
            nullptr,
            captureWitness);
        if (captureWitness)
        {
            solved.witnessReplays = replaySequentialWitness(
                instance,
                solved.tier.witness,
                solved.tier.bookedValue);
        }
        return solved;
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
        const EnemySequentialSolve solved =
            solveEnemySequential(capturedColumns,
                                 SEQUENTIAL_SEARCH_STATE_CAP,
                                 false);
        m_enemySequentialOptima.emplace(
            capturedColumns,
            solved.tier.bookedValue);
        return solved.tier.bookedValue;
    }

    JointPlanStockValuer::OurSequentialSolve
    JointPlanStockValuer::solveOurSequential(
        std::span<const PlanRowAction> actions,
        std::int64_t stateCap,
        bool captureWitness) const
    {
        OurSequentialSolve solved;
        const PropertyStockField::JointActionFacts facts =
            m_field.jointActionFacts(actions);
        solved.capturedColumns = facts.capturedColumns;
        solved.owned = m_field.m_ownedBaseline;
        for (const std::int32_t column : facts.capturedColumns)
        {
            solved.owned += ownershipFlipSwing(
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

        std::vector<std::int64_t> sortedCaptured(
            facts.capturedColumns.begin(),
            facts.capturedColumns.end());
        std::sort(sortedCaptured.begin(), sortedCaptured.end());
        std::vector<std::vector<std::int64_t>> rowKeys;
        rowKeys.reserve(m_field.m_ours.rows.size());
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
            std::vector<std::int64_t> rowKey;
            rowKey.reserve(ROW_ENUM_KEY_PREFIX + sortedCaptured.size());
            rowKey.push_back(static_cast<std::int64_t>(rowIndex));
            rowKey.push_back(
                pMover != nullptr ? pMover->tile.x : NO_STOCK_COLUMN);
            rowKey.push_back(
                pMover != nullptr ? pMover->tile.y : NO_STOCK_COLUMN);
            rowKey.push_back(
                pMover != nullptr ? pMover->carriedCapturePoints : 0);
            rowKey.push_back(
                pMover != nullptr
                    ? static_cast<std::int64_t>(pMover->survival)
                    : 0);
            rowKey.insert(rowKey.end(),
                          sortedCaptured.begin(),
                          sortedCaptured.end());
            rowKeys.push_back(std::move(rowKey));
            instance.rows.push_back(sequentialRowFor(
                m_field,
                m_field.m_ours,
                m_field.m_ours.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                pMover,
                m_ourNodeSlots,
                classIndices[rowIndex]));
        }

        ValuerRowOptionSource rowSource;
        rowSource.pCache = &m_rowOptionCache;
        rowSource.pKeys = &rowKeys;
        solved.tier = solveSequentialPacking(
            instance,
            floorOur,
            stateCap,
            &rowSource,
            captureWitness);
        if (captureWitness)
        {
            solved.witnessReplays = replaySequentialWitness(
                instance,
                solved.tier.witness,
                solved.tier.bookedValue);
        }
        return solved;
    }

    MilliFunds JointPlanStockValuer::sequentialJointStock(
        std::span<const PlanRowAction> actions) const
    {
        const OurSequentialSolve ours =
            solveOurSequential(actions,
                               SEQUENTIAL_SEARCH_STATE_CAP,
                               false);
        const MilliFunds enemyBooked =
            enemySequentialOptimum(ours.capturedColumns);
        return ours.owned + ours.tier.bookedValue - enemyBooked;
    }

    bool JointPlanStockValuer::refineOur(
        ContinuationEntry & entry,
        GrantId grant,
        std::int64_t stateCap)
    {
        std::int64_t expectedCap = 0;
        if (m_pRefinementLedger == nullptr ||
            grant == NO_GRANT ||
            !livePairRefinementSliceCap(
                entry.sliceRung,
                expectedCap) ||
            expectedCap != stateCap)
        {
            m_pricingFailure = true;
            return false;
        }
        OurSequentialSolve solved =
            solveOurSequential(
                planActions(entry.key),
                stateCap,
                true);
        if (solved.capturedColumns != entry.capturedColumns ||
            solved.tier.searchStates < 0 ||
            solved.tier.searchStates > stateCap ||
            solved.tier.bookedValue >
                solved.tier.relaxedValue)
        {
            m_pricingFailure = true;
            return false;
        }
        const std::int64_t unused =
            stateCap - solved.tier.searchStates;
        if (unused > 0 &&
            !m_pRefinementLedger->refund(grant, unused))
        {
            m_pricingFailure = true;
            return false;
        }
        MilliFunds lower = entry.ours.lower;
        const bool adoptWitness =
            solved.tier.bookedValue > lower &&
            solved.witnessReplays;
        if (adoptWitness)
        {
            lower = solved.tier.bookedValue;
        }
        const MilliFunds solvedUpper =
            solved.tier.searchCompleted
                ? solved.tier.bookedValue
                : solved.tier.relaxedValue;
        const MilliFunds upper =
            std::min(entry.ours.upper, solvedUpper);
        if (upper < lower)
        {
            m_pricingFailure = true;
            return false;
        }
        std::int32_t nextRung = entry.sliceRung;
        if (!advanceLivePairRefinementRung(nextRung, true))
        {
            m_pricingFailure = true;
            return false;
        }
        entry.ours = StockInterval{lower, upper};
        if (adoptWitness)
        {
            m_refinedOurWitnesses[entry.key] =
                RefinedWitness{
                    std::move(solved.tier.witness),
                    lower,
                    true,
                };
        }
        entry.storeBacked = true;
        entry.sliceRung = nextRung;
        entry.exact = entry.ours.exact();
        return true;
    }

    bool JointPlanStockValuer::refineEnemy(
        EnemySetBounds & enemy,
        const std::vector<std::int32_t> & capturedColumns,
        GrantId grant,
        std::int64_t stateCap)
    {
        std::int64_t expectedCap = 0;
        if (m_pRefinementLedger == nullptr ||
            grant == NO_GRANT ||
            !livePairRefinementSliceCap(
                enemy.sliceRung,
                expectedCap) ||
            expectedCap != stateCap)
        {
            m_pricingFailure = true;
            return false;
        }
        EnemySequentialSolve solved =
            solveEnemySequential(
                capturedColumns,
                stateCap,
                true);
        if (solved.tier.searchStates < 0 ||
            solved.tier.searchStates > stateCap ||
            solved.tier.bookedValue >
                solved.tier.relaxedValue ||
            solved.tier.relaxedValue >
                m_cheapModel.enemyCeiling())
        {
            m_pricingFailure = true;
            return false;
        }
        const std::int64_t unused =
            stateCap - solved.tier.searchStates;
        if (unused > 0 &&
            !m_pRefinementLedger->refund(grant, unused))
        {
            m_pricingFailure = true;
            return false;
        }
        MilliFunds lower = enemy.bounds.lower;
        const bool adoptWitness =
            solved.tier.bookedValue > lower &&
            solved.witnessReplays;
        if (adoptWitness)
        {
            lower = solved.tier.bookedValue;
        }
        const MilliFunds solvedUpper =
            solved.tier.searchCompleted
                ? solved.tier.bookedValue
                : solved.tier.relaxedValue;
        const MilliFunds upper =
            std::min(enemy.bounds.upper, solvedUpper);
        if (upper < lower)
        {
            m_pricingFailure = true;
            return false;
        }
        std::int32_t nextRung = enemy.sliceRung;
        if (!advanceLivePairRefinementRung(nextRung, true))
        {
            m_pricingFailure = true;
            return false;
        }
        enemy.bounds = StockInterval{lower, upper};
        if (adoptWitness)
        {
            m_refinedEnemyWitnesses[capturedColumns] =
                RefinedWitness{
                    std::move(solved.tier.witness),
                    lower,
                    true,
                };
        }
        enemy.storeBacked = true;
        enemy.sliceRung = nextRung;
        enemy.exact = enemy.bounds.exact();
        return true;
    }

    bool JointPlanStockValuer::refineLiveKey(
        const ContinuationKey & key)
    {
        if (m_pRefinementLedger == nullptr ||
            m_refinementClosed ||
            m_pricingFailure)
        {
            return false;
        }
        ContinuationEntry* pEntry =
            m_continuationStore.find(key);
        if (pEntry == nullptr)
        {
            return false;
        }
        EnemySetBounds* pEnemy =
            m_continuationStore.enemyFor(
                pEntry->capturedColumns);
        if (pEnemy == nullptr)
        {
            return false;
        }

        const std::vector<std::int32_t> capturedColumns =
            pEntry->capturedColumns;
        const ContinuationEntry entryBefore = *pEntry;
        const EnemySetBounds enemyBefore = *pEnemy;
        std::optional<RefinedWitness> ourWitnessBefore;
        const auto ourWitness =
            m_refinedOurWitnesses.find(key);
        if (ourWitness != m_refinedOurWitnesses.end())
        {
            ourWitnessBefore = ourWitness->second;
        }
        std::optional<RefinedWitness> enemyWitnessBefore;
        const auto enemyWitness =
            m_refinedEnemyWitnesses.find(capturedColumns);
        if (enemyWitness != m_refinedEnemyWitnesses.end())
        {
            enemyWitnessBefore = enemyWitness->second;
        }
        const auto restore = [&]()
        {
            *pEntry = entryBefore;
            *pEnemy = enemyBefore;
            if (ourWitnessBefore.has_value())
            {
                m_refinedOurWitnesses[key] =
                    *ourWitnessBefore;
            }
            else
            {
                m_refinedOurWitnesses.erase(key);
            }
            if (enemyWitnessBefore.has_value())
            {
                m_refinedEnemyWitnesses[capturedColumns] =
                    *enemyWitnessBefore;
            }
            else
            {
                m_refinedEnemyWitnesses.erase(
                    capturedColumns);
            }
        };

        const bool admitOur =
            !pEntry->storeBacked && !pEntry->exact;
        const bool admitEnemy =
            !pEnemy->storeBacked && !pEnemy->exact;
        if (admitOur || admitEnemy)
        {
            std::int64_t ourCap = 0;
            std::int64_t enemyCap = 0;
            std::int64_t request = 0;
            if ((admitOur &&
                 (!livePairRefinementSliceCap(
                      pEntry->sliceRung,
                      ourCap) ||
                  !addWork(request, ourRerunTariff()) ||
                  !addWork(request, ourCap))) ||
                (admitEnemy &&
                 (!livePairRefinementSliceCap(
                      pEnemy->sliceRung,
                      enemyCap) ||
                  !addWork(
                      request,
                      enemyRerunTariff(capturedColumns)) ||
                  !addWork(request, enemyCap))))
            {
                m_pricingFailure = true;
                m_refinementClosed = true;
                return false;
            }
            const RefinementWork work =
                admitOur
                    ? RefinementWork::OurAdmission
                    : RefinementWork::EnemyEntry;
            const GrantId grant =
                m_pRefinementLedger->draw(request, work);
            if (grant == NO_GRANT)
            {
                m_refinementClosed = true;
                return false;
            }
            bool valid = true;
            if (admitOur)
            {
                valid =
                    refineOur(*pEntry, grant, ourCap) &&
                    valid;
            }
            if (admitEnemy)
            {
                valid =
                    refineEnemy(
                        *pEnemy,
                        capturedColumns,
                        grant,
                        enemyCap) &&
                    valid;
            }
            if (!valid)
            {
                restore();
                m_pricingFailure = true;
                m_refinementClosed = true;
                return false;
            }
            m_contenderRegistry.markAdmitted(key);
            if (pEntry->exact && pEnemy->exact)
            {
                m_contenderRegistry.markResolved(key);
            }
            return true;
        }

        const bool canRefineOur = !pEntry->exact;
        const bool canRefineEnemy = !pEnemy->exact;
        if (!canRefineOur && !canRefineEnemy)
        {
            m_contenderRegistry.markResolved(key);
            return false;
        }
        const bool chooseOur =
            canRefineOur &&
            (!canRefineEnemy ||
             pEntry->ours.width() >=
                 pEnemy->bounds.width());
        const std::int32_t rung =
            chooseOur
                ? pEntry->sliceRung
                : pEnemy->sliceRung;
        std::int64_t stateCap = 0;
        std::int64_t request = 0;
        const std::int64_t tariff =
            chooseOur
                ? ourRerunTariff()
                : enemyRerunTariff(capturedColumns);
        if (tariff < 0 ||
            !livePairRefinementSliceCap(rung, stateCap) ||
            !addWork(request, tariff) ||
            !addWork(request, stateCap))
        {
            m_pricingFailure = true;
            m_refinementClosed = true;
            return false;
        }
        const GrantId grant =
            m_pRefinementLedger->draw(
                request,
                RefinementWork::SearchSlice);
        if (grant == NO_GRANT)
        {
            m_refinementClosed = true;
            return false;
        }
        const bool valid =
            chooseOur
                ? refineOur(*pEntry, grant, stateCap)
                : refineEnemy(
                      *pEnemy,
                      capturedColumns,
                      grant,
                      stateCap);
        if (!valid)
        {
            restore();
            m_pricingFailure = true;
            m_refinementClosed = true;
            return false;
        }
        if (pEntry->exact && pEnemy->exact)
        {
            m_contenderRegistry.markResolved(key);
        }
        return true;
    }

    bool JointPlanStockValuer::refineLiveAtBoundary(
        AssignPhase phase)
    {
        if (!livePairSwapIntervals() ||
            phase != AssignPhase::BetweenSwapSweeps ||
            m_refinementClosed)
        {
            return false;
        }
        bool refined = false;
        while (!m_refinementClosed && !m_pricingFailure)
        {
            bool roundRefined = false;
            const std::vector<ContinuationKey> order =
                m_contenderRegistry.refinementOrder();
            for (const ContinuationKey & key : order)
            {
                if (refineLiveKey(key))
                {
                    refined = true;
                    roundRefined = true;
                }
                if (m_refinementClosed || m_pricingFailure)
                {
                    break;
                }
            }
            if (!roundRefined)
            {
                break;
            }
        }
        return refined;
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
