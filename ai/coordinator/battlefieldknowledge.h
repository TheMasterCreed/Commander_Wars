#pragma once

#include <cstddef>
#include <vector>

#include <QString>
#include <QtGlobal>

#include "ai/coordinator/coordinatorcommon.h"
#include "game/GameEnums.h"

class GameMap;
class Player;

namespace Coordinator
{
    enum class Relation
    {
        Own,
        Allied,
        Enemy,
        Neutral,
    };

    struct KnownUnit
    {
        static constexpr qint32 NO_CARRIER = -1;

        qint32 x{0};
        qint32 y{0};
        QString unitId;
        qint32 ownerId{NO_OWNER};
        qint32 hpRounded{0};
        qint32 ammo1{0};
        qint32 ammo2{0};
        qint32 fuel{0};
        bool hasMoved{false};
        bool ignoresUnitCollision{false};
        // Transport's units() index; unitAt() skips cargo.
        qint32 carrier{NO_CARRIER};
        qint32 unitValue{0};
        bool hasWeapons{false};
        qint32 capturePoints{0};
        // Own tile facts, fuel trimmed like the pathfinder.
        qint32 movementPoints{0};
        qint32 minRange{1};
        qint32 maxRange{1};
        bool canMoveAndFire{false};
        bool canFire{false};
        // Capability; captureRate is a fact about this tile only.
        bool canCapture{false};
        qint32 captureRate{0};
    };

    struct KnownBuilding
    {
        qint32 x{0};
        qint32 y{0};
        QString buildingId;
        qint32 ownerId{NO_OWNER};
    };

    // What fog hides is absent from this snapshot, not flagged.
    class BattlefieldKnowledge
    {
    public:
        BattlefieldKnowledge() = default;

        static BattlefieldKnowledge capture(GameMap & map, Player & observer);

        static BattlefieldKnowledge forTest(qint32 width, qint32 height, std::vector<KnownUnit> units,
                                            qint32 observerId = 0,
                                            std::vector<Relation> relations = {Relation::Own, Relation::Enemy})
        {
            BattlefieldKnowledge k;
            k.m_observerId = observerId;
            k.m_width = width;
            k.m_height = height;
            k.m_units = std::move(units);
            k.m_relations = std::move(relations);
            const std::size_t cells = width > 0 && height > 0
                ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) : 0;
            k.m_unitIndex.assign(cells, NO_UNIT);
            k.m_visible.assign(cells, true);
            for (std::size_t i = 0; i < k.m_units.size(); ++i)
            {
                const auto & u = k.m_units[i];
                if (u.carrier == KnownUnit::NO_CARRIER && u.x >= 0 && u.x < width && u.y >= 0 && u.y < height)
                    k.m_unitIndex[static_cast<std::size_t>(u.y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(u.x)] = static_cast<qint32>(i);
            }
            return k;
        }

        qint32 observerId() const
        {
            return m_observerId;
        }

        qint32 width() const
        {
            return m_width;
        }

        qint32 height() const
        {
            return m_height;
        }

        const std::vector<KnownUnit> & units() const
        {
            return m_units;
        }

        const std::vector<KnownBuilding> & buildings() const
        {
            return m_buildings;
        }

        // Current turn trim only; zero does not imply the multi turn relaxation is inactive.
        qint32 currentTurnFuelTrimmedCappers() const
        {
            return m_currentTurnFuelTrimmedCappers;
        }

        const KnownUnit* unitAt(qint32 x, qint32 y) const;
        // NO_UNIT off map or on an empty tile; units() index otherwise
        qint32 unitIndexAt(qint32 x, qint32 y) const;
        // Player::getFieldVisible semantics: true under mist and no fog.
        bool isVisible(qint32 x, qint32 y) const;
        GameEnums::VisionType visionType(qint32 x, qint32 y) const;
        Relation relation(qint32 ownerId) const
        {
            if (ownerId < 0 || ownerId >= static_cast<qint32>(m_relations.size()))
            {
                return Relation::Neutral;
            }
            return m_relations[static_cast<std::size_t>(ownerId)];
        }

    private:
        bool onMap(qint32 x, qint32 y) const;
        std::size_t tileIndex(qint32 x, qint32 y) const;

        qint32 m_observerId{NO_OWNER};
        qint32 m_width{0};
        qint32 m_height{0};
        qint32 m_currentTurnFuelTrimmedCappers{0};
        std::vector<KnownUnit> m_units;
        std::vector<KnownBuilding> m_buildings;
        std::vector<qint32> m_unitIndex;
        std::vector<bool> m_visible;
        std::vector<GameEnums::VisionType> m_visionType;
        std::vector<Relation> m_relations;
    };
}
