#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterassignmentevaluator.h"
#include "tests/arena/mastercomparisonprobe.h"

namespace ArenaMasterStockReference
{
    constexpr qsizetype MAXIMUM_REQUEST_BYTES = 65536;
    constexpr std::size_t MAXIMUM_PLANS = 8;
    constexpr std::size_t MAXIMUM_STEPS = 128;

    struct Step
    {
        std::size_t plan{0};
        Coordinator::MasterRefinementTerm term{Coordinator::MasterRefinementTerm::Friendly};
        std::int64_t allowance{0};
    };

    struct Request
    {
        QString sourceRevision;
        QString fixture;
        QString modelSHA256;
        QString requestSHA256;
        Coordinator::DecisionTraceIdentity identity;
        std::int64_t allowance{0};
        std::vector<std::int32_t> incumbent;
        std::vector<std::vector<std::int32_t>> family;
        std::vector<Step> steps;
    };

    bool enabled(const Coordinator::MasterShadowConfig & config, const QByteArray & stockReference);
    std::optional<Request> parse(
        const QByteArray & bytes, const Coordinator::DecisionTraceIdentity & identity,
        std::size_t actorCount, QString & error);
    bool matches(const Request & request, const Coordinator::MasterAssignmentProblem & problem);
    ArenaMasterComparisonProbe::StockReferenceResult record(
        const Request & request, const Coordinator::MasterAssignmentProblem & problem,
        Coordinator::MasterAssignmentEvaluator & evaluator, Coordinator::RefinementLedger & ledger,
        const QString & outputPath, Coordinator::MasterAssignmentProfile* profile = nullptr);
    ArenaMasterComparisonProbe::StockReferenceResult failure(
        const QString & outputPath, const QByteArray & requestBytes, const QString & reason,
        const Coordinator::RefinementLedgerStats* work = nullptr);
}
