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
#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/stockreplykeys.h"
#include "ai/coordinator/survivorshot.h"

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
        MovementLimit,
    };

    inline constexpr std::array REPLY_PASS_NO_VALUE_NAMES{
        "NONE", "REPLAY_INVALID", "REPLAY_INCOMPLETE", "UNOWNED_ACTION", "MISSING_FACTS", "UNSUPPORTED_ACTION",
        "SELF_SERVICE", "TARGET_ALREADY_GONE", "UNUSABLE_PLAN_CHAIN", "MISSING_NEXT_SHOT_BASIS", "KEY_REJECTED",
        "ARITHMETIC", "NOT_EVALUATED", "MOVEMENT_LIMIT",
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
        OwnShotOutcome ownShot;
        std::int32_t dayStartSteps{0};
        std::int32_t stepsAfterOwnActions{0};
        std::int32_t stepsAfterReply{0};
        StockDeathCause destroyedBy{StockDeathCause::None};
        std::int32_t stockRow{NO_STOCK_ROW};
        ReplyPassTerms terms;
        bool repricedSurvivor{false};
        std::array<int, 2> survivorAmmo{};

        friend bool operator==(const ReplyPassActor &, const ReplyPassActor &) = default;
    };

    // Submitted defenders occupy the leading E0 target slots. A later target
    // was already spent on the observed board and has no submitted F0 action.
    struct ReplyPassShot
    {
        std::int32_t shooterIndex{NO_UNIT};
        std::size_t target{0};
        std::int32_t hpStepsBefore{0};
        std::int32_t hpStepsAfter{0};
        MilliFunds counterCapital{0};
        TilePoint firingTile{INVALID_TILE};
        double realBefore{}, realAfter{};
        MilliFunds targetCapital{};
        int targetKnowledge{NO_UNIT},targetEngineUnitId{NO_UNIT};
        int counterWeapon{-1};

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
        std::optional<OpeningRecruitFight> openingRecruitFight;
        std::vector<TilePoint> openingRecruitFiringSquares;
        std::vector<OpeningRecruitWindow> openingRecruitWindows;
        std::vector<OpeningLaneAction> servicedStockShots;
        std::vector<TilePoint> combatLocations;
        CombatEncirclement encirclement;
        std::vector<LethalFiringThreat> lethalThreats;
        CaptureChips captureChips;
        int enemyWaiting{NO_UNIT};
        std::map<int, int> enemyReplyHp;
        std::map<int, int> enemyAfterOwn;
        std::map<int, double> realAfterReply;

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
    struct CombatFireOrder
    {
        int first{NO_UNIT}, second{NO_UNIT};
        bool operator==(const CombatFireOrder &) const = default;
    };

    inline bool installCombatFireOrder(MasterAssignmentReplayResult &replay, CombatFireOrder order)
    {
        if (order == CombatFireOrder{})
            return true;
        int first = NO_ACTION, second = NO_ACTION;
        for (int index : replay.ordering.order)
        {
            const auto &action = replay.plan.action(index);
            if (action.unitId == order.first) first = index;
            if (action.unitId == order.second) second = index;
        }
        if (first == NO_ACTION || second == NO_ACTION || first == second)
            return false;
        const auto &a = replay.plan.action(first), &b = replay.plan.action(second);
        const auto fire = [](PlanBundleKind kind) { return kind == PlanBundleKind::Fire || kind == PlanBundleKind::MoveAndFire; };
        if (a.state == PlanActionState::Committed || b.state == PlanActionState::Committed ||
            !fire(a.kind) || !fire(b.kind) || a.targetUnitId == NO_UNIT || a.targetUnitId != b.targetUnitId)
            return false;
        for (int i = 0; i < replay.plan.edgeCount(); ++i)
            if (const auto &e = replay.plan.edge(i); e.from == first && e.to == second)
                return true;
        if (!replay.plan.addEdge(first, second, PlanEdgeKind::CombinedFire))
            return false;
        replay.ordering = replay.plan.executionOrder();
        return replay.complete();
    }

    inline void removeIncomingCombinedFireConflict(MasterAssignmentReplayResult & replay, CombatFireOrder order)
    {
        if (order == CombatFireOrder{})
            return;
        int first = NO_ACTION, second = NO_ACTION;
        for (int index : replay.ordering.order)
        {
            const auto & action = replay.plan.action(index);
            if (action.unitId == order.first) first = index;
            if (action.unitId == order.second) second = index;
        }
        if (first != NO_ACTION && second != NO_ACTION && first != second)
            replay.plan.removeCombinedFireEdgesBetween(first, second);
    }

    inline void installCombatFireOrders(MasterAssignmentReplayResult &replay, std::span<const CombatFireOrder> orders)
    {
        for (const CombatFireOrder order : orders)
        {
            MasterAssignmentReplayResult trial = replay;
            if (installCombatFireOrder(trial, order))
                replay = std::move(trial);
        }
    }

    // Pure: the replay runs without reply prediction, so the problem's shared tally is never touched.
    inline ReplyPassEvaluation evaluateReplyPassPlan(const MasterAssignmentProblem & problem,
                                                     std::span<const std::int32_t> choices,
                                                     const ReplyBoard & board, CombatFireOrder fireOrder = {},
                                                     std::span<const CombatFireOrder> extraFireOrders = {})
    {
        using namespace ReplyPassEvaluationDetail;
        pumpUiEventsIfDue();
        ReplyPassEvaluation result;
        result.replay = replayMasterAssignment(problem, choices, ReplyPredictionSetup{}, board.combat != nullptr);
        const MasterAssignmentReplayResult & replay = result.replay;
        if (!replay.valid())
        {
            return refuse(result, ReplyPassNoValue::ReplayInvalid, replay.actorSlot);
        }
        if (!replay.complete())
        {
            return refuse(result, ReplyPassNoValue::ReplayIncomplete);
        }
        if (board.combat != nullptr)
        {
            removeIncomingCombinedFireConflict(result.replay, fireOrder);
            for (const CombatFireOrder extraOrder : extraFireOrders)
                removeIncomingCombinedFireConflict(result.replay, extraOrder);
        }
        if (!installCombatFireOrder(result.replay, fireOrder))
            return refuse(result, ReplyPassNoValue::ReplayInvalid);
        if (!extraFireOrders.empty())
            installCombatFireOrders(result.replay, extraFireOrders);
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
        if (board.combat && board.combat->policy == TacticalReply::WaitOnProperty)
            for (int i = 0; i < static_cast<int>(board.combat->units.size()); ++i)
                if (board.combat->units[i].enemy && board.combat->units[i].engineUnitId == board.combat->replyTarget)
                    result.enemyWaiting = i;
        const auto observed=board.combat?std::span<const ObservedReplyDefender>(board.combat->observedDefenders):std::span<const ObservedReplyDefender>{};
        const auto resolved=resolvePlanReply(actions,board,board.combat?&board.combat->ownFire:nullptr,observed);
        if (!resolved.complete)
        {
            const bool named = resolved.failedAction < result.actors.size();
            return refuse(result, noValueOf(resolved.own.status),
                          named ? result.actors[resolved.failedAction].actorSlot : NO_REPLY_PASS_ACTOR);
        }
        if (board.combat != nullptr)
        {
            for (const auto & entry : result.actors)
            {
                const auto & cand = problem.actors()[static_cast<std::size_t>(entry.actorSlot)]
                                        .candidates[static_cast<std::size_t>(entry.candidateIndex)];
                for (const std::int32_t reqKi : cand.requiredKilledKnowledgeIndices)
                {
                    const auto it = resolved.own.chain.chain.find(reqKi);
                    if (it == resolved.own.chain.chain.end() || it->second.after > 0)
                    {
                        return refuse(result, ReplyPassNoValue::ReplayInvalid, entry.actorSlot);
                    }
                }
            }
        }
        if (const std::optional<std::size_t> overflow = oneActionOverflowTarget(resolved))
        {
            return refuse(result, ReplyPassNoValue::Arithmetic, *overflow<result.actors.size()?result.actors[*overflow].actorSlot:NO_REPLY_PASS_ACTOR);
        }
        std::vector<int> counterWeapons;
        std::optional<PlanReplyRun> projected = board.combat
            ? combatPlanReply(resolved, actions, *board.combat, result.combatLocations, result.enemyReplyHp,
                              &result.encirclement, &result.realAfterReply, &result.lethalThreats, &result.captureChips,
                              &result.openingRecruitFight, &result.openingRecruitFiringSquares, &result.openingRecruitWindows, &result.servicedStockShots,&counterWeapons)
            : std::optional<PlanReplyRun>(runResolvedPlanReply(resolved));
        if (!projected) return refuse(result, ReplyPassNoValue::MovementLimit);
        const PlanReplyRun & run = *projected;
        if (board.combat)
            for (const auto &[index, target] : resolved.own.chain.chain)
                result.enemyAfterOwn[index] = target.after;
        result.pairsExamined = run.reply.pairsExamined;
        for (const OneActionShot &shot : run.reply.shots)
        {
            const int victim=shot.target<actions.size()?actions[shot.target].pCandidate->bundle.unitId:
                observed[shot.target-actions.size()].knowledgeIndex;
            const int counterWeapon=counterWeapons.empty()?-1:counterWeapons.at(result.shots.size());
            result.shots.push_back(ReplyPassShot{run.pool[shot.shooter], shot.target, shot.hpStepsBefore,
                                                 shot.hpStepsAfter, shot.counterCost, shot.firingTile,
                                                 shot.realBefore, shot.realAfter, shot.targetCapital,
                                                 board.combat ? victim : NO_UNIT,
                                                 board.combat ? resolved.targets[shot.target].target.unitId : NO_UNIT,
                                                 counterWeapon});
        }
        for (std::size_t index = 0; index < result.actors.size(); ++index)
        {
            ReplyPassActor &entry = result.actors[index];
            const CandidateBundle &candidate = *actions[index].pCandidate;
            entry.stepsAfterOwnActions = resolved.own.chain.outcomes[index].hpSteps;
            entry.stepsAfterReply = run.reply.outcomes[index].hpSteps;
            if (board.combat)
                result.realAfterReply[problem.actors()[entry.actorSlot].knowledgeUnitIndex] =
                    run.reply.outcomes[index].hp;
            entry.destroyedBy = entry.stepsAfterOwnActions <= 0 ? StockDeathCause::OwnAction :
                                entry.stepsAfterReply <= 0 ? StockDeathCause::Reply : StockDeathCause::None;
            const std::int32_t replyStart = ceilHpSteps(resolved.targets[index].target.hp);
            const MilliFunds cost = candidate.actor.replacementCost;
            auto acting = checkedActingSeatedCandidateValue(candidate, entry.grantedDamage);
            auto correction = ownCounterCorrection(candidate, entry.dayStartSteps - entry.stepsAfterOwnActions);
            if (!resolved.own.ownShots.empty() && resolved.own.ownShots[index].target != NO_UNIT)
            {
                entry.ownShot = resolved.own.ownShots[index];
                entry.grantedDamage = ceilHpSteps(entry.ownShot.targetBefore) - ceilHpSteps(entry.ownShot.targetAfter);
                const auto &fire = candidate.bundle.components.front().fire;
                entry.ownShot.targetCost = fire.targetReplacementCost;
                entry.ownShot.actorCost = cost;
                entry.ownShot.targetCapital = combatCapitalLoss(fire.targetReplacementCost, entry.ownShot.targetBefore,
                                                               entry.ownShot.targetAfter);
                entry.ownShot.counterCapital = combatCapitalLoss(cost, entry.ownShot.attackerBefore,
                                                                entry.ownShot.attackerAfter);
                const auto base = AssignmentValuationDetail::actingStaticValue(candidate);
                // Replace both isolated whole-step bookings; reservations are a capacity
                // check, never a second source of money for the same shot.
                acting = base ? AssignmentValuationDetail::checkedCapitalAdjustment(fire, *base, 0) : std::nullopt;
                correction = ownCounterCorrection(candidate, 0);
                if (!acting || !correction || !RelativeStockDetail::checkedAdd(*acting, entry.ownShot.targetCapital) ||
                    !RelativeStockDetail::checkedSubtract(*correction, entry.ownShot.counterCapital))
                    return refuse(result, ReplyPassNoValue::Arithmetic, entry.actorSlot);
            }
            const auto capital = board.combat && board.combat->ownFire ? std::optional<MilliFunds>{combatCapitalLoss(
                cost, resolved.targets[index].target.hp, run.reply.outcomes[index].hp)}
                : checkedReplyCapitalLoss(cost, replyStart, entry.stepsAfterReply);
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
                // The normal destination estimate is defined at isolated HP. A rescued actor needs a new quote.
                const std::int32_t basis = hpAfterAction(candidate.bundle, entry.dayStartSteps).hpSteps;
                if (basis <= 0)
                {
                    if (!candidate.replyFacts->survivorWeapon)
                        return refuse(result, ReplyPassNoValue::MissingNextShotBasis, entry.actorSlot);
                    const auto &weapon = *candidate.replyFacts->survivorWeapon;
                    if (board.combat && !board.combat->pay(weapon.work(board.width, board.height)))
                        return refuse(result, ReplyPassNoValue::MovementLimit, entry.actorSlot);
                    auto state = survivorReplyPosition(weapon, board, actions, resolved, run, index,
                                                       result.combatLocations, result.enemyReplyHp);
                    nextShot = survivorShotValue(weapon, state, candidate.bundle.destination, candidate.movementCost,
                                                 entry.stepsAfterReply);
                    entry.repricedSurvivor = true;
                    entry.survivorAmmo = state.ammo;
                }
                else
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
        for(std::size_t i=0;i<observed.size();++i){
            const auto &defender=observed[i];
            const auto target=actions.size()+i;
            const auto &after=run.reply.outcomes[target];
            const auto before=defender.facts->actorRealHp;
            result.realAfterReply[defender.knowledgeIndex]=after.hp;
            const int start=ceilHpSteps(before);
            const auto loss=board.combat->ownFire?std::optional<MilliFunds>{combatCapitalLoss(defender.cost,before,after.hp)}:
                checkedReplyCapitalLoss(defender.cost,start,after.hpSteps);
            if(!loss || !RelativeStockDetail::checkedSubtract(result.exactNonStock,*loss))
                return refuse(result,ReplyPassNoValue::Arithmetic);
            for(const auto &shot:result.shots)if(shot.target==target && !RelativeStockDetail::checkedAdd(result.exactNonStock,shot.counterCapital))
                return refuse(result,ReplyPassNoValue::Arithmetic);
            if(defender.stockRow!=NO_STOCK_ROW){
                // Observed capture carry already includes orders executed before this cut.
                const auto row=stockReplyRow(defender.stockRow,defender.tile,false,start,start,after.hpSteps);
                if(!row)return refuse(result,ReplyPassNoValue::KeyRejected);
                result.afterOwnActions.push_back(row->keys.afterOwnActions);result.afterReply.push_back(row->keys.afterReply);
            }
        }
        if(result.openingRecruitFight &&
           (!RelativeStockDetail::checkedAdd(result.exactNonStock,result.openingRecruitFight->stockCapitalLost) ||
            !RelativeStockDetail::checkedSubtract(result.exactNonStock,result.openingRecruitFight->recruitCapitalLost)))
            return refuse(result,ReplyPassNoValue::Arithmetic);
        std::sort(result.afterOwnActions.begin(), result.afterOwnActions.end());
        std::sort(result.afterReply.begin(), result.afterReply.end());
        result.reason = ReplyPassNoValue::None;
        return result;
    }
}