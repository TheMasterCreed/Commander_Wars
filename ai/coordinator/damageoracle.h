#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <tuple>

#include <QString>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/ownfirereport.h"
#include "ai/fastbattleestimate.h"

class GameMap;
class Unit;

namespace Coordinator
{
    inline quint32 computePeekedShotSeed(quint32 turnNonce, qint32 attackerUniqueId,
                                         TilePoint attackerTile, TilePoint targetTile,
                                         double attackerHp, double targetHp)
    {
        constexpr std::uint64_t SPLITMIX_INC = 0x9e3779b97f4a7c15ULL;
        constexpr std::uint64_t SPLITMIX_MUL1 = 0xbf58476d1ce4e5b9ULL;
        constexpr std::uint64_t SPLITMIX_MUL2 = 0x94d049bb133111ebULL;
        constexpr int SHIFT_A = 30;
        constexpr int SHIFT_B = 27;
        constexpr int SHIFT_C = 31;
        constexpr double HP_CENTI_SCALE = 100.0;
        constexpr quint32 FALLBACK_SHOT_SEED = 1u;
        const auto mix = [&](std::uint64_t acc, std::uint64_t v) {
            std::uint64_t z = acc ^ (v + SPLITMIX_INC);
            z = (z ^ (z >> SHIFT_A)) * SPLITMIX_MUL1;
            z = (z ^ (z >> SHIFT_B)) * SPLITMIX_MUL2;
            return z ^ (z >> SHIFT_C);
        };
        std::uint64_t state = static_cast<std::uint64_t>(turnNonce);
        state = mix(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(attackerUniqueId)));
        state = mix(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(attackerTile.x)));
        state = mix(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(attackerTile.y)));
        state = mix(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(targetTile.x)));
        state = mix(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(targetTile.y)));
        state = mix(state, static_cast<std::uint64_t>(std::llround(attackerHp * HP_CENTI_SCALE)));
        state = mix(state, static_cast<std::uint64_t>(std::llround(targetHp * HP_CENTI_SCALE)));
        const quint32 out = static_cast<quint32>(state ^ (state >> 32));
        return out == 0 ? FALLBACK_SHOT_SEED : out;
    }
    constexpr std::int32_t ceilHpSteps(double hp)
    {
        const std::int32_t truncated = static_cast<std::int32_t>(hp);
        if (static_cast<double>(truncated) < hp)
        {
            return truncated + 1;
        }
        return truncated;
    }

    constexpr std::int32_t hpStepsAfterDamage(double hpBefore, double reportedDamage)
    {
        const double remaining = hpBefore - reportedDamage / FastBattleEstimate::HP_SCALE;
        if (remaining <= 0)
        {
            return 0;
        }
        return ceilHpSteps(remaining);
    }

    // A negative report means the weapon cannot hurt this defender at all.
    constexpr std::int32_t damageStepsFrom(double hpBefore, double reportedDamage)
    {
        if (reportedDamage < 0)
        {
            return 0;
        }
        return ceilHpSteps(hpBefore) - hpStepsAfterDamage(hpBefore, reportedDamage);
    }

    struct DamageEstimate
    {
        std::int32_t damageSteps{0};
        std::int32_t counterSteps{0};
        bool possible{false};
        double reportedDamage{0};
        OwnFireReport report;
    };

    class DamageOracle
    {
    public:
        explicit DamageOracle(GameMap & map)
            : m_pMap(&map)
        {
        }

        // An enemy's attack tile is unknown, so exposure asks from its current tile against the candidate tile.
        DamageEstimate estimate(std::int32_t attackerUnitIndex, Unit & attacker, const TilePoint & attackerTile,
                                std::int32_t defenderUnitIndex, Unit & defender, const TilePoint & defenderTile);

        // A cached engine quote at the actual point in a shared-target sequence.
        OwnFireReport estimateAtHp(int actorIndex, Unit &actor, TilePoint from, int targetIndex,
                                   Unit &target, TilePoint to, double actorHp, double targetHp,
                                   std::array<int, 2> targetAmmo);

        // Position free apart from the range gate, for a shot whose terrain is not decided yet.
        double baseDamageAgainst(Unit & attacker, Unit & defender, std::int32_t distance,
                                 std::int32_t minRange, std::int32_t maxRange);
        // As baseDamageAgainst with both slots loaded, for a shooter that may resupply before it fires.
        double baseDamageWithAmmo(Unit & attacker, Unit & defender, std::int32_t distance,
                                  std::int32_t minRange, std::int32_t maxRange);
        double bestSlotDamage(Unit & attacker, Unit & defender, std::int32_t distance, std::int32_t minRange,
                              std::int32_t maxRange, bool firstHasAmmo, bool secondHasAmmo);
        void setLuckPeek(quint32 turnNonce, qint32 ownPlayerId);
        void clearLuckPeek();
        void clear();

        // Requests asked, and the share the memo answered without an engine call.
        std::int64_t callCount() const
        {
            return m_callCount;
        }

        std::int64_t hitCount() const
        {
            return m_hitCount;
        }

    private:
        struct Key
        {
            std::int32_t attackerUnitIndex{NO_UNIT};
            std::int32_t defenderUnitIndex{NO_UNIT};
            std::int32_t attackerX{0};
            std::int32_t attackerY{0};
            std::int32_t defenderX{0};
            std::int32_t defenderY{0};

            friend constexpr auto operator<=>(const Key &, const Key &) = default;
        };

        using WeaponKey = std::pair<QString, QString>;

        struct WeaponSlot
        {
            QString weaponId;
            bool hasAmmo{false};
            std::int32_t weaponType{0};
        };

        double weaponBaseDamage(const QString & weaponId, Unit & defender);
        double slotBaseDamage(const WeaponSlot & slot, Unit & defender, std::int32_t distance,
                              std::int32_t minRange, std::int32_t maxRange);

        GameMap* m_pMap{nullptr};
        std::map<std::tuple<Key, double, double, std::array<int, 2>, std::array<int, 2>>, OwnFireReport> m_projected;
        // Hp is read live and never keyed on, so nothing may take damage between clear() calls.
        std::map<Key, DamageEstimate> m_estimates;
        std::map<WeaponKey, double> m_weaponBaseDamage;
        std::int64_t m_callCount{0};
        std::int64_t m_hitCount{0};
        bool m_luckPeekEnabled{false};
        quint32 m_turnLuckNonce{0};
        qint32 m_ownPlayerId{NO_OWNER};
    };
}
