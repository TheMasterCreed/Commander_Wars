#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/assignmentvaluation.h"
#include "ai/coordinator/capturedangerpricing.h"
#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/masterreplyactions.h"
#include "ai/coordinator/replyprediction.h"
#include "ai/coordinator/stockreplykeys.h"

namespace Coordinator
{
    enum class ReplyPassNoValue : std::int8_t
    {
        None,
        ReplayInvalid,
        ReplayIncomplete,
        UnownedAction,
        MissingFacts,
        UnsupportedAction,
        SelfService,
        TargetAlreadyGone,
        UnusablePlanChain,
        MissingNextShotBasis,
        KeyRejected,
        Arithmetic,
        NotEvaluated,
    };

    inline constexpr std::array REPLY_PASS_NO_VALUE_NAMES{
        "NONE", "REPLAY_INVALID", "REPLAY_INCOMPLETE", "UNOWNED_ACTION", "MISSING_FACTS", "UNSUPPORTED_ACTION",
        "SELF_SERVICE", "TARGET_ALREADY_GONE", "UNUSABLE_PLAN_CHAIN", "MISSING_NEXT_SHOT_BASIS", "KEY_REJECTED",
        "ARITHMETIC", "NOT_EVALUATED",
    };

    constexpr std::int32_t NO_REPLY_PASS_ACTOR = -1;

    struct ReplyPassTerms
    {
        MilliFunds actingSeated{0};
        MilliFunds ownCounterCorrection{0};
        MilliFunds replyCapitalLoss{0};
        MilliFunds replyCounterCapital{0};
        MilliFunds nextShotAfterReply{0};

        friend bool operator==(const ReplyPassTerms &, const ReplyPassTerms &) = default;
    };

    struct ReplyPassActor
    {
        std::int32_t actorSlot{NO_REPLY_PASS_ACTOR};
        std::int32_t engineUnitId{NO_UNIT};
        std::int32_t actionIndex{NO_ACTION};
        std::int32_t candidateIndex{NO_CANDIDATE};
        std::int32_t grantedDamage{0};
        std::int32_t dayStartSteps{0};
        std::int32_t stepsAfterOwnActions{0};
        std::int32_t stepsAfterReply{0};
        StockDeathCause destroyedBy{StockDeathCause::None};
        std::int32_t stockRow{NO_STOCK_ROW};
        ReplyPassTerms terms;

        friend bool operator==(const ReplyPassActor &, const ReplyPassActor &) = default;
    };

    // target is the position of the shot unit in ReplyPassEvaluation::actors.
    struct ReplyPassShot
    {
        std::int32_t shooterIndex{NO_UNIT};
        std::size_t target{0};
        std::int32_t hpStepsBefore{0};
        std::int32_t hpStepsAfter{0};
        MilliFunds counterCapital{0};

        friend bool operator==(const ReplyPassShot &, const ReplyPassShot &) = default;
    };

    // actors run in execution order; exactNonStock holds only once reason is None.
    struct ReplyPassEvaluation
    {
        ReplyPassNoValue reason{ReplyPassNoValue::NotEvaluated};
        std::int32_t actorSlot{NO_REPLY_PASS_ACTOR};
        MasterAssignmentReplayResult replay;
        std::vector<ReplyPassActor> actors;
        std::vector<ReplyPassShot> shots;
        ContinuationKey afterOwnActions;
        ContinuationKey afterReply;
        MilliFunds exactNonStock{0};
        std::int64_t pairsExamined{0};

        bool valued() const
        {
            return reason == ReplyPassNoValue::None;
        }
    };

    namespace ReplyPassEvaluationDetail
    {
        constexpr ReplyPassNoValue noValueOf(StockQuoteStatus status)
        {
            switch (status)
            {
                case StockQuoteStatus::Available:
                    return ReplyPassNoValue::None;
                case StockQuoteStatus::MissingField:
                case StockQuoteStatus::MissingActionFacts:
                    return ReplyPassNoValue::MissingFacts;
                case StockQuoteStatus::UnusablePlanChain:
                    return ReplyPassNoValue::UnusablePlanChain;
                case StockQuoteStatus::ReplayFailed:
                    return ReplyPassNoValue::ReplayInvalid;
                case StockQuoteStatus::CyclicOrdering:
                    return ReplyPassNoValue::ReplayIncomplete;
                case StockQuoteStatus::UnsupportedAction:
                    return ReplyPassNoValue::UnsupportedAction;
                case StockQuoteStatus::TargetAlreadyGone:
                    break;
            }
            return ReplyPassNoValue::TargetAlreadyGone;
        }

        inline ReplyPassEvaluation & refuse(ReplyPassEvaluation & result, ReplyPassNoValue reason,
                                            std::int32_t actorSlot = NO_REPLY_PASS_ACTOR)
        {
            result.reason = reason;
            result.actorSlot = actorSlot;
            return result;
        }

        inline bool servesSelf(const ActionBundle & bundle)
        {
            return std::any_of(bundle.components.begin(), bundle.components.end(), [&](const BundleComponent & part)
            {
                return part.kind == ComponentKind::Service && part.service.partnerUnitId == bundle.unitId;
            });
        }

        inline std::optional<MilliFunds> checkedBooking(MilliFunds cost, std::int32_t steps)
        {
            if (steps < 0 ||
                !AssignmentValuationDetail::nonnegativeRoundedProductFits(cost, steps, UNIT_HP_STEPS))
            {
                return std::nullopt;
            }
            return bookDamage(cost, steps);
        }

        // valueBundle booked the isolated counter; the plan replaces it with the counter the actor really took.
        inline std::optional<MilliFunds> ownCounterCorrection(const CandidateBundle & candidate,
                                                              std::int32_t measuredTaken)
        {
            const auto & components = candidate.bundle.components;
            if (components.size() != 1 || components.front().kind != ComponentKind::Fire)
            {
                return MilliFunds{0};
            }
            const FireFacts & fire = components.front().fire;
            const bool isolatedKill = std::min(fire.damageSteps, fire.targetHpSteps) >= fire.targetHpSteps;
            const std::int32_t isolatedTaken =
                isolatedKill ? 0 : std::min(fire.counterSteps, candidate.actor.hpSteps);
            std::optional<MilliFunds> correction = checkedBooking(candidate.actor.replacementCost, isolatedTaken);
            const std::optional<MilliFunds> measured = checkedBooking(candidate.actor.replacementCost, measuredTaken);
            if (!correction || !measured || !RelativeStockDetail::checkedSubtract(*correction, *measured))
            {
                return std::nullopt;
            }
            return correction;
        }

        inline std::optional<MilliFunds> checkedReplyCapitalLoss(MilliFunds cost, std::int32_t replyStart,
                                                                 std::int32_t finalSteps)
        {
            if (replyStart > finalSteps && !checkedBooking(cost, replyStart - finalSteps))
            {
                return std::nullopt;
            }
            return replyCapitalLoss(cost, replyStart, finalSteps);
        }

        inline std::optional<MilliFunds> scaledNextShot(MilliFunds best, std::int32_t finalSteps, std::int32_t basis)
        {
            if (best > std::numeric_limits<MilliFunds>::max() / finalSteps ||
                best < std::numeric_limits<MilliFunds>::lowest() / finalSteps)
            {
                return std::nullopt;
            }
            return best * finalSteps / basis;
        }

        inline std::optional<MilliFunds> checkedTermSum(const ReplyPassTerms & terms)
        {
            MilliFunds sum = terms.actingSeated;
            if (!RelativeStockDetail::checkedAdd(sum, terms.ownCounterCorrection) ||
                !RelativeStockDetail::checkedSubtract(sum, terms.replyCapitalLoss) ||
                !RelativeStockDetail::checkedAdd(sum, terms.replyCounterCapital) ||
                !RelativeStockDetail::checkedAdd(sum, terms.nextShotAfterReply))
            {
                return std::nullopt;
            }
            return sum;
        }

        // P1's net is a booked shot less a counter cost and a capture cost.
        constexpr MilliFunds ONE_ACTION_NET_TERMS = 3;
        // A booking of UNIT_HP_STEPS steps rounds within range, and three such amounts still fit one net.
        constexpr MilliFunds ONE_ACTION_AMOUNT_LIMIT =
            std::min((std::numeric_limits<MilliFunds>::max() - UNIT_HP_STEPS / 2) / UNIT_HP_STEPS,
                     std::numeric_limits<MilliFunds>::max() / ONE_ACTION_NET_TERMS);
        // Counter bookings and the shooter hp factor are sized for a unit of at most UNIT_HP_STEPS steps.
        constexpr std::int32_t ONE_ACTION_SHOOTER_HP_LIMIT = UNIT_HP_STEPS;
        // terrainDefenseBonus multiplies stars by defense per star in int and converts the scaled product back.
        constexpr std::int64_t TERRAIN_PRODUCT_MAX = std::numeric_limits<std::int32_t>::max();
        constexpr std::int64_t TERRAIN_PRODUCT_MIN = std::numeric_limits<std::int32_t>::min();
        // With the terrain product in int range, offence, defence and the classic factor stay within this magnitude.
        constexpr double TERRAIN_TERM_LIMIT =
            NEUTRAL_DEFENSE + CLASSIC_DEFENSE_CEILING - static_cast<double>(TERRAIN_PRODUCT_MIN);
        // A base, luck or counter base this small keeps base * offence * classic finite, so no NaN reaches ceilHpSteps.
        constexpr double REPLY_FACTOR_LIMIT =
            std::numeric_limits<double>::max() / (TERRAIN_TERM_LIMIT * TERRAIN_TERM_LIMIT * TERRAIN_TERM_LIMIT);

        // ownActionFacts converts it with ceilHpSteps, and every hp the reply reads descends from it.
        inline bool actorRealHpFits(double realHp)
        {
            return std::isfinite(realHp) && realHp > 0 && realHp <= UNIT_HP_STEPS;
        }

        inline bool onBoard(const TilePoint & tile, const ReplyBoard & board)
        {
            return tile.x >= 0 && tile.y >= 0 && tile.x < board.width && tile.y < board.height;
        }

        // drawsCounter takes an int Manhattan distance, so every named tile is on the board or explicitly absent.
        inline bool replyTilesOnBoard(const CandidateBundle & candidate, const ReplyBoard & board)
        {
            const auto placed = [&](const TilePoint & tile) { return tile == INVALID_TILE || onBoard(tile, board); };
            const ReplyTileFacts & facts = *candidate.replyFacts->tile;
            return onBoard(candidate.bundle.destination, board) && placed(facts.target) &&
                   std::all_of(facts.shooters.begin(), facts.shooters.end(), [&](const ReplyShooterFacts & shooter)
                   {
                       return placed(shooter.tile) &&
                              std::all_of(shooter.firingTiles.begin(), shooter.firingTiles.end(), placed);
                   });
        }

        constexpr bool oneActionAmountFits(MilliFunds amount)
        {
            return amount >= -ONE_ACTION_AMOUNT_LIMIT && amount <= ONE_ACTION_AMOUNT_LIMIT;
        }

        // False for NaN, since every comparison with it fails.
        inline bool replyFactorFits(double value)
        {
            return std::abs(value) <= REPLY_FACTOR_LIMIT;
        }

        constexpr bool terrainFits(const TerrainDefenseRule & rule)
        {
            const std::int64_t product = static_cast<std::int64_t>(rule.stars) * rule.defensePerStar;
            return product >= TERRAIN_PRODUCT_MIN && product <= TERRAIN_PRODUCT_MAX;
        }

        // planReply's pool hp: the plan chain's hp for a shooter our actions damaged, its day start hp otherwise.
        inline bool shooterHpFits(const ResolvedPlanReply & resolved, std::int32_t shooterIndex)
        {
            const auto damaged = resolved.own.chain.chain.find(shooterIndex);
            const std::int32_t hpSteps =
                damaged == resolved.own.chain.chain.end() ? resolved.startHp.at(shooterIndex) : damaged->second.after;
            return hpSteps <= ONE_ACTION_SHOOTER_HP_LIMIT;
        }

        inline bool replySlotFits(const ResolvedPlanReply & resolved, const ReplySlotFacts & slot,
                                  const ResolvedReplyShot & shot)
        {
            return oneActionAmountFits(slot.shooterCost) && oneActionAmountFits(slot.captureCost) &&
                   shooterHpFits(resolved, slot.shooterIndex) && replyFactorFits(slot.counterRawBase) &&
                   replyFactorFits(shot.rawBase) && replyFactorFits(shot.withAmmo);
        }

        // The first target whose facts could make P1 overflow or convert an out-of-range value, or none.
        inline std::optional<std::size_t> oneActionOverflowTarget(const ResolvedPlanReply & resolved)
        {
            for (std::size_t index = 0; index < resolved.targets.size(); ++index)
            {
                const ReplyTargetFacts & target = resolved.targets[index];
                const PlanReplyEntry & entry = resolved.entries[index];
                bool fits = oneActionAmountFits(target.target.cost) && terrainFits(entry.pFacts->terrain) &&
                            replyFactorFits(entry.pFacts->terrain.luck);
                for (std::size_t slot = 0; fits && slot < target.shooters.size(); ++slot)
                {
                    fits = replySlotFits(resolved, target.shooters[slot], entry.shots[slot]);
                }
                if (!fits)
                {
                    return index;
                }
            }
            return std::nullopt;
        }
    }
    // Pure: the replay runs without reply prediction, so the problem's shared tally is never touched.
    inline ReplyPassEvaluation evaluateReplyPassPlan(const MasterAssignmentProblem & problem,
                                                     std::span<const std::int32_t> choices,
                                                     const ReplyBoard & board)
    {
        using namespace ReplyPassEvaluationDetail;
        ReplyPassEvaluation result;
        result.replay = replayMasterAssignment(problem, choices, ReplyPredictionSetup{});
        const MasterAssignmentReplayResult & replay = result.replay;
        if (!replay.valid())
        {
            return refuse(result, ReplyPassNoValue::ReplayInvalid, replay.actorSlot);
        }
        if (!replay.complete())
        {
            return refuse(result, ReplyPassNoValue::ReplayIncomplete);
        }
        const std::optional<MasterReplyActions> owned = masterReplyActions(problem.actors(), choices, replay);
        if (!owned)
        {
            return refuse(result, ReplyPassNoValue::UnownedAction);
        }
        const std::vector<PlanReplyAction> & actions = owned->actions;
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const std::size_t slot = owned->actorSlots[index];
            const std::int32_t actorSlot = static_cast<std::int32_t>(slot);
            const CandidateBundle & candidate = *actions[index].pCandidate;
            if (!ReplyPredictionDetail::resolvable(actions[index]) ||
                !actorRealHpFits(candidate.replyFacts->actorRealHp) || !replyTilesOnBoard(candidate, board))
            {
                return refuse(result, ReplyPassNoValue::MissingFacts, actorSlot);
            }
            if (servesSelf(candidate.bundle))
            {
                return refuse(result, ReplyPassNoValue::SelfService, actorSlot);
            }
            result.actors.push_back(ReplyPassActor{
                .actorSlot = actorSlot,
                .engineUnitId = actions[index].engineUnitId,
                .actionIndex = actions[index].actionIndex,
                .candidateIndex = choices[slot],
                .grantedDamage = replay.plan.action(actions[index].actionIndex).plannedDamageSteps,
                .dayStartSteps = candidate.actor.hpSteps,
                .stockRow = problem.actors()[slot].stockRow,
            });
        }
        const ResolvedPlanReply resolved = resolvePlanReply(actions, board);
        if (!resolved.complete)
        {
            const bool named = resolved.failedAction < result.actors.size();
            return refuse(result, noValueOf(resolved.own.status),
                          named ? result.actors[resolved.failedAction].actorSlot : NO_REPLY_PASS_ACTOR);
        }
        if (const std::optional<std::size_t> overflow = oneActionOverflowTarget(resolved))
        {
            return refuse(result, ReplyPassNoValue::Arithmetic, result.actors[*overflow].actorSlot);
        }
        const PlanReplyRun run = runResolvedPlanReply(resolved);
        result.pairsExamined = run.reply.pairsExamined;
        for (const OneActionShot & shot : run.reply.shots)
        {
            result.shots.push_back(ReplyPassShot{
                run.pool[shot.shooter], shot.target, shot.hpStepsBefore, shot.hpStepsAfter, shot.counterCost});
        }
        for (std::size_t index = 0; index < result.actors.size(); ++index)
        {
            ReplyPassActor & entry = result.actors[index];
            const CandidateBundle & candidate = *actions[index].pCandidate;
            entry.stepsAfterOwnActions = resolved.own.chain.outcomes[index].hpSteps;
            entry.stepsAfterReply = run.reply.outcomes[index].hpSteps;
            entry.destroyedBy = entry.stepsAfterOwnActions <= 0 ? StockDeathCause::OwnAction :
                                entry.stepsAfterReply <= 0 ? StockDeathCause::Reply : StockDeathCause::None;
            const std::int32_t replyStart = ceilHpSteps(resolved.targets[index].target.hp);
            const MilliFunds cost = candidate.actor.replacementCost;
            const auto acting = checkedActingSeatedCandidateValue(candidate, entry.grantedDamage);
            const auto correction =
                ownCounterCorrection(candidate, entry.dayStartSteps - entry.stepsAfterOwnActions);
            const auto capital = checkedReplyCapitalLoss(cost, replyStart, entry.stepsAfterReply);
            MilliFunds counter = 0;
            bool counted = true;
            for (const ReplyPassShot & shot : result.shots)
            {
                counted = counted && (shot.target != index ||
                                      RelativeStockDetail::checkedAdd(counter, shot.counterCapital));
            }
            if (!acting || !correction || !capital || !counted)
            {
                return refuse(result, ReplyPassNoValue::Arithmetic, entry.actorSlot);
            }
            std::optional<MilliFunds> nextShot = MilliFunds{0};
            if (entry.stepsAfterReply > 0)
            {
                // Destination shots were generated at the isolated hp, so a unit that view kills has none.
                const std::int32_t basis = hpAfterAction(candidate.bundle, entry.dayStartSteps).hpSteps;
                if (basis <= 0)
                {
                    return refuse(result, ReplyPassNoValue::MissingNextShotBasis, entry.actorSlot);
                }
                nextShot = scaledNextShot(bestShot(candidate.actorNextShotsAtDestination), entry.stepsAfterReply,
                                          basis);
            }
            entry.terms = ReplyPassTerms{*acting, *correction, *capital, counter, nextShot.value_or(0)};
            const std::optional<MilliFunds> value = checkedTermSum(entry.terms);
            if (!nextShot || !value || !RelativeStockDetail::checkedAdd(result.exactNonStock, *value))
            {
                return refuse(result, ReplyPassNoValue::Arithmetic, entry.actorSlot);
            }
            if (entry.stockRow == NO_STOCK_ROW)
            {
                continue;
            }
            const PlannedAction & action = replay.plan.action(entry.actionIndex);
            const auto stock = stockReplyRow(entry.stockRow, action.destination, capturesInPlan(action.kind),
                                             entry.dayStartSteps, entry.stepsAfterOwnActions, entry.stepsAfterReply);
            if (!stock)
            {
                return refuse(result, ReplyPassNoValue::KeyRejected, entry.actorSlot);
            }
            result.afterOwnActions.push_back(stock->keys.afterOwnActions);
            result.afterReply.push_back(stock->keys.afterReply);
        }
        std::sort(result.afterOwnActions.begin(), result.afterOwnActions.end());
        std::sort(result.afterReply.begin(), result.afterReply.end());
        result.reason = ReplyPassNoValue::None;
        return result;
    }
}