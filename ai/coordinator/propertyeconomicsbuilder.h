#pragma once

#include <vector>

#include "ai/coordinator/propertyeconomics.h"

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;

    // Only known buildings are described, so fog keeps whatever the snapshot already hides out of the facts.
    std::vector<PropertyFacts> buildPropertyEconomics(GameMap & map, const BattlefieldKnowledge & knowledge);
}
