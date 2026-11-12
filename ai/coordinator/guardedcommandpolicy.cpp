#include "ai/coordinator/guardedcommandpolicy.h"
#include "ai/coordinator/guardedmethodrequirements.h"

#include "ai/coordinator/guardednativecode.h"
#include "coreengine/jsthis.h"
#include "coreengine/scriptvariable.h"
#include "coreengine/scriptvariables.h"
#include "game/gameaction.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/victoryrule.h"

#include <QJSEngine>
#include <QThread>
#include <algorithm>

namespace Coordinator
{
    namespace
    {
        constexpr auto & MAP_METHODS = NativeMethodRequirements::Command::MAP_METHODS;
        constexpr auto & PLAYER_METHODS = NativeMethodRequirements::Command::PLAYER_METHODS;
        constexpr auto & RULES_METHODS = NativeMethodRequirements::Command::RULES_METHODS;
        constexpr std::array RULE_METHODS{GuardedNativeMethodRequirement{"getVariables", 0}};
        constexpr std::array VARIABLES_METHODS{
            GuardedNativeMethodRequirement{"getVariable", 1}, GuardedNativeMethodRequirement{"createVariable", 1}};
        constexpr std::array VARIABLE_METHODS{GuardedNativeMethodRequirement{"readDataInt32", 0}};
        constexpr auto BOUND = GuardedNativeReceiverStatus::Bound;

        bool returned(QJSEngine & engine, const QObject & object, const QMetaObject & metadata,
                      std::span<const GuardedNativeMethodRequirement> methods, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            const auto contract = contracts.prepare(metadata, methods, ledger);
            return contract.status() == BOUND && contract.authenticateReturned(engine, object, ledger) == BOUND;
        }

        bool mineFreeCount(QJSEngine & engine, GameMap & map, GameRules & rules,
                           const GuardedNativeInventory & inventory, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            // The collector excludes loaded units. The admitted stock action grammar cannot create a mine.
            if (inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS ||
                std::ranges::any_of(inventory.actors, [](const auto & actor) { return actor.type == QStringLiteral("WATERMINE"); }))
            {
                return false;
            }
            auto* units = rules.getVictoryRule(QStringLiteral("VICTORYRULE_NOUNITS"));
            if (!units || units->getMap() != &map ||
                !returned(engine, *units, VictoryRule::staticMetaObject, RULE_METHODS, ledger, contracts))
            {
                return false;
            }
            auto* vars = units->getVariables();
            if (!vars || vars->getStoredVariables().size() > GUARDED_NATIVE_MAX_RULE_VARIABLES ||
                !returned(engine, *vars, ScriptVariables::staticMetaObject, VARIABLES_METHODS, ledger, contracts))
            {
                return false;
            }
            constexpr std::array boolean{GuardedNativeMethodRequirement{"readDataBool", 0}};
            const auto reader = contracts.prepare(ScriptVariable::staticMetaObject, boolean, ledger);
            for (const auto & id : {QStringLiteral("Active0"), QStringLiteral("Active1")})
            {
                auto* active = vars->getVariable(id);
                if (!active || !active->hasUsableStoredDataStream() || active->getStoredData() != QByteArrayView("\1", 1) ||
                    reader.status() != BOUND || reader.authenticateReturned(engine, *active, ledger) != BOUND)
                {
                    return false;
                }
            }
            // The approved createVariable call returns this existing setting without adding an entry.
            auto* setting = vars->getVariable(QStringLiteral("SpinboxValue1"));
            return setting && setting->hasUsableStoredDataStream() && setting->getStoredData() == QByteArrayView("\0\0\0\1", 4) &&
                returned(engine, *setting, ScriptVariable::staticMetaObject, VARIABLE_METHODS, ledger, contracts);
        }

        bool validReceivers(GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
                            const GuardedNativeCalendar & calendar, RefinementLedger & ledger, GuardedNativeMethodCache & contracts)
        {
            if (map.thread() != QThread::currentThread() || engine.thread() != map.thread() ||
                !inventory.playerMaterial || calendar.basis.victoryRules.size() != 3 || !calendar.commandRuleIndex ||
                *calendar.commandRuleIndex >= calendar.basis.victoryRules.size() ||
                calendar.basis.victoryRules[*calendar.commandRuleIndex].id != GUARDED_NATIVE_COMMAND_RULE ||
                map.getPlayerCount() != 2)
            {
                return false;
            }
            auto* rules = map.getGameRules();
            if (rules == nullptr || rules->getMap() != &map || rules->getVictoryRuleSize() != 3)
            {
                return false;
            }
            const auto id = QString(GUARDED_NATIVE_COMMAND_RULE);
            auto* rule = rules->getVictoryRuleAtIndex(static_cast<qint32>(*calendar.commandRuleIndex));
            if (!rule || rule->getMap() != &map || rule->getRuleID() != id ||
                !rules->hasVictoryRule(id) || rules->getVictoryRule(id) != rule)
            {
                return false;
            }
            const auto mapMethods = contracts.prepare(GameMap::staticMetaObject, MAP_METHODS, ledger);
            const auto players = contracts.prepare(Player::staticMetaObject, PLAYER_METHODS, ledger);
            const auto victory = contracts.prepare(VictoryRule::staticMetaObject, RULE_METHODS, ledger);
            if (mapMethods.status() != BOUND || mapMethods.authenticate(engine, JsThis::getJsThis(&map), map, ledger) != BOUND ||
                players.status() != BOUND || !returned(engine, *rules, GameRules::staticMetaObject, RULES_METHODS, ledger, contracts) ||
                victory.status() != BOUND || victory.authenticateReturned(engine, *rule, ledger) != BOUND ||
                victory.authenticate(engine, JsThis::getJsThis(rule), *rule, ledger) != BOUND)
            {
                return false;
            }
            for (int i = 0; i < 2; ++i)
            {
                auto* player = map.getPlayer(i);
                const auto & cos = inventory.playerMaterial->at(i).cos;
                if (player == nullptr || player->getMap() != &map || cos[0] || cos[1] ||
                    player->getCO(0) || player->getCO(1) ||
                    players.authenticate(engine, JsThis::getJsThis(player), *player, ledger) != BOUND ||
                    players.authenticateReturned(engine, *player, ledger) != BOUND)
                {
                    return false;
                }
            }
            auto* variables = rule->getVariables();
            if (!variables || variables->getStoredVariables().size() != GUARDED_NATIVE_COMMAND_VARIABLES ||
                variables->getVariable(QStringLiteral("CMODE_CO_COMMAND_POWER_VALID")) ||
                !returned(engine, *variables, ScriptVariables::staticMetaObject, VARIABLES_METHODS, ledger, contracts))
            {
                return false;
            }
            const auto integer = contracts.prepare(ScriptVariable::staticMetaObject, VARIABLE_METHODS, ledger);
            auto* enabled = variables->getVariable(QStringLiteral("SpinboxValue0"));
            if (!enabled || !enabled->hasUsableStoredDataStream() || enabled->getStoredData() != QByteArrayView("\0\0\0\1", 4) ||
                integer.status() != BOUND || integer.authenticateReturned(engine, *enabled, ledger) != BOUND)
            {
                return false;
            }
            constexpr std::array readWrite{
                GuardedNativeMethodRequirement{"readDataInt32", 0}, GuardedNativeMethodRequirement{"writeDataInt32", 1}};
            const auto spinbox = contracts.prepare(ScriptVariable::staticMetaObject, readWrite, ledger);
            const auto & settings = calendar.basis.victoryRules[*calendar.commandRuleIndex].variables;
            for (const auto & id : {QStringLiteral("SpinboxValue1"), QStringLiteral("SpinboxValue2"), QStringLiteral("SpinboxValue12")})
            {
                auto* actual = variables->getVariable(id);
                const auto prior = std::ranges::find(settings, id, &GuardedNativeVariable::id);
                if (!actual || !actual->hasUsableStoredDataStream() || prior == settings.end() ||
                    !actual->getModified() || actual->getStoredData() != prior->data ||
                    spinbox.status() != BOUND || spinbox.authenticateReturned(engine, *actual, ledger) != BOUND)
                {
                    return false;
                }
            }
            return !calendar.ignoreWatermines || mineFreeCount(engine, map, *rules, inventory, ledger, contracts);
        }
    }

    GuardedNativeBindingStatus authenticateGuardedCommandPolicy(
        GameMap & map, QJSEngine & engine, GuardedNativeCodeCatalog & catalog, const GuardedNativeInventory & inventory,
        const GuardedNativeCalendar & calendar, RefinementLedger & ledger, GuardedNativeMethodCache* sharedContracts)
    {
        GuardedNativeMethodCache localContracts(ledger);
        auto & contracts = sharedContracts ? *sharedContracts : localContracts;
        // Native identity/slot reads and bounded linear searches of three rules and nineteen short variable names.
        if (!ledger.consume(131072, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        const auto before = ledger.stats().refusals;
        if (!validReceivers(map, engine, inventory, calendar, ledger, contracts))
        {
            return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }
        const auto enums = catalog.authenticateTerminalEnums(engine, GameEnums::nativeInstance(), GameEnums::staticMetaObject);
        if (enums.status != GuardedNativeBindingStatus::Bound)
        {
            return enums.status;
        }
        return catalog.authenticateCommandPolicyEnvironment(engine);
    }

    GuardedNativeBindingStatus authenticateCommandUnits(
        GameMap & map, QJSEngine & engine, const GuardedNativeInventory & inventory,
        GuardedCommandUnitHooks hooks, RefinementLedger & ledger, GuardedNativeMethodCache* sharedContracts)
    {
        GuardedNativeMethodCache localContracts(ledger);
        auto & contracts = sharedContracts ? *sharedContracts : localContracts;
        if (inventory.width <= 0 || inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS ||
            map.thread() != QThread::currentThread() || engine.thread() != map.thread())
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        if (!ledger.consume(1024 * (inventory.actors.size() + 1), RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        constexpr auto & getter = NativeMethodRequirements::Command::UNIT_GETTER;
        const auto before = ledger.stats().refusals;
        // The JS wrapper boxes the native map returned by getMap, even when its local is unused.
        if (!returned(engine, map, GameMap::staticMetaObject, MAP_METHODS, ledger, contracts))
        {
            return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
        }
        constexpr auto & initializer = NativeMethodRequirements::Command::UNIT_INITIALIZER;
        std::optional<GuardedNativeMethodContract> unitGetter, initializerMethods, commandMethods, variablesLookup;
        constexpr std::array lookup{GuardedNativeMethodRequirement{"getVariable", 1}};
        if (hooks.weather || hooks.movement)
        {
            variablesLookup.emplace(contracts.prepare(ScriptVariables::staticMetaObject, lookup, ledger));
            if (variablesLookup->status() != BOUND)
            {
                return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
            }
        }
        if (hooks.movement)
        {
            constexpr auto & playerGetters = NativeMethodRequirements::Command::PLAYER_MOVEMENT;
            constexpr std::array actionGetters{
                GuardedNativeMethodRequirement{"getActionID", 0}, GuardedNativeMethodRequirement{"getMovePath", 0}};
            const auto players = contracts.prepare(Player::staticMetaObject, playerGetters, ledger);
            // EngineActionBuilder creates a fresh final GameAction with C++ ownership and a native QObject wrapper.
            const auto action = contracts.prepare(GameAction::staticMetaObject, actionGetters, ledger);
            auto* rules = map.getGameRules();
            if (action.status() != BOUND || players.status() != BOUND || !rules ||
                map.getPlayerCount() != 2 || rules->getVictoryRuleSize() != 3 ||
                rules->hasVictoryRule(QStringLiteral("CMODE_PROCEDURAL_FRONTIER")) || rules->hasVictoryRule(QStringLiteral("CMODE_CHESS_LITE")) ||
                rules->hasVictoryRule(QStringLiteral("CMODE_CHESS_STD")) || rules->hasVictoryRule(QStringLiteral("CMODE_CHESS_TRUE")))
            {
                return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
            }
            for (int i = 0; i < 2; ++i)
            {
                auto* player = map.getPlayer(i);
                auto* vars = player ? player->getVariables() : nullptr;
                if (!vars || vars->getStoredVariables().size() > GUARDED_NATIVE_MAX_RULE_VARIABLES ||
                    players.authenticateReturned(engine, *player, ledger) != BOUND || variablesLookup->authenticateReturned(engine, *vars, ledger) != BOUND)
                {
                    return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
                }
                const auto & entries = vars->getStoredVariables();
                if (!ledger.consume(256 * (entries.size() + 1), RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (std::ranges::any_of(entries, [](const auto & entry) { return !entry || entry->getStoredId() == GUARDED_WARDEN_BARRAGES; }))
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
            }
        }
        for (const auto & actor : inventory.actors)
        {
            const bool infantry = actor.type == QStringLiteral("INFANTRY");
            const bool lightTank = actor.type == QStringLiteral("LIGHT_TANK");
            const bool initializerNeeded = (hooks.infantryInit && infantry) || (hooks.lightTankInit && lightTank);
            const bool weather = hooks.weather && (infantry || lightTank);
            if (!initializerNeeded && !hooks.repair && !weather && !hooks.movement)
            {
                continue;
            }
            if (hooks.movement && !infantry && !lightTank)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            auto & contract = initializerNeeded ? initializerMethods : weather || hooks.movement ? commandMethods : unitGetter;
            if (!contract)
            {
                const auto base = initializerNeeded ? std::span<const GuardedNativeMethodRequirement>(initializer)
                                          : std::span<const GuardedNativeMethodRequirement>(getter);
                struct ConditionalMethod
                {
                    GuardedNativeMethodRequirement method;
                    bool enabled;
                };
                const std::array additions{
                    ConditionalMethod{NativeMethodRequirements::Command::UNIT_ADDITIONS[0], weather || hooks.movement},
                    ConditionalMethod{NativeMethodRequirements::Command::UNIT_ADDITIONS[1], weather},
                    ConditionalMethod{NativeMethodRequirements::Command::UNIT_ADDITIONS[2], hooks.movement},
                    ConditionalMethod{NativeMethodRequirements::Command::UNIT_ADDITIONS[3], hooks.movement},
                    ConditionalMethod{NativeMethodRequirements::Command::UNIT_ADDITIONS[4], hooks.movement}};
                constexpr auto maximumMethods = initializer.size() + additions.size();
                constexpr auto assemblyWork = 4 * maximumMethods * sizeof(GuardedNativeMethodRequirement);
                if (!ledger.consume(assemblyWork, RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                std::vector<GuardedNativeMethodRequirement> methods;
                methods.reserve(maximumMethods);
                methods.insert(methods.end(), base.begin(), base.end());
                for (const auto & [method, enabled] : additions)
                {
                    if (enabled)
                    {
                        methods.push_back(method);
                    }
                }
                contract.emplace(contracts.prepare(Unit::staticMetaObject, methods, ledger));
                if (contract->status() != BOUND)
                {
                    return contract->status() == GuardedNativeReceiverStatus::BudgetExhausted
                        ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
                }
            }
            auto* terrain = map.getTerrain(actor.tile % inventory.width, actor.tile / inventory.width);
            auto* unit = terrain ? terrain->getUnit() : nullptr;
            if (!unit || unit->getUniqueID() != actor.id || unit->getMap() != &map ||
                contract->authenticate(engine, JsThis::getJsThis(unit), *unit, ledger) != BOUND ||
                contract->authenticateReturned(engine, *unit, ledger) != BOUND)
            {
                return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
            }
            if (hooks.infantryInit && infantry)
            {
                // Current ammunition can decrease below -1 on service. Its exact signed value is part of inventory.
                const auto & m = actor.material;
                if (!m || actor.ammo1 >= 0 || actor.ammo2 != 0 || m->maxAmmo1 != -1 || m->maxAmmo2 != 0 ||
                    m->weapon1 != QStringLiteral("WEAPON_INFANTRY_MG") || !m->weapon2.isEmpty() ||
                    m->maxFuel != 100 || m->baseMovement != 3 || m->minRange != 1 || m->maxRange != 1 ||
                    m->movementType != QStringLiteral("MOVE_FEET") || unit->getBaseVision() != 2)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
            }
            if (hooks.lightTankInit && lightTank)
            {
                const auto & m = actor.material;
                if (!m || actor.ammo1 < 0 || actor.ammo1 > 9 || actor.ammo2 >= 0 || m->maxAmmo1 != 9 || m->maxAmmo2 != -1 ||
                    m->weapon1 != QStringLiteral("WEAPON_LIGHT_TANK_GUN") || m->weapon2 != QStringLiteral("WEAPON_LIGHT_TANK_MG") ||
                    m->maxFuel != 70 || m->baseMovement != 6 || m->minRange != 1 || m->maxRange != 1 ||
                    m->movementType != QStringLiteral("MOVE_TANK") || unit->getBaseVision() != 3)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
            }
            if (weather)
            {
                auto* variables = unit->getVariables();
                if (actor.owner < 0 || actor.owner >= map.getPlayerCount() || !unit->getOwner() ||
                    unit->getOwner() != map.getPlayer(actor.owner) || !variables ||
                    variables->getStoredVariables().size() > GUARDED_NATIVE_MAX_RULE_VARIABLES ||
                    variablesLookup->authenticateReturned(engine, *variables, ledger) != BOUND)
                {
                    return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
                }
                const auto & stored = variables->getStoredVariables();
                if (!ledger.consume(256 * (stored.size() + 1), RefinementWork::MasterPreparation))
                {
                    return GuardedNativeBindingStatus::BudgetExhausted;
                }
                if (std::ranges::any_of(stored, [](const auto & variable) {
                    return !variable || variable->getStoredId() == GUARDED_COMMAND_UNIT_OWNER ||
                           variable->getStoredId() == GUARDED_COMMAND_UNIT_LINEAGE;
                }))
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
            }
            if (hooks.movement &&
                (!actor.material || actor.material->movementType != (infantry ? QStringLiteral("MOVE_FEET") : QStringLiteral("MOVE_TANK")) ||
                 actor.owner < 0 || actor.owner >= 2 || !unit->getOwner() || unit->getOwner() != map.getPlayer(actor.owner)))
            {
                return ledger.stats().refusals != before ? GuardedNativeBindingStatus::BudgetExhausted : GuardedNativeBindingStatus::Unsupported;
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }
}
