#include "ai/coordinator/masterassignmentshadow.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include "ai/coordinator/bundleassignment.h"

#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
#include <QCoreApplication>
#include "coreengine/mainapp.h"
#include "coreengine/settings.h"
#endif

namespace
{
    constexpr auto MODE_VARIABLE = "COW_MASTER_ASSIGNMENT_MODE";
    constexpr auto WORK_VARIABLE = "COW_MASTER_ASSIGNMENT_WORK";
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_ASSIGNMENT_OUTPUT";
    constexpr auto TIER_AUDIT_VARIABLE = "COW_MASTER_ASSIGNMENT_TIER_AUDIT";
    constexpr auto EXPORT_VARIABLE = "COW_MASTER_ASSIGNMENT_EXPORT";
    constexpr auto NEIGHBOR_AUDIT_VARIABLE = "COW_MASTER_PLAN_NEIGHBOR_AUDIT";
    constexpr auto NEIGHBOR_AUDIT_RECORDED = "1";
    constexpr auto STOCK_CONTROL_MODE = "stockControl";
    constexpr auto BATTLE_ANIMATION_SPEED_PROBE = "battleAnimationSpeed";
    constexpr std::int64_t PLAY_STOCK_CONTROL_ALLOWANCE = 60000000;
}

namespace Coordinator
{
    std::optional<MasterStaleProbe> parseMasterStaleProbe(const QByteArray & probe)
    {
        if (probe.isEmpty())
        {
            return MasterStaleProbe::None;
        }
        if (probe == BATTLE_ANIMATION_SPEED_PROBE)
        {
            return MasterStaleProbe::BattleAnimationSpeed;
        }
        return std::nullopt;
    }

    MasterPlanNeighborAudit parseMasterPlanNeighborAudit(const QByteArray & value, MasterStockControlRuntime runtime)
    {
        const bool measuredRuntime = runtime == MasterStockControlRuntime::Arena ||
                                     runtime == MasterStockControlRuntime::PlayableTrial;
        return measuredRuntime && value == NEIGHBOR_AUDIT_RECORDED ? MasterPlanNeighborAudit::Recorded
                                                                   : MasterPlanNeighborAudit::Off;
    }

    bool isolatedMasterTrialPath(const QString & applicationDirectory, const QString & explicitUserPath,
                                 const QString & activeUserPath)
    {
        if (!QDir::isAbsolutePath(applicationDirectory) || !QDir::isAbsolutePath(explicitUserPath) ||
            !QDir::isAbsolutePath(activeUserPath))
        {
            return false;
        }
        const auto application = QFileInfo(applicationDirectory).canonicalFilePath();
        const auto requested = QFileInfo(explicitUserPath).canonicalFilePath();
        const auto active = QFileInfo(activeUserPath).canonicalFilePath();
        if (application.isEmpty() || requested.isEmpty() || active.isEmpty() ||
            !QFileInfo(active).isDir() || requested != active)
        {
            return false;
        }
        const auto relative = QDir(application).relativeFilePath(active);
        return relative != "." && relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
    }

    MasterShadowConfig parseMasterShadowConfig(
        const QByteArray & mode, const QByteArray & work, const QString & output, const QByteArray & tierAudit,
        const QByteArray & exportMode, MasterStockControlRuntime runtime)
    {
        MasterShadowConfig result;
        result.runtime = runtime;
        if (mode.isEmpty() || mode == "disabled")
        {
            return result;
        }
        if (mode != "shadow" && mode != STOCK_CONTROL_MODE)
        {
            result.error = MasterShadowConfigError::UnknownMode;
            return result;
        }
        result.mode = mode == STOCK_CONTROL_MODE ? MasterAssignmentMode::StockControl : MasterAssignmentMode::Shadow;
        if (result.controlsStock() && runtime == MasterStockControlRuntime::None)
        {
            result.error = MasterShadowConfigError::StockControlOutsideArena;
            return result;
        }
        if (result.controlsStock() && work.isEmpty())
        {
            result.error = MasterShadowConfigError::MissingExplicitAllowance;
            return result;
        }
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
        auto runtime = MasterStockControlRuntime::None;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (qgetenv("COW_AI_ARENA_TEST") == "1")
        {
            runtime = MasterStockControlRuntime::Arena;
        }
#endif
#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
        constexpr auto TRIAL_VARIABLE = "COW_MASTER_ASSIGNMENT_TRIAL";
        constexpr auto STALE_PROBE_VARIABLE = "COW_MASTER_ASSIGNMENT_STALE_PROBE";
        if (qgetenv(TRIAL_VARIABLE) == "1")
        {
            runtime = MasterStockControlRuntime::PlayableTrial;
        }
#endif
        auto result = parseMasterShadowConfig(qgetenv(MODE_VARIABLE), qgetenv(WORK_VARIABLE),
            QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE)), qgetenv(TIER_AUDIT_VARIABLE),
            qgetenv(EXPORT_VARIABLE), runtime);
        result.planNeighborAudit = parseMasterPlanNeighborAudit(qgetenv(NEIGHBOR_AUDIT_VARIABLE), runtime);
#ifdef COW_MASTER_ASSIGNMENT_PLAYABLE_TRIAL
        if (result.controlsStock() && runtime == MasterStockControlRuntime::PlayableTrial)
        {
            QString explicitUserPath;
            auto* settings = Settings::getInstance();
            if (!Mainapp::getInstance()->getParser().getUserPath(explicitUserPath) ||
                !isolatedMasterTrialPath(QCoreApplication::applicationDirPath(), explicitUserPath, settings->getUserPath()))
            {
                result.error = MasterShadowConfigError::InvalidTrialUserPath;
            }
            else if (settings->getSpawnAiProcess() || settings->getAiSlave() || Mainapp::getSlave())
            {
                result.error = MasterShadowConfigError::UnsupportedTrialProcess;
            }
            const auto probe = parseMasterStaleProbe(qgetenv(STALE_PROBE_VARIABLE));
            if (probe)
            {
                result.staleProbe = *probe;
            }
            else if (result.error == MasterShadowConfigError::None)
            {
                result.error = MasterShadowConfigError::InvalidStaleProbe;
            }
        }
#endif
        if (result.error != MasterShadowConfigError::None)
        {
            qWarning() << "Master assignment configuration rejected:" << static_cast<int>(result.error);
        }
        return result;
    }

    MasterShadowConfig playMasterStockControlConfig()
    {
        MasterShadowConfig result;
        result.mode = MasterAssignmentMode::StockControl;
        result.runtime = MasterStockControlRuntime::Play;
        result.allowance = PLAY_STOCK_CONTROL_ALLOWANCE;
        result.tierAuditMode = SequentialTierAuditMode::CountersOnly;
        result.exportMode = MasterShadowExportMode::Off;
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
