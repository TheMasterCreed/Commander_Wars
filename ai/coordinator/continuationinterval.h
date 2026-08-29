#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "ai/coordinator/economicledger.h"

namespace Coordinator
{
    class CanonicalPlanActionKey
    {
    public:
        static constexpr CanonicalPlanActionKey fromAction(std::int32_t row, std::int32_t x, std::int32_t y,
                                                           bool captures)
        {
            const std::uint64_t rowBits = static_cast<std::uint32_t>(row);
            const std::uint64_t xBits = static_cast<std::uint32_t>(x);
            const std::uint64_t yBits = static_cast<std::uint32_t>(y);
            return CanonicalPlanActionKey{
                (rowBits << 32) | xBits,
                (yBits << 1) | (captures ? 1ULL : 0ULL),
            };
        }

        constexpr std::int32_t row() const
        {
            return decodeSigned(static_cast<std::uint32_t>(m_limbs[0] >> 32));
        }

        constexpr std::int32_t x() const
        {
            return decodeSigned(static_cast<std::uint32_t>(m_limbs[0]));
        }

        constexpr std::int32_t y() const
        {
            return decodeSigned(static_cast<std::uint32_t>(m_limbs[1] >> 1));
        }

        constexpr bool captures() const
        {
            return (m_limbs[1] & 1ULL) != 0;
        }

        constexpr std::uint64_t hashValue() const
        {
            return m_limbs[0] ^ (m_limbs[1] + UINT64_C(0x9e3779b97f4a7c15) +
                                 (m_limbs[0] << 6) + (m_limbs[0] >> 2));
        }

        friend constexpr bool operator==(const CanonicalPlanActionKey & lhs,
                                         const CanonicalPlanActionKey & rhs)
        {
            return lhs.m_limbs == rhs.m_limbs;
        }

        friend constexpr std::strong_ordering operator<=>(const CanonicalPlanActionKey & lhs,
                                                           const CanonicalPlanActionKey & rhs)
        {
            return std::tuple{lhs.row(), lhs.x(), lhs.y(), lhs.captures()} <=>
                   std::tuple{rhs.row(), rhs.x(), rhs.y(), rhs.captures()};
        }

    private:
        constexpr CanonicalPlanActionKey(std::uint64_t first, std::uint64_t second)
            : m_limbs{first, second}
        {
        }

        static constexpr std::int32_t decodeSigned(std::uint32_t bits)
        {
            constexpr std::uint64_t SIGNED_RANGE = UINT64_C(1) << 32;
            const std::int64_t value =
                bits <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                    ? static_cast<std::int64_t>(bits)
                    : static_cast<std::int64_t>(bits) - static_cast<std::int64_t>(SIGNED_RANGE);
            return static_cast<std::int32_t>(value);
        }

        std::array<std::uint64_t, 2> m_limbs{};
    };

    struct CanonicalPlanActionKeyHash
    {
        constexpr std::size_t operator()(const CanonicalPlanActionKey & key) const
        {
            return static_cast<std::size_t>(key.hashValue());
        }
    };

    using ContinuationKey = std::vector<CanonicalPlanActionKey>;

    constexpr std::int64_t LIVE_PAIR_RELEASE_BUDGET_NANOS = 300000000;
    constexpr std::int64_t LIVE_PAIR_STATE_TENTH_NANOS = 1395;
    constexpr std::int64_t LIVE_PAIR_DAY2_PACKING_STATES = 171992654;
    constexpr std::int64_t LIVE_PAIR_DAY2_SOLVES = 17900;
    constexpr std::int64_t LIVE_PAIR_REFINEMENT_BUDGET =
        LIVE_PAIR_RELEASE_BUDGET_NANOS * 10 / LIVE_PAIR_STATE_TENTH_NANOS;
    constexpr std::int64_t LIVE_PAIR_SLICE_BASE =
        (LIVE_PAIR_DAY2_PACKING_STATES + LIVE_PAIR_DAY2_SOLVES - 1) /
        LIVE_PAIR_DAY2_SOLVES;
    static_assert(LIVE_PAIR_REFINEMENT_BUDGET == 2150537);
    static_assert(LIVE_PAIR_SLICE_BASE == 9609);

    constexpr bool livePairRefinementSliceCap(std::int32_t rung, std::int64_t & cap)
    {
        if (rung < 0)
        {
            return false;
        }
        cap = LIVE_PAIR_SLICE_BASE;
        for (std::int32_t index = 0; index < rung; ++index)
        {
            if (cap > std::numeric_limits<std::int64_t>::max() / 2)
            {
                return false;
            }
            cap *= 2;
        }
        return true;
    }

    constexpr bool advanceLivePairRefinementRung(std::int32_t & rung, bool completed)
    {
        if (!completed || rung < 0 || rung == std::numeric_limits<std::int32_t>::max())
        {
            return false;
        }
        ++rung;
        return true;
    }

    struct StockInterval
    {
        MilliFunds lower{0};
        MilliFunds upper{0};

        constexpr bool exact() const
        {
            return lower == upper;
        }

        constexpr MilliFunds width() const
        {
            return upper - lower;
        }
    };

    constexpr StockInterval composeStockInterval(MilliFunds owned, const StockInterval & ours,
                                                 const StockInterval & enemy)
    {
        return StockInterval{owned + ours.lower - enemy.upper, owned + ours.upper - enemy.lower};
    }

    enum class AssignPhase : std::int8_t
    {
        AfterGreedyInit,
        AfterSettleSweep,
        BetweenSwapSweeps,
        BeforeEnumeration,
        AfterEnumeration,
    };

    struct RowWitness
    {
        std::int32_t row{-1};
        std::vector<std::int32_t> nodes;
        MilliFunds value{0};
    };

    struct EnemySetBounds
    {
        StockInterval bounds;
        StockInterval initialBounds;
        std::int32_t sliceRung{0};
        bool exact{false};
        bool storeBacked{false};
    };

    struct ContinuationEntry
    {
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        StockInterval ours;
        StockInterval initialOurs;
        std::int32_t sliceRung{0};
        bool exact{false};
        bool storeBacked{false};
        std::vector<RowWitness> witness;
        std::vector<RowWitness> initialWitness;
    };

    class ContinuationStore
    {
    public:
        ContinuationEntry* find(const ContinuationKey & key)
        {
            const auto found = m_entries.find(key);
            return found == m_entries.end() ? nullptr : &found->second;
        }

        const ContinuationEntry* find(const ContinuationKey & key) const
        {
            const auto found = m_entries.find(key);
            return found == m_entries.end() ? nullptr : &found->second;
        }

        ContinuationEntry & admit(ContinuationEntry entry)
        {
            entry.initialOurs = entry.ours;
            entry.initialWitness = entry.witness;
            return m_entries.emplace(entry.key, std::move(entry)).first->second;
        }

        EnemySetBounds* enemyFor(const std::vector<std::int32_t> & capturedColumns)
        {
            const auto found = m_enemy.find(capturedColumns);
            return found == m_enemy.end() ? nullptr : &found->second;
        }

        const EnemySetBounds* enemyFor(const std::vector<std::int32_t> & capturedColumns) const
        {
            const auto found = m_enemy.find(capturedColumns);
            return found == m_enemy.end() ? nullptr : &found->second;
        }

        EnemySetBounds & admitEnemy(const std::vector<std::int32_t> & capturedColumns,
                                    EnemySetBounds bounds)
        {
            bounds.initialBounds = bounds.bounds;
            return m_enemy.emplace(capturedColumns, std::move(bounds)).first->second;
        }

        static bool tightenOurLower(ContinuationEntry & entry, MilliFunds lower,
                                    std::vector<RowWitness> witness)
        {
            if (lower <= entry.ours.lower)
            {
                return false;
            }
            entry.ours.lower = lower;
            entry.witness = std::move(witness);
            return true;
        }

        static bool tightenOurUpper(ContinuationEntry & entry, MilliFunds upper)
        {
            if (upper >= entry.ours.upper)
            {
                return false;
            }
            entry.ours.upper = upper;
            return true;
        }

    private:
        std::map<ContinuationKey, ContinuationEntry> m_entries;
        std::map<std::vector<std::int32_t>, EnemySetBounds> m_enemy;
    };

    struct ContenderRecord
    {
        MilliFunds cheapLower{0};
        MilliFunds cheapUpper{0};
        bool admitted{false};
        bool resolved{false};
    };

    class ContenderRegistry
    {
    public:
        void open(std::int32_t demandCapacity)
        {
            m_records.clear();
            m_demand.clear();
            m_demandCapacity = demandCapacity;
        }

        void encounter(const ContinuationKey & key, MilliFunds cheapLower, MilliFunds cheapUpper)
        {
            const auto [slot, inserted] =
                m_records.emplace(key, ContenderRecord{cheapLower, cheapUpper, false, false});
            if (inserted)
            {
                insertDemand(key, slot->second);
            }
        }

        void markAdmitted(const ContinuationKey & key)
        {
            const auto found = m_records.find(key);
            if (found == m_records.end())
            {
                return;
            }
            found->second.admitted = true;
            eraseDemand(key, found->second);
        }

        void markResolved(const ContinuationKey & key)
        {
            const auto found = m_records.find(key);
            if (found == m_records.end())
            {
                return;
            }
            found->second.resolved = true;
            eraseDemand(key, found->second);
        }

        const ContenderRecord* find(const ContinuationKey & key) const
        {
            const auto found = m_records.find(key);
            return found == m_records.end() ? nullptr : &found->second;
        }

        std::vector<ContinuationKey> refinementOrder() const
        {
            std::set<ContinuationKey> demanded;
            for (const DemandEntry & entry : m_demand)
            {
                demanded.insert(entry.key);
            }
            std::set<DemandEntry> ordered;
            for (const auto & [key, record] : m_records)
            {
                if (!record.resolved && (record.admitted || demanded.contains(key)))
                {
                    ordered.insert(DemandEntry{record.cheapLower, record.cheapUpper, key});
                }
            }
            std::vector<ContinuationKey> keys;
            keys.reserve(ordered.size());
            for (const DemandEntry & entry : ordered)
            {
                keys.push_back(entry.key);
            }
            return keys;
        }

    private:
        struct DemandEntry
        {
            MilliFunds cheapLower{0};
            MilliFunds cheapUpper{0};
            ContinuationKey key;

            friend bool operator<(const DemandEntry & lhs, const DemandEntry & rhs)
            {
                if (lhs.cheapLower != rhs.cheapLower)
                {
                    return lhs.cheapLower > rhs.cheapLower;
                }
                if (lhs.cheapUpper != rhs.cheapUpper)
                {
                    return lhs.cheapUpper > rhs.cheapUpper;
                }
                return lhs.key < rhs.key;
            }
        };

        void insertDemand(const ContinuationKey & key, const ContenderRecord & record)
        {
            if (m_demandCapacity <= 0)
            {
                return;
            }
            DemandEntry entry{record.cheapLower, record.cheapUpper, key};
            if (static_cast<std::int32_t>(m_demand.size()) >= m_demandCapacity)
            {
                const auto worst = std::prev(m_demand.end());
                if (!(entry < *worst))
                {
                    return;
                }
                m_demand.erase(worst);
            }
            m_demand.insert(std::move(entry));
        }

        void eraseDemand(const ContinuationKey & key, const ContenderRecord & record)
        {
            m_demand.erase(DemandEntry{record.cheapLower, record.cheapUpper, key});
        }

        std::map<ContinuationKey, ContenderRecord> m_records;
        std::set<DemandEntry> m_demand;
        std::int32_t m_demandCapacity{0};
    };
}
