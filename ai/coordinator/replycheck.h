#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "ai/coordinator/replyprediction.h"

namespace Coordinator
{
    // The first layer where the planner's reply model and the danger audit's part, checked in this order.
    enum class ReplyCheckLayer : std::int8_t
    {
        Agrees,
        Entry,
        StartHp,
        EnemyHp,
        Facts,
        Outcome,
    };

    struct ReplyCheckShot
    {
        std::int32_t shooterIndex{NO_UNIT};
        TilePoint firingTile{INVALID_TILE};
        ResolvedReplyShot shot;
        MilliFunds cost{0};
        MilliFunds captureCost{0};
        bool onOwnBuilding{false};

        friend bool operator==(const ReplyCheckShot &, const ReplyCheckShot &) = default;
    };

    // Plain-data facts and their resolution for one unit, as either model lists them.
    struct ReplyCheckFacts
    {
        TerrainDefenseRule terrain;
        std::vector<ReplyCheckShot> shots;

        friend bool operator==(const ReplyCheckFacts &, const ReplyCheckFacts &) = default;
    };

    // One model's view of one unit; both sides' shooterHp cover the same shooters.
    struct ReplyCheckSide
    {
        double startHp{0};
        std::map<std::int32_t, std::int32_t> shooterHp;
        ReplyCheckFacts facts;
        ReplyOutcome outcome;
    };

    // Pool start hp steps in each model, before the own-building repair.
    struct ReplyHpGap
    {
        std::int32_t knowledge{NO_UNIT};
        std::int32_t plannerHp{0};
        std::int32_t auditHp{0};

        friend bool operator==(const ReplyHpGap &, const ReplyHpGap &) = default;
    };

    inline std::vector<ReplyHpGap> shooterHpGaps(const ReplyCheckSide & planner, const ReplyCheckSide & audit)
    {
        std::vector<ReplyHpGap> gaps;
        for (const auto & [knowledge, hp] : planner.shooterHp)
        {
            const auto other = audit.shooterHp.find(knowledge);
            if (other != audit.shooterHp.end() && other->second != hp)
            {
                gaps.push_back(ReplyHpGap{.knowledge = knowledge, .plannerHp = hp, .auditHp = other->second});
            }
        }
        return gaps;
    }

    inline ReplyCheckShot replyCheckShot(const ReplyShooterFacts & facts, const TilePoint & firingTile,
                                         const ResolvedReplyShot & shot)
    {
        return ReplyCheckShot{
            .shooterIndex = facts.shooterIndex,
            .firingTile = firingTile,
            .shot = shot,
            .cost = facts.cost,
            .captureCost = facts.captureCost,
            .onOwnBuilding = facts.onOwnBuilding,
        };
    }

    inline ReplyCheckFacts replyCheckFacts(const PlanReplyEntry & entry)
    {
        ReplyCheckFacts facts{.terrain = entry.pFacts->terrain};
        facts.shots.reserve(entry.shots.size());
        for (std::size_t index = 0; index < entry.shots.size(); ++index)
        {
            facts.shots.push_back(replyCheckShot(entry.pFacts->tile->shooters[entry.shooterSlots[index]],
                                                 entry.firingTiles[index], entry.shots[index]));
        }
        return facts;
    }

    // Facts are checked first, so a fact differing on a shooter P1 leaves silent is still reported.
    inline ReplyCheckLayer factsThenOutcome(const ReplyCheckFacts & planner, const ReplyCheckFacts & audit,
                                            const ReplyOutcome & plannerOutcome, const ReplyOutcome & auditOutcome)
    {
        if (!(planner == audit))
        {
            return ReplyCheckLayer::Facts;
        }
        if (!(plannerOutcome == auditOutcome))
        {
            return ReplyCheckLayer::Outcome;
        }
        return ReplyCheckLayer::Agrees;
    }

    // A null pAudit is a unit only the planner models.
    inline ReplyCheckLayer replyCheckLayer(const ReplyCheckSide & planner, const ReplyCheckSide* pAudit)
    {
        if (pAudit == nullptr)
        {
            return ReplyCheckLayer::Entry;
        }
        if (planner.startHp != pAudit->startHp)
        {
            return ReplyCheckLayer::StartHp;
        }
        if (planner.shooterHp != pAudit->shooterHp)
        {
            return ReplyCheckLayer::EnemyHp;
        }
        return factsThenOutcome(planner.facts, pAudit->facts, planner.outcome, pAudit->outcome);
    }

    // The replay's own guard: a live action caught in or behind a cycle is missing from the order.
    inline bool orderingAcyclic(const OrderingResult & ordering)
    {
        return ordering.cycleMembers.empty() && ordering.blockedByCycle.empty();
    }

    inline bool sameOccupancy(const PlannedOccupancy & left, const PlannedOccupancy & right, std::int32_t width,
                              std::int32_t height)
    {
        for (std::int32_t y = 0; y < height; ++y)
        {
            for (std::int32_t x = 0; x < width; ++x)
            {
                if (left.at(x, y) != right.at(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    }
}
