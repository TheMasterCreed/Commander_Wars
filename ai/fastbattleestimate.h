#pragma once

#include <compare>
#include <cstdint>

namespace FastBattleEstimate
{
    // Mirrors Unit::MAX_UNIT_HP.
    constexpr double HP_SCALE = 10.0;
    // Reported by a weapon that cannot hurt its target.
    constexpr double NO_COUNTER_DAMAGE = -1.0;

    struct Combatant
    {
        double hp{0.0};
        std::int32_t costs{0};
    };

    struct Result
    {
        float attackerHpDamage{0.0f};
        float hpDamageDifference{0.0f};
        float fundsDamage{0.0f};

        friend constexpr auto operator<=>(const Result &, const Result &) = default;
    };

    constexpr Result evaluate(double attackerDamage, double counterDamage,
                              const Combatant & attacker, const Combatant & defender,
                              double ownUnitValue)
    {
        float attackerHpDamage = static_cast<float>(attackerDamage) / HP_SCALE;
        if (attackerHpDamage > defender.hp)
        {
            attackerHpDamage = defender.hp;
        }
        Result result;
        result.attackerHpDamage = attackerHpDamage;
        result.hpDamageDifference = attackerHpDamage;
        result.fundsDamage = defender.costs * attackerHpDamage / HP_SCALE;
        if (counterDamage >= 0.0)
        {
            // Deliberately unclamped, so a lethal counter still shows up as a losing trade.
            result.hpDamageDifference -= counterDamage / HP_SCALE;
            float takenHpDamage = static_cast<float>(counterDamage) / HP_SCALE;
            if (takenHpDamage > attacker.hp)
            {
                takenHpDamage = attacker.hp;
            }
            result.fundsDamage -= attacker.costs * takenHpDamage / HP_SCALE * ownUnitValue;
        }
        return result;
    }

    template <typename TDamage>
    struct ExchangedDamage
    {
        TDamage attack{};
        TDamage counter{};
    };

    // The counter slot is the defender's own damage against the attacker; anything else re-creates the phantom counter.
    template <typename TOracle, typename TUnit>
    constexpr auto exchangeBaseDamage(TOracle oracle, TUnit attacker, TUnit defender) -> ExchangedDamage<decltype(oracle(attacker, defender))>
    {
        return {oracle(attacker, defender), oracle(defender, attacker)};
    }

    // The hpDamage field carries the counter adjusted difference, so the inflicted hp comes from atkHpDamage.
    template <typename TFundsDamage, typename TDamageData>
    constexpr void fillDamageData(const TFundsDamage & funds, TDamageData & data)
    {
        data.fundsDamage = funds.fundsDamage;
        data.hpDamage = funds.atkHpDamage;
        data.hpDamageDifference = funds.hpDamage;
    }
}
