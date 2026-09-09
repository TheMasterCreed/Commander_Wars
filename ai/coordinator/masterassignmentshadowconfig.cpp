#include "ai/coordinator/masterassignmentshadow.h"

#include <QDebug>
#include <QDir>

#include "ai/coordinator/bundleassignment.h"

namespace
{
    constexpr auto MODE_VARIABLE = "COW_MASTER_ASSIGNMENT_MODE";
    constexpr auto WORK_VARIABLE = "COW_MASTER_ASSIGNMENT_WORK";
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_ASSIGNMENT_OUTPUT";
    constexpr auto TIER_AUDIT_VARIABLE = "COW_MASTER_ASSIGNMENT_TIER_AUDIT";
    constexpr auto EXPORT_VARIABLE = "COW_MASTER_ASSIGNMENT_EXPORT";
}

namespace Coordinator
{
    MasterShadowConfig parseMasterShadowConfig(
        const QByteArray & mode, const QByteArray & work, const QString & output, const QByteArray & tierAudit,
        const QByteArray & exportMode)
    {
        MasterShadowConfig result;
        if (mode.isEmpty() || mode == "disabled")
        {
            return result;
        }
        if (mode != "shadow")
        {
            result.error = MasterShadowConfigError::UnknownMode;
            return result;
        }
        result.mode = MasterAssignmentMode::Shadow;
        if (!work.isEmpty())
        {
            bool valid = false;
            result.allowance = work.toLongLong(&valid);
            if (!valid || result.allowance < 0)
            {
                result.error = MasterShadowConfigError::InvalidAllowance;
                return result;
            }
        }
        if (tierAudit == "counters")
        {
            result.tierAuditMode = SequentialTierAuditMode::CountersOnly;
        }
        else if (!tierAudit.isEmpty() && tierAudit != "full")
        {
            result.error = MasterShadowConfigError::InvalidTierAudit;
            return result;
        }
        if (exportMode == "off")
        {
            result.exportMode = MasterShadowExportMode::Off;
        }
        else if (!exportMode.isEmpty() && exportMode != "full")
        {
            result.error = MasterShadowConfigError::InvalidExport;
            return result;
        }
        if (result.exportMode == MasterShadowExportMode::Full &&
            (output.isEmpty() || !QDir::isAbsolutePath(output)))
        {
            result.error = MasterShadowConfigError::MissingAbsoluteOutput;
            return result;
        }
        result.outputPath = output;
        return result;
    }

    MasterShadowConfig readMasterShadowConfig()
    {
        const auto result = parseMasterShadowConfig(qgetenv(MODE_VARIABLE), qgetenv(WORK_VARIABLE),
            QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE)), qgetenv(TIER_AUDIT_VARIABLE), qgetenv(EXPORT_VARIABLE));
        if (result.error != MasterShadowConfigError::None)
        {
            qWarning() << "Master assignment shadow configuration rejected:" << static_cast<int>(result.error);
        }
        return result;
    }

    void prepareMasterShadow(const MasterShadowConfig & config, AssignmentInput & input, RefinementLedger & ledger)
    {
        if (config.enabled() && ledger.open(config.allowance) &&
            chargeMasterWork(ledger, input.actors.size() + 1, 1, RefinementWork::MasterPreparation))
        {
            input.retainSelections = true;
        }
    }
}
