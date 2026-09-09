#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Coordinator
{
    enum class RefinementWork : std::int8_t
    {
        MandatoryPrecompute,
        OurAdmission,
        EnemyEntry,
        SearchSlice,
        PackingDualCertificate,
        MasterPreparation,
        MasterExpansion,
        MasterBoundScan,
        MasterDualOptimization,
        MasterDualVerification,
        MasterValuation,
        MasterWitness,
        MasterReplay,
        Count,
    };

    constexpr std::size_t REFINEMENT_WORK_CLASSES =
        static_cast<std::size_t>(RefinementWork::Count);

    using GrantId = std::int32_t;
    constexpr GrantId NO_GRANT = -1;

    enum class RefinementRefusalReason : std::int8_t
    {
        Balance,
        AccountingOverflow,
        GrantCapacity,
    };

    struct RefinementRefusal
    {
        RefinementWork work;
        RefinementRefusalReason reason;
        std::int64_t requested;
        std::int64_t balance;
        bool refundable;

        bool operator==(const RefinementRefusal &) const = default;
    };

    struct RefinementLedgerStats
    {
        std::int64_t total{0};
        std::int64_t granted{0};
        std::int64_t refunded{0};
        std::int64_t consumed{0};
        std::int64_t refusals{0};
        std::int64_t refundClamps{0};
        std::array<std::int64_t, REFINEMENT_WORK_CLASSES> grantedByWork{};
        std::array<std::int64_t, REFINEMENT_WORK_CLASSES> refundedByWork{};
        std::array<std::int64_t, REFINEMENT_WORK_CLASSES> refusalsByWork{};

        bool operator==(const RefinementLedgerStats &) const = default;

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
            m_lastRefusal.reset();
            assertConservation();
            return true;
        }

        GrantId draw(std::int64_t states, RefinementWork work)
        {
            if (!canReserve(states, work, true))
            {
                return NO_GRANT;
            }
            m_grants.push_back(GrantRecord{states, 0, work});
            reserve(states, work);
            assertConservation();
            return static_cast<GrantId>(m_grants.size() - 1);
        }

        bool consume(std::int64_t states, RefinementWork work)
        {
            if (!canReserve(states, work, false))
            {
                return false;
            }
            reserve(states, work);
            m_stats.consumed += states;
            assertConservation();
            return true;
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
            m_stats.refundedByWork[static_cast<std::size_t>(record.work)] += states;
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
            std::int64_t total = m_stats.consumed;
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

        const std::optional<RefinementRefusal> & lastRefusal() const
        {
            return m_lastRefusal;
        }

        std::size_t retainedGrantCount() const
        {
            return m_grants.size();
        }

        std::size_t refundableStorageBytes() const
        {
            return m_grants.capacity() * sizeof(GrantRecord);
        }

    private:
        bool canReserve(std::int64_t states, RefinementWork work, bool needsRecord)
        {
            const auto workValue = static_cast<std::int32_t>(work);
            if (states < 0 || workValue < 0 ||
                workValue >= static_cast<std::int32_t>(REFINEMENT_WORK_CLASSES))
            {
                return false;
            }
            if (states > m_balance ||
                states > std::numeric_limits<std::int64_t>::max() - m_stats.granted ||
                (needsRecord && m_grants.size() > static_cast<std::size_t>(std::numeric_limits<GrantId>::max())))
            {
                recordRefusal(states, work, needsRecord);
                assertConservation();
                return false;
            }
            return true;
        }

        void reserve(std::int64_t states, RefinementWork work)
        {
            m_balance -= states;
            m_stats.granted += states;
            m_stats.grantedByWork[static_cast<std::size_t>(work)] += states;
        }

        void recordRefusal(std::int64_t states, RefinementWork work, bool refundable)
        {
            auto reason = RefinementRefusalReason::GrantCapacity;
            if (states > m_balance)
            {
                reason = RefinementRefusalReason::Balance;
            }
            else if (states > std::numeric_limits<std::int64_t>::max() - m_stats.granted)
            {
                reason = RefinementRefusalReason::AccountingOverflow;
            }
            m_lastRefusal = RefinementRefusal{work, reason, states, m_balance, refundable};
            const auto slot = static_cast<std::size_t>(work);
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
            assert(m_stats.consumed >= 0 && m_stats.consumed <= spent);
            assert(m_balance == m_stats.total - spent);
            assert(outstandingGranted() == spent);
        }

        struct GrantRecord
        {
            std::int64_t granted{0};
            std::int64_t refunded{0};
            RefinementWork work{RefinementWork::MandatoryPrecompute};
        };

        std::int64_t m_balance{0};
        std::vector<GrantRecord> m_grants;
        RefinementLedgerStats m_stats;
        std::optional<RefinementRefusal> m_lastRefusal;
    };
}
