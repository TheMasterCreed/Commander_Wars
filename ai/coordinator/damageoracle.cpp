#include "ai/coordinator/damageoracle.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

#include <QPoint>
#include <QRectF>

#include "ai/coreai.h"
#include "coreengine/globalutils.h"

#include "game/GameEnums.h"
#include "game/unit.h"
#include "game/player.h"
#include "game/scopedcombatunitstate.h"
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
        const auto usePeek = m_luckPeekEnabled && attacker.getOwner() != nullptr &&
                             attacker.getOwner()->getPlayerID() == m_ownPlayerId;
        const GameEnums::LuckDamageMode luckMode =
            usePeek ? GameEnums::LuckDamageMode_On : GameEnums::LuckDamageMode_Average;
        std::optional<GlobalUtils::ScopedDeterministicSeed> rngScope;
        if (usePeek)
        {
            const quint32 shotSeed = computePeekedShotSeed(
                m_turnLuckNonce, attacker.getUniqueID(), attackerTile, defenderTile,
                attacker.getHp(), defender.getHp());
            rngScope.emplace(shotSeed);
        }
        const QRectF report = CoreAI::calcVirtuelUnitDamage(m_pMap,
                                                            &attacker, NO_TAKEN_DAMAGE, QPoint(attackerTile.x, attackerTile.y),
                                                            luckMode,
                                                            &defender, NO_TAKEN_DAMAGE, QPoint(defenderTile.x, defenderTile.y),
                                                            luckMode,
                                                            IGNORE_OUT_OF_VISION_RANGE, FAST_INACCURATE_DAMAGE);
        DamageEstimate result;
        result.possible = report.x() >= 0;
        if (result.possible)
        {
            result.reportedDamage = report.x();
            result.report = {report.x(), report.width(), static_cast<int>(report.y()), static_cast<int>(report.height())};
            result.damageSteps = damageStepsFrom(defender.getHp(), report.x());
            result.counterSteps = damageStepsFrom(attacker.getHp(), report.width());
        }
        m_estimates.emplace(key, result);
        return result;
    }

    OwnFireReport DamageOracle::estimateAtHp(int actorIndex, Unit &actor, TilePoint from, int targetIndex,
                                              Unit &target, TilePoint to, double actorHp, double targetHp,
                                              std::array<int, 2> targetAmmo)
    {
        if (!std::isfinite(actorHp) || !std::isfinite(targetHp) || actorHp <= 0 || actorHp > Unit::MAX_UNIT_HP ||
            targetHp <= 0 || targetHp > Unit::MAX_UNIT_HP)
            return {};
        const std::array<int, 2> actorAmmo{actor.getAmmo1(), actor.getAmmo2()};
        const auto key = std::tuple{Key{actorIndex, targetIndex, from.x, from.y, to.x, to.y},
                                    actorHp, targetHp, actorAmmo, targetAmmo};
        const auto found = m_projected.find(key);
        if (found != m_projected.end())
            return found->second;
        ScopedCombatUnitState attacker(actor, actorHp, actorAmmo);
        ScopedCombatUnitState defender(target, targetHp, targetAmmo);
        const auto usePeek = m_luckPeekEnabled && actor.getOwner() != nullptr &&
                             actor.getOwner()->getPlayerID() == m_ownPlayerId;
        const GameEnums::LuckDamageMode luckMode =
            usePeek ? GameEnums::LuckDamageMode_On : GameEnums::LuckDamageMode_Average;
        std::optional<GlobalUtils::ScopedDeterministicSeed> rngScope;
        if (usePeek)
        {
            const quint32 shotSeed = computePeekedShotSeed(
                m_turnLuckNonce, actor.getUniqueID(), from, to, actorHp, targetHp);
            rngScope.emplace(shotSeed);
        }
        const auto native = CoreAI::calcVirtuelUnitDamage(m_pMap, &actor, NO_TAKEN_DAMAGE, QPoint(from.x, from.y),
            luckMode, &target, NO_TAKEN_DAMAGE, QPoint(to.x, to.y),
            luckMode, IGNORE_OUT_OF_VISION_RANGE, FAST_INACCURATE_DAMAGE);
        OwnFireReport result{native.x(), native.width(), static_cast<int>(native.y()), static_cast<int>(native.height())};
        m_projected.emplace(key, result);
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
        return bestSlotDamage(attacker, defender, distance, minRange, maxRange, attacker.hasAmmo1(),
                              attacker.hasAmmo2());
    }

    double DamageOracle::baseDamageWithAmmo(Unit & attacker, Unit & defender, std::int32_t distance,
                                            std::int32_t minRange, std::int32_t maxRange)
    {
        return bestSlotDamage(attacker, defender, distance, minRange, maxRange, true, true);
    }

    double DamageOracle::bestSlotDamage(Unit & attacker, Unit & defender, std::int32_t distance, std::int32_t minRange,
                                        std::int32_t maxRange, bool firstHasAmmo, bool secondHasAmmo)
    {
        const WeaponSlot first{attacker.getWeapon1ID(), firstHasAmmo,
                               static_cast<std::int32_t>(attacker.getTypeOfWeapon1())};
        const WeaponSlot second{attacker.getWeapon2ID(), secondHasAmmo,
                                static_cast<std::int32_t>(attacker.getTypeOfWeapon2())};
        return std::max(slotBaseDamage(first, defender, distance, minRange, maxRange),
                        slotBaseDamage(second, defender, distance, minRange, maxRange));
    }

    void DamageOracle::setLuckPeek(quint32 turnNonce, qint32 ownPlayerId)
    {
        if (m_luckPeekEnabled && m_turnLuckNonce == turnNonce && m_ownPlayerId == ownPlayerId)
            return;
        m_luckPeekEnabled = true;
        m_turnLuckNonce = turnNonce;
        m_ownPlayerId = ownPlayerId;
        m_estimates.clear();
        m_projected.clear();
    }

    void DamageOracle::clearLuckPeek()
    {
        if (!m_luckPeekEnabled)
            return;
        m_luckPeekEnabled = false;
        m_turnLuckNonce = 0;
        m_ownPlayerId = NO_OWNER;
        m_estimates.clear();
        m_projected.clear();
    }

    void DamageOracle::clear()
    {
        m_estimates.clear();
        m_projected.clear();
        m_weaponBaseDamage.clear();
        m_callCount = 0;
        m_hitCount = 0;
    }
}
