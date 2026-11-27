#pragma once

#include "ai/coordinator/combatforecast.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replypassevaluation.h"
#include "game/gamemap.h"
#include "game/terrain.h"
#include "game/unit.h"
#include <QJsonArray>
#include <QJsonObject>

namespace Coordinator
{
    // The live buyer acts after every friendly unit. Preserve the enemy's original
    // positions and use only damage already caused by the friendly opening volley.
    inline QJsonObject openingBuyerProjection(GameMap &map,const BattlefieldKnowledge &knowledge,
        const CombatTerrain &terrain,const MasterAssignmentProblem &problem,const PropertyStockField &field,
        const ReplyPassEvaluation &team,MilliFunds wallet,int available)
    {
        struct Position { TilePoint tile; double hp; int capture; };
        std::vector<Position> units;
        units.reserve(terrain.units.size());
        for (std::size_t i=0;i<terrain.units.size();++i)
        {
            const auto &pawn=terrain.units[i];
            const auto &unit=knowledge.units()[i];
            units.push_back({pawn.tile,pawn.realHp>=0 ? pawn.realHp : double(unit.hpRounded),unit.capturePoints});
        }
        std::map<std::pair<int,int>,int> changedOwners;
        for (const auto &actor:team.actors)
        {
            const auto &member=problem.actors()[actor.actorSlot];
            const int index=member.knowledgeUnitIndex;
            auto &state=units[index];
            const auto &unit=knowledge.units()[index];
            const auto &candidate=member.candidates[actor.candidateIndex];
            const auto start=state.tile;
            state.tile=team.replay.plan.action(actor.actionIndex).destination;
            if (actor.ownShot.target!=NO_UNIT)
            {
                state.hp=actor.ownShot.attackerAfter;
                units[actor.ownShot.target].hp=actor.ownShot.targetAfter;
            }
            if (candidate.capture)
            {
                auto *live=map.getTerrain(unit.x,unit.y)->getUnit();
                const int rate=live->getCaptureRate(QPoint(state.tile.x,state.tile.y));
                state.capture=(start==state.tile ? state.capture : 0)+rate;
                if (state.capture>=field.capturePointsToCapture())
                {
                    changedOwners[{state.tile.x,state.tile.y}]=unit.ownerId;
                    state.capture=0;
                }
            }
            else if (start!=state.tile) state.capture=0;
        }
        QJsonArray roster, properties;
        for (std::size_t i=0;i<units.size();++i)
        {
            const auto &pawn=terrain.units[i];
            if (!pawn.present || pawn.engineUnitId==NO_UNIT) continue;
            const auto &u=units[i];
            roster.append(QJsonObject{{"id",pawn.engineUnitId},{"x",u.tile.x},{"y",u.tile.y},
                {"hp",u.hp},{"owner",knowledge.units()[i].ownerId},{"capPoints",u.capture}});
        }
        for (const auto &b:knowledge.buildings())
        {
            const auto owner=changedOwners.find({b.x,b.y});
            properties.append(QJsonObject{{"x",b.x},{"y",b.y},
                {"owner",owner==changedOwners.end() ? b.ownerId : owner->second}});
        }
        return {{"units",roster},{"properties",properties},{"funds",static_cast<qint64>(wallet/MILLI_FUNDS_PER_FUND)},
                {"unitSlots",available}};
    }
}
