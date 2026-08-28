#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <QPoint>
#include <QString>

#include "ai/coordinator/attackopportunityfield.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/candidatebundle.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/propertyeconomics.h"

class GameMap;
class Player;

namespace Coordinator
{
    class MobilityFieldCache;

    inline const QString TURN_LIMIT_VICTORY_RULE = QStringLiteral("VICTORYRULE_TURNLIMIT");
    constexpr std::int32_t TURN_LIMIT_RULE_ITEM = 0;
    // No firing origin in range of the target, so this shooter has no shot at all.
    constexpr std::int32_t NO_SHOT_DISTANCE = -1;

    enum class Side : std::int8_t
    {
        Ours,
        Theirs,
        Bystander,
    };

    constexpr Side sideOf(Relation relation)
    {
        switch (relation)
        {
            case Relation::Own:
            case Relation::Allied:
                return Side::Ours;
            case Relation::Enemy:
                return Side::Theirs;
            case Relation::Neutral:
                break;
        }
        return Side::Bystander;
    }

    constexpr Side opposingSide(Side side)
    {
        switch (side)
        {
            case Side::Ours:
                return Side::Theirs;
            case Side::Theirs:
                return Side::Ours;
            case Side::Bystander:
                break;
        }
        return Side::Bystander;
    }

    // The engine stores 0 for no limit, and the day equal to the limit is still playable.
    constexpr std::int32_t remainingTurnLimit(std::int32_t engineTurnLimit, std::int32_t currentDay)
    {
        if (engineTurnLimit <= 0)
        {
            return NO_TURN_LIMIT;
        }
        const std::int32_t remaining = engineTurnLimit - currentDay;
        if (remaining < 0)
        {
            return 0;
        }
        return remaining;
    }

    // 13.5 prices lost continuation as a difference of best shots, never as a share of one.
    constexpr MilliFunds lostShotContinuation(MilliFunds bestShotBefore, MilliFunds bestShotAfter)
    {
        return bestShotBefore - bestShotAfter;
    }

    // Ledger arithmetic, so a next shot and a booked kill price the same hp identically.
    constexpr MilliFunds bookDamage(MilliFunds replacementCost, std::int32_t damageSteps)
    {
        return DEFAULT_CAPITAL_POLICY.bookValue(replacementCost, damageSteps);
    }

    constexpr std::int32_t absoluteOffset(std::int32_t offset)
    {
        if (offset < 0)
        {
            return -offset;
        }
        return offset;
    }

    constexpr std::int32_t manhattanDistance(const TilePoint & from, const TilePoint & to)
    {
        return absoluteOffset(from.x - to.x) + absoluteOffset(from.y - to.y);
    }

    constexpr bool inFireRange(std::int32_t distance, std::int32_t minRange, std::int32_t maxRange)
    {
        return distance >= minRange && distance <= maxRange;
    }

    constexpr std::int32_t stationaryShotDistance(const TilePoint & from, const TilePoint & to,
                                                  std::int32_t minRange, std::int32_t maxRange)
    {
        const std::int32_t distance = manhattanDistance(from, to);
        if (inFireRange(distance, minRange, maxRange))
        {
            return distance;
        }
        return NO_SHOT_DISTANCE;
    }

    // Occupancy blind like the reach field it reads, so a real turn may find the firing tile taken.
    inline std::int32_t movedShotDistance(const DistanceField & reach, std::int32_t movementPoints,
                                          std::int32_t minRange, std::int32_t maxRange, const TilePoint & target)
    {
        for (std::int32_t distance = std::max(minRange, 0); distance <= maxRange; ++distance)
        {
            for (std::int32_t dy = -distance; dy <= distance; ++dy)
            {
                const std::int32_t dx = distance - absoluteOffset(dy);
                if (reach.reachable(target.x + dx, target.y + dy, movementPoints) ||
                    reach.reachable(target.x - dx, target.y + dy, movementPoints))
                {
                    return distance;
                }
            }
        }
        return NO_SHOT_DISTANCE;
    }

    // The current owner's rate stands in for both sides; owner modifiers are not re-derived.
    constexpr PropertyIncome captureIncome(const PropertyFacts & facts)
    {
        MilliFunds rate = toMilliFunds(facts.incomePerTurn);
        if (facts.ownerId == NO_OWNER)
        {
            rate = toMilliFunds(facts.baseIncome * facts.coveredTileCount);
        }
        return PropertyIncome{.oursPerTurn = rate, .enemyPerTurn = rate};
    }

    constexpr OwnerSign captureOwnerBefore(std::int32_t ownerId, Relation ownerRelation)
    {
        if (ownerId == NO_OWNER)
        {
            return OwnerSign::Neutral;
        }
        if (ownerRelation == Relation::Enemy)
        {
            return OwnerSign::Enemy;
        }
        return OwnerSign::Ours;
    }

    // The engine walks a movepath destination first; a bundle path reads origin first.
    inline std::vector<TilePoint> pathFromEngineOrder(const std::vector<QPoint> & enginePath)
    {
        std::vector<TilePoint> path;
        path.reserve(enginePath.size());
        for (auto step = enginePath.rbegin(); step != enginePath.rend(); ++step)
        {
            path.push_back(TilePoint{step->x(), step->y()});
        }
        return path;
    }

    inline std::vector<QPoint> pathToEngineOrder(const std::vector<TilePoint> & path)
    {
        std::vector<QPoint> enginePath;
        enginePath.reserve(path.size());
        for (auto step = path.rbegin(); step != path.rend(); ++step)
        {
            enginePath.push_back(QPoint(step->x, step->y));
        }
        return enginePath;
    }

    struct BundleBuildContext
    {
        GameMap & map;
        Player & player;
        const BattlefieldKnowledge & knowledge;
        const AttackOpportunityField & enemyReach;
        std::span<const PropertyFacts> properties;
        MobilityFieldCache & mobility;
        DamageOracle & oracle;
        ValuationContext valuation;
    };

    struct BundleBuildStats
    {
        std::int32_t candidateCount{0};
        std::int32_t invalidCount{0};
        // Knowledge entries the live map no longer answers for, reported rather than dropped.
        std::int32_t missingUnits{0};
        std::int32_t distanceFieldBuilds{0};
        std::int64_t oracleCalls{0};
        std::int64_t oracleHits{0};
    };

    // nullptr when the live tile no longer answers with the unit the snapshot recorded
    Unit* liveUnitFor(GameMap & map, const KnownUnit & known);
    std::int32_t resolveRemainingTurnLimit(GameMap & map);
    ValuationContext makeValuationContext(GameMap & map);
    std::vector<CandidateBundle> buildCandidateBundles(const BundleBuildContext & context, std::int32_t actorUnitIndex,
                                                      BundleBuildStats & stats);
}
