#include <compare>
#include <cstdint>
#include <limits>

#include "ai/coordinator/terminalvalue.h"

namespace
{
    using Coordinator::TerminalClass;
    using Coordinator::TerminalValue;

    constexpr std::int64_t MINIMUM_ECONOMIC_VALUE = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t MAXIMUM_ECONOMIC_VALUE = std::numeric_limits<std::int64_t>::max();

    constexpr TerminalValue FORCED_LOSS_MINIMUM{TerminalClass::ForcedLoss, MINIMUM_ECONOMIC_VALUE};
    constexpr TerminalValue FORCED_LOSS_MAXIMUM{TerminalClass::ForcedLoss, MAXIMUM_ECONOMIC_VALUE};
    constexpr TerminalValue UNRESOLVED_MINIMUM{TerminalClass::Unresolved, MINIMUM_ECONOMIC_VALUE};
    constexpr TerminalValue UNRESOLVED_MAXIMUM{TerminalClass::Unresolved, MAXIMUM_ECONOMIC_VALUE};
    constexpr TerminalValue FORCED_WIN_MINIMUM{TerminalClass::ForcedWin, MINIMUM_ECONOMIC_VALUE};
    constexpr TerminalValue FORCED_WIN_MAXIMUM{TerminalClass::ForcedWin, MAXIMUM_ECONOMIC_VALUE};

    static_assert(TerminalValue{} == TerminalValue{TerminalClass::Unresolved, 0});
    static_assert(FORCED_WIN_MINIMUM > UNRESOLVED_MAXIMUM);
    static_assert(FORCED_WIN_MINIMUM > FORCED_LOSS_MAXIMUM);
    static_assert(UNRESOLVED_MINIMUM > FORCED_LOSS_MAXIMUM);
    static_assert(UNRESOLVED_MAXIMUM < FORCED_WIN_MINIMUM);
    static_assert(FORCED_LOSS_MAXIMUM < UNRESOLVED_MINIMUM);
    static_assert(FORCED_LOSS_MINIMUM < FORCED_LOSS_MAXIMUM);
    static_assert(UNRESOLVED_MINIMUM < UNRESOLVED_MAXIMUM);
    static_assert(FORCED_WIN_MINIMUM < FORCED_WIN_MAXIMUM);
    static_assert((FORCED_WIN_MINIMUM <=> UNRESOLVED_MAXIMUM) == std::strong_ordering::greater);
    static_assert((UNRESOLVED_MAXIMUM <=> UNRESOLVED_MAXIMUM) == std::strong_ordering::equal);
    static_assert(FORCED_WIN_MINIMUM > UNRESOLVED_MAXIMUM &&
                  UNRESOLVED_MAXIMUM > FORCED_LOSS_MAXIMUM &&
                  FORCED_WIN_MINIMUM > FORCED_LOSS_MAXIMUM);
}

int main()
{
    return 0;
}
