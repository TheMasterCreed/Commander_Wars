#include "ai/coordinator/masterassignmentsearch.h"
#include "ai/coordinator/masteroperationservice.h"
#include "ai/coordinator/masterplanpricingadapter.h"

#include <bit>
#include <chrono>
#include <limits>
#include <map>
#include <queue>
#include <tuple>

namespace
{
    using namespace Coordinator;
    constexpr std::int64_t REFINEMENT_ACCOUNTING_WORK = 4;
    // Seeding one challenger costs at most the incumbent evaluation and root preparation already paid, twice over.
    constexpr std::size_t PLAN_PRICING_RESERVE_FACTOR = 2;
    // The incumbent tune takes at most half the balance left after the restriction; the rest funds the cap scan and the search.
    constexpr std::int64_t PLAN_PRICING_INCUMBENT_SHARE = 2;
    // The audit tune is bounded by the challenger tune's work limit, so it may draw on the whole scratch balance.
    constexpr std::int64_t NEIGHBOR_AUDIT_TUNE_SHARE = 1;

    enum class NodeStatus : std::int8_t
    {
        Ready,
        Infeasible,
        Pruned,
        Interrupted,
    };

    struct SearchNode
    {
        MasterAssignmentState state;
        std::optional<MilliFunds> upper;
        std::vector<std::pair<std::int32_t, std::int32_t>> canonicalChoices;
        std::int32_t branchActor{-1};
        std::vector<std::int32_t> branchChoices;
        std::size_t cursor{0};
    };

    struct NodeOrder
    {
        bool operator()(const std::shared_ptr<SearchNode> & left,
                        const std::shared_ptr<SearchNode> & right) const
        {
            if (left->upper.has_value() != right->upper.has_value())
            {
                return left->upper.has_value();
            }
            if (left->upper != right->upper)
            {
                return left->upper < right->upper;
            }
            if (left->branchChoices.size() != right->branchChoices.size())
            {
                return left->branchChoices.size() > right->branchChoices.size();
            }
            if (left->branchActor != right->branchActor)
            {
                return left->branchActor > right->branchActor;
            }
            return left->canonicalChoices > right->canonicalChoices;
        }
    };

    struct DeferredLeaf
    {
        std::shared_ptr<MasterAssignmentEvaluation> evaluation;
        std::optional<MilliFunds> upper;
        MasterRefinementTerm next{MasterRefinementTerm::Friendly};
        std::int64_t friendlySlice{0};
        std::int64_t enemySlice{0};
        std::int64_t dualSlice{0};
        bool schedulable{true};
        std::int64_t refinementWork{0};

        std::int64_t & allowance()
        {
            if (next == MasterRefinementTerm::Friendly)
            {
                return friendlySlice;
            }
            if (next == MasterRefinementTerm::Enemy)
            {
                return enemySlice;
            }
            return dualSlice;
        }
    };

    struct ProofCaps
    {
        std::optional<MilliFunds> incumbent;
        std::optional<MilliFunds> challenger;
        RelativeStockComparison uncapped;
    };

    struct FirstImprovement
    {
        std::optional<std::int64_t> work;
        std::optional<MilliFunds> lower;
        std::optional<std::int64_t> nanos;
    };

    std::optional<MilliFunds> minimum(
        std::optional<MilliFunds> left, std::optional<MilliFunds> right)
    {
        return left && right ? std::min(*left, *right) : left ? left : right;
    }

    class Search final
    {
    public:
        Search(const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator,
               RefinementLedger & ledger, const MasterAssignmentSearchOptions & options)
            : m_problem(problem),
              m_evaluator(evaluator),
              m_ledger(ledger),
              m_refusalsBefore(ledger.stats().refusals),
              m_workBefore(ledger.stats().spent()),
              m_options(options),
              m_bounds(problem, options.profile)
        {
            m_result.schedule = options.schedule;
        }

        MasterAssignmentSearchResult run(const CheapPricingModel* stock)
        {
            MasterProfileScope timing(m_options.profile, MasterPhase::Search);
            if (!initialize(stock))
            {
                return finish();
            }
            if (!seedFromPlanPricing())
            {
                return finish();
            }
            if (m_options.schedule.initialContributionCompletion && !completeInitialContributions())
            {
                return finish();
            }
            if (m_options.schedule.initialIncumbentAlternatives && !evaluateInitialAlternatives())
            {
                return finish();
            }
            while (!m_frontier.empty() || hasRefinement())
            {
                if (m_evaluator.hasGuardedStock() && m_result.improvement)
                {
                    break;
                }
                if (m_frontier.empty() || m_nodesSinceRefinement >= m_options.schedule.nodesPerRefinement)
                {
                    if (!refineLeaf())
                    {
                        break;
                    }
                    m_nodesSinceRefinement = 0;
                    if (m_frontier.empty())
                    {
                        continue;
                    }
                }
                if (!chargeHeap())
                {
                    break;
                }
                m_active = m_frontier.top();
                m_frontier.pop();
                if (prune(*m_active))
                {
                    m_active.reset();
                    continue;
                }
                if (m_active->state.complete())
                {
                    if (!evaluateLeaf(*m_active))
                    {
                        break;
                    }
                }
                else
                {
                    if (m_options.schedule.incumbentCompletions && !completeWithIncumbent(*m_active))
                    {
                        break;
                    }
                    if (m_evaluator.hasGuardedStock() && m_result.improvement)
                    {
                        break;
                    }
                    if (!prune(*m_active) && !expandActive())
                    {
                        break;
                    }
                }
                m_active.reset();
                ++m_nodesSinceRefinement;
            }
            return finish();
        }

    private:
        bool initialize(const CheapPricingModel* stock)
        {
            if (m_options.schedule.currentDualRounds < 0 || m_options.schedule.continuationSlice <= 0 ||
                m_options.schedule.refinementGrowthFactor < 2 ||
                m_options.schedule.futureDualAllowance <= 0 || m_options.schedule.nodesPerRefinement == 0 ||
                m_options.schedule.incumbentRefinementStride == 0 ||
                m_options.schedule.challengerRefinementBurst == 0 ||
                m_options.schedule.planPricingRounds < 0 || m_options.schedule.planPricingWork < 0 ||
                m_options.schedule.planPricingWitnessWork < 0 ||
                (m_options.schedule.futureBounds != MasterFutureBoundScope::RootOnly &&
                 m_options.schedule.futureBounds != MasterFutureBoundScope::EveryNode) ||
                (m_options.schedule.allocation != MasterRefinementAllocation::Baseline &&
                 m_options.schedule.allocation != MasterRefinementAllocation::OperationService) ||
                (m_options.schedule.allocation == MasterRefinementAllocation::OperationService &&
                 m_options.schedule.refinementGrowthFactor != MASTER_OPERATION_GROWTH_FACTOR) ||
                m_evaluator.hasContinuation() != (stock != nullptr) || !m_evaluator.matches(m_problem, m_ledger))
            {
                m_result.invalidInput = true;
                m_result.implicitRoot = true;
                m_result.stats.residualRegions = 1;
                m_unknownResidual = true;
                return false;
            }
            auto root = masterRootState(m_problem, m_ledger);
            if (!root)
            {
                m_result.budgetExhausted = true;
                m_result.implicitRoot = true;
                m_result.stats.residualRegions = 1;
                m_unknownResidual = true;
                return false;
            }
            m_active = std::make_shared<SearchNode>();
            m_active->state = std::move(*root);
            recordResidentRegions();
            {
                MasterProfileScope timing(m_options.profile, MasterPhase::InitialIncumbent);
                m_incumbent = std::make_shared<MasterAssignmentEvaluation>(m_evaluator.evaluate({
                    m_problem.identity(), std::vector<std::int32_t>(m_problem.incumbentChoices().begin(),
                                                                   m_problem.incumbentChoices().end())}));
            }
            m_result.incumbent = m_incumbent;
            ++m_result.stats.evaluated;
            recordEvaluation(*m_incumbent);
            consider(m_incumbent);
            if (m_evaluator.hasGuardedStock())
            {
                m_best = m_incumbent;
                m_unknownResidual = true;
            }
            if (m_incumbent->status == MasterEvaluationStatus::BudgetExhausted ||
                m_ledger.stats().refusals != m_refusalsBefore)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (m_incumbent->status == MasterEvaluationStatus::Feasible && m_incumbent->quote() &&
                !m_incumbent->exact && !defer(m_incumbent, std::nullopt))
            {
                return false;
            }
            const auto prepared = m_evaluator.hasGuardedStock() ? MasterBoundStatus::Unavailable :
                m_bounds.prepare(stock, m_ledger, m_evaluator.futureCatalog());
            if (prepared == MasterBoundStatus::BudgetExhausted || !prepareConflicts())
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (prepared == MasterBoundStatus::ArithmeticFailure)
            {
                ++m_result.stats.arithmeticFailures;
            }
            auto initial = prepareNode(*m_active);
            m_rootUpper = m_active->upper;
            if (initial == NodeStatus::Interrupted)
            {
                return false;
            }
            if (initial == NodeStatus::Ready)
            {
                if (!queue(m_active))
                {
                    return false;
                }
            }
            m_active.reset();
            return true;
        }

        bool completeInitialContributions()
        {
            if (m_frontier.empty() || !m_bounds.ready())
            {
                return true;
            }
            if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 2, RefinementWork::MasterReplay))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            SearchNode completion;
            completion.state = m_frontier.top()->state;
            completion.upper = m_frontier.top()->upper;
            while (!completion.state.complete())
            {
                const auto status = chooseBranch(completion);
                if (status == NodeStatus::Interrupted)
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                if (status != NodeStatus::Ready)
                {
                    return true;
                }
                if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 2, RefinementWork::MasterReplay))
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                SearchNode child;
                child.state = completion.state;
                child.upper = completion.upper;
                child.state.choices[static_cast<std::size_t>(completion.branchActor)] = completion.branchChoices.front();
                completion = std::move(child);
            }
            if (completion.state.choices == m_incumbent->state.choices)
            {
                return true;
            }
            ++m_result.stats.contributionCompletions;
            ++m_nodesSinceRefinement;
            return evaluateLeaf(completion);
        }

        bool evaluateInitialAlternatives()
        {
            if (m_frontier.empty() || m_incumbent->status != MasterEvaluationStatus::Feasible)
            {
                return true;
            }
            for (std::size_t actor = 0; actor < m_problem.actors().size(); ++actor)
            {
                if (!m_ledger.consume(1, RefinementWork::MasterExpansion))
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                if (!m_problem.canChangeActor(static_cast<std::int32_t>(actor)))
                {
                    continue;
                }
                for (const auto choice : m_problem.actors()[actor].canonicalCandidates)
                {
                    if (!m_ledger.consume(1, RefinementWork::MasterExpansion))
                    {
                        m_result.budgetExhausted = true;
                        return false;
                    }
                    if (choice == m_problem.incumbentChoices()[actor])
                    {
                        continue;
                    }
                    if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 2,
                                          RefinementWork::MasterReplay))
                    {
                        m_result.budgetExhausted = true;
                        return false;
                    }
                    SearchNode completion;
                    completion.state = m_incumbent->state;
                    completion.state.choices[actor] = choice;
                    completion.upper = m_rootUpper;
                    if (!evaluateLeaf(completion))
                    {
                        return false;
                    }
                    ++m_result.stats.initialIncumbentAlternatives;
                    ++m_nodesSinceRefinement;
                    if (m_nodesSinceRefinement >= m_options.schedule.nodesPerRefinement)
                    {
                        if (!refineLeaf())
                        {
                            return false;
                        }
                        m_nodesSinceRefinement = 0;
                    }
                }
            }
            return true;
        }

        bool seedFromPlanPricing()
        {
            const auto & schedule = m_options.schedule;
            auto & stats = m_result.stats.planPricing;
            if (schedule.planPricingRounds == 0)
            {
                return true;
            }
            if (schedule.planBoundCap == MasterPlanBoundCap::Verified)
            {
                stats.boundCap.emplace();
            }
            stats.outcome = MasterPlanPricingOutcome::Unavailable;
            const auto* stock = m_evaluator.stockModel();
            const auto* catalog = m_evaluator.futureCatalog();
            const auto challengerLimit = schedule.planPricingWork / MASTER_PLAN_PRICING_CHALLENGER_SHARE;
            const auto preparationWork = m_ledger.stats().spent() - m_workBefore;
            if (stock == nullptr || catalog == nullptr || m_evaluator.hasGuardedStock() || !m_bounds.ready() ||
                m_incumbent->status != MasterEvaluationStatus::Feasible || m_incumbent->quote() == nullptr ||
                challengerLimit <= 0)
            {
                return true;
            }
            const auto adapterStart = m_ledger.stats().spent();
            const auto instance = buildMasterPlanPricingInstance(m_problem, *stock, *catalog, m_ledger, challengerLimit);
            stats.adapterWork = m_ledger.stats().spent() - adapterStart;
            if (!instance)
            {
                return true;
            }
            stats.outcome = MasterPlanPricingOutcome::NoChallenger;
            const auto challengerStart = m_ledger.stats().spent();
            const auto priced = tuneMasterPlanPrices(instance->model,
                {.rounds = schedule.planPricingRounds, .maximumWork = challengerLimit,
                 .witnessBuildWork = schedule.planPricingWitnessWork}, m_ledger);
            stats.challengerRounds = priced.rounds;
            stats.challengerWork = m_ledger.stats().spent() - challengerStart;
            if (priced.witness.valid && !seedChallenger(*instance, priced.witness, preparationWork))
            {
                return false;
            }
            if (stats.outcome != MasterPlanPricingOutcome::ValuerFailure)
            {
                const auto prices = hintIncumbent(*instance, schedule.planPricingWork - stats.challengerWork);
                return capIncumbentOurs(prices);
            }
            return true;
        }

        bool seedChallenger(const MasterPlanPricingInstance & instance, const MasterPlanWitness & witness,
                            std::int64_t preparationWork)
        {
            auto & stats = m_result.stats.planPricing;
            const auto state = masterPlanWitnessState(m_problem, instance, witness);
            if (!state || state->choices == m_incumbent->state.choices)
            {
                return true;
            }
            const auto reserve = masterWorkProduct(static_cast<std::size_t>(preparationWork), PLAN_PRICING_RESERVE_FACTOR);
            if (!reserve || m_ledger.balance() < *reserve)
            {
                stats.outcome = MasterPlanPricingOutcome::Unavailable;
                return true;
            }
            const auto evaluation = evaluationFor(*state);
            if (!evaluation || evaluation->status == MasterEvaluationStatus::BudgetExhausted)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (evaluation->status != MasterEvaluationStatus::Feasible || evaluation->quote() == nullptr)
            {
                stats.outcome = MasterPlanPricingOutcome::ChallengerInfeasible;
                return true;
            }
            if (stats.boundCap)
            {
                m_planChallenger = evaluation;
            }
            const auto friendly = masterPlanFriendlyWitness(witness);
            const auto adopted = friendly
                ? m_evaluator.adoptFriendlyWitness(*evaluation, friendly->rows, friendly->value)
                : MasterRefinementStatus::Unavailable;
            switch (adopted)
            {
                case MasterRefinementStatus::Updated:
                    stats.outcome = MasterPlanPricingOutcome::Seeded;
                    stats.adoptedValue = friendly->value;
                    break;
                case MasterRefinementStatus::Unavailable:
                    stats.outcome = MasterPlanPricingOutcome::WitnessDeclined;
                    break;
                case MasterRefinementStatus::BudgetExhausted:
                    stats.outcome = MasterPlanPricingOutcome::AdoptionBudgetExhausted;
                    break;
                case MasterRefinementStatus::Invalid:
                    stats.outcome = MasterPlanPricingOutcome::ValuerFailure;
                    return true;
            }
            consider(evaluation);
            if (!defer(evaluation, m_rootUpper))
            {
                return false;
            }
            if (stats.outcome == MasterPlanPricingOutcome::Seeded)
            {
                m_refinementFocus = evaluation;
                m_focusRemaining = m_options.schedule.challengerRefinementBurst;
            }
            RelativeStockComparison comparison;
            if (!compare(evaluation, &comparison))
            {
                return false;
            }
            stats.firstComparisonImproved = comparison.improves();
            return true;
        }

        enum class RestrictedTuneStatus : std::int8_t
        {
            Overflow,
            Unaffordable,
            RestrictionFailed,
            Tuned,
        };

        struct RestrictedPricing
        {
            RestrictedTuneStatus status{RestrictedTuneStatus::Unaffordable};
            std::int64_t restrictionWork{0};
            MasterPlanPricingResult priced;
            std::int64_t tuneWork{0};
        };

        RestrictedPricing tuneRestricted(const MasterPlanPricingInstance & instance,
                                         const std::optional<std::vector<std::int32_t>> & forced,
                                         std::int64_t work, std::int64_t balanceShare, RefinementLedger & ledger) const
        {
            RestrictedPricing result;
            // Copying the model and rebuilding its actions for the restriction is paid separately.
            const auto restrictionWork = masterPlanRestrictionWork(instance.model);
            if (!restrictionWork)
            {
                result.status = RestrictedTuneStatus::Overflow;
                return result;
            }
            if (work <= 0 || ledger.balance() <= *restrictionWork)
            {
                return result;
            }
            work = std::min(work, (ledger.balance() - *restrictionWork) / balanceShare);
            if (work <= 0 || !ledger.consume(*restrictionWork, RefinementWork::MasterPreparation))
            {
                return result;
            }
            result.restrictionWork = *restrictionWork;
            // A plan the model cannot represent fails here, after the charge, like any other failed restriction.
            const auto restricted = forced ? restrictMasterPlanModel(instance.model, *forced) : std::nullopt;
            if (!restricted)
            {
                result.status = RestrictedTuneStatus::RestrictionFailed;
                return result;
            }
            const auto start = ledger.stats().spent();
            result.priced = tuneMasterPlanPrices(*restricted,
                {.rounds = m_options.schedule.planPricingRounds, .maximumWork = work,
                 .witnessBuildWork = m_options.schedule.planPricingWitnessWork}, ledger);
            result.tuneWork = ledger.stats().spent() - start;
            result.status = RestrictedTuneStatus::Tuned;
            return result;
        }

        std::vector<MilliFunds> hintIncumbent(const MasterPlanPricingInstance & instance, std::int64_t work)
        {
            auto & stats = m_result.stats.planPricing;
            const auto tuned = tuneRestricted(instance, masterPlanStateChoices(m_problem, instance, m_incumbent->state),
                                              work, PLAN_PRICING_INCUMBENT_SHARE, m_ledger);
            if (tuned.status == RestrictedTuneStatus::RestrictionFailed || tuned.status == RestrictedTuneStatus::Tuned)
            {
                stats.restrictionWork = tuned.restrictionWork;
            }
            if (tuned.status != RestrictedTuneStatus::Tuned)
            {
                return {};
            }
            const auto & priced = tuned.priced;
            stats.incumbentRounds = priced.rounds;
            stats.incumbentWork = tuned.tuneWork;
            stats.hintStored = !priced.prices.empty() && m_evaluator.hintFriendlyUpperPrices(*m_incumbent, priced.prices);
            return priced.prices;
        }

        bool capIncumbentOurs(std::span<const MilliFunds> futurePrices)
        {
            auto & stats = m_result.stats.planPricing;
            const auto* quote = m_incumbent->quote();
            if (!stats.boundCap || futurePrices.empty() || quote == nullptr)
            {
                return true;
            }
            auto & cap = *stats.boundCap;
            cap.incumbentOursUpperBefore = quote->ours.upper;
            cap.futurePrices.assign(futurePrices.begin(), futurePrices.end());
            std::vector<MilliFunds> prices(m_problem.resources().size(), 0);
            prices.insert(prices.end(), futurePrices.begin(), futurePrices.end());
            // The scan runs against a mirror of the balance so an unaffordable scan never records a refusal.
            RefinementLedger mirror;
            if (!mirror.open(m_ledger.balance()))
            {
                cap.status = MasterPlanBoundCapStatus::Unaffordable;
                return true;
            }
            const auto spentBefore = m_ledger.stats().spent();
            const auto bound = m_bounds.boundAt(m_incumbent->state, MasterBoundFamily::CombinedCurrentFuture,
                                                std::move(prices), mirror);
            const auto scanWork = mirror.stats().spent();
            if (!m_ledger.consume(scanWork, RefinementWork::MasterBoundScan))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            cap.work = scanWork;
            if (bound.status == MasterBoundStatus::BudgetExhausted)
            {
                cap.status = MasterPlanBoundCapStatus::Unaffordable;
                return true;
            }
            if (!bound.valid())
            {
                cap.status = MasterPlanBoundCapStatus::ScanInvalid;
                return true;
            }
            cap.scanUpper = bound.certificate->upper;
            // A verification never costs more than its scan.
            auto registrationWork = scanWork;
            if (!RelativeStockDetail::checkedAdd(registrationWork, m_evaluator.oursCapCheckWork()) ||
                m_ledger.balance() < registrationWork)
            {
                cap.status = MasterPlanBoundCapStatus::Unaffordable;
                return true;
            }
            const auto receipt = m_evaluator.capOursUpper(*m_incumbent, *bound.certificate, m_bounds);
            cap.work = m_ledger.stats().spent() - spentBefore;
            cap.oursCeiling = receipt.ceiling;
            if (m_ledger.stats().refusals != m_refusalsBefore)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            switch (receipt.status)
            {
                case MasterOursCapStatus::Registered:
                    cap.status = MasterPlanBoundCapStatus::Registered;
                    break;
                case MasterOursCapStatus::Rejected:
                    cap.status = MasterPlanBoundCapStatus::VerifyFailed;
                    return true;
                case MasterOursCapStatus::Contradiction:
                    cap.status = MasterPlanBoundCapStatus::Contradiction;
                    ++m_result.stats.proofFailures;
                    dropCappedImprovement();
                    return true;
                case MasterOursCapStatus::OwnedMismatch:
                    cap.status = MasterPlanBoundCapStatus::OwnedMismatch;
                    ++m_result.stats.proofFailures;
                    return true;
                case MasterOursCapStatus::CatalogMismatch:
                    cap.status = MasterPlanBoundCapStatus::CatalogMismatch;
                    ++m_result.stats.proofFailures;
                    return true;
            }
            if (!m_planChallenger)
            {
                return true;
            }
            RelativeStockComparison comparison;
            if (!compare(m_planChallenger, &comparison))
            {
                return false;
            }
            cap.improvedAtCap = comparison.improves();
            return true;
        }

        static bool capped(const MasterImprovementProof & proof)
        {
            return proof.incumbentOursCap || proof.challengerOursCap;
        }

        void dropCappedImprovement()
        {
            if (!m_result.improvement || !capped(*m_result.improvement))
            {
                return;
            }
            m_result.improvement = std::move(m_uncappedImprovement);
            m_uncappedImprovement.reset();
            if (m_result.stats.planPricing.boundCap)
            {
                m_result.stats.planPricing.boundCap->cappedProofDropped = true;
            }
            m_result.stats.firstImprovementWork = m_firstUncapped.work;
            m_result.stats.firstImprovementLower = m_firstUncapped.lower;
            if (m_options.profile != nullptr)
            {
                m_options.profile->firstImprovementNanos = m_firstUncapped.nanos;
            }
        }

        bool expandActive()
        {
            MasterProfileScope timing(m_options.profile, MasterPhase::Expansion);
            ++m_result.stats.expanded;
            while (m_active->cursor < m_active->branchChoices.size())
            {
                if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 2,
                                      RefinementWork::MasterExpansion))
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                auto child = std::make_shared<SearchNode>();
                child->state = m_active->state;
                child->upper = m_active->upper;
                child->state.choices[static_cast<std::size_t>(m_active->branchActor)] =
                    m_active->branchChoices[m_active->cursor];
                const auto status = prepareNode(*child);
                if (status == NodeStatus::Interrupted ||
                    (status == NodeStatus::Ready && !queue(child)))
                {
                    return false;
                }
                ++m_active->cursor;
            }
            return true;
        }

        bool completeWithIncumbent(const SearchNode & node)
        {
            MasterProfileScope timing(m_options.profile, MasterPhase::LeafReplay);
            if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1, 3, RefinementWork::MasterReplay))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            SearchNode completion;
            completion.state = node.state;
            completion.upper = node.upper;
            for (std::size_t actor = 0; actor < completion.state.choices.size(); ++actor)
            {
                auto & choice = completion.state.choices[actor];
                if (choice == MASTER_UNASSIGNED)
                {
                    choice = m_problem.incumbentChoices()[actor];
                }
                if (choice == NO_CANDIDATE && m_problem.canChangeActor(static_cast<std::int32_t>(actor)))
                {
                    return true;
                }
            }
            if (completion.state.choices == m_incumbent->state.choices)
            {
                return true;
            }
            ++m_result.stats.incumbentCompletions;
            return evaluateLeaf(completion);
        }

        bool prepareConflicts()
        {
            if (!chargeMasterWork(m_ledger, m_problem.resources().size() + m_problem.actors().size() + 1,
                                  m_problem.actors().size() + 1, RefinementWork::MasterPreparation))
            {
                return false;
            }
            std::vector<std::uint64_t> owners(m_problem.resources().size(), 0);
            std::vector<std::vector<bool>> uses(m_problem.actors().size(),
                                              std::vector<bool>(m_problem.resources().size(), false));
            for (const auto & actor : m_problem.actors())
            {
                const auto slot = static_cast<std::size_t>(actor.canonicalSlot);
                for (const auto & candidate : actor.candidates)
                {
                    if (!chargeMasterWork(m_ledger, candidate.resources.size() + 1, 1,
                                          RefinementWork::MasterPreparation))
                    {
                        return false;
                    }
                    for (const auto & resource : candidate.resources)
                    {
                        const auto index = static_cast<std::size_t>(resource.resourceIndex);
                        if (m_problem.resources()[index].kind != MasterResourceKind::DamageTarget &&
                            !uses[slot][index])
                        {
                            uses[slot][index] = true;
                            ++owners[index];
                        }
                    }
                }
            }
            for (const auto & actorUses : uses)
            {
                std::uint64_t degree = 0;
                for (std::size_t resource = 0; resource < owners.size(); ++resource)
                {
                    if (actorUses[resource])
                    {
                        degree += owners[resource] - 1;
                    }
                }
                m_conflictDegree.push_back(degree);
            }
            return true;
        }

        std::optional<bool> compatible(const MasterAssignmentState & state, std::size_t actor,
                                       std::int32_t choice, RefinementLedger & ledger) const
        {
            const auto & candidate = m_problem.actors()[actor].candidates[static_cast<std::size_t>(choice)];
            for (std::size_t other = 0; other < state.choices.size(); ++other)
            {
                if (!chargeMasterWork(ledger, 1, 1, RefinementWork::MasterExpansion))
                {
                    return std::nullopt;
                }
                const auto selected = state.choices[other];
                if (other == actor || selected < 0 || !candidate.plannable)
                {
                    continue;
                }
                const auto & otherCandidate = m_problem.actors()[other].candidates[
                    static_cast<std::size_t>(selected)];
                if (!otherCandidate.plannable)
                {
                    continue;
                }
                if (!chargeMasterWork(ledger, candidate.resources.size() + 1,
                                      otherCandidate.resources.size() + 1, RefinementWork::MasterExpansion))
                {
                    return std::nullopt;
                }
                if (m_problem.incompatible({static_cast<std::int32_t>(actor), choice},
                                           {static_cast<std::int32_t>(other), selected}))
                {
                    return false;
                }
            }
            return true;
        }

        NodeStatus chooseBranch(SearchNode & node)
        {
            MasterProfileScope timing(m_options.profile, MasterPhase::Expansion);
            if (!chargeMasterWork(m_ledger, node.state.choices.size() + 1, 1,
                                  RefinementWork::MasterExpansion))
            {
                return NodeStatus::Interrupted;
            }
            node.canonicalChoices.reserve(node.state.choices.size());
            for (std::size_t actor = 0; actor < node.state.choices.size(); ++actor)
            {
                const auto selected = node.state.choices[actor];
                node.canonicalChoices.push_back(selected < 0 ? std::pair{selected, selected} :
                    m_problem.actors()[actor].candidates[static_cast<std::size_t>(selected)].identity.canonicalOrderKey());
                if (selected >= 0)
                {
                    const auto legal = compatible(node.state, actor, selected, m_ledger);
                    if (!legal)
                    {
                        return NodeStatus::Interrupted;
                    }
                    if (!*legal)
                    {
                        return NodeStatus::Infeasible;
                    }
                }
                if (selected != MASTER_UNASSIGNED)
                {
                    continue;
                }
                std::vector<std::int32_t> domain;
                for (const auto choice : m_problem.actors()[actor].canonicalCandidates)
                {
                    const auto legal = compatible(node.state, actor, choice, m_ledger);
                    if (!legal)
                    {
                        return NodeStatus::Interrupted;
                    }
                    if (*legal && !m_evaluator.guardedAdmits(actor, choice))
                    {
                        ++m_result.stats.guardedExcluded;
                    }
                    else if (*legal)
                    {
                        domain.push_back(choice);
                    }
                }
                if (domain.empty())
                {
                    return NodeStatus::Infeasible;
                }
                if (node.branchActor < 0 || domain.size() < node.branchChoices.size() ||
                    (domain.size() == node.branchChoices.size() &&
                     m_conflictDegree[actor] > m_conflictDegree[static_cast<std::size_t>(node.branchActor)]))
                {
                    node.branchActor = static_cast<std::int32_t>(actor);
                    node.branchChoices = std::move(domain);
                }
            }
            if (!chargeMasterWork(m_ledger, node.branchChoices.size(),
                                  std::bit_width(node.branchChoices.size()) + 1, RefinementWork::MasterExpansion))
            {
                return NodeStatus::Interrupted;
            }
            std::stable_sort(node.branchChoices.begin(), node.branchChoices.end(),
                [&](std::int32_t left, std::int32_t right)
                {
                    const auto lhs = m_bounds.contribution(static_cast<std::size_t>(node.branchActor), left);
                    const auto rhs = m_bounds.contribution(static_cast<std::size_t>(node.branchActor), right);
                    if (lhs.has_value() != rhs.has_value())
                    {
                        return !lhs.has_value();
                    }
                    return lhs != rhs ? lhs > rhs : false;
                });
            return NodeStatus::Ready;
        }

        bool prune(const SearchNode & node)
        {
            if (m_evaluator.hasGuardedStock())
            {
                return false;
            }
            if (!node.upper || !m_result.lower || *node.upper > *m_result.lower)
            {
                return false;
            }
            ++m_result.stats.pruned;
            bool containsIncumbent = true;
            for (std::size_t actor = 0; actor < node.state.choices.size(); ++actor)
            {
                if (node.state.choices[actor] != MASTER_UNASSIGNED &&
                    node.state.choices[actor] != m_problem.incumbentChoices()[actor])
                {
                    containsIncumbent = false;
                    break;
                }
            }
            if (containsIncumbent)
            {
                m_incumbentUpper = minimum(m_incumbentUpper, node.upper);
            }
            if (m_options.retainDiagnostics)
            {
                m_result.prunes.push_back({node.state, *node.upper, *m_result.lower});
            }
            return true;
        }

        NodeStatus prepareNode(SearchNode & node)
        {
            const auto branch = chooseBranch(node);
            if (branch != NodeStatus::Ready)
            {
                if (branch == NodeStatus::Infeasible)
                {
                    ++m_result.stats.infeasible;
                }
                else
                {
                    m_result.budgetExhausted = true;
                }
                return branch;
            }
            if (!m_evaluator.hasGuardedStock() && m_bounds.ready() && !node.state.complete())
            {
                for (const auto family : {MasterBoundFamily::Immediate, MasterBoundFamily::EndpointCoupled,
                                          MasterBoundFamily::CombinedCurrentFuture})
                {
                    if (family == MasterBoundFamily::CombinedCurrentFuture &&
                        (m_evaluator.futureCatalog() == nullptr ||
                         (m_options.schedule.futureBounds == MasterFutureBoundScope::RootOnly &&
                          m_result.stats.expanded != 0)))
                    {
                        continue;
                    }
                    auto result = m_bounds.bound(node.state, family, m_ledger);
                    if (result.valid())
                    {
                        node.upper = minimum(node.upper, result.certificate->upper);
                        if ((family == MasterBoundFamily::EndpointCoupled ||
                             family == MasterBoundFamily::CombinedCurrentFuture) &&
                            m_options.schedule.currentDualRounds > 0)
                        {
                            result = m_bounds.tighten(*result.certificate, m_options.schedule.currentDualRounds, m_ledger);
                            if (result.certificate)
                            {
                                node.upper = minimum(node.upper, result.certificate->upper);
                            }
                        }
                    }
                    if (result.status == MasterBoundStatus::BudgetExhausted)
                    {
                        m_result.budgetExhausted = true;
                        return NodeStatus::Interrupted;
                    }
                    if (result.status == MasterBoundStatus::ArithmeticFailure)
                    {
                        ++m_result.stats.arithmeticFailures;
                    }
                }
            }
            return prune(node) ? NodeStatus::Pruned : NodeStatus::Ready;
        }

        bool chargeHeap()
        {
            if (!chargeMasterWork(m_ledger, std::bit_width(m_frontier.size() + 1) + 1,
                                  2 * (m_problem.actors().size() + 1), RefinementWork::MasterExpansion))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            return true;
        }

        bool queue(const std::shared_ptr<SearchNode> & node)
        {
            if (!chargeHeap())
            {
                return false;
            }
            m_frontier.push(node);
            m_result.stats.peakFrontier = std::max<std::uint64_t>(
                m_result.stats.peakFrontier, m_frontier.size());
            recordResidentRegions();
            return true;
        }

        void recordResidentRegions()
        {
            const bool activeQueued = m_active && !m_frontier.empty() && m_frontier.top() == m_active;
            m_result.stats.peakResidentRegions = std::max<std::uint64_t>(
                m_result.stats.peakResidentRegions,
                m_frontier.size() + m_leaves.size() + (m_active && !activeQueued ? 1 : 0));
        }

        void consider(const std::shared_ptr<MasterAssignmentEvaluation> & evaluation)
        {
            if (m_evaluator.hasGuardedStock())
            {
                return;
            }
            if (evaluation->status != MasterEvaluationStatus::Feasible ||
                !evaluation->objective || !evaluation->lowerReplayed)
            {
                return;
            }
            if (!m_result.lower || evaluation->objective->lower > *m_result.lower ||
                (evaluation == m_incumbent && evaluation->objective->lower == *m_result.lower))
            {
                m_result.lower = evaluation->objective->lower;
                m_best = evaluation;
            }
        }

        void recordEvaluation(const MasterAssignmentEvaluation & evaluation)
        {
            if (m_options.observer != nullptr)
            {
                m_options.observer->evaluated(evaluation, m_ledger.stats().spent());
            }
            if (evaluation.status == MasterEvaluationStatus::Infeasible)
            {
                ++m_result.stats.infeasible;
            }
            if (evaluation.status == MasterEvaluationStatus::Unsupported ||
                evaluation.status == MasterEvaluationStatus::Invalid)
            {
                ++m_result.stats.unsupported;
            }
            if (evaluation.status == MasterEvaluationStatus::ArithmeticFailure ||
                evaluation.objectiveStatus == MasterObjectiveStatus::ArithmeticFailure)
            {
                ++m_result.stats.arithmeticFailures;
            }
            if (evaluation.objectiveStatus == MasterObjectiveStatus::InvalidIdentity ||
                evaluation.objectiveStatus == MasterObjectiveStatus::WitnessFailure)
            {
                ++m_result.stats.proofFailures;
            }
        }

        bool compare(std::shared_ptr<MasterAssignmentEvaluation> evaluation,
                     RelativeStockComparison* observed = nullptr)
        {
            if (evaluation == m_incumbent || !m_incumbent ||
                m_incumbent->status != MasterEvaluationStatus::Feasible ||
                evaluation->status != MasterEvaluationStatus::Feasible)
            {
                return true;
            }
            if (m_evaluator.hasGuardedStock())
            {
                return compareGuarded(evaluation);
            }
            const auto contradictions = m_evaluator.oursCapContradictions();
            const auto comparison = m_evaluator.compare(*m_incumbent, *evaluation);
            noteCapContradictions(contradictions);
            // Read before any observer runs, so a proof records exactly the ceilings its comparison applied.
            ProofCaps caps{m_evaluator.oursCap(*m_incumbent), m_evaluator.oursCap(*evaluation), comparison};
            if (comparison.improves() && (caps.incumbent || caps.challenger))
            {
                caps.uncapped = m_evaluator.recordedComparison(*m_incumbent, *evaluation, std::nullopt, std::nullopt);
            }
            if (observed != nullptr)
            {
                *observed = comparison;
            }
            if (m_refinementFocus == evaluation && comparison.valid() && !comparison.unresolved())
            {
                m_refinementFocus.reset();
                m_focusRemaining = 0;
            }
            if (m_options.observer != nullptr)
            {
                m_options.observer->comparisonObserved(
                    *m_incumbent, *evaluation, comparison, m_ledger.stats().spent());
                m_options.observer->compared(*evaluation, comparison, m_ledger.stats().spent());
            }
            consider(m_incumbent);
            consider(evaluation);
            if (comparison.improves() && !recordImprovement(evaluation, comparison, caps))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (m_ledger.stats().refusals != m_refusalsBefore)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            return true;
        }

        void noteCapContradictions(std::uint64_t before)
        {
            if (m_evaluator.oursCapContradictions() == before)
            {
                return;
            }
            ++m_result.stats.proofFailures;
            if (m_result.stats.planPricing.boundCap)
            {
                m_result.stats.planPricing.boundCap->status = MasterPlanBoundCapStatus::Contradiction;
            }
            dropCappedImprovement();
        }

        bool chargeProofCopy()
        {
            return chargeMasterWork(m_ledger, m_problem.actors().size() + 1,
                                    m_problem.actors().size() + 1, RefinementWork::MasterWitness);
        }

        void noteFirstUncapped(MilliFunds lower)
        {
            if (m_firstUncapped.work)
            {
                return;
            }
            m_firstUncapped.work = m_ledger.stats().spent();
            m_firstUncapped.lower = lower;
            if (m_options.profile != nullptr)
            {
                m_firstUncapped.nanos = m_options.profile->elapsedNanos();
            }
        }

        // Keeps the best proof and, while that one relies on a cap, the best proof that relies on none.
        bool recordImprovement(const std::shared_ptr<MasterAssignmentEvaluation> & evaluation,
                               const RelativeStockComparison & comparison, const ProofCaps & caps)
        {
            const bool relies = caps.incumbent || caps.challenger;
            const auto & uncapped = caps.uncapped;
            if (!m_result.improvement || comparison.deltaLower > m_result.improvement->comparison.deltaLower)
            {
                if (!chargeProofCopy())
                {
                    return false;
                }
                auto previous = std::move(m_result.improvement);
                m_result.improvement = MasterImprovementProof{
                    *m_incumbent, *evaluation, comparison, m_ledger.stats().spent()};
                m_result.improvement->incumbentOursCap = caps.incumbent;
                m_result.improvement->challengerOursCap = caps.challenger;
                if (!m_result.stats.firstImprovementWork)
                {
                    m_result.stats.firstImprovementWork = m_ledger.stats().spent();
                    m_result.stats.firstImprovementLower = comparison.deltaLower;
                    if (m_options.profile != nullptr)
                    {
                        m_options.profile->recordFirstImprovement();
                    }
                }
                if (!relies)
                {
                    m_uncappedImprovement.reset();
                    noteFirstUncapped(comparison.deltaLower);
                    return true;
                }
                if (previous && !capped(*previous))
                {
                    m_uncappedImprovement = std::move(previous);
                }
            }
            else if (!capped(*m_result.improvement))
            {
                return true;
            }
            if (!uncapped.improves() || (m_uncappedImprovement &&
                uncapped.deltaLower <= m_uncappedImprovement->comparison.deltaLower))
            {
                return true;
            }
            if (!chargeProofCopy())
            {
                return false;
            }
            m_uncappedImprovement = MasterImprovementProof{
                *m_incumbent, *evaluation, uncapped, m_ledger.stats().spent()};
            noteFirstUncapped(uncapped.deltaLower);
            return true;
        }

        bool compareGuarded(const std::shared_ptr<MasterAssignmentEvaluation> & evaluation)
        {
            const auto comparison = m_evaluator.compareGuarded(*m_incumbent, *evaluation);
            if (m_options.observer != nullptr)
            {
                m_options.observer->guardedCompared(
                    *m_incumbent, *evaluation, comparison, m_ledger.stats().spent());
            }
            if (comparison.improves())
            {
                auto copies = m_evaluator.retainedCopyWork(*m_incumbent);
                const auto challengerCopy = m_evaluator.retainedCopyWork(*evaluation);
                if (!copies || !challengerCopy ||
                    !RelativeStockDetail::checkedAdd(*copies, *challengerCopy) ||
                    !m_ledger.consume(*copies, RefinementWork::MasterWitness))
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                m_result.improvement = MasterImprovementProof{
                    *m_incumbent, *evaluation, {}, m_ledger.stats().spent(), comparison.proof};
                m_best = evaluation;
                m_result.stats.firstImprovementWork = m_ledger.stats().spent();
                if (m_options.profile != nullptr)
                {
                    m_options.profile->recordFirstImprovement();
                }
            }
            if (comparison.status == GuardedRelationStatus::BudgetExhausted || m_ledger.stats().refusals != m_refusalsBefore)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            return true;
        }

        std::shared_ptr<MasterAssignmentEvaluation> evaluationFor(const MasterAssignmentState & state)
        {
            if (state.choices == m_incumbent->state.choices)
            {
                return m_incumbent;
            }
            if (!chargeMasterWork(m_ledger, state.choices.size() + 1,
                                  4 * std::bit_width(m_evaluations.size() + 1) + 3, RefinementWork::MasterReplay))
            {
                m_result.budgetExhausted = true;
                return {};
            }
            const auto found = m_evaluations.find(state.choices);
            if (found != m_evaluations.end())
            {
                ++m_result.stats.evaluationCacheHits;
                return found->second;
            }
            auto evaluation = std::make_shared<MasterAssignmentEvaluation>(m_evaluator.evaluate(state));
            ++m_result.stats.evaluated;
            recordEvaluation(*evaluation);
            if (evaluation->status != MasterEvaluationStatus::BudgetExhausted)
            {
                m_evaluations.emplace(state.choices, evaluation);
            }
            return evaluation;
        }

        bool evaluateLeaf(const SearchNode & node)
        {
            const auto evaluation = evaluationFor(node.state);
            if (!evaluation || evaluation->status == MasterEvaluationStatus::BudgetExhausted)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (evaluation->status == MasterEvaluationStatus::Infeasible)
            {
                return true;
            }
            consider(evaluation);
            return defer(evaluation, node.upper) && compare(evaluation);
        }

        bool defer(const std::shared_ptr<MasterAssignmentEvaluation> & evaluation, std::optional<MilliFunds> upper)
        {
            if (!chargeMasterWork(m_ledger, m_leaves.size() + 1, 1, RefinementWork::MasterExpansion))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (evaluation->objective && evaluation->upperVerified)
            {
                upper = minimum(upper, evaluation->objective->upper);
            }
            for (auto & existing : m_leaves)
            {
                if (existing.evaluation == evaluation)
                {
                    existing.upper = minimum(existing.upper, upper);
                    return true;
                }
            }
            DeferredLeaf leaf{evaluation, upper, MasterRefinementTerm::Friendly,
                              m_options.schedule.continuationSlice, m_options.schedule.continuationSlice,
                              m_options.schedule.futureDualAllowance};
            leaf.schedulable = evaluation->status == MasterEvaluationStatus::Feasible &&
                               evaluation->quote() != nullptr && !evaluation->exact;
            m_leaves.push_back(std::move(leaf));
            recordResidentRegions();
            return true;
        }

        bool hasRefinement()
        {
            if (m_evaluator.hasGuardedStock())
            {
                return false;
            }
            if (!chargeMasterWork(m_ledger, m_leaves.size() + 1, 1, RefinementWork::MasterExpansion))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            return std::any_of(m_leaves.begin(), m_leaves.end(), [&](const DeferredLeaf & leaf)
            {
                return leaf.schedulable && (!leaf.upper || !m_result.lower || *leaf.upper > *m_result.lower);
            });
        }

        DeferredLeaf* selectRefinement()
        {
            DeferredLeaf* selected = nullptr;
            DeferredLeaf* incumbent = nullptr;
            DeferredLeaf* focused = nullptr;
            for (auto & leaf : m_leaves)
            {
                if (!leaf.schedulable || (leaf.upper && m_result.lower && *leaf.upper <= *m_result.lower))
                {
                    continue;
                }
                if (leaf.evaluation == m_incumbent)
                {
                    incumbent = &leaf;
                }
                if (leaf.evaluation == m_refinementFocus)
                {
                    focused = &leaf;
                }
                if (selected == nullptr || (!leaf.upper && selected->upper) ||
                    (leaf.upper && selected->upper && *leaf.upper > *selected->upper))
                {
                    selected = &leaf;
                }
            }
            if (focused == nullptr || m_focusRemaining == 0)
            {
                m_refinementFocus.reset();
                m_focusRemaining = 0;
            }
            if (incumbent != nullptr &&
                m_result.stats.refinements % m_options.schedule.incumbentRefinementStride == 0)
            {
                if (focused != nullptr && m_focusRemaining > 0 &&
                    incumbent->refinementWork > focused->refinementWork)
                {
                    ++m_result.stats.workBalancedSelections;
                    return focused;
                }
                return incumbent;
            }
            if (focused != nullptr && m_focusRemaining > 0)
            {
                return focused;
            }
            if (selected != nullptr && selected != incumbent)
            {
                m_refinementFocus = selected->evaluation;
                m_focusRemaining = m_options.schedule.challengerRefinementBurst;
            }
            return selected;
        }

        bool enemyCancelsInFocus()
        {
            if (!m_incumbent || !m_refinementFocus)
            {
                return false;
            }
            const auto contradictions = m_evaluator.oursCapContradictions();
            const bool cancels = m_evaluator.unresolvedCommonEnemy(*m_incumbent, *m_refinementFocus);
            noteCapContradictions(contradictions);
            return cancels;
        }

        bool refineLeaf()
        {
            if (!chargeMasterWork(m_ledger, m_leaves.size() + 1, m_problem.actors().size() + 1,
                                  RefinementWork::MasterExpansion) ||
                !m_ledger.consume(REFINEMENT_ACCOUNTING_WORK, RefinementWork::MasterExpansion))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            const auto balancedBefore = m_result.stats.workBalancedSelections;
            auto* selected = selectRefinement();
            if (selected == nullptr)
            {
                return true;
            }
            std::optional<MasterRefinementTicketReceipt> observation;
            if (m_options.observer != nullptr &&
                m_options.schedule.allocation == MasterRefinementAllocation::OperationService)
            {
                observation.emplace(MasterRefinementTicketReceipt{
                    *selected->evaluation, selected->next, selected->next,
                    selected->refinementWork, selected->refinementWork,
                    m_result.stats.refinements, m_result.stats.refinements,
                    m_focusRemaining, m_focusRemaining, selected->evaluation == m_refinementFocus,
                    m_result.stats.workBalancedSelections != balancedBefore,
                    m_result.improvement.has_value(), m_result.improvement.has_value(), false});
            }
            const auto workBefore = m_ledger.stats().spent();
            const auto serviceBefore = m_result.stats.operationPaidWork;
            const auto parkedBefore = m_result.stats.fundingParkedTickets;
            if (observation)
            {
                const auto challenger = selected->evaluation == m_incumbent
                    ? m_refinementFocus : selected->evaluation;
                m_options.observer->ticketStarted(
                    *observation, *m_incumbent, challenger.get(), workBefore);
            }
            const bool completed = m_options.schedule.allocation == MasterRefinementAllocation::OperationService
                ? refineOperationLeaf(*selected) : refineSelectedLeaf(*selected);
            // Each disjoint ledger delta includes refresh, eligibility and comparison work.
            selected->refinementWork += m_ledger.stats().spent() - workBefore;
            if (m_options.schedule.allocation == MasterRefinementAllocation::OperationService)
            {
                m_result.stats.operationScreeningWork += m_ledger.stats().spent() - workBefore -
                    (m_result.stats.operationPaidWork - serviceBefore);
            }
            if (observation)
            {
                observation->cursorAfter = selected->next;
                observation->aggregateAfter = selected->refinementWork;
                observation->refinementsAfter = m_result.stats.refinements;
                observation->focusAfter = m_focusRemaining;
                observation->proofAfter = m_result.improvement.has_value();
                observation->completed = completed;
                observation->fundingParked = m_result.stats.fundingParkedTickets != parkedBefore;
                m_options.observer->ticketFinished(*observation);
            }
            return completed;
        }

        static void rotate(DeferredLeaf & leaf)
        {
            if (leaf.next == MasterRefinementTerm::Friendly)
            {
                leaf.next = MasterRefinementTerm::Enemy;
            }
            else if (leaf.next == MasterRefinementTerm::Enemy)
            {
                leaf.next = MasterRefinementTerm::FutureDual;
            }
            else
            {
                leaf.next = MasterRefinementTerm::Friendly;
            }
        }

        void completeTicket(DeferredLeaf & leaf)
        {
            ++m_result.stats.refinements;
            if (leaf.evaluation == m_refinementFocus && m_focusRemaining > 0)
            {
                --m_focusRemaining;
            }
            if (leaf.evaluation == m_incumbent)
            {
                ++m_result.stats.incumbentRefinements;
            }
            rotate(leaf);
        }

        void refreshLeafBound(DeferredLeaf & leaf)
        {
            if (leaf.evaluation->objective && leaf.evaluation->upperVerified)
            {
                leaf.upper = minimum(leaf.upper, leaf.evaluation->objective->upper);
            }
            leaf.schedulable = leaf.schedulable && !leaf.evaluation->exact;
            consider(leaf.evaluation);
        }

        bool stopOperation(MasterOperationStatus status)
        {
            m_result.operationStop = status;
            if (status == MasterOperationStatus::FundingRefused)
            {
                m_result.refinementRefusal = m_evaluator.refinementRefusal();
                m_result.budgetExhausted = true;
            }
            else if (status == MasterOperationStatus::Invalid)
            {
                ++m_result.stats.proofFailures;
            }
            return false;
        }

        bool creditOperation(MasterOperationService & operation, std::int64_t work)
        {
            if (!MasterOperationRegistry::credit(operation, work) ||
                !RelativeStockDetail::checkedAdd(m_result.stats.operationPaidWork, work))
            {
                ++m_result.stats.arithmeticFailures;
                return false;
            }
            return true;
        }

        bool inspectOperations(
            std::span<MasterOperationDescriptor> roles, std::span<const std::size_t> indices,
            std::array<std::optional<MasterOperationOffer>, MASTER_OPERATION_ROLE_LIMIT> & offers,
            MasterOperationStatus & unavailable)
        {
            std::array<bool, MASTER_OPERATION_ROLE_LIMIT> inspected{};
            for (std::size_t pass = 0; pass < roles.size(); ++pass)
            {
                std::optional<std::size_t> next;
                std::optional<std::size_t> ready;
                for (std::size_t role = 0; role < roles.size(); ++role)
                {
                    if (!inspected[role])
                    {
                        if (!next || MasterOperationRegistry::before(
                                m_operations.at(indices[role]), m_operations.at(indices[*next])))
                        {
                            next = role;
                        }
                    }
                    else if (offers[role] &&
                             offers[role]->remainingRequiredWork <= m_ledger.balance() - MASTER_OPERATION_QUOTA_WORK &&
                             (!ready || MasterOperationRegistry::before(
                                 m_operations.at(indices[role]), m_operations.at(indices[*ready]))))
                    {
                        ready = role;
                    }
                }
                if (!next || (ready && MasterOperationRegistry::before(
                                  m_operations.at(indices[*ready]), m_operations.at(indices[*next]))))
                {
                    break;
                }
                auto & descriptor = roles[*next];
                auto & operation = m_operations.at(indices[*next]);
                for (std::size_t role = 0; role < roles.size(); ++role)
                {
                    if (indices[role] == indices[*next])
                    {
                        inspected[role] = true;
                        if (role != *next)
                        {
                            descriptor.lowerRelevant = descriptor.lowerRelevant || roles[role].lowerRelevant;
                            descriptor.upperRelevant = descriptor.upperRelevant || roles[role].upperRelevant;
                        }
                    }
                }
                if (operation.growthExhausted)
                {
                    unavailable = MasterOperationStatus::FundingRefused;
                    continue;
                }
                const auto offerStart = m_options.observer != nullptr ? m_ledger.stats().spent() : 0;
                auto offer = m_evaluator.operationOffer(
                    *descriptor.evaluation, descriptor.term, operation.allowance, operation.readiness);
                if (m_options.observer != nullptr)
                {
                    m_options.observer->operationOffered(
                        *descriptor.evaluation, offer, descriptor.lowerRelevant, descriptor.upperRelevant,
                        operation.paidWork, offerStart, m_ledger.stats().spent());
                }
                if (!creditOperation(operation, offer.preparationWork))
                {
                    return stopOperation(MasterOperationStatus::Invalid);
                }
                if (offer.preparation.status == RelativeStockPreparationStatus::Ready &&
                    offer.allowance > operation.allowance)
                {
                    operation.allowance = offer.allowance;
                }
                if (offer.status == MasterOperationStatus::Invalid)
                {
                    return stopOperation(offer.status);
                }
                if (offer.status == MasterOperationStatus::Ready)
                {
                    offers[*next] = std::move(offer);
                }
                else if (offer.status == MasterOperationStatus::FundingRefused)
                {
                    unavailable = MasterOperationStatus::FundingRefused;
                }
                else if (offer.status == MasterOperationStatus::MissingPrerequisite &&
                         unavailable != MasterOperationStatus::FundingRefused)
                {
                    unavailable = MasterOperationStatus::MissingPrerequisite;
                }
                if (m_ledger.stats().refusals != m_refusalsBefore)
                {
                    return stopOperation(MasterOperationStatus::FundingRefused);
                }
            }
            return true;
        }

        std::optional<std::size_t> affordableOperation(
            std::span<const std::optional<MasterOperationOffer>> offers, std::span<const std::size_t> indices,
            MasterOperationStatus & unavailable) const
        {
            std::optional<std::size_t> selected;
            for (std::size_t role = 0; role < offers.size(); ++role)
            {
                if (!offers[role])
                {
                    continue;
                }
                if (offers[role]->remainingRequiredWork > m_ledger.balance() - MASTER_OPERATION_QUOTA_WORK)
                {
                    unavailable = MasterOperationStatus::FundingRefused;
                    continue;
                }
                if (!selected || MasterOperationRegistry::before(
                        m_operations.at(indices[role]), m_operations.at(indices[*selected])))
                {
                    selected = role;
                }
            }
            return selected;
        }

        bool parkUnfundableComparison(
            DeferredLeaf & ticket, const std::shared_ptr<MasterAssignmentEvaluation> & challenger)
        {
            auto* blocked = &ticket;
            if (challenger)
            {
                if (!chargeMasterWork(m_ledger, m_leaves.size() + 1, 1, RefinementWork::MasterExpansion))
                {
                    return stopOperation(MasterOperationStatus::FundingRefused);
                }
                const auto found = std::find_if(m_leaves.begin(), m_leaves.end(),
                    [&](const DeferredLeaf & leaf) { return leaf.evaluation == challenger; });
                if (found == m_leaves.end())
                {
                    return stopOperation(MasterOperationStatus::Invalid);
                }
                blocked = &*found;
            }
            blocked->schedulable = false;
            ++m_result.stats.fundingParkedTickets;
            completeTicket(ticket);
            refreshLeafBound(ticket);
            if (blocked->evaluation == m_refinementFocus)
            {
                m_refinementFocus.reset();
                m_focusRemaining = 0;
            }
            return true;
        }

        bool refineOperationLeaf(DeferredLeaf & ticket)
        {
            const auto ticketStart = m_ledger.stats().spent();
            const bool firstProofPhase = !m_result.improvement;
            const auto challenger = ticket.evaluation == m_incumbent
                ? m_refinementFocus : ticket.evaluation;
            RelativeStockComparison comparison;
            if (challenger)
            {
                if (!compare(challenger, &comparison))
                {
                    return stopOperation(MasterOperationStatus::FundingRefused);
                }
                if (!comparison.valid())
                {
                    return stopOperation(MasterOperationStatus::Invalid);
                }
                if (firstProofPhase && !comparison.unresolved())
                {
                    ++m_result.stats.resolvedOperationTickets;
                    completeTicket(ticket);
                    refreshLeafBound(ticket);
                    return true;
                }
            }
            const bool allocatePair = challenger && comparison.unresolved() && !m_result.improvement;
            constexpr std::int64_t descriptorWork = 32;
            if (!m_ledger.consume(descriptorWork, RefinementWork::MasterExpansion))
            {
                return stopOperation(MasterOperationStatus::FundingRefused);
            }
            std::array<MasterOperationDescriptor, MASTER_OPERATION_ROLE_LIMIT> roles{};
            std::size_t count = 0;
            if (allocatePair)
            {
                roles[count++] = {challenger, MasterRefinementTerm::Friendly, true, false};
                roles[count++] = {m_incumbent, MasterRefinementTerm::Friendly, false, true};
                if (!comparison.commonEnemyCancelled)
                {
                    roles[count++] = {m_incumbent, MasterRefinementTerm::Enemy, true, false};
                    roles[count++] = {challenger, MasterRefinementTerm::Enemy, false, true};
                }
                roles[count++] = {m_incumbent, MasterRefinementTerm::FutureDual, false, true};
            }
            else
            {
                if (ticket.next == MasterRefinementTerm::Enemy && challenger &&
                    comparison.unresolved() && comparison.commonEnemyCancelled)
                {
                    ++m_result.stats.cancelledEnemyRefinements;
                    completeTicket(ticket);
                    return true;
                }
                roles[count++] = {ticket.evaluation, ticket.next, true, true};
            }
            std::array<std::size_t, MASTER_OPERATION_ROLE_LIMIT> indices{};
            if (!m_operations.prepare(m_problem.identity(), std::span(roles).first(count), m_ledger,
                                      m_options.schedule.continuationSlice,
                                      m_options.schedule.futureDualAllowance, indices))
            {
                return stopOperation(m_ledger.stats().refusals != m_refusalsBefore
                    ? MasterOperationStatus::FundingRefused : MasterOperationStatus::Invalid);
            }
            m_result.stats.operationRecords = m_operations.size();
            std::array<std::optional<MasterOperationOffer>, MASTER_OPERATION_ROLE_LIMIT> offers{};
            MasterOperationStatus unavailable = MasterOperationStatus::Unavailable;
            if (!inspectOperations(std::span(roles).first(count), std::span(indices).first(count), offers, unavailable))
            {
                return false;
            }
            const auto selected = affordableOperation(
                std::span(offers).first(count), std::span(indices).first(count), unavailable);
            if (!selected)
            {
                if (unavailable == MasterOperationStatus::FundingRefused)
                {
                    return parkUnfundableComparison(ticket, allocatePair ? challenger : nullptr);
                }
                completeTicket(ticket);
                refreshLeafBound(ticket);
                if (allocatePair && unavailable == MasterOperationStatus::MissingPrerequisite)
                {
                    return stopOperation(unavailable);
                }
                return true;
            }
            return performOperation(ticket, challenger, roles[*selected], m_operations.at(indices[*selected]),
                                    *offers[*selected], ticketStart);
        }

        bool performOperation(DeferredLeaf & ticket, const std::shared_ptr<MasterAssignmentEvaluation> & challenger,
                              const MasterOperationDescriptor & descriptor, MasterOperationService & operation,
                              const MasterOperationOffer & selectedOffer, std::int64_t ticketStart)
        {
            operation.allowance = selectedOffer.allowance;
            const auto operationStart = m_ledger.stats().spent();
            if (m_options.observer != nullptr)
            {
                m_options.observer->operationStarted(
                    *descriptor.evaluation, descriptor.term, operation.allowance, operationStart);
            }
            const auto outcome = m_evaluator.executeOperation(*descriptor.evaluation, selectedOffer);
            if (m_options.observer != nullptr)
            {
                m_options.observer->operationExecuted(
                    *descriptor.evaluation, descriptor.term, outcome, operationStart, m_ledger.stats().spent());
                m_options.observer->refined(*descriptor.evaluation, descriptor.term, operation.allowance,
                    outcome.status, operationStart, m_ledger.stats().spent());
            }
            if (outcome.status == MasterRefinementStatus::BudgetExhausted)
            {
                creditOperation(operation, outcome.admitted ? outcome.executionWork : 0);
                return stopOperation(MasterOperationStatus::FundingRefused);
            }
            completeTicket(ticket);
            if (outcome.status == MasterRefinementStatus::Invalid)
            {
                creditOperation(operation, outcome.admitted ? outcome.executionWork : 0);
                return stopOperation(MasterOperationStatus::Invalid);
            }
            const auto beforeGrowth = m_ledger.stats().spent();
            if (!MasterOperationRegistry::grow(operation, m_ledger, m_options.schedule.refinementGrowthFactor,
                                                outcome.admitted, outcome.completed, true))
            {
                creditOperation(operation, outcome.admitted ? outcome.executionWork : 0);
                return stopOperation(MasterOperationStatus::FundingRefused);
            }
            const auto growthWork = m_ledger.stats().spent() - beforeGrowth;
            refreshLeafBound(ticket);
            consider(descriptor.evaluation);
            const bool compared = !challenger || compare(challenger);
            const auto serviceWork = outcome.status == MasterRefinementStatus::Unavailable ? 0 :
                m_ledger.stats().spent() - operationStart - growthWork;
            if (!creditOperation(operation, serviceWork))
            {
                return stopOperation(MasterOperationStatus::Invalid);
            }
            if (m_options.observer != nullptr)
            {
                m_options.observer->operationServiced({
                    *ticket.evaluation, challenger.get(), descriptor.evaluation.get(), descriptor.term,
                    selectedOffer.allowance, m_ledger.stats().spent() - ticketStart, serviceWork,
                    outcome.admitted, outcome.completed,
                    (descriptor.lowerRelevant && outcome.lowerProgress) ||
                        (descriptor.upperRelevant && outcome.upperProgress),
                    (!descriptor.lowerRelevant && outcome.lowerProgress) ||
                        (!descriptor.upperRelevant && outcome.upperProgress)});
            }
            return compared;
        }

        bool refineSelectedLeaf(DeferredLeaf & leaf)
        {
            if (!m_evaluator.refresh(*leaf.evaluation))
            {
                if (m_ledger.stats().refusals != m_refusalsBefore)
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                leaf.schedulable = false;
                recordEvaluation(*leaf.evaluation);
                return true;
            }
            auto & allowance = leaf.allowance();
            const bool cancelEnemy = leaf.next == MasterRefinementTerm::Enemy && enemyCancelsInFocus();
            if (m_ledger.stats().refusals != m_refusalsBefore)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            const auto workBefore = m_ledger.stats().spent();
            auto status = MasterRefinementStatus::Unavailable;
            if (cancelEnemy)
            {
                ++m_result.stats.cancelledEnemyRefinements;
            }
            else
            {
                status = m_evaluator.refine(*leaf.evaluation, leaf.next, allowance);
            }
            if (m_options.observer != nullptr)
            {
                m_options.observer->refined(*leaf.evaluation, leaf.next, allowance, status,
                                            workBefore, m_ledger.stats().spent());
            }
            if (status == MasterRefinementStatus::BudgetExhausted)
            {
                m_result.refinementRefusal = m_evaluator.refinementRefusal();
                m_result.budgetExhausted = true;
                return false;
            }
            ++m_result.stats.refinements;
            if (leaf.evaluation == m_refinementFocus && m_focusRemaining > 0)
            {
                --m_focusRemaining;
            }
            if (leaf.evaluation == m_incumbent)
            {
                ++m_result.stats.incumbentRefinements;
            }
            if (status == MasterRefinementStatus::Invalid)
            {
                ++m_result.stats.proofFailures;
                leaf.schedulable = false;
            }
            if (status == MasterRefinementStatus::Updated)
            {
                const auto growth = m_options.schedule.refinementGrowthFactor;
                if (allowance <= std::numeric_limits<std::int64_t>::max() / growth)
                {
                    allowance *= growth;
                }
                else
                {
                    leaf.schedulable = false;
                }
            }
            if (leaf.next == MasterRefinementTerm::Friendly)
            {
                leaf.next = MasterRefinementTerm::Enemy;
            }
            else if (leaf.next == MasterRefinementTerm::Enemy)
            {
                leaf.next = MasterRefinementTerm::FutureDual;
            }
            else
            {
                leaf.next = MasterRefinementTerm::Friendly;
            }
            if (leaf.evaluation->objective && leaf.evaluation->upperVerified)
            {
                leaf.upper = minimum(leaf.upper, leaf.evaluation->objective->upper);
            }
            leaf.schedulable = leaf.schedulable && !leaf.evaluation->exact;
            consider(leaf.evaluation);
            if (leaf.evaluation == m_incumbent && m_best && m_best != m_incumbent)
            {
                return compare(m_best);
            }
            return compare(leaf.evaluation);
        }

        void account(const MasterAssignmentState & state, std::optional<MilliFunds> upper, bool unsupported)
        {
            if (upper && m_result.lower && *upper <= *m_result.lower)
            {
                return;
            }
            ++m_result.stats.residualRegions;
            if (!upper)
            {
                m_unknownResidual = true;
            }
            else
            {
                m_result.residualUpper = m_result.residualUpper
                    ? std::max(*m_result.residualUpper, *upper) : upper;
            }
            if (m_options.retainDiagnostics)
            {
                m_result.residual.push_back({state, upper, state.complete(), unsupported});
            }
        }

    public:
        // Runs after the search has returned and only on its own ledger, so nothing it does can change the result.
        void auditNeighbors(MasterAssignmentSearchResult & result) const
        {
            const auto & schedule = m_options.schedule;
            if (schedule.planNeighborAudit != MasterPlanNeighborAudit::Recorded || schedule.planPricingRounds == 0)
            {
                return;
            }
            const auto started = std::chrono::steady_clock::now();
            auto & audit = result.stats.planPricing.neighborAudit.emplace();
            RefinementLedger scratch;
            audit.status = measureNeighbors(result, audit, scratch);
            if (audit.status != MasterPlanNeighborAuditStatus::Recorded)
            {
                audit.changes.clear();
                audit.provenNotBetter = 0;
                audit.open = 0;
                audit.conflicting = 0;
                audit.unavailable = 0;
            }
            audit.work = scratch.stats().spent();
            audit.nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count();
        }

    private:
        MasterPlanNeighborAuditStatus measureNeighbors(const MasterAssignmentSearchResult & result,
                                                       MasterPlanNeighborAuditStats & audit,
                                                       RefinementLedger & scratch) const
        {
            const auto & schedule = m_options.schedule;
            const auto* stock = m_evaluator.stockModel();
            const auto* catalog = m_evaluator.futureCatalog();
            if (stock == nullptr || catalog == nullptr || m_evaluator.hasGuardedStock() ||
                !m_bounds.covers(m_problem.identity(), stock, catalog) || !m_incumbent ||
                m_incumbent->status != MasterEvaluationStatus::Feasible || m_incumbent->quote() == nullptr)
            {
                return MasterPlanNeighborAuditStatus::Unavailable;
            }
            audit.plan = result.improvement ? MasterNeighborAuditedPlan::Challenger : MasterNeighborAuditedPlan::Incumbent;
            const auto & plan = result.improvement ? result.improvement->challenger : *m_incumbent;
            if (!scratch.open(schedule.planNeighborAuditAllowance))
            {
                return MasterPlanNeighborAuditStatus::Unaffordable;
            }
            switch (m_evaluator.oursBasis(plan, scratch))
            {
                case MasterOursBasisStatus::Proven:
                    break;
                case MasterOursBasisStatus::Unproven:
                    return MasterPlanNeighborAuditStatus::FloorUnproven;
                case MasterOursBasisStatus::CatalogMismatch:
                    return MasterPlanNeighborAuditStatus::CatalogMismatch;
                case MasterOursBasisStatus::OwnedMismatch:
                    return MasterPlanNeighborAuditStatus::OwnedMismatch;
                case MasterOursBasisStatus::BudgetExhausted:
                    return MasterPlanNeighborAuditStatus::Unaffordable;
            }
            const auto* quote = plan.quote();
            auto floor = plan.replay->seatedEconomicTotal;
            const auto adapterWork = masterPlanAdapterWork(m_problem, *catalog);
            if (!RelativeStockDetail::checkedAdd(floor, quote->owned) ||
                !RelativeStockDetail::checkedAdd(floor, quote->ours.lower) || !adapterWork)
            {
                return MasterPlanNeighborAuditStatus::ArithmeticFailure;
            }
            audit.trustedFloor = floor;
            if (scratch.balance() < *adapterWork)
            {
                return MasterPlanNeighborAuditStatus::Unaffordable;
            }
            const auto instance = buildMasterPlanPricingInstance(m_problem, *stock, *catalog, scratch);
            if (!instance)
            {
                return MasterPlanNeighborAuditStatus::Unavailable;
            }
            const auto forced = masterPlanStateChoices(m_problem, *instance, plan.state);
            if (!forced)
            {
                return MasterPlanNeighborAuditStatus::Unrepresentable;
            }
            const auto tuned = tuneRestricted(*instance, forced,
                                              schedule.planPricingWork / MASTER_PLAN_PRICING_CHALLENGER_SHARE,
                                              NEIGHBOR_AUDIT_TUNE_SHARE, scratch);
            switch (tuned.status)
            {
                case RestrictedTuneStatus::Overflow:
                    return MasterPlanNeighborAuditStatus::ArithmeticFailure;
                case RestrictedTuneStatus::Unaffordable:
                    return MasterPlanNeighborAuditStatus::Unaffordable;
                case RestrictedTuneStatus::RestrictionFailed:
                    return MasterPlanNeighborAuditStatus::TuneFailed;
                case RestrictedTuneStatus::Tuned:
                    break;
            }
            const auto & priced = tuned.priced;
            if (priced.prices.empty())
            {
                return MasterPlanNeighborAuditStatus::TuneFailed;
            }
            audit.rounds = priced.rounds;
            std::vector<MilliFunds> prices(m_problem.resources().size(), 0);
            prices.insert(prices.end(), priced.prices.begin(), priced.prices.end());
            const auto table = m_bounds.termsAt(plan.state, std::move(prices), scratch);
            if (table.status == MasterBoundStatus::BudgetExhausted)
            {
                return MasterPlanNeighborAuditStatus::Unaffordable;
            }
            if (table.status != MasterBoundStatus::Valid)
            {
                return MasterPlanNeighborAuditStatus::ScanInvalid;
            }
            audit.scanUpper = table.upper;
            if (priced.witness.valid)
            {
                // The witness books economic values and capture swings without the owned baseline; the floor's terms replace them.
                const auto friendly = masterPlanFriendlyWitness(priced.witness);
                auto witness = plan.replay->seatedEconomicTotal;
                if (!friendly || !RelativeStockDetail::checkedAdd(witness, quote->owned) ||
                    !RelativeStockDetail::checkedAdd(witness, friendly->value))
                {
                    return MasterPlanNeighborAuditStatus::ArithmeticFailure;
                }
                audit.witnessValue = witness;
                if (witness > table.upper)
                {
                    return MasterPlanNeighborAuditStatus::WitnessAboveCeiling;
                }
            }
            for (std::size_t slot = 0; slot < table.actorTerms.size(); ++slot)
            {
                const auto selected = plan.state.choices[slot];
                if (selected < 0 || !m_problem.canChangeActor(static_cast<std::int32_t>(slot)))
                {
                    continue;
                }
                const auto & terms = table.actorTerms[slot];
                const auto base = std::find_if(terms.begin(), terms.end(),
                    [&](const MasterCandidateTerm & entry) { return entry.candidate == selected; });
                if (base == terms.end() || !base->term)
                {
                    return MasterPlanNeighborAuditStatus::ScanInvalid;
                }
                for (const auto & entry : terms)
                {
                    if (entry.candidate == selected)
                    {
                        continue;
                    }
                    MasterNeighborChange change{m_problem.actors()[slot].engineUnitId,
                                                static_cast<std::int32_t>(slot), entry.candidate};
                    const auto legal = compatible(plan.state, slot, entry.candidate, scratch);
                    if (!legal)
                    {
                        return MasterPlanNeighborAuditStatus::Unaffordable;
                    }
                    change.conflictFree = *legal;
                    auto gain = entry.term.value_or(0);
                    auto ceiling = table.upper;
                    const bool measured = entry.term && RelativeStockDetail::checkedSubtract(gain, *base->term) &&
                                          RelativeStockDetail::checkedAdd(ceiling, gain);
                    if (measured)
                    {
                        change.gain = gain;
                    }
                    // A conflicting state is no real plan, so its ceiling bounds nothing until paired with another move.
                    change.verdict = !measured ? MasterNeighborVerdict::Unavailable
                        : !change.conflictFree ? MasterNeighborVerdict::Conflicting
                        : ceiling <= floor ? MasterNeighborVerdict::ProvenNotBetter : MasterNeighborVerdict::Open;
                    switch (change.verdict)
                    {
                        case MasterNeighborVerdict::ProvenNotBetter:
                            ++audit.provenNotBetter;
                            break;
                        case MasterNeighborVerdict::Open:
                            ++audit.open;
                            break;
                        case MasterNeighborVerdict::Conflicting:
                            ++audit.conflicting;
                            break;
                        case MasterNeighborVerdict::Unavailable:
                            ++audit.unavailable;
                            break;
                    }
                    audit.changes.push_back(change);
                }
            }
            return MasterPlanNeighborAuditStatus::Recorded;
        }

        MasterAssignmentSearchResult finish()
        {
            if (m_evaluator.oursCapContradictions() != 0)
            {
                dropCappedImprovement();
            }
            if (m_active)
            {
                account(m_active->state, m_active->upper, false);
            }
            while (!m_frontier.empty())
            {
                const auto node = m_frontier.top();
                account(node->state, node->upper, false);
                m_frontier.pop();
            }
            bool incumbentAccounted = false;
            for (const auto & leaf : m_leaves)
            {
                auto upper = leaf.upper;
                if (leaf.evaluation == m_incumbent)
                {
                    incumbentAccounted = true;
                    upper = minimum(upper, minimum(m_rootUpper, m_incumbentUpper));
                    if (m_incumbent->objective && m_incumbent->upperVerified)
                    {
                        upper = minimum(upper, m_incumbent->objective->upper);
                    }
                }
                account(leaf.evaluation->state, upper,
                        leaf.evaluation->status == MasterEvaluationStatus::Unsupported);
            }
            if (!incumbentAccounted && m_incumbent && m_incumbent->state.identity.sameLiveProblem(m_problem.identity()) &&
                m_incumbent->state.choices.size() == m_problem.actors().size() &&
                m_incumbent->status != MasterEvaluationStatus::Infeasible)
            {
                auto upper = minimum(m_rootUpper, m_incumbentUpper);
                if (m_incumbent->objective && m_incumbent->upperVerified)
                {
                    upper = minimum(upper, m_incumbent->objective->upper);
                }
                account(m_incumbent->state, upper, m_incumbent->status == MasterEvaluationStatus::Unsupported);
            }
            if (!m_unknownResidual)
            {
                m_result.upper = m_result.lower;
                if (m_result.residualUpper)
                {
                    m_result.upper = m_result.upper
                        ? std::max(*m_result.upper, *m_result.residualUpper) : m_result.residualUpper;
                }
            }
            else
            {
                m_result.residualUpper.reset();
            }
            if (m_result.lower && m_result.upper)
            {
                auto gap = *m_result.upper;
                if (RelativeStockDetail::checkedSubtract(gap, *m_result.lower))
                {
                    m_result.gap = gap;
                }
                m_result.globalOptimal = *m_result.lower == *m_result.upper;
            }
            m_result.best = m_best;
            m_result.incumbentPreserved = !m_best || m_best == m_incumbent;
            m_result.canonicalRepresentative = m_result.globalOptimal && m_result.incumbentPreserved;
            m_result.noFeasibleAssignment = !m_result.lower && !m_unknownResidual &&
                                             m_result.stats.residualRegions == 0 && !m_result.invalidInput;
            m_result.stats.work = m_ledger.stats();
            m_result.stats.futurePricingCache = m_bounds.futureCacheStats();
            m_result.stats.retainedGrants = m_ledger.retainedGrantCount();
            m_result.stats.refundableStorageBytes = m_ledger.refundableStorageBytes();
            return std::move(m_result);
        }

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentEvaluator & m_evaluator;
        RefinementLedger & m_ledger;
        const std::int64_t m_refusalsBefore;
        const std::int64_t m_workBefore;
        const MasterAssignmentSearchOptions & m_options;
        MasterAssignmentBounds m_bounds;
        MasterOperationRegistry m_operations;
        MasterAssignmentSearchResult m_result;
        std::shared_ptr<MasterAssignmentEvaluation> m_incumbent;
        std::shared_ptr<MasterAssignmentEvaluation> m_best;
        std::shared_ptr<MasterAssignmentEvaluation> m_refinementFocus;
        std::shared_ptr<MasterAssignmentEvaluation> m_planChallenger;
        std::optional<MasterImprovementProof> m_uncappedImprovement;
        FirstImprovement m_firstUncapped;
        std::shared_ptr<SearchNode> m_active;
        std::priority_queue<std::shared_ptr<SearchNode>, std::vector<std::shared_ptr<SearchNode>>, NodeOrder> m_frontier;
        std::vector<DeferredLeaf> m_leaves;
        std::map<std::vector<std::int32_t>, std::shared_ptr<MasterAssignmentEvaluation>> m_evaluations;
        std::vector<std::uint64_t> m_conflictDegree;
        std::optional<MilliFunds> m_rootUpper;
        std::optional<MilliFunds> m_incumbentUpper;
        std::uint64_t m_nodesSinceRefinement{0};
        std::uint64_t m_focusRemaining{0};
        bool m_unknownResidual{false};
    };
}

namespace Coordinator
{
    MasterAssignmentSearchResult searchMasterAssignment(
        const MasterAssignmentProblem & problem, MasterAssignmentEvaluator & evaluator,
        RefinementLedger & ledger, const MasterAssignmentSearchOptions & options)
    {
        Search search(problem, evaluator, ledger, options);
        auto result = search.run(evaluator.stockModel());
        search.auditNeighbors(result);
        return result;
    }
}
