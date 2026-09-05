#include "tests/coordinator_ai/masterassignmentoracle.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
    using namespace MasterAssignmentOracle;

    constexpr int SCHEMA_VERSION = 1;

    QJsonArray tile(TilePoint value)
    {
        return {value.x, value.y};
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

    QJsonArray money(std::span<const MilliFunds> values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(QString::number(value));
        }
        return result;
    }

    QJsonArray path(std::span<const TilePoint> values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(tile(value));
        }
        return result;
    }

    QJsonArray key(const ContinuationKey & value)
    {
        QJsonArray result;
        for (const auto & action : value)
        {
            result.append(QJsonArray{action.row(), action.x(), action.y(), action.captures()});
        }
        return result;
    }

    QJsonObject income(const PropertyIncome & value)
    {
        return {{"oursPerTurn", QString::number(value.oursPerTurn)},
                {"enemyPerTurn", QString::number(value.enemyPerTurn)}};
    }

    QJsonObject claims(const ActionReservations & value)
    {
        QJsonArray damage;
        for (const auto & entry : value.damage)
        {
            damage.append(QJsonArray{entry.targetUnitId, entry.targetHpSteps, entry.damageSteps});
        }
        return {{"destinations", path(value.destinations)}, {"captures", path(value.captures)},
                {"services", indices(value.services)}, {"damage", damage}};
    }

    QJsonArray edges(std::span<const PlanEdge> values)
    {
        QJsonArray result;
        for (const auto & edge : values)
        {
            result.append(QJsonArray{edge.from, edge.to, static_cast<int>(edge.kind)});
        }
        return result;
    }

    QJsonObject action(const PlannedAction & value)
    {
        return {{"actionIndex", value.actionIndex}, {"actor", value.unitId},
                {"kind", static_cast<int>(value.kind)}, {"actionId", value.actionId},
                {"path", path(value.path)}, {"destination", tile(value.destination)},
                {"target", tile(value.target)}, {"targetUnitId", value.targetUnitId},
                {"terminal", static_cast<int>(value.marginalValue.terminal)},
                {"staticEconomic", QString::number(value.marginalValue.economicValue)},
                {"damage", value.plannedDamageSteps}, {"state", static_cast<int>(value.state)}};
    }

    QJsonObject plan(const TurnPlan & value, const OrderingResult & order)
    {
        QJsonArray actions;
        for (std::int32_t index = 0; index < value.actionCount(); ++index)
        {
            auto entry = action(value.action(index));
            entry.insert("claims", claims(value.actionReservations(index)));
            actions.append(entry);
        }
        std::vector<PlanEdge> dependencies;
        for (std::int32_t index = 0; index < value.edgeCount(); ++index)
        {
            dependencies.push_back(value.edge(index));
        }
        return {{"actions", actions}, {"edges", edges(dependencies)}, {"order", indices(order.order)},
                {"cycles", indices(order.cycleMembers)}, {"blockedByCycle", indices(order.blockedByCycle)}};
    }

    QJsonObject component(const BundleComponent & value)
    {
        const auto & fire = value.fire;
        const auto & capture = value.capture;
        const auto & service = value.service;
        return {
            {"kind", static_cast<int>(value.kind)},
            {"fire", QJsonObject{
                {"targetUnitId", fire.targetUnitId}, {"replacementCost", QString::number(fire.targetReplacementCost)},
                {"hp", fire.targetHpSteps}, {"damage", fire.damageSteps}, {"counter", fire.counterSteps},
                {"shotBefore", QString::number(fire.targetBestShotBefore)},
                {"shotAfter", QString::number(fire.targetBestShotAfter)}}},
            {"capture", QJsonObject{
                {"income", income(capture.income)}, {"ownerBefore", static_cast<int>(capture.ownerBefore)},
                {"ownerAfter", static_cast<int>(capture.ownerAfter)}, {"turnsUntilOwned", capture.turnsUntilOwned}}},
            {"service", QJsonObject{
                {"partnerUnitId", service.partnerUnitId},
                {"replacementCost", QString::number(service.partnerReplacementCost)},
                {"hpBefore", service.partnerHpStepsBefore}, {"hpAfter", service.partnerHpStepsAfter},
                {"repairCost", QString::number(service.repairCost)},
                {"shotBefore", QString::number(service.partnerBestShotBefore)},
                {"shotAfter", QString::number(service.partnerBestShotAfter)}}},
        };
    }

    QJsonObject candidate(const MasterCandidate & value)
    {
        const auto & source = value.source;
        QJsonArray components;
        for (const auto & entry : source.bundle.components)
        {
            components.append(component(entry));
        }
        const auto & ledger = source.valuation.ledger;
        const auto & future = source.valuation.continuation;
        QJsonArray resources;
        for (const auto & resource : value.resources)
        {
            resources.append(QJsonArray{resource.resourceIndex, resource.demand, resource.capacity});
        }
        QJsonArray dependencies;
        for (const auto & dependency : value.dependencies)
        {
            dependencies.append(QJsonObject{{"tile", tile(dependency.tile)}, {"occupant", dependency.occupantUnitId}});
        }
        return {
            {"actorSlot", value.identity.actorSlot}, {"actor", value.identity.engineUnitId},
            {"originalIndex", value.identity.originalCandidateIndex}, {"generationIndex", source.generationIndex},
            {"knowledgeIndex", source.bundle.unitId}, {"kind", static_cast<int>(value.actionKind)},
            {"origin", tile(source.bundle.origin)}, {"destination", tile(source.bundle.destination)},
            {"path", path(source.bundle.path)}, {"target", tile(value.target)}, {"targetUnitId", value.targetUnitId},
            {"actorFacts", QJsonObject{{"replacementCost", QString::number(source.actor.replacementCost)},
                                       {"hp", source.actor.hpSteps}}},
            {"movementCost", source.movementCost}, {"vacatesProduction", source.vacatesActiveFriendlyProduction},
            {"actorShotsOrigin", money(source.actorNextShotsAtOrigin)},
            {"actorShotsDestination", money(source.actorNextShotsAtDestination)},
            {"enemyShotsOrigin", money(source.enemyShotsOnActorAtOrigin)},
            {"enemyShotsDestination", money(source.enemyShotsOnActorAtDestination)},
            {"components", components}, {"valuationValid", source.valuation.valid},
            {"ledger", QJsonObject{
                {"income", QString::number(ledger.income)}, {"friendlyCapital", QString::number(ledger.friendlyCapital)},
                {"enemyCapital", QString::number(ledger.enemyCapital)}, {"actionCost", QString::number(ledger.actionCost)},
                {"resourceCost", QString::number(ledger.resourceCost)},
                {"scriptedCapital", QString::number(ledger.scriptedCapital)},
                {"revaluation", QString::number(ledger.revaluation)}}},
            {"continuation", QJsonObject{
                {"repositioning", QString::number(future.repositioning)}, {"exposure", QString::number(future.exposure)},
                {"targetContinuationRemoved", QString::number(future.targetContinuationRemoved)},
                {"propertyContinuation", QString::number(future.propertyContinuation)},
                {"partnerContinuation", QString::number(future.partnerContinuation)}}},
            {"staticEconomic", QString::number(value.immediateValue.economicValue)},
            {"terminal", static_cast<int>(value.immediateValue.terminal)},
            {"resources", resources}, {"dependencies", dependencies}, {"stockRow", value.stockRow},
            {"stockEndpoint", tile(value.stockEndpoint)}, {"plannable", value.plannable},
        };
    }

    QJsonObject problemRecord(const MasterAssignmentProblem & problem)
    {
        QJsonArray actors;
        for (const auto & actor : problem.actors())
        {
            QJsonArray candidates;
            for (const auto & value : actor.candidates)
            {
                candidates.append(candidate(value));
            }
            actors.append(QJsonObject{
                {"slot", actor.canonicalSlot}, {"actor", actor.engineUnitId},
                {"knowledgeIndex", actor.knowledgeUnitIndex}, {"role", static_cast<int>(actor.role)},
                {"stockRow", actor.stockRow}, {"incumbentCandidate", actor.incumbentCandidateIndex},
                {"actionIndex", actor.actionIndex}, {"incumbentDamage", actor.incumbentDamageSteps},
                {"incumbentReservations", claims(actor.incumbentReservations)},
                {"incumbentSeated", QString::number(actor.incumbentSeatedValue)},
                {"canonicalCandidates", indices(actor.canonicalCandidates)}, {"candidates", candidates},
            });
        }
        QJsonArray resources;
        for (const auto & resource : problem.resources())
        {
            resources.append(QJsonObject{{"kind", static_cast<int>(resource.kind)},
                {"tile", tile(resource.tile)}, {"unitId", resource.unitId}});
        }
        QJsonArray fixedUsage;
        for (const auto & usage : problem.fixedResourceUsage())
        {
            fixedUsage.append(QJsonArray{usage.candidate.actorSlot, usage.candidate.candidateIndex,
                                        usage.usage.resourceIndex, usage.usage.demand, usage.usage.capacity});
        }
        QJsonArray links;
        for (const auto & link : problem.unitLinks())
        {
            links.append(QJsonObject{{"unitId", link.engineUnitId}, {"tile", tile(link.tile)}});
        }
        QJsonArray occupancy;
        for (const auto & occupant : problem.occupancy())
        {
            occupancy.append(QJsonObject{{"unitId", occupant.unitId}, {"tile", tile(occupant.tile)}});
        }
        QJsonArray templates;
        for (const auto & value : problem.actionTemplates())
        {
            templates.append(action(value));
        }
        return {
            {"actors", actors}, {"resources", resources}, {"fixedUsage", fixedUsage}, {"unitLinks", links},
            {"occupancy", occupancy}, {"actionTemplates", templates}, {"incumbentEdges", edges(problem.incumbentEdges())},
            {"incumbentTuple", indices(problem.incumbentChoices())},
            {"incumbentOrder", indices(problem.incumbentExecutionOrder())},
            {"incumbentEconomic", QString::number(problem.incumbentEconomicTotal())},
            {"incumbentKey", key(problem.incumbentContinuationKey())},
            {"actionIds", QJsonObject{{"wait", problem.actionIds().wait}, {"fire", problem.actionIds().fire},
                                     {"capture", problem.actionIds().capture}}},
        };
    }

    QJsonObject sequential(const SequentialInput & value)
    {
        QJsonArray columns;
        for (const auto & column : value.columns)
        {
            columns.append(QJsonObject{
                {"slot", column.slot}, {"tile", tile(column.tile)}, {"income", income(column.income)},
                {"ownerBefore", static_cast<int>(column.ownerBefore)}, {"buildingIndex", column.buildingIndex}});
        }
        QJsonArray classes;
        for (const auto & entry : value.classes)
        {
            classes.append(QJsonObject{{"captureTurns", entry.captureTurns},
                                       {"interArrivals", indices(entry.interArrivals)}});
        }
        QJsonArray rows;
        for (const auto & row : value.rows)
        {
            rows.append(QJsonObject{{"class", row.classIndex}, {"weights", money(row.weight)},
                                   {"ownedTurns", indices(row.ownedTurn)}});
        }
        QJsonArray captured;
        for (const auto flag : value.capturedNodes)
        {
            captured.append(flag);
        }
        return {{"horizon", value.horizon}, {"columns", columns}, {"classes", classes},
                {"rows", rows}, {"capturedNodes", captured}};
    }

    QJsonObject continuation(const ContinuationInput & value)
    {
        QJsonArray cases;
        for (const auto & entry : value.cases)
        {
            cases.append(QJsonObject{{"key", key(entry.key)}, {"capturedColumns", indices(entry.capturedColumns)},
                {"owned", QString::number(entry.owned)}, {"friendly", sequential(entry.friendly)},
                {"enemy", sequential(entry.enemy)}});
        }
        return {{"mode", static_cast<int>(value.mode)}, {"immutableReference", value.immutableReference},
                {"cases", cases}};
    }

    QJsonObject sequentialProof(const SequentialExactResult & value)
    {
        QJsonArray witness;
        for (const auto & row : value.witness)
        {
            QJsonArray steps;
            for (const auto & step : row.steps)
            {
                steps.append(QJsonArray{step.node, step.turn, QString::number(step.gain)});
            }
            witness.append(QJsonObject{{"row", row.row}, {"value", QString::number(row.value)}, {"steps", steps}});
        }
        return {{"status", static_cast<int>(value.status)}, {"value", QString::number(value.value)},
                {"work", QString::number(value.work)}, {"completePackings", QString::number(value.completePackings)},
                {"witness", witness}};
    }

    QJsonObject objective(const Objective & value)
    {
        return {{"status", static_cast<int>(value.status)}, {"seated", QString::number(value.seated)},
                {"owned", QString::number(value.owned)}, {"friendly", QString::number(value.friendly)},
                {"enemy", QString::number(value.enemy)}, {"total", QString::number(value.total)},
                {"capturedColumns", indices(value.capturedColumns)},
                {"friendlyProof", sequentialProof(value.friendlyProof)}, {"enemyProof", sequentialProof(value.enemyProof)}};
    }

    QJsonArray domain(const std::vector<std::vector<std::int32_t>> & values)
    {
        QJsonArray result;
        for (const auto & choices : values)
        {
            result.append(indices(choices));
        }
        return result;
    }

    QJsonValue product(const std::optional<std::uint64_t> & value)
    {
        return value ? QJsonValue(QString::number(*value)) : QJsonValue();
    }
}

namespace MasterAssignmentOracle
{
    QByteArray exportDiagnostic(const MasterAssignmentProblem & problem, const Request & request,
                                const Result & result)
    {
        const auto ownedProblem = problemRecord(problem);
        QJsonArray tuples;
        for (const auto & receipt : result.tuples)
        {
            const auto & reference = receipt.reference;
            tuples.append(QJsonObject{
                {"choices", indices(receipt.choices)}, {"status", static_cast<int>(reference.status)},
                {"reason", reference.reason}, {"representationValid", reference.representationValid},
                {"complete", reference.complete}, {"reference", plan(reference.plan, reference.ordering)},
                {"actionIndices", indices(reference.actionIndices)}, {"seatedValues", money(reference.seatedValues)},
                {"seatedTotal", QString::number(reference.seatedTotal)}, {"key", key(reference.key)},
                {"objective", objective(receipt.objective)}, {"productionValid", receipt.productionValid},
                {"productionComplete", receipt.productionComplete}, {"productionError", static_cast<int>(receipt.productionError)},
                {"comparisonApplicable", receipt.comparisonApplicable}, {"comparisonEqual", receipt.comparisonEqual},
            });
        }
        const QJsonObject resultRecord{
            {"status", static_cast<int>(result.status)}, {"reason", result.reason},
            {"fullTupleProduct", product(result.fullTupleProduct)}, {"tupleProduct", product(result.tupleProduct)},
            {"enumerated", QString::number(result.evaluated)}, {"feasible", QString::number(result.feasible)},
            {"infeasible", QString::number(result.infeasible)}, {"unsupported", QString::number(result.unsupported)},
            {"unresolved", QString::number(result.unresolved)}, {"falsePositives", QString::number(result.falsePositives)},
            {"falseNegatives", QString::number(result.falseNegatives)},
            {"reconstructionMismatches", QString::number(result.reconstructionMismatches)},
            {"continuationWork", QString::number(result.continuationWork)}, {"enumerationComplete", result.enumerationComplete},
            {"reducedDomain", result.reducedDomain}, {"incumbentRetained", result.incumbentRetained},
            {"exactOptimum", result.exactOptimum ? QJsonValue(QString::number(*result.exactOptimum)) : QJsonValue()},
            {"representative", result.representative ? QJsonValue(QString::number(*result.representative)) : QJsonValue()},
            {"winner", result.representative ? tuples[static_cast<qsizetype>(*result.representative)] : QJsonValue()},
            {"tuples", tuples},
        };
        const QJsonObject document{
            {"schemaVersion", SCHEMA_VERSION}, {"sourceRevision", request.sourceRevision},
            {"fixture", request.fixture}, {"generationSeed", QString::number(request.seed)},
            {"problem", ownedProblem}, {"problemSHA256", QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(ownedProblem).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())},
            {"fullDomain", domain(fullDomain(problem))}, {"declaredDomain", domain(result.domain)},
            {"restriction", request.restriction}, {"continuation", continuation(request.continuation)},
            {"tupleLimit", QString::number(request.tupleLimit)},
            {"continuationLimit", QString::number(request.continuationLimit)},
            {"equalityPolicy", QStringLiteral("optimal incumbent, otherwise canonical generation-index tuple")},
            {"resultStates", QJsonArray{"ExactOptimum", "NoFeasibleAssignment", "Incomplete", "Unsupported",
                                       "NonExactObjective", "InvalidInput", "ArithmeticFailure", "ReconstructionMismatch"}},
            {"tupleStates", QJsonArray{"Feasible", "Infeasible", "Unsupported", "InvalidInput", "ArithmeticFailure"}},
            {"objectiveStates", QJsonArray{"Exact", "Unavailable", "Incomplete", "InvalidInput",
                                          "ArithmeticFailure", "WitnessFailure"}},
            {"continuationModes", QJsonArray{"None", "Unavailable", "Sequential"}},
            {"result", resultRecord},
        };
        return QJsonDocument(document).toJson(QJsonDocument::Compact);
    }
}
