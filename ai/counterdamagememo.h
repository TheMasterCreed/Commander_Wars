#pragma once

#include <cstdint>
#include <map>

namespace CounterDamage
{
    // Matches the sentinel the caller starts a lookup from.
    constexpr float NO_VALUE = -1.0f;
    static_assert(NO_VALUE < 0.0f, "callers separate a miss from stored values with unitDamage >= 0");

    template <typename TKey>
    class Memo
    {
    public:
        bool contains(const TKey & key) const
        {
            return m_values.contains(key);
        }

        float value(const TKey & key) const
        {
            const auto entry = m_values.find(key);
            if (entry == m_values.end())
            {
                return NO_VALUE;
            }
            return static_cast<float>(entry->second);
        }

        // A softened result depends on the taken damage, so only untainted values may be replayed.
        void store(const TKey & key, double value, float takenDamage)
        {
            if (takenDamage <= 0.0f)
            {
                m_values.insert_or_assign(key, static_cast<std::int32_t>(value));
            }
        }

    private:
        std::map<TKey, std::int32_t> m_values;
    };
}
