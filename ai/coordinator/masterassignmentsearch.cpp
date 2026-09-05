#include "ai/coordinator/masterassignmentsearch.h"

#include <bit>
#include <limits>
#include <map>
#include <queue>
#include <tuple>

namespace
{
    using namespace Coordinator;

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
        std::int64_t slice{0};
        std::int64_t dualSlice{0};
        bool schedulable{true};
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
            while (!m_frontier.empty() || hasRefinement())
            {
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
                m_options.schedule.futureDualAllowance <= 0 || m_options.schedule.nodesPerRefinement == 0 ||
                m_options.schedule.incumbentRefinementStride == 0 ||
                m_options.schedule.challengerRefinementBurst == 0 ||
                (m_options.schedule.futureBounds != MasterFutureBoundScope::RootOnly &&
                 m_options.schedule.futureBounds != MasterFutureBoundScope::EveryNode) ||
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
            if (m_incumbent->status == MasterEvaluationStatus::BudgetExhausted ||
                m_ledger.stats().refusals != 0)
            {
                m_result.budgetExhausted = true;
                return false;
            }
            if (m_incumbent->status == MasterEvaluationStatus::Feasible && m_incumbent->quote() &&
                !m_incumbent->exact && !defer(m_incumbent, std::nullopt))
            {
                return false;
            }
            const auto prepared = m_bounds.prepare(stock, m_ledger, m_evaluator.futureCatalog());
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

        std::optional<bool> compatible(const MasterAssignmentState & state,
                                       std::size_t actor, std::int32_t choice)
        {
            const auto & candidate = m_problem.actors()[actor].candidates[static_cast<std::size_t>(choice)];
            for (std::size_t other = 0; other < state.choices.size(); ++other)
            {
                if (!chargeMasterWork(m_ledger, 1, 1, RefinementWork::MasterExpansion))
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
                if (!chargeMasterWork(m_ledger, candidate.resources.size() + 1,
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
                    const auto legal = compatible(node.state, actor, selected);
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
                    const auto legal = compatible(node.state, actor, choice);
                    if (!legal)
                    {
                        return NodeStatus::Interrupted;
                    }
                    if (*legal)
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
            if (m_bounds.ready() && !node.state.complete())
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

        bool compare(std::shared_ptr<MasterAssignmentEvaluation> evaluation)
        {
            if (evaluation == m_incumbent || !m_incumbent ||
                m_incumbent->status != MasterEvaluationStatus::Feasible ||
                evaluation->status != MasterEvaluationStatus::Feasible)
            {
                return true;
            }
            const auto comparison = m_evaluator.compare(*m_incumbent, *evaluation);
            if (m_refinementFocus == evaluation && comparison.valid() && !comparison.unresolved())
            {
                m_refinementFocus.reset();
                m_focusRemaining = 0;
            }
            if (m_options.observer != nullptr)
            {
                m_options.observer->compared(*evaluation, comparison, m_ledger.stats().spent());
            }
            consider(m_incumbent);
            consider(evaluation);
            if (comparison.improves() && (!m_result.improvement ||
                comparison.deltaLower > m_result.improvement->comparison.deltaLower))
            {
                if (!chargeMasterWork(m_ledger, m_problem.actors().size() + 1,
                                      m_problem.actors().size() + 1, RefinementWork::MasterWitness))
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                m_result.improvement = MasterImprovementProof{
                    *m_incumbent, *evaluation, comparison, m_ledger.stats().spent()};
                if (!m_result.stats.firstImprovementWork)
                {
                    m_result.stats.firstImprovementWork = m_ledger.stats().spent();
                }
            }
            if (m_ledger.stats().refusals != 0)
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
                              m_options.schedule.continuationSlice, m_options.schedule.futureDualAllowance};
            leaf.schedulable = evaluation->status == MasterEvaluationStatus::Feasible &&
                               evaluation->quote() != nullptr && !evaluation->exact;
            m_leaves.push_back(std::move(leaf));
            recordResidentRegions();
            return true;
        }

        bool hasRefinement()
        {
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

        bool enemyCancelsInFocus() const
        {
            if (!m_incumbent || !m_refinementFocus ||
                !m_incumbent->quote() || !m_refinementFocus->quote())
            {
                return false;
            }
            const auto & incumbent = *m_incumbent->quote();
            const auto & challenger = *m_refinementFocus->quote();
            return incumbent.identity.sameLiveModel(challenger.identity) &&
                incumbent.capturedColumns == challenger.capturedColumns;
        }

        bool refineLeaf()
        {
            if (!chargeMasterWork(m_ledger, m_leaves.size() + 1, m_problem.actors().size() + 1,
                                  RefinementWork::MasterExpansion))
            {
                m_result.budgetExhausted = true;
                return false;
            }
            auto* selected = selectRefinement();
            if (selected == nullptr)
            {
                return true;
            }
            auto & leaf = *selected;
            if (!m_evaluator.refresh(*leaf.evaluation))
            {
                if (m_ledger.stats().refusals != 0)
                {
                    m_result.budgetExhausted = true;
                    return false;
                }
                leaf.schedulable = false;
                recordEvaluation(*leaf.evaluation);
                return true;
            }
            const auto allowance = leaf.next == MasterRefinementTerm::FutureDual ? leaf.dualSlice : leaf.slice;
            const auto workBefore = m_ledger.stats().spent();
            auto status = MasterRefinementStatus::Unavailable;
            if (leaf.next == MasterRefinementTerm::Enemy && enemyCancelsInFocus())
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
            if (leaf.next == MasterRefinementTerm::Friendly)
            {
                leaf.next = MasterRefinementTerm::Enemy;
            }
            else if (leaf.next == MasterRefinementTerm::Enemy)
            {
                leaf.next = MasterRefinementTerm::FutureDual;
            }
            else if (leaf.slice <= std::numeric_limits<std::int64_t>::max() / 2 &&
                     leaf.dualSlice <= std::numeric_limits<std::int64_t>::max() / 2)
            {
                leaf.next = MasterRefinementTerm::Friendly;
                leaf.slice *= 2;
                leaf.dualSlice *= 2;
            }
            else
            {
                leaf.schedulable = false;
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

        MasterAssignmentSearchResult finish()
        {
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
        const MasterAssignmentSearchOptions & m_options;
        MasterAssignmentBounds m_bounds;
        MasterAssignmentSearchResult m_result;
        std::shared_ptr<MasterAssignmentEvaluation> m_incumbent;
        std::shared_ptr<MasterAssignmentEvaluation> m_best;
        std::shared_ptr<MasterAssignmentEvaluation> m_refinementFocus;
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
        return Search(problem, evaluator, ledger, options).run(evaluator.stockModel());
    }
}
