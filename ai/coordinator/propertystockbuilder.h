#pragma once

#include <cstdint>
#include <span>

#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;

    PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                               std::span<const PropertyFacts> properties,
                                               MobilityFieldCache & mobility, std::int32_t horizonTurns);
}
