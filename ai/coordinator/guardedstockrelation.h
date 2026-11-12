#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "ai/coordinator/economicledger.h"
#include "ai/coordinator/refinementledger.h"

namespace Coordinator
{
    constexpr std::size_t GUARDED_STOCK_MAX_ACTORS = 32;
    constexpr std::size_t GUARDED_STOCK_MAX_TILES = 240;
    constexpr std::int32_t GUARDED_STOCK_HORIZON_PAYMENTS = 10;
    constexpr std::int32_t GUARDED_STOCK_CAPTURE_THRESHOLD = 20;
    constexpr std::int32_t GUARDED_STOCK_MAX_MOVEMENT = 10;
    constexpr std::int32_t GUARDED_STOCK_ACTOR_MOVEMENT = 3;
    constexpr std::ptrdiff_t GUARDED_STOCK_DIGEST_BYTES = 32;
    constexpr MilliFunds GUARDED_STOCK_PROPERTY_PAYMENT = 1000000;

    enum class GuardedStockRevision : std::int8_t
    {
        IndependentOrdinal,
        CoupledNativeTerminalV4,
    };

    enum class GuardedStockRule : std::int8_t
    {
        OwnedTownPaymentV1,
    };

    enum class GuardedPropertyKind : std::int8_t
    {
        Town,
        Factory,
        Headquarters,
    };

    enum class GuardedRelationStatus : std::int8_t
    {
        Verified,
        Inapplicable,
        Invalid,
        BudgetExhausted,
        ArithmeticFailure,
    };

    struct GuardedStockActor
    {
        std::int32_t id{-1};
        std::int32_t owner{-1};
        std::int32_t tile{-1};
        double actualHp{0};
        std::int32_t roundedHp{0};
        std::int32_t progress{0};
        std::int32_t fuel{0};
        std::int32_t ammo1{0};
        std::int32_t ammo2{0};
        std::int32_t movement{0};
        bool stock{false};
        bool moved{false};

        bool operator==(const GuardedStockActor &) const = default;
    };

    struct GuardedStockProperty
    {
        std::int32_t tile{-1};
        GuardedPropertyKind kind{GuardedPropertyKind::Town};
        std::int32_t owner{-1};
        MilliFunds payment{0};

        bool operator==(const GuardedStockProperty &) const = default;
    };

    struct GuardedStockRoot
    {
        GuardedStockRevision revision{GuardedStockRevision::IndependentOrdinal};
        std::int32_t tiles{0};
        std::int32_t player{-1};
        std::array<std::int64_t, 2> funds{};
        std::array<std::int32_t, 2> payments{};
        MilliFunds paid{0};
        bool terminal{false};
        std::vector<GuardedStockActor> actors;
        std::vector<GuardedStockProperty> properties;

        bool operator==(const GuardedStockRoot &) const = default;
    };

    struct GuardedStockRoles
    {
        std::size_t town{0};
        std::size_t occupant{0};
        std::size_t headquarters{0};
        std::size_t guard{0};

        bool operator==(const GuardedStockRoles &) const = default;
    };

    struct GuardedStockRelation
    {
        GuardedStockRule rule{GuardedStockRule::OwnedTownPaymentV1};
        GuardedStockRevision revision{GuardedStockRevision::CoupledNativeTerminalV4};
        GuardedStockRoles roles;
        MilliFunds minimumPaymentShift{0};
    };

    struct GuardedStockRelationResult
    {
        GuardedRelationStatus status{GuardedRelationStatus::Invalid};
        std::optional<GuardedStockRelation> relation;
    };

    std::optional<std::int64_t> guardedStockRuleWork(std::size_t actors, std::size_t properties);
    GuardedStockRelationResult constructGuardedStockRelation(
        const GuardedStockRoot & incoming, const GuardedStockRoot & challenger, RefinementLedger & ledger);
    GuardedRelationStatus verifyGuardedStockRelation(
        const GuardedStockRoot & incoming, const GuardedStockRoot & challenger,
        const GuardedStockRelation & relation, RefinementLedger & ledger);
}
