#pragma once

#include <cstdint>

namespace InfluenceContest
{
    constexpr float TOTAL_CONTROL_RATIO = 1.0f;
    constexpr float NEUTRAL_RATIO = 0.0f;

    // Positive when the enemy leads; every branch needs a positive divisor so no input yields inf.
    constexpr float getRatio(std::int32_t ownInfluence, std::int32_t enemyInfluence)
    {
        if (enemyInfluence <= 0)
        {
            return NEUTRAL_RATIO;
        }
        if (enemyInfluence > ownInfluence)
        {
            return TOTAL_CONTROL_RATIO - static_cast<float>(ownInfluence) / static_cast<float>(enemyInfluence);
        }
        if (ownInfluence > 0)
        {
            return -(TOTAL_CONTROL_RATIO - static_cast<float>(enemyInfluence) / static_cast<float>(ownInfluence));
        }
        return NEUTRAL_RATIO;
    }
}
