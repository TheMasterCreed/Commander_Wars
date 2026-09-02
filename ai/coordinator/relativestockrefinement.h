#pragma once

#include <array>
#include <optional>
#include <utility>

#include "ai/coordinator/relativestocksession.h"

namespace Coordinator
{
    constexpr std::int64_t DEFAULT_RELATIVE_STOCK_WORK_LIMIT = 160000;
    static_assert(DEFAULT_RELATIVE_STOCK_WORK_LIMIT <= LIVE_PAIR_REFINEMENT_BUDGET);

    enum class RelativeStockTarget : std::int8_t
    {
        IncumbentOurs,
        ChallengerOurs,
        IncumbentEnemy,
        ChallengerEnemy,
    };

    enum class RelativeStockSlice : std::int8_t
    {
        Direct,
        Doubling,
    };

    enum class RelativeStockSchedule : std::int8_t
    {
        Current,
        CertifiedFirst,
        Requeue,
        FairDomains,
    };

    enum class RelativeStockStop : std::int8_t
    {
        Certified,
        InvalidComparison,
        InvalidRefinement,
        WorkBudget,
        TermCap,
        StepLimit,
    };

    struct RelativeStockRefinementPolicy
    {
        std::int64_t workLimit{0};
        std::int64_t maximumStateCap{LIVE_PAIR_REFINEMENT_BUDGET};
        RelativeStockSlice slice{RelativeStockSlice::Direct};
        std::int32_t maximumSteps{0};
    };

    struct RelativeStockRefinementCursor
    {
        std::array<std::int32_t, 4> rungs{};
        std::array<bool, 4> exhausted{};
        bool challengerOurRefinedForDual{false};
        bool incumbentDualAttempted{false};
    };

    struct RelativeStockStep
    {
        RelativeStockTarget target{RelativeStockTarget::IncumbentOurs};
        std::int64_t requestedCap{0};
        RelativeStockRefinedTerm refined;
        StockInterval before;
        RelativeStockComparison comparison;
        RelativeStockDualTightening dual;
        bool verifiedDual{false};
        bool adopted{false};
    };

    struct RelativeStockRefinementResult
    {
        RelativeStockComparison comparison;
        std::int64_t statesUsed{0};
        std::int64_t enemyStatesUsed{0};
        std::int64_t workUsed{0};
        std::int32_t steps{0};
        std::int32_t dualAttempts{0};
        std::int32_t dualTightenings{0};
        RelativeStockStop stop{RelativeStockStop::InvalidComparison};
        bool budgetExhausted{false};
    };

    namespace RelativeStockDetail
    {
        inline bool ourTarget(RelativeStockTarget target)
        {
            return target == RelativeStockTarget::IncumbentOurs || target == RelativeStockTarget::ChallengerOurs;
        }

        inline bool incumbentTarget(RelativeStockTarget target)
        {
            return target == RelativeStockTarget::IncumbentOurs || target == RelativeStockTarget::IncumbentEnemy;
        }

        inline const RelativeStockQuote & targetQuote(
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger, RelativeStockTarget target)
        {
            return incumbentTarget(target) ? incumbent : challenger;
        }

        inline StockInterval targetInterval(
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger, RelativeStockTarget target)
        {
            const auto & quote = targetQuote(incumbent, challenger, target);
            return ourTarget(target) ? quote.ours : quote.enemy;
        }

        inline bool sameProblem(const RelativeStockQuote & before, const RelativeStockQuote & after)
        {
            return before.identity.sameLiveModel(after.identity) && before.key == after.key &&
                before.capturedColumns == after.capturedColumns && before.economic == after.economic &&
                before.owned == after.owned;
        }

        inline bool monotone(StockInterval before, const RelativeStockRefinedTerm & after)
        {
            return after.valid && after.lowerWitnessReplays && after.interval.lower >= before.lower &&
                after.interval.upper <= before.upper && after.interval.lower <= after.interval.upper &&
                (!after.searchCompleted || after.interval.exact());
        }

        inline std::optional<RelativeStockTarget> widestTarget(
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
            bool commonEnemy, const std::array<bool, 4> & exhausted)
        {
            std::optional<RelativeStockTarget> target;
            MilliFunds widest = 0;
            for (std::size_t index = 0; index < exhausted.size(); ++index)
            {
                const auto candidate = static_cast<RelativeStockTarget>(index);
                if (exhausted[index] || (commonEnemy && !ourTarget(candidate)))
                {
                    continue;
                }
                const auto interval = targetInterval(incumbent, challenger, candidate);
                MilliFunds width = interval.upper;
                if (!checkedSubtract(width, interval.lower))
                {
                    return std::nullopt;
                }
                if (width > widest)
                {
                    widest = width;
                    target = candidate;
                }
            }
            return target;
        }
    }

    template<class Observer>
    RelativeStockRefinementResult refineRelativeStock(
        RelativeStockSession & session, RelativeStockQuote & incumbent, RelativeStockQuote & challenger,
        const RelativeStockRefinementPolicy & policy, RelativeStockRefinementCursor & cursor, Observer && observe)
    {
        RelativeStockRefinementResult result;
        result.comparison = session.compare(incumbent, challenger);
        const auto workLimit = std::clamp<std::int64_t>(policy.workLimit, 0, LIVE_PAIR_REFINEMENT_BUDGET);
        while (result.comparison.valid() && result.comparison.unresolved())
        {
            if (policy.maximumSteps > 0 && result.steps >= policy.maximumSteps)
            {
                result.stop = RelativeStockStop::StepLimit;
                return result;
            }
            const auto challengerIndex =
                static_cast<std::size_t>(
                    RelativeStockTarget::ChallengerOurs);
            const bool preparingDual =
                session.supportsVerifiedDual() &&
                !cursor.challengerOurRefinedForDual &&
                !cursor.incumbentDualAttempted &&
                result.comparison.commonEnemyCancelled &&
                !cursor.exhausted[challengerIndex];
            const auto target = preparingDual ?
                std::optional<RelativeStockTarget>(
                    RelativeStockTarget::ChallengerOurs) :
                RelativeStockDetail::widestTarget(
                    incumbent,
                    challenger,
                    result.comparison.commonEnemyCancelled,
                    cursor.exhausted);
            if (!target || policy.maximumStateCap <= 0)
            {
                result.stop = RelativeStockStop::TermCap;
                return result;
            }
            const auto & source = RelativeStockDetail::targetQuote(incumbent, challenger, *target);
            const auto tariff = RelativeStockDetail::ourTarget(*target) ? session.ourRerunWork() :
                session.enemyRerunWork(source.capturedColumns);
            const auto available = std::min(workLimit - result.workUsed, session.remainingWork());
            if (tariff < 0 || available <= tariff)
            {
                result.stop = RelativeStockStop::WorkBudget;
                result.budgetExhausted = true;
                return result;
            }
            const auto index = static_cast<std::size_t>(*target);
            std::int64_t scheduled = std::min(policy.maximumStateCap, LIVE_PAIR_REFINEMENT_BUDGET);
            std::int32_t scheduledRung = cursor.rungs[index];
            if (policy.slice == RelativeStockSlice::Doubling)
            {
                if (preparingDual &&
                    *target ==
                        RelativeStockTarget::
                            ChallengerOurs &&
                    scheduledRung == 0)
                {
                    scheduledRung = 1;
                }
                std::int64_t rungCap = 0;
                if (!livePairRefinementSliceCap(
                        scheduledRung,
                        rungCap))
                {
                    result.stop = RelativeStockStop::InvalidRefinement;
                    return result;
                }
                scheduled = std::min(scheduled, rungCap);
            }
            RelativeStockStep step;
            step.target = *target;
            step.requestedCap = std::min(scheduled, available - tariff);
            step.before = RelativeStockDetail::targetInterval(incumbent, challenger, *target);
            const auto balanceBefore = session.remainingWork();
            step.refined = RelativeStockDetail::ourTarget(*target) ?
                session.refineOur(source.key, step.requestedCap) :
                session.refineEnemy(source.capturedColumns, step.requestedCap);
            const bool bounded = step.refined.statesUsed >= 0 && step.refined.statesUsed <= step.requestedCap &&
                step.refined.workUsed >= step.refined.statesUsed && step.refined.workUsed <= available &&
                session.remainingWork() == balanceBefore - step.refined.workUsed;
            if (bounded)
            {
                result.statesUsed += step.refined.statesUsed;
                result.workUsed += step.refined.workUsed;
                if (!RelativeStockDetail::ourTarget(*target))
                {
                    result.enemyStatesUsed += step.refined.statesUsed;
                }
            }
            auto refreshedIncumbent = session.quote(incumbent.key, incumbent.economic);
            auto refreshedChallenger = session.quote(challenger.key, challenger.economic);
            const auto refreshedInterval = RelativeStockDetail::targetInterval(
                refreshedIncumbent, refreshedChallenger, *target);
            const auto & refreshedQuote = RelativeStockDetail::targetQuote(refreshedIncumbent, refreshedChallenger, *target);
            const bool refreshedCompleted = RelativeStockDetail::ourTarget(*target) ?
                refreshedQuote.ourSearchCompleted : refreshedQuote.enemySearchCompleted;
            const auto comparison = session.compare(refreshedIncumbent, refreshedChallenger);
            step.adopted = bounded && RelativeStockDetail::monotone(step.before, step.refined) &&
                RelativeStockDetail::sameProblem(incumbent, refreshedIncumbent) &&
                RelativeStockDetail::sameProblem(challenger, refreshedChallenger) && comparison.valid() &&
                refreshedInterval.lower == step.refined.interval.lower &&
                refreshedInterval.upper == step.refined.interval.upper &&
                refreshedCompleted == step.refined.searchCompleted;
            if (step.adopted)
            {
                incumbent = std::move(refreshedIncumbent);
                challenger = std::move(refreshedChallenger);
                result.comparison = comparison;
                cursor.exhausted[index] = policy.slice == RelativeStockSlice::Direct ||
                    scheduled == policy.maximumStateCap || step.refined.interval.exact();
                cursor.rungs[index] =
                    scheduledRung + 1;
                cursor.challengerOurRefinedForDual =
                    cursor.challengerOurRefinedForDual ||
                    *target ==
                        RelativeStockTarget::
                            ChallengerOurs;
            }
            step.comparison = result.comparison;
            ++result.steps;
            observe(step, incumbent, challenger);
            if (!step.adopted)
            {
                result.stop = step.refined.budgetExhausted ? RelativeStockStop::WorkBudget :
                    RelativeStockStop::InvalidRefinement;
                result.budgetExhausted = step.refined.budgetExhausted;
                return result;
            }

            if (cursor.challengerOurRefinedForDual &&
                !cursor.incumbentDualAttempted &&
                session.supportsVerifiedDual() &&
                result.comparison.unresolved() &&
                result.comparison.commonEnemyCancelled)
            {
                cursor.incumbentDualAttempted = true;
                const auto available =
                    std::min(
                        workLimit - result.workUsed,
                        session.remainingWork());
                RelativeStockStep dualStep;
                dualStep.target =
                    RelativeStockTarget::IncumbentOurs;
                dualStep.requestedCap = available;
                dualStep.before = incumbent.ours;
                dualStep.verifiedDual = true;
                const auto balanceBefore =
                    session.remainingWork();
                dualStep.dual =
                    session.tightenOurUpperWithVerifiedDual(
                        incumbent,
                        available);
                ++result.dualAttempts;
                const bool bounded =
                    dualStep.dual.totalWork >= 0 &&
                    dualStep.dual.totalWork <= available &&
                    session.remainingWork() ==
                        balanceBefore -
                            dualStep.dual.totalWork;
                if (bounded)
                {
                    result.workUsed +=
                        dualStep.dual.totalWork;
                }
                auto dualIncumbent =
                    session.quote(
                        incumbent.key,
                        incumbent.economic);
                auto dualChallenger =
                    session.quote(
                        challenger.key,
                        challenger.economic);
                const auto dualComparison =
                    session.compare(
                        dualIncumbent,
                        dualChallenger);
                dualStep.adopted =
                    bounded &&
                    dualStep.dual.certificateValid &&
                    dualStep.dual.certificate != nullptr &&
                    dualStep.dual.certificateId != 0 &&
                    !dualStep.dual.globalFailure &&
                    RelativeStockDetail::sameProblem(
                        incumbent,
                        dualIncumbent) &&
                    RelativeStockDetail::sameProblem(
                        challenger,
                        dualChallenger) &&
                    dualComparison.valid() &&
                    dualIncumbent.ours.lower ==
                        dualStep.before.lower &&
                    dualIncumbent.ours.upper ==
                        dualStep.dual.intervalAfter.upper &&
                    dualIncumbent.ourSearchCompleted ==
                        incumbent.ourSearchCompleted &&
                    dualIncumbent.ourLowerWitnessReplays &&
                    dualIncumbent.ourUpperCertificateId ==
                        dualStep.dual.certificateId;
                dualStep.refined = RelativeStockRefinedTerm{
                    .interval =
                        dualStep.dual.intervalAfter,
                    .statesUsed = 0,
                    .workUsed =
                        dualStep.dual.totalWork,
                    .searchCompleted =
                        dualIncumbent.ourSearchCompleted,
                    .lowerWitnessReplays =
                        dualIncumbent.
                            ourLowerWitnessReplays,
                    .budgetExhausted =
                        dualStep.dual.
                            locallyBudgetExhausted,
                    .valid = dualStep.adopted,
                };
                if (dualStep.adopted)
                {
                    incumbent =
                        std::move(dualIncumbent);
                    challenger =
                        std::move(dualChallenger);
                    result.comparison =
                        dualComparison;
                    if (dualStep.dual.tightened())
                    {
                        ++result.dualTightenings;
                    }
                }
                dualStep.comparison =
                    result.comparison;
                ++result.steps;
                observe(
                    dualStep,
                    incumbent,
                    challenger);
                if (!bounded ||
                    dualStep.dual.globalFailure ||
                    (dualStep.dual.certificateValid &&
                     !dualStep.adopted))
                {
                    result.stop =
                        RelativeStockStop::
                            InvalidRefinement;
                    return result;
                }
            }
        }
        result.stop = result.comparison.valid() ? RelativeStockStop::Certified : RelativeStockStop::InvalidComparison;
        return result;
    }

    template<class Observer>
    RelativeStockRefinementResult refineRelativeStock(
        RelativeStockSession & session, RelativeStockQuote & incumbent, RelativeStockQuote & challenger,
        const RelativeStockRefinementPolicy & policy, Observer && observe)
    {
        RelativeStockRefinementCursor cursor;
        return refineRelativeStock(session, incumbent, challenger, policy, cursor, std::forward<Observer>(observe));
    }

    inline bool replayRelativeStockProof(
        RelativeStockSession & session, const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
        const RelativeStockQuote & replayedIncumbent, const RelativeStockQuote & replayedChallenger)
    {
        if (!sameRelativeStockQuote(incumbent, replayedIncumbent) ||
            !sameRelativeStockQuote(challenger, replayedChallenger))
        {
            return false;
        }
        const auto expected = session.compare(incumbent, challenger);
        const auto replayed = session.compare(replayedIncumbent, replayedChallenger);
        return expected.improves() && replayed.improves() && expected.deltaLower == replayed.deltaLower &&
            expected.deltaUpper == replayed.deltaUpper && session.replayOurWitness(replayedIncumbent) &&
            session.replayOurWitness(replayedChallenger) &&
            (expected.commonEnemyCancelled ||
             (session.replayEnemyWitness(replayedIncumbent) && session.replayEnemyWitness(replayedChallenger)));
    }
}
