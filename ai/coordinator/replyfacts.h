#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/capturedangerdecisions.h"
#include "ai/coordinator/capturedangerpricing.h"
#include "ai/coordinator/replypredictionmode.h"

namespace Coordinator
{
    constexpr double REPLY_TERRAIN_LUCK = averageLuck(ENGINE_DEFAULT_LUCK, NO_MISFORTUNE);

    // One reaching enemy at one target tile; nothing here depends on the defender.
    struct ReplyShooterFacts
    {
        std::int32_t slot{NO_ATTACKER_SLOT};
        std::int32_t shooterIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
        std::int32_t dayStartHpSteps{0};
        std::int32_t distance{NO_SHOT_DISTANCE};
        bool live{false};
        bool direct{false};
        bool onOwnBuilding{false};
        bool mustMove{false};
        // Direct shooters only, in cheapestFiringTile order; an indirect shooter fires from its tile.
        std::vector<TilePoint> firingTiles;
        MilliFunds cost{0};
        CaptureInterruption interruption{CaptureInterruption::None};
        MilliFunds captureCost{0};
        // Whether the in-place stream equals the enemy stock's own row weight; empty when nothing is priced.
        std::optional<bool> stockAgrees;
    };

    struct ReplyTileFacts
    {
        TilePoint target{INVALID_TILE};
        std::vector<ReplyShooterFacts> shooters;
    };

    struct ReplyShotBase
    {
        double rawBase{0};
        double withAmmo{0};
        double counterRawBase{NO_COUNTER};
    };

    // shots run parallel to tile->shooters.
    struct ReplyFacts
    {
        std::shared_ptr<const ReplyTileFacts> tile;
        TerrainDefenseRule terrain;
        double actorRealHp{0};
        std::vector<ReplyShotBase> shots;
    };

    struct ReplyBoard
    {
        std::int32_t width{0};
        std::int32_t height{0};
        std::vector<OccupantAt> standing;
        std::int32_t alliedOccupants{0};
    };

    struct ResolvedReplyShot
    {
        ShotStatus weapon{ShotStatus::NoLiveShooter};
        double rawBase{0};
        double base{0};
        double withAmmo{0};
        std::int32_t shooterHpSteps{0};
        PricedResolution resolution{PricedResolution::NoLiveShooter};
        std::int32_t pricedHpSteps{0};
        double counterRawBase{NO_COUNTER};

        friend bool operator==(const ResolvedReplyShot &, const ResolvedReplyShot &) = default;
    };

    // Every tile on the map, in range and reachable, ignoring occupancy, cheapest first.
    template <typename ReachCost>
    std::vector<TilePoint> orderedFiringTiles(const FiringRange & range, const PlannedOccupancy & emptyBoard,
                                              ReachCost && reachCost)
    {
        std::vector<std::pair<std::int32_t, TilePoint>> found;
        forEachFreeFiringTile(range, emptyBoard, reachCost, [&](const TilePoint & tile, std::int32_t cost)
        {
            found.emplace_back(cost, tile);
        });
        std::sort(found.begin(), found.end(), [](const auto & left, const auto & right)
        {
            return firingTileBeats(left.first, left.second, right.first, right.second);
        });
        std::vector<TilePoint> tiles;
        tiles.reserve(found.size());
        for (const auto & [cost, tile] : found)
        {
            tiles.push_back(tile);
        }
        return tiles;
    }

    // The first free entry is cheapestFiringTile, since (cost, y, x) orders distinct tiles strictly.
    inline TilePoint firstFreeFiringTile(std::span<const TilePoint> tiles, const PlannedOccupancy & occupancy)
    {
        for (const TilePoint & tile : tiles)
        {
            if (occupancy.at(tile.x, tile.y) == NO_UNIT)
            {
                return tile;
            }
        }
        return INVALID_TILE;
    }

    inline TilePoint replyFiringTile(const ReplyShooterFacts & facts, const PlannedOccupancy & occupancy)
    {
        return facts.direct ? firstFreeFiringTile(facts.firingTiles, occupancy) : facts.tile;
    }

    constexpr bool anyCounterTile(bool direct, std::span<const TilePoint> tiles, const TilePoint & target)
    {
        return std::any_of(tiles.begin(), tiles.end(), [&](const TilePoint & tile)
        {
            return drawsCounter(direct, tile, target);
        });
    }

    // forEachEnemyShot's own-victim override.
    constexpr std::int32_t replyShooterHpSteps(const ReplyShooterFacts & facts, std::int32_t victimIndex,
                                               std::int32_t victimHpSteps)
    {
        return facts.shooterIndex == victimIndex ? victimHpSteps : facts.dayStartHpSteps;
    }

    inline ResolvedReplyShot resolveReplyShot(const ReplyShooterFacts & facts, const ReplyShotBase & base,
                                              std::int32_t hpSteps, const TilePoint & firingTile,
                                              const TilePoint & target)
    {
        ResolvedReplyShot shot;
        shot.shooterHpSteps = hpSteps;
        shot.rawBase = base.rawBase;
        shot.base = hpScaledBase(base.rawBase, hpSteps);
        shot.withAmmo = base.withAmmo;
        if (drawsCounter(facts.direct, firingTile, target))
        {
            shot.counterRawBase = base.counterRawBase;
        }
        shot.weapon = shooterWeaponStatus(facts.live, shot.rawBase, shot.withAmmo,
                                          repairPending(hpSteps, facts.onOwnBuilding));
        shot.resolution = pricedResolution(shot.weapon, facts.onOwnBuilding, firingTile != INVALID_TILE);
        shot.pricedHpSteps = pricedShooterHpSteps(hpSteps, facts.onOwnBuilding);
        return shot;
    }

    inline double pricedTerrainDamage(const TerrainDefenseRule & rule, const ResolvedReplyShot & shot,
                                      std::int32_t shooterHpSteps, std::int32_t defenderHpSteps)
    {
        return terrainReducedDamage(rule, pricedRawBase(shot.resolution, shot.rawBase, shot.withAmmo), shooterHpSteps,
                                    defenderHpSteps);
    }

    inline ReplySlotFacts replySlotFacts(const ReplyShooterFacts & facts, const ResolvedReplyShot & shot)
    {
        return ReplySlotFacts{
            .shooterIndex = facts.shooterIndex,
            .onOwnBuilding = facts.onOwnBuilding,
            .resolution = shot.resolution,
            .counterRawBase = shot.counterRawBase,
            .shooterCost = facts.cost,
            .captureCost = facts.captureCost,
        };
    }
}
