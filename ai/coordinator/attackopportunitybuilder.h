#pragma once

#include "ai/coordinator/attackopportunityfield.h"

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;

    // Spec grid pointers belong to the cache, so rebuild the field whenever the cache may have cleared.
    AttackOpportunityField buildAttackOpportunityField(GameMap & map, const BattlefieldKnowledge & knowledge, MobilityFieldCache & cache);
}
