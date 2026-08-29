#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Coordinator
{
    enum class RefinementWork : std::int8_t
    {
        MandatoryPrecompute,
        OurAdmission,
        EnemyEntry,
        SearchSlice,
    };

    constexpr std::size_t REFINEMENT_WORK_CLASSES = 4;

    using GrantId = std::int32_t;
    constexpr GrantId NO_GRANT = -1;

    struct RefinementLedgerStats
    {
        std::int64_t total{0};
        std::int64_t granted{0};
        std::int64_t refunded{0};
        std::int64_t refusals{0};
        std::int64_t refundClamps{0};
        std::array<std::int64_t, REFINEMENT_WORK_CLASSES> grantedByWork{};
        std::array<std::int64_t, REFINEMENT_WORK_CLASSES> refusalsByWork{};

        std::int64_t spent() const
        {
            return granted - refunded;
        }
    };

    class RefinementLedger
    {
    public:
        bool open(std::int64_t total)
        {
            if (total < 0)
            {
                return false;
            }
            m_stats = RefinementLedgerStats{};
            m_stats.total = total;
            m_balance = total;
            m_grants.clear();
            assertConservation();
            return true;
        }

        GrantId draw(std::int64_t states, RefinementWork work)
        {
            const std::int32_t workValue = static_cast<std::int32_t>(work);
            if (states < 0 || workValue < 0 ||
                workValue >= static_cast<std::int32_t>(REFINEMENT_WORK_CLASSES))
            {
                return NO_GRANT;
            }
            const std::size_t slot = static_cast<std::size_t>(workValue);
            if (states > m_balance ||
                states > std::numeric_limits<std::int64_t>::max() - m_stats.granted ||
                m_grants.size() > static_cast<std::size_t>(std::numeric_limits<GrantId>::max()))
            {
                recordRefusal(slot);
                assertConservation();
                return NO_GRANT;
            }
            m_grants.push_back(GrantRecord{states, 0});
            m_balance -= states;
            m_stats.granted += states;
            m_stats.grantedByWork[slot] += states;
            assertConservation();
            return static_cast<GrantId>(m_grants.size() - 1);
        }

        bool refund(GrantId grant, std::int64_t states)
        {
            if (grant < 0 || static_cast<std::size_t>(grant) >= m_grants.size() || states < 0)
            {
                recordRefundClamp();
                return false;
            }
            GrantRecord & record = m_grants[static_cast<std::size_t>(grant)];
            const std::int64_t outstanding = record.granted - record.refunded;
            if (states > outstanding)
            {
                recordRefundClamp();
                return false;
            }
            record.refunded += states;
            m_balance += states;
            m_stats.refunded += states;
            assertConservation();
            return true;
        }

        std::int64_t balance() const
        {
            return m_balance;
        }

        bool exhausted() const
        {
            return m_balance == 0;
        }

        std::int64_t outstandingGranted() const
        {
            std::int64_t total = 0;
            for (const GrantRecord & record : m_grants)
            {
                const std::int64_t outstanding = record.granted - record.refunded;
                assert(outstanding >= 0);
                assert(outstanding <= m_stats.total - total);
                total += outstanding;
            }
            return total;
        }

        const RefinementLedgerStats & stats() const
        {
            return m_stats;
        }

    private:
        void recordRefusal(std::size_t slot)
        {
            if (m_stats.refusals < std::numeric_limits<std::int64_t>::max())
            {
                ++m_stats.refusals;
            }
            if (m_stats.refusalsByWork[slot] < std::numeric_limits<std::int64_t>::max())
            {
                ++m_stats.refusalsByWork[slot];
            }
        }

        void recordRefundClamp()
        {
            if (m_stats.refundClamps < std::numeric_limits<std::int64_t>::max())
            {
                ++m_stats.refundClamps;
            }
        }

        void assertConservation() const
        {
            const std::int64_t spent = m_stats.spent();
            assert(m_stats.total >= 0);
            assert(m_balance >= 0 && m_balance <= m_stats.total);
            assert(spent >= 0 && spent <= m_stats.total);
            assert(m_balance == m_stats.total - spent);
            assert(outstandingGranted() == spent);
        }

        struct GrantRecord
        {
            std::int64_t granted{0};
            std::int64_t refunded{0};
        };

        std::int64_t m_balance{0};
        std::vector<GrantRecord> m_grants;
        RefinementLedgerStats m_stats;
    };
}
