#pragma once

#include <cstdint>

#include "ai/coordinator/turnplan.h"
#include "game/gameaction.h"

class GameMap;
class Player;

namespace Coordinator
{
    enum class EngineActionFailure : std::int8_t
    {
        None,
        InvalidShape,
        ActorUnavailable,
        OriginMismatch,
        IllegalAction,
        TargetUnavailable,
        InvalidTargetStep,
    };

    struct EngineActionBuildResult
    {
        spGameAction action;
        EngineActionFailure failure{EngineActionFailure::None};

        explicit operator bool() const
        {
            return action != nullptr;
        }
    };

    EngineActionBuildResult buildEngineAction(GameMap & map, Player & player,
                                              const PlannedAction & planned);
}
