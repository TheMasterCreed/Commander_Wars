#pragma once

#include <QByteArray>
#include <QString>

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterassignmentsearch.h"

namespace Coordinator
{
    enum class MasterAssignmentMode : std::int8_t
    {
        Disabled,
        Shadow,
    };

    enum class MasterShadowConfigError : std::int8_t
    {
        None,
        UnknownMode,
        InvalidAllowance,
        MissingAbsoluteOutput,
    };

    struct MasterShadowConfig
    {
        MasterAssignmentMode mode{MasterAssignmentMode::Disabled};
        std::int64_t allowance{MASTER_ASSIGNMENT_INITIAL_ALLOWANCE};
        QString outputPath;
        MasterShadowConfigError error{MasterShadowConfigError::None};

        bool enabled() const
        {
            return error == MasterShadowConfigError::None && mode == MasterAssignmentMode::Shadow;
        }
    };

    MasterShadowConfig parseMasterShadowConfig(const QByteArray & mode, const QByteArray & work, const QString & output);
    MasterShadowConfig readMasterShadowConfig();
    void prepareMasterShadow(const MasterShadowConfig & config, AssignmentInput & input, RefinementLedger & ledger);
    std::int64_t observeMasterAssignment(const MasterShadowConfig & config, const AssignmentInput & input,
                                        const AssignmentResult & incumbent, const PropertyStockField & field,
                                        const RefinementLedger & legacyLedger, RefinementLedger & masterLedger,
                                        const DecisionTraceIdentity & identity);
}
