#pragma once

#include <algorithm>
#include <compare>
#include <cstdint>
#include <span>

namespace UnloadSelection
{
    constexpr std::int32_t NO_CANDIDATE = -1;
    constexpr std::int32_t NO_FIELD = -1;
    constexpr std::int32_t FIRST_FIELD = 0;
    constexpr std::int32_t SINGLE_FIELD = 1;

    // Where the caller reads the unload tile from once a candidate is picked.
    enum class FieldSource : std::int8_t
    {
        None = 0,
        MarkedFieldData = 1,
        CandidateField = 2,
    };

    struct Candidate
    {
        std::int32_t loadedIndex{NO_CANDIDATE};
        bool needsRefuel{false};
        bool canCapture{false};
        std::int32_t fieldCount{0};
        std::int32_t enemyBuildingField{NO_FIELD};

        friend constexpr auto operator<=>(const Candidate &, const Candidate &) = default;
    };

    struct Choice
    {
        std::int32_t candidate{NO_CANDIDATE};
        std::int32_t field{NO_FIELD};
        FieldSource source{FieldSource::None};

        friend constexpr auto operator<=>(const Choice &, const Choice &) = default;
    };

    // No candidate is eligible, so the caller has to fall back to its own tile scan.
    constexpr Choice FALLBACK{};

    // A menu action id that is not a usable loaded unit index degrades to the menu position it was read from.
    constexpr std::int32_t resolveLoadedIndex(bool parsedOk, std::int32_t parsedValue, std::int32_t menuPosition,
                                              std::int32_t loadedUnitCount)
    {
        if (parsedOk && parsedValue >= 0 && parsedValue < loadedUnitCount)
        {
            return parsedValue;
        }
        return menuPosition;
    }

    constexpr bool isUnloaded(std::span<const std::int32_t> alreadyUnloaded, std::int32_t loadedIndex)
    {
        return std::find(alreadyUnloaded.begin(), alreadyUnloaded.end(), loadedIndex) != alreadyUnloaded.end();
    }

    constexpr Choice select(std::span<const Candidate> candidates, std::span<const std::int32_t> alreadyUnloaded)
    {
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(candidates.size()); ++i)
        {
            const Candidate & candidate = candidates[i];
            if (candidate.needsRefuel || isUnloaded(alreadyUnloaded, candidate.loadedIndex))
            {
                continue;
            }
            if (candidate.fieldCount == SINGLE_FIELD)
            {
                return Choice{i, FIRST_FIELD, FieldSource::MarkedFieldData};
            }
            if (candidate.fieldCount > 0 && candidate.canCapture && candidate.enemyBuildingField != NO_FIELD)
            {
                return Choice{i, candidate.enemyBuildingField, FieldSource::CandidateField};
            }
        }
        return FALLBACK;
    }
}
