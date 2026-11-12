#pragma once

#include <cstdint>

#include "ai/coordinator/capturedanger.h"
#include "ai/coordinator/propertystock.h"

namespace Coordinator
{
    // Walk turn 1 is today's capture, so delivery on walk turn t is the stock model's owned turn t - 1.
    constexpr MilliFunds captureWalkStream(const PropertyStockColumn & column, std::int32_t deliveredTurn,
                                           std::int32_t horizonTurns)
    {
        if (deliveredTurn == NO_DELIVERY_TURN)
        {
            return 0;
        }
        return columnStreamValue(column, deliveredTurn - 1, horizonTurns);
    }
}
