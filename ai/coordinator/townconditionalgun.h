#pragma once

#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    // These are reactions to declined stock proposals. No ammunition was
    // consumed. The original gun remains available to answer the same actor,
    // subject to any other proposal whose denial depended on that gun.
    struct TownGunClaim
    {
        int claimant;
        TilePoint target;
        int firstShot, through;
        bool incomplete;
        int entry{-1};
    };
}
