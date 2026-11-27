#pragma once

namespace Coordinator
{
    struct TownWorkerGap { const char *reason; };
    inline thread_local bool forbidTownOuterState{};
    inline void requireTownOwnerState(const char *reason)
    {
        if(forbidTownOuterState)throw TownWorkerGap{reason};
    }
}
