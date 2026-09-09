#pragma once

#include "ai/coordinator/masterassignmentsearch.h"

namespace Coordinator
{
    struct AssignmentResult;

    enum class MasterInstallationStatus : std::int8_t
    {
        Unprepared,
        Reserved,
        Installed,
        NoImprovement,
        InvalidContext,
        IncomingChanged,
        ReplayFailure,
        ProofFailure,
        NonImprovement,
        ArithmeticFailure,
        BudgetExhausted,
    };

    struct MasterInstallationResult
    {
        MasterInstallationStatus status{MasterInstallationStatus::Unprepared};
        std::int64_t reservedWork{0};
        std::int64_t finalizationWork{0};
        std::optional<MasterImprovementProof> proof;
    };

    class MasterAssignmentInstallation final
    {
    public:
        MasterAssignmentInstallation(const MasterAssignmentProblem & problem,
                                     MasterAssignmentEvaluator & evaluator, RefinementLedger & ledger);
        MasterAssignmentInstallation(const MasterAssignmentInstallation &) = delete;
        MasterAssignmentInstallation & operator=(const MasterAssignmentInstallation &) = delete;

        MasterInstallationStatus prepare(const AssignmentResult & incoming);
        MasterInstallationResult finish(const std::optional<MasterImprovementProof> & retained,
                                        AssignmentResult & incoming);
        std::int64_t reservedWork() const;

    private:
        bool matchesIncoming(const AssignmentResult & incoming,
                             const MasterAssignmentEvaluation & evaluated) const;

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentIdentity m_identity;
        MasterAssignmentEvaluator & m_evaluator;
        RefinementLedger & m_ledger;
        MasterInstallationStatus m_status{MasterInstallationStatus::Unprepared};
        GrantId m_reservation{NO_GRANT};
        std::int64_t m_reservedWork{0};
        std::int64_t m_publicationWork{0};
        std::size_t m_incomingDiagnosticValues{0};
    };
}
