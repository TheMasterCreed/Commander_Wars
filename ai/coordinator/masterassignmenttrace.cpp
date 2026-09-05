#include "ai/coordinator/masterassignmenttrace.h"

namespace
{
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
            "masterDualVerification", "masterValuation", "masterWitness", "masterReplay",
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
            {"evaluationCacheHits", QString::number(result.stats.evaluationCacheHits)},
            {"infeasible", QString::number(result.stats.infeasible)}, {"pruned", QString::number(result.stats.pruned)},
            {"unsupported", QString::number(result.stats.unsupported)},
            {"arithmeticFailures", QString::number(result.stats.arithmeticFailures)},
            {"proofFailures", QString::number(result.stats.proofFailures)},
            {"firstImprovementWork", masterValueRecord(result.stats.firstImprovementWork)},
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
                    {"actor", action.unitId}, {"actionId", action.actionId}, {"kind", static_cast<int>(action.kind)},
                    {"destination", tile(action.destination)}, {"target", tile(action.target)},
                    {"targetUnit", action.targetUnitId}, {"path", path}, {"state", static_cast<int>(action.state)},
                    {"damage", action.plannedDamageSteps}});
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
        if (const auto* quote = evaluation.quote())
        {
            const auto & proof = *evaluation.proof;
            QJsonArray cheapWitness;
            for (const auto & row : proof.cheapOurWitness)
            {
                cheapWitness.append(QJsonObject{{"row", row.row}, {"nodes", masterChoicesRecord(row.nodes)},
                    {"value", QString::number(row.value)}});
            }
            record.insert("quote", QJsonObject{
                {"key", keyRecord(quote->key)}, {"capturedColumns", masterChoicesRecord(quote->capturedColumns)},
                {"economic", QString::number(quote->economic)}, {"owned", QString::number(quote->owned)},
                {"ours", QJsonArray{QString::number(quote->ours.lower), QString::number(quote->ours.upper)}},
                {"enemy", QJsonArray{QString::number(quote->enemy.lower), QString::number(quote->enemy.upper)}},
                {"ourSearchCompleted", quote->ourSearchCompleted}, {"enemySearchCompleted", quote->enemySearchCompleted},
                {"ourUpperCertificateId", QString::number(quote->ourUpperCertificateId)},
                {"ourWitnessKind", static_cast<int>(proof.ourKind)},
                {"cheapOurWitness", cheapWitness},
                {"ourWitness", sequentialWitnessRecord(proof.ourWitness)},
                {"enemyWitness", sequentialWitnessRecord(proof.enemyWitness)}});
        }
        return record;
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
