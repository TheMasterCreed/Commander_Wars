#pragma once

#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/masterassignmentprofile.h"

namespace Coordinator
{
    struct MasterCaptureResult
    {
        MasterAssignmentBuildResult extraction;
        std::int64_t workUsed{0};
        bool budgetExhausted{false};
        bool arithmeticFailure{false};
    };

    MasterCaptureResult captureMasterAssignment(
        const AssignmentInput & input, const AssignmentResult & incumbent, std::uint64_t sessionIdentity,
        RefinementLedger & ledger, MasterAssignmentProfile* profile = nullptr);
}
