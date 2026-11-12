#include "ai/coordinator/guardedstockrelation.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ai/coordinator/relativestockcomparison.h"

namespace Coordinator
{
    namespace
    {
        constexpr std::int64_t RULE_FIXED_WORK = 128;
        constexpr std::int64_t ACTOR_FIELD_PASSES = 192;
        constexpr std::int64_t PROPERTY_FIELD_PASSES = 96;

        bool validRoot(const GuardedStockRoot & root)
        {
            if (root.revision != GuardedStockRevision::CoupledNativeTerminalV4 ||
                root.tiles <= 0 || root.tiles > static_cast<std::int32_t>(GUARDED_STOCK_MAX_TILES) ||
                root.player != 0 || root.terminal || root.actors.empty() ||
                root.actors.size() > GUARDED_STOCK_MAX_ACTORS || root.properties.size() < 2 ||
                root.properties.size() > static_cast<std::size_t>(root.tiles) ||
                root.payments[0] < 0 || root.payments[0] >= GUARDED_STOCK_HORIZON_PAYMENTS ||
                root.payments[1] != root.payments[0] ||
                root.funds[0] < 0 || root.funds[1] < 0)
            {
                return false;
            }
            std::array<bool, GUARDED_STOCK_MAX_TILES> occupied{};
            std::array<bool, GUARDED_STOCK_MAX_TILES> properties{};
            std::array<std::size_t, 2> living{};
            std::array<std::size_t, 2> headquarters{};
            MilliFunds rate = 0;
            std::int32_t previousId = -1;
            for (const auto & actor : root.actors)
            {
                if (actor.id <= previousId || actor.owner < 0 || actor.owner > 1 ||
                    actor.tile < 0 || actor.tile >= root.tiles || occupied[actor.tile] ||
                    !std::isfinite(actor.actualHp) || actor.actualHp <= 0 || actor.actualHp > UNIT_HP_STEPS ||
                    actor.roundedHp != static_cast<std::int32_t>(std::ceil(actor.actualHp)) ||
                    actor.progress < 0 || actor.progress >= GUARDED_STOCK_CAPTURE_THRESHOLD ||
                    actor.fuel < 0 || actor.ammo1 < 0 || actor.ammo2 < 0 ||
                    actor.movement < 0 || actor.movement > GUARDED_STOCK_MAX_MOVEMENT ||
                    (actor.stock && actor.movement != GUARDED_STOCK_ACTOR_MOVEMENT))
                {
                    return false;
                }
                previousId = actor.id;
                occupied[actor.tile] = true;
                ++living[actor.owner];
            }
            for (const auto & property : root.properties)
            {
                if (property.tile < 0 || property.tile >= root.tiles || properties[property.tile] ||
                    property.owner < -1 || property.owner > 1 ||
                    property.payment != GUARDED_STOCK_PROPERTY_PAYMENT ||
                    !RelativeStockDetail::checkedAdd(rate, property.payment))
                {
                    return false;
                }
                properties[property.tile] = true;
                if (property.kind == GuardedPropertyKind::Headquarters)
                {
                    if (property.owner < 0)
                    {
                        return false;
                    }
                    ++headquarters[property.owner];
                }
                else if (property.kind != GuardedPropertyKind::Town &&
                         property.kind != GuardedPropertyKind::Factory)
                {
                    return false;
                }
            }
            if (living[0] == 0 || living[1] == 0 || headquarters != std::array<std::size_t, 2>{1, 1})
            {
                return false;
            }
            const auto absolutePaid = root.paid < 0
                ? static_cast<std::uint64_t>(-(root.paid + 1)) + 1 : static_cast<std::uint64_t>(root.paid);
            return absolutePaid <= static_cast<std::uint64_t>(std::numeric_limits<MilliFunds>::max()) &&
                rate <= (std::numeric_limits<MilliFunds>::max() - static_cast<MilliFunds>(absolutePaid)) /
                    (GUARDED_STOCK_HORIZON_PAYMENTS + 1);
        }

        bool sameClock(const GuardedStockRoot & incoming, const GuardedStockRoot & challenger)
        {
            return incoming.tiles == challenger.tiles && incoming.player == challenger.player &&
                incoming.funds == challenger.funds && incoming.payments == challenger.payments &&
                incoming.paid == challenger.paid && incoming.terminal == challenger.terminal &&
                incoming.actors.size() == challenger.actors.size() &&
                incoming.properties.size() == challenger.properties.size();
        }

        bool prepayRule(const GuardedStockRoot & incoming, const GuardedStockRoot & challenger,
                        RefinementLedger & ledger)
        {
            const auto actors = std::max(incoming.actors.size(), challenger.actors.size());
            const auto properties = std::max(incoming.properties.size(), challenger.properties.size());
            const auto work = guardedStockRuleWork(actors, properties);
            return work && ledger.consume(*work, RefinementWork::MasterWitness);
        }
    }

    std::optional<std::int64_t> guardedStockRuleWork(std::size_t actors, std::size_t properties)
    {
        if (actors > GUARDED_STOCK_MAX_ACTORS || properties > GUARDED_STOCK_MAX_TILES)
        {
            return std::nullopt;
        }
        return RULE_FIXED_WORK + ACTOR_FIELD_PASSES * static_cast<std::int64_t>(actors) +
            PROPERTY_FIELD_PASSES * static_cast<std::int64_t>(properties);
    }

    GuardedStockRelationResult constructGuardedStockRelation(
        const GuardedStockRoot & incoming, const GuardedStockRoot & challenger, RefinementLedger & ledger)
    {
        if (!prepayRule(incoming, challenger, ledger))
        {
            return {GuardedRelationStatus::BudgetExhausted, std::nullopt};
        }
        if (!validRoot(incoming) || !validRoot(challenger) || !sameClock(incoming, challenger))
        {
            return {GuardedRelationStatus::Inapplicable, std::nullopt};
        }
        std::optional<std::size_t> town;
        std::optional<std::size_t> headquarters;
        for (std::size_t property = 0; property < incoming.properties.size(); ++property)
        {
            const auto & before = incoming.properties[property];
            const auto & after = challenger.properties[property];
            if (before.kind == GuardedPropertyKind::Headquarters && before.owner == 0)
            {
                headquarters = property;
            }
            if (before != after)
            {
                if (town || before.tile != after.tile || before.kind != after.kind ||
                    before.payment != after.payment || before.kind != GuardedPropertyKind::Town ||
                    before.owner != -1 || after.owner != 0)
                {
                    return {GuardedRelationStatus::Inapplicable, std::nullopt};
                }
                town = property;
            }
        }
        if (!town || !headquarters)
        {
            return {GuardedRelationStatus::Inapplicable, std::nullopt};
        }
        std::optional<std::size_t> occupant;
        std::optional<std::size_t> guard;
        for (std::size_t actor = 0; actor < incoming.actors.size(); ++actor)
        {
            const auto & before = incoming.actors[actor];
            const auto & after = challenger.actors[actor];
            auto expected = before;
            if (before.tile == incoming.properties[*town].tile)
            {
                if (before.owner != 0 || !before.stock ||
                    before.progress + before.roundedHp < GUARDED_STOCK_CAPTURE_THRESHOLD)
                {
                    return {GuardedRelationStatus::Inapplicable, std::nullopt};
                }
                expected.progress = 0;
                occupant = actor;
            }
            if (before != after && expected != after)
            {
                return {GuardedRelationStatus::Inapplicable, std::nullopt};
            }
            if (before.owner == 0)
            {
                if (before.actualHp != UNIT_HP_STEPS || !before.moved ||
                    (before.stock && before.fuel <
                        before.movement * (GUARDED_STOCK_HORIZON_PAYMENTS - incoming.payments[0])))
                {
                    return {GuardedRelationStatus::Inapplicable, std::nullopt};
                }
                if (before.tile == incoming.properties[*headquarters].tile)
                {
                    guard = actor;
                }
            }
        }
        if (!occupant || !guard || challenger.actors[*occupant].progress != 0)
        {
            return {GuardedRelationStatus::Inapplicable, std::nullopt};
        }
        GuardedStockRelation relation;
        relation.roles = {*town, *occupant, *headquarters, *guard};
        relation.minimumPaymentShift = incoming.properties[*town].payment;
        return {GuardedRelationStatus::Verified, relation};
    }

    GuardedRelationStatus verifyGuardedStockRelation(
        const GuardedStockRoot & incoming, const GuardedStockRoot & challenger,
        const GuardedStockRelation & relation, RefinementLedger & ledger)
    {
        if (!prepayRule(incoming, challenger, ledger))
        {
            return GuardedRelationStatus::BudgetExhausted;
        }
        const auto & roles = relation.roles;
        if (relation.rule != GuardedStockRule::OwnedTownPaymentV1 ||
            relation.revision != GuardedStockRevision::CoupledNativeTerminalV4 ||
            !validRoot(incoming) || !validRoot(challenger) || !sameClock(incoming, challenger) ||
            roles.town >= incoming.properties.size() || roles.headquarters >= incoming.properties.size() ||
            roles.occupant >= incoming.actors.size() || roles.guard >= incoming.actors.size())
        {
            return GuardedRelationStatus::Invalid;
        }
        const auto & town = incoming.properties[roles.town];
        const auto & headquarters = incoming.properties[roles.headquarters];
        const auto & occupant = incoming.actors[roles.occupant];
        const auto & guard = incoming.actors[roles.guard];
        if (town.kind != GuardedPropertyKind::Town || town.owner != -1 ||
            headquarters.kind != GuardedPropertyKind::Headquarters || headquarters.owner != 0 ||
            occupant.tile != town.tile || occupant.owner != 0 || !occupant.stock ||
            occupant.progress + occupant.roundedHp < GUARDED_STOCK_CAPTURE_THRESHOLD ||
            guard.tile != headquarters.tile || guard.owner != 0 ||
            relation.minimumPaymentShift <= 0 || relation.minimumPaymentShift != town.payment)
        {
            return GuardedRelationStatus::Invalid;
        }
        for (std::size_t property = 0; property < incoming.properties.size(); ++property)
        {
            const auto & before = incoming.properties[property];
            const auto & after = challenger.properties[property];
            if (property == roles.town)
            {
                if (after.tile != before.tile || after.kind != before.kind ||
                    after.payment != before.payment || after.owner != 0)
                {
                    return GuardedRelationStatus::Invalid;
                }
            }
            else if (before != after)
            {
                return GuardedRelationStatus::Invalid;
            }
        }
        for (std::size_t actor = 0; actor < incoming.actors.size(); ++actor)
        {
            const auto & before = incoming.actors[actor];
            auto expected = before;
            if (actor == roles.occupant)
            {
                expected.progress = 0;
            }
            if (expected != challenger.actors[actor] ||
                (before.owner == 0 && (before.actualHp != UNIT_HP_STEPS || !before.moved ||
                    (before.stock && before.fuel <
                        before.movement * (GUARDED_STOCK_HORIZON_PAYMENTS - incoming.payments[0])))))
            {
                return GuardedRelationStatus::Invalid;
            }
        }
        return GuardedRelationStatus::Verified;
    }
}
