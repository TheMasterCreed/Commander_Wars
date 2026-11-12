#pragma once

#include <cstdint>

class QString;

namespace Coordinator
{
    struct AssignmentInput;
    struct AssignmentResult;
    struct MasterShadowConfig;
    class PropertyStockField;
    struct DecisionTraceIdentity;
}

namespace ArenaMasterComparisonProbe
{
    enum class StockReferenceResult : std::int8_t
    {
        NotRequested,
        Complete,
        Failed,
    };

    QString stockReferenceStopReason(StockReferenceResult result);
    StockReferenceResult recordStockReference(
        const Coordinator::MasterShadowConfig & config,
        const Coordinator::AssignmentInput & input,
        const Coordinator::AssignmentResult & incumbent,
        const Coordinator::PropertyStockField & field,
        const Coordinator::DecisionTraceIdentity & identity);

    void record(const Coordinator::AssignmentInput & input,
                const Coordinator::AssignmentResult & incumbent,
                const Coordinator::PropertyStockField & field,
                const Coordinator::DecisionTraceIdentity & identity);
}
