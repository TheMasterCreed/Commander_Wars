#pragma once

#include "ai/coordinator/masterassignmentevaluator.h"
#include "ai/coordinator/propertystockbuilder.h"

namespace Coordinator
{
    enum class MasterStockSetupStatus : std::int8_t
    {
        Unprepared,
        Prepared,
        BudgetExhausted,
        Invalid,
        ArithmeticFailure,
    };

    class MasterAssignmentStockContext final
    {
    public:
        MasterAssignmentStockContext() = default;
        MasterAssignmentStockContext(const MasterAssignmentStockContext &) = delete;
        MasterAssignmentStockContext & operator=(const MasterAssignmentStockContext &) = delete;

        MasterStockSetupStatus prepare(const PropertyStockField & source, const AssignmentInput & input,
                                       RefinementLedger & ledger, MasterAssignmentProfile* profile = nullptr,
                                       SequentialTierAuditMode auditMode = SequentialTierAuditMode::Full);
        RelativeStockSession* session();
        MasterValuationTariffs tariffs() const;
        MasterStockSetupStatus status() const;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        const DiagnosticWarmStart* diagnosticLastWarmStart() const
        {
            return m_valuer ? m_valuer->diagnosticLastWarmStart() : nullptr;
        }
#endif

    private:
        bool copyColdField(const PropertyStockField & source, RefinementLedger & ledger);
        bool prepareTariffs(const RelativeStockSession & session, RefinementLedger & ledger);

        PropertyStockField m_field;
        std::unique_ptr<JointPlanStockValuer> m_valuer;
        MasterValuationTariffs m_tariffs;
        MasterStockSetupStatus m_status{MasterStockSetupStatus::Unprepared};
    };
}
