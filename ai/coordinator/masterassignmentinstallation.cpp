#include "ai/coordinator/masterassignmentinstallation.h"

#include <limits>
#include <type_traits>

#include "ai/coordinator/bundleassignment.h"

namespace
{
    std::optional<std::int64_t> repeatedWork(std::int64_t units, std::int64_t passes)
    {
        if (units < 0 || passes <= 0 || units > std::numeric_limits<std::int64_t>::max() / passes)
        {
            return std::nullopt;
        }
        return units * passes;
    }

    bool verified(const Coordinator::MasterAssignmentEvaluation & evaluation)
    {
        return evaluation.status == Coordinator::MasterEvaluationStatus::Feasible &&
            evaluation.objectiveStatus == Coordinator::MasterObjectiveStatus::Verified &&
            evaluation.replay && evaluation.replay->complete() && evaluation.objective &&
            evaluation.lowerReplayed && evaluation.upperVerified;
    }

    Coordinator::MasterInstallationStatus failure(const Coordinator::MasterAssignmentEvaluation & evaluation)
    {
        using namespace Coordinator;
        if (evaluation.status == MasterEvaluationStatus::BudgetExhausted ||
            evaluation.objectiveStatus == MasterObjectiveStatus::BudgetExhausted)
        {
            return MasterInstallationStatus::BudgetExhausted;
        }
        if (evaluation.status == MasterEvaluationStatus::ArithmeticFailure ||
            evaluation.objectiveStatus == MasterObjectiveStatus::ArithmeticFailure)
        {
            return MasterInstallationStatus::ArithmeticFailure;
        }
        return evaluation.status == MasterEvaluationStatus::Feasible
            ? MasterInstallationStatus::ProofFailure : MasterInstallationStatus::ReplayFailure;
    }

    bool sameNominee(const Coordinator::MasterAssignmentEvaluation & fresh,
                     const Coordinator::MasterAssignmentEvaluation & retained)
    {
        return retained.replay && fresh.replay &&
            fresh.replay->continuationKey == retained.replay->continuationKey &&
            fresh.replay->seatedEconomicTotal == retained.replay->seatedEconomicTotal &&
            fresh.replay->plan.equivalentState(retained.replay->plan) &&
            fresh.replay->ordering.order == retained.replay->ordering.order;
    }

    bool recordedComparisonMatches(const Coordinator::MasterImprovementProof & proof)
    {
        using namespace Coordinator;
        RelativeStockComparison comparison;
        if (proof.incumbent.quote() && proof.challenger.quote())
        {
            auto incumbent = *proof.incumbent.quote();
            auto challenger = *proof.challenger.quote();
            incumbent.economic = proof.incumbent.replay->seatedEconomicTotal;
            challenger.economic = proof.challenger.replay->seatedEconomicTotal;
            comparison = compareRelativeStock(incumbent, challenger);
        }
        else
        {
            comparison.economicDelta = proof.challenger.replay->seatedEconomicTotal;
            if (!RelativeStockDetail::checkedSubtract(
                    comparison.economicDelta, proof.incumbent.replay->seatedEconomicTotal))
            {
                return false;
            }
            comparison.deltaLower = comparison.economicDelta;
            comparison.deltaUpper = comparison.economicDelta;
            comparison.reason = comparison.deltaLower > 0 ? RelativeStockReason::CertifiedPositive
                                                         : RelativeStockReason::CertifiedNonImprovement;
        }
        return comparison.improves() && comparison.deltaLower == proof.comparison.deltaLower &&
            comparison.deltaUpper == proof.comparison.deltaUpper &&
            comparison.economicDelta == proof.comparison.economicDelta &&
            comparison.ownedDelta == proof.comparison.ownedDelta &&
            comparison.commonEnemyCancelled == proof.comparison.commonEnemyCancelled &&
            comparison.reason == proof.comparison.reason;
    }
}

namespace Coordinator
{
    MasterAssignmentInstallation::MasterAssignmentInstallation(
        const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator, RefinementLedger & ledger)
        : m_problem(problem), m_identity(problem.identity()), m_evaluator(evaluator), m_ledger(ledger)
    {
    }

    MasterInstallationStatus MasterAssignmentInstallation::prepare(const AssignmentResult & incoming)
    {
        if (m_status != MasterInstallationStatus::Unprepared || !m_evaluator.matches(m_problem, m_ledger))
        {
            return MasterInstallationStatus::InvalidContext;
        }
        const auto graph = m_problem.actors().size() + incoming.selections.size() +
            static_cast<std::size_t>(incoming.plan.actionCount()) +
            static_cast<std::size_t>(incoming.plan.edgeCount()) + incoming.executionOrder.size() + 1;
        if (!chargeMasterWork(m_ledger, graph, 1, RefinementWork::MasterPreparation))
        {
            m_status = MasterInstallationStatus::BudgetExhausted;
            return m_status;
        }
        std::int64_t payload = 0;
        for (const auto & selection : incoming.selections)
        {
            const auto count = masterWorkProduct(selection.completeValues.size() + 1, 1);
            if (!count || !RelativeStockDetail::checkedAdd(payload, *count))
            {
                m_status = MasterInstallationStatus::ArithmeticFailure;
                return m_status;
            }
            m_incomingDiagnosticValues += selection.completeValues.size();
        }
        for (const auto & actor : m_problem.actors())
        {
            if (!chargeMasterWork(m_ledger, actor.candidates.size() + 1, 1, RefinementWork::MasterPreparation))
            {
                m_status = MasterInstallationStatus::BudgetExhausted;
                return m_status;
            }
            std::size_t maximumPath = 0;
            for (const auto & candidate : actor.candidates)
            {
                maximumPath = std::max(maximumPath, candidate.source.bundle.path.size());
            }
            const auto count = masterWorkProduct(maximumPath + 1, 1);
            if (!count || !RelativeStockDetail::checkedAdd(payload, *count))
            {
                m_status = MasterInstallationStatus::ArithmeticFailure;
                return m_status;
            }
        }
        for (std::int32_t index = 0; index < incoming.plan.actionCount(); ++index)
        {
            const auto count = masterWorkProduct(incoming.plan.action(index).path.size() + 1, 1);
            if (!count || !RelativeStockDetail::checkedAdd(payload, *count))
            {
                m_status = MasterInstallationStatus::ArithmeticFailure;
                return m_status;
            }
        }
        constexpr std::int64_t PUBLICATION_PASSES = 16;
        constexpr std::int64_t PAYLOAD_COPY_PASSES = 3;
        const auto structural = masterWorkProduct(graph, graph);
        const auto publication = structural ? repeatedWork(*structural, PUBLICATION_PASSES) : std::nullopt;
        const auto copies = repeatedWork(payload, PAYLOAD_COPY_PASSES);
        const auto evaluation = m_evaluator.finalizationWork();
        m_publicationWork = publication.value_or(0);
        if (!publication || !copies || !evaluation ||
            !RelativeStockDetail::checkedAdd(m_publicationWork, *copies))
        {
            m_status = m_ledger.lastRefusal() ? MasterInstallationStatus::BudgetExhausted
                                            : MasterInstallationStatus::ArithmeticFailure;
            return m_status;
        }
        m_reservedWork = m_publicationWork;
        if (!RelativeStockDetail::checkedAdd(m_reservedWork, *evaluation))
        {
            m_status = MasterInstallationStatus::ArithmeticFailure;
            return m_status;
        }
        m_reservation = m_ledger.draw(m_reservedWork, RefinementWork::MasterInstallation);
        m_status = m_reservation == NO_GRANT ? MasterInstallationStatus::BudgetExhausted
                                           : MasterInstallationStatus::Reserved;
        return m_status;
    }

    std::int64_t MasterAssignmentInstallation::reservedWork() const
    {
        return m_reservation == NO_GRANT ? 0 : m_reservedWork;
    }

    bool MasterAssignmentInstallation::matchesIncoming(
        const AssignmentResult & incoming, const MasterAssignmentEvaluation & evaluated) const
    {
        if (!verified(evaluated) || incoming.selections.size() != m_problem.actors().size() ||
            incoming.executionOrder != evaluated.replay->ordering.order ||
            !incoming.plan.equivalentState(evaluated.replay->plan) ||
            checkedAssignmentEconomicTotal(incoming) != m_problem.incumbentEconomicTotal())
        {
            return false;
        }
        std::size_t diagnosticValues = 0;
        for (const auto & selected : incoming.selections)
        {
            if (selected.completeValues.size() > m_incomingDiagnosticValues - diagnosticValues)
            {
                return false;
            }
            diagnosticValues += selected.completeValues.size();
        }
        for (const auto & actor : m_problem.actors())
        {
            const auto selected = std::find_if(incoming.selections.begin(), incoming.selections.end(),
                [&](const auto & entry) { return entry.engineUnitId == actor.engineUnitId; });
            if (selected == incoming.selections.end() || selected->knowledgeUnitIndex != actor.knowledgeUnitIndex ||
                selected->actionIndex != actor.actionIndex ||
                selected->candidateIndex != actor.incumbentCandidateIndex ||
                selected->stockCoupled != (actor.role == MasterActorRole::Mutable) ||
                selected->seatedValue != actor.incumbentSeatedValue)
            {
                return false;
            }
        }
        return true;
    }

    MasterInstallationResult MasterAssignmentInstallation::finish(
        const std::optional<MasterImprovementProof> & retained, AssignmentResult & incoming)
    {
        MasterInstallationResult result;
        result.status = m_status;
        result.reservedWork = reservedWork();
        if (m_status != MasterInstallationStatus::Reserved)
        {
            return result;
        }
        m_status = MasterInstallationStatus::InvalidContext;
        if (!m_ledger.refund(m_reservation, m_reservedWork))
        {
            result.status = MasterInstallationStatus::BudgetExhausted;
            return result;
        }
        const auto before = m_ledger.stats().spent();
        const auto complete = [&](MasterInstallationStatus status)
        {
            result.status = status;
            result.finalizationWork = m_ledger.stats().spent() - before;
            return std::move(result);
        };
        if (!retained)
        {
            return complete(MasterInstallationStatus::NoImprovement);
        }
        if (!m_ledger.consume(m_publicationWork, RefinementWork::MasterInstallation))
        {
            return complete(MasterInstallationStatus::BudgetExhausted);
        }
        if (!m_identity.sameLiveProblem(m_problem.identity()) || !m_evaluator.matches(m_problem, m_ledger) ||
            !m_evaluator.ownsRetainedEvaluation(retained->incumbent) ||
            !m_evaluator.ownsRetainedEvaluation(retained->challenger) ||
            !std::equal(retained->incumbent.state.choices.begin(), retained->incumbent.state.choices.end(),
                        m_problem.incumbentChoices().begin(), m_problem.incumbentChoices().end()) ||
            !recordedComparisonMatches(*retained))
        {
            return complete(MasterInstallationStatus::InvalidContext);
        }
        for (const auto* evaluation : {&retained->incumbent, &retained->challenger})
        {
            const auto status = m_evaluator.replayRetainedEvaluation(*evaluation);
            if (status != MasterObjectiveStatus::Verified)
            {
                return complete(status == MasterObjectiveStatus::BudgetExhausted
                    ? MasterInstallationStatus::BudgetExhausted : MasterInstallationStatus::ProofFailure);
            }
        }
        auto incumbent = m_evaluator.evaluate(retained->incumbent.state);
        if (!verified(incumbent))
        {
            return complete(failure(incumbent));
        }
        if (!sameNominee(incumbent, retained->incumbent))
        {
            return complete(MasterInstallationStatus::InvalidContext);
        }
        if (!matchesIncoming(incoming, incumbent))
        {
            return complete(MasterInstallationStatus::IncomingChanged);
        }
        auto challenger = m_evaluator.evaluate(retained->challenger.state);
        if (!verified(challenger))
        {
            return complete(failure(challenger));
        }
        if (!sameNominee(challenger, retained->challenger))
        {
            return complete(MasterInstallationStatus::InvalidContext);
        }
        const auto comparison = m_evaluator.compare(incumbent, challenger);
        if (!verified(incumbent) || !verified(challenger) || !comparison.valid())
        {
            return complete(MasterInstallationStatus::ProofFailure);
        }
        if (!comparison.improves())
        {
            return complete(MasterInstallationStatus::NonImprovement);
        }
        AssignmentResult replacement = incoming;
        for (const auto & actor : m_problem.actors())
        {
            if (!m_problem.canChangeActor(actor.canonicalSlot))
            {
                continue;
            }
            auto selected = std::find_if(replacement.selections.begin(), replacement.selections.end(),
                [&](const auto & entry) { return entry.engineUnitId == actor.engineUnitId; });
            const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
            selected->candidateIndex = challenger.state.choices[slot];
            selected->actionIndex = challenger.replay->actionIndices[slot];
            selected->seatedValue = challenger.replay->seatedValues[slot];
            selected->phase = AssignmentResult::SelectionPhase::Master;
            selected->completeValues.clear();
        }
        replacement.plan = challenger.replay->plan;
        replacement.executionOrder = challenger.replay->ordering.order;
        result.proof = MasterImprovementProof{
            std::move(incumbent), std::move(challenger), comparison, m_ledger.stats().spent()};
        static_assert(std::is_nothrow_move_assignable_v<AssignmentResult>);
        incoming = std::move(replacement);
        return complete(MasterInstallationStatus::Installed);
    }
}
