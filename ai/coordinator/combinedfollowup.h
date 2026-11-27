#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/supportcounterattack.h"

namespace Coordinator
{
    struct CombinedFollowupShot
    {
        int actor{NO_UNIT}, vehicle{NO_UNIT}, target{NO_UNIT};
        TilePoint end{INVALID_TILE};
        double targetAfter{}, actorAfter{};
        MilliFunds net{};
        bool countered{};
        int slot{-1}, counterSlot{-1};
        int movementCost{};
    };

    struct FollowupFrontier
    {
        // Decline preserves the actor's capture/guard/future-sale alternatives.
        // Nonpositive exchanges remain explicit for the downstream policy.
        std::vector<CombinedFollowupShot> shots;
        bool friendlyComplete{}, workExhausted{};
        void begin() { shots.clear(); friendlyComplete=false; workExhausted=false; }
    };

    // A vehicle has already fired at a committed endpoint. Compare one real capper
    // order from the resulting board; the caller prices that order against keeping
    // the same unit available to capture, hold or withdraw.
    inline std::optional<CombinedFollowupShot> findCombinedFollowup(const CombatTerrain &terrain,
        std::span<const KnownUnit> units, std::span<const TilePoint> positions, std::span<const int> health,
        std::span<const double> real, std::span<const int> ammo, std::span<const int> vehicleOnTarget,
        std::span<const int> movement, std::span<const MilliFunds> nextShot, std::int64_t &budget,
        std::span<const std::array<int,2>> loaded = {},
        const std::set<std::pair<int,int>> &friendlyBirths = {},
        FollowupFrontier *frontier = nullptr, bool *selectionComplete = nullptr)
    {
        if (frontier) frontier->begin();
        if (selectionComplete) *selectionComplete=false;
        const auto weapon = [&](int actor, int target, int distance) {
            const auto load = loaded.empty() ? std::array<int,2>{ammo[actor],0} : loaded[actor];
            return supportWeapon(terrain,actor,target,distance,load);
        };
        std::vector<int> remaining;
        std::vector<TilePoint> blockers;
        auto occupied = friendlyBirths;
        for (int j = 0; j < static_cast<int>(positions.size()); ++j)
        {
            if (positions[j] == INVALID_TILE || health[j] <= 0)
                continue;
            occupied.emplace(positions[j].x, positions[j].y);
            if (terrain.units[j].enemy)
            {
                blockers.push_back(positions[j]);
                if (vehicleOnTarget[j] != NO_UNIT)
                    remaining.push_back(j);
            }
        }
        if (remaining.empty()) {
            if (frontier) frontier->friendlyComplete=true;
            if (selectionComplete) *selectionComplete=true;
            return CombinedFollowupShot{};
        }
        std::vector<CombinedFollowupShot> offers;
        for (int i = 0; i < static_cast<int>(movement.size()); ++i)
        {
            const auto &pawn = terrain.units[i];
            const auto &u = units[i];
            if (movement[i] < 0 || !pawn.capper || !(u.canFire || (!loaded.empty() && pawn.hasNativeWeapon)) || health[i] <= 0 || positions[i] == INVALID_TILE)
                continue;
            const auto origin = positions[i];
            const bool mobile = u.canMoveAndFire && pawn.grid >= 0 && movement[i] > 0;
            std::vector<TilePoint> ends;
            for (int a : remaining)
                for (int y = std::max(0, positions[a].y-u.maxRange); y <= std::min(terrain.height-1, positions[a].y+u.maxRange); ++y)
                    for (int x = std::max(0, positions[a].x-u.maxRange); x <= std::min(terrain.width-1, positions[a].x+u.maxRange); ++x)
                    {
                        TilePoint t{x,y};
                        const int d = manhattanDistance(t, positions[a]);
                        if (d < u.minRange || d > u.maxRange || (t != origin && (!mobile || occupied.contains({x,y}))) ||
                            weapon(i, a, d).base <= 0) continue;
                        ends.push_back(t);
                    }
            std::sort(ends.begin(), ends.end(), [](TilePoint a, TilePoint b) { return std::tie(a.y,a.x) < std::tie(b.y,b.x); });
            ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
            if (ends.empty()) continue;
            const auto cost = (mobile ? 2LL*terrain.width*terrain.height : 0) +
                              static_cast<std::int64_t>(ends.size())*(1+remaining.size());
            if (cost > budget) {
                if (frontier) frontier->workExhausted=true;
                return {};
            }
            budget -= cost;
            ActivationField reach;
            if (mobile)
            {
                const auto &grid = terrain.grids[pawn.grid];
                reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(blockers), origin, movement[i], 1, ends);
            }
            for (auto end : ends)
            {
                if (end != origin && reach.activations(end.x,end.y) != 1) continue;
                for (int a : remaining)
                {
                    const int dist = manhattanDistance(end, positions[a]);
                    if (dist < u.minRange || dist > u.maxRange) continue;
                    const auto gun = weapon(i, a, dist);
                    const double power = gun.base;
                    if (power <= 0) continue;
                    const double rawPrimary=terrainReducedDamage(terrain.defense(a,positions[a]),power,health[i],health[a]);
                    const double damage=engineShotDamage(rawPrimary);
                    const double targetAfter = engineHpAfterReply(real[a], damage);
                    const int rem = ceilHpSteps(targetAfter);
                    const auto counterGun = rem > 0 && dist == 1 ? weapon(a,i,dist) : LoadedCombatWeapon{};
                    const double counter = counterGun.base;
                    const double back = counter > 0 ? engineShotDamage(sameActionCounterDamage(terrain.defense(i,end),
                                                                                   counter,real[a],rawPrimary,health[i])) : 0.;
                    const double actorAfter = engineHpAfterReply(real[i], back);
                    const auto net = terrain.ownFire ? combatCapitalLoss(terrain.units[a].replacementCost, real[a], targetAfter) -
                                     combatCapitalLoss(pawn.replacementCost, real[i], actorAfter) :
                                     bookDamage(terrain.units[a].replacementCost, health[a] - rem) -
                                     bookDamage(pawn.replacementCost, health[i] - ceilHpSteps(actorAfter));
                    // Replace an existing abstract opportunity with this physical
                    // comparison only when the supported hit is worth at least as much.
                    const CombinedFollowupShot shot{i, vehicleOnTarget[a], a, end, targetAfter,
                        actorAfter, net, counter >= 0, gun.slot, counterGun.slot,
                        end==origin?0:reach.routeCost(end.x,end.y)};
                    if (frontier) frontier->shots.push_back(shot);
                    if (net > 0 && net >= nextShot[i]) offers.push_back(shot);
                }
            }
        }
        if (frontier) {
            frontier->friendlyComplete=true;
            // The consumer keeps all distinct post-shot resources and ranks
            // the E1 options on the completed F1 roster and buyer. Do not spend
            // E1 as if the remaining F1 units had already acted.
            if (!selectionComplete) return CombinedFollowupShot{};
        }
        const auto priority = [&](const CombinedFollowupShot &s) {
            return std::tuple{s.targetAfter > 0, nextShot[s.actor] - s.net, s.end != positions[s.actor], s.actor,
                              s.target, s.end.y, s.end.x};
        };
        std::stable_sort(offers.begin(), offers.end(), [&](const auto &a, const auto &b) { return priority(a)<priority(b); });
        bool finished=true;
        CombinedFollowupShot best;
        MilliFunds bestMargin = std::numeric_limits<MilliFunds>::lowest();
        for (const auto &offer : offers)
        {
            // Select the bounded pair using a legal adverse reply, not a boolean danger radius.
            // The continuation later replays that reply with the real E1 action reservations.
            const auto work=4LL*positions.size();
            if(work>budget) { finished=false; if(best.actor==NO_UNIT) return {}; break; }
            budget-=work;
            std::vector<TilePoint> end(positions.begin(),positions.end());
            std::vector<int> steps(health.begin(),health.end());
            std::vector<double> after(real.begin(),real.end());
            std::vector<std::array<int,2>> load(loaded.begin(),loaded.end());
            if(load.empty()) for(int rounds:ammo) load.push_back({rounds,0});
            end[offer.actor]=offer.actorAfter>0 ? offer.end : INVALID_TILE;
            if(offer.targetAfter<=0) end[offer.target]=INVALID_TILE;
            after[offer.actor]=offer.actorAfter; after[offer.target]=offer.targetAfter;
            steps[offer.actor]=ceilHpSteps(offer.actorAfter); steps[offer.target]=ceilHpSteps(offer.targetAfter);
            spendCombatRound(load[offer.actor],offer.slot); spendCombatRound(load[offer.target],offer.counterSlot);
            auto reply=bestSupportCounterattack(terrain,units,end,steps,after,load,{},offer.actor,budget);
            if(!reply) { finished=false; if(best.actor==NO_UNIT) return {}; break; }
            const MilliFunds margin=offer.net-nextShot[offer.actor]-reply->gain();
            if(best.actor==NO_UNIT || margin>bestMargin) { best=offer; bestMargin=margin; }
        }
        if (selectionComplete) *selectionComplete=finished;
        return best;
    }
}
