#pragma once

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/unifiedproduction.h"
#include <span>

class GameMap;
class Player;

namespace Coordinator
{
    // Builds a live UnifiedProductionModel from the current board for any player count (2p, 3p, 4p, FFA, teams).
    UnifiedProductionModel buildLiveUnifiedProductionModel(
        GameMap & map, Player & player, const BattlefieldKnowledge & knowledge,
        MobilityFieldCache & mobilityFields, DamageOracle & oracle,
        std::span<const PropertyFacts> properties, const PropertyStockField & stockField,
        bool computeEnemyBlockDenial = true);
}
