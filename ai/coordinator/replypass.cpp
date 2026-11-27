#include "ai/coordinator/replypass.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>

namespace Coordinator
{
    namespace
    {
        class StageMeter final
        {
        public:
            StageMeter(ReplyPassCost & cost, const ReplyStockContext & context)
                : m_cost(cost), m_context(context), m_started(std::chrono::steady_clock::now()),
                  m_workBefore(context.work())
            {
            }

            ~StageMeter()
            {
                m_cost.nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - m_started).count();
                m_cost.work += m_context.work() - m_workBefore;
                ++m_cost.calls;
            }

            StageMeter(const StageMeter &) = delete;
            StageMeter & operator=(const StageMeter &) = delete;

        private:
            ReplyPassCost & m_cost;
            const ReplyStockContext & m_context;
            std::chrono::steady_clock::time_point m_started;
            std::int64_t m_workBefore;
        };

        std::optional<MilliFunds> checkedStockSum(MilliFunds exactNonStock, MilliFunds owned, MilliFunds stock)
        {
            MilliFunds sum = exactNonStock;
            if (!RelativeStockDetail::checkedAdd(sum, owned) || !RelativeStockDetail::checkedAdd(sum, stock))
            {
                return std::nullopt;
            }
            return sum;
        }

        std::optional<ReplyPassInterval> checkedInterval(MilliFunds exactNonStock, MilliFunds owned, MilliFunds floor,
                                                         MilliFunds ceiling)
        {
            const auto lower = checkedStockSum(exactNonStock, owned, floor);
            const auto upper = checkedStockSum(exactNonStock, owned, ceiling);
            if (!lower || !upper)
            {
                return std::nullopt;
            }
            return ReplyPassInterval{*lower, *upper};
        }

        ReplyPassStockEvidence evidenceOf(std::span<const MilliFunds> prices, MilliFunds ceiling,
                                          const ReplyStockFloor & floor, const ReplyPassInterval & interval)
        {
            return ReplyPassStockEvidence{
                .present = true,
                .prices = std::vector<MilliFunds>(prices.begin(), prices.end()),
                .ceiling = ceiling,
                .floor = floor.floor,
                .witness = floor.witness,
                .interval = interval,
            };
        }

        ReplyPassPlan planOf(std::span<const std::int32_t> choices, const ReplyPassEvaluation & evaluation,
                             MilliFunds owned)
        {
            return ReplyPassPlan{
                .choices = std::vector<std::int32_t>(choices.begin(), choices.end()),
                .executionOrder = evaluation.replay.ordering.order,
                .actors = evaluation.actors,
                .afterOwnActions = evaluation.afterOwnActions,
                .afterReply = evaluation.afterReply,
                .exactNonStock = evaluation.exactNonStock,
                .owned = owned,
            };
        }

        bool validLimits(const ReplyPassLimits & limits)
        {
            return limits.warmupRounds > 0 && limits.baselineRounds > 0 && limits.retuneRounds > 0 &&
                   limits.retuneChanges >= 0 && limits.switchLimit > 0 &&
                   replyStockTargetGap(0, limits.targetGapPpm).has_value();
        }

        MilliFunds replyLossOf(const ReplyPassEvaluation & evaluation, std::int32_t actorSlot)
        {
            const auto found = std::find_if(evaluation.actors.begin(), evaluation.actors.end(),
                                            [&](const ReplyPassActor & actor) { return actor.actorSlot == actorSlot; });
            return found == evaluation.actors.end() ? 0 : found->terms.replyCapitalLoss;
        }

        // The certificate of the plan in effect, against which every change of the round is judged.
        struct Incumbent
        {
            ReplyPassInterval interval;
            ReplyStockFloor floor;
            std::vector<MilliFunds> prices;
            std::uint64_t priceVersion{0};
            MilliFunds targetGap{0};
        };

        struct Candidate
        {
            std::vector<std::int32_t> choices;
            ReplyPassEvaluation evaluation;
            MilliFunds owned{0};
            MilliFunds upper{0};
            MilliFunds ceiling{0};
            ReplyStockFloor floor;
            MilliFunds gain{0};
        };

        struct Proof
        {
            std::size_t change{0};
            Candidate candidate;
            ReplyPassStockEvidence evidence;
            MilliFunds margin{0};
        };

        enum class RoundEnd : std::int8_t
        {
            Switched,
            NoProof,
            PricesLost,
        };

        class ReplyPassRun final
        {
        public:
            ReplyPassRun(const MasterAssignmentProblem & problem, const ReplyBoard & board,
                         const PropertyStockField & field, const ReplyPassLimits & limits, ReplyPassResult & result)
                : m_problem(problem), m_board(board), m_field(field), m_limits(limits), m_result(result)
            {
            }

            void run(std::span<const std::int32_t> choices)
            {
                std::vector<std::int32_t> working(choices.begin(), choices.end());
                ReplyPassEvaluation current = evaluateReplyPassPlan(m_problem, working, m_board);
                m_result.incumbentNoValue = current.reason;
                m_result.end = current.valued() ? ReplyPassEnd::NotRun : ReplyPassEnd::IncumbentNoValue;
                std::optional<ReplyStockFloor> reference;
                for (std::int32_t round = 0; m_result.end == ReplyPassEnd::NotRun; ++round)
                {
                    m_result.end = runRound(round, working, current, reference);
                }
                m_result.finalChoices = std::move(working);
            }

        private:
            ReplyPassEnd runRound(std::int32_t round, std::vector<std::int32_t> & working,
                                  ReplyPassEvaluation & current, std::optional<ReplyStockFloor> & reference)
            {
                if (current.shots.empty())
                {
                    return ReplyPassEnd::NoContact;
                }
                std::vector<ReplyPassChange> changes =
                    ReplyPassDetail::listChanges(m_problem, working, current, round, m_result.counters);
                if (changes.empty())
                {
                    return ReplyPassEnd::NoEligibleChange;
                }
                if (!m_context)
                {
                    m_context.emplace(m_field);
                    m_result.stockContextBuilt = true;
                }
                const bool first = m_result.plans.empty();
                ReplyPassPlan opening = first ? planOf(working, current, 0) : ReplyPassPlan{};
                const std::optional<Incumbent> incumbent =
                    baseline(current, reference ? &*reference : nullptr, first ? opening : m_result.plans.back());
                if (!incumbent)
                {
                    return ReplyPassEnd::BaselineFailed;
                }
                if (first)
                {
                    m_result.plans.push_back(std::move(opening));
                }
                std::optional<Proof> proof;
                const RoundEnd ended = tryChanges(working, *incumbent, changes, proof);
                m_result.changes.insert(m_result.changes.end(), changes.begin(), changes.end());
                if (ended != RoundEnd::Switched)
                {
                    return ended == RoundEnd::PricesLost ? ReplyPassEnd::BaselineFailed : ReplyPassEnd::NoProvenChange;
                }
                accept(*incumbent, changes[proof->change], std::move(*proof), working, current, reference);
                if (static_cast<std::int32_t>(m_result.switches.size()) < m_limits.switchLimit)
                {
                    return ReplyPassEnd::NotRun;
                }
                m_result.counters.endedByLimit = true;
                return ReplyPassEnd::SwitchLimit;
            }

            ReplyPassCost & cost(ReplyPassStage stage)
            {
                return m_result.costs[static_cast<std::size_t>(stage)];
            }

            std::optional<Incumbent> baseline(const ReplyPassEvaluation & current, const ReplyStockFloor * reference,
                                              ReplyPassPlan & plan)
            {
                const StageMeter meter(cost(ReplyPassStage::Baseline), *m_context);
                const ContinuationKey & key = current.afterReply;
                std::optional<ReplyStockFloor> warm;
                if (!m_warmed)
                {
                    ReplyStockTune warmed = m_context->tune(key, m_limits.warmupRounds, ReplyStockTuneStart::Zero);
                    m_result.baselineStatus = warmed.status;
                    if (warmed.status != ReplyStockStatus::Valid)
                    {
                        return std::nullopt;
                    }
                    m_warmed = true;
                    warm = std::move(warmed.floor);
                    reference = reference == nullptr ? &*warm : reference;
                }
                const ReplyStockCeiling quick = m_context->ceiling(key);
                m_result.baselineStatus = quick.status;
                if (quick.status != ReplyStockStatus::Valid)
                {
                    return std::nullopt;
                }
                const MilliFunds targetGap = *replyStockTargetGap(quick.ceiling, m_limits.targetGapPpm);
                const ReplyStockTune tuned = m_context->tune(
                    key, m_limits.baselineRounds, ReplyStockTuneStart::InstalledPrices, reference, targetGap);
                const ReplyStockOwned owned = tuned.status == ReplyStockStatus::Valid ? m_context->owned(key)
                                                                                     : ReplyStockOwned{tuned.status};
                m_result.baselineStatus = owned.status;
                if (owned.status != ReplyStockStatus::Valid)
                {
                    return std::nullopt;
                }
                const auto interval =
                    checkedInterval(current.exactNonStock, owned.owned, tuned.floor.floor, tuned.ceiling);
                if (!interval)
                {
                    m_result.baselineStatus = ReplyStockStatus::ArithmeticFailure;
                    return std::nullopt;
                }
                plan.owned = owned.owned;
                plan.baseline = evidenceOf(m_context->installedPrices(), tuned.ceiling, tuned.floor, *interval);
                return Incumbent{*interval, tuned.floor, plan.baseline.prices, m_context->priceVersion(), targetGap};
            }

            void refuseInterval(ReplyPassChange & change, ReplyStockStatus status)
            {
                change.outcome = ReplyPassOutcome::NoInterval;
                change.stockStatus = status == ReplyStockStatus::Valid ? ReplyStockStatus::ArithmeticFailure : status;
                ++m_result.counters.noInterval;
            }

            bool value(ReplyPassChange & change, Candidate & candidate)
            {
                const StageMeter meter(cost(ReplyPassStage::Value), *m_context);
                change.tier = ReplyPassTier::Value;
                candidate.evaluation = evaluateReplyPassPlan(m_problem, candidate.choices, m_board);
                change.noValue = candidate.evaluation.reason;
                if (!candidate.evaluation.valued())
                {
                    change.outcome = ReplyPassOutcome::NoValue;
                    ++m_result.counters.noValue[static_cast<std::size_t>(candidate.evaluation.reason)];
                    return false;
                }
                const ReplyStockOwned owned = m_context->owned(candidate.evaluation.afterReply);
                if (owned.status != ReplyStockStatus::Valid)
                {
                    refuseInterval(change, owned.status);
                    return false;
                }
                candidate.owned = owned.owned;
                return true;
            }

            bool ceiling(ReplyPassChange & change, Candidate & candidate, const Incumbent & incumbent)
            {
                const StageMeter meter(cost(ReplyPassStage::Ceiling), *m_context);
                change.tier = ReplyPassTier::Ceiling;
                const ReplyStockCeiling ceiling = m_context->ceiling(candidate.evaluation.afterReply);
                const auto upper = ceiling.status == ReplyStockStatus::Valid ?
                    checkedStockSum(candidate.evaluation.exactNonStock, candidate.owned, ceiling.ceiling) :
                    std::nullopt;
                if (!upper)
                {
                    refuseInterval(change, ceiling.status);
                    return false;
                }
                candidate.ceiling = ceiling.ceiling;
                candidate.upper = *upper;
                change.upper = *upper;
                if (*upper <= incumbent.interval.lower)
                {
                    change.outcome = ReplyPassOutcome::ProvenNotBetter;
                    ++m_result.counters.provenNotBetter;
                    return false;
                }
                if (*upper <= incumbent.interval.upper)
                {
                    change.outcome = ReplyPassOutcome::CannotClear;
                    ++m_result.counters.cannotClear;
                    return false;
                }
                return true;
            }

            std::optional<MilliFunds> marginOver(const Incumbent & incumbent, MilliFunds lower) const
            {
                MilliFunds margin = lower;
                if (lower <= incumbent.interval.upper ||
                    !RelativeStockDetail::checkedSubtract(margin, incumbent.interval.upper))
                {
                    return std::nullopt;
                }
                return margin;
            }

            void switchTo(ReplyPassChange & change, ReplyPassTier tier, std::size_t index, Candidate & candidate,
                          ReplyPassStockEvidence evidence, MilliFunds margin, std::optional<Proof> & proof)
            {
                change.tier = tier;
                change.outcome = ReplyPassOutcome::Switched;
                ++(tier == ReplyPassTier::Floor ? m_result.counters.switchesAtFloor
                                                : m_result.counters.switchesAtRetune);
                proof = Proof{index, std::move(candidate), std::move(evidence), margin};
            }

            bool floor(ReplyPassChange & change, std::size_t index, Candidate & candidate, const Incumbent & incumbent,
                       std::optional<Proof> & proof)
            {
                const StageMeter meter(cost(ReplyPassStage::Floor), *m_context);
                change.tier = ReplyPassTier::Floor;
                ++m_result.counters.floorsBuilt;
                candidate.floor = m_context->floor(candidate.evaluation.afterReply, &incumbent.floor);
                const auto lower = candidate.floor.status == ReplyStockStatus::Valid ?
                    checkedStockSum(candidate.evaluation.exactNonStock, candidate.owned, candidate.floor.floor) :
                    std::nullopt;
                if (!lower)
                {
                    refuseInterval(change, candidate.floor.status);
                    return false;
                }
                change.lower = *lower;
                if (const auto margin = marginOver(incumbent, *lower))
                {
                    ReplyPassStockEvidence evidence = evidenceOf(incumbent.prices, candidate.ceiling, candidate.floor,
                                                                 ReplyPassInterval{*lower, candidate.upper});
                    switchTo(change, ReplyPassTier::Floor, index, candidate, std::move(evidence), *margin, proof);
                    return false;
                }
                candidate.gain = candidate.upper;
                if (!RelativeStockDetail::checkedSubtract(candidate.gain, incumbent.interval.upper))
                {
                    refuseInterval(change, ReplyStockStatus::ArithmeticFailure);
                    return false;
                }
                change.outcome = ReplyPassOutcome::Undecided;
                ++m_result.counters.undecided;
                return true;
            }

            void retune(ReplyPassChange & change, std::size_t index, Candidate & candidate, const Incumbent & incumbent,
                        std::optional<Proof> & proof)
            {
                const StageMeter meter(cost(ReplyPassStage::Retune), *m_context);
                ++m_result.counters.retunes;
                change.tier = ReplyPassTier::Retune;
                const ReplyStockTune tuned =
                    m_context->tune(candidate.evaluation.afterReply, m_limits.retuneRounds,
                                    ReplyStockTuneStart::InstalledPrices, &candidate.floor, incumbent.targetGap);
                const auto interval = tuned.status == ReplyStockStatus::Valid ?
                    checkedInterval(candidate.evaluation.exactNonStock, candidate.owned, tuned.floor.floor,
                                    tuned.ceiling) :
                    std::nullopt;
                change.stockStatus = tuned.status;
                if (!interval)
                {
                    change.stockStatus =
                        tuned.status == ReplyStockStatus::Valid ? ReplyStockStatus::ArithmeticFailure : tuned.status;
                    return;
                }
                change.lower = interval->lower;
                change.upper = interval->upper;
                if (const auto margin = marginOver(incumbent, interval->lower))
                {
                    ReplyPassStockEvidence evidence =
                        evidenceOf(m_context->installedPrices(), tuned.ceiling, tuned.floor, *interval);
                    candidate.floor = tuned.floor;
                    switchTo(change, ReplyPassTier::Retune, index, candidate, std::move(evidence), *margin, proof);
                }
            }

            RoundEnd tryChanges(std::span<const std::int32_t> working, const Incumbent & incumbent,
                                std::vector<ReplyPassChange> & changes, std::optional<Proof> & proof)
            {
                std::vector<std::pair<std::size_t, Candidate>> undecided;
                for (std::size_t index = 0; index < changes.size() && !proof; ++index)
                {
                    ReplyPassChange & change = changes[index];
                    Candidate candidate;
                    candidate.choices.assign(working.begin(), working.end());
                    candidate.choices[static_cast<std::size_t>(change.actorSlot)] = change.candidateIndex;
                    if (value(change, candidate) && ceiling(change, candidate, incumbent) &&
                        floor(change, index, candidate, incumbent, proof))
                    {
                        undecided.emplace_back(index, std::move(candidate));
                    }
                }
                if (proof)
                {
                    return RoundEnd::Switched;
                }
                std::stable_sort(undecided.begin(), undecided.end(), [](const auto & left, const auto & right)
                {
                    return left.second.gain > right.second.gain;
                });
                const std::size_t retunes =
                    std::min(undecided.size(), static_cast<std::size_t>(m_limits.retuneChanges));
                for (std::size_t rank = 0; rank < retunes; ++rank)
                {
                    auto & [index, candidate] = undecided[rank];
                    retune(changes[index], index, candidate, incumbent, proof);
                    if (proof)
                    {
                        return RoundEnd::Switched;
                    }
                    if (m_context->priceVersion() != incumbent.priceVersion &&
                        m_context->installPrices(incumbent.prices).status != ReplyStockStatus::Valid)
                    {
                        return RoundEnd::PricesLost;
                    }
                }
                return RoundEnd::NoProof;
            }

            void accept(const Incumbent & incumbent, const ReplyPassChange & change, Proof proof,
                        std::vector<std::int32_t> & working, ReplyPassEvaluation & current,
                        std::optional<ReplyStockFloor> & reference)
            {
                const std::size_t from = m_result.plans.size() - 1;
                Candidate & candidate = proof.candidate;
                m_result.switches.push_back(ReplyPassSwitch{
                    .from = from,
                    .to = from + 1,
                    .actorSlot = change.actorSlot,
                    .candidateIndex = change.candidateIndex,
                    .reason = change.reason,
                    .tier = change.tier,
                    .incumbentUpper = incumbent.interval.upper,
                    .changedLower = proof.evidence.interval.lower,
                    .margin = proof.margin,
                    .proof = std::move(proof.evidence),
                });
                m_result.plans.push_back(planOf(candidate.choices, candidate.evaluation, candidate.owned));
                working = std::move(candidate.choices);
                current = std::move(candidate.evaluation);
                reference = std::move(candidate.floor);
            }

            const MasterAssignmentProblem & m_problem;
            const ReplyBoard & m_board;
            const PropertyStockField & m_field;
            const ReplyPassLimits & m_limits;
            ReplyPassResult & m_result;
            std::optional<ReplyStockContext> m_context;
            bool m_warmed{false};
        };

        ReplyPassMismatch checkEvidence(ReplyStockContext & context, const ReplyPassPlan & plan,
                                        const ReplyPassStockEvidence & evidence)
        {
            if (!evidence.present)
            {
                return ReplyPassMismatch::MissingEvidence;
            }
            if (context.installPrices(evidence.prices).status != ReplyStockStatus::Valid)
            {
                return ReplyPassMismatch::Prices;
            }
            const ReplyStockCeiling ceiling = context.ceiling(plan.afterReply);
            if (ceiling.status != ReplyStockStatus::Valid || ceiling.ceiling != evidence.ceiling)
            {
                return ReplyPassMismatch::Ceiling;
            }
            const ReplyStockFloor floor = context.replayWitness(plan.afterReply, evidence.witness);
            if (floor.status != ReplyStockStatus::Valid)
            {
                return ReplyPassMismatch::Witness;
            }
            if (floor.floor != evidence.floor || floor.floor > ceiling.ceiling)
            {
                return ReplyPassMismatch::Floor;
            }
            const auto interval = checkedInterval(plan.exactNonStock, plan.owned, floor.floor, ceiling.ceiling);
            return interval && *interval == evidence.interval ? ReplyPassMismatch::None : ReplyPassMismatch::Interval;
        }

        ReplyPassMismatch checkPlan(const MasterAssignmentProblem & problem, const ReplyBoard & board,
                                    ReplyStockContext & context, const ReplyPassPlan & plan)
        {
            const ReplyPassEvaluation evaluation = evaluateReplyPassPlan(problem, plan.choices, board);
            if (!evaluation.valued())
            {
                return ReplyPassMismatch::PlanNoValue;
            }
            if (evaluation.replay.ordering.order != plan.executionOrder)
            {
                return ReplyPassMismatch::ExecutionOrder;
            }
            if (evaluation.afterOwnActions != plan.afterOwnActions || evaluation.afterReply != plan.afterReply)
            {
                return ReplyPassMismatch::Keys;
            }
            if (evaluation.actors != plan.actors)
            {
                return ReplyPassMismatch::Terms;
            }
            if (evaluation.exactNonStock != plan.exactNonStock)
            {
                return ReplyPassMismatch::ExactNonStock;
            }
            const ReplyStockOwned owned = context.owned(plan.afterReply);
            if (owned.status != ReplyStockStatus::Valid || owned.owned != plan.owned)
            {
                return ReplyPassMismatch::Owned;
            }
            return plan.baseline.present ? checkEvidence(context, plan, plan.baseline) : ReplyPassMismatch::None;
        }

        bool linked(const ReplyPassResult & result, std::size_t index)
        {
            const ReplyPassSwitch & made = result.switches[index];
            if (made.from != index || made.to != index + 1 || made.to >= result.plans.size() ||
                (made.tier != ReplyPassTier::Floor && made.tier != ReplyPassTier::Retune) ||
                !result.plans[made.from].baseline.present)
            {
                return false;
            }
            const std::vector<std::int32_t> & before = result.plans[made.from].choices;
            const std::vector<std::int32_t> & after = result.plans[made.to].choices;
            if (before.size() != after.size() || made.actorSlot < 0 ||
                static_cast<std::size_t>(made.actorSlot) >= after.size())
            {
                return false;
            }
            for (std::size_t slot = 0; slot < after.size(); ++slot)
            {
                const bool changed = slot == static_cast<std::size_t>(made.actorSlot);
                if (changed ? after[slot] != made.candidateIndex || before[slot] == after[slot]
                            : after[slot] != before[slot])
                {
                    return false;
                }
            }
            return true;
        }

        // Only a plan reached by a switch may lack a baseline, and only when its round never priced it.
        std::optional<std::size_t> missingBaseline(const ReplyPassResult & result)
        {
            if (!result.plans.front().baseline.present)
            {
                return 0;
            }
            const std::size_t last = result.plans.size() - 1;
            if (result.end == ReplyPassEnd::NoProvenChange && !result.plans[last].baseline.present)
            {
                return last;
            }
            return std::nullopt;
        }

        ReplyPassMismatch checkSwitch(ReplyStockContext & context, const ReplyPassResult & result, std::size_t index)
        {
            const ReplyPassSwitch & made = result.switches[index];
            if (made.incumbentUpper != result.plans[made.from].baseline.interval.upper)
            {
                return ReplyPassMismatch::Interval;
            }
            const ReplyPassMismatch proof = checkEvidence(context, result.plans[made.to], made.proof);
            if (proof != ReplyPassMismatch::None)
            {
                return proof;
            }
            if (made.changedLower != made.proof.interval.lower)
            {
                return ReplyPassMismatch::Interval;
            }
            MilliFunds margin = made.changedLower;
            return RelativeStockDetail::checkedSubtract(margin, made.incumbentUpper) && margin > 0 &&
                           margin == made.margin ?
                ReplyPassMismatch::None : ReplyPassMismatch::Margin;
        }
    }

    namespace ReplyPassDetail
    {
        std::vector<ReplyPassChange> listChanges(const MasterAssignmentProblem & problem,
                                                 std::span<const std::int32_t> working,
                                                 const ReplyPassEvaluation & evaluation, std::int32_t round,
                                                 ReplyPassCounters & counters)
        {
            std::set<std::int32_t> shotSlots;
            std::set<std::int32_t> firingShooters;
            for (const ReplyPassShot & shot : evaluation.shots)
            {
                if(shot.target<evaluation.actors.size())shotSlots.insert(evaluation.actors[shot.target].actorSlot);
                firingShooters.insert(shot.shooterIndex);
            }
            const std::span<const MasterActor> actors = problem.actors();
            std::vector<ReplyPassChange> changes;
            const auto addChanges = [&](std::size_t slot, ChangeReason reason, MilliFunds replyLoss)
            {
                const MasterActor & actor = actors[slot];
                for (std::size_t index = 0; index < actor.candidates.size(); ++index)
                {
                    const MasterCandidate & candidate = actor.candidates[index];
                    const auto candidateIndex = static_cast<std::int32_t>(index);
                    if (candidateIndex == working[slot] || !candidate.plannable ||
                        !consideredFor(reason, candidate.source.bundle, firingShooters))
                    {
                        continue;
                    }
                    changes.push_back(ReplyPassChange{
                        .round = round,
                        .actorSlot = static_cast<std::int32_t>(slot),
                        .engineUnitId = actor.engineUnitId,
                        .candidateIndex = candidateIndex,
                        .reason = reason,
                        .replyLoss = replyLoss,
                    });
                    ++(reason == ChangeReason::Shot ? counters.listedShot : counters.listedHitsShooter);
                }
            };
            for (const std::int32_t slot : shotSlots)
            {
                ++counters.unitsShot;
                if (!problem.canChangeActor(slot))
                {
                    const bool fixedRole = actors[static_cast<std::size_t>(slot)].role != MasterActorRole::Mutable;
                    ++(fixedRole ? counters.shotFixedRole : counters.shotCommitted);
                    continue;
                }
                ++counters.shotChangeable;
                addChanges(static_cast<std::size_t>(slot), ChangeReason::Shot, replyLossOf(evaluation, slot));
            }
            for (std::size_t slot = 0; slot < actors.size(); ++slot)
            {
                const auto actorSlot = static_cast<std::int32_t>(slot);
                if (!shotSlots.contains(actorSlot) && problem.canChangeActor(actorSlot))
                {
                    addChanges(slot, ChangeReason::HitsShooter, 0);
                }
            }
            std::sort(changes.begin(), changes.end(), ReplyPassChangeOrder{});
            return changes;
        }
    }

    ReplyPassResult runReplyPass(const MasterAssignmentProblem & problem, std::span<const std::int32_t> choices,
                                 const ReplyBoard & board, const PropertyStockField & field,
                                 const ReplyPassLimits & limits)
    {
        ReplyPassResult result;
        result.finalChoices.assign(choices.begin(), choices.end());
        if (!validLimits(limits))
        {
            result.end = ReplyPassEnd::InvalidLimits;
            return result;
        }
        ReplyPassRun(problem, board, field, limits, result).run(choices);
        return result;
    }

    ReplyPassVerification verifyReplyPass(const MasterAssignmentProblem & problem, const ReplyBoard & board,
                                          const PropertyStockField & field, const ReplyPassResult & result)
    {
        ReplyPassVerification verification;
        if (result.plans.empty() || result.switches.size() + 1 != result.plans.size())
        {
            verification.mismatch = result.plans.empty() ? ReplyPassMismatch::NoPlans : ReplyPassMismatch::SwitchLinks;
            return verification;
        }
        if (const auto missing = missingBaseline(result))
        {
            verification.index = *missing;
            verification.mismatch = ReplyPassMismatch::MissingEvidence;
            return verification;
        }
        for (std::size_t index = 0; index < result.switches.size(); ++index)
        {
            if (!linked(result, index))
            {
                verification.index = index;
                verification.mismatch = ReplyPassMismatch::SwitchLinks;
                return verification;
            }
        }
        ReplyStockContext context(field);
        for (std::size_t index = 0; index < result.plans.size(); ++index)
        {
            verification.index = index;
            verification.mismatch = checkPlan(problem, board, context, result.plans[index]);
            if (verification.mismatch != ReplyPassMismatch::None)
            {
                return verification;
            }
        }
        for (std::size_t index = 0; index < result.switches.size(); ++index)
        {
            verification.index = index;
            verification.mismatch = checkSwitch(context, result, index);
            if (verification.mismatch != ReplyPassMismatch::None)
            {
                return verification;
            }
        }
        verification.index = 0;
        verification.mismatch = result.finalChoices == result.plans.back().choices ? ReplyPassMismatch::None
                                                                                   : ReplyPassMismatch::FinalChoices;
        return verification;
    }
}
