#pragma once

#include <QByteArray>
#include <QString>

#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/masterassignmentcapture.h"
#include "ai/coordinator/masterassignmentsearch.h"
#include "ai/coordinator/propertystocksequential.h"

class QJsonObject;

namespace Coordinator
{
    enum class MasterStockSetupStatus : std::int8_t;

    enum class MasterAssignmentMode : std::int8_t
    {
        Disabled,
        Shadow,
        StockControl,
    };

    enum class MasterStockControlRuntime : std::int8_t
    {
        None,
        Arena,
        PlayableTrial,
        Play,
    };

    enum class MasterShadowConfigError : std::int8_t
    {
        None,
        UnknownMode,
        InvalidAllowance,
        MissingAbsoluteOutput,
        InvalidTierAudit,
        InvalidExport,
        MissingExplicitAllowance,
        StockControlOutsideArena,
        InvalidTrialUserPath,
        UnsupportedTrialProcess,
        InvalidStaleProbe,
    };

    // Playable-trial only: changes one authenticated live fact between search and finalization.
    enum class MasterStaleProbe : std::int8_t
    {
        None,
        BattleAnimationSpeed,
    };

    enum class MasterShadowExportMode : std::int8_t
    {
        Full,
        Off,
    };

    enum class MasterShadowOutcome : std::int8_t
    {
        MasterCertifiedBetter,
        LegacyCertifiedBetter,
        ExactEquality,
        Unresolved,
        UnsupportedDomain,
        RepresentationMismatch,
        ReplayFailure,
    };

    struct MasterShadowConfig
    {
        MasterAssignmentMode mode{MasterAssignmentMode::Disabled};
        MasterStockControlRuntime runtime{MasterStockControlRuntime::None};
        std::int64_t allowance{MASTER_ASSIGNMENT_INITIAL_ALLOWANCE};
        QString outputPath;
        MasterShadowConfigError error{MasterShadowConfigError::None};
        SequentialTierAuditMode tierAuditMode{SequentialTierAuditMode::Full};
        MasterShadowExportMode exportMode{MasterShadowExportMode::Full};
        MasterStaleProbe staleProbe{MasterStaleProbe::None};
        MasterPlanNeighborAudit planNeighborAudit{MasterPlanNeighborAudit::Off};

        bool enabled() const
        {
            return error == MasterShadowConfigError::None && mode == MasterAssignmentMode::Shadow;
        }

        bool controlsStock() const
        {
            return error == MasterShadowConfigError::None && mode == MasterAssignmentMode::StockControl;
        }
    };

    struct MasterShadowTiming
    {
        std::int64_t totalNanos{0};
        std::int64_t computationNanos{0};
        std::int64_t recordNanos{0};
        std::int64_t encodingNanos{0};
        std::int64_t outputNanos{0};
        std::int64_t inspectionNanos{0};
        std::int64_t exportBytes{0};
        std::int32_t exportRecords{0};
        std::array<MasterPhaseSample, MASTER_PHASE_NAMES.size()> phases{};
        MasterShadowOutcome outcome{MasterShadowOutcome::Unresolved};
        std::optional<std::int64_t> firstImprovementWork;
        std::optional<std::int64_t> firstImprovementNanos;
        std::optional<MasterRefinementRefusal> refinementRefusal;
        std::optional<MilliFunds> improvementLower;
        std::optional<MilliFunds> gap;
        bool legacyPlanUnchanged{true};
        bool legacyLedgerUnchanged{true};
        bool exportSucceeded{true};
    };

    struct MasterShadowObservation
    {
        const MasterShadowConfig & config;
        const DecisionTraceIdentity & identity;
        const MasterAssignmentProfile & profile;
        const RefinementLedgerStats & legacyBefore;
        const RefinementLedgerStats & legacyAfter;
        const RefinementLedger & masterLedger;
        const MasterCaptureResult* capture{nullptr};
        const MasterAssignmentSearchResult* search{nullptr};
        const MasterFutureCatalog* future{nullptr};
        const RelativeStockSession* session{nullptr};
        const MasterAssignmentEvaluator* evaluator{nullptr};
        std::optional<MasterStockSetupStatus> stockSetup;
        MasterShadowOutcome outcome{MasterShadowOutcome::Unresolved};
        bool legacyPlanUnchanged{true};
    };

    class MasterShadowInspection
    {
    public:
        virtual ~MasterShadowInspection() = default;
        // The view is valid only during this call, while both model identities remain live.
        virtual void completed(const MasterShadowObservation & observation) = 0;
    };

    QString masterShadowOutcomeName(MasterShadowOutcome outcome);
    QJsonObject masterShadowRecord(const MasterShadowObservation & observation);
    bool isolatedMasterTrialPath(const QString & applicationDirectory, const QString & explicitUserPath,
                                 const QString & activeUserPath);
    MasterShadowConfig parseMasterShadowConfig(const QByteArray & mode, const QByteArray & work, const QString & output,
                                               const QByteArray & tierAudit = {}, const QByteArray & exportMode = {},
                                               MasterStockControlRuntime runtime = MasterStockControlRuntime::None);
    std::optional<MasterStaleProbe> parseMasterStaleProbe(const QByteArray & probe);
    MasterPlanNeighborAudit parseMasterPlanNeighborAudit(const QByteArray & value, MasterStockControlRuntime runtime);
    MasterShadowConfig readMasterShadowConfig();
    MasterShadowConfig playMasterStockControlConfig();
    void prepareMasterShadow(const MasterShadowConfig & config, AssignmentInput & input, RefinementLedger & ledger);
    MasterShadowTiming observeMasterAssignment(const MasterShadowConfig & config, const AssignmentInput & input,
                                        const AssignmentResult & incumbent, const PropertyStockField & field,
                                        const RefinementLedger & legacyLedger, RefinementLedger & masterLedger,
                                        const DecisionTraceIdentity & identity, MasterShadowInspection* inspection = nullptr);
}
