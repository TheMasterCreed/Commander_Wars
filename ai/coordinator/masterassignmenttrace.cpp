#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/masterassignmentshadow.h"

namespace
{
    bool liveEvaluation(const Coordinator::MasterShadowObservation & observation,
                        const Coordinator::MasterAssignmentEvaluation & evaluation)
    {
        if (observation.capture == nullptr || !observation.capture->extraction.valid() ||
            !Coordinator::validMasterState(*observation.capture->extraction.problem, evaluation.state))
        {
            return false;
        }
        if (!evaluation.lowerReplayed)
        {
            return true;
        }
        return observation.session != nullptr && evaluation.proof &&
            observation.session->owns(evaluation.proof->quote) &&
            observation.session->replayLowerProof(*evaluation.proof);
    }

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

    QJsonArray valuesRecord(std::span<const std::int64_t> values)
    {
        QJsonArray result;
        for (const auto value : values)
        {
            result.append(QString::number(value));
        }
        return result;
    }

    QJsonArray stockRowsRecord(std::span<const Coordinator::PropertyStockRow> rows)
    {
        QJsonArray result;
        for (const auto & row : rows)
        {
            result.append(QJsonObject{{"knowledgeIndex", row.knowledgeIndex}, {"gridIdentity", row.gridIdentity},
                {"movementPoints", row.movementPoints}, {"capturePoints", row.capturePoints},
                {"captureRate", row.captureRate}, {"tile", tile(row.tile)}});
        }
        return result;
    }

    QJsonArray stockColumnsRecord(std::span<const Coordinator::PropertyStockColumn> columns)
    {
        QJsonArray result;
        for (const auto & column : columns)
        {
            result.append(QJsonObject{{"slot", column.slot}, {"tile", tile(column.tile)},
                {"income", QJsonObject{{"oursPerTurn", QString::number(column.income.oursPerTurn)},
                    {"enemyPerTurn", QString::number(column.income.enemyPerTurn)}}},
                {"ownerBefore", static_cast<int>(column.ownerBefore)}, {"buildingIndex", column.buildingIndex}});
        }
        return result;
    }

    QJsonObject mobilityGridRecord(const Coordinator::MobilityCostGrid & grid)
    {
        QJsonArray directions;
        for (const auto direction : Coordinator::ENTRY_DIRECTIONS)
        {
            directions.append(static_cast<int>(direction));
        }
        QJsonArray costs;
        for (std::int32_t y = 0; y < grid.height(); ++y)
        {
            QJsonArray row;
            for (std::int32_t x = 0; x < grid.width(); ++x)
            {
                QJsonArray cell;
                for (const auto direction : Coordinator::ENTRY_DIRECTIONS)
                {
                    cell.append(grid.cost(x, y, direction));
                }
                row.append(cell);
            }
            costs.append(row);
        }
        return {{"width", grid.width()}, {"height", grid.height()}, {"entryDirections", directions}, {"costs", costs}};
    }

    QJsonObject packingFieldRecord(const Coordinator::PackingDualFieldIdentity & field)
    {
        QJsonArray columnTiles;
        for (const auto point : field.columnTiles)
        {
            columnTiles.append(tile(point));
        }
        QJsonArray grids;
        for (const auto & grid : field.mobilityGrids)
        {
            grids.append(mobilityGridRecord(grid));
        }
        return {{"horizonTurns", field.horizonTurns}, {"width", field.width}, {"height", field.height},
            {"capturePointsToCapture", field.capturePointsToCapture},
            {"ownedBaseline", QString::number(field.ownedBaseline)},
            {"ourRows", stockRowsRecord(field.ourRows)}, {"ourColumns", stockColumnsRecord(field.ourColumns)},
            {"ourWeights", valuesRecord(field.ourWeights)}, {"enemyRows", stockRowsRecord(field.enemyRows)},
            {"enemyColumns", stockColumnsRecord(field.enemyColumns)}, {"enemyWeights", valuesRecord(field.enemyWeights)},
            {"columnTiles", columnTiles}, {"columnCapturers", Coordinator::masterChoicesRecord(field.columnCapturers)},
            {"columnCapturePoints", Coordinator::masterChoicesRecord(field.columnCapturePoints)},
            {"enemyArrivals", Coordinator::masterChoicesRecord(field.enemyArrivals)}, {"mobilityGrids", grids}};
    }

    QJsonObject packingIdentityRecord(const Coordinator::PackingDualProblemIdentity & identity)
    {
        QJsonArray rowDomains;
        for (const auto & domain : identity.rowDomains)
        {
            rowDomains.append(valuesRecord(domain));
        }
        QJsonArray usedNodes;
        for (std::int32_t node = 0; node < identity.usedNodes.nodeCount(); ++node)
        {
            if (identity.usedNodes.contains(node))
            {
                usedNodes.append(node);
            }
        }
        return {{"session", QString::number(identity.session)}, {"immutableField", packingFieldRecord(identity.immutableField)},
            {"key", keyRecord(identity.key)}, {"capturedColumns", Coordinator::masterChoicesRecord(identity.capturedColumns)},
            {"rowDomains", rowDomains}, {"nodeColumns", stockColumnsRecord(identity.nodeColumns)},
            {"usedNodes", QJsonObject{{"nodeCount", identity.usedNodes.nodeCount()}, {"nodes", usedNodes}}},
            {"economic", QString::number(identity.economic)}, {"owned", QString::number(identity.owned)},
            {"incumbentFloor", QString::number(identity.incumbentFloor)}, {"fixedValue", QString::number(identity.fixedValue)}};
    }

    QJsonObject packingCertificateRecord(const Coordinator::PackingDualCertificate & certificate)
    {
        return {{"identity", packingIdentityRecord(certificate.identity)},
            {"rows", Coordinator::masterChoicesRecord(certificate.rows)},
            {"nodePrices", valuesRecord(certificate.nodePrices)}, {"rowReducedMaxima", valuesRecord(certificate.rowReducedMaxima)},
            {"dualObjective", QString::number(certificate.dualObjective)}};
    }

    QJsonArray sequentialWitnessRecord(std::span<const Coordinator::SequentialRowWitness> witness)
    {
        QJsonArray result;
        for (const auto & row : witness)
        {
            QJsonArray visits;
            for (const auto & visit : row.steps)
            {
                visits.append(QJsonArray{visit.node, visit.turn});
            }
            result.append(QJsonObject{{"row", row.row}, {"visits", visits}, {"value", QString::number(row.value)}});
        }
        return result;
    }
}

namespace Coordinator
{
    bool masterShadowProofsMatch(const MasterShadowObservation & observation)
    {
        if (observation.search == nullptr)
        {
            return true;
        }
        const auto & result = *observation.search;
        if ((result.incumbent && !liveEvaluation(observation, *result.incumbent)) ||
            (result.best && !liveEvaluation(observation, *result.best)))
        {
            return false;
        }
        if (!result.improvement)
        {
            return true;
        }
        const auto & proof = *result.improvement;
        if (!proof.incumbent.lowerReplayed || !proof.incumbent.upperVerified ||
            !proof.challenger.lowerReplayed || !proof.challenger.upperVerified ||
            !proof.incumbent.replay || !proof.challenger.replay ||
            !proof.incumbent.proof || !proof.challenger.proof ||
            !liveEvaluation(observation, proof.incumbent) || !liveEvaluation(observation, proof.challenger))
        {
            return false;
        }
        auto incumbent = *proof.incumbent.quote();
        auto challenger = *proof.challenger.quote();
        if (incumbent.economic != 0 || challenger.economic != 0)
        {
            return false;
        }
        // Historical stock snapshots use the shared comparator with their recorded seated economics.
        incumbent.economic = proof.incumbent.replay->seatedEconomicTotal;
        challenger.economic = proof.challenger.replay->seatedEconomicTotal;
        const auto comparison = compareRelativeStock(incumbent, challenger);
        return comparison.improves() && comparison.deltaLower == proof.comparison.deltaLower &&
            comparison.deltaUpper == proof.comparison.deltaUpper &&
            comparison.economicDelta == proof.comparison.economicDelta &&
            comparison.ownedDelta == proof.comparison.ownedDelta &&
            comparison.commonEnemyCancelled == proof.comparison.commonEnemyCancelled &&
            comparison.reason == proof.comparison.reason;
    }

    QJsonArray masterChoicesRecord(std::span<const std::int32_t> choices)
    {
        QJsonArray result;
        for (const auto value : choices)
        {
            result.append(value);
        }
        return result;
    }

    QJsonValue masterValueRecord(std::optional<MilliFunds> value)
    {
        return value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null);
    }

    QJsonObject masterWorkRecord(const RefinementLedgerStats & work)
    {
        constexpr std::array<const char*, REFINEMENT_WORK_CLASSES> names{
            "mandatoryPrecompute", "ourAdmission", "enemyEntry", "searchSlice", "packingDualCertificate",
            "masterPreparation", "masterExpansion", "masterBoundScan", "masterDualOptimization",
            "masterDualVerification", "masterValuation", "masterWitness", "masterReplay", "masterInstallation",
        };
        QJsonArray net;
        QJsonObject categories;
        for (std::size_t category = 0; category < REFINEMENT_WORK_CLASSES; ++category)
        {
            const auto spent = work.grantedByWork[category] - work.refundedByWork[category];
            net.append(QString::number(spent));
            categories.insert(QLatin1String(names[category]), QJsonObject{
                {"spent", QString::number(spent)}, {"granted", QString::number(work.grantedByWork[category])},
                {"refunded", QString::number(work.refundedByWork[category])},
                {"refusals", QString::number(work.refusalsByWork[category])},
            });
        }
        return {{"allowance", QString::number(work.total)}, {"spent", QString::number(work.spent())},
            {"directConsumed", QString::number(work.consumed)},
            {"netSpentByWork", net}, {"workCategories", categories}, {"refusals", QString::number(work.refusals)},
            {"refundClamps", QString::number(work.refundClamps)}};
    }

    QJsonObject masterRefusalRecord(const std::optional<RefinementRefusal> & refusal)
    {
        if (!refusal)
        {
            return {};
        }
        return {{"work", static_cast<int>(refusal->work)}, {"reason", static_cast<int>(refusal->reason)},
            {"requested", QString::number(refusal->requested)}, {"balance", QString::number(refusal->balance)},
            {"refundable", refusal->refundable}};
    }

    QJsonObject masterRefinementRefusalRecord(const std::optional<MasterRefinementRefusal> & refusal)
    {
        if (!refusal)
        {
            return {};
        }
        return {{"term", static_cast<int>(refusal->term)}, {"requested", QString::number(refusal->requested)},
            {"balance", QString::number(refusal->balance)}, {"allowance", QString::number(refusal->allowance)},
            {"preparation", QString::number(refusal->preparation)}};
    }

    QJsonObject masterCertificatePreparationRecord(
        const std::optional<RelativeStockCertificatePreparation> & preparation)
    {
        if (!preparation)
        {
            return {};
        }
        return {{"status", static_cast<int>(preparation->status)},
            {"readinessWork", QString::number(preparation->readinessWork)},
            {"scanWork", QString::number(preparation->scanWork)}};
    }

    QJsonObject masterSearchRecord(const MasterAssignmentSearchResult & result)
    {
        QJsonArray residual;
        for (const auto & region : result.residual)
        {
            residual.append(QJsonObject{{"choices", masterChoicesRecord(region.state.choices)},
                {"upper", masterValueRecord(region.upper)}, {"complete", region.completeTuple},
                {"unsupported", region.unsupported}});
        }
        QJsonArray prunes;
        for (const auto & prune : result.prunes)
        {
            prunes.append(QJsonObject{{"choices", masterChoicesRecord(prune.state.choices)},
                {"upper", QString::number(prune.upper)}, {"feasibleLower", QString::number(prune.feasibleLower)}});
        }
        QJsonObject improvement;
        if (result.improvement)
        {
            const auto & proof = *result.improvement;
            improvement = {{"choices", masterChoicesRecord(proof.challenger.state.choices)},
                {"deltaLower", QString::number(proof.comparison.deltaLower)},
                {"deltaUpper", QString::number(proof.comparison.deltaUpper)},
                {"commonEnemy", proof.comparison.commonEnemyCancelled}, {"work", QString::number(proof.work)}};
        }
        QJsonObject record{
            {"schedule", QJsonObject{{"currentDualRounds", result.schedule.currentDualRounds},
                {"continuationSlice", QString::number(result.schedule.continuationSlice)},
                {"refinementGrowthFactor", QString::number(result.schedule.refinementGrowthFactor)},
                {"futureDualAllowance", QString::number(result.schedule.futureDualAllowance)},
                {"futureBounds", static_cast<int>(result.schedule.futureBounds)},
                {"incumbentCompletions", result.schedule.incumbentCompletions},
                {"incumbentRefinementStride", QString::number(result.schedule.incumbentRefinementStride)},
                {"challengerRefinementBurst", QString::number(result.schedule.challengerRefinementBurst)},
                {"nodesPerRefinement", QString::number(result.schedule.nodesPerRefinement)}}},
            {"lower", masterValueRecord(result.lower)}, {"upper", masterValueRecord(result.upper)},
            {"residualUpper", masterValueRecord(result.residualUpper)},
            {"gap", masterValueRecord(result.gap)}, {"globalOptimal", result.globalOptimal},
            {"budgetExhausted", result.budgetExhausted}, {"domainAccounted", result.domainAccounted},
            {"fullDomain", result.fullDomain}, {"noFeasibleAssignment", result.noFeasibleAssignment},
            {"invalidInput", result.invalidInput}, {"implicitRoot", result.implicitRoot},
            {"residualRegions", QString::number(result.stats.residualRegions)},
            {"peakQueuedRegions", QString::number(result.stats.peakFrontier)},
            {"peakResidentRegions", QString::number(result.stats.peakResidentRegions)},
            {"incumbentPreserved", result.incumbentPreserved}, {"canonicalRepresentative", result.canonicalRepresentative},
            {"bestChoices", result.best ? masterChoicesRecord(result.best->state.choices) : QJsonArray{}},
            {"expanded", QString::number(result.stats.expanded)}, {"evaluated", QString::number(result.stats.evaluated)},
            {"incumbentCompletions", QString::number(result.stats.incumbentCompletions)},
            {"incumbentRefinements", QString::number(result.stats.incumbentRefinements)},
            {"cancelledEnemyRefinements", QString::number(result.stats.cancelledEnemyRefinements)},
            {"workBalancedSelections", QString::number(result.stats.workBalancedSelections)},
            {"evaluationCacheHits", QString::number(result.stats.evaluationCacheHits)},
            {"infeasible", QString::number(result.stats.infeasible)}, {"pruned", QString::number(result.stats.pruned)},
            {"unsupported", QString::number(result.stats.unsupported)},
            {"arithmeticFailures", QString::number(result.stats.arithmeticFailures)},
            {"proofFailures", QString::number(result.stats.proofFailures)},
            {"firstImprovementWork", masterValueRecord(result.stats.firstImprovementWork)},
            {"firstImprovementLower", masterValueRecord(result.stats.firstImprovementLower)},
            {"futurePricingCache", QJsonObject{{"hits", QString::number(result.stats.futurePricingCache.hits)},
                {"recomputations", QString::number(result.stats.futurePricingCache.recomputations)}}},
            {"retainedGrants", QString::number(result.stats.retainedGrants)},
            {"refundableStorageBytes", QString::number(result.stats.refundableStorageBytes)},
            {"refinements", QString::number(result.stats.refinements)}, {"residual", residual},
            {"prunes", prunes}, {"improvement", improvement}};
        const auto work = masterWorkRecord(result.stats.work);
        for (auto entry = work.begin(); entry != work.end(); ++entry)
        {
            record.insert(entry.key(), entry.value());
        }
        return record;
    }

    QJsonObject masterModelRecord(const MasterAssignmentProblem & problem)
    {
        QJsonArray actors;
        std::int64_t mutableCount = 0;
        std::int64_t candidateCount = 0;
        for (const auto & actor : problem.actors())
        {
            const bool changeable = problem.canChangeActor(actor.canonicalSlot);
            mutableCount += changeable ? 1 : 0;
            QJsonArray candidates;
            for (const auto & candidate : actor.candidates)
            {
                QJsonArray resources;
                for (const auto & use : candidate.resources)
                {
                    resources.append(QJsonArray{use.resourceIndex, use.demand, use.capacity});
                }
                candidates.append(QJsonObject{
                    {"index", candidate.identity.originalCandidateIndex},
                    {"generation", candidate.identity.generationIndex}, {"kind", static_cast<int>(candidate.actionKind)},
                    {"origin", tile(candidate.origin)}, {"destination", tile(candidate.destination)},
                    {"target", tile(candidate.target)}, {"targetUnit", candidate.targetUnitId},
                    {"stockEndpoint", tile(candidate.stockEndpoint)}, {"capture", candidate.capture.has_value()},
                    {"economic", QString::number(candidate.immediateValue.economicValue)},
                    {"terminal", static_cast<int>(candidate.immediateValue.terminal)},
                    {"plannable", candidate.plannable}, {"resources", resources}});
            }
            candidateCount += static_cast<std::int64_t>(actor.candidates.size());
            actors.append(QJsonObject{
                {"actor", actor.engineUnitId}, {"knowledgeIndex", actor.knowledgeUnitIndex},
                {"slot", actor.canonicalSlot}, {"role", static_cast<int>(actor.role)}, {"mutable", changeable},
                {"stockRow", actor.stockRow}, {"incumbent", actor.incumbentCandidateIndex},
                {"canonicalCandidates", masterChoicesRecord(actor.canonicalCandidates)}, {"candidates", candidates}});
        }
        return {{"actors", actors}, {"actorCount", static_cast<qint64>(problem.actors().size())},
            {"mutableActors", static_cast<qint64>(mutableCount)}, {"candidates", static_cast<qint64>(candidateCount)},
            {"currentResources", static_cast<qint64>(problem.resources().size())},
            {"incumbentChoices", masterChoicesRecord(problem.incumbentChoices())}};
    }

    QJsonObject masterEvaluationRecord(const MasterAssignmentEvaluation & evaluation)
    {
        QJsonArray actions;
        if (evaluation.replay)
        {
            const auto & plan = evaluation.replay->plan;
            for (std::int32_t index = 0; index < plan.actionCount(); ++index)
            {
                const auto & action = plan.action(index);
                QJsonArray path;
                for (const auto & point : action.path)
                {
                    path.append(tile(point));
                }
                actions.append(QJsonObject{
                    {"index", action.actionIndex}, {"actor", action.unitId},
                    {"actionId", action.actionId}, {"kind", static_cast<int>(action.kind)},
                    {"destination", tile(action.destination)}, {"target", tile(action.target)},
                    {"targetUnit", action.targetUnitId}, {"path", path}, {"state", static_cast<int>(action.state)},
                    {"damage", action.plannedDamageSteps}, {"claims", masterClaimsRecord(plan.actionReservations(index))}});
            }
        }
        QJsonObject record{
            {"choices", masterChoicesRecord(evaluation.state.choices)}, {"status", static_cast<int>(evaluation.status)},
            {"objectiveStatus", static_cast<int>(evaluation.objectiveStatus)}, {"actions", actions},
            {"seatedEconomic", masterValueRecord(evaluation.replay
                ? std::optional<MilliFunds>{evaluation.replay->seatedEconomicTotal} : std::nullopt)},
            {"lowerReplayed", evaluation.lowerReplayed}, {"upperVerified", evaluation.upperVerified},
            {"exact", evaluation.exact}, {"lower", masterValueRecord(evaluation.objective
                ? std::optional<MilliFunds>{evaluation.objective->lower} : std::nullopt)},
            {"upper", masterValueRecord(evaluation.objective
                ? std::optional<MilliFunds>{evaluation.objective->upper} : std::nullopt)}};
        if (evaluation.replay)
        {
            QJsonArray edges;
            for (std::int32_t index = 0; index < evaluation.replay->plan.edgeCount(); ++index)
            {
                const auto & edge = evaluation.replay->plan.edge(index);
                edges.append(QJsonArray{edge.from, edge.to, static_cast<int>(edge.kind)});
            }
            record.insert("edges", edges);
            record.insert("executionOrder", masterChoicesRecord(evaluation.replay->ordering.order));
            record.insert("actionIndices", masterChoicesRecord(evaluation.replay->actionIndices));
        }
        if (const auto* quote = evaluation.quote())
        {
            const auto & proof = *evaluation.proof;
            QJsonArray cheapWitness;
            for (const auto & row : proof.cheapOurWitness)
            {
                cheapWitness.append(QJsonObject{{"row", row.row}, {"nodes", masterChoicesRecord(row.nodes)},
                    {"value", QString::number(row.value)}});
            }
            QJsonObject quoted{
                {"key", keyRecord(quote->key)}, {"capturedColumns", masterChoicesRecord(quote->capturedColumns)},
                {"economic", QString::number(quote->economic)}, {"owned", QString::number(quote->owned)},
                {"ours", QJsonArray{QString::number(quote->ours.lower), QString::number(quote->ours.upper)}},
                {"enemy", QJsonArray{QString::number(quote->enemy.lower), QString::number(quote->enemy.upper)}},
                {"ourSearchCompleted", quote->ourSearchCompleted}, {"enemySearchCompleted", quote->enemySearchCompleted},
                {"ourUpperCertificateId", QString::number(quote->ourUpperCertificateId)},
                {"ourWitnessKind", static_cast<int>(proof.ourKind)},
                {"cheapOurWitness", cheapWitness},
                {"ourWitness", sequentialWitnessRecord(proof.ourWitness)},
                {"enemyWitness", sequentialWitnessRecord(proof.enemyWitness)}};
            if (proof.ourUpperCertificate)
            {
                quoted.insert("ownedUpperCertificatePayload", packingCertificateRecord(*proof.ourUpperCertificate));
            }
            record.insert("quote", quoted);
        }
        return record;
    }

    QJsonObject masterClaimsRecord(const ActionReservations & claims)
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
                {"services", masterChoicesRecord(claims.services)}, {"damage", damage}};
    }

    QJsonObject masterProfileRecord(const MasterAssignmentProfile & profile)
    {
        QJsonObject result;
        for (std::size_t phase = 0; phase < profile.samples.size(); ++phase)
        {
            const auto & sample = profile.samples[phase];
            result.insert(QLatin1String(MASTER_PHASE_NAMES[phase]), QJsonObject{
                {"calls", QString::number(sample.calls)}, {"inclusiveNanos", QString::number(sample.inclusiveNanos)},
                {"exclusiveNanos", QString::number(sample.exclusiveNanos)}});
        }
        return result;
    }
}
