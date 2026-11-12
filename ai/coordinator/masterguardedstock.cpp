#include "ai/coordinator/masterguardedstock.h"

#include <algorithm>
#include <utility>

#include "ai/coordinator/relativestockcomparison.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t PROOF_CONTROL_WORK = 256;
        constexpr std::int64_t ROOT_COPY_FIELD_WORK = 24;

        std::optional<std::int64_t> rootRetentionWork(std::size_t actors, std::size_t properties)
        {
            auto work = masterWorkProduct(actors + properties + 1, ROOT_COPY_FIELD_WORK);
            if (!work || !RelativeStockDetail::checkedAdd(*work, PROOF_CONTROL_WORK))
            {
                return std::nullopt;
            }
            return work;
        }
    }

    MasterGuardedPlanRoot::MasterGuardedPlanRoot(
        MasterAssignmentState state, MasterGuardedRootPreparation prepared, MilliFunds economic,
        const std::shared_ptr<const Context> & context)
        : m_state(std::move(state)),
          m_root(std::move(*prepared.root)),
          m_authority(std::move(prepared.authority)),
          m_consequences(std::move(prepared.consequences)),
          m_economic(economic),
          m_currentFamily(std::move(prepared.currentFamily)),
          m_context(context)
    {
    }

    const GuardedStockRoot & MasterGuardedPlanRoot::root() const
    {
        return m_root;
    }

    const MasterAssignmentState & MasterGuardedPlanRoot::state() const
    {
        return m_state;
    }

    const QByteArray & MasterGuardedPlanRoot::authority() const
    {
        return m_authority;
    }

    const QByteArray & MasterGuardedPlanRoot::consequences() const
    {
        return m_consequences;
    }

    MilliFunds MasterGuardedPlanRoot::economic() const
    {
        return m_economic;
    }

    const std::optional<GuardedStockCurrentFamily> & MasterGuardedPlanRoot::currentFamily() const
    {
        return m_currentFamily;
    }

    const MasterGuardedPlanRoot & MasterGuardedStockProof::incoming() const
    {
        return *m_incoming;
    }

    const MasterGuardedPlanRoot & MasterGuardedStockProof::challenger() const
    {
        return *m_challenger;
    }

    const GuardedStockRelation & MasterGuardedStockProof::relation() const
    {
        return m_relation;
    }

    bool MasterGuardedComparison::improves() const
    {
        return status == GuardedRelationStatus::Verified && proof != nullptr &&
            proof->relation().minimumPaymentShift > 0;
    }

    MasterGuardedStockOwner::MasterGuardedStockOwner(
        const MasterAssignmentProblem & problem, RefinementLedger & ledger,
        std::unique_ptr<MasterGuardedRootSource> source)
        : m_problem(problem),
          m_identity(problem.identity()),
          m_ledger(ledger),
          m_source(std::move(source))
    {
        if (m_ledger.consume(PROOF_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            m_context = std::make_shared<const MasterGuardedPlanRoot::Context>();
        }
    }

    std::shared_ptr<const MasterGuardedPlanRoot> MasterGuardedStockOwner::prepare(
        const MasterAssignmentState & state, const MasterAssignmentReplayResult & replay)
    {
        m_lastStatus = GuardedRelationStatus::Invalid;
        if (!m_source || !m_context || !m_identity.sameLiveProblem(m_problem.identity()) ||
            !state.identity.sameLiveProblem(m_identity) || state.choices.size() != m_problem.actors().size() ||
            !state.complete() || !replay.complete())
        {
            return {};
        }
        // compare() admits only equal seated economics, so other plans never need a root.
        if (replay.seatedEconomicTotal != m_problem.incumbentEconomicTotal())
        {
            m_lastStatus = GuardedRelationStatus::Inapplicable;
            return {};
        }
        const auto retention = rootRetentionWork(state.choices.size(), GUARDED_STOCK_MAX_TILES);
        if (!retention || !m_ledger.consume(*retention, RefinementWork::MasterWitness))
        {
            m_lastStatus = GuardedRelationStatus::BudgetExhausted;
            return {};
        }
        auto prepared = m_source->prepare(state, replay, m_ledger);
        m_lastStatus = prepared.status;
        if (prepared.status != GuardedRelationStatus::Verified || !prepared.root ||
            prepared.authority.size() != GUARDED_STOCK_DIGEST_BYTES ||
            prepared.consequences.size() != GUARDED_STOCK_DIGEST_BYTES ||
            prepared.root->actors.size() > GUARDED_STOCK_MAX_ACTORS ||
            prepared.root->properties.size() > GUARDED_STOCK_MAX_TILES ||
            (prepared.currentFamily && (m_source->maximumCurrentPrefixActions() > GUARDED_STOCK_MAX_ACTORS ||
                prepared.currentFamily->commonPrefix.size() > m_source->maximumCurrentPrefixActions())))
        {
            return {};
        }
        return std::shared_ptr<const MasterGuardedPlanRoot>(new MasterGuardedPlanRoot(
            state, std::move(prepared), replay.seatedEconomicTotal, m_context));
    }

    bool MasterGuardedStockOwner::matches(
        const MasterAssignmentProblem & problem, const RefinementLedger & ledger) const
    {
        return m_context && m_source && &m_ledger == &ledger &&
            m_identity.sameLiveProblem(m_problem.identity()) &&
            m_identity.sameLiveProblem(problem.identity());
    }

    bool MasterGuardedStockOwner::owns(const MasterGuardedPlanRoot & root) const
    {
        return matches(m_problem, m_ledger) &&
            root.state().identity.sameLiveProblem(m_identity) && root.m_context.lock() == m_context &&
            root.root().revision == GuardedStockRevision::CoupledNativeTerminalV4;
    }

    MasterGuardedComparison MasterGuardedStockOwner::compare(
        const std::shared_ptr<const MasterGuardedPlanRoot> & incoming,
        const std::shared_ptr<const MasterGuardedPlanRoot> & challenger)
    {
        if (!m_ledger.consume(PROOF_CONTROL_WORK, RefinementWork::MasterWitness))
        {
            return {GuardedRelationStatus::BudgetExhausted, {}};
        }
        if (!incoming || !challenger || !owns(*incoming) || !owns(*challenger) ||
            incoming->authority() != challenger->authority() || incoming->economic() != challenger->economic() ||
            !std::equal(incoming->state().choices.begin(), incoming->state().choices.end(),
                        m_problem.incumbentChoices().begin(), m_problem.incumbentChoices().end()))
        {
            return {GuardedRelationStatus::Inapplicable, {}};
        }
        const auto result = constructGuardedStockRelation(incoming->root(), challenger->root(), m_ledger);
        if (result.status != GuardedRelationStatus::Verified || !result.relation)
        {
            return {result.status, {}};
        }
        auto proof = std::make_shared<MasterGuardedStockProof>();
        proof->m_incoming = incoming;
        proof->m_challenger = challenger;
        proof->m_relation = *result.relation;
        const auto status = verify(*proof, false);
        return {status, status == GuardedRelationStatus::Verified ? std::move(proof) : nullptr};
    }

    GuardedRelationStatus MasterGuardedStockOwner::verify(const MasterGuardedStockProof & proof, bool fresh)
    {
        if (!m_ledger.consume(PROOF_CONTROL_WORK, RefinementWork::MasterWitness))
        {
            return GuardedRelationStatus::BudgetExhausted;
        }
        if (!proof.m_incoming || !proof.m_challenger || !owns(*proof.m_incoming) ||
            !owns(*proof.m_challenger) || proof.incoming().authority() != proof.challenger().authority() ||
            proof.incoming().economic() != proof.challenger().economic())
        {
            return GuardedRelationStatus::Invalid;
        }
        if (fresh)
        {
            const auto status = m_source->authenticate(m_ledger);
            if (status != GuardedRelationStatus::Verified)
            {
                return status;
            }
        }
        if (proof.incoming().currentFamily() || proof.challenger().currentFamily())
        {
            if (!proof.incoming().currentFamily() || !proof.challenger().currentFamily())
            {
                return GuardedRelationStatus::Invalid;
            }
            return verifyGuardedStockFamilyRelation(
                proof.incoming().root(), proof.challenger().root(),
                *proof.incoming().currentFamily(), *proof.challenger().currentFamily(), proof.relation(), m_ledger);
        }
        return verifyGuardedStockRelation(
            proof.incoming().root(), proof.challenger().root(), proof.relation(), m_ledger);
    }

    std::optional<std::int64_t> MasterGuardedStockOwner::finalizationWork() const
    {
        const auto preparation = m_source ? m_source->maximumPreparationWork() : std::nullopt;
        const auto authentication = m_source ? m_source->maximumAuthenticationWork() : std::nullopt;
        const auto rule = guardedStockRuleWork(GUARDED_STOCK_MAX_ACTORS, GUARDED_STOCK_MAX_TILES);
        const auto retention = rootRetentionWork(m_problem.actors().size(), GUARDED_STOCK_MAX_TILES);
        const auto family = m_source
            ? guardedStockFamilyWork(GUARDED_STOCK_MAX_ACTORS, m_source->maximumCurrentPrefixActions()) : std::nullopt;
        if (!preparation || *preparation < 0 || !authentication || *authentication < 0 ||
            !rule || !retention || !family)
        {
            return std::nullopt;
        }
        auto result = masterWorkProduct(static_cast<std::size_t>(*preparation), 2);
        if (!result || !RelativeStockDetail::checkedAdd(*result, *authentication) ||
            !RelativeStockDetail::checkedAdd(*result, 4 * (*rule + PROOF_CONTROL_WORK)) ||
            !RelativeStockDetail::checkedAdd(*result, 4 * *family) ||
            !RelativeStockDetail::checkedAdd(*result, 2 * *retention))
        {
            return std::nullopt;
        }
        return result;
    }

    GuardedRelationStatus MasterGuardedStockOwner::lastStatus() const
    {
        return m_lastStatus;
    }

    // The relation keeps every actor tile, and native binding ends each action on its candidate destination.
    bool MasterGuardedStockOwner::admits(std::size_t actor, std::int32_t candidate) const
    {
        const auto & actors = m_problem.actors();
        const auto & incumbents = m_problem.incumbentChoices();
        if (actor >= actors.size() || actor >= incumbents.size() || candidate < 0)
        {
            return true;
        }
        const auto & candidates = actors[actor].candidates;
        const auto incumbent = incumbents[actor];
        if (incumbent < 0 || static_cast<std::size_t>(incumbent) >= candidates.size() ||
            static_cast<std::size_t>(candidate) >= candidates.size())
        {
            return true;
        }
        return candidates[static_cast<std::size_t>(candidate)].destination ==
            candidates[static_cast<std::size_t>(incumbent)].destination;
    }
}
