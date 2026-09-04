#pragma once

#include <cstdint>

#include <QString>

#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    struct PlanActionIds
    {
        QString wait;
        QString fire;
        QString capture;
    };

    struct KnownUnitLink
    {
        std::int32_t engineUnitId{NO_UNIT};
        TilePoint tile{INVALID_TILE};
    };

    PlanActionIds engineActionIds();
}
