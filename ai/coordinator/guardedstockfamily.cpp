#include "ai/coordinator/guardedstockfamily.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/relativestockcomparison.h"
#include "coreengine/precisiongrid.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t FAMILY_CONTROL_WORK = 256;
        constexpr std::int64_t FAMILY_ACTOR_WORK = 256;
        constexpr std::int64_t FAMILY_ACTION_WORK = 256;
        constexpr std::int64_t FAMILY_PATH_POINT_WORK = 32;
        constexpr std::int64_t FAMILY_ID_UNIT_WORK = 4;
        constexpr std::int64_t MAX_REPAIR_COST_PER_HP = 1000000;
        constexpr std::int64_t DAMAGE_INPUT_RESULT_WORK = 2048;
        constexpr std::int64_t DAMAGE_ARITHMETIC_WORK = 2048;
        constexpr std::int32_t DEFENSE_BASE = 100;
        constexpr std::int32_t DEFAULT_LUCK_MAXIMUM = 10;
        constexpr std::int32_t DAMAGE_PER_HP = 10;
        constexpr std::int32_t LETHAL_DAMAGE = DAMAGE_PER_HP * UNIT_HP_STEPS;
        constexpr double HP_PRECISION = 100;

        double upward(double value)
        {
            return std::nextafter(value, std::numeric_limits<double>::infinity());
        }

        double damageUpper(const GuardedStockDamageInput & input, std::int64_t product)
        {
            const auto luckProduct = upward(static_cast<double>(DEFAULT_LUCK_MAXIMUM) * DEFENSE_BASE);
            const auto luck = upward(luckProduct / DEFENSE_BASE);
            if (input.formula == GuardedNativeDamageFormula::AdvanceWars4)
            {
                const auto ratio = upward(static_cast<double>(input.maximumOffense) / DEFENSE_BASE);
                const auto attack = upward(static_cast<double>(input.baseDamage) * ratio);
                return upward(attack + luck);
            }
            const auto attack = upward(static_cast<double>(product) / DEFENSE_BASE);
            const auto factor = upward(1.0);
            const auto scaledAttack = upward(attack * factor);
            const auto scaledLuck = upward(static_cast<double>(DEFAULT_LUCK_MAXIMUM) * factor);
            return upward(scaledAttack + scaledLuck);
        }

        bool validShape(const GuardedStockRoot & root, const GuardedStockCurrentFamily & family)
        {
            return root.actors.size() <= GUARDED_STOCK_MAX_ACTORS &&
                family.nativeBefore.size() == GUARDED_STOCK_DIGEST_BYTES &&
                family.width > 0 && family.width <= GUARDED_NATIVE_MAX_WIDTH &&
                family.height > 0 && family.height <= GUARDED_NATIVE_MAX_HEIGHT &&
                family.width * family.height == root.tiles &&
                family.repairCostPerHp.size() == root.actors.size() &&
                !family.commonPrefix.empty() && family.commonPrefix.size() <= root.actors.size() &&
                family.targetUnitId >= 0 && std::isfinite(family.lowerHp) &&
                std::isfinite(family.upperHp) && family.lowerHp > 0 &&
                family.lowerHp <= family.upperHp && family.upperHp <= UNIT_HP_STEPS;
        }

        bool onMap(TilePoint point, const GuardedStockCurrentFamily & family)
        {
            return point.x >= 0 && point.x < family.width && point.y >= 0 && point.y < family.height;
        }

        bool validPrefix(const GuardedStockRoot & root, const GuardedStockCurrentFamily & family)
        {
            std::array<bool, GUARDED_STOCK_MAX_ACTORS> actors{};
            std::array<bool, GUARDED_STOCK_MAX_ACTORS> actions{};
            for (std::size_t index = 0; index < family.commonPrefix.size(); ++index)
            {
                const auto & action = family.commonPrefix[index];
                if (action.actor >= root.actors.size() || actors[action.actor] ||
                    action.actionIndex < 0 ||
                    action.actionIndex >= static_cast<std::int32_t>(root.actors.size()) ||
                    actions[action.actionIndex] || root.actors[action.actor].owner != root.player ||
                    action.actionId.isEmpty() || action.actionId.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    action.path.empty() || action.path.size() > GUARDED_NATIVE_MAX_PATH_POINTS)
                {
                    return false;
                }
                actors[action.actor] = true;
                actions[action.actionIndex] = true;
                for (std::size_t point = 0; point < action.path.size(); ++point)
                {
                    if (!onMap(action.path[point], family) ||
                        (point > 0 && manhattanDistance(action.path[point - 1], action.path[point]) != 1))
                    {
                        return false;
                    }
                }
                const auto destination = action.path.back();
                if (root.actors[action.actor].tile != destination.y * family.width + destination.x)
                {
                    return false;
                }
                if (index + 1 == family.commonPrefix.size())
                {
                    if ((action.kind != PlanBundleKind::Fire && action.kind != PlanBundleKind::MoveAndFire) ||
                        action.targetUnitId != family.targetUnitId || !onMap(action.target, family))
                    {
                        return false;
                    }
                }
                else if ((action.kind != PlanBundleKind::Wait && action.kind != PlanBundleKind::Move) ||
                         action.target != INVALID_TILE || action.targetUnitId != NO_UNIT)
                {
                    return false;
                }
            }
            return true;
        }

        bool repairCovered(const GuardedStockRoot & root, const GuardedStockCurrentFamily & family)
        {
            std::array<std::int64_t, 2> required{};
            bool targetFound = false;
            for (std::size_t actor = 0; actor < root.actors.size(); ++actor)
            {
                const auto & record = root.actors[actor];
                const auto cost = family.repairCostPerHp[actor];
                if (record.owner < 0 || record.owner > 1 || cost <= 0 || cost > MAX_REPAIR_COST_PER_HP)
                {
                    return false;
                }
                auto roundedHp = record.roundedHp;
                if (record.id == family.targetUnitId)
                {
                    const auto & shot = family.commonPrefix.back();
                    const auto target = record.tile;
                    if (record.owner == root.player || record.actualHp != family.upperHp ||
                        record.roundedHp != static_cast<std::int32_t>(std::ceil(family.upperHp)) ||
                        shot.target != TilePoint{target % family.width, target / family.width})
                    {
                        return false;
                    }
                    roundedHp = static_cast<std::int32_t>(std::ceil(family.lowerHp));
                    targetFound = true;
                }
                if (roundedHp <= 0 || roundedHp > UNIT_HP_STEPS ||
                    !RelativeStockDetail::checkedAdd(required[record.owner], (UNIT_HP_STEPS - roundedHp) * cost))
                {
                    return false;
                }
            }
            return targetFound && required[0] <= root.funds[0] && required[1] <= root.funds[1];
        }

        bool sameFamily(const GuardedStockCurrentFamily & incoming, const GuardedStockCurrentFamily & challenger)
        {
            return incoming.nativeBefore == challenger.nativeBefore &&
                incoming.width == challenger.width && incoming.height == challenger.height &&
                incoming.targetUnitId == challenger.targetUnitId &&
                incoming.lowerHp == challenger.lowerHp && incoming.upperHp == challenger.upperHp &&
                incoming.repairCostPerHp == challenger.repairCostPerHp &&
                incoming.commonPrefix == challenger.commonPrefix;
        }
    }

    std::int64_t guardedStockDamageEnvelopeWork()
    {
        return DAMAGE_INPUT_RESULT_WORK + DAMAGE_ARITHMETIC_WORK;
    }

    GuardedStockDamageResult boundGuardedStockDamage(
        const GuardedStockDamageInput & input, RefinementLedger & ledger)
    {
        static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
        if (!ledger.consume(guardedStockDamageEnvelopeWork(), RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        if (input.calculationHp != UNIT_HP_STEPS || input.targetActualHp != UNIT_HP_STEPS ||
            input.baseDamage <= 0 || input.maximumOffense < 0 || input.minimumDefense < DEFENSE_BASE ||
            (input.formula != GuardedNativeDamageFormula::AdvanceWars4 &&
             input.formula != GuardedNativeDamageFormula::AdvanceWars1To3))
        {
            return {};
        }
        const auto product = static_cast<std::int64_t>(input.baseDamage) * input.maximumOffense;
        if (product >= static_cast<std::int64_t>(LETHAL_DAMAGE - DEFAULT_LUCK_MAXIMUM) * DEFENSE_BASE)
        {
            return {};
        }
        // D >= 100 gives a common outward upper for both admitted expression orders.
        const auto upper = damageUpper(input, product);
        if (!std::isfinite(upper) || upper < 0 || upper >= LETHAL_DAMAGE)
        {
            return {};
        }
        const auto maximumDamage = static_cast<std::int32_t>(std::floor(upper));
        const auto lowerHp = PrecisionGrid::floor(
            static_cast<double>(UNIT_HP_STEPS) - static_cast<double>(maximumDamage) / DAMAGE_PER_HP,
            HP_PRECISION);
        if (!std::isfinite(lowerHp) || lowerHp <= 0 || lowerHp > UNIT_HP_STEPS)
        {
            return {};
        }
        return {GuardedNativeBindingStatus::Bound,
                GuardedStockDamageEnvelope{maximumDamage, lowerHp, static_cast<double>(UNIT_HP_STEPS)}};
    }

    std::optional<std::int64_t> guardedStockFamilyWork(std::size_t actors, std::size_t prefixActions)
    {
        if (actors == 0 || actors > GUARDED_STOCK_MAX_ACTORS || prefixActions > actors)
        {
            return std::nullopt;
        }
        if (prefixActions == 0)
        {
            return 0;
        }
        return FAMILY_CONTROL_WORK + FAMILY_ACTOR_WORK * actors +
            prefixActions * (FAMILY_ACTION_WORK + FAMILY_ID_UNIT_WORK * GUARDED_NATIVE_MAX_ID_LENGTH +
                             FAMILY_PATH_POINT_WORK * GUARDED_NATIVE_MAX_PATH_POINTS);
    }

    GuardedRelationStatus verifyGuardedStockFamilyRelation(
        const GuardedStockRoot & incomingAnchor, const GuardedStockRoot & challengerAnchor,
        const GuardedStockCurrentFamily & incomingFamily, const GuardedStockCurrentFamily & challengerFamily,
        const GuardedStockRelation & relation, RefinementLedger & ledger)
    {
        const auto actors = std::max(incomingAnchor.actors.size(), challengerAnchor.actors.size());
        const auto prefix = std::max(incomingFamily.commonPrefix.size(), challengerFamily.commonPrefix.size());
        const auto work = guardedStockFamilyWork(actors, prefix);
        if (!work || prefix == 0)
        {
            return GuardedRelationStatus::Invalid;
        }
        if (!ledger.consume(*work, RefinementWork::MasterWitness))
        {
            return GuardedRelationStatus::BudgetExhausted;
        }
        if (!validShape(incomingAnchor, incomingFamily) || !validShape(challengerAnchor, challengerFamily) ||
            !validPrefix(incomingAnchor, incomingFamily) || !validPrefix(challengerAnchor, challengerFamily) ||
            !sameFamily(incomingFamily, challengerFamily))
        {
            return GuardedRelationStatus::Invalid;
        }
        const auto checked = verifyGuardedStockRelation(incomingAnchor, challengerAnchor, relation, ledger);
        if (checked != GuardedRelationStatus::Verified)
        {
            return checked;
        }
        for (const auto & action : incomingFamily.commonPrefix)
        {
            if (action.actor == relation.roles.occupant)
            {
                return GuardedRelationStatus::Invalid;
            }
        }
        if (!repairCovered(incomingAnchor, incomingFamily) || !repairCovered(challengerAnchor, challengerFamily))
        {
            return GuardedRelationStatus::Invalid;
        }
        return GuardedRelationStatus::Verified;
    }
}
