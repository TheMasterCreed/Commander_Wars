#include "ai/coordinator/engineactionbuilder.h"

#include <utility>

#include <QPoint>

#include "ai/coordinator/bundlebuilder.h"
#include "coreengine/memorymanagement.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/unitpathfindingsystem.h"

namespace
{
constexpr bool EMPTY_FIELD_ACTION = false;

bool isFireAction(Coordinator::PlanBundleKind kind)
{
    return kind == Coordinator::PlanBundleKind::Fire ||
           kind == Coordinator::PlanBundleKind::MoveAndFire;
}

qint32 liveUnitIdAt(GameMap & map, Coordinator::TilePoint tile)
{
    if (!map.onMap(tile.x, tile.y))
    {
        return Coordinator::NO_UNIT;
    }
    Terrain* pTerrain = map.getTerrain(tile.x, tile.y);
    if (pTerrain == nullptr || pTerrain->getUnit() == nullptr)
    {
        return Coordinator::NO_UNIT;
    }
    return pTerrain->getUnit()->getUniqueID();
}

Unit* resolveActor(GameMap & map, Player & player, qint32 unitId)
{
    Unit* pUnit = map.getUnit(unitId);
    if (pUnit == nullptr || pUnit->getOwner() != &player)
    {
        return nullptr;
    }
    return pUnit;
}

Coordinator::EngineActionBuildResult reject(Coordinator::EngineActionFailure failure)
{
    return Coordinator::EngineActionBuildResult{spGameAction(), failure};
}
}

Coordinator::EngineActionBuildResult Coordinator::buildEngineAction(
    GameMap & map, Player & player, const PlannedAction & planned)
{
    if (planned.actionId.isEmpty() || planned.path.empty())
    {
        return reject(EngineActionFailure::InvalidShape);
    }
    Unit* pUnit = resolveActor(map, player, planned.unitId);
    if (pUnit == nullptr || pUnit->getHasMoved())
    {
        return reject(EngineActionFailure::ActorUnavailable);
    }
    const TilePoint origin = planned.path.front();
    if (pUnit->Unit::getX() != origin.x || pUnit->Unit::getY() != origin.y)
    {
        return reject(EngineActionFailure::OriginMismatch);
    }

    const std::vector<QPoint> enginePath = pathToEngineOrder(planned.path);
    UnitPathFindingSystem pfs(&map, pUnit, &player);
    pfs.explore();
    spGameAction pAction = MemoryManagement::create<GameAction>(planned.actionId, &map);
    pAction->setTarget(QPoint(origin.x, origin.y));
    pAction->setMovepath(enginePath, pfs.getCosts(enginePath));
    if (!pAction->canBePerformed(planned.actionId, EMPTY_FIELD_ACTION, &player))
    {
        return reject(EngineActionFailure::IllegalAction);
    }
    if (!isFireAction(planned.kind))
    {
        return EngineActionBuildResult{std::move(pAction), EngineActionFailure::None};
    }
    if (liveUnitIdAt(map, planned.target) != planned.targetUnitId)
    {
        return reject(EngineActionFailure::TargetUnavailable);
    }
    pAction->writeDataInt32(planned.target.x);
    pAction->writeDataInt32(planned.target.y);
    pAction->setInputStep(pAction->getInputStep() + 1);
    if (!pAction->isFinalStep() || !pAction->canBePerformed())
    {
        return reject(EngineActionFailure::InvalidTargetStep);
    }
    return EngineActionBuildResult{std::move(pAction), EngineActionFailure::None};
}
