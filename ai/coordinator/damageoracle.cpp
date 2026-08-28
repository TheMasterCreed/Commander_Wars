#include "ai/coordinator/damageoracle.h"

#include <algorithm>
#include <iterator>

#include <QPoint>
#include <QRectF>

#include "ai/coreai.h"

#include "game/GameEnums.h"
#include "game/unit.h"
#include "game/weaponrangecheck.h"

#include "resource_management/weaponmanager.h"

namespace
{
    constexpr float NO_TAKEN_DAMAGE = 0.0f;
    constexpr bool IGNORE_OUT_OF_VISION_RANGE = false;
    // The fast path pins offence and defence to 100 and luck to 5, which is barely more than base damage.
    constexpr bool FAST_INACCURATE_DAMAGE = false;
    constexpr double NO_WEAPON_DAMAGE = -1;
}

namespace Coordinator
{
    DamageEstimate DamageOracle::estimate(std::int32_t attackerUnitIndex, Unit & attacker, const TilePoint & attackerTile,
                                          std::int32_t defenderUnitIndex, Unit & defender, const TilePoint & defenderTile)
    {
        ++m_callCount;
        const Key key{
            .attackerUnitIndex = attackerUnitIndex,
            .defenderUnitIndex = defenderUnitIndex,
            .attackerX = attackerTile.x,
            .attackerY = attackerTile.y,
            .defenderX = defenderTile.x,
            .defenderY = defenderTile.y,
        };
        const auto known = m_estimates.find(key);
        if (known != m_estimates.end())
        {
            ++m_hitCount;
            return known->second;
        }
        const QRectF report = CoreAI::calcVirtuelUnitDamage(m_pMap,
                                                            &attacker, NO_TAKEN_DAMAGE, QPoint(attackerTile.x, attackerTile.y),
                                                            GameEnums::LuckDamageMode_Average,
                                                            &defender, NO_TAKEN_DAMAGE, QPoint(defenderTile.x, defenderTile.y),
                                                            GameEnums::LuckDamageMode_Average,
                                                            IGNORE_OUT_OF_VISION_RANGE, FAST_INACCURATE_DAMAGE);
        DamageEstimate result;
        result.possible = report.x() >= 0;
        if (result.possible)
        {
            result.damageSteps = damageStepsFrom(defender.getHp(), report.x());
            result.counterSteps = damageStepsFrom(attacker.getHp(), report.width());
        }
        m_estimates.emplace(key, result);
        return result;
    }

    double DamageOracle::weaponBaseDamage(const QString & weaponId, Unit & defender)
    {
        const WeaponKey key{weaponId, defender.getUnitID()};
        const auto known = m_weaponBaseDamage.find(key);
        if (known != m_weaponBaseDamage.end())
        {
            return known->second;
        }
        const double damage = WeaponManager::getInstance()->getBaseDamage(weaponId, &defender);
        m_weaponBaseDamage.emplace(key, damage);
        return damage;
    }

    // Mirrors the weapon gate in Unit::canAttackWithWeapon, so an indirect slot never answers at melee range.
    double DamageOracle::slotBaseDamage(const WeaponSlot & slot, Unit & defender, std::int32_t distance,
                                        std::int32_t minRange, std::int32_t maxRange)
    {
        const WeaponRangeCheck::Slot usable{!slot.weaponId.isEmpty(), slot.hasAmmo};
        if (!WeaponRangeCheck::canAttack(usable, static_cast<WeaponRangeCheck::WeaponType>(slot.weaponType), distance,
                                         minRange, maxRange, WeaponRangeCheck::RangeCheck::All))
        {
            return NO_WEAPON_DAMAGE;
        }
        return weaponBaseDamage(slot.weaponId, defender);
    }

    double DamageOracle::baseDamageAgainst(Unit & attacker, Unit & defender, std::int32_t distance,
                                           std::int32_t minRange, std::int32_t maxRange)
    {
        const WeaponSlot first{attacker.getWeapon1ID(), attacker.hasAmmo1(),
                               static_cast<std::int32_t>(attacker.getTypeOfWeapon1())};
        const WeaponSlot second{attacker.getWeapon2ID(), attacker.hasAmmo2(),
                                static_cast<std::int32_t>(attacker.getTypeOfWeapon2())};
        return std::max(slotBaseDamage(first, defender, distance, minRange, maxRange),
                        slotBaseDamage(second, defender, distance, minRange, maxRange));
    }

    void DamageOracle::clear()
    {
        m_estimates.clear();
        m_weaponBaseDamage.clear();
        m_callCount = 0;
        m_hitCount = 0;
    }
}
