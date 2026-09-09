#pragma once

#include <memory>

class QJsonObject;

namespace Coordinator
{
    struct AssignmentInput;
    struct AssignmentResult;
    class PropertyStockField;
    struct DecisionTraceIdentity;
    struct MasterShadowObservation;
    class MasterShadowInspection;
}

namespace ArenaMasterAssignmentProbe
{
    bool enabled();
    std::unique_ptr<Coordinator::MasterShadowInspection> openShadowInspection();
    QJsonObject inspectShadow(const Coordinator::MasterShadowObservation & observation);
    void record(const Coordinator::AssignmentInput & input,
                const Coordinator::AssignmentResult & incumbent,
                const Coordinator::PropertyStockField & field,
                const Coordinator::DecisionTraceIdentity & identity);
}
