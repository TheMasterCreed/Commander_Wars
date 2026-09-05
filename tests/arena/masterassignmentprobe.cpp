#include "tests/arena/masterassignmentprobe.h"
#include "tests/arena/masterassignmentoracleprobe.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/bundleassignment.h"

namespace
{
    constexpr auto OUTPUT_VARIABLE = "COW_MASTER_ASSIGNMENT_AUDIT";

    QJsonArray tile(Coordinator::TilePoint point)
    {
        return {point.x, point.y};
    }

    QJsonArray keyRecord(const Coordinator::ContinuationKey & key)
    {
        QJsonArray result;
        for (const auto & action : key)
        {
            result.append(QJsonArray{action.row(), action.x(), action.y(), action.captures()});
        }
        return result;
    }

    QJsonArray indices(std::span<const std::int32_t> values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(value);
        }
        return result;
    }

    QJsonObject claimRecord(const Coordinator::ActionReservations & claims)
    {
        QJsonArray destinations;
        QJsonArray captures;
        QJsonArray damage;
        for (const auto point : claims.destinations)
        {
            destinations.append(tile(point));
        }
        for (const auto point : claims.captures)
        {
            captures.append(tile(point));
        }
        for (const auto & share : claims.damage)
        {
            damage.append(QJsonArray{share.targetUnitId, share.targetHpSteps, share.damageSteps});
        }
        return {{"destinations", destinations}, {"captures", captures},
                {"services", indices(claims.services)}, {"damage", damage}};
    }

    QJsonObject planRecord(const Coordinator::TurnPlan & plan)
    {
        QJsonArray actions;
        for (std::int32_t index = 0; index < plan.actionCount(); ++index)
        {
            const auto & action = plan.action(index);
            QJsonArray path;
            for (const auto point : action.path)
            {
                path.append(tile(point));
            }
            actions.append(QJsonObject{
                {"index", action.actionIndex}, {"actor", action.unitId},
                {"kind", static_cast<int>(action.kind)}, {"actionId", action.actionId},
                {"path", path}, {"destination", tile(action.destination)},
                {"target", tile(action.target)}, {"targetUnit", action.targetUnitId},
                {"terminalClass", static_cast<int>(action.marginalValue.terminal)},
                {"economic", QString::number(action.marginalValue.economicValue)},
                {"damage", action.plannedDamageSteps}, {"state", static_cast<int>(action.state)},
                {"destinationClaimant", plan.destinationClaimant(action.destination)},
                {"captureClaimant", plan.captureClaimant(action.destination)},
                {"plannedTargetDamage", plan.plannedDamageOn(action.targetUnitId)},
                {"remainingTargetHp", plan.remainingHpStepsOn(action.targetUnitId)},
                {"claims", claimRecord(plan.actionReservations(index))},
            });
        }
        QJsonArray edges;
        for (std::int32_t index = 0; index < plan.edgeCount(); ++index)
        {
            const auto & edge = plan.edge(index);
            edges.append(QJsonArray{edge.from, edge.to, static_cast<int>(edge.kind)});
        }
        const auto ordering = plan.executionOrder();
        return {{"actions", actions}, {"edges", edges}, {"executionOrder", indices(ordering.order)},
                {"cycleMembers", indices(ordering.cycleMembers)}, {"blockedByCycle", indices(ordering.blockedByCycle)}};
    }

    QString hash(const QJsonObject & value)
    {
        return QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    }
}

namespace ArenaMasterAssignmentProbe
{
    bool enabled()
    {
        return qgetenv("COW_AI_ARENA_TEST") == "1" && !qgetenv(OUTPUT_VARIABLE).isEmpty();
    }

    void record(const Coordinator::AssignmentInput & input,
                const Coordinator::AssignmentResult & incumbent,
                const Coordinator::DecisionTraceIdentity & identity)
    {
        if (!enabled())
        {
            return;
        }
        QElapsedTimer timer;
        timer.start();
        const auto extracted = Coordinator::buildMasterAssignmentProblem(input, incumbent, identity.planSequence);
        const auto extractionNanos = timer.nsecsElapsed();
        QJsonObject result{
            {"schemaVersion", 2}, {"day", identity.day}, {"player", identity.playerId},
            {"planSequence", QString::number(identity.planSequence)}, {"horizon", identity.horizonTurns},
            {"extracted", extracted.valid()}, {"error", static_cast<int>(extracted.error)},
            {"errorActorSlot", extracted.actorSlot}, {"errorCandidate", extracted.candidateIndex},
            {"errorAction", extracted.actionIndex}, {"extractionNanos", QString::number(extractionNanos)},
            {"legacyExecuted", true},
        };
        const auto original = planRecord(incumbent.plan);
        result.insert("incumbent", original);
        result.insert("incumbentHash", hash(original));
        const auto incumbentEconomic = Coordinator::checkedAssignmentEconomicTotal(incumbent);
        const auto incumbentKey = input.pStockValuer == nullptr
            ? std::optional<Coordinator::ContinuationKey>(Coordinator::ContinuationKey{})
            : input.pStockValuer->continuationKey(incumbent.plan);
        result.insert("incumbentEconomicKnown", incumbentEconomic.has_value());
        result.insert("incumbentKeyKnown", incumbentKey.has_value());
        if (incumbentEconomic)
        {
            result.insert("incumbentEconomicTotal", QString::number(*incumbentEconomic));
        }
        if (incumbentKey)
        {
            result.insert("incumbentContinuationKey", keyRecord(*incumbentKey));
        }
        if (extracted.valid())
        {
            timer.restart();
            const auto & problem = *extracted.problem;
            const auto replay = Coordinator::replayMasterAssignment(problem, problem.incumbentChoices());
            result.insert("replayNanos", QString::number(timer.nsecsElapsed()));
            result.insert("replayValid", replay.valid());
            result.insert("replayComplete", replay.complete());
            result.insert("allActorsExecutable", replay.allActorsExecutable);
            result.insert("dependenciesExecutable", replay.dependenciesExecutable);
            result.insert("incumbentRecomputedEconomicTotal", QString::number(problem.incumbentEconomicTotal()));
            result.insert("reconstructedEconomicTotal", QString::number(replay.seatedEconomicTotal));
            result.insert("economicEqual", incumbentEconomic && *incumbentEconomic == problem.incumbentEconomicTotal() &&
                          *incumbentEconomic == replay.seatedEconomicTotal);
            result.insert("reconstructedContinuationKey", keyRecord(replay.continuationKey));
            result.insert("continuationKeyEqual", incumbentKey && *incumbentKey == problem.incumbentContinuationKey() &&
                          *incumbentKey == replay.continuationKey);
            result.insert("equivalent", replay.valid() && Coordinator::masterPlanEquivalent(
                problem, incumbent.plan, incumbent.executionOrder, replay.plan, replay.ordering.order));
            const auto reconstructed = planRecord(replay.plan);
            result.insert("reconstructed", reconstructed);
            result.insert("reconstructedHash", hash(reconstructed));
            result.insert("resourceCount", static_cast<qint64>(problem.resources().size()));
            result.insert("incumbentTuple", indices(problem.incumbentChoices()));
            result.insert("executionOrderEqual", incumbent.executionOrder == replay.ordering.order);
            result.insert("actionClaimStateEqual", incumbent.plan.equivalentState(replay.plan));
            QJsonArray actors;
            std::int32_t mutableActors = 0;
            for (const auto & actor : problem.actors())
            {
                mutableActors += actor.role == Coordinator::MasterActorRole::Mutable ? 1 : 0;
                QJsonArray candidates;
                for (const auto & candidate : actor.candidates)
                {
                    candidates.append(QJsonObject{
                        {"originalIndex", candidate.identity.originalCandidateIndex},
                        {"generationIndex", candidate.identity.generationIndex},
                        {"staticEconomic", QString::number(candidate.immediateValue.economicValue)},
                        {"stockEndpoint", tile(candidate.stockEndpoint)},
                        {"plannable", candidate.plannable},
                    });
                }
                actors.append(QJsonObject{
                    {"actor", actor.engineUnitId}, {"knowledgeIndex", actor.knowledgeUnitIndex},
                    {"canonicalSlot", actor.canonicalSlot}, {"stockRow", actor.stockRow},
                    {"mutable", actor.role == Coordinator::MasterActorRole::Mutable},
                    {"canChange", problem.canChangeActor(actor.canonicalSlot)},
                    {"candidateCount", static_cast<qint64>(actor.candidates.size())},
                    {"selectedCandidate", actor.incumbentCandidateIndex}, {"actionIndex", actor.actionIndex},
                    {"incumbentSeatedValue", QString::number(actor.incumbentSeatedValue)},
                    {"reconstructedSeatedValue", static_cast<std::size_t>(actor.canonicalSlot) < replay.seatedValues.size()
                        ? QJsonValue(QString::number(replay.seatedValues[actor.canonicalSlot])) : QJsonValue()},
                    {"canonicalCandidates", indices(actor.canonicalCandidates)},
                    {"candidates", candidates},
                });
            }
            result.insert("actors", actors);
            result.insert("actorCount", static_cast<qint64>(problem.actors().size()));
            result.insert("mutableActorCount", mutableActors);
            result.insert("fixedActorCount", static_cast<qint64>(problem.actors().size()) - mutableActors);
            ArenaMasterAssignmentOracleProbe::record(problem, identity);
        }
        QFile output(QString::fromLocal8Bit(qgetenv(OUTPUT_VARIABLE)));
        const auto line = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
        if (!output.open(QIODevice::WriteOnly | QIODevice::Append) ||
            output.write(line) != line.size() || !output.flush())
        {
            qWarning() << "Master assignment audit write failed:" << output.errorString();
        }
    }
}
