#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include <QString>

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/masterassignmentprofile.h"
#include "ai/coordinator/packingdualhint.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coreai.h"

#include "coreengine/gameconsole.h"

#include "game/gamemap.h"
#include "game/player.h"
#include "game/unit.h"

namespace
{
    constexpr std::size_t ROW_ENUM_KEY_PREFIX = 5;
    constexpr std::int64_t CERTIFICATE_OPTION_SCAN_WORK = 3;

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

    bool addCertificateOptionWork(const Coordinator::SequentialDetail::RowOption & option,
                                  std::int64_t & catalog, std::int64_t & records, std::int64_t & incidences)
    {
        if (!std::in_range<std::int64_t>(option.nodes.size()))
        {
            return false;
        }
        const auto nodes = static_cast<std::int64_t>(option.nodes.size());
        return addWork(catalog, std::max<std::int64_t>(1, nodes)) &&
            addWork(records, 1) && addWork(incidences, nodes);
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
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::set<std::vector<std::int64_t>>* pPaidKeys{nullptr};
#endif

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

#ifdef COW_COORDINATED_REFINEMENT_TESTING
        bool onceCharge(
            std::int32_t rowIndex,
            std::int64_t statesSpent,
            std::int64_t & chargedStates) const override
        {
            if (pPaidKeys == nullptr)
            {
                return false;
            }
            const auto & key =
                (*pKeys)[static_cast<std::size_t>(rowIndex)];
            chargedStates =
                pPaidKeys->contains(key) ? 0 : statesSpent;
            return true;
        }

        void recordCharge(std::int32_t rowIndex) override
        {
            if (pPaidKeys != nullptr)
            {
                pPaidKeys->insert(
                    (*pKeys)[static_cast<std::size_t>(rowIndex)]);
            }
        }
#endif
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

    MilliFunds PropertyStockField::enemyOptimumWithout(std::int32_t removedRow)
    {
        if (removedRow == NO_STOCK_ROW)
        {
            return m_enemyOptimum;
        }
        const auto known = m_enemyOptimaWithoutRow.find(removedRow);
        if (known != m_enemyOptimaWithoutRow.end())
        {
            return known->second;
        }
        const AssignmentSubInstance sub = subInstanceWithout(m_theirs, removedRow, NO_STOCK_COLUMN);
        AssignmentSolver solver;
        solver.solve(sub.rowCount, sub.columnCount, sub.weights);
        const MilliFunds optimum = solver.optimum();
        m_enemyOptimaWithoutRow.emplace(removedRow, optimum);
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
        return owned + ourPositionalStock(actor, outcome) -
            enemyOptimumWithout(enemyRowOf(outcome.destroyedKnowledgeIndex));
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
        result.jointEnemyOptimum = m_enemyOptimum;
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
                                               std::span<const KnownUnitLink> unitLinks,
                                               SequentialTierAuditMode auditMode)
        : m_field(field),
          m_tierAuditMode(auditMode)
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
        return prepareContinuationPricing(std::move(catalog), ledger, LIVE_PAIR_REFINEMENT_BUDGET);
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    bool JointPlanStockValuer::prepareDiagnosticContinuationPricing(
        ContinuationPricingCatalog catalog, RefinementLedger & ledger, std::int64_t diagnosticWorkLimit)
    {
        if (m_pricingState != ContinuationPricingState::Unprepared || diagnosticWorkLimit <= 0)
        {
            return false;
        }
        catalog.refinementBudget = diagnosticWorkLimit;
        catalog.ledgerTotal = catalog.mandatoryTariff;
        if (!catalog.valid || !addWork(catalog.ledgerTotal, diagnosticWorkLimit) || !ledger.open(catalog.ledgerTotal))
        {
            return false;
        }
        return prepareContinuationPricing(std::move(catalog), ledger, diagnosticWorkLimit);
    }
#endif

    bool JointPlanStockValuer::prepareMasterContinuationPricing(
        ContinuationPricingCatalog catalog, RefinementLedger & ledger, MasterAssignmentProfile* profile)
    {
        m_masterProfile = profile;
        catalog.refinementBudget = 0;
        catalog.ledgerTotal = catalog.mandatoryTariff;
        return prepareContinuationPricing(std::move(catalog), ledger, 0, PricingBudgetPolicy::Shared);
    }

    bool JointPlanStockValuer::retainOperationBaseTariffs(
        std::int64_t ours, std::int64_t enemy, std::int64_t certificateAdmissionWork)
    {
        if (ours < 0 || enemy < 0 || !m_masterFutureCatalog ||
            !m_refinedOurWitnesses.empty() || !m_refinedEnemyWitnesses.empty())
        {
            return false;
        }
        m_operationBaseTariffs = OperationBaseTariffs{ours, enemy, certificateAdmissionWork};
        return true;
    }

    bool JointPlanStockValuer::prepareOperationService()
    {
        if (!m_operationBaseTariffs || m_operationBaseTariffs->certificateAdmissionWork < 0 ||
            !m_masterFutureCatalog || !livePairSwapIntervals())
        {
            return false;
        }
        m_operationServiceEnabled = true;
        return true;
    }

    bool JointPlanStockValuer::prepareContinuationPricing(
        ContinuationPricingCatalog catalog, RefinementLedger & ledger, std::int64_t refinementBudget,
        PricingBudgetPolicy policy)
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
            addWork(expectedTotal, refinementBudget);
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
        const bool invalidLedger = policy == PricingBudgetPolicy::Dedicated &&
            (stats.total != expectedTotal || stats.granted != 0 ||
             stats.refunded != 0 || ledger.balance() != expectedTotal);
        if (!catalog.valid || !expectedValid ||
            catalog.mandatoryTariff != expectedMandatory ||
            catalog.refinementBudget != refinementBudget ||
            catalog.ledgerTotal != expectedTotal ||
            invalidLedger ||
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
        auto preparationWork = expectedMandatory;
        if (policy == PricingBudgetPolicy::Shared)
        {
            const auto rows = static_cast<std::int64_t>(m_field.m_ours.rows.size());
            const auto columns = static_cast<std::int64_t>(m_field.m_ours.columns.size());
            const auto actions = static_cast<std::int64_t>(catalog.actions.size());
            const auto horizon = std::max<std::int64_t>(m_field.m_horizonTurns, 0);
            if (!addProduct(preparationWork, {rows, columns, columns}) ||
                !addProduct(preparationWork, {rows, columns, horizon, 2}) ||
                !addProduct(preparationWork, {rows + actions, columns, 2}) ||
                !addWork(preparationWork, columns))
            {
                m_pricingState = ContinuationPricingState::Failed;
                return false;
            }
        }
        if (ledger.draw(preparationWork,
                        RefinementWork::MandatoryPrecompute) ==
            NO_GRANT)
        {
            m_pricingState = ContinuationPricingState::Failed;
            return false;
        }
        if (!buildCheapModel(catalog.actions, policy == PricingBudgetPolicy::Shared))
        {
            m_pricingState = ContinuationPricingState::Failed;
            return false;
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        m_rowOptionPaidKeys.clear();
#endif
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
        if (m_operationServiceEnabled)
        {
            return m_operationBaseTariffs->ours;
        }
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
        if (m_operationServiceEnabled)
        {
            auto tariff = m_operationBaseTariffs->enemy;
            if (!std::in_range<std::int64_t>(m_field.m_theirs.columns.size()) ||
                !std::in_range<std::int64_t>(capturedColumns.size()) ||
                !addProduct(tariff, {static_cast<std::int64_t>(m_field.m_theirs.columns.size()),
                                     static_cast<std::int64_t>(capturedColumns.size())}))
            {
                return -1;
            }
            return tariff;
        }
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

    std::int64_t JointPlanStockValuer::warmStartTariff(
        std::size_t witnesses, std::size_t keyWidth, std::size_t rows, std::size_t columns)
    {
        if (witnesses == 0)
        {
            return 0;
        }
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        if (keyWidth >= maximum || rows > maximum || columns > maximum)
        {
            return -1;
        }
        auto tariff = sequentialWarmStartWork(static_cast<std::int64_t>(rows), static_cast<std::int64_t>(columns));
        const std::int64_t lookupDepth = 2 * std::bit_width(witnesses) + 1;
        if (tariff < 0 || !addProduct(tariff, {static_cast<std::int64_t>(keyWidth) + 1, lookupDepth}))
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

    MilliFunds JointPlanStockValuer::enemyRowCeiling(PropertyStockField & field) const
    {
        const std::int32_t columnCount = field.m_theirs.columnCount();
        if (columnCount == 0 || field.m_theirs.rowCount() == 0)
        {
            return 0;
        }

        std::vector<std::int32_t> nodeSlots;
        std::vector<std::int32_t> nodeOfColumn(
            static_cast<std::size_t>(columnCount),
            NO_SEQUENTIAL_NODE);
        for (std::int32_t column = 0; column < columnCount; ++column)
        {
            const std::size_t slot = static_cast<std::size_t>(column);
            if (field.m_theirs.columns[slot].ownerBefore ==
                STOCK_OWNER_AFTER)
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
        std::vector<MilliFunds> prizes(
            static_cast<std::size_t>(nodeCount) *
                static_cast<std::size_t>(std::max(horizon, 0)),
            0);
        for (std::int32_t node = 0; node < nodeCount; ++node)
        {
            const PropertyStockColumn & base =
                field.m_theirs.columns[static_cast<std::size_t>(
                    nodeSlots[static_cast<std::size_t>(node)])];
            for (std::int32_t turn = 0; turn < horizon; ++turn)
            {
                prizes[
                    static_cast<std::size_t>(node) *
                        static_cast<std::size_t>(horizon) +
                    static_cast<std::size_t>(turn)] =
                    columnStreamValue(base, turn, horizon);
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
                prizes,
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
                rowCeiling = std::max(
                    rowCeiling,
                    baseWeights[columnSlot] + continuation);
            }
            ceiling += std::max<MilliFunds>(rowCeiling, 0);
        }
        return ceiling;
    }

    bool JointPlanStockValuer::buildCheapModel(
        std::span<const PlanRowAction> catalogActions, bool retainSequentialRows)
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
                return false;
            }
            dayRows.push_back(std::move(terms));
        }

        std::vector<CheapActionTerms> actions;
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
                return false;
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
        const MilliFunds enemyCeiling = enemyRowCeiling(*pricingField);
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
        if (future)
        {
            future->complete = true;
            m_masterFutureCatalog = std::move(future);
        }
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
            solveEnemySequential(SEQUENTIAL_SEARCH_STATE_CAP,
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
        m_ourStateSource = &continuationField();
        const std::vector<PropertyStockColumn> & columns =
            m_ourStateSource->m_ours.columns;
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
        PropertyStockField & field = continuationField();
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
            nodeArrivalsFor(field,
                            row.gridIdentity,
                            row.movementPoints,
                            m_ourNodeSlots);
        SequentialClassTable table;
        table.build(
            std::span<const PropertyStockColumn>(m_ourNodeColumns),
            std::span<const std::int32_t>(arrivals),
            captureTurnsFor(0,
                            row.captureRate,
                            field.m_capturePointsToCapture),
            field.m_horizonTurns);
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
        std::int64_t stateCap,
        bool captureWitness,
        const RefinedWitness* pReplay,
        const SequentialWarmStart* pWarmStart) const
    {
        MasterProfileScope setupTiming(m_masterProfile, MasterPhase::EnemySetup);
        PropertyStockField & field = continuationField();
        EnemySequentialSolve solved;
        // Start-of-turn ownership prices the enemy, so our captures this turn leave its instance unchanged.
        const PropertyStockInstance & theirs = field.m_theirs;
        const MilliFunds enemyFloor = field.m_enemyOptimum;

        std::vector<std::int32_t> nodeSlots;
        std::vector<PropertyStockColumn> nodeColumns;
        for (std::size_t slot = 0; slot < theirs.columns.size(); ++slot)
        {
            if (theirs.columns[slot].ownerBefore == STOCK_OWNER_AFTER)
            {
                continue;
            }
            nodeSlots.push_back(static_cast<std::int32_t>(slot));
            nodeColumns.push_back(theirs.columns[slot]);
        }

        std::vector<SequentialClassKey> classKeys;
        std::vector<SequentialClassTable> classTables;
        std::vector<std::int32_t> classIndices(
            theirs.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0;
             rowIndex < theirs.rows.size();
             ++rowIndex)
        {
            const PropertyStockRow & row = theirs.rows[rowIndex];
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
                nodeArrivalsFor(field,
                                row.gridIdentity,
                                row.movementPoints,
                                nodeSlots);
            SequentialClassTable table;
            table.build(
                std::span<const PropertyStockColumn>(nodeColumns),
                std::span<const std::int32_t>(arrivals),
                captureTurnsFor(0,
                                row.captureRate,
                                field.m_capturePointsToCapture),
                field.m_horizonTurns);
            classKeys.push_back(key);
            classTables.push_back(std::move(table));
            classIndices[rowIndex] =
                static_cast<std::int32_t>(classKeys.size()) - 1;
        }

        SequentialInstance instance;
        instance.horizonTurns = field.m_horizonTurns;
        instance.nodeColumns =
            std::span<const PropertyStockColumn>(nodeColumns);
        instance.classes =
            std::span<const SequentialClassTable>(classTables);
        instance.capturedNodes.assign(nodeSlots.size(), false);
        instance.rows.reserve(theirs.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < theirs.rows.size();
             ++rowIndex)
        {
            instance.rows.push_back(sequentialRowFor(
                field,
                theirs,
                theirs.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                nullptr,
                nodeSlots,
                classIndices[rowIndex]));
        }
        setupTiming.finish();
        if (pReplay != nullptr)
        {
            MasterProfileScope replayTiming(m_masterProfile, MasterPhase::SequentialReplay);
            solved.witnessReplays = replaySequentialWitness(instance, pReplay->rows, pReplay->value);
            return solved;
        }
        {
            MasterProfileScope searchTiming(m_masterProfile, MasterPhase::EnemySearch);
            solved.tier = solveSequentialPacking(
                instance,
                enemyFloor,
                stateCap,
                nullptr,
                captureWitness,
                m_masterFutureCatalog ? SequentialWitnessPolicy::BookedValue : SequentialWitnessPolicy::ImprovementOnly,
                m_tierAuditMode,
                pWarmStart,
                m_masterProfile != nullptr ? m_masterProfile->enemySequentialCost : nullptr,
                m_masterFutureCatalog ? SequentialPackingPolicy::CompatibleRows : SequentialPackingPolicy::LegacySuffix);
        }
        if (captureWitness)
        {
            MasterProfileScope replayTiming(m_masterProfile, MasterPhase::SequentialReplay);
            solved.witnessReplays = replaySequentialWitness(
                instance,
                solved.tier.witness,
                solved.tier.bookedValue);
        }
        return solved;
    }

    MilliFunds JointPlanStockValuer::enemySequentialOptimum() const
    {
        if (!m_enemySequentialOptimum)
        {
            m_enemySequentialOptimum =
                solveEnemySequential(SEQUENTIAL_SEARCH_STATE_CAP, false).tier.bookedValue;
        }
        return *m_enemySequentialOptimum;
    }

    const PropertyStockActor* JointPlanStockValuer::sequentialMover(
        const PropertyStockField::JointActionFacts & facts, std::int32_t row)
    {
        for (std::size_t mover = 0; mover < facts.moverRows.size(); ++mover)
        {
            if (facts.moverRows[mover] == row)
            {
                return &facts.movers[mover];
            }
        }
        return nullptr;
    }

    std::vector<std::int64_t> JointPlanStockValuer::sequentialRowKey(
        std::int32_t row, const PropertyStockActor* mover, std::span<const std::int32_t> capturedColumns)
    {
        std::vector<std::int64_t> key;
        key.reserve(ROW_ENUM_KEY_PREFIX + capturedColumns.size());
        key.push_back(row);
        key.push_back(mover != nullptr ? mover->tile.x : NO_STOCK_COLUMN);
        key.push_back(mover != nullptr ? mover->tile.y : NO_STOCK_COLUMN);
        key.push_back(mover != nullptr ? mover->carriedCapturePoints : 0);
        key.push_back(mover != nullptr ? static_cast<std::int64_t>(mover->survival) : 0);
        key.insert(key.end(), capturedColumns.begin(), capturedColumns.end());
        return key;
    }

    ContinuationEntry* JointPlanStockValuer::certificateEntry(const RelativeStockQuote & quote) const
    {
        auto* entry = m_continuationStore.find(quote.key);
        if (entry == nullptr || !quote.valid || entry->capturedColumns != quote.capturedColumns ||
            entry->ours.lower != quote.ours.lower || entry->ours.upper != quote.ours.upper ||
            entry->searchCompleted != quote.ourSearchCompleted)
        {
            return nullptr;
        }
        return entry;
    }

    std::int64_t JointPlanStockValuer::certificateReadinessTariff(
        const RelativeStockQuote & quote, bool pendingAdmission) const
    {
        const auto & field = continuationField();
        if (!std::in_range<std::int32_t>(field.m_ours.rows.size()) ||
            !std::in_range<std::int32_t>(field.m_ours.columns.size()) ||
            quote.key.size() > field.m_ours.rows.size() ||
            quote.capturedColumns.size() > field.m_ours.columns.size())
        {
            return -1;
        }
        const auto rows = static_cast<std::int64_t>(field.m_ours.rows.size());
        const auto columns = static_cast<std::int64_t>(field.m_ours.columns.size());
        const auto actions = static_cast<std::int64_t>(quote.key.size());
        const auto keyWidth = static_cast<std::int64_t>(ROW_ENUM_KEY_PREFIX) + columns;
        const auto lookupDepth = 2 * static_cast<std::int64_t>(std::bit_width(m_rowOptionCache.size())) + 1;
        auto entries = m_continuationStore.entryCount();
        if (pendingAdmission)
        {
            if (entries == std::numeric_limits<std::size_t>::max())
            {
                return -1;
            }
            ++entries;
        }
        const auto entryDepth = 2 * static_cast<std::int64_t>(std::bit_width(entries)) + 1;
        std::int64_t work = 1;
        // Both readiness and native setup look up the same immutable row domains.
        if (!addProduct(work, {actions + 1, actions + 1}) ||
            !addProduct(work, {rows, actions + 1}) ||
            !addProduct(work, {rows, keyWidth, 2 * lookupDepth + 2}) ||
            !addProduct(work, {actions + 1, 2 * entryDepth}) ||
            !addProduct(work, {actions + columns + 1, 2}))
        {
            return -1;
        }
        return work;
    }

    RelativeStockCertificatePreparation JointPlanStockValuer::prepareOurCertificate(
        const RelativeStockQuote & quote)
    {
        MasterProfileScope timing(m_masterProfile, MasterPhase::NativeCertificateReadiness);
        RelativeStockCertificatePreparation result;
        const auto tariff = certificateReadinessTariff(quote);
        if (!livePairSwapIntervals() || m_pRefinementLedger == nullptr || tariff < 0)
        {
            m_pricingFailure = true;
            return result;
        }
        if (!m_pRefinementLedger->consume(tariff, RefinementWork::MasterValuation))
        {
            result.status = RelativeStockPreparationStatus::BudgetExhausted;
            return result;
        }
        result.readinessWork = tariff;
        if (certificateEntry(quote) == nullptr)
        {
            m_pricingFailure = true;
            return result;
        }
        const auto & field = continuationField();
        const auto facts = field.jointActionFacts(planActions(quote.key));
        auto owned = field.m_ownedBaseline;
        for (const auto column : facts.capturedColumns)
        {
            if (!RelativeStockDetail::checkedAdd(owned, ownershipFlipSwing(
                    field.m_ours.columns[static_cast<std::size_t>(column)], field.m_horizonTurns)))
            {
                m_pricingFailure = true;
                return result;
            }
        }
        if (facts.capturedColumns != quote.capturedColumns || owned != quote.owned)
        {
            m_pricingFailure = true;
            return result;
        }
        std::int64_t scanWork = 0;
        for (std::size_t row = 0; row < field.m_ours.rows.size(); ++row)
        {
            const auto rowIndex = static_cast<std::int32_t>(row);
            const auto key = sequentialRowKey(rowIndex, sequentialMover(facts, rowIndex), facts.capturedColumns);
            const auto cached = m_rowOptionCache.find(key);
            if (cached == m_rowOptionCache.end())
            {
                result.status = RelativeStockPreparationStatus::MissingRows;
                return result;
            }
            if (!std::in_range<std::int64_t>(cached->second.options.size()) ||
                !addProduct(scanWork, {static_cast<std::int64_t>(cached->second.options.size()),
                                      CERTIFICATE_OPTION_SCAN_WORK}))
            {
                m_pricingFailure = true;
                return result;
            }
        }
        result.scanWork = scanWork;
        result.status = RelativeStockPreparationStatus::Ready;
        return result;
    }

    std::optional<std::int64_t> JointPlanStockValuer::certificateMinimumWork(const RelativeStockQuote & quote) const
    {
        const auto & field = continuationField();
        const auto facts = field.jointActionFacts(planActions(quote.key));
        std::int64_t catalogWork = 0;
        std::int64_t optionRecords = 0;
        std::int64_t incidences = 0;
        for (std::size_t row = 0; row < field.m_ours.rows.size(); ++row)
        {
            const auto rowIndex = static_cast<std::int32_t>(row);
            const auto key = sequentialRowKey(rowIndex, sequentialMover(facts, rowIndex), facts.capturedColumns);
            const auto cached = m_rowOptionCache.find(key);
            if (cached == m_rowOptionCache.end())
            {
                return std::nullopt;
            }
            for (const auto & option : cached->second.options)
            {
                if (!addCertificateOptionWork(option, catalogWork, optionRecords, incidences))
                {
                    return std::nullopt;
                }
            }
        }
        auto minimum = m_operationBaseTariffs->ours;
        if (!std::in_range<std::int64_t>(m_masterFutureCatalog->columns.size()) ||
            !addWork(minimum, catalogWork) || !addWork(minimum, optionRecords) ||
            !addWork(minimum, incidences) ||
            !addWork(minimum, static_cast<std::int64_t>(m_masterFutureCatalog->columns.size())))
        {
            return std::nullopt;
        }
        return minimum;
    }

    bool JointPlanStockValuer::prepareOperationEntry(const RelativeStockQuote & quote)
    {
        if (!quote.valid || !RelativeStockDetail::canonical(quote) || !RelativeStockDetail::coherent(quote))
        {
            return false;
        }
        const auto* entry = m_continuationStore.find(quote.key);
        if (entry != nullptr && certificateEntry(quote) == nullptr)
        {
            return false;
        }
        const auto knownUpper = m_verifiedOurUppers.find(quote.key);
        std::uint64_t certificateId = 0;
        if (knownUpper != m_verifiedOurUppers.end())
        {
            certificateId = knownUpper->second.id;
        }
        const auto* enemy = m_continuationStore.enemyFor(quote.capturedColumns);
        if (certificateId != quote.ourUpperCertificateId || enemy == nullptr ||
            enemy->bounds.lower != quote.enemy.lower || enemy->bounds.upper != quote.enemy.upper ||
            enemy->searchCompleted != quote.enemySearchCompleted)
        {
            return false;
        }
        CheapInitialPrice initial;
        return priceCheap(quote.key, initial);
    }

    RelativeStockOperationPreparation JointPlanStockValuer::prepareOurCertificateOperation(
        const RelativeStockQuote & quote)
    {
        RelativeStockOperationPreparation result;
        if (!m_operationServiceEnabled || !livePairSwapIntervals())
        {
            return result;
        }
        const auto before = m_pRefinementLedger->balance();
        if (!m_pRefinementLedger->consume(m_operationBaseTariffs->certificateAdmissionWork,
                                         RefinementWork::MasterValuation))
        {
            result.status = RelativeStockPreparationStatus::BudgetExhausted;
            result.requiredPreparationWork = m_operationBaseTariffs->certificateAdmissionWork;
            return result;
        }
        result.paidWork = before - m_pRefinementLedger->balance();
        if (!prepareOperationEntry(quote))
        {
            m_pricingFailure = true;
            return result;
        }
        const auto preparation = prepareOurCertificate(quote);
        result.status = preparation.status;
        result.readinessChecked = preparation.status == RelativeStockPreparationStatus::Ready ||
            preparation.status == RelativeStockPreparationStatus::MissingRows;
        result.paidWork = before - m_pRefinementLedger->balance();
        result.scanWork = preparation.scanWork;
        if (preparation.status == RelativeStockPreparationStatus::BudgetExhausted)
        {
            result.requiredPreparationWork = certificateReadinessTariff(quote);
        }
        if (preparation.status != RelativeStockPreparationStatus::Ready)
        {
            return result;
        }
        auto minimumScanWork = preparation.readinessWork;
        if (!addWork(minimumScanWork, preparation.scanWork))
        {
            m_pricingFailure = true;
            result.status = RelativeStockPreparationStatus::Invalid;
            return result;
        }
        // The second lookup and option-metadata pass prices the native admission minimum.
        if (m_pRefinementLedger->balance() < minimumScanWork ||
            !m_pRefinementLedger->consume(minimumScanWork, RefinementWork::MasterValuation))
        {
            result.status = RelativeStockPreparationStatus::BudgetExhausted;
            result.requiredPreparationWork = minimumScanWork;
            return result;
        }
        result.paidWork = before - m_pRefinementLedger->balance();
        const auto minimum = certificateMinimumWork(quote);
        if (!minimum)
        {
            m_pricingFailure = true;
            result.status = RelativeStockPreparationStatus::Invalid;
            return result;
        }
        result.minimumNativeWork = *minimum;
        return result;
    }

    JointPlanStockValuer::OurSequentialProblem
    JointPlanStockValuer::buildOurSequentialProblem(
        std::span<const PlanRowAction> actions) const
    {
        PropertyStockField & field = continuationField();
        OurSequentialProblem problem;
        const PropertyStockField::JointActionFacts facts =
            field.jointActionFacts(actions);
        problem.capturedColumns = facts.capturedColumns;
        problem.owned = field.m_ownedBaseline;
        for (const std::int32_t column : facts.capturedColumns)
        {
            problem.owned += ownershipFlipSwing(
                field.m_ours.columns[static_cast<std::size_t>(column)],
                field.m_horizonTurns);
        }
        problem.floorValue = field.ourOpenOptimum(facts);

        ensureOurSequentialState();
        std::vector<std::int32_t> classIndices(
            field.m_ours.rows.size(),
            NO_SEQUENTIAL_CLASS);
        for (std::size_t rowIndex = 0;
             rowIndex < field.m_ours.rows.size();
             ++rowIndex)
        {
            classIndices[rowIndex] =
                ourClassIndexFor(field.m_ours.rows[rowIndex]);
        }

        problem.instance.horizonTurns = field.m_horizonTurns;
        problem.instance.nodeColumns =
            std::span<const PropertyStockColumn>(m_ourNodeColumns);
        problem.instance.classes =
            std::span<const SequentialClassTable>(m_ourClassTables);
        problem.instance.capturedNodes.assign(
            m_ourNodeSlots.size(), false);
        for (const std::int32_t column : facts.capturedColumns)
        {
            const std::int32_t node =
                m_ourNodeOfColumn[static_cast<std::size_t>(column)];
            if (node != NO_SEQUENTIAL_NODE)
            {
                problem.instance.capturedNodes[
                    static_cast<std::size_t>(node)] = true;
            }
        }

        problem.rowKeys.reserve(field.m_ours.rows.size());
        problem.instance.rows.reserve(field.m_ours.rows.size());
        for (std::size_t rowIndex = 0;
             rowIndex < field.m_ours.rows.size();
             ++rowIndex)
        {
            const auto* pMover = sequentialMover(facts, static_cast<std::int32_t>(rowIndex));
            problem.rowKeys.push_back(sequentialRowKey(
                static_cast<std::int32_t>(rowIndex), pMover, facts.capturedColumns));
            problem.instance.rows.push_back(sequentialRowFor(
                field,
                field.m_ours,
                field.m_ours.rows[rowIndex],
                static_cast<std::int32_t>(rowIndex),
                pMover,
                m_ourNodeSlots,
                classIndices[rowIndex]));
        }
        return problem;
    }

    const PackingDualFieldIdentity &
    JointPlanStockValuer::packingDualFieldIdentity() const
    {
        if (!m_packingDualFieldIdentity.has_value())
        {
            const PropertyStockField & field =
                continuationField();
            m_packingDualFieldIdentity.emplace(
                PackingDualFieldIdentity{
                    .horizonTurns = field.m_horizonTurns,
                    .width = field.m_width,
                    .height = field.m_height,
                    .capturePointsToCapture =
                        field.m_capturePointsToCapture,
                    .ownedBaseline = field.m_ownedBaseline,
                    .ourRows = field.m_ours.rows,
                    .ourColumns = field.m_ours.columns,
                    .ourWeights = field.m_ours.weights,
                    .enemyRows = field.m_theirs.rows,
                    .enemyColumns = field.m_theirs.columns,
                    .enemyWeights = field.m_theirs.weights,
                    .columnTiles = field.m_columnTiles,
                    .columnCapturers = field.m_columnCapturer,
                    .columnCapturePoints =
                        field.m_columnCapturePoints,
                    .enemyArrivals = field.m_enemyArrivals,
                    .mobilityGrids = field.m_grids,
                });
        }
        return *m_packingDualFieldIdentity;
    }

    PackingDualProblemIdentity
    JointPlanStockValuer::packingDualProblemIdentity(
        const RelativeStockQuote & quote,
        const OurSequentialProblem & problem) const
    {
        PackingDualProblemIdentity identity;
        identity.immutableField =
            packingDualFieldIdentity();
        identity.key = quote.key;
        identity.capturedColumns =
            problem.capturedColumns;
        identity.rowDomains = problem.rowKeys;
        identity.nodeColumns.assign(
            problem.instance.nodeColumns.begin(),
            problem.instance.nodeColumns.end());
        identity.usedNodes =
            PackingDualNodeMask(
                problem.instance.nodeCount());
        for (std::int32_t node = 0;
             node < problem.instance.nodeCount();
             ++node)
        {
            if (problem.instance.capturedNodes[
                    static_cast<std::size_t>(node)])
            {
                identity.usedNodes.set(node);
            }
        }
        identity.economic = quote.economic;
        identity.owned = problem.owned;
        identity.incumbentFloor =
            problem.floorValue;
        identity.fixedValue = 0;
        return identity;
    }

    RelativeStockDualTightening
    JointPlanStockValuer::
        tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote,
            std::uint64_t sessionIdentity,
            std::int64_t maximumWork)
    {
        MasterProfileScope setupTiming(m_masterProfile, MasterPhase::NativeCertificateSetup);
        RelativeStockDualTightening result;
        result.intervalBefore = quote.ours;
        result.intervalAfter = quote.ours;
        const auto failGlobal = [&]()
        {
            result.globalFailure = true;
            m_pricingFailure = true;
        };
        if (!livePairSwapIntervals() ||
            sessionIdentity == 0 ||
            maximumWork < 0 ||
            m_pRefinementLedger == nullptr)
        {
            failGlobal();
            return result;
        }
        ContinuationEntry* pEntry = certificateEntry(quote);
        if (pEntry == nullptr)
        {
            failGlobal();
            return result;
        }

        OurSequentialProblem problem =
            buildOurSequentialProblem(
                planActions(quote.key));
        PackingDualProblemIdentity identity;
        {
            MasterProfileScope identityTiming(m_masterProfile, MasterPhase::NativeCertificateIdentity);
            identity = packingDualProblemIdentity(quote, problem);
        }
        identity.session = sessionIdentity;
        if (problem.capturedColumns !=
                quote.capturedColumns ||
            problem.owned != quote.owned ||
            identity.usedNodes.nodeCount() !=
                problem.instance.nodeCount())
        {
            failGlobal();
            return result;
        }

        const auto known =
            m_verifiedOurUppers.find(quote.key);
        if (known != m_verifiedOurUppers.end())
        {
            const auto & certificate =
                known->second.certificate;
            if (certificate == nullptr ||
                certificate->identity != identity ||
                known->second.id == 0)
            {
                failGlobal();
                return result;
            }
            result.certificate = certificate;
            result.certificateId = known->second.id;
            result.certifiedUpper =
                certificate->dualObjective;
            if (result.certifiedUpper <
                    identity.incumbentFloor ||
                pEntry->ours.lower >
                    result.certifiedUpper ||
                pEntry->ours.upper >
                    result.certifiedUpper)
            {
                failGlobal();
                return result;
            }
            result.intervalAfter = pEntry->ours;
            result.catalogComplete = true;
            result.certificateValid = true;
            result.optimizerCompleted = true;
            result.verifierCompleted = true;
            result.reused = true;
            result.completed = true;
            return result;
        }

        std::vector<PackingDualRowSource> sources;
        sources.reserve(problem.rowKeys.size());
        std::int64_t expectedCatalogWork = 0;
        std::int64_t expectedOptionRecords = 0;
        std::int64_t expectedIncidences = 0;
        MasterProfileScope readinessTiming(m_masterProfile, MasterPhase::NativeCertificateReadiness);
        for (std::size_t row = 0;
             row < problem.rowKeys.size();
             ++row)
        {
            const auto cached =
                m_rowOptionCache.find(
                    problem.rowKeys[row]);
            if (cached == m_rowOptionCache.end())
            {
                result.locallyBudgetExhausted = true;
                return result;
            }
            for (const auto & option :
                 cached->second.options)
            {
                if (!addCertificateOptionWork(option, expectedCatalogWork, expectedOptionRecords, expectedIncidences))
                {
                    failGlobal();
                    return result;
                }
            }
            sources.push_back(PackingDualRowSource{
                .row = static_cast<std::int32_t>(row),
                .domain =
                    std::span<const std::int64_t>(
                        problem.rowKeys[row]),
                .options =
                    std::span<const
                        SequentialDetail::RowOption>(
                            cached->second.options),
            });
        }

        std::int64_t verifierWork =
            static_cast<std::int64_t>(
                identity.nodeColumns.size());
        std::int64_t minimumWork = ourRerunTariff();
        if (minimumWork < 0 ||
            !addWork(verifierWork, expectedOptionRecords) ||
            !addWork(verifierWork, expectedIncidences) ||
            !addWork(minimumWork, expectedCatalogWork) ||
            !addWork(minimumWork, verifierWork))
        {
            failGlobal();
            return result;
        }
        const std::int64_t allowance =
            std::min(
                maximumWork,
                m_pRefinementLedger->balance());
        if (allowance <= minimumWork)
        {
            result.locallyBudgetExhausted = true;
            return result;
        }
        readinessTiming.finish();
        setupTiming.finish();
        const std::int64_t balanceBefore =
            m_pRefinementLedger->balance();
        const GrantId grant =
            m_pRefinementLedger->draw(
                allowance,
                RefinementWork::
                    PackingDualCertificate);
        if (grant == NO_GRANT)
        {
            result.locallyBudgetExhausted = true;
            return result;
        }
        result.admitted = true;
        result.tariffWork = ourRerunTariff();
        const auto finishGrant =
            [&](std::int64_t spent) -> bool
        {
            return spent >= 0 &&
                spent <= allowance &&
                m_pRefinementLedger->refund(
                    grant,
                    allowance - spent);
        };
        const auto finishWork =
            [&](std::initializer_list<std::int64_t> parts)
        {
            std::int64_t spent = 0;
            for (const std::int64_t part : parts)
            {
                if (!addWork(spent, part))
                {
                    return false;
                }
            }
            if (!finishGrant(spent))
            {
                return false;
            }
            result.totalWork =
                balanceBefore -
                m_pRefinementLedger->balance();
            return result.totalWork == spent;
        };

        PackingDualCatalog catalog;
        {
            MasterProfileScope timing(m_masterProfile, MasterPhase::NativeCertificateCatalog);
            catalog = buildPackingDualCatalog(sources, identity, expectedCatalogWork);
        }
        result.catalogWork =
            catalog.constructionWork;
        result.catalogComplete =
            catalog.complete;
        if (!catalog.complete ||
            catalog.constructionWork !=
                expectedCatalogWork ||
            catalog.optionRecords !=
                expectedOptionRecords ||
            catalog.nodeIncidences !=
                expectedIncidences ||
            packingDualVerifierWork(catalog) !=
                verifierWork)
        {
            finishWork({
                result.tariffWork,
                result.catalogWork,
            });
            failGlobal();
            return result;
        }

        const std::int64_t optimizerLimit =
            allowance -
            result.tariffWork -
            result.catalogWork -
            verifierWork;
        PackingDualOptimizerResult optimizer;
        {
            MasterProfileScope timing(m_masterProfile, MasterPhase::NativeCertificateOptimization);
            optimizer = optimizePackingDualPrices(catalog, identity, optimizerLimit);
        }
        result.optimizerWork = optimizer.workUsed;
        result.optimizerCompleted =
            optimizer.completed;
        if (!optimizer.completed)
        {
            if (!finishWork({
                    result.tariffWork,
                    result.catalogWork,
                    result.optimizerWork,
                }))
            {
                failGlobal();
            }
            result.locallyBudgetExhausted =
                !result.globalFailure;
            return result;
        }

        PackingDualVerification verified;
        {
            MasterProfileScope timing(m_masterProfile, MasterPhase::NativeCertificateVerification);
            verified = verifyPackingDualCertificate(catalog, sources, identity, optimizer.nodePrices, verifierWork);
        }
        result.verifierWork = verified.workUsed;
        result.verifierCompleted =
            verified.completed;
        result.certificateValid =
            verified.valid;
        const bool optimizerCertified =
            verified.completed && verified.valid && result.verifierWork == verifierWork;
        const auto hint = m_verifiedOurUppers.contains(quote.key)
            ? m_ourUpperPriceHints.end() : m_ourUpperPriceHints.find(quote.key);
        // Untrusted hinted prices only compete with the optimizer's certificate inside the remaining allowance.
        const std::int64_t hintLimit = optimizerLimit - result.optimizerWork - verifierWork;
        if (optimizerCertified && hint != m_ourUpperPriceHints.end() && m_masterFutureCatalog && hintLimit > 0)
        {
            MasterProfileScope timing(m_masterProfile, MasterPhase::NativeCertificateVerification);
            const auto hinted =
                hintedPackingDualPrices(identity, m_masterFutureCatalog->columns, hint->second, hintLimit);
            result.hintWork = hinted.workUsed;
            if (hinted.completed)
            {
                auto hintedVerified =
                    verifyPackingDualCertificate(catalog, sources, identity, hinted.nodePrices, verifierWork);
                if (!addWork(result.hintWork, hintedVerified.workUsed))
                {
                    failGlobal();
                }
                else if (hintedVerified.completed && hintedVerified.valid && hintedVerified.workUsed == verifierWork &&
                         hintedVerified.certificate.dualObjective < verified.certificate.dualObjective)
                {
                    verified = std::move(hintedVerified);
                }
            }
        }
        if (!finishWork({
                result.tariffWork,
                result.catalogWork,
                result.optimizerWork,
                result.verifierWork,
                result.hintWork,
            }))
        {
            failGlobal();
        }
        if (result.globalFailure || !optimizerCertified)
        {
            result.certificateValid = false;
            failGlobal();
            return result;
        }

        result.certifiedUpper =
            verified.certificate.dualObjective;
        if (result.certifiedUpper <
                identity.incumbentFloor ||
            result.certifiedUpper <
                pEntry->ours.lower)
        {
            result.certificateValid = false;
            failGlobal();
            return result;
        }
        result.certificate =
            std::make_shared<const
                PackingDualCertificate>(
                    verified.certificate);
        if (m_nextCertificateId == 0 ||
            m_nextCertificateId ==
                std::numeric_limits<std::uint64_t>::max())
        {
            result.certificate.reset();
            result.certificateValid = false;
            failGlobal();
            return result;
        }
        result.certificateId =
            m_nextCertificateId++;
        const bool inserted =
            m_verifiedOurUppers.emplace(
                quote.key,
                VerifiedOurUpper{
                    result.certificateId,
                    result.certificate,
                })
                .second;
        if (!inserted)
        {
            result.certificate.reset();
            result.certificateId = 0;
            result.certificateValid = false;
            failGlobal();
            return result;
        }
        if (result.certifiedUpper <
            pEntry->ours.upper)
        {
            pEntry->ours.upper =
                result.certifiedUpper;
        }
        result.intervalAfter = pEntry->ours;
        result.completed = true;
        return result;
    }

    bool JointPlanStockValuer::verifiedUpperMatches(
        const RelativeStockQuote & quote,
        std::uint64_t sessionIdentity) const
    {
        if (quote.ourUpperCertificateId == 0)
        {
            return true;
        }
        const auto known =
            m_verifiedOurUppers.find(quote.key);
        if (known == m_verifiedOurUppers.end() ||
            known->second.id !=
                quote.ourUpperCertificateId ||
            known->second.certificate == nullptr)
        {
            return false;
        }
        const auto & identity =
            known->second.certificate->identity;
        const MilliFunds certifiedUpper =
            known->second.certificate->dualObjective;
        const ContinuationEntry* pEntry =
            m_continuationStore.find(quote.key);
        return identity.session == sessionIdentity &&
            identity.immutableField ==
                packingDualFieldIdentity() &&
            identity.key == quote.key &&
            identity.capturedColumns ==
                quote.capturedColumns &&
            identity.economic == quote.economic &&
            identity.owned == quote.owned &&
            certifiedUpper >=
                identity.incumbentFloor &&
            pEntry != nullptr &&
            pEntry->ours.lower == quote.ours.lower &&
            pEntry->ours.upper == quote.ours.upper &&
            pEntry->searchCompleted ==
                quote.ourSearchCompleted &&
            quote.ours.lower <= certifiedUpper &&
            quote.ours.upper <= certifiedUpper;
    }

    JointPlanStockValuer::OurSequentialSolve
    JointPlanStockValuer::solveOurSequential(
        std::span<const PlanRowAction> actions,
        std::int64_t stateCap,
        bool captureWitness,
        const RefinedWitness* pReplay,
        const SequentialWarmStart* pWarmStart) const
    {
        MasterProfileScope setupTiming(m_masterProfile, MasterPhase::FriendlySetup);
        OurSequentialProblem problem =
            buildOurSequentialProblem(actions);
        OurSequentialSolve solved;
        solved.capturedColumns = problem.capturedColumns;
        solved.owned = problem.owned;
        solved.floorValue = problem.floorValue;
        setupTiming.finish();

        if (pReplay != nullptr)
        {
            MasterProfileScope replayTiming(m_masterProfile, MasterPhase::SequentialReplay);
            solved.witnessReplays = replaySequentialWitness(
                problem.instance,
                pReplay->rows,
                pReplay->value);
            return solved;
        }
        ValuerRowOptionSource rowSource;
        rowSource.pCache = &m_rowOptionCache;
        rowSource.pKeys = &problem.rowKeys;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        rowSource.pPaidKeys =
            m_pRefinementLedger != nullptr
                ? &m_rowOptionPaidKeys
                : nullptr;
#endif
        {
            MasterProfileScope searchTiming(m_masterProfile, MasterPhase::FriendlySearch);
            solved.tier = solveSequentialPacking(
                problem.instance,
                problem.floorValue,
                stateCap,
                &rowSource,
                captureWitness,
                m_masterFutureCatalog ? SequentialWitnessPolicy::BookedValue : SequentialWitnessPolicy::ImprovementOnly,
                m_tierAuditMode,
                pWarmStart,
                m_masterProfile != nullptr ? m_masterProfile->friendlySequentialCost : nullptr,
                m_masterFutureCatalog ? SequentialPackingPolicy::CompatibleRows : SequentialPackingPolicy::LegacySuffix);
        }
        if (captureWitness)
        {
            MasterProfileScope replayTiming(m_masterProfile, MasterPhase::SequentialReplay);
            solved.witnessReplays = replaySequentialWitness(
                problem.instance,
                solved.tier.witness,
                solved.tier.bookedValue);
        }
        return solved;
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    DiagnosticSequentialThresholdProof
    JointPlanStockValuer::diagnosticProveOurBelow(
        const ContinuationKey & key,
        MilliFunds threshold,
        std::int64_t maximumWork,
        SequentialCacheAccounting cacheAccounting,
        SequentialThresholdOptions options)
    {
        DiagnosticSequentialThresholdProof result;
        result.tariff = ourRerunTariff();
        CheapInitialPrice initial;
        if (!livePairSwapIntervals() ||
            !priceCheap(key, initial) ||
            maximumWork <= result.tariff ||
            result.tariff < 0 ||
            m_pRefinementLedger == nullptr)
        {
            return result;
        }
        const std::int64_t available =
            std::min(maximumWork, m_pRefinementLedger->balance());
        if (available <= result.tariff)
        {
            return result;
        }
        result.stateCap = available - result.tariff;
        const GrantId grant = m_pRefinementLedger->draw(
            available, RefinementWork::SearchSlice);
        if (grant == NO_GRANT)
        {
            return result;
        }
        const std::int64_t balanceBefore =
            m_pRefinementLedger->balance() + available;
        OurSequentialProblem problem =
            buildOurSequentialProblem(planActions(key));
        ValuerRowOptionSource rowSource;
        rowSource.pCache = &m_rowOptionCache;
        rowSource.pKeys = &problem.rowKeys;
        rowSource.pPaidKeys = &m_rowOptionPaidKeys;
        result.proof = proveSequentialBelow(
            problem.instance,
            problem.floorValue,
            threshold,
            result.stateCap,
            &rowSource,
            cacheAccounting,
            options);
        if (result.proof.workUsed < 0 ||
            result.proof.workUsed > result.stateCap ||
            !m_pRefinementLedger->refund(
                grant,
                result.stateCap - result.proof.workUsed))
        {
            return result;
        }
        result.workUsed =
            balanceBefore - m_pRefinementLedger->balance();
        result.witnessReplays = replaySequentialWitness(
            problem.instance,
            result.proof.witness,
            result.proof.feasibleLower);
        result.proof.witnessReplays =
            result.proof.witnessReplays &&
            result.witnessReplays;
        result.valid =
            result.proof.status !=
                SequentialThresholdStatus::Invalid &&
            result.witnessReplays &&
            result.workUsed ==
                result.tariff + result.proof.workUsed;
        return result;
    }
#endif

    MilliFunds JointPlanStockValuer::sequentialJointStock(
        std::span<const PlanRowAction> actions) const
    {
        const OurSequentialSolve ours =
            solveOurSequential(actions,
                               SEQUENTIAL_SEARCH_STATE_CAP,
                               false);
        const MilliFunds enemyBooked = enemySequentialOptimum();
        return ours.owned + ours.tier.bookedValue - enemyBooked;
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    void JointPlanStockValuer::recordWarmStart(
        std::size_t witnesses, std::size_t keyWidth, std::size_t rows, std::size_t columns,
        const std::optional<SequentialWarmStart> & seed, const SequentialTierResult & tier, bool witnessReplays)
    {
        m_diagnosticLastWarmStart = DiagnosticWarmStart{
            .witnesses = witnesses,
            .keyWidth = keyWidth,
            .rows = rows,
            .columns = columns,
            .floor = tier.floorValue,
            .repair = tier.repairValue,
            .booked = tier.bookedValue,
            .states = tier.searchStates,
            .accepted = tier.warmStartAccepted,
            .selected = tier.warmStartSelected,
            .witnessReplays = witnessReplays,
        };
        if (seed)
        {
            m_diagnosticLastWarmStart->seedValue = seed->value;
        }
    }
#endif

    bool JointPlanStockValuer::refineOur(
        ContinuationEntry & entry,
        GrantId grant,
        std::int64_t stateCap,
        RefinementSchedule schedule,
        bool* pCompleted)
    {
        std::int64_t expectedCap = 0;
        if (m_pRefinementLedger == nullptr ||
            grant == NO_GRANT ||
            stateCap <= 0 ||
            (schedule == RefinementSchedule::Rung &&
             (!livePairRefinementSliceCap(entry.sliceRung, expectedCap) || expectedCap != stateCap)))
        {
            m_pricingFailure = true;
            return false;
        }
        std::optional<SequentialWarmStart> seed;
        if (m_masterFutureCatalog && schedule == RefinementSchedule::Relative && !m_refinedOurWitnesses.empty())
        {
            const auto known = m_refinedOurWitnesses.find(entry.key);
            if (known != m_refinedOurWitnesses.end() && known->second.replays &&
                known->second.value == entry.ours.lower)
            {
                seed = SequentialWarmStart{known->second.rows, known->second.value};
            }
        }
        OurSequentialSolve solved =
            solveOurSequential(
                planActions(entry.key),
                stateCap,
                true,
                nullptr,
                seed ? &*seed : nullptr);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        recordWarmStart(m_refinedOurWitnesses.size(), m_field.m_ours.rows.size(),
                        m_field.m_ours.rows.size(), m_field.m_ours.columns.size(),
                        seed, solved.tier, solved.witnessReplays);
        if (m_tierAuditMode == SequentialTierAuditMode::Full)
        {
            MasterProfileScope archiveTiming(m_masterProfile, MasterPhase::TierArchiveCopy);
            m_diagnosticLastOurRefinement = solved.tier;
        }
#endif
        if ((seed && !solved.tier.warmStartAccepted) ||
            solved.capturedColumns != entry.capturedColumns ||
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
        if (schedule == RefinementSchedule::Rung && !advanceLivePairRefinementRung(nextRung, true))
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
        entry.searchCompleted = entry.searchCompleted || solved.tier.searchCompleted;
        if (pCompleted != nullptr)
        {
            *pCompleted = solved.tier.searchCompleted || solved.tier.certificate;
        }
        return true;
    }

    JointPlanStockValuer::WitnessAdoption JointPlanStockValuer::adoptOurWitness(
        ContinuationEntry & entry, std::span<const SequentialRowWitness> witness, MilliFunds value)
    {
        RefinedWitness candidate{.rows = {witness.begin(), witness.end()}, .value = value, .replays = true};
        const OurSequentialSolve solved = solveOurSequential(planActions(entry.key), 0, false, &candidate);
        if (solved.capturedColumns != entry.capturedColumns)
        {
            m_pricingFailure = true;
            return WitnessAdoption::Failed;
        }
        // A stored witness below the floor would be refused as the next refinement's warm start.
        if (!solved.witnessReplays || value < solved.floorValue)
        {
            return WitnessAdoption::Declined;
        }
        if (value > entry.ours.upper)
        {
            m_pricingFailure = true;
            return WitnessAdoption::Failed;
        }
        entry.ours.lower = value;
        m_refinedOurWitnesses[entry.key] = std::move(candidate);
        entry.storeBacked = true;
        entry.exact = entry.ours.exact();
        return WitnessAdoption::Adopted;
    }

    bool JointPlanStockValuer::refineEnemy(
        EnemySetBounds & enemy,
        const std::vector<std::int32_t> & capturedColumns,
        GrantId grant,
        std::int64_t stateCap,
        RefinementSchedule schedule,
        bool* pCompleted)
    {
        std::int64_t expectedCap = 0;
        if (m_pRefinementLedger == nullptr ||
            grant == NO_GRANT ||
            stateCap <= 0 ||
            (schedule == RefinementSchedule::Rung &&
             (!livePairRefinementSliceCap(enemy.sliceRung, expectedCap) || expectedCap != stateCap)))
        {
            m_pricingFailure = true;
            return false;
        }
        std::optional<SequentialWarmStart> seed;
        if (m_masterFutureCatalog && schedule == RefinementSchedule::Relative && !m_refinedEnemyWitnesses.empty())
        {
            const auto known = m_refinedEnemyWitnesses.find(capturedColumns);
            if (known != m_refinedEnemyWitnesses.end() && known->second.replays &&
                known->second.value == enemy.bounds.lower)
            {
                seed = SequentialWarmStart{known->second.rows, known->second.value};
            }
        }
        EnemySequentialSolve solved =
            solveEnemySequential(
                stateCap,
                true,
                nullptr,
                seed ? &*seed : nullptr);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        recordWarmStart(m_refinedEnemyWitnesses.size(), capturedColumns.size(),
                        m_field.m_theirs.rows.size(), m_field.m_theirs.columns.size(),
                        seed, solved.tier, solved.witnessReplays);
#endif
        if ((seed && !solved.tier.warmStartAccepted) ||
            solved.tier.searchStates < 0 ||
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
        if (schedule == RefinementSchedule::Rung && !advanceLivePairRefinementRung(nextRung, true))
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
        enemy.searchCompleted = enemy.searchCompleted || solved.tier.searchCompleted;
        if (pCompleted != nullptr)
        {
            *pCompleted = solved.tier.searchCompleted || solved.tier.certificate;
        }
        return true;
    }

    LiveRefinementResult JointPlanStockValuer::refineLiveKey(
        const ContinuationKey & key, std::int64_t maximumWork)
    {
        LiveRefinementResult result;
        result.valid = m_pRefinementLedger != nullptr && !m_pricingFailure && maximumWork >= 0;
        if (!result.valid || m_refinementClosed)
        {
            return result;
        }
        ContinuationEntry* pEntry =
            m_continuationStore.find(key);
        if (pEntry == nullptr)
        {
            return result;
        }
        EnemySetBounds* pEnemy =
            m_continuationStore.enemyFor(
                pEntry->capturedColumns);
        if (pEnemy == nullptr)
        {
            return result;
        }

        const std::vector<std::int32_t> capturedColumns =
            pEntry->capturedColumns;
        const ContinuationEntry entryBefore = *pEntry;
        const EnemySetBounds enemyBefore = *pEnemy;
        const std::int64_t balanceBefore = m_pRefinementLedger->balance();
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
                result.valid = false;
                return result;
            }
            if (request > maximumWork)
            {
                result.budgetExhausted = true;
                return result;
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
                result.budgetExhausted = true;
                return result;
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
                const auto enemyBalance = m_pRefinementLedger->balance();
                valid =
                    refineEnemy(
                        *pEnemy,
                        capturedColumns,
                        grant,
                        enemyCap) &&
                    valid;
                result.enemyStatesUsed = enemyCap - (m_pRefinementLedger->balance() - enemyBalance);
            }
            result.workUsed = balanceBefore - m_pRefinementLedger->balance();
            result.statesUsed = result.workUsed - (request - ourCap - enemyCap);
            if (!valid)
            {
                restore();
                m_pricingFailure = true;
                m_refinementClosed = true;
                result.valid = false;
                return result;
            }
            m_contenderRegistry.markAdmitted(key);
            if (pEntry->exact && pEnemy->exact)
            {
                m_contenderRegistry.markResolved(key);
            }
            result.refined = pEntry->ours.lower > entryBefore.ours.lower || pEntry->ours.upper < entryBefore.ours.upper ||
                pEnemy->bounds.lower > enemyBefore.bounds.lower || pEnemy->bounds.upper < enemyBefore.bounds.upper;
            result.keysRefined = result.refined ? 1 : 0;
            return result;
        }

        const bool canRefineOur = !pEntry->exact;
        const bool canRefineEnemy = !pEnemy->exact;
        if (!canRefineOur && !canRefineEnemy)
        {
            m_contenderRegistry.markResolved(key);
            return result;
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
            result.valid = false;
            return result;
        }
        if (request > maximumWork)
        {
            result.budgetExhausted = true;
            return result;
        }
        const GrantId grant =
            m_pRefinementLedger->draw(
                request,
                RefinementWork::SearchSlice);
        if (grant == NO_GRANT)
        {
            m_refinementClosed = true;
            result.budgetExhausted = true;
            return result;
        }
        const bool valid =
            chooseOur
                ? refineOur(*pEntry, grant, stateCap)
                : refineEnemy(
                      *pEnemy,
                      capturedColumns,
                      grant,
                      stateCap);
        result.workUsed = balanceBefore - m_pRefinementLedger->balance();
        result.statesUsed = result.workUsed - tariff;
        result.enemyStatesUsed = chooseOur ? 0 : result.statesUsed;
        if (!valid)
        {
            restore();
            m_pricingFailure = true;
            m_refinementClosed = true;
            result.valid = false;
            return result;
        }
        if (pEntry->exact && pEnemy->exact)
        {
            m_contenderRegistry.markResolved(key);
        }
        result.refined = pEntry->ours.lower > entryBefore.ours.lower || pEntry->ours.upper < entryBefore.ours.upper ||
            pEnemy->bounds.lower > enemyBefore.bounds.lower || pEnemy->bounds.upper < enemyBefore.bounds.upper;
        result.keysRefined = result.refined ? 1 : 0;
        return result;
    }

    LiveRefinementResult JointPlanStockValuer::refineLiveAtBoundary(
        AssignPhase phase, std::int64_t maximumWork)
    {
        LiveRefinementResult result;
        result.valid = !m_pricingFailure && maximumWork >= 0;
        if (!livePairSwapIntervals() ||
            phase != AssignPhase::BetweenSwapSweeps ||
            m_refinementClosed || !result.valid)
        {
            return result;
        }
        const std::vector<ContinuationKey> order = m_contenderRegistry.refinementOrder();
        for (const ContinuationKey & key : order)
        {
            const auto step = refineLiveKey(key, maximumWork - result.workUsed);
            result.refined = result.refined || step.refined;
            result.keysRefined += step.keysRefined;
            result.statesUsed += step.statesUsed;
            result.enemyStatesUsed += step.enemyStatesUsed;
            result.workUsed += step.workUsed;
            result.budgetExhausted = result.budgetExhausted || step.budgetExhausted;
            result.valid = result.valid && step.valid;
            if (m_refinementClosed || !result.valid || step.workUsed > 0)
            {
                break;
            }
        }
        return result;
    }

    MilliFunds JointPlanStockValuer::stockCeiling() const
    {
        return m_field.stockCeiling();
    }

    bool JointPlanStockValuer::affectsStock(std::int32_t engineUnitId) const
    {
        return rowOf(engineUnitId) != NO_STOCK_ROW;
    }

    std::int32_t JointPlanStockValuer::stockRow(
        std::int32_t engineUnitId) const
    {
        return rowOf(engineUnitId);
    }

    std::optional<ContinuationKey> JointPlanStockValuer::continuationKey(const TurnPlan & plan) const
    {
        return canonicalPlanKey(planActions(plan));
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
