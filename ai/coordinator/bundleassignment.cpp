#include "ai/coordinator/bundleassignment.h"

#include "ai/coreai.h"

namespace Coordinator
{
    PlanActionIds engineActionIds()
    {
        return PlanActionIds{
            .wait = QString(CoreAI::ACTION_WAIT),
            .fire = QString(CoreAI::ACTION_FIRE),
            .capture = QString(CoreAI::ACTION_CAPTURE),
        };
    }
}
