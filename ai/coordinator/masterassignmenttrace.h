#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFile>

#include "ai/coordinator/masterassignmentsearch.h"

namespace Coordinator
{
    struct MasterShadowObservation;
    struct MasterShadowConfig;
    struct DecisionTraceIdentity;
    struct MasterNativeSourcePreparation;

    inline constexpr auto MASTER_OPERATION_TRACE_ENV = "COW_MASTER_OPERATION_TRACE";
    inline constexpr auto TRACE_NEIGHBOR_AUDIT = "neighborAudit";

    class MasterAssignmentSearchTrace final : public MasterAssignmentSearchObserver
    {
    public:
        MasterAssignmentSearchTrace(const QString & path, const DecisionTraceIdentity & identity,
                                    const MasterAssignmentProblem & problem, const RefinementLedger & ledger,
                                    std::int64_t reservedWork, const QString & attempt = {});
        void evaluated(const MasterAssignmentEvaluation &, std::int64_t) override;
        void refined(const MasterAssignmentEvaluation &, MasterRefinementTerm, std::int64_t,
                     MasterRefinementStatus, std::int64_t, std::int64_t) override;
        void compared(const MasterAssignmentEvaluation &, const RelativeStockComparison &, std::int64_t) override;
        void comparisonObserved(const MasterAssignmentEvaluation &, const MasterAssignmentEvaluation &,
                                const RelativeStockComparison &, std::int64_t) override;
        void guardedCompared(const MasterAssignmentEvaluation &, const MasterAssignmentEvaluation &,
                             const MasterGuardedComparison &, std::int64_t) override;
        void ticketStarted(const MasterRefinementTicketReceipt &, const MasterAssignmentEvaluation &,
                           const MasterAssignmentEvaluation*, std::int64_t) override;
        void operationOffered(const MasterAssignmentEvaluation &, const MasterOperationOffer &, bool, bool,
                              std::int64_t, std::int64_t, std::int64_t) override;
        void operationStarted(const MasterAssignmentEvaluation &, MasterRefinementTerm,
                              std::int64_t, std::int64_t) override;
        void operationExecuted(const MasterAssignmentEvaluation &, MasterRefinementTerm,
                               const MasterOperationOutcome &, std::int64_t, std::int64_t) override;
        void operationServiced(const MasterOperationTransaction &) override;
        void ticketFinished(const MasterRefinementTicketReceipt &) override;
        void finish(const QJsonObject & outcome);
        bool complete() const;
        QString path() const;

    private:
        enum class Event : std::int8_t
        {
            Begin,
            Evaluated,
            Compared,
            TicketStarted,
            Offered,
            OperationStarted,
            Executed,
            Serviced,
            TicketFinished,
            End,
            GuardedCompared,
        };
        void write(Event event, std::int64_t work, QJsonObject record);
        void failed();

        QFile m_output;
        const RefinementLedger & m_ledger;
        QElapsedTimer m_timer;
        QJsonObject m_identity;
        std::uint64_t m_sequence{0};
        std::uint64_t m_ticket{0};
        bool m_ticketActive{false};
        bool m_failed{false};
        bool m_finished{false};
    };

    std::unique_ptr<MasterAssignmentSearchTrace> attachMasterStockTrace(
        const MasterShadowConfig & config, const DecisionTraceIdentity & identity,
        const MasterAssignmentProblem & problem, const RefinementLedger & ledger,
        std::int64_t reservedWork, MasterAssignmentSearchOptions & options, const QString & attempt = {});

    bool masterShadowProofsMatch(const MasterShadowObservation & observation);
    QJsonValue masterValueRecord(std::optional<MilliFunds> value);
    QJsonArray masterChoicesRecord(std::span<const std::int32_t> choices);
    QJsonObject masterSearchRecord(const MasterAssignmentSearchResult & result);
    QJsonObject masterModelRecord(const MasterAssignmentProblem & problem);
    QJsonObject masterClaimsRecord(const ActionReservations & claims);
    QJsonObject masterEvaluationRecord(const MasterAssignmentEvaluation & evaluation);
    QJsonObject masterGuardedProofRecord(const MasterGuardedStockProof & proof);
    QJsonObject masterProfileRecord(const MasterAssignmentProfile & profile);
    QJsonObject masterWorkRecord(const RefinementLedgerStats & work);
    QJsonObject masterNativeSourceWorkRecord(const MasterNativeSourcePreparation & source);
    QJsonArray masterNativeCollectionCostsRecord(const MasterNativeSourcePreparation & source);
    QJsonObject masterRefusalRecord(const std::optional<RefinementRefusal> & refusal);
    QJsonObject masterRefinementRefusalRecord(const std::optional<MasterRefinementRefusal> & refusal);
    QJsonObject masterCertificatePreparationRecord(
        const std::optional<RelativeStockCertificatePreparation> & preparation);
}
