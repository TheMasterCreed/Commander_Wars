#pragma once

#include <array>
#include <optional>

#include <QString>

#include "ai/coordinator/masterguardedstock.h"

class GameMap;

namespace Coordinator
{
    enum class MasterNativeSourceStatus : std::int8_t
    {
        Unprepared,
        Ready,
        InvalidContext,
        UnsupportedInventory,
        UnsupportedCallbacks,
        UnsupportedCalendar,
        IncompleteDomain,
        BudgetExhausted,
        UnsupportedReceiver,
    };

    enum class MasterNativeCollectionStage : std::int8_t
    {
        Bookkeeping,
        Globals,
        Intrinsics,
        ValueConstructors,
        Inventory,
        Calendar,
        Enums,
        Receivers,
        Actors,
        Co,
        Actions,
        Properties,
        Construction,
        Movement,
        Context,
        Terrain,
        Policy,
        Fire,
        Count,
    };

    struct MasterNativeCollectionCost
    {
        std::size_t observations{0};
        std::int64_t work{0};
        std::int64_t expectedConstructionWork{0};
        std::int64_t receiverConstructionWork{0};
    };

    using MasterNativeCollectionCosts = std::array<MasterNativeCollectionCost,
        static_cast<std::size_t>(MasterNativeCollectionStage::Count)>;

    enum class GuardedNativeCodeKind : std::int8_t;
    enum class GuardedNativeBindingStatus : std::int8_t;

    struct MasterNativeCallbackRefusal
    {
        GuardedNativeCodeKind module{};
        GuardedNativeBindingStatus status{};
        QString lastCheckedMember;
        std::optional<GuardedNativeBindingStatus> lastCheckedStatus;
    };

    struct MasterNativeSourcePreparation
    {
        MasterNativeSourceStatus status{MasterNativeSourceStatus::Unprepared};
        std::unique_ptr<MasterGuardedRootSource> source;
        std::int64_t work{0};
        std::size_t boundModules{0};
        std::uint16_t commandCallbacks{0};
        bool detailPresentation{false};
        std::int64_t collectionWork{0};
        std::int64_t expectedConstructionWork{0};
        std::int64_t receiverConstructionWork{0};
        std::int64_t commonCollectionWork{0};
        std::optional<std::int64_t> preparationMaximumWork;
        std::optional<std::int64_t> authenticationMaximumWork;
        std::int64_t commandAuthenticationAllowance{0};
        MasterNativeCollectionCosts collectionCosts{};
        std::optional<MasterNativeCallbackRefusal> callbackRefusal;
    };

    struct GuardedNativePlanBounds;

    MasterNativeSourcePreparation prepareMasterNativeSource(
        GameMap &, const MasterAssignmentProblem &, RefinementLedger &);
    std::optional<std::int64_t> masterNativeRootPreparationWork(const GuardedNativePlanBounds &);
    QString masterNativeSourceStatusName(MasterNativeSourceStatus);
    QString masterNativeCollectionStageName(MasterNativeCollectionStage);
}
