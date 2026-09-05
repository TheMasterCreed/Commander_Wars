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
                                       RefinementLedger & ledger, MasterAssignmentProfile* profile = nullptr);
        RelativeStockSession* session();
        MasterValuationTariffs tariffs() const;
        MasterStockSetupStatus status() const;

    private:
        bool copyColdField(const PropertyStockField & source, RefinementLedger & ledger);
        bool prepareTariffs(const RelativeStockSession & session, RefinementLedger & ledger);

        PropertyStockField m_field;
        std::unique_ptr<JointPlanStockValuer> m_valuer;
        MasterValuationTariffs m_tariffs;
        MasterStockSetupStatus m_status{MasterStockSetupStatus::Unprepared};
    };
}
