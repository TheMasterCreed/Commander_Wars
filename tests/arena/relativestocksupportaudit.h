#pragma once

#include <QDataStream>
#include <QIODevice>
#include <QJsonObject>
#include <QVariantMap>

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/propertystockfield.h"

class ArenaRelativeStockSupportAudit
{
public:
    static constexpr std::size_t MAXIMUM_SUPPORT = 4;

    class SelectionTrace final : public Coordinator::DecisionTrace
    {
    public:
        bool candidateDetailsEnabled() const override
        {
            return false;
        }

        bool stockDetailsEnabled() const override
        {
            return false;
        }

        void record(const QString &, const QString &) override
        {
        }

        bool flush() override
        {
            return true;
        }
    };

    struct Trial
    {
        Coordinator::AssignmentResult assignment;
        Coordinator::MilliFunds economic{0};
        bool valid{false};
    };

    struct Limits
    {
        std::int64_t totalWork{0};
        std::int64_t discoveryWork{0};
        std::int64_t maximumStateCap{0};
        std::int64_t incumbentStateCap{0};
    };

    struct Target
    {
        QString name;
        std::vector<std::int32_t> tuple;
    };

    bool load(const QJsonObject & object, const QString & directory, QString & error);
    QVariantMap run(const Coordinator::AssignmentInput & input, const Coordinator::PropertyStockField & field,
                    const Coordinator::AssignmentResult & incumbent) const;

    static std::vector<Coordinator::ActorProgress> progress(
        const Coordinator::AssignmentInput & input, const Coordinator::AssignmentResult & assignment)
    {
        if (assignment.selections.size() != input.actors.size())
        {
            return {};
        }
        Coordinator::AssignmentStats ignored;
        auto actors = Coordinator::MaximumValueAssignment::prepareActors(input, ignored);
        for (auto & actor : actors)
        {
            const auto found = std::find_if(assignment.selections.begin(), assignment.selections.end(),
                [&](const auto & selection) { return selection.engineUnitId == actor.pActor->engineUnitId; });
            if (found == assignment.selections.end())
            {
                return {};
            }
            actor.actionIndex = found->actionIndex;
            actor.chosen = found->candidateIndex;
            actor.greedyChosen = found->greedyCandidateIndex;
            actor.seatedValue = found->seatedValue;
            actor.phase = found->phase;
            actor.completeValues = found->completeValues;
        }
        return actors;
    }

    static Trial makeTrial(const Coordinator::AssignmentInput & input,
                           const Coordinator::AssignmentResult & incumbent,
                           const std::vector<Coordinator::ActorProgress> & actors,
                           const std::vector<std::int32_t> & support,
                           const std::vector<std::int32_t> & tuple)
    {
        using namespace Coordinator;
        Trial result;
        if (actors.size() != input.actors.size() || tuple.size() != actors.size() ||
            support.empty() || support.size() > MAXIMUM_SUPPORT)
        {
            return result;
        }
        std::vector<std::int32_t> actual;
        for (std::size_t slot = 0; slot < actors.size(); ++slot)
        {
            const auto candidate = tuple[slot];
            const auto & actor = actors[slot];
            if (actor.chosen == NO_CANDIDATE || candidate < 0 ||
                static_cast<std::size_t>(candidate) >= actor.pActor->candidates.size())
            {
                return result;
            }
            if (candidate != actor.chosen)
            {
                if (!stockCoupled(input, actor.pActor->engineUnitId) ||
                    !isPlannableCandidate(input.actionIds, input.unitLinks, actor.pActor->candidates[candidate]))
                {
                    return result;
                }
                actual.push_back(static_cast<std::int32_t>(slot));
            }
        }
        auto declared = support;
        std::sort(declared.begin(), declared.end());
        if (actual != declared)
        {
            return result;
        }
        auto trial = MaximumValueAssignment::relativeTrial(incumbent.plan, input, actors, support, tuple);
        if (!trial.valid || !MaximumValueAssignment::replayRelativeClaims(trial, input, actors, tuple) ||
            nonVacateIdentity(trial.plan) != nonVacateIdentity(incumbent.plan))
        {
            return result;
        }
        result.assignment = incumbent;
        result.assignment.plan = std::move(trial.plan);
        for (std::size_t index = 0; index < support.size(); ++index)
        {
            const auto slot = static_cast<std::size_t>(support[index]);
            const auto found = std::find_if(result.assignment.selections.begin(), result.assignment.selections.end(),
                [&](const auto & selection) { return selection.engineUnitId == actors[slot].pActor->engineUnitId; });
            if (found == result.assignment.selections.end())
            {
                return {};
            }
            found->candidateIndex = trial.seats[index].chosen;
            found->actionIndex = trial.seats[index].actionIndex;
            found->seatedValue = trial.seats[index].seatedValue;
            found->phase = AssignmentResult::SelectionPhase::None;
        }
        if (!result.assignment.plan.rebuildVacateEdges(planOccupancy(result.assignment.plan)).empty())
        {
            return {};
        }
        result.assignment.executionOrder = result.assignment.plan.executionOrder().order;
        if (result.assignment.executionOrder != trial.order)
        {
            return {};
        }
        for (const auto & selection : incumbent.selections)
        {
            const auto slot = std::find_if(actors.begin(), actors.end(),
                [&](const auto & actor) { return actor.pActor->engineUnitId == selection.engineUnitId; });
            const auto index = static_cast<std::int32_t>(slot - actors.begin());
            if (slot == actors.end() ||
                (std::find(support.begin(), support.end(), index) == support.end() &&
                 actorIdentity(result.assignment, selection.engineUnitId) != actorIdentity(incumbent, selection.engineUnitId)))
            {
                return {};
            }
        }
        result.economic = trial.economic;
        result.valid = true;
        return result;
    }

    static bool replayIncumbent(const Coordinator::AssignmentInput & input,
                                const Coordinator::AssignmentResult & assignment,
                                Coordinator::MilliFunds & economic)
    {
        const auto actors = progress(input, assignment);
        if (actors.size() != input.actors.size() || actors.size() != assignment.selections.size())
        {
            return false;
        }
        std::vector<std::int32_t> tuple;
        for (const auto & actor : actors)
        {
            tuple.push_back(actor.chosen);
        }
        auto trial = Coordinator::MaximumValueAssignment::relativeTrial(assignment.plan, input, actors, {}, tuple);
        if (!trial.valid || !Coordinator::MaximumValueAssignment::replayRelativeClaims(trial, input, actors, tuple) ||
            trial.order != assignment.executionOrder)
        {
            return false;
        }
        economic = trial.economic;
        return true;
    }

    static QByteArray actorIdentity(const Coordinator::AssignmentResult & assignment, std::int32_t actor)
    {
        const auto found = std::find_if(assignment.selections.begin(), assignment.selections.end(),
            [&](const auto & selection) { return selection.engineUnitId == actor; });
        if (found == assignment.selections.end() || !assignment.plan.hasAction(found->actionIndex))
        {
            return {};
        }
        QByteArray result;
        QDataStream stream(&result, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_5);
        stream << found->knowledgeUnitIndex << found->engineUnitId << found->actionIndex << found->candidateIndex
               << found->greedyCandidateIndex << static_cast<qint64>(found->seatedValue) << found->stockCoupled
               << static_cast<qint32>(found->phase);
        const auto & action = assignment.plan.action(found->actionIndex);
        stream << action.actionIndex << action.unitId << static_cast<qint32>(action.kind) << action.actionId
               << action.destination.x << action.destination.y << action.target.x << action.target.y
               << action.targetUnitId << static_cast<qint32>(action.marginalValue.terminal)
               << static_cast<qint64>(action.marginalValue.economicValue) << action.plannedDamageSteps
               << static_cast<qint32>(action.state) << static_cast<quint64>(action.path.size());
        for (const auto & tile : action.path)
        {
            stream << tile.x << tile.y;
        }
        stream << claimsIdentity(assignment.plan, found->actionIndex);
        return result;
    }

    static QByteArray planIdentity(const Coordinator::AssignmentResult & assignment)
    {
        std::vector<std::int32_t> actors;
        for (const auto & selection : assignment.selections)
        {
            actors.push_back(selection.engineUnitId);
        }
        std::sort(actors.begin(), actors.end());
        QByteArray result;
        QDataStream stream(&result, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_5);
        stream << static_cast<quint64>(actors.size()) << assignment.plan.actionCount();
        for (const auto actor : actors)
        {
            stream << actorIdentity(assignment, actor);
        }
        stream << static_cast<quint64>(assignment.plan.m_edges.size());
        for (const auto & edge : assignment.plan.m_edges)
        {
            stream << edge.from << edge.to << static_cast<qint32>(edge.kind);
        }
        stream << static_cast<quint64>(assignment.executionOrder.size());
        for (const auto action : assignment.executionOrder)
        {
            stream << action;
        }
        return result;
    }

private:
    enum class ClaimKind : qint32
    {
        Destination,
        Capture,
        Service,
        Damage,
    };

    static QByteArray nonVacateIdentity(const Coordinator::TurnPlan & plan)
    {
        QByteArray result;
        QDataStream stream(&result, QIODevice::WriteOnly);
        for (const auto & edge : plan.m_edges)
        {
            if (edge.kind != Coordinator::PlanEdgeKind::Vacate)
            {
                stream << edge.from << edge.to << static_cast<qint32>(edge.kind);
            }
        }
        return result;
    }

    static QByteArray claimsIdentity(const Coordinator::TurnPlan & plan, std::int32_t action)
    {
        QByteArray result;
        QDataStream stream(&result, QIODevice::WriteOnly);
        for (const auto & claim : plan.m_destinationClaims)
        {
            if (claim.actionIndex == action)
            {
                stream << static_cast<qint32>(ClaimKind::Destination) << claim.tile.x << claim.tile.y;
            }
        }
        for (const auto & claim : plan.m_captureClaims)
        {
            if (claim.actionIndex == action)
            {
                stream << static_cast<qint32>(ClaimKind::Capture) << claim.tile.x << claim.tile.y;
            }
        }
        for (const auto & claim : plan.m_serviceClaims)
        {
            if (claim.actionIndex == action)
            {
                stream << static_cast<qint32>(ClaimKind::Service) << claim.partnerUnitId;
            }
        }
        for (const auto & claim : plan.m_damageClaims)
        {
            for (const auto & contribution : claim.contributions)
            {
                if (contribution.actionIndex == action)
                {
                    stream << static_cast<qint32>(ClaimKind::Damage) << claim.targetUnitId
                           << claim.remainingHpSteps << contribution.hpSteps;
                }
            }
        }
        return result;
    }

    QString m_outputPath;
    Limits m_limits;
    std::vector<Target> m_targets;
};
