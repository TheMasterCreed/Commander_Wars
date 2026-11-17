#include "ai/coordinator/masterplanpricing.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace Coordinator;

    constexpr std::int32_t STALL_ROUNDS = 20;
    constexpr std::int32_t WITNESS_INTERVAL = MASTER_PLAN_WITNESS_INTERVAL;
    constexpr std::int32_t MAX_POLISH_PASSES = MASTER_PLAN_POLISH_PASSES;
    constexpr double INITIAL_SCALE = 1.0;
    constexpr double SCALE_DECAY = 0.5;
    constexpr double PRICE_LIMIT = static_cast<double>(MASTER_PLAN_PRICE_LIMIT);

    enum class MeterStop : std::int8_t
    {
        None,
        BuildCap,
        Total,
    };

    // Checks the local limits before the ledger, so the caller's ledger never records a refusal.
    class WorkMeter final
    {
    public:
        WorkMeter(RefinementLedger & ledger, std::int64_t maximum)
            : m_ledger(ledger),
              m_remaining(maximum)
        {
        }

        bool charge(std::size_t count, RefinementWork category)
        {
            const auto work = masterWorkProduct(count, 1);
            if (!work)
            {
                m_stop = MeterStop::Total;
                return false;
            }
            return chargeWork(*work, category);
        }

        MasterFuturePricing priceFuture(const MasterFutureCatalog & catalog, std::span<const MilliFunds> prices)
        {
            RefinementLedger scratch;
            if (!scratch.open(available()))
            {
                m_stop = MeterStop::Total;
                return {MasterFutureStatus::BudgetExhausted};
            }
            auto pricing = priceMasterFuture(catalog, prices, scratch, RefinementWork::MasterDualOptimization);
            if (!chargeWork(scratch.stats().granted, RefinementWork::MasterDualOptimization))
            {
                pricing.status = MasterFutureStatus::BudgetExhausted;
            }
            else if (scratch.lastRefusal())
            {
                refuse(scratch.lastRefusal()->requested);
            }
            return pricing;
        }

        void startBuild(std::int64_t cap)
        {
            m_build = cap;
        }

        void endBuild()
        {
            m_build.reset();
            if (m_stop == MeterStop::BuildCap)
            {
                m_stop = MeterStop::None;
            }
        }

        bool stopped() const
        {
            return m_stop != MeterStop::None;
        }

        bool exhausted() const
        {
            return m_stop == MeterStop::Total;
        }

    private:
        std::int64_t available() const
        {
            return m_build ? std::min(m_remaining, *m_build) : m_remaining;
        }

        void refuse(std::int64_t work)
        {
            m_stop = work > m_remaining ? MeterStop::Total : MeterStop::BuildCap;
        }

        bool chargeWork(std::int64_t work, RefinementWork category)
        {
            if (work > available())
            {
                refuse(work);
                return false;
            }
            if (!m_ledger.consume(work, category))
            {
                m_stop = MeterStop::Total;
                return false;
            }
            m_remaining -= work;
            if (m_build)
            {
                *m_build -= work;
            }
            return true;
        }

        RefinementLedger & m_ledger;
        std::int64_t m_remaining{0};
        std::optional<std::int64_t> m_build;
        MeterStop m_stop{MeterStop::None};
    };

    bool sameRow(const SequentialRow & left, const SequentialRow & right)
    {
        return left.classIndex == right.classIndex && left.weight == right.weight && left.ownedTurn == right.ownedTurn;
    }

    void countNodes(std::vector<std::int32_t> & demand, const std::vector<std::int32_t> & nodes)
    {
        for (const auto node : nodes)
        {
            ++demand[static_cast<std::size_t>(node)];
        }
    }

    std::vector<std::int32_t> descending(std::span<const MilliFunds> values)
    {
        std::vector<std::int32_t> order(values.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](std::int32_t left, std::int32_t right) {
            return values[static_cast<std::size_t>(left)] > values[static_cast<std::size_t>(right)];
        });
        return order;
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    bool auditing(const MasterPlanWitnessOptions & options)
    {
        return options.auditSkips;
    }
#else
    constexpr bool auditing(const MasterPlanWitnessOptions &)
    {
        return false;
    }
#endif

    enum class ResourcePolicy : std::int8_t
    {
        Enforced,
        Ignored,
    };

    using Usage = std::map<std::int32_t, std::int64_t>;

    bool fits(const MasterPlanPricingCandidate & candidate, const Usage & usage, const std::vector<bool> & used,
              ResourcePolicy policy)
    {
        if (candidate.capturedNode != NO_SEQUENTIAL_NODE && used[static_cast<std::size_t>(candidate.capturedNode)])
        {
            return false;
        }
        if (policy == ResourcePolicy::Ignored)
        {
            return true;
        }
        // The witness does not yet order a follower behind its leader, so it never seats one.
        if (candidate.follows)
        {
            return false;
        }
        for (const auto & resource : candidate.resources)
        {
            std::int64_t demand = 0;
            for (const auto & other : candidate.resources)
            {
                if (other.resourceIndex == resource.resourceIndex)
                {
                    demand += other.demand;
                }
            }
            const auto found = usage.find(resource.resourceIndex);
            if ((found == usage.end() ? 0 : found->second) + demand > resource.capacity)
            {
                return false;
            }
        }
        return true;
    }

    MasterPlanPricingStatus unpricedStatus(const MasterFuturePricing & pricing, const WorkMeter & meter)
    {
        if (meter.stopped())
        {
            return MasterPlanPricingStatus::LimitReached;
        }
        return pricing.status == MasterFutureStatus::ArithmeticFailure ? MasterPlanPricingStatus::ArithmeticFailure
                                                                       : MasterPlanPricingStatus::Unavailable;
    }

    bool addIdleRows(const MasterPlanPricingModel & model, const MasterFuturePricing & pricing, MilliFunds & ceiling,
                     std::vector<std::int32_t> & nodeDemand)
    {
        for (std::size_t row = 0; row < model.rowActors.size(); ++row)
        {
            if (model.rowActors[row] != MASTER_PLAN_NO_ACTOR)
            {
                continue;
            }
            if (!RelativeStockDetail::checkedAdd(ceiling, pricing.dayRows[row].upper))
            {
                return false;
            }
            countNodes(nodeDemand, pricing.dayRows[row].nodes);
        }
        return true;
    }

    std::optional<std::vector<MilliFunds>> candidateValues(const MasterPlanPricingActor & actor,
                                                           std::span<const MilliFunds> prices,
                                                           const MasterFuturePricing & pricing)
    {
        std::vector<MilliFunds> values;
        for (const auto & candidate : actor.candidates)
        {
            const auto & future = pricing.actions[static_cast<std::size_t>(candidate.actionIndex)];
            const auto value =
                MasterPlanPricingDetail::masterPlanCandidateValue(candidate.immediateValue, candidate.capturedNode,
                                                                  future.upper, prices);
            if (!value)
            {
                return std::nullopt;
            }
            values.push_back(*value);
        }
        return values;
    }

    MasterPlanCeiling evaluateCeiling(const MasterPlanPricingModel & model, std::span<const MilliFunds> prices,
                                      WorkMeter & meter)
    {
        MasterPlanCeiling result;
        result.pricing = meter.priceFuture(model.catalog, prices);
        const auto & pricing = result.pricing;
        if (pricing.status != MasterFutureStatus::Valid)
        {
            result.status = unpricedStatus(pricing, meter);
            return result;
        }
        if (pricing.dayRows.size() != model.rowActors.size() || pricing.actions.size() != model.catalog.actions.size())
        {
            result.status = MasterPlanPricingStatus::Invalid;
            return result;
        }
        MilliFunds ceiling = 0;
        result.nodeDemand.assign(prices.size(), 0);
        if (!MasterPlanPricingDetail::addMasterPlanTerms(ceiling, prices) || !addIdleRows(model, pricing, ceiling, result.nodeDemand))
        {
            result.status = MasterPlanPricingStatus::ArithmeticFailure;
            return result;
        }
        for (const auto & actor : model.actors)
        {
            if (!meter.charge(actor.candidates.size(), RefinementWork::MasterDualOptimization))
            {
                result.status = MasterPlanPricingStatus::LimitReached;
                return result;
            }
            if (actor.candidates.empty())
            {
                result.status = MasterPlanPricingStatus::Invalid;
                return result;
            }
            const auto values = candidateValues(actor, prices, pricing);
            if (!values)
            {
                result.status = MasterPlanPricingStatus::ArithmeticFailure;
                return result;
            }
            const auto best =
                static_cast<std::size_t>(std::max_element(values->begin(), values->end()) - values->begin());
            const auto & chosen = actor.candidates[best];
            if (!RelativeStockDetail::checkedAdd(ceiling, (*values)[best]))
            {
                result.status = MasterPlanPricingStatus::ArithmeticFailure;
                return result;
            }
            countNodes(result.nodeDemand, pricing.actions[static_cast<std::size_t>(chosen.actionIndex)].nodes);
            if (chosen.capturedNode != NO_SEQUENTIAL_NODE)
            {
                ++result.nodeDemand[static_cast<std::size_t>(chosen.capturedNode)];
            }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            result.choicePositions.push_back(static_cast<std::int32_t>(best));
#endif
            result.actorMaxima.push_back((*values)[best]);
        }
        result.ceiling = ceiling;
        result.status = MasterPlanPricingStatus::Valid;
        return result;
    }

    struct Itinerary
    {
        std::vector<SequentialWitnessStep> steps;
        MilliFunds value{0};
        MilliFunds objective{0};
    };

    enum class SearchStatus : std::int8_t
    {
        Found,
        Dominated,
        Failed,
    };

    enum class TailSource : std::int8_t
    {
        PricedTable,
        Unpriced,
    };

    struct Branch
    {
        std::int32_t node{0};
        std::int32_t turn{NO_CAPTURE_TURNS};
        MilliFunds gain{0};
        MilliFunds bound{0};
    };

    bool chargeWitness(WorkMeter & meter, std::size_t units, std::int64_t & work)
    {
        if (!meter.charge(units, RefinementWork::MasterWitness))
        {
            return false;
        }
        work += static_cast<std::int64_t>(units);
        return true;
    }

    class ItinerarySearch final
    {
    public:
        ItinerarySearch(const MasterFutureCatalog & catalog, std::span<const MilliFunds> prices,
                        std::span<const MasterFuturePricedClass> pricedClasses, WorkMeter & meter,
                        MasterPlanSearchMode mode, TailSource source, MasterPlanWitnessStats & stats,
                        std::int64_t & searchWork)
            : m_catalog(catalog), m_prices(prices), m_pricedClasses(pricedClasses), m_meter(meter), m_mode(mode),
              m_source(source), m_stats(stats), m_searchWork(searchWork)
        {
        }

        // Ties keep the first maximum so pruned and exhaustive searches agree.
        SearchStatus best(const SequentialRow & row, std::vector<bool> & used, std::optional<MilliFunds> threshold)
        {
            m_tail = nullptr;
            if (pruning() && m_source == TailSource::PricedTable)
            {
                m_tail = pricedTail(row.classIndex);
                if (m_tail == nullptr)
                {
                    return SearchStatus::Failed;
                }
            }
            m_table = &m_catalog.classes[static_cast<std::size_t>(row.classIndex)];
            m_used = &used;
            m_failed = false;
            m_best = Itinerary{};
            m_found = !pruning() || !threshold || *threshold < 0;
            if (!m_found)
            {
                m_best.objective = *threshold;
            }
            std::vector<Branch> heads;
            for (std::size_t head = 0; head < used.size(); ++head)
            {
                if (!charge(1))
                {
                    return SearchStatus::Failed;
                }
                // Non-positive heads cannot beat the empty itinerary, so keeping them is harmless.
                if (!used[head] &&
                    !addBranch(heads, static_cast<std::int32_t>(head), row.ownedTurn[head], row.weight[head], 0))
                {
                    return SearchStatus::Failed;
                }
            }
            explore(heads, 0, 0);
            if (m_failed)
            {
                return SearchStatus::Failed;
            }
            return m_found ? SearchStatus::Found : SearchStatus::Dominated;
        }

        const Itinerary & result() const
        {
            return m_best;
        }

        MilliFunds nodePrice(std::int32_t node) const
        {
            return unpriced() ? 0 : m_prices[static_cast<std::size_t>(node)];
        }

        bool unpriced() const
        {
            return m_source == TailSource::Unpriced;
        }

    private:
        bool pruning() const
        {
            return m_mode == MasterPlanSearchMode::Pruned;
        }

        bool charge(std::size_t units)
        {
            return chargeWitness(m_meter, units, m_searchWork);
        }

        std::size_t cell(std::int32_t node, std::int32_t turn) const
        {
            return static_cast<std::size_t>(node) * static_cast<std::size_t>(m_catalog.horizon) +
                   static_cast<std::size_t>(turn);
        }

        MilliFunds tailAt(std::int32_t node, std::int32_t turn) const
        {
            if (turn == NO_CAPTURE_TURNS || !pruning())
            {
                return 0;
            }
            // The unpriced revisit-allowing tail over all nodes bounds every free set at zero prices.
            if (m_tail == nullptr)
            {
                return std::max<MilliFunds>(m_table->valueAt(node, turn), 0);
            }
            // The pick only removes nodes, so the ceiling's all-node table at the same prices stays an upper bound.
            return (*m_tail)[cell(node, turn)].value;
        }

        // Checked before the row's class is dereferenced, so a missing or misshapen table is never read.
        const MasterFuturePricedClass* pricedTail(std::int32_t classIndex) const
        {
            const auto index = static_cast<std::size_t>(classIndex);
            const auto cells = masterWorkProduct(m_catalog.columns.size(),
                                                 static_cast<std::size_t>(std::max(m_catalog.horizon, 0)));
            if (classIndex < 0 || index >= m_pricedClasses.size() || index >= m_catalog.classes.size() || !cells ||
                m_pricedClasses[index].size() != static_cast<std::size_t>(*cells))
            {
                return nullptr;
            }
            return &m_pricedClasses[index];
        }

        bool addBranch(std::vector<Branch> & branches, std::int32_t node, std::int32_t turn, MilliFunds gain,
                       MilliFunds objective)
        {
            auto bound = objective;
            if (!RelativeStockDetail::checkedAdd(bound, gain) ||
                !RelativeStockDetail::checkedSubtract(bound, nodePrice(node)) ||
                !RelativeStockDetail::checkedAdd(bound, tailAt(node, turn)))
            {
                return false;
            }
            branches.push_back({node, turn, gain, bound});
            return true;
        }

        bool open(MilliFunds bound) const
        {
            return !pruning() || bound > m_best.objective;
        }

        void explore(const std::vector<Branch> & branches, MilliFunds value, MilliFunds objective)
        {
            for (const auto & branch : branches)
            {
                if (open(branch.bound))
                {
                    visit(branch, value, objective);
                }
                if (m_failed)
                {
                    return;
                }
            }
        }

        void visit(const Branch & branch, MilliFunds value, MilliFunds objective)
        {
            ++m_stats.states;
            const auto node = branch.node;
            const auto turn = branch.turn;
            const auto slot = static_cast<std::size_t>(node);
            if (!RelativeStockDetail::checkedAdd(value, branch.gain) ||
                !RelativeStockDetail::checkedAdd(objective, branch.gain) ||
                !RelativeStockDetail::checkedSubtract(objective, nodePrice(node)))
            {
                m_failed = true;
                return;
            }
            (*m_used)[slot] = true;
            m_steps.push_back({node, turn, branch.gain});
            if (objective > m_best.objective)
            {
                if (!charge(m_steps.size()))
                {
                    m_failed = true;
                    return;
                }
                m_best = {m_steps, value, objective};
                m_found = true;
            }
            if (turn != NO_CAPTURE_TURNS && open(branch.bound))
            {
                std::vector<Branch> children;
                for (std::int32_t next = 0; next < m_table->nodeCount() && !m_failed; ++next)
                {
                    if (!charge(1))
                    {
                        m_failed = true;
                        break;
                    }
                    if ((*m_used)[static_cast<std::size_t>(next)])
                    {
                        continue;
                    }
                    const auto nextTurn = m_table->continuationOwnedTurn(node, turn, next);
                    if (nextTurn != NO_CAPTURE_TURNS &&
                        !addBranch(children, next, nextTurn,
                                   columnStreamValue(m_catalog.columns[static_cast<std::size_t>(next)], nextTurn,
                                                     m_catalog.horizon), objective))
                    {
                        m_failed = true;
                    }
                }
                if (!m_failed)
                {
                    explore(children, value, objective);
                }
            }
            m_steps.pop_back();
            (*m_used)[slot] = false;
        }

        const MasterFutureCatalog & m_catalog;
        std::span<const MilliFunds> m_prices;
        std::span<const MasterFuturePricedClass> m_pricedClasses;
        WorkMeter & m_meter;
        MasterPlanSearchMode m_mode;
        TailSource m_source;
        MasterPlanWitnessStats & m_stats;
        std::int64_t & m_searchWork;
        const MasterFuturePricedClass* m_tail{nullptr};
        const SequentialClassTable* m_table{nullptr};
        std::vector<bool>* m_used{nullptr};
        std::vector<SequentialWitnessStep> m_steps;
        Itinerary m_best;
        bool m_found{false};
        bool m_failed{false};
    };

    struct Seat
    {
        std::int32_t row{0};
        std::int32_t slot{MASTER_PLAN_NO_ACTOR};
        std::vector<const MasterPlanPricingCandidate*> options;
        const MasterPlanPricingCandidate* chosen{nullptr};
        Itinerary itinerary;
        std::optional<std::int64_t> settledAt;
    };

    struct Choice
    {
        const MasterPlanPricingCandidate* candidate{nullptr};
        Itinerary itinerary;
        MilliFunds score{0};
        bool laterDay{false};
    };

    struct Occupancy
    {
        Usage usage;
        std::vector<bool> used;

        void occupy(const Seat & seat)
        {
            adjust(seat, 1);
        }

        void release(const Seat & seat)
        {
            adjust(seat, -1);
        }

        void adjust(const Seat & seat, std::int64_t direction)
        {
            const auto occupied = direction > 0;
            for (const auto & resource : seat.chosen->resources)
            {
                usage[resource.resourceIndex] += direction * resource.demand;
            }
            if (seat.chosen->capturedNode != NO_SEQUENTIAL_NODE)
            {
                used[static_cast<std::size_t>(seat.chosen->capturedNode)] = occupied;
            }
            for (const auto & step : seat.itinerary.steps)
            {
                used[static_cast<std::size_t>(step.node)] = occupied;
            }
        }
    };

    struct ChoiceContext
    {
        WorkMeter & meter;
        MasterPlanWitnessStats & stats;
        ResourcePolicy policy;
        MasterPlanSearchMode mode;
    };

    const SequentialWitnessStep* headOf(const Itinerary & itinerary)
    {
        return itinerary.steps.empty() ? nullptr : &itinerary.steps.front();
    }

    // Raw movement only: ownership time also folds in capture rate and carried points.
    bool closerToHead(const SequentialRow & row, const Choice & running)
    {
        const auto* pHead = headOf(running.itinerary);
        if (pHead == nullptr)
        {
            return false;
        }
        const auto head = static_cast<std::size_t>(pHead->node);
        const auto & held = running.candidate->row;
        if (head >= row.approach.size() || head >= held.approach.size() || head >= row.ownedTurn.size() ||
            row.ownedTurn[head] == NO_CAPTURE_TURNS)
        {
            return false;
        }
        const auto & approach = row.approach[head];
        return approach.reachable() && held.approach[head].reachable() && approach < held.approach[head];
    }

    bool sameHead(const Itinerary & left, const Itinerary & right)
    {
        const auto* pLeft = headOf(left);
        const auto* pRight = headOf(right);
        return pLeft != nullptr && pRight != nullptr && pLeft->node == pRight->node;
    }

    // A key holds only for the claims of the scan that read it.
    std::optional<SequentialLaterKey> readLaterKey(const SequentialRow & row, const Occupancy & occupancy,
                                                   const ChoiceContext & context)
    {
        const auto read = sequentialLaterKey(row.later, occupancy.used, [&] {
            if (!context.meter.charge(1, RefinementWork::MasterWitness))
            {
                return false;
            }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            ++context.stats.laterEntries;
#endif
            return true;
        });
        if (!read)
        {
            return std::nullopt;
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (read->state == SequentialLaterKeyState::FirstPositive)
        {
            context.stats.laterDeepestRank = std::max(context.stats.laterDeepestRank, read->examined);
        }
#endif
        return read;
    }

    struct LaterDayScan
    {
        const Choice* pHeld{nullptr};
        std::optional<bool> heldFits;
        std::optional<SequentialLaterKey> runningKey;
    };

    // Decides for both the threshold and the replacement; empty means the work ran out.
    std::optional<bool> laterDayBetter(const MasterPlanPricingCandidate & candidate, MilliFunds offset,
                                       const Choice & running, const Occupancy & occupancy,
                                       const ChoiceContext & context, LaterDayScan & scan,
                                       std::optional<SequentialLaterKey> & candidateKey)
    {
        auto gap = running.score;
        if (&candidate == running.candidate || !running.itinerary.steps.empty() ||
            candidate.capturedNode != running.candidate->capturedNode ||
            !RelativeStockDetail::checkedSubtract(gap, offset) || gap != 0 ||
            candidate.row.later.state != SequentialLaterState::Complete ||
            running.candidate->row.later.state != SequentialLaterState::Complete)
        {
            return false;
        }
        if (&running == scan.pHeld && !scan.heldFits)
        {
            const auto resources = running.candidate->resources.size();
            if (!context.meter.charge(resources * resources + 1, RefinementWork::MasterWitness))
            {
                return std::nullopt;
            }
            scan.heldFits = fits(*running.candidate, occupancy.usage, occupancy.used, context.policy);
        }
        if (&running == scan.pHeld && !*scan.heldFits)
        {
            return false;
        }
        if (!scan.runningKey)
        {
            scan.runningKey = readLaterKey(running.candidate->row, occupancy, context);
            if (!scan.runningKey)
            {
                return std::nullopt;
            }
        }
        candidateKey = readLaterKey(candidate.row, occupancy, context);
        if (!candidateKey)
        {
            return std::nullopt;
        }
        return sequentialLaterKeyBetter(*candidateKey, *scan.runningKey);
    }

    // Found means the held choice, if any, was replaced: by a higher score, or an equal one closer or positive sooner.
    SearchStatus bestChoice(const Seat & seat, Occupancy & occupancy, ItinerarySearch & search,
                            const ChoiceContext & context, const Choice* pHeld, Choice & result)
    {
        ++context.stats.responses;
        auto status = SearchStatus::Dominated;
        std::optional<MilliFunds> unseededBest;
        LaterDayScan later{pHeld};
        for (const auto * candidate : seat.options)
        {
            const auto resources = candidate->resources.size();
            if (!context.meter.charge(resources * resources + 1, RefinementWork::MasterWitness))
            {
                return SearchStatus::Failed;
            }
            if (!fits(*candidate, occupancy.usage, occupancy.used, context.policy))
            {
                continue;
            }
            const auto captured = static_cast<std::size_t>(candidate->capturedNode);
            const auto capturing = candidate->capturedNode != NO_SEQUENTIAL_NODE;
            auto offset = candidate->immediateValue;
            if (capturing && !RelativeStockDetail::checkedSubtract(offset, search.nodePrice(candidate->capturedNode)))
            {
                return SearchStatus::Failed;
            }
            const Choice* pRunning = status == SearchStatus::Found ? &result : pHeld;
            const auto closer = pRunning != nullptr && closerToHead(candidate->row, *pRunning);
            std::optional<SequentialLaterKey> candidateKey;
            auto laterDay = false;
            // A priced empty itinerary proves nothing about today's unpriced optimum, so picking never reads keys.
            if (search.unpriced() && pRunning != nullptr)
            {
                const auto decided =
                    laterDayBetter(*candidate, offset, *pRunning, occupancy, context, later, candidateKey);
                if (!decided)
                {
                    return SearchStatus::Failed;
                }
                laterDay = *decided;
            }
            // Exhaustive mode ignores the threshold but keeps the overflow failures of its unseeded scan.
            auto threshold = context.mode == MasterPlanSearchMode::Pruned && pRunning != nullptr ?
                std::optional<MilliFunds>(pRunning->score) : unseededBest;
            if (threshold)
            {
                if (!RelativeStockDetail::checkedSubtract(*threshold, offset))
                {
                    return SearchStatus::Failed;
                }
                // The search only reports a strict excess, so an equal closer itinerary needs one less.
                if ((closer && *threshold >= 0) || (laterDay && context.mode == MasterPlanSearchMode::Pruned))
                {
                    --*threshold;
                }
            }
            if (capturing)
            {
                occupancy.used[captured] = true;
            }
            const auto searched = search.best(candidate->row, occupancy.used, threshold);
            if (capturing)
            {
                occupancy.used[captured] = false;
            }
            if (searched == SearchStatus::Failed)
            {
                return SearchStatus::Failed;
            }
            if (searched == SearchStatus::Dominated)
            {
                continue;
            }
            auto score = offset;
            if (!RelativeStockDetail::checkedAdd(score, search.result().objective))
            {
                return SearchStatus::Failed;
            }
            if (!unseededBest || score > *unseededBest)
            {
                unseededBest = score;
            }
            const auto tie = pRunning != nullptr && score == pRunning->score;
            const auto laterSwitch = tie && laterDay && search.result().steps.empty();
            if (pRunning == nullptr || score > pRunning->score ||
                (tie && closer && sameHead(search.result(), pRunning->itinerary)) || laterSwitch)
            {
                later.runningKey = candidateKey;
                result = Choice{candidate, search.result(), score, laterSwitch};
                status = SearchStatus::Found;
            }
        }
        return status;
    }

    enum class PolishOutcome : std::int8_t
    {
        Failed,
        Unchanged,
        Improved,
    };

    PolishOutcome polish(Seat & seat, Occupancy & occupancy, ItinerarySearch & search, const ChoiceContext & context)
    {
        occupancy.release(seat);
        auto current = seat.chosen->immediateValue;
        if (!RelativeStockDetail::checkedAdd(current, seat.itinerary.value))
        {
            return PolishOutcome::Failed;
        }
        const Choice held{seat.chosen, seat.itinerary, current};
        Choice best;
        const auto status = bestChoice(seat, occupancy, search, context, &held, best);
        if (status == SearchStatus::Failed)
        {
            return PolishOutcome::Failed;
        }
        const auto improved = status == SearchStatus::Found;
        if (improved)
        {
            if (best.score == current)
            {
                ++(best.laterDay ? context.stats.laterSwitches : context.stats.tieSwitches);
            }
            seat.chosen = best.candidate;
            seat.itinerary = std::move(best.itinerary);
        }
        occupancy.occupy(seat);
        return improved ? PolishOutcome::Improved : PolishOutcome::Unchanged;
    }

    std::vector<MasterPlanPricingCandidate> idleCandidates(const MasterPlanPricingModel & model)
    {
        std::vector<MasterPlanPricingCandidate> result;
        for (std::size_t row = 0; row < model.rowActors.size(); ++row)
        {
            if (model.rowActors[row] == MASTER_PLAN_NO_ACTOR)
            {
                MasterPlanPricingCandidate candidate;
                candidate.row = model.catalog.dayRows[row];
                result.push_back(std::move(candidate));
            }
        }
        return result;
    }

    std::vector<const MasterPlanPricingCandidate*> candidateOptions(const MasterPlanPricingActor & actor)
    {
        std::vector<const MasterPlanPricingCandidate*> options;
        for (const auto & candidate : actor.candidates)
        {
            options.push_back(&candidate);
        }
        return options;
    }

    std::vector<Seat> buildSeats(const MasterPlanPricingModel & model, const MasterPlanCeiling & priced,
                                 const std::vector<MasterPlanPricingCandidate> & idle)
    {
        std::vector<Seat> seats;
        for (const auto slot : descending(priced.actorMaxima))
        {
            const auto & actor = model.actors[static_cast<std::size_t>(slot)];
            seats.push_back(Seat{actor.row, slot, candidateOptions(actor), nullptr, {}});
        }
        auto spare = idle.begin();
        for (std::size_t row = 0; row < model.rowActors.size(); ++row)
        {
            if (model.rowActors[row] == MASTER_PLAN_NO_ACTOR)
            {
                seats.push_back(Seat{static_cast<std::int32_t>(row), MASTER_PLAN_NO_ACTOR, {&*spare++}, nullptr, {}});
            }
        }
        return seats;
    }

    bool pickSeats(std::vector<Seat> & seats, Occupancy & occupancy, ItinerarySearch & search,
                   const ChoiceContext & context)
    {
        for (auto & seat : seats)
        {
            Choice choice;
            if (bestChoice(seat, occupancy, search, context, nullptr, choice) != SearchStatus::Found)
            {
                return false;
            }
            seat.chosen = choice.candidate;
            seat.itinerary = std::move(choice.itinerary);
            occupancy.occupy(seat);
        }
        return true;
    }

    bool polishSeats(std::vector<Seat> & seats, Occupancy & occupancy, ItinerarySearch & search,
                     const ChoiceContext & context, const MasterPlanWitnessOptions & options)
    {
        // Bumped on every switch; a seat settled at the current version would only re-find its own value.
        std::int64_t version = 0;
        auto changed = true;
        for (std::int32_t pass = 0; changed && pass < MAX_POLISH_PASSES; ++pass)
        {
            changed = false;
            for (auto & seat : seats)
            {
                const auto skipped = options.mode == MasterPlanSearchMode::Pruned && seat.settledAt == version;
                if (skipped)
                {
                    ++context.stats.skippedResponses;
                    if (!auditing(options))
                    {
                        continue;
                    }
                }
                const auto outcome = polish(seat, occupancy, search, context);
                if (outcome == PolishOutcome::Failed)
                {
                    return false;
                }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
                if (skipped && outcome == PolishOutcome::Improved)
                {
                    ++context.stats.skipViolations;
                }
#endif
                if (outcome == PolishOutcome::Improved)
                {
                    ++version;
                    changed = true;
                }
                seat.settledAt = version;
            }
        }
        return true;
    }

    MasterPlanWitness collectWitness(const MasterPlanPricingModel & model, const std::vector<Seat> & seats)
    {
        MasterPlanWitness result;
        result.rows.resize(model.rowActors.size());
        result.candidateIndices.assign(model.actors.size(), NO_CANDIDATE);
        MilliFunds value = 0;
        for (const auto & seat : seats)
        {
            auto & row = result.rows[static_cast<std::size_t>(seat.row)];
            row.row = seat.row;
            row.steps = seat.itinerary.steps;
            for (const auto & step : row.steps)
            {
                if (!RelativeStockDetail::checkedAdd(row.value, step.gain))
                {
                    return result;
                }
            }
            if (!RelativeStockDetail::checkedAdd(value, seat.chosen->immediateValue) ||
                !RelativeStockDetail::checkedAdd(value, row.value))
            {
                return result;
            }
            if (seat.slot != MASTER_PLAN_NO_ACTOR)
            {
                result.candidateIndices[static_cast<std::size_t>(seat.slot)] = seat.chosen->candidateIndex;
            }
        }
        result.value = value;
        result.valid = true;
        return result;
    }

    MasterPlanWitness assembleWitness(const MasterPlanPricingModel & model, std::span<const MilliFunds> prices,
                                      const MasterPlanCeiling & priced, ResourcePolicy policy,
                                      MasterPlanWitnessOptions options, WorkMeter & meter,
                                      MasterPlanWitnessStats & stats)
    {
        const auto & catalog = model.catalog;
        if (priced.status != MasterPlanPricingStatus::Valid || prices.size() != catalog.columns.size())
        {
            return {};
        }
        const auto idle = idleCandidates(model);
        auto seats = buildSeats(model, priced, idle);
        Occupancy occupancy{masterPlanOccupiedUsage(model), std::vector<bool>(catalog.columns.size(), false)};
        const ChoiceContext context{meter, stats, policy, options.mode};
        ItinerarySearch reduced(catalog, prices, priced.pricing.classes, meter, options.mode, TailSource::PricedTable,
                                stats, stats.pickSearchWork);
        if (!pickSeats(seats, occupancy, reduced, context))
        {
            return {};
        }
        ItinerarySearch exact(catalog, {}, {}, meter, options.mode, TailSource::Unpriced, stats,
                              stats.polishSearchWork);
        if (!polishSeats(seats, occupancy, exact, context, options))
        {
            return {};
        }
        return collectWitness(model, seats);
    }

    MasterPlanWitness buildWitness(const MasterPlanPricingModel & model, std::span<const MilliFunds> prices,
                                   const MasterPlanCeiling & priced, ResourcePolicy policy,
                                   MasterPlanWitnessOptions options, std::int64_t cap, WorkMeter & meter,
                                   MasterPlanWitnessStats & stats)
    {
        ++stats.builds;
        meter.startBuild(cap);
        auto result = assembleWitness(model, prices, priced, policy, options, meter, stats);
        if (meter.stopped())
        {
            ++stats.abandonedBuilds;
            result = {};
        }
        meter.endBuild();
        return result;
    }

    bool sumSteps(std::span<const SequentialWitnessStep> steps, MilliFunds & value)
    {
        return std::all_of(steps.begin(), steps.end(),
            [&](const SequentialWitnessStep & step) { return RelativeStockDetail::checkedAdd(value, step.gain); });
    }

    bool claimSlot(std::vector<bool> & used, std::int32_t node)
    {
        if (node < 0 || static_cast<std::size_t>(node) >= used.size() || used[static_cast<std::size_t>(node)])
        {
            return false;
        }
        used[static_cast<std::size_t>(node)] = true;
        return true;
    }

    std::optional<std::vector<Seat>> seatsFromWitness(const MasterPlanPricingModel & model,
                                                      const MasterPlanWitness & start,
                                                      const std::vector<MasterPlanPricingCandidate> & idle)
    {
        const auto rows = model.rowActors.size();
        std::vector<Seat> seats(rows);
        auto spare = idle.begin();
        for (std::size_t row = 0; row < rows; ++row)
        {
            auto & seat = seats[row];
            seat.row = static_cast<std::int32_t>(row);
            seat.slot = model.rowActors[row];
            if (seat.slot == MASTER_PLAN_NO_ACTOR)
            {
                seat.options = {&*spare++};
                seat.chosen = seat.options.front();
            }
            else
            {
                const auto & actor = model.actors[static_cast<std::size_t>(seat.slot)];
                seat.options = candidateOptions(actor);
                const auto index = start.candidateIndices[static_cast<std::size_t>(seat.slot)];
                seat.chosen = findMasterPlanCandidate(actor, index);
            }
            const auto & source = start.rows[row];
            // Held choices are kept without passing fits, so a supplied follower must be refused here.
            if (seat.chosen == nullptr || seat.chosen->follows || source.row != seat.row ||
                !sumSteps(source.steps, seat.itinerary.value))
            {
                return std::nullopt;
            }
            seat.itinerary.steps = source.steps;
            seat.itinerary.objective = seat.itinerary.value;
        }
        return seats;
    }

    // Captured nodes are claimed before any chain, so a chain through a captured node is rejected too.
    bool disjointSeats(const std::vector<Seat> & seats, std::size_t nodes)
    {
        std::vector<bool> used(nodes, false);
        const auto capturesFit = std::all_of(seats.begin(), seats.end(), [&](const Seat & seat) {
            return seat.chosen->capturedNode == NO_SEQUENTIAL_NODE || claimSlot(used, seat.chosen->capturedNode);
        });
        return capturesFit && std::all_of(seats.begin(), seats.end(), [&](const Seat & seat) {
            return std::all_of(seat.itinerary.steps.begin(), seat.itinerary.steps.end(),
                               [&](const SequentialWitnessStep & step) { return claimSlot(used, step.node); });
        });
    }

    enum class PriceStep : std::int8_t
    {
        Moved,
        Converged,
        LimitReached,
    };

    class PriceTuner final
    {
    public:
        PriceTuner(const MasterPlanPricingModel & model, const MasterPlanPricingLimits & limits,
                   RefinementLedger & ledger, MasterPlanWitnessOptions options, std::span<const MilliFunds> seed)
            : m_model(model),
              m_limits(limits),
              m_options(options),
              m_meter(ledger, limits.maximumWork),
#ifdef COW_COORDINATED_REFINEMENT_TESTING
              m_ledger(ledger),
              m_pricingStart(granted(RefinementWork::MasterDualOptimization)),
              m_witnessStart(granted(RefinementWork::MasterWitness)),
#endif
              m_prices(model.catalog.columns.size(), 0)
        {
            std::copy(seed.begin(), seed.end(), m_prices.begin());
        }

        MasterPlanPricingResult run()
        {
            auto scale = INITIAL_SCALE;
            std::int32_t stall = 0;
            for (std::int32_t round = 0; round < m_limits.rounds; ++round)
            {
                const auto priced = evaluateCeiling(m_model, m_prices, m_meter);
                if (priced.status != MasterPlanPricingStatus::Valid)
                {
                    return finish(priced.status);
                }
                ++m_result.rounds;
                if (!m_bestCeiling || priced.ceiling < *m_bestCeiling)
                {
                    m_bestCeiling = priced.ceiling;
                    m_result.prices = m_prices;
                    m_bestDemand = priced.nodeDemand;
                    stall = 0;
                }
                else
                {
                    ++stall;
                }
                if (round % WITNESS_INTERVAL == 0)
                {
                    consider(m_prices, priced);
                }
                if (m_meter.exhausted())
                {
                    return finish(MasterPlanPricingStatus::LimitReached);
                }
                if (onTarget())
                {
                    m_result.stoppedOnTarget = true;
                    break;
                }
                auto ceiling = priced.ceiling;
                const auto * demand = &priced.nodeDemand;
                if (stall >= STALL_ROUNDS)
                {
                    scale *= SCALE_DECAY;
                    m_prices = m_result.prices;
                    ceiling = *m_bestCeiling;
                    demand = &m_bestDemand;
                    stall = 0;
                }
                const auto stepped = step(ceiling, *demand, scale);
                if (stepped == PriceStep::LimitReached)
                {
                    return finish(MasterPlanPricingStatus::LimitReached);
                }
                if (stepped == PriceStep::Converged)
                {
                    break;
                }
            }
            const auto priced = evaluateCeiling(m_model, m_result.prices, m_meter);
            if (priced.status != MasterPlanPricingStatus::Valid)
            {
                return finish(priced.status);
            }
            if (priced.ceiling != *m_bestCeiling)
            {
                return finish(MasterPlanPricingStatus::Inconsistent);
            }
            consider(m_result.prices, priced);
            return finish(m_meter.exhausted() ? MasterPlanPricingStatus::LimitReached : MasterPlanPricingStatus::Valid);
        }

    private:
        // A gap below zero would mean a floor above the ceiling, which never counts as reaching the target.
        bool onTarget() const
        {
            auto floor = m_floor;
            if (m_limits.knownFloor && (!floor || *m_limits.knownFloor > *floor))
            {
                floor = m_limits.knownFloor;
            }
            auto gap = *m_bestCeiling;
            return floor && RelativeStockDetail::checkedSubtract(gap, *floor) && gap >= 0 && gap <= m_limits.targetGap;
        }

        void consider(std::span<const MilliFunds> at, const MasterPlanCeiling & priced)
        {
            const auto witness = buildWitness(m_model, at, priced, ResourcePolicy::Enforced, m_options,
                                              m_limits.witnessBuildWork, m_meter, m_stats);
            if (witness.valid && (!m_floor || witness.value > *m_floor))
            {
                m_floor = witness.value;
                m_result.witness = witness;
            }
            if (!m_floor && !m_meter.exhausted())
            {
                const auto relaxed = buildWitness(m_model, at, priced, ResourcePolicy::Ignored, m_options,
                                                  m_limits.witnessBuildWork, m_meter, m_stats);
                if (relaxed.valid && (!m_relaxedTarget || relaxed.value > *m_relaxedTarget))
                {
                    m_relaxedTarget = relaxed.value;
                }
            }
        }

        PriceStep step(MilliFunds ceiling, const std::vector<std::int32_t> & demand, double scale)
        {
            if (!m_meter.charge(demand.size() + m_prices.size(), RefinementWork::MasterDualOptimization))
            {
                return PriceStep::LimitReached;
            }
            std::int64_t norm = 0;
            for (const auto count : demand)
            {
                const std::int64_t gradient = 1 - count;
                norm += gradient * gradient;
            }
            if (norm == 0)
            {
                return PriceStep::Converged;
            }
            const auto target = m_floor ? m_floor : m_relaxedTarget;
            const auto gap = static_cast<double>(ceiling) - static_cast<double>(target.value_or(0));
            const auto size = scale * gap / static_cast<double>(norm);
            for (std::size_t node = 0; node < m_prices.size(); ++node)
            {
                const auto next = static_cast<double>(m_prices[node]) -
                    size * static_cast<double>(1 - demand[node]);
                m_prices[node] = static_cast<MilliFunds>(std::llround(std::clamp(next, 0.0, PRICE_LIMIT)));
            }
            return PriceStep::Moved;
        }

        MasterPlanPricingResult finish(MasterPlanPricingStatus status)
        {
#ifdef COW_COORDINATED_REFINEMENT_TESTING
            m_result.pricingWork = granted(RefinementWork::MasterDualOptimization) - m_pricingStart;
            m_result.witnessWork = granted(RefinementWork::MasterWitness) - m_witnessStart;
            m_result.ceiling = m_bestCeiling.value_or(0);
            m_result.floor = m_floor;
            m_result.witnessStats = m_stats;
#endif
            m_result.status = status == MasterPlanPricingStatus::LimitReached && !m_bestCeiling
                ? MasterPlanPricingStatus::Unavailable : status;
            return m_result;
        }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::int64_t granted(RefinementWork work) const
        {
            return m_ledger.stats().grantedByWork[static_cast<std::size_t>(work)];
        }
#endif

        const MasterPlanPricingModel & m_model;
        const MasterPlanPricingLimits & m_limits;
        MasterPlanWitnessOptions m_options;
        WorkMeter m_meter;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        const RefinementLedger & m_ledger;
        std::int64_t m_pricingStart{0};
        std::int64_t m_witnessStart{0};
#endif
        std::vector<MilliFunds> m_prices;
        MasterPlanPricingResult m_result;
        MasterPlanWitnessStats m_stats;
        std::optional<MilliFunds> m_floor;
        std::optional<MilliFunds> m_bestCeiling;
        std::vector<std::int32_t> m_bestDemand;
        std::optional<MilliFunds> m_relaxedTarget;
    };
}

namespace Coordinator
{
    // A node captured this turn leaves the live instance, so the capturer must not earn it again.
    void removeCapturedNode(SequentialRow & row, std::int32_t node)
    {
        row.weight[static_cast<std::size_t>(node)] = 0;
        row.ownedTurn[static_cast<std::size_t>(node)] = NO_CAPTURE_TURNS;
        if (!row.approach.empty())
        {
            row.approach[static_cast<std::size_t>(node)] = {};
        }
        clearLaterNode(row.later, node);
    }

    bool MasterPlanPricingDetail::addMasterPlanTerms(MilliFunds & total, std::span<const MilliFunds> terms)
    {
        return std::all_of(terms.begin(), terms.end(),
            [&](MilliFunds term) { return RelativeStockDetail::checkedAdd(total, term); });
    }

    std::optional<MilliFunds> MasterPlanPricingDetail::masterPlanCandidateValue(
        MilliFunds immediateValue, std::int32_t capturedNode, MilliFunds futureUpper, std::span<const MilliFunds> prices)
    {
        auto value = immediateValue;
        if ((capturedNode != NO_SEQUENTIAL_NODE &&
             !RelativeStockDetail::checkedSubtract(value, prices[static_cast<std::size_t>(capturedNode)])) ||
            !RelativeStockDetail::checkedAdd(value, futureUpper))
        {
            return std::nullopt;
        }
        return value;
    }

    std::map<std::int32_t, std::int64_t> masterPlanOccupiedUsage(const MasterPlanPricingModel & model)
    {
        std::map<std::int32_t, std::int64_t> result;
        for (const auto & use : model.occupiedResources)
        {
            result[use.resourceIndex] += use.demand;
        }
        return result;
    }

    bool indexMasterPlanActions(MasterPlanPricingModel & model)
    {
        std::vector<MasterFutureAction> actions;
        for (const auto & actor : model.actors)
        {
            for (const auto & candidate : actor.candidates)
            {
                actions.push_back({candidate.key, candidate.row});
            }
        }
        std::stable_sort(actions.begin(), actions.end(),
            [](const auto & left, const auto & right) { return left.key < right.key; });
        std::vector<MasterFutureAction> unique;
        for (auto & action : actions)
        {
            if (!unique.empty() && unique.back().key == action.key)
            {
                if (!sameRow(unique.back().row, action.row))
                {
                    return false;
                }
                continue;
            }
            unique.push_back(std::move(action));
        }
        model.catalog.actions = std::move(unique);
        for (auto & actor : model.actors)
        {
            for (auto & candidate : actor.candidates)
            {
                const auto* found = findMasterFutureAction(model.catalog, candidate.key);
                if (found == nullptr)
                {
                    return false;
                }
                candidate.actionIndex = static_cast<std::int32_t>(found - model.catalog.actions.data());
            }
        }
        return true;
    }

    std::optional<MasterPlanPricingModel> restrictMasterPlanModel(const MasterPlanPricingModel & model,
                                                                  std::span<const std::int32_t> forced)
    {
        if (forced.size() != model.actors.size())
        {
            return std::nullopt;
        }
        auto result = model;
        for (std::size_t slot = 0; slot < forced.size(); ++slot)
        {
            if (forced[slot] == MASTER_PLAN_UNRESTRICTED)
            {
                continue;
            }
            auto & candidates = result.actors[slot].candidates;
            std::erase_if(candidates,
                [&](const MasterPlanPricingCandidate & candidate) { return candidate.candidateIndex != forced[slot]; });
            if (candidates.empty())
            {
                return std::nullopt;
            }
        }
        if (!indexMasterPlanActions(result))
        {
            return std::nullopt;
        }
        return result;
    }

    const MasterPlanPricingCandidate* findMasterPlanCandidate(const MasterPlanPricingActor & actor,
                                                             std::int32_t candidateIndex)
    {
        const auto found = std::find_if(actor.candidates.begin(), actor.candidates.end(),
            [&](const MasterPlanPricingCandidate & candidate) { return candidate.candidateIndex == candidateIndex; });
        return found == actor.candidates.end() ? nullptr : &*found;
    }

    MasterPlanCeiling priceMasterPlan(const MasterPlanPricingModel & model, std::span<const MilliFunds> prices,
                                      RefinementLedger & ledger)
    {
        WorkMeter meter(ledger, ledger.balance());
        return evaluateCeiling(model, prices, meter);
    }

    MasterPlanPolishResult polishMasterPlanWitness(const MasterPlanPricingModel & model,
                                                   const MasterPlanWitness & start,
                                                   std::span<const std::int32_t> rowOrder, std::int64_t maximumWork,
                                                   RefinementLedger & ledger, MasterPlanWitnessOptions options)
    {
        MasterPlanPolishResult result;
        result.status = MasterPlanPricingStatus::Invalid;
        const auto & catalog = model.catalog;
        const auto rows = model.rowActors.size();
        if (start.rows.size() != rows || start.candidateIndices.size() != model.actors.size() ||
            rowOrder.size() != rows || maximumWork <= 0 || ledger.balance() < maximumWork)
        {
            return result;
        }
        std::vector<bool> ordered(rows, false);
        if (!std::all_of(rowOrder.begin(), rowOrder.end(), [&](std::int32_t row) { return claimSlot(ordered, row); }))
        {
            return result;
        }
        const auto idle = idleCandidates(model);
        const auto byRow = seatsFromWitness(model, start, idle);
        if (!byRow || !disjointSeats(*byRow, catalog.columns.size()))
        {
            return result;
        }
        std::vector<Seat> seats;
        Occupancy occupancy{masterPlanOccupiedUsage(model), std::vector<bool>(catalog.columns.size(), false)};
        for (const auto row : rowOrder)
        {
            seats.push_back((*byRow)[static_cast<std::size_t>(row)]);
            occupancy.occupy(seats.back());
        }
        MasterPlanWitnessStats stats;
        WorkMeter meter(ledger, maximumWork);
        const ChoiceContext context{meter, stats, ResourcePolicy::Enforced, options.mode};
        ItinerarySearch exact(catalog, {}, {}, meter, options.mode, TailSource::Unpriced, stats,
                              stats.polishSearchWork);
        if (!polishSeats(seats, occupancy, exact, context, options) || meter.stopped())
        {
            result.status = meter.stopped() ? MasterPlanPricingStatus::LimitReached : MasterPlanPricingStatus::Invalid;
            return result;
        }
        result.witness = collectWitness(model, seats);
        result.status =
            result.witness.valid ? MasterPlanPricingStatus::Valid : MasterPlanPricingStatus::ArithmeticFailure;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        result.witnessStats = stats;
#endif
        return result;
    }

    MasterPlanPricingResult tuneMasterPlanPrices(const MasterPlanPricingModel & model,
                                                 const MasterPlanPricingLimits & limits,
                                                 RefinementLedger & ledger, MasterPlanWitnessOptions options,
                                                 std::span<const MilliFunds> seed)
    {
        const auto outOfRange = [](MilliFunds price) { return price < 0 || price > MASTER_PLAN_PRICE_LIMIT; };
        if (limits.targetGap < 0 || (!seed.empty() && (seed.size() != model.catalog.columns.size() ||
                                                       std::any_of(seed.begin(), seed.end(), outOfRange))))
        {
            MasterPlanPricingResult refused;
            refused.status = MasterPlanPricingStatus::Invalid;
            return refused;
        }
        if (limits.rounds <= 0 || limits.maximumWork <= 0 || limits.witnessBuildWork <= 0 ||
            ledger.balance() < limits.maximumWork)
        {
            return {};
        }
        return PriceTuner(model, limits, ledger, options, seed).run();
    }
}
