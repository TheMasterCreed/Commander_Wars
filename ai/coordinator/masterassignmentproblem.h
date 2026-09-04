#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/planidentity.h"
#include "ai/coordinator/propertystock.h"
#include "ai/coordinator/turnplan.h"

namespace Coordinator
{
    struct AssignmentInput;
    struct AssignmentResult;
    struct MasterAssignmentBuildResult;
    struct MasterAssignmentBuildRequest;
    struct MasterAssignmentReplayResult;

    enum class MasterActorRole : std::int8_t
    {
        Fixed,
        Mutable,
    };

    enum class MasterResourceKind : std::int8_t
    {
        Destination,
        Capture,
        Service,
        DamageTarget,
    };

    struct MasterResourceKey
    {
        MasterResourceKind kind{MasterResourceKind::Destination};
        TilePoint tile{INVALID_TILE};
        std::int32_t unitId{NO_UNIT};

        friend bool operator==(const MasterResourceKey &,
                               const MasterResourceKey &) = default;
    };

    struct MasterResourceUsage
    {
        std::int32_t resourceIndex{-1};
        std::int32_t demand{0};
        std::int32_t capacity{0};

        friend bool operator==(const MasterResourceUsage &,
                               const MasterResourceUsage &) = default;
    };

    struct MasterCandidateIdentity
    {
        std::int32_t actorSlot{-1};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t originalCandidateIndex{-1};
        std::int32_t generationIndex{-1};

        friend bool operator==(const MasterCandidateIdentity &,
                               const MasterCandidateIdentity &) = default;
    };

    struct MasterVacateRequirement
    {
        TilePoint tile{INVALID_TILE};
        std::int32_t occupantUnitId{NO_UNIT};

        friend bool operator==(const MasterVacateRequirement &,
                               const MasterVacateRequirement &) = default;
    };

    struct MasterCaptureConsequence
    {
        PropertyIncome income{};
        OwnerSign ownerBefore{OwnerSign::Neutral};
        OwnerSign ownerAfter{OwnerSign::Ours};
        std::int32_t turnsUntilOwned{NO_CAPTURE_TURNS};

        friend bool operator==(const MasterCaptureConsequence &,
                               const MasterCaptureConsequence &) = default;
    };

    struct MasterCandidate
    {
        MasterCandidateIdentity identity;
        PlanBundleKind actionKind{PlanBundleKind::Wait};
        TilePoint origin{INVALID_TILE};
        TilePoint destination{INVALID_TILE};
        TilePoint target{INVALID_TILE};
        std::int32_t targetUnitId{NO_UNIT};
        TerminalValue immediateValue{};
        std::vector<MasterResourceUsage> resources;
        std::vector<MasterVacateRequirement> dependencies;
        std::optional<MasterCaptureConsequence> capture;
        std::int32_t stockRow{NO_STOCK_ROW};
        TilePoint stockEndpoint{INVALID_TILE};
        bool plannable{false};
        CandidateBundle source;
    };

    struct MasterActor
    {
        std::int32_t canonicalSlot{-1};
        std::int32_t knowledgeUnitIndex{NO_UNIT};
        std::int32_t engineUnitId{NO_UNIT};
        MasterActorRole role{MasterActorRole::Fixed};
        std::int32_t stockRow{NO_STOCK_ROW};
        std::int32_t incumbentCandidateIndex{NO_CANDIDATE};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t incumbentDamageSteps{0};
        ActionReservations incumbentReservations;
        MilliFunds incumbentSeatedValue{0};
        std::vector<MasterCandidate> candidates;
        std::vector<std::int32_t> canonicalCandidates;
    };

    struct MasterCandidateRef
    {
        std::int32_t actorSlot{-1};
        std::int32_t candidateIndex{NO_CANDIDATE};

        friend bool operator==(const MasterCandidateRef &,
                               const MasterCandidateRef &) = default;
    };

    struct MasterFixedResourceUsage
    {
        MasterCandidateRef candidate;
        MasterResourceUsage usage;

        friend bool operator==(const MasterFixedResourceUsage &,
                               const MasterFixedResourceUsage &) = default;
    };

    class MasterAssignmentIdentity final
    {
    public:
        MasterAssignmentIdentity() = default;

        bool established() const;
        std::uint64_t session() const;
        bool sameLiveProblem(const MasterAssignmentIdentity & other) const;

    private:
        friend class MasterAssignmentProblem;

        struct Binding
        {
            std::uint64_t session{0};
        };

        explicit MasterAssignmentIdentity(
            const std::shared_ptr<const Binding> & binding);

        std::weak_ptr<const Binding> m_binding;
    };

    class MasterAssignmentProblem final
    {
    public:
        MasterAssignmentProblem(const MasterAssignmentProblem &) = default;
        MasterAssignmentProblem(MasterAssignmentProblem &&) noexcept = default;
        MasterAssignmentProblem & operator=(
            const MasterAssignmentProblem &) = default;
        MasterAssignmentProblem & operator=(
            MasterAssignmentProblem &&) noexcept = default;

        MasterAssignmentIdentity identity() const;
        const PlanActionIds & actionIds() const;
        std::span<const KnownUnitLink> unitLinks() const;
        std::span<const MasterActor> actors() const;
        std::span<const MasterResourceKey> resources() const;
        std::span<const MasterFixedResourceUsage> fixedResourceUsage() const;
        bool incompatible(MasterCandidateRef left, MasterCandidateRef right) const;
        bool canChangeActor(std::int32_t actorSlot) const;
        std::span<const OccupancyEntry> occupancy() const;
        std::span<const PlannedAction> actionTemplates() const;
        std::span<const PlanEdge> incumbentEdges() const;
        std::span<const std::int32_t> incumbentChoices() const;
        std::span<const std::int32_t> incumbentExecutionOrder() const;
        MilliFunds incumbentEconomicTotal() const;
        const ContinuationKey & incumbentContinuationKey() const;

    private:
        friend struct MasterAssignmentBuildResult;
        friend MasterAssignmentBuildResult buildMasterAssignmentProblem(
            const AssignmentInput &,
            const struct MasterAssignmentBuildRequest &);
        friend struct MasterAssignmentReplayResult replayMasterAssignment(
            const MasterAssignmentProblem &,
            std::span<const std::int32_t>);

        explicit MasterAssignmentProblem(std::uint64_t sessionIdentity);

        std::shared_ptr<const MasterAssignmentIdentity::Binding> m_identity;
        PlanActionIds m_actionIds;
        std::vector<KnownUnitLink> m_unitLinks;
        std::vector<MasterActor> m_actors;
        std::vector<MasterResourceKey> m_resources;
        std::vector<MasterFixedResourceUsage> m_fixedResourceUsage;
        std::vector<OccupancyEntry> m_occupancy;
        std::vector<PlannedAction> m_actionTemplates;
        std::vector<PlanEdge> m_incumbentEdges;
        std::vector<std::int32_t> m_incumbentChoices;
        std::vector<std::int32_t> m_incumbentExecutionOrder;
        MilliFunds m_incumbentEconomicTotal{0};
        ContinuationKey m_incumbentContinuationKey;
    };

    struct MasterActorSelection
    {
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t candidateIndex{NO_CANDIDATE};
        MasterActorRole role{MasterActorRole::Fixed};
        std::int32_t stockRow{NO_STOCK_ROW};
        std::optional<MilliFunds> seatedValue;
    };

    struct MasterAssignmentBuildRequest
    {
        std::uint64_t sessionIdentity{0};
        std::span<const MasterActorSelection> selections;
        const TurnPlan* pIncumbentPlan{nullptr};
        std::span<const std::int32_t> incumbentExecutionOrder;
    };

    enum class MasterAssignmentProblemError : std::int8_t
    {
        None,
        InvalidSession,
        MissingIncumbent,
        ActorCountMismatch,
        DuplicateActor,
        MissingActorSelection,
        DuplicateAction,
        InvalidActor,
        EmptyCandidateDomain,
        CandidateActorMismatch,
        InvalidCandidate,
        InvalidSelection,
        InvalidAction,
        IncumbentActionMismatch,
        IncumbentClaimMismatch,
        IncumbentOrderMismatch,
        IncumbentEconomicMismatch,
        InvalidStockIdentity,
    };

    struct MasterAssignmentBuildResult
    {
        std::optional<MasterAssignmentProblem> problem;
        MasterAssignmentProblemError error{
            MasterAssignmentProblemError::MissingIncumbent};
        std::int32_t actorSlot{-1};
        std::int32_t candidateIndex{NO_CANDIDATE};
        std::int32_t actionIndex{NO_ACTION};

        bool valid() const
        {
            return error == MasterAssignmentProblemError::None &&
                   problem.has_value();
        }
    };

    enum class MasterAssignmentReplayError : std::int8_t
    {
        None,
        ChoiceCountMismatch,
        InvalidChoice,
        FixedChoiceChanged,
        InvalidAction,
        ClaimRejected,
        VacateConflict,
        DependencyRejected,
        FixedStateChanged,
        InvalidValuation,
        UnsupportedDependency,
    };

    struct MasterAssignmentReplayResult
    {
        TurnPlan plan;
        OrderingResult ordering;
        MasterAssignmentReplayError error{
            MasterAssignmentReplayError::ChoiceCountMismatch};
        std::int32_t actorSlot{-1};
        std::int32_t candidateIndex{NO_CANDIDATE};
        std::int32_t actionIndex{NO_ACTION};
        bool allActorsExecutable{false};
        bool dependenciesExecutable{true};
        std::vector<std::int32_t> actionIndices;
        std::vector<MilliFunds> seatedValues;
        MilliFunds seatedEconomicTotal{0};
        ContinuationKey continuationKey;

        bool valid() const
        {
            return error == MasterAssignmentReplayError::None;
        }

        bool complete() const
        {
            return valid() && allActorsExecutable && dependenciesExecutable && ordering.cycleMembers.empty() &&
                   ordering.blockedByCycle.empty();
        }
    };

    MasterAssignmentBuildResult buildMasterAssignmentProblem(
        const AssignmentInput & input,
        const MasterAssignmentBuildRequest & request);

    MasterAssignmentBuildResult buildMasterAssignmentProblem(
        const AssignmentInput & input,
        const AssignmentResult & incumbent,
        std::uint64_t sessionIdentity);

    MasterAssignmentReplayResult replayMasterAssignment(
        const MasterAssignmentProblem & problem,
        std::span<const std::int32_t> choices);

    std::optional<MilliFunds> checkedAssignmentEconomicTotal(const AssignmentResult & assignment);

    std::optional<ContinuationKey> masterPlanContinuationKey(
        const MasterAssignmentProblem & problem, const TurnPlan & plan);

    bool masterPlanEquivalent(
        const MasterAssignmentProblem & problem,
        const TurnPlan & left,
        std::span<const std::int32_t> leftExecutionOrder,
        const TurnPlan & right,
        std::span<const std::int32_t> rightExecutionOrder);
}
