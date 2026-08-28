#pragma once

#include <algorithm>
#include <compare>
#include <cstdint>
#include <vector>

namespace FarAwayCapture
{
    constexpr std::int32_t NO_DAY = -1;

    struct Target
    {
        std::int32_t x{0};
        std::int32_t y{0};

        friend constexpr auto operator<=>(const Target &, const Target &) = default;
    };

    class Reservations
    {
    public:
        void reserve(std::int32_t day, const Target & target)
        {
            expire(day);
            if (!contains(target))
            {
                m_targets.push_back(target);
            }
        }

        bool isReserved(std::int32_t day, const Target & target)
        {
            expire(day);
            return contains(target);
        }

        void reset()
        {
            m_targets.clear();
        }

    private:
        void expire(std::int32_t day)
        {
            if (day != m_day)
            {
                m_day = day;
                m_targets.clear();
            }
        }

        bool contains(const Target & target) const
        {
            return std::find(m_targets.begin(), m_targets.end(), target) != m_targets.end();
        }

        // One turn per player per day.
        std::int32_t m_day{NO_DAY};
        std::vector<Target> m_targets;
    };
}
