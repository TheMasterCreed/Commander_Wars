#include "ai/coordinator/guardednativecalendar.h"

#include <QtEndian>
#include <limits>
#include <string_view>

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t CALENDAR_CONTROL_WORK = 1024;
        constexpr std::int64_t WEATHER_ENTRY_WORK = 128 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH;
        constexpr std::int64_t FORECAST_DAY_WORK = 256;
        constexpr std::int64_t RULE_ENTRY_WORK = 256 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH;
        constexpr std::int64_t VARIABLE_ENTRY_WORK =
            128 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH + 4 * GUARDED_NATIVE_MAX_RULE_BYTES;
        constexpr std::int64_t VARIABLE_PAIR_WORK = 64 + 2 * GUARDED_NATIVE_MAX_ID_LENGTH;
        constexpr std::int64_t PHASE_WORK = 256;
        constexpr std::size_t NATIVE_FORECAST_TARGET = 4;
        constexpr std::int32_t NO_FOG = 0;
        constexpr std::string_view SUN_ID = "WEATHER_1SUN";
        constexpr std::string_view HQ_RULE_ID = "VICTORYRULE_NOHQ";
        constexpr std::string_view UNITS_RULE_ID = "VICTORYRULE_NOUNITS";
        constexpr std::array<std::string_view, 2> ACTIVE_IDS{"Active0", "Active1"};
        constexpr std::string_view IGNORE_WATERMINES_ID = "SpinboxValue1";

        bool boundedText(const QString & text)
        {
            return !text.isEmpty() && text.size() <= GUARDED_NATIVE_MAX_ID_LENGTH;
        }

        bool boundedRule(const GuardedNativeVictoryRule & rule, std::size_t ruleCount)
        {
            if (!boundedText(rule.id) || rule.variables.size() > guardedNativeRuleVariableLimit(rule.id, ruleCount))
            {
                return false;
            }
            for (std::size_t index = 0; index < rule.variables.size(); ++index)
            {
                const auto & variable = rule.variables[index];
                if (!boundedText(variable.id) || variable.data.size() > GUARDED_NATIVE_MAX_RULE_BYTES)
                {
                    return false;
                }
                for (std::size_t previous = 0; previous < index; ++previous)
                {
                    if (rule.variables[previous].id == variable.id)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        const GuardedNativeVariable* variableAt(
            const GuardedNativeVictoryRule & rule, std::string_view name)
        {
            for (const auto & variable : rule.variables)
            {
                if (variable.id == QLatin1StringView(name.data(), static_cast<qsizetype>(name.size())))
                {
                    return &variable;
                }
            }
            return nullptr;
        }

        bool activePlayers(const GuardedNativeVictoryRule & rule, std::array<bool, 2> & active)
        {
            for (std::size_t player = 0; player < active.size(); ++player)
            {
                const auto* variable = variableAt(rule, ACTIVE_IDS[player]);
                if (variable == nullptr || variable->data.size() != 1 || variable->data[0] != '\1')
                {
                    return false;
                }
                active[player] = true;
            }
            return true;
        }

        bool projectVictoryRules(const GuardedNativeCalendarBasis & basis, GuardedNativeCalendar & calendar)
        {
            bool hq = false;
            bool units = false;
            for (std::size_t index = 0; index < basis.victoryRules.size(); ++index)
            {
                const auto & rule = basis.victoryRules[index];
                if (!boundedRule(rule, basis.victoryRules.size()))
                {
                    return false;
                }
                if (rule.id == QLatin1StringView(HQ_RULE_ID.data(), HQ_RULE_ID.size()))
                {
                    if (hq || !activePlayers(rule, calendar.headquartersActive))
                    {
                        return false;
                    }
                    hq = true;
                }
                else if (rule.id == QLatin1StringView(UNITS_RULE_ID.data(), UNITS_RULE_ID.size()))
                {
                    if (units || !activePlayers(rule, calendar.unitsActive))
                    {
                        return false;
                    }
                    units = true;
                    const auto* value = variableAt(rule, IGNORE_WATERMINES_ID);
                    calendar.ignoreWaterminesVariableMissing = value == nullptr;
                    if (value != nullptr)
                    {
                        calendar.ignoreWatermines = value->data == QByteArrayView("\0\0\0\1", 4);
                        if (!calendar.ignoreWatermines && !value->data.isEmpty() && value->data != QByteArray(4, '\0'))
                        {
                            return false;
                        }
                    }
                }
                else if (rule.id == GUARDED_NATIVE_COMMAND_RULE && !calendar.commandRuleIndex &&
                         rule.variables.size() == GUARDED_NATIVE_COMMAND_VARIABLES)
                {
                    // Only saved integer settings. This excludes pending power and all other mod state.
                    for (const auto & variable : rule.variables)
                    {
                        constexpr QLatin1StringView prefix{"SpinboxValue"};
                        if (!variable.id.startsWith(prefix) || variable.data.size() != 4)
                        {
                            return false;
                        }
                        const auto suffix = QStringView(variable.id).sliced(prefix.size());
                        bool ok = false;
                        const auto ordinal = suffix.toInt(&ok);
                        if (!ok || ordinal < 0 || ordinal >= static_cast<int>(GUARDED_NATIVE_COMMAND_VARIABLES) ||
                            suffix != QString::number(ordinal))
                        {
                            return false;
                        }
                    }
                    const auto* enabled = variableAt(rule, "SpinboxValue0");
                    const auto* builder = variableAt(rule, "SpinboxValue1");
                    const auto* charge = variableAt(rule, "SpinboxValue2");
                    const auto* join = variableAt(rule, "SpinboxValue12");
                    // Map hashing canonicalizes these three stored spinboxes and marks them modified.
                    const auto canonical = [](const GuardedNativeVariable* variable, qint32 min, qint32 max) {
                        if (!variable || !variable->modified || variable->data.size() != 4)
                        {
                            return false;
                        }
                        const auto value = qFromBigEndian<qint32>(variable->data.constData());
                        return value >= min && value <= max;
                    };
                    if (!enabled || enabled->data != QByteArrayView("\0\0\0\1", 4) ||
                        !canonical(builder, 2, 2) || !canonical(charge, 1, 5) || !canonical(join, 1, 3))
                    {
                        return false;
                    }
                    calendar.commandRuleIndex = index;
                }
                else
                {
                    return false;
                }
            }
            // Mine exclusion needs the separately verified mine-free census and native rule receivers.
            return hq && units && (!calendar.ignoreWatermines || calendar.commandRuleIndex.has_value());
        }

        bool sunWeather(const GuardedNativeCalendarBasis & basis, std::int32_t index)
        {
            return index >= 0 && static_cast<std::size_t>(index) < basis.weatherIds.size() &&
                basis.weatherIds[index] == QLatin1StringView(SUN_ID.data(), SUN_ID.size());
        }

        bool boundedBasis(const GuardedNativeCalendarBasis & basis)
        {
            if (basis.day < 0 || basis.day > std::numeric_limits<std::int32_t>::max() -
                    GUARDED_STOCK_HORIZON_PAYMENTS || basis.player != 0 || basis.terminal ||
                basis.humanMatch || basis.randomWeather || basis.fog != NO_FOG ||
                basis.weatherIds.empty() || basis.weatherIds.size() > GUARDED_NATIVE_MAX_WEATHERS ||
                basis.forecast.empty() || basis.forecast.size() > GUARDED_NATIVE_MAX_FORECAST_DAYS ||
                (basis.victoryRules.size() != 2 && basis.victoryRules.size() != 3) ||
                !sunWeather(basis, basis.currentWeather) ||
                !sunWeather(basis, basis.startWeather) || !sunWeather(basis, basis.dayWeather))
            {
                return false;
            }
            for (const auto & id : basis.weatherIds)
            {
                if (!boundedText(id))
                {
                    return false;
                }
            }
            for (const auto & day : basis.forecast)
            {
                if (!sunWeather(basis, day[0]) || !sunWeather(basis, day[1]))
                {
                    return false;
                }
            }
            return basis.forecast[0][0] == basis.currentWeather;
        }

        void projectPhases(const GuardedNativeCalendarBasis & basis, GuardedNativeCalendar & calendar)
        {
            auto queue = basis.forecast;
            auto day = basis.day;
            std::array<std::int32_t, 2> payments{};
            for (std::size_t phase = 0; phase < calendar.phases.size(); ++phase)
            {
                const auto player = static_cast<std::int32_t>((phase + 1) % 2);
                const auto newDay = player == 0;
                if (newDay)
                {
                    ++day;
                    queue.erase(queue.begin());
                }
                while (queue.size() < NATIVE_FORECAST_TARGET)
                {
                    queue.push_back({basis.startWeather, basis.startWeather});
                }
                ++payments[player];
                calendar.phases[phase] = {
                    day, player, queue.front()[player], payments, newDay,
                    phase + 1 < GUARDED_NATIVE_FUTURE_PHASES};
            }
        }
    }

    std::size_t guardedNativeRuleVariableLimit(const QString & id, std::size_t ruleCount)
    {
        // Three expanded rules still fit the existing capture and duplicate-scan tariffs.
        static_assert(3 * GUARDED_NATIVE_COMMAND_VARIABLES <= GUARDED_NATIVE_MAX_VICTORY_RULES * GUARDED_NATIVE_MAX_RULE_VARIABLES);
        static_assert(3 * GUARDED_NATIVE_COMMAND_VARIABLES * GUARDED_NATIVE_COMMAND_VARIABLES <=
                      GUARDED_NATIVE_MAX_VICTORY_RULES * GUARDED_NATIVE_MAX_RULE_VARIABLES * GUARDED_NATIVE_MAX_RULE_VARIABLES);
        return ruleCount == 3 && id == GUARDED_NATIVE_COMMAND_RULE ? GUARDED_NATIVE_COMMAND_VARIABLES : GUARDED_NATIVE_MAX_RULE_VARIABLES;
    }

    std::int64_t guardedNativeCalendarCaptureWork()
    {
        return CALENDAR_CONTROL_WORK +
            GUARDED_NATIVE_MAX_WEATHERS * WEATHER_ENTRY_WORK +
            GUARDED_NATIVE_MAX_FORECAST_DAYS * FORECAST_DAY_WORK +
            GUARDED_NATIVE_MAX_VICTORY_RULES *
                (RULE_ENTRY_WORK + GUARDED_NATIVE_MAX_RULE_VARIABLES * VARIABLE_ENTRY_WORK);
    }

    std::int64_t guardedNativeCalendarProjectionWork()
    {
        return guardedNativeCalendarCaptureWork() +
            GUARDED_NATIVE_MAX_VICTORY_RULES * GUARDED_NATIVE_MAX_RULE_VARIABLES *
                GUARDED_NATIVE_MAX_RULE_VARIABLES * VARIABLE_PAIR_WORK +
            GUARDED_NATIVE_FUTURE_PHASES * PHASE_WORK;
    }

    GuardedNativeCalendarResult projectGuardedNativeCalendar(
        const GuardedNativeCalendarBasis & basis, RefinementLedger & ledger)
    {
        if (!ledger.consume(guardedNativeCalendarProjectionWork(), RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        if (!boundedBasis(basis))
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        GuardedNativeCalendar calendar;
        if (!projectVictoryRules(basis, calendar))
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        calendar.basis = basis;
        projectPhases(basis, calendar);
        return {GuardedNativeBindingStatus::Bound, std::move(calendar)};
    }
}
