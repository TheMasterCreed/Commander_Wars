#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/masterassignmentproblem.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replypass.h"
#include "ai/coordinator/replypassevaluation.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace Coordinator
{
    inline constexpr std::int32_t OPEN_REFINEMENT_MIN_CONTESTED_CAPTURE_HP = UNIT_HP_STEPS / 2;
    inline constexpr std::int64_t OPEN_REFINEMENT_CONTESTED_LOSS_WEIGHT = 2;
    inline constexpr std::int64_t OPEN_REFINEMENT_STOCK_OFFER = 200'000;
    inline constexpr std::size_t OPEN_REFINEMENT_MAX_SAFE_MOVES = 12;
    inline constexpr std::int32_t OPEN_REFINEMENT_MAX_VETO_ROUNDS = 4;
    inline constexpr std::int32_t OPEN_REFINEMENT_ENEMY_REPAIR_STEPS = 2;

    inline MilliFunds openRefinementCaptureContinuation(
        const MasterCaptureConsequence & capture, std::int32_t horizonTurns)
    {
        if (capture.turnsUntilOwned == NO_CAPTURE_TURNS || capture.turnsUntilOwned >= horizonTurns)
            return 0;
        return incomeSwing(capture.income, horizonTurns - capture.turnsUntilOwned,
                           capture.ownerBefore, capture.ownerAfter);
    }

    struct OpenTacticalPolicySummary
    {
        bool valid{false};
        MilliFunds worstExactNonStock{std::numeric_limits<MilliFunds>::max()};
        MilliFunds worstRealizedMaterial{std::numeric_limits<MilliFunds>::max()};
        std::tuple<TerminalClass, int, int, MilliFunds> worstHqRank{
            TerminalClass::ForcedWin, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
            std::numeric_limits<MilliFunds>::max()};
        std::int32_t actorMinHpAfterReply{UNIT_HP_STEPS};
        bool actorEverDestroyed{false};
        MilliFunds actorWorstReplyLoss{0};
        std::set<int> killedAllies;
        int economicTargetKnowledge{NO_UNIT};
        double economicTargetAfterHp{-1.0};
    };

    inline bool openRefinementEnemyBuildingAt(const BattlefieldKnowledge & knowledge, TilePoint tile)
    {
        return std::ranges::any_of(knowledge.buildings(), [&](const KnownBuilding & b) {
            return b.x == tile.x && b.y == tile.y && knowledge.relation(b.ownerId) == Relation::Enemy;
        });
    }

    inline std::optional<MilliFunds> openRefinementRealizedMaterial(
        const ReplyPassEvaluation & eval, const CombatTerrain & local, const BattlefieldKnowledge & knowledge)
    {
        MilliFunds net = 0;
        for (const auto & actor : eval.actors)
        {
            if (actor.ownShot.target == NO_UNIT || isTerrainTargetId(actor.ownShot.target)) continue;
            MilliFunds targetGain = actor.ownShot.targetCapital;
            if (actor.ownShot.targetAfter > 0.0 &&
                actor.ownShot.target >= 0 && actor.ownShot.target < static_cast<int>(local.units.size()))
            {
                const auto & enemyPawn = local.units[static_cast<std::size_t>(actor.ownShot.target)];
                if (openRefinementEnemyBuildingAt(knowledge, enemyPawn.tile))
                {
                    // Surviving targets on enemy buildings heal at E0 start, undoing chip damage.
                    const MilliFunds repairOffset = (enemyPawn.replacementCost * OPEN_REFINEMENT_ENEMY_REPAIR_STEPS) / UNIT_HP_STEPS;
                    targetGain -= std::min(targetGain, repairOffset);
                }
            }
            if (!RelativeStockDetail::checkedAdd(net, targetGain) ||
                !RelativeStockDetail::checkedSubtract(net, actor.ownShot.counterCapital))
                return std::nullopt;
        }
        for (const auto & shot : eval.shots)
        {
            if (!RelativeStockDetail::checkedAdd(net, shot.counterCapital) ||
                !RelativeStockDetail::checkedSubtract(net, shot.targetCapital))
                return std::nullopt;
        }
        return net;
    }

    struct OpenCaptureInterruptInfo
    {
        bool capturesOwnHq{false};
        MilliFunds delayCredit{0};
    };

    inline OpenCaptureInterruptInfo openRefinementCaptureInterruptCredit(
        const BattlefieldKnowledge & knowledge, const PropertyStockField & field,
        std::span<const PropertyFacts> properties, int targetKnowledge, double targetHpAfterOwn)
    {
        OpenCaptureInterruptInfo info;
        if (targetKnowledge < 0 || targetKnowledge >= static_cast<int>(knowledge.units().size())) return info;
        const auto & unit = knowledge.units()[static_cast<std::size_t>(targetKnowledge)];
        if (!unit.canCapture || unit.capturePoints <= 0 || unit.captureRate <= 0) return info;
        const auto prop = std::ranges::find_if(properties, [&](const PropertyFacts & p) {
            return p.capturable && p.capturerIndex == targetKnowledge && p.capturePoints > 0;
        });
        if (prop == properties.end()) return info;
        if (prop->objective == ObjectiveKind::Headquarters && prop->ownerId == knowledge.observerId())
        {
            info.capturesOwnHq = true;
            return info;
        }
        const int slot = field.columnSlotAt({prop->x, prop->y});
        if (slot == NO_STOCK_COLUMN) return info;
        const auto col = mirroredColumn(field.enemyColumn(slot));
        const int threshold = field.capturePointsToCapture();
        const int horizon = field.horizonTurns();
        const int beforeTurns = static_cast<int>(widenedCaptureTurns(prop->capturePoints, unit.captureRate, threshold));
        const int afterSteps = targetHpAfterOwn >= 0.0 ? ceilHpSteps(targetHpAfterOwn) : unit.hpRounded;
        const int afterRate = replyCaptureRate(unit.captureRate, unit.hpRounded - afterSteps, afterSteps <= 0);
        int delayTurns = 0;
        if (afterRate <= 0)
            delayTurns = std::max(0, horizon - std::max(0, beforeTurns - 1));
        else
        {
            const int afterTurns = static_cast<int>(widenedCaptureTurns(prop->capturePoints, afterRate, threshold));
            delayTurns = std::max(0, afterTurns - beforeTurns);
        }
        if (delayTurns > 0)
        {
            const MilliFunds swing = incomeSwing(col.income, delayTurns, col.ownerBefore, OwnerSign::Enemy);
            info.delayCredit = swing < 0 ? -swing : swing;
        }
        return info;
    }

    template <class HqRankFn>
    inline OpenTacticalPolicySummary evaluateOpenTacticalPolicies(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        std::span<const int> choices, CombatTerrain & local,
        const ReplyBoard & actingBoard, int focusUnitId, std::int64_t replayTariff, std::int64_t & work,
        HqRankFn hqRankFn)
    {
        OpenTacticalPolicySummary summary;
        const auto priorPolicy = local.policy;
        const int priorTarget = local.replyTarget;
        const int priorShooter = local.containShooter;
        const auto priorFrom = local.containFrom;
        const auto restore = qScopeGuard([&] {
            local.policy = priorPolicy;
            local.replyTarget = priorTarget;
            local.containShooter = priorShooter;
            local.containFrom = priorFrom;
        });
        local.containShooter = NO_UNIT;
        local.containFrom = INVALID_TILE;

        std::vector<std::pair<TacticalReply, int>> policies{
            {TacticalReply::Economic, NO_UNIT},
            {TacticalReply::InterruptCapture, NO_UNIT},
            {TacticalReply::ArmorAndInfantry, NO_UNIT},
        };
        if (focusUnitId != NO_UNIT)
            policies.emplace_back(TacticalReply::TargetUnit, focusUnitId);

        bool hasAny = false;
        for (const auto & [policy, targetId] : policies)
        {
            if (work < replayTariff) return summary;
            work -= replayTariff;
            local.policy = policy;
            local.replyTarget = targetId;
            const auto eval = evaluateReplyPassPlan(problem, choices, actingBoard);
            if (!eval.valued() || !eval.replay.valid() || !eval.replay.complete()) return summary;
            const auto realized = openRefinementRealizedMaterial(eval, local, knowledge);
            if (!realized) return summary;
            const auto rank = hqRankFn(eval);
            if (!rank) return summary;
            if (!hasAny)
            {
                summary.worstExactNonStock = eval.exactNonStock;
                summary.worstRealizedMaterial = *realized;
                summary.worstHqRank = *rank;
                hasAny = true;
            }
            else
            {
                summary.worstExactNonStock = std::min(summary.worstExactNonStock, eval.exactNonStock);
                summary.worstRealizedMaterial = std::min(summary.worstRealizedMaterial, *realized);
                if (*rank < summary.worstHqRank) summary.worstHqRank = *rank;
            }
            for (const auto & actor : eval.actors)
            {
                if (actor.stepsAfterReply <= 0) summary.killedAllies.insert(actor.engineUnitId);
                if (focusUnitId != NO_UNIT && actor.engineUnitId == focusUnitId)
                {
                    summary.actorMinHpAfterReply = std::min(summary.actorMinHpAfterReply, actor.stepsAfterReply);
                    if (actor.stepsAfterReply <= 0) summary.actorEverDestroyed = true;
                    summary.actorWorstReplyLoss = std::max(summary.actorWorstReplyLoss, actor.terms.replyCapitalLoss);
                    if (policy == TacticalReply::Economic && actor.ownShot.target != NO_UNIT)
                    {
                        summary.economicTargetKnowledge = actor.ownShot.target;
                        summary.economicTargetAfterHp = actor.ownShot.targetAfter;
                    }
                }
            }
        }
        summary.valid = hasAny;
        return summary;
    }

    inline bool hasOpenTacticalRefinementWork(
        const MasterAssignmentProblem & problem, const BattlefieldKnowledge & knowledge,
        const ReplyPassEvaluation & reference, std::span<const int> choices,
        std::span<const int> stockProposed)
    {
        if (!reference.shots.empty()) return true;
        for (const auto & actorEval : reference.actors)
        {
            if (actorEval.ownShot.target != NO_UNIT &&
                actorEval.ownShot.counterCapital > actorEval.ownShot.targetCapital)
                return true;
        }
        for (const auto & actor : problem.actors())
        {
            const int slot = actor.canonicalSlot;
            if (slot < 0 || slot >= static_cast<int>(choices.size())) continue;
            const int current = choices[static_cast<std::size_t>(slot)];
            if (current < 0 || current >= static_cast<int>(actor.candidates.size())) continue;
            const auto & now = actor.candidates[static_cast<std::size_t>(current)];
            if (problem.canChangeActor(slot) &&
                actor.knowledgeUnitIndex >= 0 &&
                actor.knowledgeUnitIndex < static_cast<int>(knowledge.units().size()) &&
                knowledge.units()[static_cast<std::size_t>(actor.knowledgeUnitIndex)].capturePoints > 0)
            {
                if (!now.capture || now.destination != now.origin)
                {
                    const bool canFinish = std::ranges::any_of(actor.candidates, [&](const MasterCandidate & c) {
                        return c.plannable && c.capture && c.destination == now.origin && c.capture->turnsUntilOwned <= 0;
                    });
                    if (canFinish) return true;
                }
            }
            if (!stockProposed.empty() && slot < static_cast<int>(stockProposed.size()) &&
                problem.canChangeActor(slot) && stockProposed[static_cast<std::size_t>(slot)] != current)
            {
                const int propIdx = stockProposed[static_cast<std::size_t>(slot)];
                if (propIdx >= 0 && propIdx < static_cast<int>(actor.candidates.size()))
                {
                    const auto & proposed = actor.candidates[static_cast<std::size_t>(propIdx)];
                    const bool idleWait = now.actionKind == PlanBundleKind::Wait && now.destination == now.origin;
                    if (proposed.plannable && (proposed.capture.has_value() ||
                        (idleWait && proposed.actionKind == PlanBundleKind::Move)))
                        return true;
                }
            }
        }
        return false;
    }

    inline bool openRefinementDestinationTaken(
        const MasterAssignmentProblem & problem, std::span<const int> team, int skipSlot, TilePoint tile)
    {
        for (const auto & other : problem.actors())
        {
            const int s = other.canonicalSlot;
            if (s == skipSlot || s < 0 || s >= static_cast<int>(team.size())) continue;
            const int idx = team[static_cast<std::size_t>(s)];
            if (idx >= 0 && idx < static_cast<int>(other.candidates.size()) &&
                other.candidates[static_cast<std::size_t>(idx)].destination == tile)
                return true;
        }
        return false;
    }

    inline bool openRefinementNoNewAllyDeaths(
        const std::set<int> & beforeKilled, const std::set<int> & afterKilled, int changedUnitId,
        bool changedPreviouslyDied)
    {
        for (int uid : afterKilled)
        {
            if (uid == changedUnitId)
            {
                if (!changedPreviouslyDied) return false;
                continue;
            }
            if (!beforeKilled.contains(uid)) return false;
        }
        return true;
    }
}
