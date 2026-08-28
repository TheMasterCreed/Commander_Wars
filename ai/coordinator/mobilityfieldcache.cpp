#include "ai/coordinator/mobilityfieldcache.h"

#include "coreengine/memorymanagement.h"

#include "game/building.h"
#include "game/co.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace
{
    qint32 normalizeCost(qint32 cost)
    {
        return cost < 0 ? Coordinator::IMPASSABLE_COST : cost;
    }

    // Terrain scripts receive the from tile, so every entry direction is probed even for origin independent movement tables.
    Coordinator::MobilityCostGrid buildMobilityGrid(GameMap & map, Player & owner, const QString & unitId)
    {
        const qint32 width = map.getMapWidth();
        const qint32 height = map.getMapHeight();
        Coordinator::MobilityCostGrid grid(width, height);
        spUnit pUnit = MemoryManagement::create<Unit>(unitId, &owner, false, &map);
        for (qint32 x = 0; x < width; ++x)
        {
            for (qint32 y = 0; y < height; ++y)
            {
                for (const Coordinator::EntryDirection direction : Coordinator::ENTRY_DIRECTIONS)
                {
                    const Coordinator::StepVector step = Coordinator::entryStep(direction);
                    const qint32 fromX = x - step.dx;
                    const qint32 fromY = y - step.dy;
                    if (map.onMap(fromX, fromY))
                    {
                        grid.setCost(x, y, direction, normalizeCost(pUnit->getMovementCosts(x, y, fromX, fromY)));
                    }
                    else
                    {
                        grid.setCost(x, y, direction, Coordinator::IMPASSABLE_COST);
                    }
                }
            }
        }
        return grid;
    }
}

namespace Coordinator
{
    MobilityEpoch MobilityEpoch::capture(GameMap & map)
    {
        MobilityEpoch epoch;
        epoch.terrainGeneration = map.getTerrainGeneration();
        GameRules* pRules = map.getGameRules();
        if (pRules != nullptr)
        {
            epoch.weatherId = pRules->getCurrentWeatherId();
        }
        const qint32 playerCount = map.getPlayerCount();
        epoch.playerTeams.reserve(static_cast<std::size_t>(playerCount));
        epoch.coIds.reserve(playerCount * CO_SLOT_COUNT);
        epoch.coPowerModes.reserve(static_cast<std::size_t>(playerCount) * CO_SLOT_COUNT);
        for (qint32 player = 0; player < playerCount; ++player)
        {
            Player* pPlayer = map.getPlayer(player);
            epoch.playerTeams.push_back(pPlayer != nullptr ? pPlayer->getTeam() : NO_OWNER);
            for (qint32 slot = 0; slot < CO_SLOT_COUNT; ++slot)
            {
                CO* pCO = pPlayer != nullptr ? pPlayer->getCO(static_cast<quint8>(slot)) : nullptr;
                epoch.coIds.append(pCO != nullptr ? pCO->getCoID() : QString());
                epoch.coPerks.append(pCO != nullptr ? pCO->getPerkList().join(QChar(',')) : QString());
                epoch.coPowerModes.push_back(pCO != nullptr ? static_cast<qint32>(pCO->getPowerMode()) : NO_CO);
            }
        }
        // Building::setOwner never touches the terrain generation, so ownership is fingerprinted directly.
        const qint32 width = map.getMapWidth();
        const qint32 height = map.getMapHeight();
        for (qint32 y = 0; y < height; ++y)
        {
            for (qint32 x = 0; x < width; ++x)
            {
                Building* pBuilding = map.getTerrain(x, y)->getBuilding();
                if (pBuilding == nullptr)
                {
                    continue;
                }
                Player* pOwner = pBuilding->getOwner();
                epoch.buildingOwners.push_back(pOwner != nullptr ? pOwner->getPlayerID() : NO_OWNER);
            }
        }
        return epoch;
    }

    const MobilityCostGrid & MobilityFieldCache::grid(GameMap & map, Player & owner, const QString & unitId)
    {
        const Entry & entry = m_entries[entryIndex(map, owner, unitId)];
        return *m_grids[entry.gridIndex];
    }

    void MobilityFieldCache::clear()
    {
        m_entries.clear();
        m_grids.clear();
        m_epoch = MobilityEpoch();
        m_hasEpoch = false;
        m_buildCount = 0;
    }

    std::size_t MobilityFieldCache::entryIndex(GameMap & map, Player & owner, const QString & unitId)
    {
        MobilityEpoch epoch = MobilityEpoch::capture(map);
        if (!m_hasEpoch || !(epoch == m_epoch))
        {
            if (m_hasEpoch)
            {
                ++m_epochChanges;
            }
            clear();
            m_epoch = std::move(epoch);
            m_hasEpoch = true;
        }
        const MobilityProfile profile{owner.getPlayerID(), unitId};
        for (std::size_t entry = 0; entry < m_entries.size(); ++entry)
        {
            if (m_entries[entry].profile == profile)
            {
                return entry;
            }
        }
        MobilityCostGrid grid = buildMobilityGrid(map, owner, unitId);
        ++m_buildCount;
        std::size_t gridIndex = m_grids.size();
        for (std::size_t candidate = 0; candidate < m_grids.size(); ++candidate)
        {
            if (*m_grids[candidate] == grid)
            {
                gridIndex = candidate;
                break;
            }
        }
        if (gridIndex == m_grids.size())
        {
            m_grids.push_back(std::make_unique<MobilityCostGrid>(std::move(grid)));
        }
        m_entries.push_back(Entry{profile, gridIndex});
        return m_entries.size() - 1;
    }
}
