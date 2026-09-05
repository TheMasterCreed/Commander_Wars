#pragma once

namespace Coordinator
{
    class MasterAssignmentProblem;
    struct DecisionTraceIdentity;
}

namespace ArenaMasterAssignmentOracleProbe
{
    void record(const Coordinator::MasterAssignmentProblem & problem,
                const Coordinator::DecisionTraceIdentity & identity);
}
