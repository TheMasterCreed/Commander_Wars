#include "ai/coordinator/propertystockbuilder.h"

#include "ai/coordinator/masterassignmentprofile.h"

namespace Coordinator
{
    JointPlanStockValuer::RelativeSession::RelativeSession(
        JointPlanStockValuer & owner, const PropertyStockField & snapshot,
        const PropertyStockInstance & enemyModel)
        : RelativeStockSession(snapshot, enemyModel), m_owner(owner)
    {
    }

    PropertyStockField & JointPlanStockValuer::continuationField() const
    {
        return m_pricingField ? *m_pricingField : m_field;
    }

    RelativeStockSession* JointPlanStockValuer::relativeStockSession()
    {
        if (!livePairSwapIntervals() || !m_pricingField)
        {
            return nullptr;
        }
        if (!m_relativeSession)
        {
            m_relativeSession.emplace(*this, *m_pricingField, m_pricingField->m_theirs);
        }
        return &*m_relativeSession;
    }

    RelativeStockQuote JointPlanStockValuer::RelativeSession::quote(
        const TurnPlan & plan, MilliFunds economic)
    {
        return quote(canonicalPlanKey(m_owner.planActions(plan)), economic);
    }

    const CheapPricingModel* JointPlanStockValuer::RelativeSession::pricingModel() const
    {
        return m_owner.livePairSwapIntervals() ? &m_owner.m_cheapModel : nullptr;
    }

    const MasterFutureCatalog* JointPlanStockValuer::RelativeSession::futureCatalog() const
    {
        return m_owner.m_masterFutureCatalog.get();
    }

    RelativeStockQuote JointPlanStockValuer::RelativeSession::quote(
        const ContinuationKey & key, MilliFunds economic)
    {
        RelativeStockQuote result;
        CheapInitialPrice initial;
        if (!m_owner.livePairSwapIntervals() ||
            !m_owner.m_cheapModel.price(key, m_owner.m_cheapState, initial))
        {
            return result;
        }
        result.key = initial.key;
        result.capturedColumns = initial.capturedColumns;
        result.identity = identity();
        result.economic = economic;
        result.owned = initial.owned;
        result.ours = initial.ours;
        const auto* entry = m_owner.m_continuationStore.find(key);
        if (entry != nullptr)
        {
            if (entry->capturedColumns != initial.capturedColumns ||
                entry->initialOurs.lower != initial.ours.lower || entry->initialOurs.upper != initial.ours.upper)
            {
                m_owner.m_pricingFailure = true;
                return result;
            }
            result.ours = entry->ours;
            result.ourSearchCompleted = entry->searchCompleted;
            const auto certificate =
                m_owner.m_verifiedOurUppers.find(key);
            if (certificate !=
                m_owner.m_verifiedOurUppers.end())
            {
                result.ourUpperCertificateId =
                    certificate->second.id;
            }
        }
        if (result.ours.lower == initial.ours.lower)
        {
            result.ourLowerWitnessReplays = m_owner.m_cheapModel.replayWitness(
                m_owner.m_cheapState, initial.witness, initial.ours.lower);
        }
        else
        {
            const auto witness = m_owner.m_refinedOurWitnesses.find(key);
            result.ourLowerWitnessReplays = witness != m_owner.m_refinedOurWitnesses.end() &&
                witness->second.replays && witness->second.value == result.ours.lower;
        }
        auto* enemy = m_owner.m_continuationStore.enemyFor(initial.capturedColumns);
        if (enemy == nullptr)
        {
            EnemySetBounds bounds;
            bounds.bounds = initial.enemy;
            bounds.exact = initial.enemy.exact();
            enemy = &m_owner.m_continuationStore.admitEnemy(initial.capturedColumns, bounds);
        }
        if (enemy->initialBounds.lower != initial.enemy.lower || enemy->initialBounds.upper != initial.enemy.upper)
        {
            m_owner.m_pricingFailure = true;
            return result;
        }
        result.enemy = enemy->bounds;
        result.enemySearchCompleted = enemy->searchCompleted;
        const auto enemyWitness = m_owner.m_refinedEnemyWitnesses.find(initial.capturedColumns);
        result.enemyLowerWitnessReplays = result.enemy.lower == 0 ||
            (enemyWitness != m_owner.m_refinedEnemyWitnesses.end() && enemyWitness->second.replays &&
             enemyWitness->second.value == result.enemy.lower);
        result.valid = result.ourLowerWitnessReplays && RelativeStockDetail::canonical(result) &&
            RelativeStockDetail::coherent(result);
        return result;
    }

    std::int64_t JointPlanStockValuer::RelativeSession::remainingWork() const
    {
        return m_owner.m_pRefinementLedger != nullptr ? m_owner.m_pRefinementLedger->balance() : 0;
    }

    std::int64_t JointPlanStockValuer::RelativeSession::ourRerunWork() const
    {
        auto tariff = m_owner.ourRerunTariff();
        if (m_owner.m_masterFutureCatalog)
        {
            const auto rows = m_owner.m_field.m_ours.rows.size();
            const auto extra = warmStartTariff(m_owner.m_refinedOurWitnesses.size(), rows, rows,
                                              m_owner.m_field.m_ours.columns.size());
            if (tariff < 0 || extra < 0 || !RelativeStockDetail::checkedAdd(tariff, extra))
            {
                return -1;
            }
        }
        return tariff;
    }

    std::int64_t JointPlanStockValuer::RelativeSession::enemyRerunWork(
        const std::vector<std::int32_t> & capturedColumns) const
    {
        auto tariff = m_owner.enemyRerunTariff(capturedColumns);
        if (m_owner.m_masterFutureCatalog)
        {
            const auto extra = warmStartTariff(m_owner.m_refinedEnemyWitnesses.size(), capturedColumns.size(),
                m_owner.m_field.m_theirs.rows.size(), m_owner.m_field.m_theirs.columns.size());
            if (tariff < 0 || extra < 0 || !RelativeStockDetail::checkedAdd(tariff, extra))
            {
                return -1;
            }
        }
        return tariff;
    }

    GrantId JointPlanStockValuer::RelativeSession::grant(
        std::int64_t tariff, std::int64_t stateCap, RefinementWork work)
    {
        std::int64_t total = tariff;
        if (!m_owner.livePairSwapIntervals() || tariff < 0 || stateCap <= 0 ||
            !RelativeStockDetail::checkedAdd(total, stateCap))
        {
            return NO_GRANT;
        }
        return m_owner.m_pRefinementLedger->draw(total, work);
    }

    RelativeStockRefinedTerm JointPlanStockValuer::RelativeSession::refineOur(
        const ContinuationKey & key, std::int64_t stateCap)
    {
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        m_owner.m_diagnosticLastWarmStart.reset();
#endif
        RelativeStockRefinedTerm result;
        CheapInitialPrice initial;
        if (!m_owner.livePairSwapIntervals() || !m_owner.priceCheap(key, initial))
        {
            return result;
        }
        auto* entry = m_owner.m_continuationStore.find(key);
        result.interval = entry->ours;
        const auto tariff = ourRerunWork();
        const auto before = remainingWork();
        const auto granted = grant(tariff, stateCap,
            entry->storeBacked ? RefinementWork::SearchSlice : RefinementWork::OurAdmission);
        if (granted == NO_GRANT)
        {
            result.budgetExhausted = true;
            return result;
        }
        result.valid = m_owner.refineOur(*entry, granted, stateCap, RefinementSchedule::Relative);
        result.workUsed = before - m_owner.m_pRefinementLedger->balance();
        result.statesUsed = result.workUsed - tariff;
        if (m_owner.m_masterFutureCatalog && result.statesUsed < 0)
        {
            result.valid = false;
            return result;
        }
        result.interval = entry->ours;
        result.searchCompleted = entry->searchCompleted;
        const auto refreshed = quote(key, 0);
        result.lowerWitnessReplays = refreshed.valid && refreshed.ourLowerWitnessReplays;
        result.valid = result.valid && result.lowerWitnessReplays;
        return result;
    }

    RelativeStockRefinedTerm JointPlanStockValuer::RelativeSession::refineEnemy(
        const std::vector<std::int32_t> & capturedColumns, std::int64_t stateCap)
    {
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        m_owner.m_diagnosticLastWarmStart.reset();
#endif
        RelativeStockRefinedTerm result;
        auto* enemy = m_owner.m_continuationStore.enemyFor(capturedColumns);
        if (!m_owner.livePairSwapIntervals() || enemy == nullptr)
        {
            return result;
        }
        result.interval = enemy->bounds;
        const auto tariff = enemyRerunWork(capturedColumns);
        const auto before = remainingWork();
        const auto granted = grant(tariff, stateCap,
            enemy->storeBacked ? RefinementWork::SearchSlice : RefinementWork::EnemyEntry);
        if (granted == NO_GRANT)
        {
            result.budgetExhausted = true;
            return result;
        }
        result.valid = m_owner.refineEnemy(*enemy, capturedColumns, granted, stateCap, RefinementSchedule::Relative);
        result.workUsed = before - m_owner.m_pRefinementLedger->balance();
        result.statesUsed = result.workUsed - tariff;
        result.interval = enemy->bounds;
        result.searchCompleted = enemy->searchCompleted;
        const auto witness = m_owner.m_refinedEnemyWitnesses.find(capturedColumns);
        result.lowerWitnessReplays = enemy->bounds.lower == 0 ||
            (witness != m_owner.m_refinedEnemyWitnesses.end() && witness->second.replays &&
             witness->second.value == enemy->bounds.lower);
        result.valid = result.valid && result.lowerWitnessReplays;
        return result;
    }

    RelativeStockDualTightening
    JointPlanStockValuer::RelativeSession::
        tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote,
            std::int64_t maximumWork)
    {
        if (!owns(quote))
        {
            RelativeStockDualTightening result;
            result.intervalBefore = quote.ours;
            result.intervalAfter = quote.ours;
            result.globalFailure = true;
            return result;
        }
        return m_owner.tightenOurUpperWithVerifiedDual(
            quote,
            sessionIdentity(),
            maximumWork);
    }

    RelativeStockCertificatePreparation JointPlanStockValuer::RelativeSession::prepareOurCertificate(
        const RelativeStockQuote & quote)
    {
        if (!owns(quote))
        {
            return {};
        }
        return m_owner.prepareOurCertificate(quote);
    }

    bool JointPlanStockValuer::RelativeSession::
        verifiedUpperMatches(
            const RelativeStockQuote & quote) const
    {
        return m_owner.verifiedUpperMatches(
            quote,
            sessionIdentity());
    }

    bool JointPlanStockValuer::RelativeSession::replayOurWitness(const RelativeStockQuote & expected)
    {
        if (!owns(expected) || !sameRelativeStockQuote(expected, quote(expected.key, expected.economic)))
        {
            return false;
        }
        CheapInitialPrice initial;
        if (!m_owner.m_cheapModel.price(expected.key, m_owner.m_cheapState, initial))
        {
            return false;
        }
        if (expected.ours.lower == initial.ours.lower)
        {
            return m_owner.m_cheapModel.replayWitness(m_owner.m_cheapState, initial.witness, expected.ours.lower);
        }
        const auto witness = m_owner.m_refinedOurWitnesses.find(expected.key);
        return witness != m_owner.m_refinedOurWitnesses.end() && witness->second.value == expected.ours.lower &&
            m_owner.solveOurSequential(planActions(expected.key), 0, false, &witness->second).witnessReplays;
    }

    bool JointPlanStockValuer::RelativeSession::replayEnemyWitness(const RelativeStockQuote & expected)
    {
        if (!owns(expected) || !sameRelativeStockQuote(expected, quote(expected.key, expected.economic)) ||
            !expected.enemyLowerWitnessReplays)
        {
            return false;
        }
        if (expected.enemy.lower == 0)
        {
            return true;
        }
        const auto witness = m_owner.m_refinedEnemyWitnesses.find(expected.capturedColumns);
        return witness != m_owner.m_refinedEnemyWitnesses.end() && witness->second.value == expected.enemy.lower &&
            m_owner.solveEnemySequential(expected.capturedColumns, 0, false, &witness->second).witnessReplays;
    }

    std::shared_ptr<const RelativeStockProofSnapshot> JointPlanStockValuer::RelativeSession::captureProof(
        const RelativeStockQuote & expected)
    {
        MasterProfileScope timing(m_owner.m_masterProfile, MasterPhase::ProofCapture);
        if (!owns(expected) || !expected.ourLowerWitnessReplays || !expected.enemyLowerWitnessReplays ||
            !sameRelativeStockQuote(expected, quote(expected.key, expected.economic)))
        {
            return {};
        }
        auto state = m_owner.m_cheapModel.initialState();
        CheapInitialPrice initial;
        if (!m_owner.m_cheapModel.price(expected.key, state, initial))
        {
            return {};
        }
        auto proof = std::make_shared<RelativeStockProofSnapshot>();
        proof->quote = expected;
        if (expected.ours.lower == initial.ours.lower)
        {
            proof->cheapOurWitness = std::move(initial.witness);
        }
        else
        {
            const auto witness = m_owner.m_refinedOurWitnesses.find(expected.key);
            if (witness == m_owner.m_refinedOurWitnesses.end() || witness->second.value != expected.ours.lower)
            {
                return {};
            }
            proof->ourKind = RelativeStockWitnessKind::Sequential;
            MasterProfileScope copyTiming(m_owner.m_masterProfile, MasterPhase::ProofCopy);
            proof->ourWitness = witness->second.rows;
        }
        if (expected.enemy.lower != 0)
        {
            const auto witness = m_owner.m_refinedEnemyWitnesses.find(expected.capturedColumns);
            if (witness == m_owner.m_refinedEnemyWitnesses.end() || witness->second.value != expected.enemy.lower)
            {
                return {};
            }
            MasterProfileScope copyTiming(m_owner.m_masterProfile, MasterPhase::ProofCopy);
            proof->enemyWitness = witness->second.rows;
        }
        if (expected.ourUpperCertificateId != 0)
        {
            const auto certificate = m_owner.m_verifiedOurUppers.find(expected.key);
            if (certificate == m_owner.m_verifiedOurUppers.end() || certificate->second.id != expected.ourUpperCertificateId)
            {
                return {};
            }
            proof->ourUpperCertificate = certificate->second.certificate;
        }
        return proof;
    }

    bool JointPlanStockValuer::RelativeSession::replayLowerProof(const RelativeStockProofSnapshot & proof) const
    {
        MasterProfileScope timing(m_owner.m_masterProfile, MasterPhase::ProofReplay);
        const auto & expected = proof.quote;
        if (!owns(expected) || !expected.valid || !RelativeStockDetail::canonical(expected) ||
            !RelativeStockDetail::coherent(expected))
        {
            return false;
        }
        auto state = m_owner.m_cheapModel.initialState();
        CheapInitialPrice initial;
        if (!m_owner.m_cheapModel.price(expected.key, state, initial) ||
            initial.capturedColumns != expected.capturedColumns || initial.owned != expected.owned)
        {
            return false;
        }
        if (proof.ourKind == RelativeStockWitnessKind::Cheap)
        {
            if (!proof.ourWitness.empty() ||
                !m_owner.m_cheapModel.replayWitness(state, proof.cheapOurWitness, expected.ours.lower))
            {
                return false;
            }
        }
        else if (proof.ourKind == RelativeStockWitnessKind::Sequential)
        {
            MasterProfileScope copyTiming(m_owner.m_masterProfile, MasterPhase::ProofCopy);
            const RefinedWitness witness{proof.ourWitness, expected.ours.lower, true};
            copyTiming.finish();
            if (!proof.cheapOurWitness.empty() ||
                !m_owner.solveOurSequential(planActions(expected.key), 0, false, &witness).witnessReplays)
            {
                return false;
            }
        }
        else
        {
            return false;
        }
        if (expected.enemy.lower == 0 && proof.enemyWitness.empty())
        {
            return true;
        }
        MasterProfileScope copyTiming(m_owner.m_masterProfile, MasterPhase::ProofCopy);
        const RefinedWitness enemy{proof.enemyWitness, expected.enemy.lower, true};
        copyTiming.finish();
        return m_owner.solveEnemySequential(expected.capturedColumns, 0, false, &enemy).witnessReplays;
    }
}
