#pragma once

#include "ai/coordinator/supportcounterattack.h"

namespace Coordinator
{
    enum class ApproachBranch { Observe, Bypass, Shoot, Decline };

    struct CaptureApproach
    {
        int actor{NO_UNIT}, shooter{NO_UNIT};
        TilePoint origin{INVALID_TILE}, stage{INVALID_TILE}, target{INVALID_TILE}, gun{INVALID_TILE};
        int firstCost{}, rate{}, finish{};
    };

    struct ApproachStep
    {
        TilePoint tile{INVALID_TILE};
        int first{}, second{};
    };

    using ApproachStepKey = std::tuple<int,int,int,int,int,std::vector<int>>;
    using ApproachStepCache = std::map<ApproachStepKey,ApproachStep>;

    // First establish a compulsory stopping square even on empty terrain.
    // Then certify both actual legs against living hostile transit blockers.
    // An absent result means the comparison ran out of work.
    inline std::optional<ApproachStep> compulsoryApproachStep(const CombatTerrain &terrain,int grid,
        TilePoint from,TilePoint target,int movement,int fuel,std::vector<int> enemies,
        ApproachStepCache *cache,std::int64_t &budget)
    {
        if (grid<0 || movement<=0 || from==target || fuel<=0) return ApproachStep{};
        const int width=terrain.width, size=width*terrain.height;
        const auto cell=[&](TilePoint p){return p.y*width+p.x;};
        const auto key=ApproachStepKey{grid,cell(from),cell(target),movement,fuel,std::move(enemies)};
        if (cache)
            if (auto found=cache->find(key);found!=cache->end()) return found->second;
        auto finish=[&](ApproachStep step) {
            if (cache && cache->size()<8192) cache->emplace(key,step);
            return step;
        };
        if (budget<5LL*size) return {};
        budget-=5LL*size;
        const auto &costs=terrain.grids[grid];
        const auto first=DistanceField::build(costs,{from},FieldExpansion::FromSources);
        const auto second=DistanceField::build(costs,{target},FieldExpansion::ToTargets);
        if (first.reachable(target.x,target.y,movement)) return finish({});
        TilePoint stage=INVALID_TILE;
        for (int y=0;y<terrain.height;++y)
            for (int x=0;x<width;++x)
            {
                const auto a=first.distance(x,y), b=second.distance(x,y);
                if (a<0 || a>movement || b<0 || b>movement || b>fuel-a) continue;
                if (stage!=INVALID_TILE) return finish({});
                stage={x,y};
            }
        if (stage==INVALID_TILE) return finish({});
        std::vector<TilePoint> blocked;
        for (int pos:std::get<5>(key)) blocked.push_back({pos%width,pos/width});
        if (blocked.empty())
            return finish({stage,first.distance(stage.x,stage.y),second.distance(stage.x,stage.y)});
        if (budget<5LL*size) return {};
        budget-=5LL*size;
        const auto live=costs.withBlockedTiles(blocked);
        const auto leg1=DistanceField::build(live,{from},FieldExpansion::FromSources).distance(stage.x,stage.y);
        const auto leg2=DistanceField::build(live,{target},FieldExpansion::ToTargets).distance(stage.x,stage.y);
        if (leg1<0 || leg1>movement || leg2<0 || leg2>movement || leg2>fuel-leg1) return finish({});
        return finish({stage,leg1,leg2});
    }

    inline bool stationaryThreatAt(const CombatTerrain &terrain,std::span<const KnownUnit> units,
        int capper,TilePoint square,std::span<const TilePoint> positions,std::span<const int> hp,
        std::span<const std::array<int,2>> load,std::span<const int> lastShot={})
    {
        for (int i=0;i<static_cast<int>(units.size());++i)
        {
            if (!terrain.units[i].enemy || hp[i]<=0 || positions[i]==INVALID_TILE ||
                (!lastShot.empty() && lastShot[i]>=1)) continue;
            const auto &gun=units[i];
            const int range=manhattanDistance(positions[i],square);
            if ((gun.canFire || gun.hasWeapons) && range>=gun.minRange && range<=gun.maxRange &&
                supportWeapon(terrain,i,capper,range,load[i]).base>0) return true;
        }
        return false;
    }

    inline SupportCounterattack standingApproachShot(const CombatTerrain &terrain,
        std::span<const KnownUnit> units,const CaptureApproach &c,std::span<const int> hp,
        std::span<const double> real,std::span<const std::array<int,2>> load)
    {
        const int i=c.actor,a=c.shooter;
        const auto &foe=units[a];
        const int dist=manhattanDistance(c.gun,c.stage);
        if (hp[a]<=0 || hp[i]<=0 || dist<foe.minRange || dist>foe.maxRange ||
            (!foe.canFire && !foe.hasWeapons)) return {};
        const auto gun=supportWeapon(terrain,a,i,dist,load[a]);
        if (gun.base<=0) return {};
        const double rawPrimary=terrainReducedDamage(terrain.defense(i,c.stage),gun.base,hp[a],hp[i]);
        const double dealt=engineShotDamage(rawPrimary);
        const double left=engineHpAfterReply(real[i],dealt);
        if (left>=real[i]) return {};
        const int steps=ceilHpSteps(left);
        const auto &capper=units[i];
        const auto counter=steps>0 && dist==1 && capper.minRange<=1 && capper.maxRange>=1 ?
            supportWeapon(terrain,i,a,dist,load[i]) : LoadedCombatWeapon{};
        const double back=counter.base>0 ? engineShotDamage(sameActionCounterDamage(terrain.defense(a,c.gun),
            counter.base,real[i],rawPrimary,hp[a])) : 0.;
        const double after=engineHpAfterReply(real[a],back);
        return {a,gun.slot,counter.base>0 ? counter.slot : -1,c.gun,left,after,
                combatCapitalLoss(terrain.units[i].replacementCost,real[i],left),
                combatCapitalLoss(terrain.units[a].replacementCost,real[a],after)};
    }
}
