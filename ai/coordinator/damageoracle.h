#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include <QString>

#include "ai/coordinator/coordinatorcommon.h"
#include "ai/fastbattleestimate.h"

class GameMap;
class Unit;

namespace Coordinator
{
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

        // Position free apart from the range gate, for a shot whose terrain is not decided yet.
        double baseDamageAgainst(Unit & attacker, Unit & defender, std::int32_t distance,
                                 std::int32_t minRange, std::int32_t maxRange);
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
        // Hp is read live and never keyed on, so nothing may take damage between clear() calls.
        std::map<Key, DamageEstimate> m_estimates;
        std::map<WeaponKey, double> m_weaponBaseDamage;
        std::int64_t m_callCount{0};
        std::int64_t m_hitCount{0};
    };
}
