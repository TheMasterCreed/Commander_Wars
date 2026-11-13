#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/bundlevaluation.h"
#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/ownershipschedule.h"

namespace Coordinator
{
    struct OccupantAt
    {
        std::int32_t unitIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
    };

    struct PlannedMove
    {
        std::int32_t unitIndex{NO_UNIT};
        TilePoint origin{INVALID_TILE};
        TilePoint destination{INVALID_TILE};
    };

    // Holds our and allied units after our plan's moves; enemy units are expected to vacate their tiles.
    class PlannedOccupancy
    {
    public:
        PlannedOccupancy(std::int32_t width, std::int32_t height, std::span<const OccupantAt> ours,
                         std::span<const PlannedMove> moves)
            : m_width(width),
              m_height(height),
              m_occupants(static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)),
                          NO_UNIT)
        {
            for (const OccupantAt & unit : ours)
            {
                set(unit.tile, unit.unitIndex);
            }
            for (const PlannedMove & move : moves)
            {
                if (at(move.origin.x, move.origin.y) == move.unitIndex)
                {
                    set(move.origin, NO_UNIT);
                }
            }
            for (const PlannedMove & move : moves)
            {
                set(move.destination, move.unitIndex);
            }
        }

        bool onMap(std::int32_t x, std::int32_t y) const
        {
            return x >= 0 && y >= 0 && x < m_width && y < m_height;
        }

        std::int32_t at(std::int32_t x, std::int32_t y) const
        {
            if (!onMap(x, y))
            {
                return NO_UNIT;
            }
            return m_occupants[slot(x, y)];
        }

        PlannedOccupancy withoutUnits(std::span<const std::int32_t> units) const
        {
            PlannedOccupancy result = *this;
            for (auto & occupant : result.m_occupants)
            {
                if (std::find(units.begin(), units.end(), occupant) != units.end())
                {
                    occupant = NO_UNIT;
                }
            }
            return result;
        }

    private:
        std::size_t slot(std::int32_t x, std::int32_t y) const
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        }

        void set(const TilePoint & tile, std::int32_t unitIndex)
        {
            if (onMap(tile.x, tile.y))
            {
                m_occupants[slot(tile.x, tile.y)] = unitIndex;
            }
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::vector<std::int32_t> m_occupants;
    };

    struct FiringRange
    {
        TilePoint target{INVALID_TILE};
        std::int32_t minRange{0};
        std::int32_t maxRange{0};
    };

    constexpr bool firingTileBeats(std::int32_t cost, const TilePoint & tile, std::int32_t bestCost, const TilePoint & best)
    {
        if (bestCost == UNREACHABLE)
        {
            return true;
        }
        if (cost != bestCost)
        {
            return cost < bestCost;
        }
        if (tile.y != best.y)
        {
            return tile.y < best.y;
        }
        return tile.x < best.x;
    }

    // reachCost(x, y) is the movement cost or UNREACHABLE; visit(tile, cost) sees every free reachable tile in range.
    template <typename ReachCost, typename Visit>
    void forEachFreeFiringTile(const FiringRange & range, const PlannedOccupancy & occupancy, ReachCost && reachCost,
                               Visit && visit)
    {
        for (std::int32_t y = range.target.y - range.maxRange; y <= range.target.y + range.maxRange; ++y)
        {
            for (std::int32_t x = range.target.x - range.maxRange; x <= range.target.x + range.maxRange; ++x)
            {
                const TilePoint tile{x, y};
                if (!occupancy.onMap(x, y) || occupancy.at(x, y) != NO_UNIT ||
                    !inFireRange(manhattanDistance(tile, range.target), range.minRange, range.maxRange))
                {
                    continue;
                }
                const std::int32_t cost = reachCost(x, y);
                if (cost != UNREACHABLE)
                {
                    visit(tile, cost);
                }
            }
        }
    }

    // Cheapest wins, then the lowest row, then the lowest column.
    template <typename ReachCost>
    TilePoint cheapestFiringTile(const FiringRange & range, const PlannedOccupancy & occupancy, ReachCost && reachCost)
    {
        TilePoint best = INVALID_TILE;
        std::int32_t bestCost = UNREACHABLE;
        forEachFreeFiringTile(range, occupancy, reachCost, [&](const TilePoint & tile, std::int32_t cost)
        {
            if (firingTileBeats(cost, tile, bestCost, best))
            {
                best = tile;
                bestCost = cost;
            }
        });
        return best;
    }

    inline std::int32_t distinctTileCount(std::vector<TilePoint> tiles)
    {
        const auto before = [](const TilePoint & left, const TilePoint & right)
        {
            return left.y != right.y ? left.y < right.y : left.x < right.x;
        };
        std::sort(tiles.begin(), tiles.end(), before);
        return static_cast<std::int32_t>(std::unique(tiles.begin(), tiles.end()) - tiles.begin());
    }

    // Allies stay where they stand and block a firing tile like our own units; cargo blocks nothing.
    constexpr bool blocksEnemyFiringTile(Relation relation, bool loaded)
    {
        return !loaded && (relation == Relation::Own || relation == Relation::Allied);
    }

    // Mirrors the capture candidate: only this actor's own progress on this property carries over.
    constexpr std::int32_t carriedCapturePoints(std::int32_t capturerIndex, std::int32_t capturePoints,
                                                std::int32_t actorIndex)
    {
        return capturerIndex == actorIndex ? capturePoints : 0;
    }

    constexpr bool shooterMustMove(const TilePoint & shooter, const TilePoint & target, std::int32_t minRange,
                                   std::int32_t maxRange)
    {
        return stationaryShotDistance(shooter, target, minRange, maxRange) == NO_SHOT_DISTANCE;
    }

    constexpr bool shooterCapturingHere(std::int32_t capturerIndex, std::int32_t capturePoints, std::int32_t shooterIndex)
    {
        return capturerIndex == shooterIndex && capturePoints > 0;
    }

    // From the shooter's side only our and neutral properties are surely its to capture.
    constexpr bool shooterCouldCaptureInPlace(bool canCapture, bool capturable, OwnerSign ownerFromOurSide)
    {
        return canCapture && capturable && ownerFromOurSide != OwnerSign::Enemy;
    }

    // Without its candidate only a kind that leaves the actor's hp alone has a known start for the reply.
    constexpr bool replyStartKnown(bool candidateMatched, PlanBundleKind kind)
    {
        if (candidateMatched)
        {
            return true;
        }
        switch (kind)
        {
            case PlanBundleKind::Wait:
            case PlanBundleKind::Move:
            case PlanBundleKind::Capture:
            case PlanBundleKind::MoveAndCapture:
                return true;
            case PlanBundleKind::Fire:
            case PlanBundleKind::MoveAndFire:
            case PlanBundleKind::Service:
            case PlanBundleKind::MoveAndService:
            case PlanBundleKind::Compound:
                break;
        }
        return false;
    }

    // The engine counters only at melee range, so the firing tile, not the reach, decides.
    constexpr bool drawsCounter(bool direct, const TilePoint & firingTile, const TilePoint & target)
    {
        return direct && firingTile != INVALID_TILE && manhattanDistance(firingTile, target) == 1;
    }

    struct PlanWideHp
    {
        std::int32_t before{0};
        std::int32_t after{0};

        friend bool operator==(const PlanWideHp &, const PlanWideHp &) = default;
    };

    // Keyed by target knowledge index; one chain per bundle is the isolated view, one per plan the plan view.
    using TargetHpChain = std::map<std::int32_t, PlanWideHp>;

    // A shooter the plan kills is out; one it only damages is repaired on its own building from the reduced hp.
    constexpr std::int32_t oneActionShooterHpSteps(std::int32_t planHpSteps, bool onOwnBuilding)
    {
        return planHpSteps <= 0 ? 0 : pricedShooterHpSteps(planHpSteps, onOwnBuilding);
    }

    enum class ActionCounterSource : std::int8_t
    {
        Isolated,
        Repriced,
        Unavailable,
    };

    struct ActionCounterDamage
    {
        std::int32_t steps{0};
        ActionCounterSource source{ActionCounterSource::Unavailable};
    };

    inline ActionCounterDamage isolatedActionCounter(const FireFacts & fire, std::int32_t, std::int32_t)
    {
        return {fire.counterSteps, ActionCounterSource::Isolated};
    }

    // victimIndex is the last fire target, NO_UNIT without one; its hp after our shots is what it replies with.
    struct ActionHpOutcome
    {
        std::int32_t hpSteps{0};
        bool counterApplied{false};
        std::int32_t victimIndex{NO_UNIT};
        std::int32_t victimHpSteps{0};
        // A counter or a self service changed the hp in whole steps, so the real hp is known only as those steps.
        bool realHpFromSteps{false};
        // The target survives weaker than the candidate assumed, so the counter steps are the isolated overestimate.
        bool counterFromIsolatedView{false};
    };

    // valueBundle's hp walk on a target chain the caller may share across bundles; booking stops at death.
    template <typename CounterAt>
    ActionHpOutcome hpAfterActionOn(const ActionBundle & bundle, std::int32_t actorHpSteps, TargetHpChain & chain,
                                    CounterAt && counterAt)
    {
        ActionHpOutcome outcome{.hpSteps = actorHpSteps};
        for (const BundleComponent & component : bundle.components)
        {
            if (outcome.hpSteps <= 0)
            {
                break;
            }
            switch (component.kind)
            {
                case ComponentKind::Fire:
                {
                    const FireFacts & fire = component.fire;
                    PlanWideHp & target =
                        chain.emplace(fire.targetUnitId, PlanWideHp{fire.targetHpSteps, fire.targetHpSteps}).first->second;
                    const std::int32_t dealt = std::min(fire.damageSteps, target.after);
                    const auto counter = counterAt(fire, target.after - dealt, outcome.hpSteps);
                    if (dealt < target.after)
                    {
                        const std::int32_t taken = std::min(counter.steps, outcome.hpSteps);
                        outcome.hpSteps -= taken;
                        outcome.counterApplied = outcome.counterApplied || taken > 0;
                        outcome.realHpFromSteps = outcome.realHpFromSteps || taken > 0;
                        const std::int32_t isolatedAfter = fire.targetHpSteps - std::min(fire.damageSteps, fire.targetHpSteps);
                        outcome.counterFromIsolatedView =
                            outcome.counterFromIsolatedView || (counter.source == ActionCounterSource::Isolated &&
                                taken > 0 && target.after - dealt < isolatedAfter);
                    }
                    target.after -= dealt;
                    outcome.victimIndex = fire.targetUnitId;
                    outcome.victimHpSteps = target.after;
                    break;
                }
                case ComponentKind::Capture:
                    break;
                case ComponentKind::Service:
                    if (component.service.partnerUnitId == bundle.unitId &&
                        component.service.partnerHpStepsAfter != outcome.hpSteps)
                    {
                        outcome.hpSteps = component.service.partnerHpStepsAfter;
                        outcome.realHpFromSteps = true;
                    }
                    break;
            }
        }
        return outcome;
    }

    inline ActionHpOutcome hpAfterActionOn(const ActionBundle & bundle, std::int32_t actorHpSteps, TargetHpChain & chain)
    {
        return hpAfterActionOn(bundle, actorHpSteps, chain, isolatedActionCounter);
    }

    // The per-candidate view the planner prices: the bundle alone, from each target's start-of-day hp.
    inline ActionHpOutcome hpAfterAction(const ActionBundle & bundle, std::int32_t actorHpSteps)
    {
        TargetHpChain chain;
        return hpAfterActionOn(bundle, actorHpSteps, chain);
    }

    // The step value is the top of the real interval, so the record marks it approximate.
    constexpr double realHpAfterAction(double realHpBefore, const ActionHpOutcome & outcome)
    {
        return outcome.realHpFromSteps ? static_cast<double>(outcome.hpSteps) : realHpBefore;
    }
}
