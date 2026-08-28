#pragma once

#include <compare>
#include <cstdint>

namespace Coordinator
{
    enum class TerminalClass : std::int8_t
    {
        ForcedLoss = -1,
        Unresolved = 0,
        ForcedWin = 1,
    };

    struct TerminalValue
    {
        TerminalClass terminal{TerminalClass::Unresolved};
        std::int64_t economicValue{0};

        friend constexpr auto operator<=>(const TerminalValue&, const TerminalValue&) = default;
    };
}
