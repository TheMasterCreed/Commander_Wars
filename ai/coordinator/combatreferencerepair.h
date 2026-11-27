#pragma once

#include "ai/coordinator/replypassevaluation.h"

namespace Coordinator
{
    struct CombatReferenceChange
    {
        int slot, unit, before, after, deadTarget;
        TilePoint beforeEnd, afterEnd;
    };

    struct CombatReferenceRepair
    {
        std::optional<ReplyPassEvaluation> evaluation;
        std::vector<int> choices;
        std::vector<CombatReferenceChange> changes;
        int attempts{};
        bool limited{};
    };

    // A projected kill can invalidate another pending shot from a structurally
    // legal master plan. Preserve the original handoff for publication, and
    // construct an executable reference using only actual generated orders.
    inline CombatReferenceRepair reconcileOverbookedCombatReference(const MasterAssignmentProblem &problem,
        const ReplyBoard &board, const ReplyPassEvaluation &original, const std::vector<int> &opening,
        std::int64_t &work)
    {
        CombatReferenceRepair repair;
        if (!original.replay.complete() || original.reason != ReplyPassNoValue::TargetAlreadyGone ||
            opening.size() != problem.actors().size())
            return repair;
        auto choices = opening;
        std::vector<CombatReferenceChange> changes;
        const auto count = static_cast<std::int64_t>(opening.size());
        const auto replayWork = 1 + count * (1 + count);
        // Repair is a starting witness for normal priced search. Prefer its
        // exact original endpoint, then its original square; replay checks
        // vacate edges, occupancy, committed actions and everyone else's shot.
        auto search = [&](auto &&self, const ReplyPassEvaluation &failed) -> bool {
            const int slot = failed.actorSlot;
            if (failed.reason != ReplyPassNoValue::TargetAlreadyGone || slot < 0 || slot >= count ||
                choices[slot] != opening[slot])
                return false;
            const auto &actor = problem.actors()[slot];
            if (!problem.canChangeActor(slot) && !problem.canChangeCombatHelper(actor))
                return false;
            const int before = choices[slot];
            const auto &shot = actor.candidates[before];
            if (shot.actionKind != PlanBundleKind::Fire && shot.actionKind != PlanBundleKind::MoveAndFire)
                return false;
            using Option = std::pair<std::tuple<int,int,int>,int>;
            std::vector<Option> options;
            for (int i = 0; i < static_cast<int>(actor.candidates.size()); ++i)
            {
                const auto &c = actor.candidates[i];
                const bool movement = c.actionKind == PlanBundleKind::Wait || c.actionKind == PlanBundleKind::Move;
                const bool capture = c.actionKind == PlanBundleKind::Capture || c.actionKind == PlanBundleKind::MoveAndCapture;
                if (!c.plannable || (!movement && !capture))
                    continue;
                options.push_back({{c.destination == shot.destination ? 0 : c.destination == shot.origin ? 1 : 2,
                                     movement ? 0 : 1, manhattanDistance(c.destination,shot.destination)},i});
            }
            std::sort(options.begin(), options.end());
            for (const auto &option : options)
            {
                if (work < replayWork || repair.attempts >= 2 * count + 8)
                {
                    repair.limited = true;
                    break;
                }
                ++repair.attempts;
                work -= replayWork;
                const int after = option.second;
                choices[slot] = after;
                changes.push_back({slot,actor.engineUnitId,before,after,shot.targetUnitId,
                                   shot.destination,actor.candidates[after].destination});
                auto evaluated = evaluateReplyPassPlan(problem,choices,board);
                if (evaluated.valued())
                {
                    repair.evaluation = std::move(evaluated);
                    repair.choices = choices;
                    repair.changes = changes;
                    return true;
                }
                if (self(self,evaluated)) return true;
                changes.pop_back();
                choices[slot] = before;
                if (repair.limited) break;
            }
            return false;
        };
        search(search,original);
        return repair;
    }
}
