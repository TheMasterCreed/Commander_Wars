#pragma once

#include <array>

class Unit;

// Native battle scripts query the same Unit objects as gameplay. A projected shot
// sees its turn-local health and ammo, including any temporary virtual-HP changes
// made inside the script, without editing the units on the live map.
class ScopedCombatUnitState
{
public:
    ScopedCombatUnitState(const Unit &unit, double hp, std::array<int, 2> ammo)
        : hp(hp), ammo(ammo), m_unit(&unit), m_previous(s_current)
    {
        s_current = this;
    }
    ~ScopedCombatUnitState() { s_current = m_previous; }
    ScopedCombatUnitState(const ScopedCombatUnitState &) = delete;
    ScopedCombatUnitState &operator=(const ScopedCombatUnitState &) = delete;

    static ScopedCombatUnitState *forUnit(const Unit *unit)
    {
        for (auto *scope = s_current; scope; scope = scope->m_previous)
            if (scope->m_unit == unit)
                return scope;
        return nullptr;
    }

    const double hp;
    const std::array<int, 2> ammo;
    double virtualHp{-1};

private:
    const Unit *m_unit;
    ScopedCombatUnitState *m_previous;
    inline static thread_local ScopedCombatUnitState *s_current{};
};
