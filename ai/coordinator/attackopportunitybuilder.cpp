#include "ai/coordinator/attackopportunitybuilder.h"

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/mobilityfieldcache.h"

#include "game/gamemap.h"
#include "game/player.h"

namespace Coordinator
{
    AttackOpportunityField buildAttackOpportunityField(GameMap & map, const BattlefieldKnowledge & knowledge, MobilityFieldCache & cache)
    {
        std::vector<AttackerSpec> attackers;
        const std::vector<KnownUnit> & units = knowledge.units();
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            const KnownUnit & unit = units[i];
            if (knowledge.relation(unit.ownerId) != Relation::Enemy ||
                unit.carrier != KnownUnit::NO_CARRIER ||
                !unit.canFire)
            {
                continue;
            }
            Player* pOwner = map.getPlayer(unit.ownerId);
            if (pOwner == nullptr)
            {
                continue;
            }
            attackers.push_back(AttackerSpec{
                .unitIndex = static_cast<std::int32_t>(i),
                .x = unit.x,
                .y = unit.y,
                .movementPoints = unit.movementPoints,
                .minRange = unit.minRange,
                .maxRange = unit.maxRange,
                .canMoveAndFire = unit.canMoveAndFire,
                .grid = &cache.grid(map, *pOwner, unit.unitId),
            });
        }
        return AttackOpportunityField::build(knowledge.width(), knowledge.height(), attackers);
    }
}
