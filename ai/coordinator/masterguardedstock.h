#pragma once

#include <QByteArray>

#include "ai/coordinator/guardedstockrelation.h"
#include "ai/coordinator/guardedstockfamily.h"
#include "ai/coordinator/masterassignmentstate.h"

namespace Coordinator
{
    struct MasterGuardedRootPreparation
    {
        GuardedRelationStatus status{GuardedRelationStatus::Invalid};
        std::optional<GuardedStockRoot> root;
        QByteArray authority;
        QByteArray consequences;
        std::optional<GuardedStockCurrentFamily> currentFamily;
    };

    class MasterGuardedRootSource
    {
    public:
        virtual ~MasterGuardedRootSource() = default;
        virtual MasterGuardedRootPreparation prepare(
            const MasterAssignmentState &, const MasterAssignmentReplayResult &, RefinementLedger &) = 0;
        virtual GuardedRelationStatus authenticate(RefinementLedger &) = 0;
        virtual std::optional<std::int64_t> maximumPreparationWork() const = 0;
        // Bounds the attempt's spending; budget exhaustion must refuse installation.
        virtual std::optional<std::int64_t> maximumAuthenticationWork() const = 0;
        virtual std::size_t maximumCurrentPrefixActions() const
        {
            return 0;
        }
    };

    class MasterGuardedStockOwner;

    class MasterGuardedPlanRoot final
    {
    public:
        const GuardedStockRoot & root() const;
        const MasterAssignmentState & state() const;
        const QByteArray & authority() const;
        const QByteArray & consequences() const;
        MilliFunds economic() const;
        const std::optional<GuardedStockCurrentFamily> & currentFamily() const;

    private:
        friend class MasterGuardedStockOwner;
        struct Context
        {
        };
        MasterGuardedPlanRoot(MasterAssignmentState state, MasterGuardedRootPreparation prepared,
                              MilliFunds economic, const std::shared_ptr<const Context> & context);

        MasterAssignmentState m_state;
        GuardedStockRoot m_root;
        QByteArray m_authority;
        QByteArray m_consequences;
        MilliFunds m_economic{0};
        std::optional<GuardedStockCurrentFamily> m_currentFamily;
        std::weak_ptr<const Context> m_context;
    };

    class MasterGuardedStockProof final
    {
    public:
        const MasterGuardedPlanRoot & incoming() const;
        const MasterGuardedPlanRoot & challenger() const;
        const GuardedStockRelation & relation() const;

    private:
        friend class MasterGuardedStockOwner;
        std::shared_ptr<const MasterGuardedPlanRoot> m_incoming;
        std::shared_ptr<const MasterGuardedPlanRoot> m_challenger;
        GuardedStockRelation m_relation;
    };

    struct MasterGuardedComparison
    {
        GuardedRelationStatus status{GuardedRelationStatus::Inapplicable};
        std::shared_ptr<const MasterGuardedStockProof> proof;

        bool improves() const;
    };

    class MasterGuardedStockOwner final
    {
    public:
        MasterGuardedStockOwner(const MasterAssignmentProblem &, RefinementLedger &,
                               std::unique_ptr<MasterGuardedRootSource>);
        MasterGuardedStockOwner(const MasterGuardedStockOwner &) = delete;
        MasterGuardedStockOwner & operator=(const MasterGuardedStockOwner &) = delete;

        std::shared_ptr<const MasterGuardedPlanRoot> prepare(
            const MasterAssignmentState &, const MasterAssignmentReplayResult &);
        MasterGuardedComparison compare(const std::shared_ptr<const MasterGuardedPlanRoot> & incoming,
                                        const std::shared_ptr<const MasterGuardedPlanRoot> & challenger);
        GuardedRelationStatus verify(const MasterGuardedStockProof &, bool fresh);
        bool matches(const MasterAssignmentProblem &, const RefinementLedger &) const;
        bool owns(const MasterGuardedPlanRoot &) const;
        bool admits(std::size_t actor, std::int32_t candidate) const;
        std::optional<std::int64_t> finalizationWork() const;
        GuardedRelationStatus lastStatus() const;

    private:
        const MasterAssignmentProblem & m_problem;
        MasterAssignmentIdentity m_identity;
        RefinementLedger & m_ledger;
        std::unique_ptr<MasterGuardedRootSource> m_source;
        std::shared_ptr<const MasterGuardedPlanRoot::Context> m_context;
        GuardedRelationStatus m_lastStatus{GuardedRelationStatus::Invalid};
    };
}
