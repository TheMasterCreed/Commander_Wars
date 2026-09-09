#include "ai/coordinator/masterassignmentevaluator.h"

#include <algorithm>
#include <limits>

namespace Coordinator
{
    MasterAssignmentEvaluator::MasterAssignmentEvaluator(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger, MasterAssignmentProfile* profile)
        : m_problem(problem),
          m_problemIdentity(problem.identity()),
          m_ledger(ledger),
          m_profile(profile)
    {
    }

    MasterAssignmentEvaluator::MasterAssignmentEvaluator(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger,
        RelativeStockSession & session, MasterValuationTariffs tariffs, MasterAssignmentProfile* profile)
        : m_problem(problem),
          m_problemIdentity(problem.identity()),
          m_ledger(ledger),
          m_session(&session),
          m_tariffs(tariffs),
          m_profile(profile)
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

    bool MasterAssignmentEvaluator::ownsRetainedEvaluation(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto actorCount = m_problem.actors().size();
        if (!m_problemIdentity.sameLiveProblem(m_problem.identity()) ||
            !m_problemIdentity.sameLiveProblem(evaluation.state.identity) ||
            evaluation.status != MasterEvaluationStatus::Feasible ||
            evaluation.objectiveStatus != MasterObjectiveStatus::Verified ||
            !evaluation.replay || !evaluation.replay->complete() || !evaluation.objective ||
            !evaluation.lowerReplayed || !evaluation.upperVerified ||
            evaluation.objective->lower > evaluation.objective->upper ||
            evaluation.state.choices.size() != actorCount ||
            evaluation.replay->actionIndices.size() != actorCount ||
            evaluation.replay->seatedValues.size() != actorCount ||
            evaluation.replay->continuationKey.size() > actorCount || !evaluation.state.complete())
        {
            return false;
        }
        if (m_session == nullptr)
        {
            return !evaluation.proof && evaluation.exact &&
                evaluation.objective->lower == evaluation.replay->seatedEconomicTotal &&
                evaluation.objective->upper == evaluation.replay->seatedEconomicTotal;
        }
        const auto quote = evaluation.quote();
        return quote != nullptr && quote->key.size() <= actorCount &&
            quote->capturedColumns.size() <= actorCount && m_session->owns(*quote) &&
            quote->valid && quote->economic == 0 &&
            quote->ourLowerWitnessReplays && quote->enemyLowerWitnessReplays &&
            RelativeStockDetail::coherent(*quote) && quote->key == evaluation.replay->continuationKey;
    }

    bool MasterAssignmentEvaluator::retainedWitnessFits(
        const RelativeStockProofSnapshot & proof, std::int64_t work)
    {
        const auto subtract = [&](std::size_t count)
        {
            const auto units = masterWorkProduct(count, 1);
            return units && RelativeStockDetail::checkedSubtract(work, *units) && work >= 0;
        };
        if (!subtract(proof.cheapOurWitness.size()) || !subtract(proof.ourWitness.size()) ||
            !subtract(proof.enemyWitness.size()))
        {
            return false;
        }
        for (const auto & row : proof.cheapOurWitness)
        {
            if (!subtract(row.nodes.size()))
            {
                return false;
            }
        }
        for (const auto* rows : {&proof.ourWitness, &proof.enemyWitness})
        {
            for (const auto & row : *rows)
            {
                if (!subtract(row.steps.size()))
                {
                    return false;
                }
            }
        }
        return true;
    }

    MasterObjectiveStatus MasterAssignmentEvaluator::replayRetainedEvaluation(
        const MasterAssignmentEvaluation & evaluation)
    {
        if (!ownsRetainedEvaluation(evaluation))
        {
            return MasterObjectiveStatus::InvalidIdentity;
        }
        if (m_session == nullptr)
        {
            return MasterObjectiveStatus::Verified;
        }
        if (m_tariffs.witness < 0 || m_ledger.balance() != m_session->remainingWork())
        {
            return MasterObjectiveStatus::InvalidIdentity;
        }
        if (!m_ledger.consume(m_tariffs.witness, RefinementWork::MasterWitness))
        {
            return MasterObjectiveStatus::BudgetExhausted;
        }
        MasterProfileScope timing(m_profile, MasterPhase::LowerWitness);
        if (!retainedWitnessFits(*evaluation.proof, m_tariffs.witness) ||
            !m_session->replayLowerProof(*evaluation.proof))
        {
            return MasterObjectiveStatus::WitnessFailure;
        }
        return MasterObjectiveStatus::Verified;
    }

    const std::optional<MasterRefinementRefusal> & MasterAssignmentEvaluator::refinementRefusal() const
    {
        return m_refinementRefusal;
    }

    const std::optional<RelativeStockCertificatePreparation> &
    MasterAssignmentEvaluator::certificatePreparation() const
    {
        return m_certificatePreparation;
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::candidateReplayWork(const MasterCandidate & candidate)
    {
        return masterWorkProduct(candidate.source.bundle.path.size() +
            candidate.source.bundle.components.size() + candidate.resources.size() + 1, 1);
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::replayWork(std::int64_t payload) const
    {
        const auto actions = m_problem.actors().size() + m_problem.actionTemplates().size() +
                             m_problem.incumbentEdges().size() + m_problem.unitLinks().size() + 1;
        const auto structural = masterWorkProduct(actions, actions);
        if (!structural || !RelativeStockDetail::checkedAdd(payload, *structural))
        {
            return std::nullopt;
        }
        return payload;
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::finalizationWork()
    {
        if (!m_problemIdentity.sameLiveProblem(m_problem.identity()) ||
            (m_session != nullptr && (m_tariffs.quote < 0 || m_tariffs.witness < 0 ||
                                     m_session->remainingWork() != m_ledger.balance())) ||
            !chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 1, RefinementWork::MasterReplay))
        {
            return std::nullopt;
        }
        std::int64_t payload = 0;
        for (const auto & actor : m_problem.actors())
        {
            if (!chargeMasterWork(m_ledger, actor.candidates.size() + 1, 1, RefinementWork::MasterReplay))
            {
                return std::nullopt;
            }
            std::int64_t largest = 0;
            for (const auto & candidate : actor.candidates)
            {
                const auto work = candidateReplayWork(candidate);
                if (!work)
                {
                    return std::nullopt;
                }
                largest = std::max(largest, *work);
            }
            if (!RelativeStockDetail::checkedAdd(payload, largest))
            {
                return std::nullopt;
            }
        }
        constexpr std::size_t evaluationCalls = 2;
        constexpr std::size_t scansPerEvaluation = 2;
        constexpr std::size_t comparisonRefreshes = 2;
        constexpr std::size_t retainedReplays = 2;
        auto total = masterWorkProduct(m_problem.actors().size() + 1, evaluationCalls * scansPerEvaluation);
        const auto replay = replayWork(payload);
        if (!total || !replay)
        {
            return std::nullopt;
        }
        for (std::size_t call = 0; call < evaluationCalls; ++call)
        {
            if (!RelativeStockDetail::checkedAdd(*total, *replay))
            {
                return std::nullopt;
            }
        }
        if (m_session != nullptr)
        {
            for (std::size_t call = 0; call < evaluationCalls + comparisonRefreshes; ++call)
            {
                if (!RelativeStockDetail::checkedAdd(*total, m_tariffs.quote) ||
                    !RelativeStockDetail::checkedAdd(*total, m_tariffs.witness))
                {
                    return std::nullopt;
                }
            }
            for (std::size_t call = 0; call < retainedReplays; ++call)
            {
                if (!RelativeStockDetail::checkedAdd(*total, m_tariffs.witness))
                {
                    return std::nullopt;
                }
            }
        }
        return total;
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
            const auto work = candidateReplayWork(candidate);
            if (!work || !RelativeStockDetail::checkedAdd(payload, *work))
            {
                return std::nullopt;
            }
        }
        return replayWork(payload);
    }

    MasterAssignmentEvaluation MasterAssignmentEvaluator::evaluate(const MasterAssignmentState & state)
    {
        MasterProfileScope timing(m_profile, MasterPhase::LeafReplay);
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
        MasterProfileScope timing(m_profile, MasterPhase::Quote);
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
        // Continuation-keyed certificates exclude the separate seated economic term.
        auto quote = m_session->quote(evaluation.replay->continuationKey, 0);
        if (!m_session->owns(quote) || quote.key != evaluation.replay->continuationKey ||
            quote.economic != 0 || !m_session->compare(quote, quote).valid())
        {
            evaluation.objectiveStatus = MasterObjectiveStatus::InvalidIdentity;
            return false;
        }
        auto proof = evaluation.proof;
        if (!proof || !evaluation.lowerReplayed || !evaluation.upperVerified ||
            !sameRelativeStockQuote(proof->quote, quote))
        {
            if (m_ledger.draw(m_tariffs.witness, RefinementWork::MasterWitness) == NO_GRANT)
            {
                evaluation.objectiveStatus = MasterObjectiveStatus::BudgetExhausted;
                return false;
            }
            MasterProfileScope witnessTiming(m_profile, MasterPhase::LowerWitness);
            proof = m_session->captureProof(quote);
            if (!proof || !sameRelativeStockQuote(proof->quote, quote) || !m_session->replayLowerProof(*proof))
            {
                evaluation.objectiveStatus = MasterObjectiveStatus::WitnessFailure;
                return false;
            }
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

    MasterRefinementStatus MasterAssignmentEvaluator::refineCertificate(
        MasterAssignmentEvaluation & evaluation, std::int64_t allowance)
    {
        if (!m_session->supportsVerifiedDual())
        {
            return MasterRefinementStatus::Unavailable;
        }
        const auto balanceBefore = m_ledger.balance();
        m_certificatePreparation = m_session->prepareOurCertificate(*evaluation.quote());
        const auto & preparation = *m_certificatePreparation;
        if (m_session->remainingWork() != m_ledger.balance() || preparation.readinessWork < 0 ||
            preparation.readinessWork != balanceBefore - m_ledger.balance() || preparation.scanWork < 0)
        {
            return MasterRefinementStatus::Invalid;
        }
        if (preparation.status == RelativeStockPreparationStatus::BudgetExhausted)
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        if (preparation.status == RelativeStockPreparationStatus::MissingRows)
        {
            return MasterRefinementStatus::Unavailable;
        }
        if (preparation.status != RelativeStockPreparationStatus::Ready)
        {
            return MasterRefinementStatus::Invalid;
        }
        auto preflight = preparation.scanWork;
        auto total = allowance;
        if (m_tariffs.certificateIdentity < 0 ||
            !RelativeStockDetail::checkedAdd(preflight, m_tariffs.certificateIdentity) ||
            !RelativeStockDetail::checkedAdd(preflight, m_tariffs.witness) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.quote) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.witness) ||
            !RelativeStockDetail::checkedAdd(total, preflight))
        {
            return MasterRefinementStatus::Invalid;
        }
        if (m_ledger.balance() < total)
        {
            m_refinementRefusal = MasterRefinementRefusal{
                MasterRefinementTerm::FutureDual, total, m_ledger.balance(), allowance, preparation.scanWork};
            return MasterRefinementStatus::BudgetExhausted;
        }
        if (!m_ledger.consume(preflight, RefinementWork::MasterValuation))
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        const auto tightened = m_session->tightenOurUpperWithVerifiedDual(*evaluation.quote(), allowance);
        if (tightened.globalFailure)
        {
            return MasterRefinementStatus::Invalid;
        }
        return refresh(evaluation) ? MasterRefinementStatus::Updated : MasterRefinementStatus::Invalid;
    }

    MasterRefinementStatus MasterAssignmentEvaluator::refine(
        MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term, std::int64_t allowance)
    {
        MasterProfileScope timing(m_profile, MasterPhase::ContinuationRefinement);
        m_refinementRefusal.reset();
        m_certificatePreparation.reset();
        const auto* quote = evaluation.quote();
        if (m_session == nullptr || quote == nullptr || allowance <= 0 ||
            m_ledger.balance() != m_session->remainingWork())
        {
            return MasterRefinementStatus::Unavailable;
        }
        if ((term == MasterRefinementTerm::Friendly && quote->ours.exact()) ||
            (term == MasterRefinementTerm::Enemy && quote->enemy.exact()) ||
            (term == MasterRefinementTerm::FutureDual &&
             (quote->ours.exact() || quote->ourUpperCertificateId != 0)))
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
            return refineCertificate(evaluation, allowance);
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
            m_refinementRefusal = MasterRefinementRefusal{
                term, total, m_ledger.balance(), allowance, tariff};
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
        RelativeStockComparison result;
        if (m_session != nullptr)
        {
            result = m_session->compare(*incumbent.quote(), *challenger.quote());
            if (!result.valid())
            {
                return result;
            }
        }
        return includeSeatedEconomics(result, incumbent.replay->seatedEconomicTotal,
                                     challenger.replay->seatedEconomicTotal);
    }

    RelativeStockComparison MasterAssignmentEvaluator::includeSeatedEconomics(
        RelativeStockComparison result, MilliFunds incumbent, MilliFunds challenger)
    {
        result.economicDelta = challenger;
        if (!RelativeStockDetail::checkedSubtract(result.economicDelta, incumbent) ||
            !RelativeStockDetail::checkedAdd(result.deltaLower, result.economicDelta) ||
            !RelativeStockDetail::checkedAdd(result.deltaUpper, result.economicDelta))
        {
            result.reason = RelativeStockReason::ArithmeticOverflow;
            return result;
        }
        result.reason = result.deltaLower > 0 ? RelativeStockReason::CertifiedPositive
            : result.deltaUpper <= 0 ? RelativeStockReason::CertifiedNonImprovement : RelativeStockReason::Overlap;
        return result;
    }

    bool MasterAssignmentEvaluator::unresolvedCommonEnemy(
        const MasterAssignmentEvaluation & incumbent, const MasterAssignmentEvaluation & challenger)
    {
        if (m_session == nullptr || incumbent.status != MasterEvaluationStatus::Feasible ||
            challenger.status != MasterEvaluationStatus::Feasible ||
            incumbent.objectiveStatus != MasterObjectiveStatus::Verified ||
            challenger.objectiveStatus != MasterObjectiveStatus::Verified ||
            !incumbent.replay || !challenger.replay || !incumbent.proof || !challenger.proof ||
            !incumbent.lowerReplayed || !challenger.lowerReplayed ||
            !incumbent.upperVerified || !challenger.upperVerified ||
            !m_problemIdentity.sameLiveProblem(incumbent.state.identity) ||
            !m_problemIdentity.sameLiveProblem(challenger.state.identity))
        {
            return false;
        }
        const auto & left = incumbent.proof->quote;
        const auto & right = challenger.proof->quote;
        constexpr std::int64_t scalarWork = 32;
        constexpr std::size_t canonicalPasses = 4;
        const auto entries = left.key.size() + right.key.size() +
                             left.capturedColumns.size() + right.capturedColumns.size();
        const auto scans = masterWorkProduct(entries, canonicalPasses);
        auto work = scalarWork;
        if (!scans || !RelativeStockDetail::checkedAdd(work, *scans) ||
            !m_ledger.consume(work, RefinementWork::MasterValuation) ||
            !m_session->owns(left) || !m_session->owns(right))
        {
            return false;
        }
        const auto relative = compareRelativeStock(left, right);
        if (!relative.valid())
        {
            return false;
        }
        const auto full = includeSeatedEconomics(relative, incumbent.replay->seatedEconomicTotal,
                                               challenger.replay->seatedEconomicTotal);
        return full.valid() && full.commonEnemyCancelled && full.unresolved();
    }
}
