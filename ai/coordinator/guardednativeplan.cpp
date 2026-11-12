#include "ai/coordinator/guardednativeinventory.h"

#include <algorithm>
#include <cmath>

#include "ai/coordinator/bundleassignment.h"
#include "game/GameEnums.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t INVENTORY_TILE_WORK = 1536;
        constexpr std::int64_t INVENTORY_ACTOR_PAIR_WORK = 16;
        constexpr std::int64_t INVENTORY_CONTROL_WORK = 256;
        constexpr std::int64_t INVENTORY_CO_WORK =
            512 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH + GUARDED_NATIVE_CO_STATE_WORK;
        constexpr std::int64_t INVENTORY_PERK_WORK = 64 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH;
        constexpr std::int64_t AMMUNITION_WORK_PER_ACTOR = 32;
        constexpr std::int64_t MATERIAL_ACTOR_WORK = 512 + AMMUNITION_WORK_PER_ACTOR;
        constexpr std::int32_t UNLIMITED_AMMO_MAXIMUM = -1;
        constexpr std::int64_t MATERIAL_CONTROL_WORK = 1024;
        constexpr std::int64_t MATERIAL_SHAPE_ENTRY_WORK = 64;
        constexpr std::size_t MATERIAL_FIXED_SHAPE_RECORDS = 7;
        constexpr std::int64_t PLAN_ACTOR_PAIR_WORK = 64;
        constexpr std::int64_t PLAN_PATH_POINT_WORK = 32;
        constexpr std::int64_t PLAN_INVENTORY_WORK = 192;
        constexpr std::int64_t PLAN_GRAPH_WORK = 64;
        constexpr std::int64_t PLAN_CANDIDATE_WORK = 16;
        constexpr std::array<std::int32_t, 2> CO_UNIT_RANKS{
            GameEnums::UnitRank_CO0, GameEnums::UnitRank_CO1};

        bool onMap(TilePoint point, const GuardedNativeInventory & inventory)
        {
            return point.x >= 0 && point.x < inventory.width && point.y >= 0 && point.y < inventory.height;
        }

        std::optional<std::size_t> actorAt(const GuardedNativeInventory & inventory, std::int32_t id)
        {
            for (std::size_t index = 0; index < inventory.actors.size(); ++index)
            {
                if (inventory.actors[index].id == id)
                {
                    return index;
                }
            }
            return std::nullopt;
        }

        bool bindsTarget(const GuardedNativeInventory & inventory, const PlannedAction & action)
        {
            if (action.kind == PlanBundleKind::Fire || action.kind == PlanBundleKind::MoveAndFire)
            {
                const auto actor = actorAt(inventory, action.targetUnitId);
                if (!actor || inventory.actors[*actor].owner == inventory.player)
                {
                    return false;
                }
                const auto tile = inventory.actors[*actor].tile;
                return action.target == TilePoint{tile % inventory.width, tile / inventory.width};
            }
            if (action.kind == PlanBundleKind::Capture || action.kind == PlanBundleKind::MoveAndCapture)
            {
                if (action.target != action.destination || action.targetUnitId != NO_UNIT)
                {
                    return false;
                }
                const auto tile = action.destination.y * inventory.width + action.destination.x;
                return std::any_of(inventory.properties.begin(), inventory.properties.end(),
                    [&](const auto & property) { return property.tile == tile; });
            }
            return action.target == INVALID_TILE && action.targetUnitId == NO_UNIT;
        }

        bool validAmmunitionSlot(std::int32_t current, std::int32_t maximum, const QString & weapon)
        {
            if (maximum == UNLIMITED_AMMO_MAXIMUM)
            {
                return !weapon.isEmpty() && current >= -GUARDED_NATIVE_MAX_RESOURCE && current < 0;
            }
            return current >= 0 && current <= maximum && maximum <= GUARDED_NATIVE_MAX_RESOURCE &&
                (!weapon.isEmpty() || maximum == 0);
        }

        bool validAmmunition(const GuardedNativeActor & actor)
        {
            if (!actor.material)
            {
                return actor.ammo1 >= 0 && actor.ammo2 >= 0;
            }
            return validAmmunitionSlot(actor.ammo1, actor.material->maxAmmo1, actor.material->weapon1) &&
                validAmmunitionSlot(actor.ammo2, actor.material->maxAmmo2, actor.material->weapon2);
        }

        bool validInventory(const GuardedNativeInventory & inventory)
        {
            if (!guardedNativeInventoryWork(inventory.width, inventory.height) || inventory.player != 0 ||
                inventory.day < 0 || inventory.funds[0] < 0 || inventory.funds[1] < 0 ||
                inventory.defeated[0] || inventory.defeated[1] || inventory.teams[0] == inventory.teams[1] ||
                inventory.actors.empty() || inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS ||
                inventory.properties.size() > static_cast<std::size_t>(inventory.width * inventory.height))
            {
                return false;
            }
            std::array<bool, GUARDED_STOCK_MAX_TILES> occupied{};
            std::array<bool, GUARDED_STOCK_MAX_TILES> properties{};
            std::int32_t previousId = NO_UNIT;
            const auto tiles = inventory.width * inventory.height;
            for (const auto & actor : inventory.actors)
            {
                if (actor.id <= previousId || actor.owner < 0 || actor.owner > 1 ||
                    actor.tile < 0 || actor.tile >= tiles || occupied[actor.tile] ||
                    actor.type.isEmpty() || actor.type.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    !std::isfinite(actor.actualHp) || actor.actualHp <= 0 || actor.actualHp > UNIT_HP_STEPS ||
                    actor.progress < 0 || actor.progress >= GUARDED_STOCK_CAPTURE_THRESHOLD ||
                    actor.fuel < 0 || !validAmmunition(actor) ||
                    actor.hidden || actor.cloaked)
                {
                    return false;
                }
                occupied[actor.tile] = true;
                previousId = actor.id;
            }
            for (const auto & property : inventory.properties)
            {
                if (property.tile < 0 || property.tile >= tiles || properties[property.tile] ||
                    property.owner < NO_OWNER || property.owner > 1 ||
                    property.type.isEmpty() || property.type.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
                properties[property.tile] = true;
            }
            return true;
        }

        bool bindPlan(const MasterAssignmentProblem & problem, const GuardedNativeInventory & inventory,
                      const MasterAssignmentState & state, const MasterAssignmentReplayResult & replay,
                      std::size_t pathPoints, GuardedNativePlanBinding & binding)
        {
            if (!state.identity.sameLiveProblem(problem.identity()) || !validMasterState(problem, state) ||
                !state.complete() || !replay.complete() ||
                replay.plan.actionCount() != static_cast<std::int32_t>(problem.actors().size()) ||
                replay.plan.edgeCount() < 0 ||
                static_cast<std::size_t>(replay.plan.edgeCount()) > problem.actors().size() * problem.actors().size() ||
                replay.ordering.order.size() != problem.actors().size() ||
                replay.actionIndices.size() != problem.actors().size())
            {
                return false;
            }
            const auto nativeOrder = replay.plan.executionOrder();
            if (nativeOrder.order != replay.ordering.order || !nativeOrder.cycleMembers.empty() ||
                !nativeOrder.blockedByCycle.empty())
            {
                return false;
            }
            binding.identity = state.identity;
            binding.actions.reserve(problem.actors().size());
            std::array<bool, GUARDED_STOCK_MAX_ACTORS> seenActions{};
            std::array<bool, GUARDED_STOCK_MAX_ACTORS> seenActors{};
            for (const auto actionIndex : replay.ordering.order)
            {
                if (actionIndex < 0 || actionIndex >= replay.plan.actionCount() || seenActions[actionIndex])
                {
                    return false;
                }
                seenActions[actionIndex] = true;
                const auto & action = replay.plan.action(actionIndex);
                const auto nativeActor = actorAt(inventory, action.unitId);
                if (!nativeActor || seenActors[*nativeActor] ||
                    inventory.actors[*nativeActor].owner != inventory.player ||
                    inventory.actors[*nativeActor].moved || action.state != PlanActionState::Pending ||
                    action.path.empty() || action.path.size() > pathPoints ||
                    action.actionId.isEmpty() || action.actionId.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    action.path.back() != action.destination || !onMap(action.destination, inventory))
                {
                    return false;
                }
                seenActors[*nativeActor] = true;
                const auto & native = inventory.actors[*nativeActor];
                const TilePoint origin{native.tile % inventory.width, native.tile / inventory.width};
                if (action.path.front() != origin)
                {
                    return false;
                }
                const auto actor = std::find_if(problem.actors().begin(), problem.actors().end(),
                    [&](const auto & record) { return record.engineUnitId == action.unitId; });
                if (actor == problem.actors().end() || actor->canonicalSlot < 0 ||
                    static_cast<std::size_t>(actor->canonicalSlot) >= state.choices.size() ||
                    replay.actionIndices[actor->canonicalSlot] != actionIndex)
                {
                    return false;
                }
                const auto choice = state.choices[actor->canonicalSlot];
                if (choice < 0 || static_cast<std::size_t>(choice) >= actor->candidates.size())
                {
                    return false;
                }
                const auto & candidate = actor->candidates[choice];
                if (candidate.origin != origin || candidate.destination != action.destination ||
                    candidate.actionKind != action.kind ||
                    candidate.target != action.target || candidate.targetUnitId != action.targetUnitId ||
                    candidate.source.bundle.path != action.path ||
                    action.actionId != planActionIdFor(problem.actionIds(), action.kind))
                {
                    return false;
                }
                for (std::size_t point = 0; point < action.path.size(); ++point)
                {
                    if (!onMap(action.path[point], inventory) ||
                        (point > 0 && manhattanDistance(action.path[point - 1], action.path[point]) != 1))
                    {
                        return false;
                    }
                }
                if (!bindsTarget(inventory, action))
                {
                    return false;
                }
                binding.actions.push_back({*nativeActor, actionIndex, action.kind, action.actionId,
                                           action.path, action.target, action.targetUnitId});
            }
            for (std::size_t actor = 0; actor < inventory.actors.size(); ++actor)
            {
                if (inventory.actors[actor].owner == inventory.player &&
                    !inventory.actors[actor].moved && !seenActors[actor])
                {
                    return false;
                }
            }
            return true;
        }

        bool allowedDifference(const GuardedNativeAction & incoming, const GuardedNativeAction & challenger)
        {
            return incoming.kind == PlanBundleKind::Wait && challenger.kind == PlanBundleKind::Capture &&
                incoming.actor == challenger.actor && incoming.actionIndex == challenger.actionIndex &&
                incoming.path.size() == 1 && incoming.path == challenger.path &&
                incoming.target == INVALID_TILE && challenger.target == challenger.path.front() &&
                incoming.targetUnitId == NO_UNIT && challenger.targetUnitId == NO_UNIT;
        }

        bool validCoUnitRank(const GuardedNativeActor & actor, const GuardedNativeInventory & inventory)
        {
            const auto rank = actor.material->rank;
            if (rank == GUARDED_NATIVE_UNRANKED)
            {
                return true;
            }
            const auto marker = std::find(CO_UNIT_RANKS.begin(), CO_UNIT_RANKS.end(), rank);
            if (marker == CO_UNIT_RANKS.end())
            {
                return false;
            }
            const auto slot = static_cast<std::size_t>(marker - CO_UNIT_RANKS.begin());
            const auto & co = (*inventory.playerMaterial)[actor.owner].cos[slot];
            return co && co->unitId == actor.id;
        }

        bool validMaterial(const GuardedNativeActor & actor, const GuardedNativeInventory & inventory)
        {
            if (!actor.material)
            {
                return false;
            }
            const auto & material = *actor.material;
            return material.maxFuel >= actor.fuel && material.maxFuel <= GUARDED_NATIVE_MAX_RESOURCE &&
                material.baseMovement >= 0 && material.baseMovement <= GUARDED_NATIVE_MAX_MOVEMENT &&
                material.minRange >= 0 && material.maxRange >= material.minRange &&
                material.maxRange <= static_cast<std::int32_t>(GUARDED_STOCK_MAX_TILES) &&
                validCoUnitRank(actor, inventory) && !material.movementType.isEmpty() &&
                material.movementType.size() <= GUARDED_NATIVE_MAX_ID_LENGTH &&
                material.weapon1.size() <= GUARDED_NATIVE_MAX_ID_LENGTH &&
                material.weapon2.size() <= GUARDED_NATIVE_MAX_ID_LENGTH &&
                material.moddingFlags == 0 && !material.collisionOverride && !material.temporaryBonuses &&
                !material.multiTurnPath && !material.virtualHp;
        }

        bool validCoRules(const GuardedNativeCoRules & rules)
        {
            if (!std::isfinite(rules.gainSpeed) || rules.gainSpeed < 0 ||
                rules.gainSpeed > GUARDED_NATIVE_MAX_POWER_FACTOR ||
                !std::isfinite(rules.usageReduction) || rules.usageReduction < 0 ||
                rules.usageReduction > GUARDED_NATIVE_MAX_POWER_FACTOR)
            {
                return false;
            }
            switch (rules.gainMode)
            {
                case GuardedNativePowerGainMode::Money:
                case GuardedNativePowerGainMode::MoneyOnlyAttacker:
                case GuardedNativePowerGainMode::HitPoints:
                case GuardedNativePowerGainMode::HitPointsOnlyAttacker:
                    break;
                default:
                    return false;
            }
            switch (rules.gainZone)
            {
                case GuardedNativePowerGainZone::Global:
                case GuardedNativePowerGainZone::GlobalCoZoneBonus:
                case GuardedNativePowerGainZone::OnlyCoZone:
                    return true;
                default:
                    return false;
            }
        }

        bool validCombatRules(const GuardedNativeCombatRules & rules)
        {
            if (rules.terrainDefense < 0 || rules.terrainDefense > GUARDED_NATIVE_MAX_TERRAIN_DEFENSE)
            {
                return false;
            }
            switch (rules.formula)
            {
                case GuardedNativeDamageFormula::AdvanceWars4:
                case GuardedNativeDamageFormula::AdvanceWars1To3:
                    return true;
                default:
                    return false;
            }
        }

        bool validCoState(const GuardedNativeCO & co)
        {
            return co.powerMode == static_cast<std::int32_t>(GameEnums::PowerMode_Off) &&
                co.powerStars >= 0 && co.powerStars <= GUARDED_NATIVE_MAX_POWER_STARS &&
                co.superpowerStars >= 0 && co.superpowerStars <= GUARDED_NATIVE_MAX_POWER_STARS &&
                co.powerUsed >= 0 && co.powerUsed <= GUARDED_NATIVE_MAX_POWER_USES &&
                std::isfinite(co.powerFilled) && co.powerFilled >= 0 &&
                co.powerFilled <= co.powerStars + co.superpowerStars;
        }

        bool validEconomyFacts(const GuardedNativeInventory & inventory)
        {
            if (!inventory.gameRuleIds || inventory.gameRuleIds->size() > GUARDED_NATIVE_MAX_GAME_RULES)
            {
                return false;
            }
            for (const auto & id : *inventory.gameRuleIds)
            {
                if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                {
                    return false;
                }
            }
            for (const auto & player : *inventory.playerMaterial)
            {
                if (player.army.isEmpty() || player.army.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    player.buildList.size() > GUARDED_NATIVE_MAX_BUILD_LIST)
                {
                    return false;
                }
                for (const auto & id : player.buildList)
                {
                    if (id.isEmpty() || id.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        bool validGeometry(const GuardedNativeInventory & inventory)
        {
            const auto tiles = static_cast<std::size_t>(inventory.width * inventory.height);
            if (!inventory.terrains || inventory.terrains->size() != tiles)
            {
                return false;
            }
            for (std::size_t tile = 0; tile < tiles; ++tile)
            {
                const auto & terrain = (*inventory.terrains)[tile];
                if (terrain.tile != static_cast<std::int32_t>(tile) || terrain.layers.empty() ||
                    terrain.layers.size() > GUARDED_NATIVE_MAX_TERRAIN_DEPTH)
                {
                    return false;
                }
                for (const auto & layer : terrain.layers)
                {
                    if (layer.type.isEmpty() || layer.type.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                        layer.hp < -1 || layer.hp > GUARDED_NATIVE_MAX_TERRAIN_HP || layer.startOfTurn)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        GuardedNativeBindingStatus materialValidationWork(
            const GuardedNativeInventory & inventory, RefinementLedger & ledger, std::int64_t & work)
        {
            if (!ledger.consume(MATERIAL_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            if (!guardedNativeInventoryWork(inventory.width, inventory.height) ||
                inventory.actors.empty() || inventory.actors.size() > GUARDED_STOCK_MAX_ACTORS ||
                inventory.properties.size() > GUARDED_STOCK_MAX_TILES || !inventory.playerMaterial ||
                !inventory.gameRuleIds || inventory.gameRuleIds->size() > GUARDED_NATIVE_MAX_GAME_RULES ||
                !inventory.terrains || inventory.terrains->size() > GUARDED_STOCK_MAX_TILES)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            const auto shapeWork = MATERIAL_SHAPE_ENTRY_WORK *
                (inventory.terrains->size() + MATERIAL_FIXED_SHAPE_RECORDS);
            if (!ledger.consume(shapeWork, RefinementWork::MasterPreparation))
            {
                return GuardedNativeBindingStatus::BudgetExhausted;
            }
            std::int64_t buildEntries = inventory.gameRuleIds->size();
            std::int64_t perks = 0;
            std::int64_t layers = 0;
            for (const auto & player : *inventory.playerMaterial)
            {
                if (player.buildList.size() > GUARDED_NATIVE_MAX_BUILD_LIST)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                buildEntries += player.buildList.size();
                for (const auto & co : player.cos)
                {
                    if (co)
                    {
                        if (co->effectivePerks.size() > GUARDED_NATIVE_MAX_EFFECTIVE_PERKS)
                        {
                            return GuardedNativeBindingStatus::Unsupported;
                        }
                        perks += co->effectivePerks.size();
                    }
                }
            }
            for (const auto & terrain : *inventory.terrains)
            {
                if (terrain.layers.size() > GUARDED_NATIVE_MAX_TERRAIN_DEPTH)
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                layers += terrain.layers.size();
            }
            work = GUARDED_NATIVE_CO_RULE_WORK + GUARDED_NATIVE_COMBAT_RULE_WORK +
                2 * (256 + 4 * GUARDED_NATIVE_MAX_ID_LENGTH) +
                GUARDED_NATIVE_BUILD_ENTRY_WORK * buildEntries +
                MATERIAL_SHAPE_ENTRY_WORK *
                    (inventory.actors.size() + inventory.properties.size() + inventory.terrains->size()) +
                GUARDED_NATIVE_TERRAIN_LAYER_WORK * layers +
                MATERIAL_ACTOR_WORK * inventory.actors.size() +
                4 * INVENTORY_CO_WORK + INVENTORY_PERK_WORK * perks;
            return GuardedNativeBindingStatus::Bound;
        }
    }

    std::optional<std::int64_t> guardedNativeInventoryWork(std::int32_t width, std::int32_t height)
    {
        if (width <= 0 || width > GUARDED_NATIVE_MAX_WIDTH || height <= 0 ||
            height > GUARDED_NATIVE_MAX_HEIGHT)
        {
            return std::nullopt;
        }
        return GUARDED_NATIVE_HEADER_WORK + INVENTORY_CONTROL_WORK + GUARDED_NATIVE_CO_RULE_WORK +
            GUARDED_NATIVE_COMBAT_RULE_WORK +
            GUARDED_NATIVE_ECONOMY_FACT_WORK +
            GUARDED_NATIVE_TERRAIN_TILE_WORK * width * height +
            INVENTORY_TILE_WORK * width * height +
            INVENTORY_ACTOR_PAIR_WORK * GUARDED_STOCK_MAX_ACTORS * GUARDED_STOCK_MAX_ACTORS +
            4 * (INVENTORY_CO_WORK + INVENTORY_PERK_WORK * GUARDED_NATIVE_MAX_EFFECTIVE_PERKS);
    }

    std::optional<std::int64_t> guardedNativeMaterialWork(std::size_t actors)
    {
        if (actors == 0 || actors > GUARDED_STOCK_MAX_ACTORS)
        {
            return std::nullopt;
        }
        return MATERIAL_CONTROL_WORK + GUARDED_NATIVE_CO_RULE_WORK + GUARDED_NATIVE_COMBAT_RULE_WORK +
            GUARDED_NATIVE_ECONOMY_FACT_WORK +
            GUARDED_NATIVE_TERRAIN_TILE_WORK * GUARDED_STOCK_MAX_TILES +
            MATERIAL_ACTOR_WORK * actors +
            4 * (INVENTORY_CO_WORK + INVENTORY_PERK_WORK * GUARDED_NATIVE_MAX_EFFECTIVE_PERKS);
    }

    GuardedNativeBindingStatus admitGuardedNativeMaterials(
        const GuardedNativeInventory & inventory, RefinementLedger & ledger)
    {
        std::int64_t work = 0;
        const auto priced = materialValidationWork(inventory, ledger, work);
        if (priced != GuardedNativeBindingStatus::Bound)
        {
            return priced;
        }
        if (!ledger.consume(work, RefinementWork::MasterPreparation))
        {
            return GuardedNativeBindingStatus::BudgetExhausted;
        }
        if (!validInventory(inventory) || !inventory.playerMaterial || !inventory.coRules || !inventory.combatRules ||
            !validCoRules(*inventory.coRules) || !validCombatRules(*inventory.combatRules) ||
            !validEconomyFacts(inventory) || !validGeometry(inventory))
        {
            return GuardedNativeBindingStatus::Unsupported;
        }
        for (const auto & actor : inventory.actors)
        {
            if (!validMaterial(actor, inventory))
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
        }
        for (std::size_t side = 0; side < inventory.playerMaterial->size(); ++side)
        {
            const auto & player = (*inventory.playerMaterial)[side];
            if (!std::isfinite(player.fundsModifier) || player.fundsModifier != 1)
            {
                return GuardedNativeBindingStatus::Unsupported;
            }
            for (std::size_t slot = 0; slot < player.cos.size(); ++slot)
            {
                const auto & co = player.cos[slot];
                if (!co)
                {
                    continue;
                }
                if (co->id.isEmpty() || co->id.size() > GUARDED_NATIVE_MAX_ID_LENGTH ||
                    co->effectivePerks.size() > GUARDED_NATIVE_MAX_EFFECTIVE_PERKS ||
                    !validCoState(*co))
                {
                    return GuardedNativeBindingStatus::Unsupported;
                }
                if (co->unitId != NO_UNIT)
                {
                    const auto actor = actorAt(inventory, co->unitId);
                    if (!actor || inventory.actors[*actor].owner != static_cast<std::int32_t>(side) ||
                        inventory.actors[*actor].material->rank != CO_UNIT_RANKS[slot])
                    {
                        return GuardedNativeBindingStatus::Unsupported;
                    }
                }
                for (const auto & perk : co->effectivePerks)
                {
                    if (perk.isEmpty() || perk.size() > GUARDED_NATIVE_MAX_ID_LENGTH)
                    {
                        return GuardedNativeBindingStatus::Unsupported;
                    }
                }
            }
        }
        return GuardedNativeBindingStatus::Bound;
    }

    std::optional<std::int64_t> guardedNativePairWork(std::size_t actors)
    {
        if (actors == 0 || actors > GUARDED_STOCK_MAX_ACTORS)
        {
            return std::nullopt;
        }
        return INVENTORY_CONTROL_WORK +
            AMMUNITION_WORK_PER_ACTOR * GUARDED_STOCK_MAX_ACTORS +
            PLAN_INVENTORY_WORK * (GUARDED_STOCK_MAX_TILES + GUARDED_STOCK_MAX_ACTORS) +
            PLAN_GRAPH_WORK * actors * actors * actors +
            PLAN_ACTOR_PAIR_WORK * actors * GUARDED_STOCK_MAX_ACTORS +
            PLAN_INVENTORY_WORK * actors * GUARDED_STOCK_MAX_TILES +
            PLAN_PATH_POINT_WORK * actors * GUARDED_NATIVE_MAX_PATH_POINTS;
    }

    GuardedNativePairResult bindGuardedNativePair(
        const MasterAssignmentProblem & problem, const GuardedNativeInventory & inventory,
        const MasterAssignmentState & incoming, const MasterAssignmentReplayResult & incomingReplay,
        const MasterAssignmentState & challenger, const MasterAssignmentReplayResult & challengerReplay,
        RefinementLedger & ledger)
    {
        const auto work = guardedNativePairWork(problem.actors().size());
        if (!work || incoming.choices.size() != problem.actors().size() ||
            challenger.choices.size() != problem.actors().size())
        {
            return {};
        }
        if (!ledger.consume(*work, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        GuardedNativePairBinding pair;
        if (!validInventory(inventory) ||
            !std::equal(incoming.choices.begin(), incoming.choices.end(),
                        problem.incumbentChoices().begin(), problem.incumbentChoices().end()) ||
            !bindPlan(problem, inventory, incoming, incomingReplay, GUARDED_NATIVE_MAX_PATH_POINTS, pair.incoming) ||
            !bindPlan(problem, inventory, challenger, challengerReplay, GUARDED_NATIVE_MAX_PATH_POINTS,
                      pair.challenger))
        {
            return {};
        }
        std::optional<std::size_t> changed;
        for (std::size_t index = 0; index < pair.incoming.actions.size(); ++index)
        {
            const auto & before = pair.incoming.actions[index];
            const auto & after = pair.challenger.actions[index];
            if (before != after)
            {
                if (changed || !allowedDifference(before, after))
                {
                    return {GuardedNativeBindingStatus::Unsupported, {}};
                }
                changed = index;
            }
            if (before.kind == PlanBundleKind::Fire || before.kind == PlanBundleKind::MoveAndFire)
            {
                if (before != after || pair.commonFire)
                {
                    return {GuardedNativeBindingStatus::Unsupported, {}};
                }
                pair.commonFire = index;
            }
        }
        if (!changed || (pair.commonFire && *pair.commonFire >= *changed))
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        const auto & action = pair.incoming.actions[*changed];
        const auto tile = inventory.actors[action.actor].tile;
        const auto property = std::find_if(inventory.properties.begin(), inventory.properties.end(),
            [&](const auto & record) { return record.tile == tile; });
        if (property == inventory.properties.end() || property->owner != NO_OWNER)
        {
            return {GuardedNativeBindingStatus::Unsupported, {}};
        }
        pair.changedAction = *changed;
        pair.property = static_cast<std::size_t>(property - inventory.properties.begin());
        return {GuardedNativeBindingStatus::Bound, std::move(pair)};
    }

    std::optional<GuardedNativePlanBounds> guardedNativePlanBounds(
        const MasterAssignmentProblem & problem, const GuardedNativeInventory & inventory, RefinementLedger & ledger)
    {
        const auto actors = problem.actors().size();
        if (actors == 0 || actors > GUARDED_STOCK_MAX_ACTORS || inventory.width <= 0 || inventory.height <= 0 ||
            inventory.width > GUARDED_NATIVE_MAX_WIDTH || inventory.height > GUARDED_NATIVE_MAX_HEIGHT)
        {
            return std::nullopt;
        }
        const auto tiles = static_cast<std::size_t>(inventory.width) * static_cast<std::size_t>(inventory.height);
        if (tiles > GUARDED_STOCK_MAX_TILES)
        {
            return std::nullopt;
        }
        GuardedNativePlanBounds bounds{actors, tiles, inventory.properties.size(), 0};
        if (bounds.properties > tiles)
        {
            return std::nullopt;
        }
        for (const auto & actor : problem.actors())
        {
            if (actor.candidates.size() > GUARDED_STOCK_MAX_TILES ||
                !ledger.consume(PLAN_CANDIDATE_WORK * std::ssize(actor.candidates), RefinementWork::MasterPreparation))
            {
                return std::nullopt;
            }
            for (const auto & candidate : actor.candidates)
            {
                bounds.pathPoints = std::max(bounds.pathPoints, candidate.source.bundle.path.size());
            }
        }
        if (bounds.pathPoints == 0 || bounds.pathPoints > GUARDED_NATIVE_MAX_PATH_POINTS)
        {
            return std::nullopt;
        }
        return bounds;
    }

    std::optional<std::int64_t> guardedNativePlanWork(const GuardedNativePlanBounds & bounds)
    {
        if (bounds.actors == 0 || bounds.actors > GUARDED_STOCK_MAX_ACTORS || bounds.tiles == 0 ||
            bounds.tiles > GUARDED_STOCK_MAX_TILES || bounds.properties > bounds.tiles || bounds.pathPoints == 0 ||
            bounds.pathPoints > GUARDED_NATIVE_MAX_PATH_POINTS)
        {
            return std::nullopt;
        }
        const auto actors = static_cast<std::int64_t>(bounds.actors);
        constexpr auto MAX_ACTORS = static_cast<std::int64_t>(GUARDED_STOCK_MAX_ACTORS);
        // Each action scans at most the inventory's properties and actors to bind its target.
        return INVENTORY_CONTROL_WORK +
            AMMUNITION_WORK_PER_ACTOR * MAX_ACTORS +
            PLAN_INVENTORY_WORK * (static_cast<std::int64_t>(bounds.tiles) + MAX_ACTORS) +
            PLAN_GRAPH_WORK * actors * actors * actors +
            PLAN_ACTOR_PAIR_WORK * actors * MAX_ACTORS +
            PLAN_INVENTORY_WORK * actors * (static_cast<std::int64_t>(bounds.properties) + MAX_ACTORS) +
            PLAN_PATH_POINT_WORK * actors * static_cast<std::int64_t>(bounds.pathPoints);
    }

    GuardedNativePlanResult bindGuardedNativePlan(
        const MasterAssignmentProblem & problem, const GuardedNativeInventory & inventory,
        const GuardedNativePlanBounds & bounds, const MasterAssignmentState & state,
        const MasterAssignmentReplayResult & replay, RefinementLedger & ledger)
    {
        const auto work = guardedNativePlanWork(bounds);
        if (!work || state.choices.size() != problem.actors().size() || bounds.actors != problem.actors().size() ||
            bounds.tiles != static_cast<std::size_t>(inventory.width) * static_cast<std::size_t>(inventory.height) ||
            bounds.properties != inventory.properties.size())
        {
            return {};
        }
        if (!ledger.consume(*work, RefinementWork::MasterPreparation))
        {
            return {GuardedNativeBindingStatus::BudgetExhausted, {}};
        }
        GuardedNativePlanBinding binding;
        if (!validInventory(inventory) || !bindPlan(problem, inventory, state, replay, bounds.pathPoints, binding))
        {
            return {};
        }
        return {GuardedNativeBindingStatus::Bound, std::move(binding)};
    }
}
