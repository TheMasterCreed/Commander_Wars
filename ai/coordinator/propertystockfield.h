#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/propertystock.h"
#include "ai/coordinator/propertystocksequential.h"

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

    constexpr std::int32_t replyCaptureRate(std::int32_t originalRate, std::int32_t stepsLost, bool destroyed)
    {
        if (destroyed)
        {
            return 0;
        }
        if (stepsLost == 0)
        {
            return originalRate;
        }
        return originalRate > stepsLost ? originalRate - stepsLost : 0;
    }

    struct PropertyStockActor
    {
        std::int32_t knowledgeIndex{NO_UNIT};
        TilePoint tile{INVALID_TILE};
        std::int32_t movementPoints{0};
        std::int32_t carriedCapturePoints{0};
        ActorSurvival survival{ActorSurvival::Alive};
        std::int32_t stepsLost{0};
        bool destroyed{false};

        std::int32_t effectiveCaptureRate(std::int32_t originalRate) const
        {
            return replyCaptureRate(originalRate, stepsLost, destroyed);
        }
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
        std::int32_t stepsLost{0};
        bool destroyed{false};
    };

    inline ArrivalFacts planRowArrival(const PropertyStockActor & mover, std::int32_t originalRate,
                                       const TilePoint & columnTile, std::int32_t arrivalActivations)
    {
        return ArrivalFacts{
            .arrivalActivations = arrivalActivations,
            .carriedPoints = columnTile == mover.tile ? mover.carriedCapturePoints : 0,
            .ratePerTurn = mover.effectiveCaptureRate(originalRate),
        };
    }

    inline PropertyStockActor planRowMover(const PropertyStockRow & row, const PlanRowAction & action,
                                           std::int32_t carried, bool onColumn)
    {
        return PropertyStockActor{
            .knowledgeIndex = row.knowledgeIndex,
            .tile = action.destination,
            .movementPoints = row.movementPoints,
            .carriedCapturePoints = onColumn ? carried + (action.captures ? row.captureRate : 0) : 0,
            .survival = action.destroyed ? ActorSurvival::Destroyed : ActorSurvival::Alive,
            .stepsLost = action.stepsLost,
            .destroyed = action.destroyed,
        };
    }

    // Only rows of candidate actions in a retained future catalog prepare a later list.
    enum class LaterListPreparation : std::int8_t
    {
        Skip,
        Prepare,
    };

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    struct LaterListMeasure
    {
        std::int64_t endpointFloods{0};
        std::int64_t settledWithLimit{0};
        std::int64_t settledWithoutLimit{0};
        std::int64_t rowsPrepared{0};
        std::int64_t completeRows{0};
        std::int64_t unpreparedRows{0};
        std::int64_t overflowRows{0};
        std::int64_t negativeStreamRows{0};
        std::int64_t earlyNodeRows{0};
        std::int64_t earlyNodesRows{0};
        std::int64_t mostQualifyingNodes{0};
        std::int64_t floodNanos{0};
        std::int64_t preparationNanos{0};
    };
#endif

    struct JointStockDecomposition
    {
        std::vector<std::int32_t> capturedColumns;
        MilliFunds ownedBaseline{0};
        MilliFunds ownershipFlipSwingTotal{0};
        MilliFunds ourOpenOptimum{0};
        MilliFunds jointEnemyOptimum{0};
        MilliFunds stockAbsolute{0};
    };

    class PropertyStockField
    {
    public:
        MilliFunds originStock() const
        {
            return m_originStock;
        }

        bool actionCompletesCapture(const PlanRowAction & action) const
        {
            std::int32_t capturedColumn = NO_STOCK_COLUMN;
            actionMover(action, capturedColumn);
            return capturedColumn != NO_STOCK_COLUMN;
        }

        MilliFunds destinationStock(const PropertyStockActor & actor, const PropertyStockOutcome & outcome);
        MilliFunds jointStock(std::span<const PlanRowAction> actions);
        JointStockDecomposition jointStockDecomposition(
            std::span<const PlanRowAction> actions);
        MilliFunds stockCeiling() const;

        std::int32_t propertyIndexAt(const TilePoint & tile) const;
        std::int32_t columnSlotAt(const TilePoint & tile) const;
        std::int32_t ourRowOf(std::int32_t knowledgeIndex) const;
        std::int32_t enemyRowOf(std::int32_t knowledgeIndex) const;

        std::int32_t horizonTurns() const
        {
            return m_horizonTurns;
        }

        std::int32_t capturePointsToCapture() const
        {
            return m_capturePointsToCapture;
        }

        // The enemy instance's mirrored column at a slot from columnSlotAt.
        const PropertyStockColumn & enemyColumn(std::int32_t slot) const
        {
            return m_theirs.columns[static_cast<std::size_t>(slot)];
        }

        MilliFunds enemyWeight(std::int32_t row, std::int32_t slot) const
        {
            return m_theirs.weights[static_cast<std::size_t>(row) * m_theirs.columns.size() + static_cast<std::size_t>(slot)];
        }

        friend PropertyStockField buildPropertyStockField(GameMap & map, const BattlefieldKnowledge & knowledge,
                                                          std::span<const PropertyFacts> properties,
                                                          MobilityFieldCache & mobility, std::int32_t horizonTurns);
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        friend struct PropertyStockDiagnosticFixture;
#endif
        friend class JointPlanStockValuer;
        friend class MasterAssignmentStockContext;
        friend class ReplyStockContext;

    private:
        static std::int32_t tileSlot(std::int32_t width, std::int32_t height, std::int32_t x, std::int32_t y);

        struct JointActionFacts
        {
            std::vector<std::int32_t> capturedColumns;
            std::vector<std::int32_t> moverRows;
            std::vector<PropertyStockActor> movers;
        };

        struct ArrivalVectors
        {
            std::vector<std::int32_t> activations;
            std::vector<std::int32_t> spent;
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

        const ArrivalVectors & arrivalsFrom(std::int32_t gridIdentity, std::int32_t movementPoints,
                                            const TilePoint & tile);
        const ArrivalVectors & rawArrivalsFrom(std::int32_t gridIdentity, std::int32_t movementPoints,
                                               const TilePoint & tile);
        ArrivalVectors readArrivals(std::int32_t readLimit) const;
        void prepareLaterList(const PropertyStockRow & row, const PropertyStockActor & mover,
                              std::span<const std::int32_t> nodeSlots, SequentialLaterList & later);
        PropertyStockActor actionMover(const PlanRowAction & action, std::int32_t & capturedColumn) const;
        JointActionFacts jointActionFacts(std::span<const PlanRowAction> actions) const;
        MilliFunds ourOpenOptimum(const JointActionFacts & facts);
        const MarginalTable & marginalFor(std::int32_t row, std::int32_t excludedColumn);
        MilliFunds enemyOptimumWithout(std::int32_t removedRow);
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
        // Kept apart from m_arrivals so a raw request never answers a clipped one, nor the reverse.
        std::map<ArrivalKey, ArrivalVectors> m_rawArrivals;
        std::map<MarginalKey, MarginalTable> m_marginals;
        std::map<std::int32_t, MilliFunds> m_enemyOptimaWithoutRow;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        LaterListMeasure m_laterMeasure;
#endif
    };
}
