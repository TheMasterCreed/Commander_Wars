#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>

#include <QString>

#include "ai/coordinator/attackopportunityfield.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockfield.h"
#include "ai/coordinator/replyfacts.h"

class GameMap;
class Unit;

namespace Coordinator
{
    TerrainDefenseRule projectedTerrainRule(GameMap &map, Unit &actor, const TilePoint &tile);
    bool ownBuildingAt(GameMap & map, const TilePoint & tile, std::int32_t ownerId);
    ReplyBoard replyBoardOf(const BattlefieldKnowledge & knowledge);

    // No engine damage call; the scripts it does run are weapon, unit-type, terrain and our actor's own.
    class ReplyFactsBuilder
    {
    public:
        ReplyFactsBuilder(GameMap & map, const BattlefieldKnowledge & knowledge,
                          const AttackOpportunityField & enemyReach, std::span<const PropertyFacts> properties,
                          const PropertyStockField & propertyStock, DamageOracle & oracle);

        std::shared_ptr<const ReplyTileFacts> tileFacts(const TilePoint & target);
        std::shared_ptr<const ReplyFacts> factsFor(std::int32_t actorIndex, Unit & actor,
                                                   const TilePoint & destination);

        void countCandidate()
        {
            ++m_stats.candidates;
        }

        const ReplyFactsStats & stats() const
        {
            return m_stats;
        }

    private:
        struct TileKey
        {
            std::int32_t x{0};
            std::int32_t y{0};

            friend constexpr auto operator<=>(const TileKey &, const TileKey &) = default;
        };

        struct ActorKey
        {
            std::int32_t actorIndex{NO_UNIT};
            TileKey tile;

            friend constexpr auto operator<=>(const ActorKey &, const ActorKey &) = default;
        };

        const KnownUnit & known(std::int32_t knowledgeIndex) const;
        std::shared_ptr<const ReplyTileFacts> cachedTileFacts(const TilePoint & target);
        ReplyShooterFacts shooterFacts(const EnemyShot & shot, const TilePoint & target);
        void priceShooter(ReplyShooterFacts & shooter);


        GameMap & m_map;
        const BattlefieldKnowledge & m_knowledge;
        const AttackOpportunityField & m_enemyReach;
        std::span<const PropertyFacts> m_properties;
        const PropertyStockField & m_propertyStock;
        DamageOracle & m_oracle;
        PlannedOccupancy m_emptyBoard;
        std::map<TileKey, std::shared_ptr<const ReplyTileFacts>> m_tiles;
        std::map<ActorKey, std::shared_ptr<const ReplyFacts>> m_actors;
        std::map<QString, MilliFunds> m_baseCosts;
        ReplyFactsStats m_stats;
    };
}
