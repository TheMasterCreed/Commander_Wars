#include "ai/coordinator/masterassignmentevaluator.h"

#include <algorithm>
#include <limits>

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t OPERATION_INSPECTION_SCALAR_WORK = 32;
        constexpr std::size_t OPERATION_INSPECTION_PASSES = 4;
        constexpr std::size_t OPERATION_KEY_SCALARS = 4;
    }

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
        MasterGuardedStockOwner & owner, MasterAssignmentProfile* profile)
        : MasterAssignmentEvaluator(problem, ledger, profile)
    {
        m_guardedOwner = &owner;
    }

    bool MasterAssignmentEvaluator::hasGuardedStock() const
    {
        return m_guardedOwner != nullptr;
    }

    bool MasterAssignmentEvaluator::guardedAdmits(std::size_t actor, std::int32_t candidate) const
    {
        return m_guardedOwner == nullptr || m_guardedOwner->admits(actor, candidate);
    }

    MasterGuardedComparison MasterAssignmentEvaluator::compareGuarded(
        const MasterAssignmentEvaluation & incumbent, const MasterAssignmentEvaluation & challenger)
    {
        if (incumbent.objectiveStatus == MasterObjectiveStatus::BudgetExhausted ||
            challenger.objectiveStatus == MasterObjectiveStatus::BudgetExhausted)
        {
            return {GuardedRelationStatus::BudgetExhausted, {}};
        }
        if (!m_guardedOwner || !ownsRetainedEvaluation(incumbent) || !ownsRetainedEvaluation(challenger))
        {
            return {GuardedRelationStatus::Inapplicable, {}};
        }
        return m_guardedOwner->compare(incumbent.guardedRoot, challenger.guardedRoot);
    }

    GuardedRelationStatus MasterAssignmentEvaluator::verifyGuarded(
        const MasterGuardedStockProof & proof, bool fresh)
    {
        return m_guardedOwner && matches(m_problem, m_ledger)
            ? m_guardedOwner->verify(proof, fresh) : GuardedRelationStatus::Invalid;
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::operationInspectionWork(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto* quote = evaluation.quote();
        const auto actors = m_problem.actors().size();
        if (quote == nullptr || quote->key.size() > actors || quote->capturedColumns.size() > actors)
        {
            return std::nullopt;
        }
        auto work = OPERATION_INSPECTION_SCALAR_WORK;
        for (const auto count : {actors, quote->capturedColumns.size(), std::size_t{1}})
        {
            const auto scans = masterWorkProduct(count, OPERATION_INSPECTION_PASSES);
            if (!scans || !RelativeStockDetail::checkedAdd(work, *scans))
            {
                return std::nullopt;
            }
        }
        const auto keys = masterWorkProduct(quote->key.size(), OPERATION_KEY_SCALARS * OPERATION_INSPECTION_PASSES);
        if (!keys || !RelativeStockDetail::checkedAdd(work, *keys))
        {
            return std::nullopt;
        }
        return work;
    }

    bool MasterAssignmentEvaluator::validOperationMetadata(
        const RelativeStockOperationMetadata & metadata, const RelativeStockQuote & quote) const
    {
        return metadata.valid && metadata.identity.sameLiveModel(quote.identity) &&
            metadata.ourRerunWork >= 0 && metadata.enemyRerunWork >= 0 &&
            metadata.certificateAdmissionWork >= 0 &&
            metadata.certificateReadinessWork >= 0 && metadata.certificateEntryWork >= 0;
    }

    void MasterAssignmentEvaluator::refuseOperation(
        MasterOperationOffer & offer, std::int64_t requested, std::int64_t preparation)
    {
        offer.status = MasterOperationStatus::FundingRefused;
        m_refinementRefusal = MasterRefinementRefusal{
            offer.term, requested, m_ledger.balance(), offer.allowance, preparation};
    }

    bool MasterAssignmentEvaluator::prepareOperation(
        MasterOperationOffer & offer, std::shared_ptr<const MasterOperationReadiness> & readiness)
    {
        const auto & quote = offer.proof->quote;
        const auto actors = m_problem.actors().size();
        if (readiness && readiness->identity.sameLiveProblem(m_problemIdentity) &&
            readiness->key.size() <= actors && readiness->capturedColumns.size() <= actors &&
            readiness->preparation.identity.sameLiveModel(quote.identity) &&
            readiness->preparation.rowCacheRevision == offer.metadata.rowCacheRevision &&
            readiness->key == quote.key && readiness->capturedColumns == quote.capturedColumns)
        {
            offer.preparation = readiness->preparation;
            offer.preparation.paidWork = 0;
        }
        else
        {
            auto minimumPreparation = offer.metadata.certificateAdmissionWork;
            if (!RelativeStockDetail::checkedAdd(minimumPreparation, offer.metadata.certificateReadinessWork))
            {
                return false;
            }
            if (m_ledger.balance() < minimumPreparation)
            {
                refuseOperation(offer, minimumPreparation, minimumPreparation);
                return false;
            }
            const auto before = m_ledger.balance();
            offer.preparation = m_session->prepareOurCertificateOperation(quote);
            offer.preparationWork = before - m_ledger.balance();
            if (offer.preparation.paidWork < 0 || offer.preparation.paidWork != offer.preparationWork ||
                m_ledger.balance() != m_session->remainingWork())
            {
                return false;
            }
            const bool checkedRefusal = offer.preparation.status == RelativeStockPreparationStatus::BudgetExhausted &&
                offer.preparation.readinessChecked && offer.preparationWork > 0;
            if (offer.preparation.status == RelativeStockPreparationStatus::Ready ||
                offer.preparation.status == RelativeStockPreparationStatus::MissingRows || checkedRefusal)
            {
                if (!offer.preparation.identity.sameLiveModel(quote.identity) ||
                    offer.preparation.rowCacheRevision != offer.metadata.rowCacheRevision ||
                    !offer.preparation.readinessChecked || offer.preparation.scanWork < 0 ||
                    offer.preparation.minimumNativeWork < 0 || offer.preparation.requiredPreparationWork < 0 ||
                    (checkedRefusal && offer.preparation.requiredPreparationWork <= m_ledger.balance()))
                {
                    return false;
                }
                readiness = std::make_shared<const MasterOperationReadiness>(
                    MasterOperationReadiness{m_problemIdentity, quote.key, quote.capturedColumns, offer.preparation});
            }
        }
        if (offer.preparation.status == RelativeStockPreparationStatus::BudgetExhausted)
        {
            if (offer.preparation.requiredPreparationWork <= 0)
            {
                return false;
            }
            refuseOperation(offer, offer.preparation.requiredPreparationWork, offer.preparationWork);
            return false;
        }
        if (offer.preparation.status == RelativeStockPreparationStatus::MissingRows)
        {
            if (offer.preparation.scanWork == 0 && offer.preparation.minimumNativeWork == 0)
            {
                offer.status = MasterOperationStatus::MissingPrerequisite;
            }
            return false;
        }
        return offer.preparation.status == RelativeStockPreparationStatus::Ready &&
            offer.preparation.readinessChecked &&
            offer.preparation.identity.sameLiveModel(quote.identity) &&
            offer.preparation.rowCacheRevision == offer.metadata.rowCacheRevision &&
            offer.preparation.scanWork >= 0 && offer.preparation.minimumNativeWork >= 0;
    }

    bool MasterAssignmentEvaluator::planOperationAllowance(MasterOperationOffer & offer)
    {
        for (;;)
        {
            if (!m_ledger.consume(MASTER_OPERATION_QUOTA_WORK, RefinementWork::MasterValuation))
            {
                refuseOperation(offer, MASTER_OPERATION_QUOTA_WORK, 0);
                return false;
            }
            offer.quotaWork += MASTER_OPERATION_QUOTA_WORK;
            if (offer.allowance > offer.preparation.minimumNativeWork)
            {
                return true;
            }
            if (offer.allowance > std::numeric_limits<std::int64_t>::max() / MASTER_OPERATION_GROWTH_FACTOR)
            {
                return false;
            }
            offer.allowance *= MASTER_OPERATION_GROWTH_FACTOR;
        }
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::operationRequiredWork(
        const MasterOperationOffer & offer) const
    {
        if (offer.allowance <= 0 || m_tariffs.quote < 0 || m_tariffs.witness < 0 ||
            (offer.term != MasterRefinementTerm::Friendly && offer.term != MasterRefinementTerm::Enemy &&
             offer.term != MasterRefinementTerm::FutureDual))
        {
            return std::nullopt;
        }
        auto work = offer.allowance;
        std::size_t quoteCount = 5;
        std::size_t witnessCount = 4;
        if (offer.term == MasterRefinementTerm::FutureDual)
        {
            quoteCount = 4;
            witnessCount = 5;
            if (offer.preparation.scanWork < 0 || m_tariffs.certificateIdentity < 0 ||
                offer.metadata.certificateEntryWork < 0 ||
                !RelativeStockDetail::checkedAdd(work, offer.preparation.scanWork) ||
                !RelativeStockDetail::checkedAdd(work, offer.metadata.certificateEntryWork) ||
                !RelativeStockDetail::checkedAdd(work, m_tariffs.certificateIdentity))
            {
                return std::nullopt;
            }
        }
        else
        {
            const auto tariff = offer.term == MasterRefinementTerm::Friendly
                ? offer.metadata.ourRerunWork : offer.metadata.enemyRerunWork;
            if (tariff < 0 || !RelativeStockDetail::checkedAdd(work, tariff))
            {
                return std::nullopt;
            }
        }
        for (std::size_t index = 0; index < quoteCount; ++index)
        {
            if (!RelativeStockDetail::checkedAdd(work, m_tariffs.quote))
            {
                return std::nullopt;
            }
        }
        for (std::size_t index = 0; index < witnessCount; ++index)
        {
            if (!RelativeStockDetail::checkedAdd(work, m_tariffs.witness))
            {
                return std::nullopt;
            }
        }
        const auto actors = m_problem.actors().size();
        if (actors == std::numeric_limits<std::size_t>::max())
        {
            return std::nullopt;
        }
        const auto retention = masterWorkProduct(actors + 1, actors + 1);
        if (!retention || !RelativeStockDetail::checkedAdd(work, *retention))
        {
            return std::nullopt;
        }
        return work;
    }

    MasterOperationOffer MasterAssignmentEvaluator::operationOffer(
        const MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term,
        std::int64_t allowance, std::shared_ptr<const MasterOperationReadiness> & readiness)
    {
        m_refinementRefusal.reset();
        m_certificatePreparation.reset();
        MasterOperationOffer offer;
        offer.term = term;
        offer.allowance = allowance;
        offer.identity = m_problemIdentity;
        if (m_session == nullptr)
        {
            offer.status = MasterOperationStatus::Unavailable;
            return offer;
        }
        const auto inspection = operationInspectionWork(evaluation);
        if (!inspection || allowance <= 0 ||
            (term != MasterRefinementTerm::Friendly && term != MasterRefinementTerm::Enemy &&
             term != MasterRefinementTerm::FutureDual))
        {
            return offer;
        }
        if (!m_ledger.consume(*inspection, RefinementWork::MasterValuation))
        {
            refuseOperation(offer, *inspection, 0);
            return offer;
        }
        offer.inspectionWork = *inspection;
        if (!m_operationBinding)
        {
            m_operationBinding = std::make_shared<const MasterOperationOffer::InspectionPermit::Binding>();
        }
        offer.m_inspection = std::make_shared<MasterOperationOffer::InspectionPermit>(
            MasterOperationOffer::InspectionPermit{m_operationBinding});
        if (!ownsRetainedEvaluation(evaluation) || m_ledger.balance() != m_session->remainingWork())
        {
            return offer;
        }
        offer.proof = evaluation.proof;
        const auto & quote = offer.proof->quote;
        if ((term == MasterRefinementTerm::Friendly && quote.ours.exact()) ||
            (term == MasterRefinementTerm::Enemy && quote.enemy.exact()) ||
            (term == MasterRefinementTerm::FutureDual &&
             (quote.ours.exact() || quote.ourUpperCertificateId != 0 || !m_session->supportsVerifiedDual())))
        {
            offer.status = MasterOperationStatus::Unavailable;
            return offer;
        }
        offer.metadata = m_session->operationMetadata(quote);
        if (!offer.metadata.valid)
        {
            offer.status = MasterOperationStatus::Unavailable;
            return offer;
        }
        if (!validOperationMetadata(offer.metadata, quote))
        {
            return offer;
        }
        if (term == MasterRefinementTerm::FutureDual)
        {
            if (!prepareOperation(offer, readiness))
            {
                return offer;
            }
            offer.metadata = m_session->operationMetadata(quote);
            if (!validOperationMetadata(offer.metadata, quote) ||
                offer.metadata.rowCacheRevision != offer.preparation.rowCacheRevision ||
                !planOperationAllowance(offer))
            {
                return offer;
            }
        }
        const auto required = operationRequiredWork(offer);
        if (!required)
        {
            return offer;
        }
        offer.remainingRequiredWork = *required;
        if (m_ledger.balance() < *required)
        {
            refuseOperation(offer, *required, term == MasterRefinementTerm::Friendly
                ? offer.metadata.ourRerunWork : term == MasterRefinementTerm::Enemy
                    ? offer.metadata.enemyRerunWork : offer.preparation.scanWork);
        }
        else
        {
            offer.status = MasterOperationStatus::Ready;
        }
        return offer;
    }

    MasterOperationOutcome MasterAssignmentEvaluator::executePreparedOperation(
        MasterAssignmentEvaluation & evaluation, const MasterOperationOffer & offer)
    {
        MasterOperationOutcome outcome;
        if (m_session == nullptr || !offer.proof ||
            !offer.identity.sameLiveProblem(m_problemIdentity) ||
            !m_session->owns(offer.proof->quote) ||
            !ownsRetainedEvaluation(evaluation) ||
            !validOperationMetadata(offer.metadata, offer.proof->quote) ||
            offer.proof->quote.key.size() > m_problem.actors().size() ||
            offer.proof->quote.capturedColumns.size() > m_problem.actors().size() ||
            offer.proof->quote.key != evaluation.quote()->key ||
            offer.proof->quote.capturedColumns != evaluation.quote()->capturedColumns ||
            m_ledger.balance() != m_session->remainingWork())
        {
            return outcome;
        }
        if (offer.status != MasterOperationStatus::Ready)
        {
            if (offer.status != MasterOperationStatus::Invalid)
            {
                outcome.status = offer.status == MasterOperationStatus::FundingRefused
                    ? MasterRefinementStatus::BudgetExhausted : MasterRefinementStatus::Unavailable;
            }
            return outcome;
        }
        const auto required = operationRequiredWork(offer);
        if (!required || *required != offer.remainingRequiredWork)
        {
            return outcome;
        }
        if (m_ledger.balance() < *required)
        {
            m_refinementRefusal = MasterRefinementRefusal{
                offer.term, *required, m_ledger.balance(), offer.allowance,
                offer.term == MasterRefinementTerm::Friendly ? offer.metadata.ourRerunWork
                    : offer.term == MasterRefinementTerm::Enemy ? offer.metadata.enemyRerunWork
                                                               : offer.preparation.scanWork};
            outcome.status = MasterRefinementStatus::BudgetExhausted;
            return outcome;
        }
        if (!refresh(evaluation))
        {
            outcome.status = evaluation.objectiveStatus == MasterObjectiveStatus::BudgetExhausted
                ? MasterRefinementStatus::BudgetExhausted : MasterRefinementStatus::Invalid;
            return outcome;
        }
        const auto before = evaluation.proof;
        const auto & quote = before->quote;
        auto current = offer;
        current.proof = before;
        current.metadata = m_session->operationMetadata(quote);
        if (!validOperationMetadata(current.metadata, quote))
        {
            return outcome;
        }
        if ((offer.term == MasterRefinementTerm::Friendly && quote.ours.exact()) ||
            (offer.term == MasterRefinementTerm::Enemy && quote.enemy.exact()) ||
            (offer.term == MasterRefinementTerm::FutureDual &&
             (quote.ours.exact() || quote.ourUpperCertificateId != 0)))
        {
            outcome.status = MasterRefinementStatus::Unavailable;
            return outcome;
        }
        if (offer.term == MasterRefinementTerm::FutureDual &&
            (offer.preparation.status != RelativeStockPreparationStatus::Ready ||
             !offer.preparation.identity.sameLiveModel(quote.identity) ||
             offer.preparation.rowCacheRevision != current.metadata.rowCacheRevision ||
             offer.allowance <= offer.preparation.minimumNativeWork))
        {
            outcome.status = MasterRefinementStatus::Unavailable;
            return outcome;
        }
        auto remaining = operationRequiredWork(current);
        if (!remaining || !RelativeStockDetail::checkedSubtract(*remaining, m_tariffs.quote) ||
            !RelativeStockDetail::checkedSubtract(*remaining, m_tariffs.witness))
        {
            return outcome;
        }
        if (m_ledger.balance() < *remaining)
        {
            m_refinementRefusal = MasterRefinementRefusal{
                offer.term, *remaining, m_ledger.balance(), offer.allowance,
                offer.term == MasterRefinementTerm::Friendly ? current.metadata.ourRerunWork
                    : offer.term == MasterRefinementTerm::Enemy ? current.metadata.enemyRerunWork
                                                               : offer.preparation.scanWork};
            outcome.status = MasterRefinementStatus::BudgetExhausted;
            return outcome;
        }
        return runOperation(evaluation, current);
    }

    MasterOperationOutcome MasterAssignmentEvaluator::runOperation(
        MasterAssignmentEvaluation & evaluation, const MasterOperationOffer & offer)
    {
        MasterOperationOutcome outcome;
        const auto & quote = offer.proof->quote;
        outcome.nativeBefore = offer.term == MasterRefinementTerm::Enemy ? quote.enemy : quote.ours;
        MasterProfileScope timing(m_profile, MasterPhase::ContinuationRefinement);
        if (offer.term == MasterRefinementTerm::FutureDual)
        {
            auto preflight = offer.preparation.scanWork;
            if (!RelativeStockDetail::checkedAdd(preflight, offer.metadata.certificateEntryWork) ||
                !RelativeStockDetail::checkedAdd(preflight, m_tariffs.certificateIdentity) ||
                !RelativeStockDetail::checkedAdd(preflight, m_tariffs.witness))
            {
                return outcome;
            }
            if (!m_ledger.consume(preflight, RefinementWork::MasterValuation))
            {
                outcome.status = MasterRefinementStatus::BudgetExhausted;
                return outcome;
            }
            const auto refined = m_session->tightenOurUpperWithVerifiedDual(quote, offer.allowance);
            outcome.nativeWork = refined.totalWork;
            outcome.admitted = refined.admitted;
            outcome.completed = refined.completed;
            if (refined.globalFailure)
            {
                return outcome;
            }
        }
        else
        {
            if (m_ledger.draw(m_tariffs.quote, RefinementWork::MasterValuation) == NO_GRANT)
            {
                outcome.status = MasterRefinementStatus::BudgetExhausted;
                return outcome;
            }
            const auto refined = offer.term == MasterRefinementTerm::Friendly
                ? m_session->refineOur(quote.key, offer.allowance)
                : m_session->refineEnemy(quote.capturedColumns, offer.allowance);
            outcome.nativeWork = refined.workUsed;
            outcome.nativeStates = refined.statesUsed;
            outcome.admitted = refined.admitted;
            outcome.completed = refined.completed;
            if (!refined.valid)
            {
                outcome.status = refined.budgetExhausted
                    ? MasterRefinementStatus::BudgetExhausted : MasterRefinementStatus::Invalid;
                return outcome;
            }
        }
        if (!refresh(evaluation))
        {
            outcome.status = evaluation.objectiveStatus == MasterObjectiveStatus::BudgetExhausted
                ? MasterRefinementStatus::BudgetExhausted : MasterRefinementStatus::Invalid;
            return outcome;
        }
        const auto & after = *evaluation.quote();
        const auto & initialInterval = offer.term == MasterRefinementTerm::Enemy ? quote.enemy : quote.ours;
        const auto & finalInterval = offer.term == MasterRefinementTerm::Enemy ? after.enemy : after.ours;
        outcome.nativeAfter = finalInterval;
        outcome.lowerProgress = finalInterval.lower > initialInterval.lower;
        outcome.upperProgress = finalInterval.upper < initialInterval.upper;
        outcome.certificateProgress = after.ourUpperCertificateId != 0 &&
            after.ourUpperCertificateId != quote.ourUpperCertificateId;
        outcome.status = MasterRefinementStatus::Updated;
        return outcome;
    }

    MasterOperationOutcome MasterAssignmentEvaluator::executeOperation(
        MasterAssignmentEvaluation & evaluation, const MasterOperationOffer & offer)
    {
        m_refinementRefusal.reset();
        m_certificatePreparation.reset();
        if (!offer.m_inspection || offer.m_inspection->consumed ||
            offer.m_inspection->binding.lock() != m_operationBinding || !m_operationBinding)
        {
            return {};
        }
        offer.m_inspection->consumed = true;
        const auto before = m_ledger.stats().spent();
        auto outcome = executePreparedOperation(evaluation, offer);
        outcome.executionWork = m_ledger.stats().spent() - before;
        return outcome;
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
               m_problemIdentity.sameLiveProblem(problem.identity()) && &m_ledger == &ledger &&
               (m_guardedOwner == nullptr || m_guardedOwner->matches(problem, ledger));
    }

    bool MasterAssignmentEvaluator::ownsRetainedEvaluation(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto actorCount = m_problem.actors().size();
        if (m_guardedOwner)
        {
            return matches(m_problem, m_ledger) &&
                m_problemIdentity.sameLiveProblem(evaluation.state.identity) &&
                evaluation.state.choices.size() == actorCount && evaluation.state.complete() &&
                evaluation.status == MasterEvaluationStatus::Feasible &&
                evaluation.objectiveStatus == MasterObjectiveStatus::GuardedRootPrepared &&
                evaluation.replay && evaluation.replay->complete() && evaluation.guardedRoot &&
                !evaluation.objective && !evaluation.proof && !evaluation.exact &&
                !evaluation.lowerReplayed && !evaluation.upperVerified &&
                evaluation.replay->actionIndices.size() == actorCount &&
                evaluation.replay->seatedValues.size() == actorCount &&
                evaluation.replay->continuationKey.size() <= actorCount &&
                evaluation.state.choices == evaluation.guardedRoot->state().choices &&
                evaluation.replay->seatedEconomicTotal == evaluation.guardedRoot->economic() &&
                m_guardedOwner->owns(*evaluation.guardedRoot);
        }
        if (!matches(m_problem, m_ledger) ||
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

    std::optional<std::int64_t> MasterAssignmentEvaluator::retainedCopyWork(
        const MasterAssignmentEvaluation & evaluation)
    {
        if (!ownsRetainedEvaluation(evaluation))
        {
            return std::nullopt;
        }
        const auto scan = evaluation.replay->plan.copyScanWork();
        if (!scan || !chargeMasterWork(m_ledger, static_cast<std::size_t>(*scan), 8,
                                      RefinementWork::MasterWitness))
        {
            return std::nullopt;
        }
        auto work = evaluation.replay->plan.copyWorkUnits();
        if (!work || !RelativeStockDetail::checkedAdd(*work, sizeof(MasterAssignmentEvaluation)))
        {
            return std::nullopt;
        }
        const auto & replay = *evaluation.replay;
        for (const auto count : {
                 evaluation.state.choices.size(), replay.actionIndices.size(), replay.seatedValues.size(),
                 replay.continuationKey.size() * 4, replay.ordering.order.size(),
                 replay.ordering.cycleMembers.size(), replay.ordering.blockedByCycle.size()})
        {
            const auto payload = masterWorkProduct(count, sizeof(std::int64_t));
            if (!payload || !RelativeStockDetail::checkedAdd(*work, *payload))
            {
                return std::nullopt;
            }
        }
        return work;
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
        if (m_guardedOwner)
        {
            const auto guarded = m_guardedOwner->finalizationWork();
            if (!guarded || !RelativeStockDetail::checkedAdd(*total, *guarded))
            {
                return std::nullopt;
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
        if (!matches(m_problem, m_ledger))
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
            !matches(m_problem, m_ledger) ||
            !evaluation.state.identity.sameLiveProblem(m_problemIdentity))
        {
            return false;
        }
        const auto economic = evaluation.replay->seatedEconomicTotal;
        if (m_guardedOwner)
        {
            evaluation.guardedRoot = m_guardedOwner->prepare(evaluation.state, *evaluation.replay);
            evaluation.objective.reset();
            evaluation.proof.reset();
            evaluation.lowerReplayed = false;
            evaluation.upperVerified = false;
            evaluation.exact = false;
            evaluation.objectiveStatus = evaluation.guardedRoot
                ? MasterObjectiveStatus::GuardedRootPrepared : MasterObjectiveStatus::Unavailable;
            if (m_guardedOwner->lastStatus() == GuardedRelationStatus::BudgetExhausted)
            {
                evaluation.objectiveStatus = MasterObjectiveStatus::BudgetExhausted;
            }
            return evaluation.guardedRoot != nullptr;
        }
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

    const RelativeStockQuote* MasterAssignmentEvaluator::beginRefinement(const MasterAssignmentEvaluation & evaluation)
    {
        m_refinementRefusal.reset();
        m_certificatePreparation.reset();
        const auto* quote = evaluation.quote();
        return m_session == nullptr || quote == nullptr || m_ledger.balance() != m_session->remainingWork()
            ? nullptr : quote;
    }

    std::optional<std::int64_t> MasterAssignmentEvaluator::rerunTotal(std::int64_t tariff, std::int64_t allowance) const
    {
        auto total = allowance;
        if (tariff < 0 || m_tariffs.quote < 0 || m_tariffs.witness < 0 ||
            !RelativeStockDetail::checkedAdd(total, tariff) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.quote) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.witness) ||
            !RelativeStockDetail::checkedAdd(total, m_tariffs.quote))
        {
            return std::nullopt;
        }
        return total;
    }

    MasterRefinementStatus MasterAssignmentEvaluator::refine(
        MasterAssignmentEvaluation & evaluation, MasterRefinementTerm term, std::int64_t allowance)
    {
        MasterProfileScope timing(m_profile, MasterPhase::ContinuationRefinement);
        const auto* quote = beginRefinement(evaluation);
        if (quote == nullptr || allowance <= 0)
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
        const auto total = rerunTotal(tariff, allowance);
        if (!total)
        {
            return MasterRefinementStatus::Invalid;
        }
        if (m_ledger.balance() < *total)
        {
            m_refinementRefusal = MasterRefinementRefusal{
                term, *total, m_ledger.balance(), allowance, tariff};
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

    MasterRefinementStatus MasterAssignmentEvaluator::adoptFriendlyWitness(
        MasterAssignmentEvaluation & evaluation, std::span<const SequentialRowWitness> witness, MilliFunds value)
    {
        MasterProfileScope timing(m_profile, MasterPhase::ContinuationRefinement);
        const auto* quote = beginRefinement(evaluation);
        if (quote == nullptr || !m_session->owns(*quote) || value <= quote->ours.lower)
        {
            return MasterRefinementStatus::Unavailable;
        }
        const auto total = rerunTotal(m_session->ourRerunWork(), 0);
        if (!total)
        {
            return MasterRefinementStatus::Invalid;
        }
        // An unaffordable suggestion is skipped without a recorded refusal, which the search treats as fatal.
        if (m_ledger.balance() < *total || m_ledger.draw(m_tariffs.quote, RefinementWork::MasterValuation) == NO_GRANT)
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        const auto lower = quote->ours.lower;
        const auto adopted = m_session->adoptOurWitness(quote->key, witness, value);
        if (adopted.budgetExhausted)
        {
            return MasterRefinementStatus::BudgetExhausted;
        }
        if (!adopted.valid || !refresh(evaluation))
        {
            return MasterRefinementStatus::Invalid;
        }
        return evaluation.quote()->ours.lower > lower ? MasterRefinementStatus::Updated
                                                      : MasterRefinementStatus::Unavailable;
    }

    bool MasterAssignmentEvaluator::hintFriendlyUpperPrices(
        const MasterAssignmentEvaluation & evaluation, std::span<const MilliFunds> futureNodePrices)
    {
        const auto* quote = evaluation.quote();
        return m_session != nullptr && quote != nullptr && m_session->owns(*quote) &&
            m_session->hintOurUpperPrices(quote->key, futureNodePrices);
    }

    RelativeStockComparison MasterAssignmentEvaluator::compare(
        MasterAssignmentEvaluation & incumbent, MasterAssignmentEvaluation & challenger)
    {
        if (m_guardedOwner)
        {
            return {};
        }
        if (!refresh(incumbent) || !refresh(challenger) ||
            !incumbent.objective || !challenger.objective)
        {
            return {};
        }
        if (m_session != nullptr)
        {
            const auto session = m_session->compare(*incumbent.quote(), *challenger.quote());
            if (!session.valid())
            {
                return session;
            }
            if (!checkOursCaps(incumbent, challenger))
            {
                return {.reason = RelativeStockReason::InvalidInterval};
            }
        }
        return compareQuotes(incumbent, challenger, comparisonQuote(incumbent), comparisonQuote(challenger));
    }

    RelativeStockComparison MasterAssignmentEvaluator::compareRecorded(
        MasterAssignmentEvaluation & incumbent, MasterAssignmentEvaluation & challenger,
        std::optional<MilliFunds> incumbentCap, std::optional<MilliFunds> challengerCap)
    {
        if (m_guardedOwner)
        {
            return {};
        }
        if (!refresh(incumbent) || !refresh(challenger) ||
            !incumbent.objective || !challenger.objective)
        {
            return {};
        }
        if (m_session != nullptr)
        {
            const auto session = m_session->compare(*incumbent.quote(), *challenger.quote());
            if (!session.valid())
            {
                return session;
            }
            // A contradiction disables every cap, so only a proof that recorded none survives it.
            checkOursCaps(incumbent, challenger);
        }
        return compareQuotes(incumbent, challenger, recordedQuote(incumbent, incumbentCap),
                             recordedQuote(challenger, challengerCap));
    }

    RelativeStockComparison MasterAssignmentEvaluator::compareQuotes(
        MasterAssignmentEvaluation & incumbent, MasterAssignmentEvaluation & challenger,
        const std::optional<RelativeStockQuote> & left, const std::optional<RelativeStockQuote> & right)
    {
        RelativeStockComparison result;
        if (m_session != nullptr)
        {
            if (!left || !right)
            {
                return {.reason = RelativeStockReason::InvalidInterval};
            }
            result = compareRelativeStock(*left, *right);
            if (!result.valid())
            {
                return result;
            }
        }
        return includeSeatedEconomics(result, incumbent.replay->seatedEconomicTotal,
                                     challenger.replay->seatedEconomicTotal);
    }

    bool MasterAssignmentEvaluator::checkOursCaps(
        const MasterAssignmentEvaluation & incumbent, const MasterAssignmentEvaluation & challenger)
    {
        if (verifiedCeiling(incumbent).status == CeilingStatus::Contradiction ||
            verifiedCeiling(challenger).status == CeilingStatus::Contradiction)
        {
            rejectOursCaps();
            return false;
        }
        return true;
    }

    const MasterAssignmentEvaluator::OursCap* MasterAssignmentEvaluator::applicableOursCap(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto* quote = evaluation.quote();
        if (m_oursCapContradictions != 0 || m_session == nullptr || m_guardedOwner != nullptr || quote == nullptr ||
            !evaluation.replay || evaluation.replay->continuationKey != quote->key || !m_session->owns(*quote) ||
            !m_problemIdentity.sameLiveProblem(evaluation.state.identity))
        {
            return nullptr;
        }
        const auto found = std::find_if(m_oursCaps.begin(), m_oursCaps.end(), [&](const OursCap & cap)
        {
            return cap.key == quote->key && cap.identity.sameLiveModel(quote->identity) &&
                   cap.certificate.state.identity.sameLiveProblem(evaluation.state.identity) &&
                   cap.certificate.state.choices == evaluation.state.choices;
        });
        return found == m_oursCaps.end() ? nullptr : &*found;
    }

    std::optional<MilliFunds> MasterAssignmentEvaluator::oursCeiling(
        MilliFunds upper, const MasterAssignmentEvaluation & evaluation)
    {
        // The comparison subtracts this evaluation's own seated and owned terms, so they cancel against the scan.
        auto ceiling = upper;
        if (!RelativeStockDetail::checkedSubtract(ceiling, evaluation.replay->seatedEconomicTotal) ||
            !RelativeStockDetail::checkedSubtract(ceiling, evaluation.quote()->owned))
        {
            return std::nullopt;
        }
        return ceiling;
    }

    MasterAssignmentEvaluator::Ceiling MasterAssignmentEvaluator::verifiedCeiling(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto* cap = applicableOursCap(evaluation);
        const auto ceiling = cap != nullptr ? oursCeiling(cap->upper, evaluation) : std::nullopt;
        if (!ceiling)
        {
            return {};
        }
        const auto & quote = *evaluation.quote();
        if (quote.owned != cap->owned || !m_session->futureCatalogMatchesSolver() || *ceiling < quote.ours.lower)
        {
            return {CeilingStatus::Contradiction, *ceiling};
        }
        return {CeilingStatus::Usable, *ceiling};
    }

    std::optional<RelativeStockQuote> MasterAssignmentEvaluator::comparisonQuote(
        const MasterAssignmentEvaluation & evaluation) const
    {
        const auto* quote = evaluation.quote();
        if (quote == nullptr)
        {
            return std::nullopt;
        }
        auto result = *quote;
        const auto ceiling = verifiedCeiling(evaluation);
        if (ceiling.status == CeilingStatus::Contradiction)
        {
            return std::nullopt;
        }
        if (ceiling.status == CeilingStatus::Usable)
        {
            result.ours.upper = std::min(result.ours.upper, ceiling.value);
        }
        return result;
    }

    std::optional<RelativeStockQuote> MasterAssignmentEvaluator::recordedQuote(
        const MasterAssignmentEvaluation & evaluation, std::optional<MilliFunds> cap) const
    {
        const auto* quote = evaluation.quote();
        if (quote == nullptr)
        {
            return std::nullopt;
        }
        auto result = *quote;
        if (!cap)
        {
            return result;
        }
        const auto ceiling = verifiedCeiling(evaluation);
        if (ceiling.status != CeilingStatus::Usable || ceiling.value != *cap)
        {
            return std::nullopt;
        }
        result.ours.upper = std::min(result.ours.upper, *cap);
        return result;
    }

    std::optional<MilliFunds> MasterAssignmentEvaluator::oursCap(const MasterAssignmentEvaluation & evaluation) const
    {
        const auto ceiling = verifiedCeiling(evaluation);
        return ceiling.status == CeilingStatus::Usable && ceiling.value < evaluation.quote()->ours.upper
            ? std::optional<MilliFunds>{ceiling.value} : std::nullopt;
    }

    RelativeStockComparison MasterAssignmentEvaluator::recordedComparison(
        const MasterAssignmentEvaluation & incumbent, const MasterAssignmentEvaluation & challenger,
        std::optional<MilliFunds> incumbentCap, std::optional<MilliFunds> challengerCap) const
    {
        auto left = recordedQuote(incumbent, incumbentCap);
        auto right = recordedQuote(challenger, challengerCap);
        if (!left || !right || !incumbent.replay || !challenger.replay)
        {
            return {};
        }
        // Historical stock snapshots use the shared comparator with their recorded seated economics.
        left->economic = incumbent.replay->seatedEconomicTotal;
        right->economic = challenger.replay->seatedEconomicTotal;
        return compareRelativeStock(*left, *right);
    }

    std::uint64_t MasterAssignmentEvaluator::oursCapContradictions() const
    {
        return m_oursCapContradictions;
    }

    void MasterAssignmentEvaluator::rejectOursCaps()
    {
        m_oursCaps.clear();
        ++m_oursCapContradictions;
    }

    std::optional<MilliFunds> MasterAssignmentEvaluator::verifiedUpper(
        const MasterBoundCertificate & certificate, const MasterAssignmentBounds & bounds)
    {
        const auto currentResources = m_problem.resources().size();
        if (certificate.family != MasterBoundFamily::CombinedCurrentFuture ||
            !bounds.covers(m_problemIdentity, stockModel(), futureCatalog()) ||
            certificate.prices.size() < currentResources ||
            std::any_of(certificate.prices.begin(),
                        certificate.prices.begin() + static_cast<std::ptrdiff_t>(currentResources),
                        [](MilliFunds price) { return price != 0; }))
        {
            return std::nullopt;
        }
        const auto verified = bounds.verify(certificate, m_ledger);
        return verified.valid() ? std::optional<MilliFunds>{verified.certificate->upper} : std::nullopt;
    }

    MasterOursCapReceipt MasterAssignmentEvaluator::capOursUpper(
        const MasterAssignmentEvaluation & evaluation, const MasterBoundCertificate & certificate,
        const MasterAssignmentBounds & bounds)
    {
        MasterOursCapReceipt receipt;
        if (m_oursCapContradictions != 0 || !matches(m_problem, m_ledger) ||
            !certificate.state.identity.sameLiveProblem(evaluation.state.identity) ||
            certificate.state.choices != evaluation.state.choices)
        {
            return receipt;
        }
        switch (oursBasis(evaluation, m_ledger))
        {
            case MasterOursBasisStatus::Proven:
                break;
            case MasterOursBasisStatus::CatalogMismatch:
                receipt.status = MasterOursCapStatus::CatalogMismatch;
                return receipt;
            case MasterOursBasisStatus::OwnedMismatch:
                receipt.status = MasterOursCapStatus::OwnedMismatch;
                return receipt;
            case MasterOursBasisStatus::Unproven:
            case MasterOursBasisStatus::BudgetExhausted:
                return receipt;
        }
        const auto* quote = evaluation.quote();
        const auto upper = verifiedUpper(certificate, bounds);
        const auto* applicable = applicableOursCap(evaluation);
        if (!upper)
        {
            return receipt;
        }
        const auto inForce = applicable != nullptr ? std::min(applicable->upper, *upper) : *upper;
        const auto ceiling = oursCeiling(inForce, evaluation);
        if (!ceiling)
        {
            return receipt;
        }
        receipt.verifiedUpper = inForce;
        receipt.ceiling = ceiling;
        if (*ceiling < quote->ours.lower || (applicable != nullptr && applicable->owned != quote->owned))
        {
            rejectOursCaps();
            receipt.status = MasterOursCapStatus::Contradiction;
            return receipt;
        }
        OursCap cap{quote->key, quote->identity, *upper, quote->owned, certificate};
        if (applicable == nullptr)
        {
            m_oursCaps.push_back(std::move(cap));
        }
        else if (*upper < applicable->upper)
        {
            m_oursCaps[static_cast<std::size_t>(applicable - m_oursCaps.data())] = std::move(cap);
        }
        receipt.status = MasterOursCapStatus::Registered;
        return receipt;
    }

    MasterOursBasisStatus MasterAssignmentEvaluator::oursBasis(
        const MasterAssignmentEvaluation & evaluation, RefinementLedger & ledger) const
    {
        const auto* quote = evaluation.quote();
        if (m_session == nullptr || m_guardedOwner != nullptr || evaluation.status != MasterEvaluationStatus::Feasible ||
            evaluation.objectiveStatus != MasterObjectiveStatus::Verified || !evaluation.lowerReplayed ||
            !evaluation.replay || !evaluation.replay->complete() || quote == nullptr || !quote->valid ||
            !m_session->owns(*quote) || quote->key != evaluation.replay->continuationKey ||
            !m_problemIdentity.sameLiveProblem(evaluation.state.identity) ||
            !validMasterState(m_problem, evaluation.state) || !evaluation.state.complete())
        {
            return MasterOursBasisStatus::Unproven;
        }
        if (!m_session->futureCatalogMatchesSolver())
        {
            return MasterOursBasisStatus::CatalogMismatch;
        }
        const auto* model = stockModel();
        if (model == nullptr || m_tariffs.quote < 0)
        {
            return MasterOursBasisStatus::Unproven;
        }
        if (!ledger.consume(m_tariffs.quote, RefinementWork::MasterValuation))
        {
            return MasterOursBasisStatus::BudgetExhausted;
        }
        // The scan bounds the stock model's owned term, so the subtracted owned term must be that same value.
        auto modelled = model->initialState();
        if (!model->apply(quote->key, modelled) || modelled.owned != quote->owned)
        {
            return MasterOursBasisStatus::OwnedMismatch;
        }
        return MasterOursBasisStatus::Proven;
    }

    std::int64_t MasterAssignmentEvaluator::oursCapCheckWork() const
    {
        return m_tariffs.quote;
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
            !m_session->owns(left) || !m_session->owns(right) || !checkOursCaps(incumbent, challenger))
        {
            return false;
        }
        const auto cappedLeft = comparisonQuote(incumbent);
        const auto cappedRight = comparisonQuote(challenger);
        if (!cappedLeft || !cappedRight)
        {
            return false;
        }
        const auto relative = compareRelativeStock(*cappedLeft, *cappedRight);
        if (!relative.valid())
        {
            return false;
        }
        const auto full = includeSeatedEconomics(relative, incumbent.replay->seatedEconomicTotal,
                                               challenger.replay->seatedEconomicTotal);
        return full.valid() && full.commonEnemyCancelled && full.unresolved();
    }
}
