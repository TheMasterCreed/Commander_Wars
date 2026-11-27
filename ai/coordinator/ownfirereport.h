#pragma once

#include "ai/coordinator/coordinatorcommon.h"
#include <array>
#include <functional>

namespace Coordinator
{
    // Damage is in the same tenths-of-HP units as ACTION_FIRE. Negative values
    // denote an unavailable shot or counter; keep overkill until applying the hit.
    struct OwnFireReport
    {
        double damage{-1}, counter{-1};
        int weapon{-1}, counterWeapon{-1};
        bool operator==(const OwnFireReport &) const = default;
    };
    using OwnFireAt = std::function<OwnFireReport(int, TilePoint, int, double, double, std::array<int, 2>)>;
}
