#pragma once

#include <array>
#include <cstdint>

namespace Coordinator
{
    enum class ReplyPassMode : std::int8_t
    {
        Off,
        Recorded,
    };

    // The switch values and the record's mode field share these spellings.
    inline constexpr std::array REPLY_PASS_MODE_NAMES{"off", "recorded"};
}
