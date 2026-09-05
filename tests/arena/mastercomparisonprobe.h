#pragma once

namespace Coordinator
{
    struct AssignmentInput;
    struct AssignmentResult;
    class PropertyStockField;
    struct DecisionTraceIdentity;
}

namespace ArenaMasterComparisonProbe
{
    void record(const Coordinator::AssignmentInput & input,
                const Coordinator::AssignmentResult & incumbent,
                const Coordinator::PropertyStockField & field,
                const Coordinator::DecisionTraceIdentity & identity);
}
