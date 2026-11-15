#pragma once

#include <cstdint>
#include <optional>

#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    enum class StockDeathCause : std::int8_t
    {
        None,
        OwnAction,
        Reply,
    };

    struct StockReplyKeys
    {
        CanonicalPlanActionKey afterOwnActions;
        CanonicalPlanActionKey afterReply;
        StockDeathCause death{StockDeathCause::None};
    };

    inline std::optional<StockReplyKeys> stockReplyKeys(
        const CanonicalPlanActionKey & today, std::int32_t dayStartSteps,
        std::int32_t counterSteps, std::int32_t replySteps)
    {
        if (today.stepsLost() != 0 || today.destroyed() || dayStartSteps <= 0 ||
            dayStartSteps > CanonicalPlanActionKey::MAX_STEPS_LOST || counterSteps < 0 || replySteps < 0 ||
            counterSteps > dayStartSteps || replySteps > dayStartSteps - counterSteps)
        {
            return std::nullopt;
        }
        const bool diedAttacking = counterSteps == dayStartSteps;
        const bool dead = counterSteps + replySteps == dayStartSteps;
        return StockReplyKeys{
            *CanonicalPlanActionKey::checkedFromAction(today.row(), today.x(), today.y(), today.captures(),
                                                       counterSteps, diedAttacking),
            *CanonicalPlanActionKey::checkedFromAction(today.row(), today.x(), today.y(), today.captures(),
                                                       counterSteps + replySteps, dead),
            diedAttacking ? StockDeathCause::OwnAction : dead ? StockDeathCause::Reply : StockDeathCause::None,
        };
    }

    struct StockReplyRow
    {
        CanonicalPlanActionKey today;
        std::int32_t counterSteps{0};
        std::int32_t replySteps{0};
        StockReplyKeys keys;
    };

    // One stock row's key today, after its own action and after the reply, from its hp steps at each point.
    inline std::optional<StockReplyRow> stockReplyRow(std::int32_t row, const TilePoint & destination, bool captures,
                                                      std::int32_t dayStartSteps, std::int32_t stepsAfterOwnActions,
                                                      std::int32_t stepsAfterReply)
    {
        const auto today = CanonicalPlanActionKey::fromAction(row, destination.x, destination.y, captures);
        const std::int32_t counterSteps = dayStartSteps - stepsAfterOwnActions;
        const std::int32_t replySteps = stepsAfterOwnActions - stepsAfterReply;
        const auto keys = stockReplyKeys(today, dayStartSteps, counterSteps, replySteps);
        if (!keys)
        {
            return std::nullopt;
        }
        return StockReplyRow{today, counterSteps, replySteps, *keys};
    }
}