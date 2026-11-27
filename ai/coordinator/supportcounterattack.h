#pragma once

#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/realcombatcapital.h"
#include "ai/coordinator/battlefieldknowledge.h"

namespace Coordinator
{
    struct SupportCounterattack
    {
        int shooter{NO_UNIT}, weapon{-1}, counterWeapon{-1};
        TilePoint firingTile{INVALID_TILE};
        double supporterAfter{}, shooterAfter{};
        MilliFunds damageValue{}, counterValue{};
        int movementCost{};
        MilliFunds gain() const { return damageValue - counterValue; }
    };

    // One E1 order on the same board used for guard/hold replies. An attacker
    // needs a free firing tile, a legal route and a compatible loaded weapon.
    inline std::optional<SupportCounterattack> bestSupportCounterattack(const CombatTerrain &terrain,
        std::span<const KnownUnit> units, std::span<const TilePoint> positions, std::span<const int> health,
        std::span<const double> real, std::span<const std::array<int,2>> load, const std::set<int> &spent,
        int supporter, std::int64_t &budget,
        std::vector<SupportCounterattack> *alternatives = nullptr,
        const std::set<std::pair<int,int>> &friendlyBirths = {})
    {
        SupportCounterattack best;
        if (health[supporter] <= 0 || positions[supporter] == INVALID_TILE) return best;
        const auto target = positions[supporter];
        std::vector<TilePoint> blocked;
        auto occupied=friendlyBirths;
        for (const auto &[x,y] : friendlyBirths) blocked.push_back({x,y});
        for (int i=0;i<static_cast<int>(positions.size());++i)
            if (health[i]>0 && positions[i]!=INVALID_TILE)
            {
                occupied.emplace(positions[i].x,positions[i].y);
                if (!terrain.units[i].enemy) blocked.push_back(positions[i]);
            }
        for (int a=0;a<static_cast<int>(positions.size());++a)
        {
            const auto &pawn=terrain.units[a];
            const auto &enemy=units[a];
            if (!pawn.enemy || spent.contains(a) || health[a]<=0 || positions[a]==INVALID_TILE ||
                (!enemy.canFire && !enemy.hasWeapons) || (load[a][0]==0 && load[a][1]==0)) continue;
            const auto origin=positions[a];
            // The exact E0 fuel charge is unavailable for a relocated enemy. Allow
            // only moves still affordable if it spent the full E0 allowance.
            const int fuel = enemy.fuel<0 ? pawn.movement : std::max(0,enemy.fuel-(origin!=pawn.tile ? pawn.movement : 0));
            const int movement=std::min(pawn.movement,fuel);
            const bool mobile=enemy.canMoveAndFire && pawn.grid>=0 && movement>0;
            const int radius=std::min<int>(enemy.maxRange,terrain.width+terrain.height);
            std::vector<TilePoint> ends;
            for (int y=std::max(0,target.y-radius);y<=std::min(terrain.height-1,target.y+radius);++y)
                for (int x=std::max(0,target.x-radius);x<=std::min(terrain.width-1,target.x+radius);++x)
                {
                    const TilePoint t{x,y};
                    const int d=manhattanDistance(t,target);
                    if(d<enemy.minRange || d>enemy.maxRange || (t!=origin && (!mobile || occupied.contains({x,y})))) continue;
                    if(supportWeapon(terrain,a,supporter,d,load[a]).base>0) ends.push_back(t);
                }
            if(ends.empty()) continue;
            const auto work=(mobile ? 2LL*terrain.width*terrain.height : 0)+static_cast<std::int64_t>(ends.size());
            if(work>budget) return {};
            budget-=work;
            ActivationField reach;
            if(mobile)
            {
                const auto &grid=terrain.grids[pawn.grid];
                reach.build(pawn.ignoresCollision ? grid : grid.withBlockedTiles(blocked),origin,movement,1,ends);
            }
            for(auto tile:ends)
            {
                if(tile!=origin && reach.activations(tile.x,tile.y)!=1) continue;
                const int d=manhattanDistance(tile,target);
                const auto gun=supportWeapon(terrain,a,supporter,d,load[a]);
                const auto damage=engineShotDamage(terrainReducedDamage(terrain.defense(supporter,target),gun.base,
                                                                       health[a],health[supporter]));
                if(damage<=0 && !alternatives) continue;
                const double after=engineHpAfterReply(real[supporter],damage);
                const int rem=ceilHpSteps(after);
                const auto reply = d==1 && rem>0 ? supportWeapon(terrain,supporter,a,d,load[supporter]) : LoadedCombatWeapon{};
                const auto back=reply.base>0 ? engineShotDamage(terrainReducedDamage(terrain.defense(a,tile),reply.base,
                                                                                     rem,health[a])) : 0.;
                const double surviving=engineHpAfterReply(real[a],back);
                const MilliFunds hurt=terrain.ownFire ? combatCapitalLoss(terrain.units[supporter].replacementCost,real[supporter],after)
                    : bookDamage(terrain.units[supporter].replacementCost,health[supporter]-rem);
                const MilliFunds cost=terrain.ownFire ? combatCapitalLoss(pawn.replacementCost,real[a],surviving)
                    : bookDamage(pawn.replacementCost,health[a]-ceilHpSteps(surviving));
                SupportCounterattack proposal{a,gun.slot,reply.slot,tile,after,surviving,hurt,cost};
                proposal.movementCost=tile==origin?0:reach.routeCost(tile.x,tile.y);
                if (alternatives) alternatives->push_back(proposal);
                if(proposal.gain()>best.gain() || (proposal.gain()==best.gain() && best.shooter!=NO_UNIT &&
                    std::tuple{tile!=origin,a,tile.y,tile.x} < std::tuple{best.firingTile!=positions[best.shooter],best.shooter,
                                                                         best.firingTile.y,best.firingTile.x}))
                    best=proposal;
            }
        }
        return best;
    }
}
