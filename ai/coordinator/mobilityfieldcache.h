#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "ai/coordinator/mobilityfield.h"

class GameMap;
class Player;

namespace Coordinator
{
    // Keyed by unit id: weather immunity and CO hooks dispatch on it.
    struct MobilityProfile
    {
        qint32 playerId{-1};
        QString unitId;

        friend bool operator==(const MobilityProfile &, const MobilityProfile &) = default;
    };

    // What Unit::getMovementCosts reads besides unit id and owner.
    struct MobilityEpoch
    {
        static constexpr qint32 CO_SLOT_COUNT = 2;
        static constexpr qint32 NO_CO = -1;
        static constexpr qint32 NO_OWNER = -1;
        static constexpr qint32 NO_WEATHER = -1;

        qint32 terrainGeneration{0};
        qint32 weatherId{NO_WEATHER};
        std::vector<qint32> playerTeams;
        QStringList coIds;
        QStringList coPerks;
        std::vector<qint32> coPowerModes;
        std::vector<qint32> buildingOwners;

        static MobilityEpoch capture(GameMap & map);

        friend bool operator==(const MobilityEpoch &, const MobilityEpoch &) = default;
    };

    class MobilityFieldCache
    {
    public:
        // The reference stays valid until the next grid() call, which may detect a new epoch and drop every grid.
        const MobilityCostGrid & grid(GameMap & map, Player & owner, const QString & unitId);
        void clear();

        qint32 profileCount() const
        {
            return static_cast<qint32>(m_entries.size());
        }

        qint32 gridCount() const
        {
            return static_cast<qint32>(m_grids.size());
        }

        qint32 buildCount() const
        {
            return m_buildCount;
        }

        // Survives clear() so tests can observe epoch driven invalidation.
        qint32 epochChangeCount() const
        {
            return m_epochChanges;
        }

    private:
        struct Entry
        {
            MobilityProfile profile;
            std::size_t gridIndex{0};
        };

        std::size_t entryIndex(GameMap & map, Player & owner, const QString & unitId);

        MobilityEpoch m_epoch;
        bool m_hasEpoch{false};
        std::vector<Entry> m_entries;
        // Owning pointers keep grid() references valid across later inserts.
        std::vector<std::unique_ptr<MobilityCostGrid>> m_grids;
        qint32 m_buildCount{0};
        qint32 m_epochChanges{0};
    };
}
