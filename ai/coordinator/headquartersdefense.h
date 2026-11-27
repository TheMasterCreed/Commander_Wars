#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/combatforecast.h"

namespace Coordinator
{
    struct HeadquartersTerrainTurn
    {
        bool newDay{};
        bool hasStartOfTurn{};
    };

    // A stationary enemy capture consumes the first opposing action. Its
    // carry belongs to this actor on this square and its rate uses HP after
    // our actual orders, before any optional opposing shot or counter.
    inline bool completesStandingHeadquartersCapture(int afterOwnHp,int captureRate,int carry,
                                                     int threshold,bool gainedHeadquarters,
                                                     HeadquartersTerrainTurn terrainTurn={})
    {
        // On a new day the engine may run the terrain action before the
        // incoming player moves. It can alter this unit's actual capture rate.
        return !(terrainTurn.newDay && terrainTurn.hasStartOfTurn) &&
               !gainedHeadquarters && afterOwnHp>0 && captureRate>0 && threshold>0 &&
               static_cast<std::int64_t>(captureRate)+carry>=threshold;
    }

    enum class OpeningHqFinish { NoFinish, Unverified, Proved };

    inline OpeningHqFinish openingHqFinish(int hp,int rate,int carry,int threshold,bool gainedHq,
                                           HeadquartersTerrainTurn terrain,bool authenticated)
    {
        if(!completesStandingHeadquartersCapture(hp,rate,carry,threshold,gainedHq))
            return OpeningHqFinish::NoFinish;
        if(!authenticated || (terrain.newDay && terrain.hasStartOfTurn))
            return OpeningHqFinish::Unverified;
        return OpeningHqFinish::Proved;
    }

    struct HeadquartersVolley
    {
        struct Shot
        {
            int shooter;
            double guardAfter, shooterAfter;
            bool counter;
        };
        std::vector<Shot> shots;
        bool cleared{};
    };

    // Replay stationary E1 shots in a stable order. A capper firing in this
    // volley must also spend E1 in the capture calendar.
    inline std::optional<HeadquartersVolley> headquartersGuardVolley(
        const CombatTerrain &terrain, const BattlefieldKnowledge &knowledge,
        std::span<const TilePoint> positions, std::span<const int> hp,
        std::span<const double> real, std::span<const int> ammo, int guard, TilePoint home)
    {
        if (!terrain.weapon || !terrain.defense || !terrain.pay(1 + static_cast<std::int64_t>(positions.size())))
            return {};
        HeadquartersVolley volley;
        double remaining = real[guard];
        int guardAmmo = ammo[guard];
        const auto defense = terrain.defense(guard, home);
        for (std::size_t i = 0; i < positions.size() && remaining > 0; ++i)
        {
            const auto &unit = knowledge.units()[i];
            if (terrain.units[i].enemy == terrain.units[guard].enemy || positions[i] == INVALID_TILE || hp[i] <= 0 || ammo[i] <= 0 || !unit.canFire)
                continue;
            const int distance = manhattanDistance(positions[i], home);
            if (distance < unit.minRange || distance > unit.maxRange)
                continue;
            const double power = terrain.weapon(static_cast<int>(i), guard, distance);
            if (power > 0)
            {
                const auto damage = engineShotDamage(terrainReducedDamage(defense, power, hp[i], ceilHpSteps(remaining)));
                const auto after = engineHpAfterReply(remaining, damage);
                if (after >= remaining)
                    continue;
                remaining = after;
                const auto &defender = knowledge.units()[guard];
                const bool inCounterRange = distance == 1 && defender.minRange <= 1 && defender.maxRange >= 1;
                const double counterPower = remaining > 0 && guardAmmo > 0 && inCounterRange
                    ? terrain.weapon(guard, static_cast<int>(i), distance) : 0;
                const double back = counterPower > 0 ? engineShotDamage(terrainReducedDamage(
                    terrain.defense(static_cast<int>(i), positions[i]), counterPower, ceilHpSteps(remaining), hp[i])) : 0;
                if (back > 0)
                    --guardAmmo;
                volley.shots.push_back({static_cast<int>(i), remaining, engineHpAfterReply(real[i], back), back > 0});
            }
        }
        volley.cleared = remaining <= 0;
        return volley;
    }

    // Try one mobile capturer and one other unit. Their actual opportunity costs
    // are priced by the same continuation that reserves their F1 action.
    inline std::optional<std::vector<int>> headquartersReturnCandidates(
        const CombatTerrain &terrain, std::span<const TilePoint> positions, TilePoint home,
        std::span<const int> available)
    {
        std::vector<int> candidates(available.begin(), available.end());
        std::sort(candidates.begin(), candidates.end(), [&](int left, int right) {
            const auto priority = [&](int unit) {
                return std::pair{manhattanDistance(positions[unit], home), terrain.units[unit].engineUnitId};
            };
            return priority(left) < priority(right);
        });
        std::vector<TilePoint> blockers;
        for (std::size_t i = 0; i < positions.size(); ++i)
        {
            if (terrain.units[i].enemy && positions[i] != INVALID_TILE)
                blockers.push_back(positions[i]);
        }
        std::vector<int> guards;
        bool capper = false, other = false;
        ActivationField reach;
        for (const int i : candidates)
        {
            const auto &pawn = terrain.units[i];
            if ((pawn.capper ? capper : other) || pawn.grid < 0 || pawn.movement <= 0)
                continue;
            if (!terrain.pay(2LL * terrain.width * terrain.height))
                return {};
            const auto &grid = terrain.grids[pawn.grid];
            reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(blockers), positions[i],
                        pawn.movement, 1, {home});
            if (reach.activations(home.x, home.y) != 1)
                continue;
            guards.push_back(i);
            if (pawn.capper)
                capper = true;
            else
                other = true;
            if (capper && other)
                break;
        }
        return guards;
    }
}
