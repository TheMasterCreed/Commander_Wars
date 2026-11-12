#pragma once

#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "ai/coordinator/guardednativecalendar.h"

class QJSEngine;

namespace Coordinator
{
    class GuardedNativeCodeCatalog;
    struct PlannedAction;

    enum class GuardedNativeAnimationTerrain : std::int8_t
    {
        Plains,
        Forest,
        Mountain,
        Street,
        Property,
    };

    constexpr std::size_t GUARDED_NATIVE_MAX_ANIMATION_VARIABLES = 16;
    constexpr std::int32_t GUARDED_NATIVE_BUILDING_ANIMATION_ARMIES = 10;
    inline constexpr std::string_view GUARDED_NATIVE_SETTINGS_ALIAS{"settings"};
    inline const QString GUARDED_NATIVE_ANIMATION_FOREGROUND = QStringLiteral("FOREGROUND_ID");
    inline const QString GUARDED_NATIVE_ANIMATION_BACKGROUND = QStringLiteral("BACKGROUND_ID");
    inline const QString GUARDED_NATIVE_ANIMATION_ARMY = QStringLiteral("ARMYBACKGROUND_ID");

    constexpr std::int64_t GUARDED_NATIVE_ANIMATION_ADMISSION_CONTROL_WORK = 128;

    constexpr std::int64_t guardedNativeAnimationEntryWork(std::size_t count)
    {
        return 256 + 8 * GUARDED_NATIVE_MAX_RULE_BYTES +
            4 * GUARDED_NATIVE_MAX_ID_LENGTH * static_cast<std::int64_t>(count + 1);
    }

    constexpr std::int64_t guardedNativeAnimationAdmissionWork(std::size_t count)
    {
        return GUARDED_NATIVE_ANIMATION_ADMISSION_CONTROL_WORK +
            guardedNativeAnimationEntryWork(count) * static_cast<std::int64_t>(count);
    }

    inline GuardedNativeBindingStatus admitGuardedNativeAnimationVariables(
        GuardedNativeAnimationTerrain terrain, std::span<const GuardedNativeVariable> variables,
        RefinementLedger & ledger)
    {
        if (!ledger.consume(GUARDED_NATIVE_ANIMATION_ADMISSION_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (variables.size() > GUARDED_NATIVE_MAX_ANIMATION_VARIABLES)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        switch (terrain)
        {
            case GuardedNativeAnimationTerrain::Plains:
            case GuardedNativeAnimationTerrain::Forest:
            case GuardedNativeAnimationTerrain::Mountain:
            case GuardedNativeAnimationTerrain::Street:
            case GuardedNativeAnimationTerrain::Property:
                break;
            default:
                return GuardedNativeBindingStatus::Unsupported;
        }
        if (!ledger.consume(guardedNativeAnimationEntryWork(variables.size()) * std::ssize(variables),
                            RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        std::optional<std::int32_t> foreground;
        std::optional<std::int32_t> background;
        std::optional<std::int32_t> army;
        for (std::size_t index = 0; index < variables.size(); ++index)
        {
            const auto & variable = variables[index];
            if (variable.id.isEmpty() || variable.id.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                variable.data.size() > GUARDED_NATIVE_MAX_RULE_BYTES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            for (std::size_t prior = 0; prior < index; ++prior)
            {
                if (variables[prior].id == variable.id)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
            }
            std::optional<std::int32_t>* destination = nullptr;
            if (variable.id == GUARDED_NATIVE_ANIMATION_FOREGROUND)
            {
                destination = &foreground;
            }
            else if (variable.id == GUARDED_NATIVE_ANIMATION_BACKGROUND)
            {
                destination = &background;
            }
            else if (variable.id == GUARDED_NATIVE_ANIMATION_ARMY)
            {
                destination = &army;
            }
            if (destination != nullptr)
            {
                if (variable.data.size() != sizeof(std::int32_t))
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                const auto* bytes = reinterpret_cast<const unsigned char*>(variable.data.constData());
                const auto value = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                    (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
                if (value > std::numeric_limits<std::int32_t>::max())
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                *destination = static_cast<std::int32_t>(value);
            }
        }
        const auto foregroundMaximum = terrain == GuardedNativeAnimationTerrain::Street ? 1 : 3;
        if (foreground && *foreground > foregroundMaximum)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        const auto backgroundMaximum = terrain == GuardedNativeAnimationTerrain::Forest ||
            terrain == GuardedNativeAnimationTerrain::Street ? 2 : 1;
        if (background && *background > backgroundMaximum)
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        if (terrain == GuardedNativeAnimationTerrain::Property &&
            (background.has_value() != army.has_value() ||
             (army && *army >= GUARDED_NATIVE_BUILDING_ANIMATION_ARMIES)))
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        return GuardedNativeBindingStatus::Bound;
    }

    class GuardedNativeDetailPresentation final
    {
    public:
        GuardedNativeDetailPresentation() = default;
        GuardedNativeBindingStatus capture(
            GameMap &, const PlannedAction &, const GuardedNativeInventory &,
            QJSEngine &, GuardedNativeCodeCatalog &, RefinementLedger &);
        // Rereads only native facts; the caller must hold the realm quiescent since capture.
        GuardedNativeBindingStatus recheckNative(GameMap &, QJSEngine &, RefinementLedger &) const;
        std::optional<std::int64_t> recheckNativeWork() const;
        const QByteArray & identity() const;
        std::int64_t receiverConstructionWork() const;

    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        std::int64_t m_receiverConstructionWork{0};
    };
}
