#pragma once

#include <QJsonArray>
#include <QJsonObject>

#include "ai/coordinator/masterassignmentsearch.h"

namespace Coordinator
{
    QJsonValue masterValueRecord(std::optional<MilliFunds> value);
    QJsonArray masterChoicesRecord(std::span<const std::int32_t> choices);
    QJsonObject masterSearchRecord(const MasterAssignmentSearchResult & result);
    QJsonObject masterModelRecord(const MasterAssignmentProblem & problem);
    QJsonObject masterEvaluationRecord(const MasterAssignmentEvaluation & evaluation);
    QJsonObject masterProfileRecord(const MasterAssignmentProfile & profile);
    QJsonObject masterWorkRecord(const RefinementLedgerStats & work);
}
