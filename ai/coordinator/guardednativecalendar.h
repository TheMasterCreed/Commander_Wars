#pragma once

#include <array>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include "ai/coordinator/guardednativeinventory.h"

namespace Coordinator
{
    constexpr std::size_t GUARDED_NATIVE_MAX_WEATHERS = 32;
    constexpr std::size_t GUARDED_NATIVE_MAX_FORECAST_DAYS = 32;
    constexpr std::size_t GUARDED_NATIVE_MAX_VICTORY_RULES = 8;
    constexpr std::size_t GUARDED_NATIVE_MAX_RULE_VARIABLES = 16;
    constexpr std::size_t GUARDED_NATIVE_COMMAND_VARIABLES = 19;
    inline constexpr QLatin1StringView GUARDED_NATIVE_COMMAND_RULE{"CMODE_CO_COMMAND"};
    constexpr qsizetype GUARDED_NATIVE_MAX_RULE_BYTES = 128;
    constexpr std::size_t GUARDED_NATIVE_FUTURE_PHASES = 2 * GUARDED_STOCK_HORIZON_PAYMENTS;

    struct GuardedNativeVariable
    {
        QString id;
        QByteArray data;
        bool modified{false};

        bool operator==(const GuardedNativeVariable &) const = default;
    };

    struct GuardedNativeVictoryRule
    {
        QString id;
        std::vector<GuardedNativeVariable> variables;

        bool operator==(const GuardedNativeVictoryRule &) const = default;
    };

    struct GuardedNativeCalendarBasis
    {
        std::int32_t day{0};
        std::int32_t player{-1};
        bool terminal{false};
        bool humanMatch{false};
        bool randomWeather{true};
        std::int32_t fog{-1};
        std::int32_t currentWeather{-1};
        std::int32_t startWeather{-1};
        std::int32_t dayWeather{-1};
        std::vector<QString> weatherIds;
        std::vector<std::array<std::int32_t, 2>> forecast;
        std::vector<GuardedNativeVictoryRule> victoryRules;

        bool operator==(const GuardedNativeCalendarBasis &) const = default;
    };

    struct GuardedNativeFuturePhase
    {
        std::int32_t day{0};
        std::int32_t player{-1};
        std::int32_t weather{-1};
        std::array<std::int32_t, 2> payments{};
        bool newDay{false};
        bool actionsWithinWindow{false};

        bool operator==(const GuardedNativeFuturePhase &) const = default;
    };

    // This is a potential nonterminal calendar, not callback or terminal proof authority.
    struct GuardedNativeCalendar
    {
        GuardedNativeCalendarBasis basis;
        std::array<GuardedNativeFuturePhase, GUARDED_NATIVE_FUTURE_PHASES> phases;
        std::array<bool, 2> headquartersActive{};
        std::array<bool, 2> unitsActive{};
        bool ignoreWatermines{false};
        bool ignoreWaterminesVariableMissing{false};
        std::optional<std::size_t> commandRuleIndex;

        bool operator==(const GuardedNativeCalendar &) const = default;
    };

    struct GuardedNativeCalendarBasisResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Invalid};
        std::optional<GuardedNativeCalendarBasis> basis;
    };

    struct GuardedNativeCalendarResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Invalid};
        std::optional<GuardedNativeCalendar> calendar;
    };

    std::int64_t guardedNativeCalendarCaptureWork();
    std::int64_t guardedNativeCalendarProjectionWork();
    std::size_t guardedNativeRuleVariableLimit(const QString & id, std::size_t ruleCount);
    GuardedNativeCalendarBasisResult captureGuardedNativeCalendar(GameMap &, RefinementLedger &);
    GuardedNativeCalendarResult projectGuardedNativeCalendar(
        const GuardedNativeCalendarBasis &, RefinementLedger &);
}
