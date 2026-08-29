#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/propertystock.h"

class GameMap;

namespace Coordinator
{
    class BattlefieldKnowledge;
    class MobilityFieldCache;

    constexpr std::int32_t NO_PROPERTY_INDEX = -1;

    enum class ActorSurvival : std::int8_t
    {
        Alive,
        Destroyed,
    };

    struct PropertyStockActor
    {
        std::int32_t knowledgeIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
        std::int32_t movementPoints{0};
        std::int32_t carriedCapturePoints{0};
        ActorSurvival survival{ActorSurvival::Alive};
    };

    struct PropertyStockOutcome
    {
        std::int32_t capturedColumn{NO_STOCK_COLUMN};
        std::int32_t destroyedKnowledgeIndex{NO_UNIT};
    };

    struct PlanRowAction
    {
        std::int32_t row{NO_STOCK_ROW};
        TilePoint destination{INVALID_TILE};
        bool captures{false};
    };

    class PropertyStockField
    {
    public:
        MilliFunds originStock() const
        {
            return m_originStock;
        }

        MilliFunds destinationStock(const PropertyStockActor & actor, const PropertyStockOutcome & outcome);
        MilliFunds jointStock(std::span<const PlanRowAction> actions);
        MilliFunds stockCeiling() const;

        std::int32_t propertyIndexAt(const TilePoint & tile) const;
        std::int32_t columnSlotAt(const TilePoint & tile) const;
        std::int32_t ourRowOf(std::int32_t knowledgeIndex) const;
        std::int32_t enemyRowOf(std::int32_t knowledgeIndex) const;

        std::int32_t horizonTurns() const
        {
            return m_horizonTurns;
        }

        friend PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                                          std::span<const PropertyFacts> properties,
                                                          MobilityFieldCache & mobility, std::int32_t horizonTurns);

    private:
        struct ArrivalVectors
        {
            std::vector<std::int32_t> activations;
        };

        struct ArrivalKey
        {
            std::int32_t gridIdentity{NO_STOCK_ROW};
            std::int32_t movementPoints{0};
            std::int32_t x{0};
            std::int32_t y{0};

            friend constexpr auto operator<=>(const ArrivalKey &, const ArrivalKey &) = default;
        };

        struct MarginalKey
        {
            std::int32_t row{NO_STOCK_ROW};
            std::int32_t excludedColumn{NO_STOCK_COLUMN};

            friend constexpr auto operator<=>(const MarginalKey &, const MarginalKey &) = default;
        };

        struct EnemyKey
        {
            std::int32_t flippedColumn{NO_STOCK_COLUMN};
            std::int32_t removedRow{NO_STOCK_ROW};

            friend constexpr auto operator<=>(const EnemyKey &, const EnemyKey &) = default;
        };

        const ArrivalVectors & arrivalsFrom(std::int32_t gridIdentity, std::int32_t movementPoints,
                                            const TilePoint & tile);
        const MarginalTable & marginalFor(std::int32_t row, std::int32_t excludedColumn);
        MilliFunds enemyOptimum(const PropertyStockOutcome & outcome);
        MilliFunds enemyOptimumWith(std::int32_t flippedColumn, std::int32_t removedRow);
        void flipColumnForEnemy(PropertyStockInstance & instance, std::int32_t flippedColumn) const;
        MilliFunds jointEnemyOptimum(const std::vector<std::int32_t> & capturedColumns);
        MilliFunds ourPositionalStock(const PropertyStockActor & actor, const PropertyStockOutcome & outcome);
        std::vector<MilliFunds> actingWeights(std::int32_t row, const PropertyStockActor & actor);
        std::vector<MilliFunds> rowWeights(const PropertyStockInstance & instance,
                                           std::span<const std::int32_t> arrivals,
                                           const PropertyStockRow & row) const;
        std::int32_t carriedFor(const PropertyStockRow & row, std::int32_t slot) const;

        std::int32_t m_horizonTurns{0};
        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::int32_t m_capturePointsToCapture{0};
        MilliFunds m_ownedBaseline{0};
        MilliFunds m_originStock{0};
        MilliFunds m_ourOptimum{0};
        MilliFunds m_enemyOptimum{0};
        PropertyStockInstance m_ours;
        PropertyStockInstance m_theirs;
        std::vector<std::int32_t> m_columnAtTile;
        std::vector<std::int32_t> m_propertyAtTile;
        std::vector<TilePoint> m_columnTiles;
        std::vector<std::int32_t> m_columnCapturer;
        std::vector<std::int32_t> m_columnCapturePoints;
        std::vector<std::int32_t> m_enemyArrivals;
        std::vector<MobilityCostGrid> m_grids;
        ActivationField m_reach;
        std::map<ArrivalKey, ArrivalVectors> m_arrivals;
        std::map<MarginalKey, MarginalTable> m_marginals;
        std::map<EnemyKey, MilliFunds> m_enemyOptima;
        std::map<std::vector<std::int32_t>, MilliFunds> m_jointEnemyOptima;
    };
}
