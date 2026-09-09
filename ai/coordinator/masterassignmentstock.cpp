#include "ai/coordinator/masterassignmentstock.h"

#include <bit>

#include "ai/coordinator/bundleassignment.h"

namespace Coordinator
{
    bool MasterAssignmentStockContext::copyColdField(
        const PropertyStockField & source, RefinementLedger & ledger)
    {
        if (!chargeMasterWork(ledger, source.m_grids.size() + 1, 1, RefinementWork::MasterPreparation))
        {
            return false;
        }
        std::int64_t tariff = 1;
        for (const auto size : {source.m_ours.rows.size(), source.m_ours.columns.size(),
             source.m_ours.weights.size(), source.m_theirs.rows.size(), source.m_theirs.columns.size(),
             source.m_theirs.weights.size(), source.m_columnAtTile.size(), source.m_propertyAtTile.size(),
             source.m_columnTiles.size(), source.m_columnCapturer.size(), source.m_columnCapturePoints.size(),
             source.m_enemyArrivals.size()})
        {
            const auto cells = masterWorkProduct(size, 1);
            if (!cells || !RelativeStockDetail::checkedAdd(tariff, *cells))
            {
                m_status = MasterStockSetupStatus::ArithmeticFailure;
                return false;
            }
        }
        for (const auto & grid : source.m_grids)
        {
            const auto cells = masterWorkProduct(static_cast<std::size_t>(grid.width()),
                                                static_cast<std::size_t>(grid.height()) * ENTRY_DIRECTION_COUNT);
            if (!cells || !RelativeStockDetail::checkedAdd(tariff, *cells))
            {
                m_status = MasterStockSetupStatus::ArithmeticFailure;
                return false;
            }
        }
        if (ledger.draw(tariff, RefinementWork::MasterPreparation) == NO_GRANT)
        {
            return false;
        }
        m_tariffs.certificateIdentity = tariff;
        m_field.m_horizonTurns = source.m_horizonTurns;
        m_field.m_width = source.m_width;
        m_field.m_height = source.m_height;
        m_field.m_capturePointsToCapture = source.m_capturePointsToCapture;
        m_field.m_ownedBaseline = source.m_ownedBaseline;
        m_field.m_originStock = source.m_originStock;
        m_field.m_ourOptimum = source.m_ourOptimum;
        m_field.m_enemyOptimum = source.m_enemyOptimum;
        m_field.m_ours = source.m_ours;
        m_field.m_theirs = source.m_theirs;
        m_field.m_columnAtTile = source.m_columnAtTile;
        m_field.m_propertyAtTile = source.m_propertyAtTile;
        m_field.m_columnTiles = source.m_columnTiles;
        m_field.m_columnCapturer = source.m_columnCapturer;
        m_field.m_columnCapturePoints = source.m_columnCapturePoints;
        m_field.m_enemyArrivals = source.m_enemyArrivals;
        m_field.m_grids = source.m_grids;
        return true;
    }

    bool MasterAssignmentStockContext::prepareTariffs(
        const RelativeStockSession & session, RefinementLedger & ledger)
    {
        const auto rows = m_field.m_ours.rows.size() + m_field.m_theirs.rows.size() + 1;
        const auto columns = m_field.m_ours.columns.size() + 1;
        const auto horizon = static_cast<std::size_t>(std::max(m_field.m_horizonTurns, 0)) + 1;
        if (!chargeMasterWork(ledger, rows + columns, std::bit_width(rows) + 1,
                              RefinementWork::MasterPreparation))
        {
            return false;
        }
        const auto rowCells = masterWorkProduct(rows, columns);
        if (!rowCells)
        {
            return false;
        }
        const auto quote = masterWorkProduct(static_cast<std::size_t>(*rowCells), horizon);
        if (!quote)
        {
            return false;
        }
        m_tariffs.quote = *quote;
        m_tariffs.witness = session.ourRerunWork();
        if (m_tariffs.witness < 0)
        {
            return false;
        }
        const auto enemy = session.enemyRerunWork({});
        const auto capturedScan = masterWorkProduct(columns, columns);
        return enemy >= 0 && capturedScan &&
            RelativeStockDetail::checkedAdd(m_tariffs.witness, enemy) &&
            RelativeStockDetail::checkedAdd(m_tariffs.witness, *capturedScan) &&
            RelativeStockDetail::checkedAdd(m_tariffs.witness, *quote);
    }

    MasterStockSetupStatus MasterAssignmentStockContext::prepare(
        const PropertyStockField & source, const AssignmentInput & input, RefinementLedger & ledger,
        MasterAssignmentProfile* profile, SequentialTierAuditMode auditMode)
    {
        MasterProfileScope timing(profile, MasterPhase::Catalog);
        if (m_status != MasterStockSetupStatus::Unprepared)
        {
            return MasterStockSetupStatus::Invalid;
        }
        if (!copyColdField(source, ledger))
        {
            if (m_status != MasterStockSetupStatus::ArithmeticFailure)
            {
                m_status = MasterStockSetupStatus::BudgetExhausted;
            }
            return m_status;
        }
        if (!chargeMasterWork(ledger, input.unitLinks.size() + input.actors.size() + 1,
                              std::bit_width(input.unitLinks.size()) + 1, RefinementWork::MasterPreparation))
        {
            m_status = MasterStockSetupStatus::BudgetExhausted;
            return m_status;
        }
        std::int64_t candidateCount = 0;
        for (const auto & actor : input.actors)
        {
            if (!chargeMasterWork(ledger, actor.candidates.size() + 1, 1, RefinementWork::MasterPreparation))
            {
                m_status = MasterStockSetupStatus::BudgetExhausted;
                return m_status;
            }
            for (const auto & candidate : actor.candidates)
            {
                if (!chargeMasterWork(ledger, candidate.bundle.components.size() + candidate.bundle.path.size() + 1,
                                      input.unitLinks.size() + 1, RefinementWork::MasterPreparation))
                {
                    m_status = MasterStockSetupStatus::BudgetExhausted;
                    return m_status;
                }
                if (!RelativeStockDetail::checkedAdd(candidateCount, 1))
                {
                    m_status = MasterStockSetupStatus::ArithmeticFailure;
                    return m_status;
                }
            }
        }
        if (!chargeMasterWork(ledger, static_cast<std::size_t>(candidateCount) + m_field.m_ours.columns.size() + 1,
                              std::bit_width(static_cast<std::uint64_t>(candidateCount)) + 1,
                              RefinementWork::MasterPreparation))
        {
            m_status = MasterStockSetupStatus::BudgetExhausted;
            return m_status;
        }
        m_valuer = std::make_unique<JointPlanStockValuer>(m_field, input.unitLinks, auditMode);
        auto catalog = m_valuer->continuationPricingCatalog(input);
        if (!catalog.valid)
        {
            m_status = MasterStockSetupStatus::Invalid;
            return m_status;
        }
        if (!m_valuer->prepareMasterContinuationPricing(std::move(catalog), ledger, profile))
        {
            m_status = ledger.stats().refusals == 0
                ? MasterStockSetupStatus::Invalid : MasterStockSetupStatus::BudgetExhausted;
            return m_status;
        }
        const auto* relative = m_valuer->relativeStockSession();
        if (relative == nullptr || !prepareTariffs(*relative, ledger))
        {
            m_status = ledger.stats().refusals == 0
                ? MasterStockSetupStatus::ArithmeticFailure : MasterStockSetupStatus::BudgetExhausted;
            return m_status;
        }
        m_status = MasterStockSetupStatus::Prepared;
        return m_status;
    }

    RelativeStockSession* MasterAssignmentStockContext::session()
    {
        return m_status == MasterStockSetupStatus::Prepared ? m_valuer->relativeStockSession() : nullptr;
    }

    MasterValuationTariffs MasterAssignmentStockContext::tariffs() const
    {
        return m_tariffs;
    }

    MasterStockSetupStatus MasterAssignmentStockContext::status() const
    {
        return m_status;
    }
}
