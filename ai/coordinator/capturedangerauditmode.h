#pragma once

#include <cstdint>

namespace Coordinator
{
    enum class CaptureDangerAudit : std::int8_t
    {
        Off,
        Recorded,
    };
}
