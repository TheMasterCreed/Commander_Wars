#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/assignmentvaluation.h"
#include "ai/coordinator/propertystockbuilder.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace
{
    using namespace Coordinator;

    struct PendingResourceUsage
    {
        MasterResourceKey key;
        std::int32_t demand{0};
        std::int32_t capacity{0};
    };

    bool resourceLess(const MasterResourceKey & left,
                      const MasterResourceKey & right)
    {
        return std::tie(left.kind, left.tile.x, left.tile.y, left.unitId) <
               std::tie(right.kind, right.tile.x, right.tile.y, right.unitId);
    }

    bool orderingEqual(const OrderingResult & left,
                       const OrderingResult & right)
    {
        return left.order == right.order &&
               left.cycleMembers == right.cycleMembers &&
               left.blockedByCycle == right.blockedByCycle;
    }

    bool validCandidateShape(const AssignmentActor & actor,
                             const CandidateBundle & candidate)
    {
        const ActionBundle & bundle = candidate.bundle;
        if (bundle.unitId != actor.knowledgeUnitIndex ||
            bundle.origin == INVALID_TILE ||
            bundle.destination == INVALID_TILE ||
            !candidate.valuation.valid ||
            candidate.actor.replacementCost < 0 ||
            !isLiveHpStepCount(candidate.actor.hpSteps))
        {
            return false;
        }
        if (bundle.path.empty())
        {
            if (bundle.origin != bundle.destination)
            {
                return false;
            }
        }
        else if (bundle.path.front() != bundle.origin ||
                 bundle.path.back() != bundle.destination)
        {
            return false;
        }
        const auto* component = singleComponentOf(candidate.bundle);
        const auto damage = component != nullptr && component->kind == ComponentKind::Fire
            ? std::min(component->fire.damageSteps, component->fire.targetHpSteps) : 0;
        return checkedSeatedCandidateValue(candidate, damage).has_value() && std::all_of(
            bundle.components.begin(),
            bundle.components.end(),
            [](const BundleComponent & component)
            {
                return isWellFormedComponent(component);
            });
    }

    std::int32_t occupantAt(std::span<const OccupancyEntry> occupancy,
                            TilePoint tile)
    {
        for (const OccupancyEntry & entry : occupancy)
        {
            if (entry.tile == tile)
            {
                return entry.unitId;
            }
        }
        return NO_UNIT;
    }

    std::vector<PendingResourceUsage> pendingResources(
        std::span<const KnownUnitLink> links,
        const CandidateBundle & candidate,
        bool & valid)
    {
        std::vector<PendingResourceUsage> result;
        result.push_back(PendingResourceUsage{
            .key = MasterResourceKey{
                .kind = MasterResourceKind::Destination,
                .tile = candidate.bundle.destination,
            },
            .demand = 1,
            .capacity = 1,
        });
        for (const BundleComponent & component :
             candidate.bundle.components)
        {
            switch (component.kind)
            {
                case ComponentKind::Fire:
                {
                    const KnownUnitLink target =
                        linkOf(links, component.fire.targetUnitId);
                    if (target.engineUnitId == NO_UNIT ||
                        target.tile == INVALID_TILE)
                    {
                        valid = false;
                        return {};
                    }
                    result.push_back(PendingResourceUsage{
                        .key = MasterResourceKey{
                            .kind = MasterResourceKind::DamageTarget,
                            .unitId = target.engineUnitId,
                        },
                        .demand = component.fire.damageSteps,
                        .capacity = component.fire.targetHpSteps,
                    });
                    break;
                }
                case ComponentKind::Capture:
                    result.push_back(PendingResourceUsage{
                        .key = MasterResourceKey{
                            .kind = MasterResourceKind::Capture,
                            .tile = candidate.bundle.destination,
                        },
                        .demand = 1,
                        .capacity = 1,
                    });
                    break;
                case ComponentKind::Service:
                {
                    const KnownUnitLink partner =
                        linkOf(links, component.service.partnerUnitId);
                    if (partner.engineUnitId == NO_UNIT)
                    {
                        valid = false;
                        return {};
                    }
                    result.push_back(PendingResourceUsage{
                        .key = MasterResourceKey{
                            .kind = MasterResourceKind::Service,
                            .unitId = partner.engineUnitId,
                        },
                        .demand = 1,
                        .capacity = 1,
                    });
                    break;
                }
            }
        }
        std::sort(
            result.begin(),
            result.end(),
            [](const PendingResourceUsage & left,
               const PendingResourceUsage & right)
            {
                return resourceLess(left.key, right.key);
            });
        for (std::size_t slot = 1; slot < result.size(); ++slot)
        {
            if (result[slot - 1].key == result[slot].key)
            {
                valid = false;
                return {};
            }
        }
        return result;
    }

    std::optional<MasterCaptureConsequence> captureConsequence(
        const CandidateBundle & candidate)
    {
        const BundleComponent* component =
            singleComponentOf(candidate.bundle);
        if (component == nullptr ||
            component->kind != ComponentKind::Capture)
        {
            return std::nullopt;
        }
        return MasterCaptureConsequence{
            .income = component->capture.income,
            .ownerBefore = component->capture.ownerBefore,
            .ownerAfter = component->capture.ownerAfter,
            .turnsUntilOwned = component->capture.turnsUntilOwned,
        };
    }

    std::int32_t resourceIndexOf(
        std::span<const MasterResourceKey> resources,
        const MasterResourceKey & key)
    {
        const auto found = std::lower_bound(
            resources.begin(), resources.end(), key, resourceLess);
        if (found == resources.end() || *found != key)
        {
            return -1;
        }
        return static_cast<std::int32_t>(found - resources.begin());
    }

    bool exclusiveResource(MasterResourceKind kind)
    {
        return kind == MasterResourceKind::Destination ||
               kind == MasterResourceKind::Capture ||
               kind == MasterResourceKind::Service;
    }

    bool sameActionExceptReplayFields(const PlannedAction & expected,
                                      const PlannedAction & actual)
    {
        PlannedAction normalized = expected;
        normalized.actionIndex = actual.actionIndex;
        normalized.plannedDamageSteps = actual.plannedDamageSteps;
        normalized.state = actual.state;
        return normalized == actual;
    }

    ReservationResult claimCandidate(
        TurnPlan & plan,
        std::int32_t actionIndex,
        const MasterCandidate & candidate,
        std::int32_t damageSteps)
    {
        const BundleComponent* component =
            singleComponentOf(candidate.source.bundle);
        return claimFor(plan, actionIndex, candidate.source,
                        component != nullptr && component->kind == ComponentKind::Fire
                            ? std::optional<std::int32_t>(damageSteps) : std::nullopt);
    }

    const MasterActor* actorAtAction(
        std::span<const MasterActor> actors,
        std::int32_t actionIndex)
    {
        for (const MasterActor & actor : actors)
        {
            if (actor.actionIndex == actionIndex)
            {
                return &actor;
            }
        }
        return nullptr;
    }

}

namespace Coordinator
{
    MasterAssignmentIdentity::MasterAssignmentIdentity(
        const std::shared_ptr<const Binding> & binding)
        : m_binding(binding)
    {
    }

    bool MasterAssignmentIdentity::established() const
    {
        const auto binding = m_binding.lock();
        return binding && binding->session != 0;
    }

    std::uint64_t MasterAssignmentIdentity::session() const
    {
        const auto binding = m_binding.lock();
        return binding == nullptr ? 0 : binding->session;
    }

    bool MasterAssignmentIdentity::sameLiveProblem(
        const MasterAssignmentIdentity & other) const
    {
        const auto left = m_binding.lock();
        const auto right = other.m_binding.lock();
        return left && right && left == right && left->session != 0;
    }

    MasterAssignmentProblem::MasterAssignmentProblem(
        std::uint64_t sessionIdentity)
        : m_identity(
              std::make_shared<MasterAssignmentIdentity::Binding>(
                  MasterAssignmentIdentity::Binding{sessionIdentity}))
    {
    }

    MasterAssignmentIdentity MasterAssignmentProblem::identity() const
    {
        return MasterAssignmentIdentity(m_identity);
    }

    const PlanActionIds & MasterAssignmentProblem::actionIds() const
    {
        return m_actionIds;
    }

    std::span<const KnownUnitLink>
    MasterAssignmentProblem::unitLinks() const
    {
        return m_unitLinks;
    }

    std::span<const MasterActor>
    MasterAssignmentProblem::actors() const
    {
        return m_actors;
    }

    std::span<const MasterResourceKey>
    MasterAssignmentProblem::resources() const
    {
        return m_resources;
    }

    std::span<const MasterFixedResourceUsage>
    MasterAssignmentProblem::fixedResourceUsage() const
    {
        return m_fixedResourceUsage;
    }

    bool MasterAssignmentProblem::incompatible(
        MasterCandidateRef left, MasterCandidateRef right) const
    {
        const auto candidate = [&](MasterCandidateRef ref) -> const MasterCandidate*
        {
            if (ref.actorSlot < 0 || ref.actorSlot >= static_cast<std::int32_t>(m_actors.size()))
            {
                return nullptr;
            }
            const auto & choices = m_actors[static_cast<std::size_t>(ref.actorSlot)].candidates;
            return ref.candidateIndex < 0 ||
                   ref.candidateIndex >= static_cast<std::int32_t>(choices.size())
                       ? nullptr : &choices[static_cast<std::size_t>(ref.candidateIndex)];
        };
        const auto * lhs = candidate(left);
        const auto * rhs = candidate(right);
        if (lhs == nullptr || rhs == nullptr)
        {
            return true;
        }
        if (left.actorSlot == right.actorSlot)
        {
            return left.candidateIndex != right.candidateIndex;
        }
        for (const auto & usage : lhs->resources)
        {
            if (exclusiveResource(m_resources[static_cast<std::size_t>(usage.resourceIndex)].kind) &&
                std::any_of(rhs->resources.begin(), rhs->resources.end(),
                            [&](const auto & other) { return usage.resourceIndex == other.resourceIndex; }))
            {
                return true;
            }
        }
        return false;
    }

    std::span<const OccupancyEntry>
    MasterAssignmentProblem::occupancy() const
    {
        return m_occupancy;
    }

    bool MasterAssignmentProblem::canChangeActor(std::int32_t actorSlot) const
    {
        if (actorSlot < 0 || actorSlot >= static_cast<std::int32_t>(m_actors.size()))
        {
            return false;
        }
        const auto & actor = m_actors[static_cast<std::size_t>(actorSlot)];
        return actor.role == MasterActorRole::Mutable &&
               (actor.actionIndex == NO_ACTION ||
                m_actionTemplates[static_cast<std::size_t>(actor.actionIndex)].state != PlanActionState::Committed);
    }

    std::span<const PlannedAction>
    MasterAssignmentProblem::actionTemplates() const
    {
        return m_actionTemplates;
    }

    std::span<const PlanEdge>
    MasterAssignmentProblem::incumbentEdges() const
    {
        return m_incumbentEdges;
    }

    std::span<const std::int32_t>
    MasterAssignmentProblem::incumbentChoices() const
    {
        return m_incumbentChoices;
    }

    std::span<const std::int32_t>
    MasterAssignmentProblem::incumbentExecutionOrder() const
    {
        return m_incumbentExecutionOrder;
    }

    MilliFunds MasterAssignmentProblem::incumbentEconomicTotal() const
    {
        return m_incumbentEconomicTotal;
    }

    const ContinuationKey & MasterAssignmentProblem::incumbentContinuationKey() const
    {
        return m_incumbentContinuationKey;
    }

    MasterAssignmentBuildResult buildMasterAssignmentProblem(
        const AssignmentInput & input,
        const MasterAssignmentBuildRequest & request)
    {
        MasterAssignmentBuildResult result;
        if (request.sessionIdentity == 0)
        {
            result.error =
                MasterAssignmentProblemError::InvalidSession;
            return result;
        }
        if (request.pIncumbentPlan == nullptr)
        {
            result.error =
                MasterAssignmentProblemError::MissingIncumbent;
            return result;
        }
        if (request.selections.size() != input.actors.size())
        {
            result.error =
                MasterAssignmentProblemError::ActorCountMismatch;
            return result;
        }

        const TurnPlan & incumbent = *request.pIncumbentPlan;
        const auto incumbentOrder = incumbent.executionOrder().order;
        if (!std::equal(incumbentOrder.begin(), incumbentOrder.end(),
                        request.incumbentExecutionOrder.begin(), request.incumbentExecutionOrder.end()))
        {
            result.error = MasterAssignmentProblemError::IncumbentOrderMismatch;
            return result;
        }
        MasterAssignmentProblem problem(request.sessionIdentity);
        problem.m_actionIds = input.actionIds;
        problem.m_unitLinks = input.unitLinks;
        problem.m_incumbentExecutionOrder.assign(
            request.incumbentExecutionOrder.begin(),
            request.incumbentExecutionOrder.end());
        std::set<std::int32_t> occupantIds;
        std::set<std::pair<std::int32_t, std::int32_t>> occupiedTiles;
        for (const auto & link : input.unitLinks)
        {
            if (link.engineUnitId == NO_UNIT)
            {
                continue;
            }
            if (link.tile == INVALID_TILE || !occupantIds.insert(link.engineUnitId).second ||
                !occupiedTiles.emplace(link.tile.x, link.tile.y).second)
            {
                result.error = MasterAssignmentProblemError::InvalidActor;
                return result;
            }
            problem.m_occupancy.push_back({link.engineUnitId, link.tile});
        }
        std::sort(
            problem.m_occupancy.begin(),
            problem.m_occupancy.end(),
            [](const OccupancyEntry & left,
               const OccupancyEntry & right)
            {
                return std::tie(left.unitId, left.tile.x, left.tile.y) <
                       std::tie(right.unitId, right.tile.x, right.tile.y);
            });
        for (std::int32_t actionIndex = 0;
             actionIndex < incumbent.actionCount();
             ++actionIndex)
        {
            problem.m_actionTemplates.push_back(
                incumbent.action(actionIndex));
        }
        for (std::int32_t edgeIndex = 0;
             edgeIndex < incumbent.edgeCount();
             ++edgeIndex)
        {
            const PlanEdge & edge = incumbent.edge(edgeIndex);
            problem.m_incumbentEdges.push_back(edge);
        }

        std::vector<const AssignmentActor*> canonicalActors;
        canonicalActors.reserve(input.actors.size());
        for (const AssignmentActor & actor : input.actors)
        {
            canonicalActors.push_back(&actor);
        }
        std::sort(
            canonicalActors.begin(),
            canonicalActors.end(),
            [](const AssignmentActor* left,
               const AssignmentActor* right)
            {
                return std::tie(
                           left->engineUnitId,
                           left->knowledgeUnitIndex) <
                       std::tie(
                           right->engineUnitId,
                           right->knowledgeUnitIndex);
            });
        std::set<std::int32_t> engineIds;
        std::set<std::int32_t> knowledgeIds;
        for (const AssignmentActor* actor : canonicalActors)
        {
            if (!engineIds.insert(actor->engineUnitId).second ||
                !knowledgeIds.insert(actor->knowledgeUnitIndex).second)
            {
                result.error =
                    MasterAssignmentProblemError::DuplicateActor;
                return result;
            }
        }

        std::vector<std::vector<std::vector<PendingResourceUsage>>>
            pending;
        pending.reserve(canonicalActors.size());
        std::vector<bool> actionSeen(
            static_cast<std::size_t>(incumbent.actionCount()), false);
        std::set<std::int32_t> stockRows;
        for (std::size_t canonicalSlot = 0;
             canonicalSlot < canonicalActors.size();
             ++canonicalSlot)
        {
            const AssignmentActor & sourceActor =
                *canonicalActors[canonicalSlot];
            if (sourceActor.engineUnitId == NO_UNIT ||
                sourceActor.knowledgeUnitIndex < 0 ||
                linkOf(input.unitLinks, sourceActor.knowledgeUnitIndex).engineUnitId != sourceActor.engineUnitId)
            {
                result.error =
                    MasterAssignmentProblemError::InvalidActor;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                return result;
            }
            const auto selection = std::find_if(
                request.selections.begin(),
                request.selections.end(),
                [&](const MasterActorSelection & current)
                {
                    return current.engineUnitId ==
                           sourceActor.engineUnitId;
                });
            if (selection == request.selections.end())
            {
                result.error =
                    MasterAssignmentProblemError::MissingActorSelection;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                return result;
            }
            if (sourceActor.candidates.empty() && selection->candidateIndex != NO_CANDIDATE)
            {
                result.error =
                    MasterAssignmentProblemError::EmptyCandidateDomain;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                return result;
            }
            if (selection->candidateIndex < NO_CANDIDATE ||
                selection->candidateIndex >=
                    static_cast<std::int32_t>(
                        sourceActor.candidates.size()))
            {
                result.error =
                    MasterAssignmentProblemError::InvalidSelection;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                result.candidateIndex = selection->candidateIndex;
                return result;
            }
            if (selection->role != MasterActorRole::Fixed &&
                selection->role != MasterActorRole::Mutable)
            {
                result.error =
                    MasterAssignmentProblemError::InvalidSelection;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                return result;
            }
            if (selection->stockRow < NO_STOCK_ROW ||
                (selection->stockRow != NO_STOCK_ROW && !stockRows.insert(selection->stockRow).second))
            {
                result.error = MasterAssignmentProblemError::InvalidStockIdentity;
                result.actorSlot = static_cast<std::int32_t>(canonicalSlot);
                return result;
            }
            if (selection->actionIndex < NO_ACTION ||
                selection->actionIndex >= incumbent.actionCount() ||
                (selection->candidateIndex != NO_CANDIDATE &&
                 selection->actionIndex == NO_ACTION))
            {
                result.error =
                    MasterAssignmentProblemError::InvalidAction;
                result.actorSlot =
                    static_cast<std::int32_t>(canonicalSlot);
                result.actionIndex = selection->actionIndex;
                return result;
            }
            if (selection->actionIndex != NO_ACTION)
            {
                const std::size_t actionSlot =
                    static_cast<std::size_t>(selection->actionIndex);
                if (actionSeen[actionSlot])
                {
                    result.error =
                        MasterAssignmentProblemError::DuplicateAction;
                    result.actorSlot =
                        static_cast<std::int32_t>(canonicalSlot);
                    result.actionIndex = selection->actionIndex;
                    return result;
                }
                actionSeen[actionSlot] = true;
                if (incumbent.action(selection->actionIndex).unitId !=
                    sourceActor.engineUnitId)
                {
                    result.error =
                        MasterAssignmentProblemError::InvalidAction;
                    result.actorSlot =
                        static_cast<std::int32_t>(canonicalSlot);
                    result.actionIndex = selection->actionIndex;
                    return result;
                }
            }

            MasterActor actor;
            actor.canonicalSlot =
                static_cast<std::int32_t>(canonicalSlot);
            actor.knowledgeUnitIndex =
                sourceActor.knowledgeUnitIndex;
            actor.engineUnitId = sourceActor.engineUnitId;
            actor.role = selection->role;
            actor.stockRow = selection->stockRow;
            actor.incumbentCandidateIndex =
                selection->candidateIndex;
            actor.actionIndex = selection->actionIndex;
            if (selection->actionIndex != NO_ACTION)
            {
                actor.incumbentDamageSteps =
                    incumbent.action(selection->actionIndex)
                        .plannedDamageSteps;
                actor.incumbentReservations = incumbent.actionReservations(selection->actionIndex);
            }

            std::vector<std::vector<PendingResourceUsage>>
                actorPending;
            actorPending.reserve(sourceActor.candidates.size());
            actor.candidates.reserve(sourceActor.candidates.size());
            std::set<std::int32_t> generationIds;
            for (std::size_t candidateSlot = 0;
                 candidateSlot < sourceActor.candidates.size();
                 ++candidateSlot)
            {
                const CandidateBundle & sourceCandidate =
                    sourceActor.candidates[candidateSlot];
                if (sourceCandidate.generationIndex >= 0 &&
                    !generationIds.insert(sourceCandidate.generationIndex).second)
                {
                    result.error = MasterAssignmentProblemError::InvalidCandidate;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex = static_cast<std::int32_t>(candidateSlot);
                    return result;
                }
                if (sourceCandidate.bundle.unitId !=
                    sourceActor.knowledgeUnitIndex)
                {
                    result.error =
                        MasterAssignmentProblemError::
                            CandidateActorMismatch;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex =
                        static_cast<std::int32_t>(candidateSlot);
                    return result;
                }
                if (sourceCandidate.bundle.origin !=
                    linkOf(input.unitLinks, sourceActor.knowledgeUnitIndex).tile)
                {
                    result.error = MasterAssignmentProblemError::InvalidCandidate;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex = static_cast<std::int32_t>(candidateSlot);
                    return result;
                }
                if (!validCandidateShape(
                        sourceActor, sourceCandidate))
                {
                    result.error =
                        MasterAssignmentProblemError::InvalidCandidate;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex =
                        static_cast<std::int32_t>(candidateSlot);
                    return result;
                }
                bool validResources = true;
                std::vector<PendingResourceUsage>
                    candidatePending = pendingResources(
                        input.unitLinks,
                        sourceCandidate,
                        validResources);
                if (!validResources)
                {
                    result.error =
                        MasterAssignmentProblemError::InvalidCandidate;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex =
                        static_cast<std::int32_t>(candidateSlot);
                    return result;
                }
                const PlannedAction action = plannedActionFrom(
                    input.actionIds,
                    input.unitLinks,
                    sourceActor.engineUnitId,
                    sourceCandidate);
                MasterCandidate candidate;
                candidate.identity = MasterCandidateIdentity{
                    .actorSlot = actor.canonicalSlot,
                    .engineUnitId = actor.engineUnitId,
                    .originalCandidateIndex =
                        static_cast<std::int32_t>(candidateSlot),
                    .generationIndex =
                        sourceCandidate.generationIndex,
                };
                candidate.actionKind = action.kind;
                candidate.origin = sourceCandidate.bundle.origin;
                candidate.destination = action.destination;
                candidate.target = action.target;
                candidate.targetUnitId = action.targetUnitId;
                candidate.immediateValue =
                    sourceCandidate.valuation.value();
                candidate.capture =
                    captureConsequence(sourceCandidate);
                candidate.stockRow = actor.stockRow;
                candidate.stockEndpoint = action.destination;
                candidate.plannable = isPlannableCandidate(
                    input.actionIds,
                    input.unitLinks,
                    sourceCandidate);
                candidate.source = sourceCandidate;

                const std::int32_t occupant = occupantAt(
                    problem.m_occupancy, action.destination);
                if (occupant != NO_UNIT &&
                    occupant != actor.engineUnitId)
                {
                    candidate.dependencies.push_back(
                        MasterVacateRequirement{
                            .tile = action.destination,
                            .occupantUnitId = occupant,
                        });
                }
                for (const PendingResourceUsage & usage :
                     candidatePending)
                {
                    problem.m_resources.push_back(usage.key);
                }
                actorPending.push_back(
                    std::move(candidatePending));
                actor.candidates.push_back(std::move(candidate));
                actor.canonicalCandidates.push_back(static_cast<std::int32_t>(candidateSlot));
            }
            std::sort(actor.canonicalCandidates.begin(), actor.canonicalCandidates.end(),
                [&](std::int32_t left, std::int32_t right)
                {
                    return actor.candidates[static_cast<std::size_t>(left)].identity.canonicalOrderKey() <
                        actor.candidates[static_cast<std::size_t>(right)].identity.canonicalOrderKey();
                });
            if (actor.incumbentCandidateIndex != NO_CANDIDATE)
            {
                const MasterCandidate & selected =
                    actor.candidates[static_cast<std::size_t>(
                        actor.incumbentCandidateIndex)];
                if (!selected.plannable)
                {
                    result.error =
                        MasterAssignmentProblemError::InvalidSelection;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex =
                        actor.incumbentCandidateIndex;
                    return result;
                }
                const PlannedAction expected = plannedActionFrom(
                    input.actionIds,
                    input.unitLinks,
                    actor.engineUnitId,
                    selected.source);
                if (!sameActionExceptReplayFields(
                        expected,
                        incumbent.action(actor.actionIndex)))
                {
                    result.error =
                        MasterAssignmentProblemError::
                            IncumbentActionMismatch;
                    result.actorSlot = actor.canonicalSlot;
                    result.candidateIndex =
                        actor.incumbentCandidateIndex;
                    result.actionIndex = actor.actionIndex;
                    return result;
                }
            }
            else if (actor.actionIndex != NO_ACTION &&
                     isLiveState(
                         incumbent.action(actor.actionIndex).state))
            {
                result.error =
                    MasterAssignmentProblemError::InvalidSelection;
                result.actorSlot = actor.canonicalSlot;
                result.actionIndex = actor.actionIndex;
                return result;
            }
            pending.push_back(std::move(actorPending));
            problem.m_actors.push_back(std::move(actor));
        }

        for (std::int32_t actionIndex = 0;
             actionIndex < incumbent.actionCount();
             ++actionIndex)
        {
            if (isLiveState(incumbent.action(actionIndex).state) &&
                !actionSeen[static_cast<std::size_t>(actionIndex)])
            {
                result.error =
                    MasterAssignmentProblemError::InvalidAction;
                result.actionIndex = actionIndex;
                return result;
            }
        }

        std::sort(
            problem.m_resources.begin(),
            problem.m_resources.end(),
            resourceLess);
        problem.m_resources.erase(
            std::unique(
                problem.m_resources.begin(),
                problem.m_resources.end()),
            problem.m_resources.end());

        for (std::size_t actorSlot = 0;
             actorSlot < problem.m_actors.size();
             ++actorSlot)
        {
            MasterActor & actor = problem.m_actors[actorSlot];
            for (std::size_t candidateSlot = 0;
                 candidateSlot < actor.candidates.size();
                 ++candidateSlot)
            {
                MasterCandidate & candidate =
                    actor.candidates[candidateSlot];
                for (const PendingResourceUsage & usage :
                     pending[actorSlot][candidateSlot])
                {
                    candidate.resources.push_back(
                        MasterResourceUsage{
                            .resourceIndex = resourceIndexOf(
                                problem.m_resources, usage.key),
                            .demand = usage.demand,
                            .capacity = usage.capacity,
                        });
                }
            }
            problem.m_incumbentChoices.push_back(
                actor.incumbentCandidateIndex);
            if (!problem.canChangeActor(actor.canonicalSlot) &&
                actor.incumbentCandidateIndex != NO_CANDIDATE &&
                isLiveState(incumbent.action(actor.actionIndex).state))
            {
                const MasterCandidate & candidate =
                    actor.candidates[static_cast<std::size_t>(
                        actor.incumbentCandidateIndex)];
                for (const MasterResourceUsage & usage :
                     candidate.resources)
                {
                    problem.m_fixedResourceUsage.push_back(
                        MasterFixedResourceUsage{
                            .candidate = MasterCandidateRef{
                                .actorSlot = actor.canonicalSlot,
                                .candidateIndex =
                                    actor.incumbentCandidateIndex,
                            },
                            .usage = MasterResourceUsage{
                                .resourceIndex = usage.resourceIndex,
                                .demand = problem.m_resources[static_cast<std::size_t>(usage.resourceIndex)].kind ==
                                                  MasterResourceKind::DamageTarget
                                              ? actor.incumbentDamageSteps : usage.demand,
                                .capacity = usage.capacity,
                            },
                        });
                }
            }
        }

        const MasterAssignmentReplayResult replay =
            replayMasterAssignment(
                problem, problem.m_incumbentChoices);
        if (!replay.valid() ||
            !masterPlanEquivalent(
                problem,
                incumbent,
                request.incumbentExecutionOrder,
                replay.plan,
                replay.ordering.order))
        {
            result.error = MasterAssignmentProblemError::IncumbentClaimMismatch;
            result.actorSlot = replay.actorSlot;
            result.candidateIndex = replay.candidateIndex;
            result.actionIndex = replay.actionIndex;
            return result;
        }
        for (auto & actor : problem.m_actors)
        {
            const auto selection = std::find_if(request.selections.begin(), request.selections.end(),
                [&](const auto & entry) { return entry.engineUnitId == actor.engineUnitId; });
            actor.incumbentSeatedValue = replay.seatedValues[static_cast<std::size_t>(actor.canonicalSlot)];
            if (actor.actionIndex != NO_ACTION && isLiveState(incumbent.action(actor.actionIndex).state) &&
                selection->seatedValue && *selection->seatedValue != actor.incumbentSeatedValue)
            {
                result.error = MasterAssignmentProblemError::IncumbentEconomicMismatch;
                result.actorSlot = actor.canonicalSlot;
                result.actionIndex = actor.actionIndex;
                return result;
            }
        }
        problem.m_incumbentEconomicTotal = replay.seatedEconomicTotal;
        problem.m_incumbentContinuationKey = replay.continuationKey;
        if (input.pStockValuer != nullptr)
        {
            const auto key = input.pStockValuer->continuationKey(incumbent);
            if (!key || *key != problem.m_incumbentContinuationKey)
            {
                result.error = MasterAssignmentProblemError::InvalidStockIdentity;
                return result;
            }
        }
        result.problem.emplace(std::move(problem));
        result.error = MasterAssignmentProblemError::None;
        return result;
    }

    MasterAssignmentBuildResult buildMasterAssignmentProblem(
        const AssignmentInput & input,
        const AssignmentResult & incumbent,
        std::uint64_t sessionIdentity)
    {
        std::vector<MasterActorSelection> selections;
        selections.reserve(incumbent.selections.size());
        for (const AssignmentResult::Selection & selection :
             incumbent.selections)
        {
            selections.push_back(MasterActorSelection{
                .engineUnitId = selection.engineUnitId,
                .actionIndex = selection.actionIndex,
                .candidateIndex = selection.candidateIndex,
                .role = selection.stockCoupled
                            ? MasterActorRole::Mutable
                            : MasterActorRole::Fixed,
                .stockRow =
                    input.pStockValuer == nullptr
                        ? NO_STOCK_ROW
                        : input.pStockValuer->stockRow(
                              selection.engineUnitId),
                .seatedValue = selection.seatedValue,
            });
        }
        return buildMasterAssignmentProblem(
            input,
            MasterAssignmentBuildRequest{
                .sessionIdentity = sessionIdentity,
                .selections = selections,
                .pIncumbentPlan = &incumbent.plan,
                .incumbentExecutionOrder =
                    incumbent.executionOrder,
            });
    }

    MasterAssignmentReplayResult replayMasterAssignment(
        const MasterAssignmentProblem & problem,
        std::span<const std::int32_t> choices)
    {
        MasterAssignmentReplayResult result;
        if (choices.size() != problem.m_actors.size())
        {
            result.error =
                MasterAssignmentReplayError::ChoiceCountMismatch;
            return result;
        }
        const bool incumbentTuple = std::equal(
            choices.begin(), choices.end(), problem.m_incumbentChoices.begin());
        result.actionIndices.resize(problem.m_actors.size(), NO_ACTION);
        for (std::size_t actorSlot = 0;
             actorSlot < problem.m_actors.size();
             ++actorSlot)
        {
            const MasterActor & actor =
                problem.m_actors[actorSlot];
            const std::int32_t choice = choices[actorSlot];
            if (choice < NO_CANDIDATE ||
                (choice == NO_CANDIDATE && actor.incumbentCandidateIndex != NO_CANDIDATE) ||
                choice >= static_cast<std::int32_t>(
                    actor.candidates.size()) ||
                (choice != NO_CANDIDATE &&
                 !actor.candidates[static_cast<std::size_t>(choice)].plannable))
            {
                result.error =
                    MasterAssignmentReplayError::InvalidChoice;
                result.actorSlot =
                    static_cast<std::int32_t>(actorSlot);
                result.candidateIndex = choice;
                return result;
            }
            if (!problem.canChangeActor(actor.canonicalSlot) &&
                choice != actor.incumbentCandidateIndex)
            {
                result.error =
                    MasterAssignmentReplayError::FixedChoiceChanged;
                result.actorSlot =
                    static_cast<std::int32_t>(actorSlot);
                result.candidateIndex = choice;
                return result;
            }
        }

        for (std::int32_t actionIndex = 0;
             actionIndex <
                 static_cast<std::int32_t>(
                     problem.m_actionTemplates.size());
             ++actionIndex)
        {
            const PlannedAction & actionTemplate =
                problem.m_actionTemplates[
                    static_cast<std::size_t>(actionIndex)];
            const MasterActor* actor = actorAtAction(
                problem.m_actors, actionIndex);
            PlannedAction action = actionTemplate;
            if (actor != nullptr)
            {
                result.actionIndices[static_cast<std::size_t>(actor->canonicalSlot)] = actionIndex;
                const std::int32_t choice =
                    choices[static_cast<std::size_t>(
                        actor->canonicalSlot)];
                if (choice == NO_CANDIDATE)
                {
                    action.plannedDamageSteps = 0;
                }
                else
                {
                    const MasterCandidate & candidate =
                        actor->candidates[static_cast<std::size_t>(
                            choice)];
                    action = plannedActionFrom(
                        problem.m_actionIds,
                        problem.m_unitLinks,
                        actor->engineUnitId,
                        candidate.source);
                    action.state = incumbentTuple || !problem.canChangeActor(actor->canonicalSlot)
                        ? actionTemplate.state : PlanActionState::Pending;
                }
            }
            if (result.plan.addAction(std::move(action)) ==
                NO_ACTION)
            {
                result.error =
                    MasterAssignmentReplayError::InvalidAction;
                result.actionIndex = actionIndex;
                return result;
            }
        }

        for (const auto & actor : problem.m_actors)
        {
            const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
            if (actor.actionIndex == NO_ACTION && choices[slot] != NO_CANDIDATE)
            {
                const auto & candidate = actor.candidates[static_cast<std::size_t>(choices[slot])];
                const auto index = result.plan.addAction(plannedActionFrom(
                    problem.m_actionIds, problem.m_unitLinks, actor.engineUnitId, candidate.source));
                if (index == NO_ACTION)
                {
                    result.error = MasterAssignmentReplayError::InvalidAction;
                    result.actorSlot = actor.canonicalSlot;
                    return result;
                }
                result.actionIndices[slot] = index;
            }
        }

        const auto requestedDamage = [&](const MasterActor & actor)
        {
            const auto choice = choices[static_cast<std::size_t>(actor.canonicalSlot)];
            const auto & candidate = actor.candidates[static_cast<std::size_t>(choice)];
            const auto* component = singleComponentOf(candidate.source.bundle);
            if (component == nullptr || component->kind != ComponentKind::Fire)
            {
                return 0;
            }
            return incumbentTuple || !problem.canChangeActor(actor.canonicalSlot)
                ? actor.incumbentDamageSteps : component->fire.damageSteps;
        };
        std::vector<const MasterActor*> claimOrder;
        claimOrder.reserve(problem.m_actors.size());
        for (const MasterActor & actor : problem.m_actors)
        {
            const auto actionIndex = result.actionIndices[static_cast<std::size_t>(actor.canonicalSlot)];
            if (actionIndex != NO_ACTION &&
                isLiveState(result.plan.action(actionIndex).state) &&
                choices[static_cast<std::size_t>(
                    actor.canonicalSlot)] != NO_CANDIDATE)
            {
                claimOrder.push_back(&actor);
            }
        }
        std::sort(
            claimOrder.begin(),
            claimOrder.end(),
            [&](const MasterActor* left, const MasterActor* right)
            {
                if ((requestedDamage(*left) == 0) != (requestedDamage(*right) == 0))
                {
                    return requestedDamage(*left) == 0;
                }
                if (problem.canChangeActor(left->canonicalSlot) != problem.canChangeActor(right->canonicalSlot))
                {
                    return !problem.canChangeActor(left->canonicalSlot);
                }
                return result.actionIndices[static_cast<std::size_t>(left->canonicalSlot)] <
                       result.actionIndices[static_cast<std::size_t>(right->canonicalSlot)];
            });
        for (const MasterActor* actor : claimOrder)
        {
            const std::int32_t choice =
                choices[static_cast<std::size_t>(
                    actor->canonicalSlot)];
            const MasterCandidate & candidate =
                actor->candidates[static_cast<std::size_t>(choice)];
            const auto actionIndex = result.actionIndices[static_cast<std::size_t>(actor->canonicalSlot)];
            const ReservationResult claim = claimCandidate(
                result.plan,
                actionIndex,
                candidate,
                requestedDamage(*actor));
            if (claim != ReservationResult::Granted)
            {
                result.error =
                    MasterAssignmentReplayError::ClaimRejected;
                result.actorSlot = actor->canonicalSlot;
                result.candidateIndex = choice;
                result.actionIndex = actionIndex;
                return result;
            }
        }

        const auto sameIntent = [](const PlannedAction & left, const PlannedAction & right)
        {
            return left.kind == right.kind && left.actionId == right.actionId && left.path == right.path &&
                   left.destination == right.destination && left.target == right.target &&
                   left.targetUnitId == right.targetUnitId;
        };
        for (const PlanEdge & edge : problem.m_incumbentEdges)
        {
            if (!incumbentTuple && edge.kind == PlanEdgeKind::Vacate)
            {
                continue;
            }
            const auto & from = result.plan.action(edge.from);
            const auto & to = result.plan.action(edge.to);
            if (isLiveState(to.state) && !isLiveState(from.state))
            {
                result.dependenciesExecutable = false;
            }
            if (isLiveState(from.state) && isLiveState(to.state))
            {
                bool prerequisiteValid = true;
                if (edge.kind == PlanEdgeKind::Wallbreak)
                {
                    prerequisiteValid = from.targetUnitId == problem.m_actionTemplates[edge.from].targetUnitId &&
                                        result.plan.targetIsLethal(from.targetUnitId);
                }
                else if (!incumbentTuple && edge.kind != PlanEdgeKind::Vacate)
                {
                    prerequisiteValid = sameIntent(from, problem.m_actionTemplates[edge.from]) &&
                                        sameIntent(to, problem.m_actionTemplates[edge.to]);
                    if (!prerequisiteValid)
                    {
                        result.error = MasterAssignmentReplayError::UnsupportedDependency;
                        result.actionIndex = edge.to;
                        return result;
                    }
                }
                if (!prerequisiteValid)
                {
                    result.error = MasterAssignmentReplayError::DependencyRejected;
                    result.actionIndex = edge.to;
                    return result;
                }
            }
            if (!result.plan.addEdge(edge.from, edge.to, edge.kind))
            {
                result.error =
                    MasterAssignmentReplayError::InvalidAction;
                result.actionIndex = edge.from;
                return result;
            }
        }
        TurnPlan derived = result.plan;
        const auto conflicts = derived.rebuildVacateEdges(problem.m_occupancy);
        if (!conflicts.empty())
        {
            result.error =
                MasterAssignmentReplayError::VacateConflict;
            result.actionIndex = conflicts.front().moverAction;
            return result;
        }
        if (incumbentTuple)
        {
            const auto liveVacates = [](const TurnPlan & plan)
            {
                std::vector<std::pair<std::int32_t, std::int32_t>> edges;
                for (std::int32_t index = 0; index < plan.edgeCount(); ++index)
                {
                    const auto & edge = plan.edge(index);
                    if (edge.kind == PlanEdgeKind::Vacate && isLiveState(plan.action(edge.from).state) &&
                        isLiveState(plan.action(edge.to).state))
                    {
                        edges.emplace_back(edge.from, edge.to);
                    }
                }
                std::sort(edges.begin(), edges.end());
                return edges;
            };
            if (liveVacates(derived) != liveVacates(result.plan))
            {
                result.error = MasterAssignmentReplayError::VacateConflict;
                return result;
            }
        }
        else
        {
            result.plan = std::move(derived);
        }
        for (const auto & actor : problem.m_actors)
        {
            if (!problem.canChangeActor(actor.canonicalSlot) && actor.actionIndex != NO_ACTION &&
                (result.plan.action(actor.actionIndex) != problem.m_actionTemplates[actor.actionIndex] ||
                 result.plan.actionReservations(actor.actionIndex) != actor.incumbentReservations))
            {
                result.error = MasterAssignmentReplayError::FixedStateChanged;
                result.actorSlot = actor.canonicalSlot;
                result.actionIndex = actor.actionIndex;
                return result;
            }
        }
        result.ordering = result.plan.executionOrder();
        result.allActorsExecutable = std::all_of(
            problem.m_actors.begin(), problem.m_actors.end(), [&](const MasterActor & actor)
            {
                const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
                return choices[slot] != NO_CANDIDATE && result.plan.hasAction(result.actionIndices[slot]) &&
                       isLiveState(result.plan.action(result.actionIndices[slot]).state);
            });
        result.seatedValues.resize(problem.m_actors.size(), 0);
        for (const auto & actor : problem.m_actors)
        {
            const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
            const auto actionIndex = result.actionIndices[slot];
            if (actionIndex == NO_ACTION || !isLiveState(result.plan.action(actionIndex).state))
            {
                continue;
            }
            const auto & candidate = actor.candidates[static_cast<std::size_t>(choices[slot])];
            const auto value = checkedSeatedCandidateValue(
                candidate.source, result.plan.action(actionIndex).plannedDamageSteps);
            if (!value || !RelativeStockDetail::checkedAdd(result.seatedEconomicTotal, *value))
            {
                result.error = MasterAssignmentReplayError::InvalidValuation;
                result.actorSlot = actor.canonicalSlot;
                return result;
            }
            result.seatedValues[slot] = *value;
        }
        const auto key = masterPlanContinuationKey(problem, result.plan);
        if (!key)
        {
            result.error = MasterAssignmentReplayError::InvalidValuation;
            return result;
        }
        result.continuationKey = *key;
        result.error = MasterAssignmentReplayError::None;
        return result;
    }

    std::optional<MilliFunds> checkedAssignmentEconomicTotal(const AssignmentResult & assignment)
    {
        MilliFunds total = 0;
        std::vector<bool> seen(static_cast<std::size_t>(assignment.plan.actionCount()), false);
        for (const auto & selection : assignment.selections)
        {
            if (selection.actionIndex == NO_ACTION)
            {
                continue;
            }
            if (!assignment.plan.hasAction(selection.actionIndex) ||
                seen[static_cast<std::size_t>(selection.actionIndex)] ||
                assignment.plan.action(selection.actionIndex).unitId != selection.engineUnitId)
            {
                return std::nullopt;
            }
            seen[static_cast<std::size_t>(selection.actionIndex)] = true;
            if (isLiveState(assignment.plan.action(selection.actionIndex).state) &&
                !RelativeStockDetail::checkedAdd(total, selection.seatedValue))
            {
                return std::nullopt;
            }
        }
        for (std::int32_t index = 0; index < assignment.plan.actionCount(); ++index)
        {
            if (isLiveState(assignment.plan.action(index).state) && !seen[static_cast<std::size_t>(index)])
            {
                return std::nullopt;
            }
        }
        return total;
    }

    std::optional<ContinuationKey> masterPlanContinuationKey(
        const MasterAssignmentProblem & problem, const TurnPlan & plan)
    {
        std::vector<PlanRowAction> actions;
        for (std::int32_t index = 0; index < plan.actionCount(); ++index)
        {
            const auto & action = plan.action(index);
            if (!isLiveState(action.state))
            {
                continue;
            }
            const auto actor = std::find_if(problem.actors().begin(), problem.actors().end(),
                [&](const auto & entry) { return entry.engineUnitId == action.unitId; });
            if (actor == problem.actors().end())
            {
                return std::nullopt;
            }
            if (actor->stockRow != NO_STOCK_ROW)
            {
                actions.push_back({actor->stockRow, action.destination,
                    action.kind == PlanBundleKind::Capture || action.kind == PlanBundleKind::MoveAndCapture});
            }
        }
        return canonicalPlanKey(actions);
    }

    bool masterPlanEquivalent(
        const MasterAssignmentProblem &,
        const TurnPlan & left,
        std::span<const std::int32_t> leftExecutionOrder,
        const TurnPlan & right,
        std::span<const std::int32_t> rightExecutionOrder)
    {
        return left.equivalentState(right) &&
               std::equal(leftExecutionOrder.begin(), leftExecutionOrder.end(),
                          rightExecutionOrder.begin(), rightExecutionOrder.end()) &&
               orderingEqual(left.executionOrder(), right.executionOrder());
    }
}
