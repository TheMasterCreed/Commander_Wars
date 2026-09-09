#pragma once

#include "ai/coordinator/masterassignmentinstallation.h"
#include "ai/coordinator/masterassignmentshadow.h"

namespace Coordinator
{
    enum class MasterStockControlStatus : std::int8_t
    {
        Disabled,
        ConstructorRefused,
        ExtractionRefused,
        StockPreparationRefused,
        FinalizationRefused,
        Retained,
        Installed,
    };

    struct MasterStockControlTiming
    {
        MasterStockControlStatus status{MasterStockControlStatus::Disabled};
        MasterInstallationStatus installationStatus{MasterInstallationStatus::Unprepared};
        std::int64_t totalNanos{0};
        std::int64_t constructorNanos{0};
        std::int64_t searchNanos{0};
        std::int64_t finalizationNanos{0};
        std::int64_t exportNanos{0};
        RefinementLedgerStats work;
        std::int64_t reservedWork{0};
        std::int64_t finalizationWork{0};
        std::optional<std::int64_t> firstImprovementWork;
        std::optional<std::int64_t> firstImprovementNanos;
        std::optional<MilliFunds> firstImprovementLower;
        std::optional<MilliFunds> improvementLower;
        std::optional<MilliFunds> gap;
        bool constructorComplete{false};
        bool exportSucceeded{true};
    };

    QString masterStockControlStatusName(MasterStockControlStatus status);
    MasterStockControlTiming controlMasterAssignment(
        const MasterShadowConfig & config, const AssignmentInput & input, AssignmentResult & assignment,
        const PropertyStockField & field, const DecisionTraceIdentity & identity);
}
