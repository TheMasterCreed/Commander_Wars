#include "ai/coordinator/masterassignmentevaluator.h"

#include <algorithm>
#include <limits>

namespace Coordinator
{
    MasterAssignmentEvaluator::MasterAssignmentEvaluator(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger)
        : m_problem(problem),
          m_problemIdentity(problem.identity()),
          m_ledger(ledger)
    {
    }

    MasterAssignmentEvaluator::MasterAssignmentEvaluator(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger,
        RelativeStockSession & session, MasterValuationTariffs tariffs)
        : m_problem(problem),
          m_problemIdentity(problem.identity()),
          m_ledger(ledger),
          m_session(&session),
          m_tariffs(tariffs)
    {
    }

    bool MasterAssignmentEvaluator::hasContinuation() const
    {
        return m_session != nullptr;
    }

    const CheapPricingModel* MasterAssignmentEvaluator::stockModel() const
    {
        return m_session == nullptr ? nullptr : m_session->pricingModel();
    }

    const MasterFutureCatalog* MasterAssignmentEvaluator::futureCatalog() const
    {
        return m_session == nullptr ? nullptr : m_session->futureCatalog();
    }

    bool MasterAssignmentEvaluator::matches(
        const MasterAssignmentProblem & problem, const RefinementLedger & ledger) const
    {
        return m_problemIdentity.sameLiveProblem(m_problem.identity()) &&
               m_problemIdentity.sameLiveProblem(problem.identity()) && &m_ledger == &ledger;
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::replayTariff(
        const MasterAssignmentState & state)
    {
        if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 1,
                              RefinementWork::MasterReplay))
        {
            return std::nullopt;
        }
        std::int64_t payload = 0;
        for (std::size_t actor = 0; actor < state.choices.size(); ++actor)
        {
            if (state.choices[actor] == NO_CANDIDATE)
            {
                continue;
            }
            const auto & candidate = m_problem.actors()[actor].candidates[
                static_cast<std::size_t>(state.choices[actor])];
            const auto work = masterWorkProduct(candidate.source.bundle.path.size() +
                candidate.source.bundle.components.size() +
                candidate.resources.size() + 1, 1);
            if (!work || !RelativeStockDetail::checkedAdd(payload, *work))
            {
                return std::nullopt;
            }
        }
        const auto actions = m_problem.actors().size() + m_problem.actionTemplates().size() +
                             m_problem.incumbentEdges().size() + m_problem.unitLinks().size() + 1;
        const auto structural = masterWorkProduct(actions, actions);
        if (!structural || !RelativeStockDetail::checkedAdd(payload, *structural))
        {
            return std::nullopt;
        }
        return payload;
    }

    MasterAssignmentEvaluation MasterAssignmentEvaluator::evaluate(const MasterAssignmentState & state)
    {
        MasterAssignmentEvaluation result;
        if (!m_problemIdentity.sameLiveProblem(m_problem.identity()))
        {
            return result;
        }
        if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 1, RefinementWork::MasterReplay))
        {
            result.status = MasterEvaluationStatus::BudgetExhausted;
            return result;
        }
        const bool incumbent = state.identity.sameLiveProblem(m_problem.identity()) &&
            std::equal(state.choices.begin(), state.choices.end(),
                       m_problem.incumbentChoices().begin(), m_problem.incumbentChoices().end());
        if ((!incumbent && !validMasterState(m_problem, state)) || !state.complete())
        {
            return result;
        }
        const auto tariff = replayTariff(state);
        if (!tariff || m_ledger.draw(*tariff, RefinementWork::MasterReplay) == NO_GRANT)
        {
            result.status = MasterEvaluationStatus::BudgetExhausted;
            return result;
        }
        result.state = state;
        result.replay = replayMasterAssignment(m_problem, state.choices);
        if (result.replay->error == MasterAssignmentReplayError::UnsupportedDependency)
        {
            result.status = MasterEvaluationStatus::Unsupported;
            return result;
        }
        if (result.replay->error == MasterAssignmentReplayError::InvalidValuation)
        {
            result.status = MasterEvaluationStatus::ArithmeticFailure;
            return result;
        }
        if (!result.replay->complete())
        {
            result.status = MasterEvaluationStatus::Infeasible;
            return result;
        }
        result.status = MasterEvaluationStatus::Feasible;
        price(result);
        return result;
    }

    bool MasterAssignmentEvaluator::price(MasterAssignmentEvaluation & evaluation)
    {
        if (!evaluation.replay || !evaluation.replay->complete() ||
            !m_problemIdentity.sameLiveProblem(m_problem.identity()) ||
            !evaluation.state.identity.sameLiveProblem(m_problemIdentity))
        {
            return false;
        }
        const auto economic = evaluation.replay->seatedEconomicTotal;
        if (m_session == nullptr)
        {
            evaluation.proof.reset();
            evaluation.objective = StockInterval{economic, economic};
            evaluation.objectiveStatus = MasterObjectiveStatus::Verified;
            evaluation.lowerReplayed = true;
            evaluation.upperVerified = true;
            evaluation.exact = true;
            return true;
        }
        if (m_tariffs.quote < 0 || m_tariffs.witness < 0 ||
            m_ledger.balance() != m_session->remainingWork())
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::InvalidIdentity;
            return false;
        }
        if (m_ledger.draw(m_tariffs.quote, RefinementWork::MasterValuation) == NO_GRANT)
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::BudgetExhausted;
            return false;
        }
        auto quote = m_session->quote(evaluation.replay->continuationKey, economic);
        if (!m_session->owns(quote) || quote.key != evaluation.replay->continuationKey ||
            quote.economic != economic || !m_session->compare(quote, quote).valid())
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::InvalidIdentity;
            return false;
        }
        if (m_ledger.draw(m_tariffs.witness, RefinementWork::MasterWitness) == NO_GRANT)
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::BudgetExhausted;
            return false;
        }
        auto proof = m_session->captureProof(quote);
        if (!proof || !sameRelativeStockQuote(proof->quote, quote) || !m_session->replayLowerProof(*proof))
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::WitnessFailure;
            return false;
        }
        StockInterval value{economic, economic};
        if (!RelativeStockDetail::checkedAdd(value.lower, quote.owned) ||
            !RelativeStockDetail::checkedAdd(value.upper, quote.owned) ||
            !RelativeStockDetail::checkedAdd(value.lower, quote.ours.lower) ||
            !RelativeStockDetail::checkedAdd(value.upper, quote.ours.upper) ||
            !RelativeStockDetail::checkedSubtract(value.lower, quote.enemy.upper) ||
            !RelativeStockDetail::checkedSubtract(value.upper, quote.enemy.lower))
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::ArithmeticFailure;
            return false;
        }
        evaluation.proof = std::move(proof);
        evaluation.objectiveStatus = MasterObjectiveStatus::Verified;
        evaluation.objective = value;
        evaluation.lowerReplayed = true;
        evaluation.upperVerified = true;
        evaluation.exact = evaluation.quote()->absoluteExact();
        return true;
    }

    bool MasterAssignmentEvaluator::refresh(MasterAssignmentEvaluation & evaluation)
    {
        return evaluation.status == MasterEvaluationStatus::Feasible && price(evaluation);
    }

    MasterRefinementStatus MasterAssignmentEvaluator::refine(
        MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term, std::int64_t allowance)
    {
        const auto* quote = evaluation.quote();
        if (m_session == nullptr || quote == nullptr || allowance <= 0 ||
            m_ledger.balance() != m_session->remainingWork())
        {
            return MasterRefinementStatus::Unavailable;
        }
        auto overhead = m_tariffs.quote;
        if (overhead < 0 || m_tariffs.witness < 0 ||
            !RelativeStockDetail::checkedAdd(overhead, m_tariffs.witness))
        {
            return MasterRefinementStatus::Invalid;
        }
        if (term == MasterRefinementTerm::FutureDual)
        {
            if (!m_session->supportsVerifiedDual())
            {
                return MasterRefinementStatus::Unavailable;
            }
            auto total = allowance;
            auto preflight = m_session->certificatePreparationWork();
            if (preflight < 0 || m_tariffs.certificateIdentity < 0 ||
                !RelativeStockDetail::checkedAdd(preflight, m_tariffs.certificateIdentity) ||
                !RelativeStockDetail::checkedAdd(preflight, m_tariffs.witness) ||
                !RelativeStockDetail::checkedAdd(total, overhead) ||
                !RelativeStockDetail::checkedAdd(total, preflight))
            {
                return MasterRefinementStatus::Invalid;
            }
            if (m_ledger.balance() < total)
            {
                return MasterRefinementStatus::BudgetExhausted;
            }
            if (m_ledger.draw(preflight, RefinementWork::MasterValuation) == NO_GRANT)
            {
                return MasterRefinementStatus::BudgetExhausted;
            }
            const auto tightened = m_session->tightenOurUpperWithVerifiedDual(*quote, allowance);
            if (tightened.globalFailure)
            {
                return MasterRefinementStatus::Invalid;
            }
            return refresh(evaluation) ? MasterRefinementStatus::Updated : MasterRefinementStatus::Invalid;
        }
        const auto tariff = term == MasterRefinementTerm::Friendly ? m_session->ourRerunWork()
            : m_session->enemyRerunWork(quote->capturedColumns);
        auto total = allowance;
        if (tariff < 0 || !RelativeStockDetail::checkedAdd(total, tariff) ||
            !RelativeStockDetail::checkedAdd(total, overhead) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.quote))
        {
            return MasterRefinementStatus::Invalid;
        }
        if (m_ledger.balance() < total)
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        if (m_ledger.draw(m_tariffs.quote, RefinementWork::MasterValuation) == NO_GRANT)
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        const auto refined = term == MasterRefinementTerm::Friendly
            ? m_session->refineOur(quote->key, allowance)
            : m_session->refineEnemy(quote->capturedColumns, allowance);
        return refined.valid && refresh(evaluation)
            ? MasterRefinementStatus::Updated : MasterRefinementStatus::Invalid;
    }

    RelativeStockComparison MasterAssignmentEvaluator::compare(
        MasterAssignmentEvaluation & incumbent, MasterAssignmentEvaluation & challenger)
    {
        if (!refresh(incumbent) || !refresh(challenger) ||
            !incumbent.objective || !challenger.objective)
        {
            return {};
        }
        if (m_session != nullptr)
        {
            return m_session->compare(*incumbent.quote(), *challenger.quote());
        }
        RelativeStockComparison result;
        result.deltaLower = challenger.objective->lower;
        result.deltaUpper = challenger.objective->upper;
        if (!RelativeStockDetail::checkedSubtract(result.deltaLower, incumbent.objective->upper) ||
            !RelativeStockDetail::checkedSubtract(result.deltaUpper, incumbent.objective->lower))
        {
            result.reason = RelativeStockReason::ArithmeticOverflow;
            return result;
        }
        result.reason = result.deltaLower > 0 ? RelativeStockReason::CertifiedPositive
            : result.deltaUpper <= 0 ? RelativeStockReason::CertifiedNonImprovement : RelativeStockReason::Overlap;
        return result;
    }
}
