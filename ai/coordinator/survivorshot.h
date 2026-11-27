#pragma once

#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/mobilityfield.h"
#include <array>
#include "ai/coordinator/replyprediction.h"
#include <cmath>
#include <set>

namespace Coordinator
{
    struct SurvivorTarget
    {
        int index;
        TilePoint tile;
        int hp;
        MilliFunds cost;
        std::vector<std::array<double, 2>> baseByRange;

        int weaponAt(int distance, const std::array<int, 2> &ammo) const
        {
            if (distance < 0 || distance >= static_cast<int>(baseByRange.size()))
                return -1;
            const auto &base = baseByRange[distance];
            const double first = ammo[0] == 0 ? -1 : base[0];
            const double second = ammo[1] == 0 ? -1 : base[1];
            return first >= 0 && first >= second ? 0 : second >= 0 ? 1 : -1;
        }
    };

    struct SurvivorWeaponFacts
    {
        int movement{}, fuel{-1};
        std::array<int, 2> ammo{-1, -1};
        bool ignoresCollision{};
        std::optional<MobilityCostGrid> grid;
        std::vector<SurvivorTarget> targets;

        std::int64_t work(int width, int height) const
        {
            return static_cast<std::int64_t>(width) * height * (2 + static_cast<std::int64_t>(targets.size()));
        }
    };

    struct SurvivorPosition
    {
        std::array<int, 2> ammo{-1, -1};
        std::map<int, TilePoint> friendly;
        std::map<int, std::pair<TilePoint, int>> enemy;
    };

    inline SurvivorPosition survivorReplyPosition(const SurvivorWeaponFacts &weapon, const ReplyBoard &board,
                                                  std::span<const PlanReplyAction> actions,
                                                  const ResolvedPlanReply &resolved, const PlanReplyRun &run,
                                                  std::size_t actorIndex, const std::vector<TilePoint> &locations,
                                                  const std::map<int, int> &enemyHp)
    {
        SurvivorPosition state;
        state.ammo = weapon.ammo;
        const auto &actor = actions[actorIndex].pCandidate->bundle;
        auto spend = [&](int enemy, TilePoint shotFrom, bool ownFire) {
            auto it = std::find_if(weapon.targets.begin(), weapon.targets.end(),
                                   [enemy](const auto &t) { return t.index == enemy; });
            if (it == weapon.targets.end())
                return;
            const auto targetTile = ownFire ? it->tile : shotFrom;
            const int slot = it->weaponAt(manhattanDistance(actor.destination, targetTile), state.ammo);
            if (slot >= 0 && state.ammo[slot] > 0)
                --state.ammo[slot];
        };
        const auto *fire = singleComponentOf(actor);
        if (fire && fire->kind == ComponentKind::Fire)
            spend(fire->fire.targetUnitId, INVALID_TILE, true);
        for (const auto &unit : board.standing)
            state.friendly[unit.unitIndex] = unit.tile;
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            const auto &bundle = actions[i].pCandidate->bundle;
            state.friendly[bundle.unitId] = run.reply.outcomes[i].hpSteps > 0 ? bundle.destination : INVALID_TILE;
        }
        for (const auto &target : weapon.targets)
        {
            auto &end = state.enemy[target.index];
            end = {locations.empty() ? target.tile : locations.at(target.index), target.hp};
            if (const auto it = resolved.own.chain.chain.find(target.index); it != resolved.own.chain.chain.end())
                end.second = it->second.after;
            if (const auto it = enemyHp.find(target.index); it != enemyHp.end())
                end.second = it->second;
        }
        // Count a legal counter even if its damage rounded to zero monetary value.
        for (const auto &shot : run.reply.shots)
        {
            int id = run.pool[shot.shooter];
            const auto &reply = resolved.entries[shot.target];
            const auto &shooters = resolved.targets[shot.target].shooters;
            const auto slot =
                std::find_if(shooters.begin(), shooters.end(), [id](const auto &s) { return s.shooterIndex == id; });
            const auto index = static_cast<std::size_t>(slot - shooters.begin());
            const auto &source = reply.pFacts->tile->shooters[reply.shooterSlots[index]];
            const auto from = locations.empty() ? reply.firingTiles[index] : shot.firingTile;
            if (shot.target == actorIndex && shot.hpStepsAfter > 0 && drawsCounter(source.direct, from, actor.destination))
                spend(id, from, false);
            if (!locations.empty())
                continue;
            const int hp = oneActionShooterHpSteps(run.poolHp[shot.shooter], source.onOwnBuilding);
            const auto back = damageStepsFrom(hp, hpScaledBase(slot->counterRawBase, shot.hpStepsAfter));
            state.enemy[id] = {reply.firingTiles[index], hp - back};
        }
        return state;
    }

    // Like the normal next-shot term this is the best gross damage opportunity,
    // before assigning the next order.
    struct SurvivorShotChoice
    {
        int target{}, weapon{};
        TilePoint from{};
    };

    inline std::optional<MilliFunds> survivorShotAtEnds(const SurvivorWeaponFacts &weapon,const SurvivorPosition &state,
        std::span<const TilePoint> ends,int hp,std::optional<SurvivorShotChoice> *choice)
    {
        if(choice)choice->reset();
        MilliFunds best = 0;
        for (const auto &target : weapon.targets)
        {
            const auto &[tile, remaining] = state.enemy.at(target.index);
            if (tile == INVALID_TILE || remaining <= 0)
                continue;
            for (TilePoint from : ends)
            {
                const int d = std::abs(from.x - tile.x) + std::abs(from.y - tile.y);
                const int slot = target.weaponAt(d, state.ammo);
                if (slot < 0)
                    continue;
                const double base = target.baseByRange[d][slot];
                if (!std::isfinite(base) || remaining > UNIT_HP_STEPS || target.cost < 0 ||
                    target.cost > (std::numeric_limits<MilliFunds>::max() - UNIT_HP_STEPS / 2) / UNIT_HP_STEPS)
                    return {};
                const int dealt = damageStepsFrom(remaining, base * (static_cast<double>(hp) / UNIT_HP_STEPS));
                const auto amount=bookDamage(target.cost,dealt);
                if(amount>best)
                {
                    best=amount;
                    if(choice)*choice=SurvivorShotChoice{target.index,slot,from};
                }
            }
        }
        return best;
    }

    inline std::optional<MilliFunds> survivorShotValue(const SurvivorWeaponFacts &weapon, const SurvivorPosition &state,
                                                       TilePoint origin, int movementSpent, int hp,
                                                       std::optional<SurvivorShotChoice> *choice=nullptr)
    {
        if(choice)choice->reset();
        std::vector<TilePoint> blockers;
        std::set<std::pair<int, int>> occupied;
        for (const auto &[id, tile] : state.friendly)
            if (tile != INVALID_TILE && tile != origin)
                occupied.emplace(tile.x, tile.y);
        for (const auto &[id, target] : state.enemy)
            if (target.second > 0 && target.first != INVALID_TILE)
            {
                blockers.push_back(target.first);
                occupied.emplace(target.first.x, target.first.y);
            }
        const int movement =
            weapon.fuel < 0 ? weapon.movement : std::min(weapon.movement, std::max(0, weapon.fuel - movementSpent));
        std::vector<TilePoint> ends{origin};
        if (weapon.grid && movement > 0)
        {
            for (int y = std::max(0, origin.y - movement);
                 y <= std::min(weapon.grid->height() - 1, origin.y + movement); ++y)
                for (int x = std::max(0, origin.x - movement);
                     x <= std::min(weapon.grid->width() - 1, origin.x + movement); ++x)
                    if ((x != origin.x || y != origin.y) && !occupied.contains({x, y}))
                        ends.push_back({x, y});
            ActivationField reach;
            reach.build(weapon.ignoresCollision ? *weapon.grid : weapon.grid->withBlockedTiles(blockers), origin,
                        movement, 1, ends);
            std::erase_if(ends, [&](TilePoint t) { return t != origin && reach.activations(t.x, t.y) != 1; });
        }
        return survivorShotAtEnds(weapon,state,ends,hp,choice);
    }
} // namespace Coordinator
