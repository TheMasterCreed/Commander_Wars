#pragma once

#include <cstdint>
#include <vector>

#include "ai/coordinator/continuationinterval.h"
#include "ai/coordinator/coordinatorcommon.h"
#include "ai/coordinator/economicledger.h"

namespace Coordinator
{
    class TurnPlan;

    struct LivePlanStockQuote
    {
        ContinuationKey key;
        StockInterval stockAbsolute;
        bool valid{false};
        bool lowerWitnessReplays{false};
    };

    struct SequentialStockAudit
    {
        MilliFunds floorValue{0};
        MilliFunds relaxedValue{0};
        MilliFunds repairValue{0};
        MilliFunds searchValue{0};
        MilliFunds bookedValue{0};
        std::int64_t searchStates{0};
        bool certificate{false};
        bool searchCompleted{false};
        bool witnessReplays{false};
        bool provenExact{false};
    };

    struct PlanStockActionAudit
    {
        std::int32_t knowledgeIndex{NO_UNIT};
        TilePoint destination{INVALID_TILE};
        bool captures{false};
        std::int32_t propertyIndex{-1};
        std::int32_t stockColumn{-1};
        std::int32_t currentOwnerSign{0};
        std::int32_t currentCapturePoints{0};
        std::int32_t currentCapturerKnowledge{NO_UNIT};
        std::int32_t carriedCapturePoints{0};
        std::int32_t captureRate{0};
        std::int32_t turnsUntilOwned{-1};
        bool capturedColumn{false};
    };

    struct PlanStockAudit
    {
        bool available{false};
        std::vector<std::int32_t> capturedColumns;
        MilliFunds ownedBaseline{0};
        MilliFunds ownershipFlipSwingTotal{0};
        MilliFunds ourOpenOptimum{0};
        MilliFunds jointEnemyOptimum{0};
        MilliFunds floorStockAbsolute{0};
        SequentialStockAudit ours;
        SequentialStockAudit enemy;
        MilliFunds scalarStockAbsolute{0};
        MilliFunds liveOriginStock{0};
        MilliFunds originScalarStock{0};
        bool scalarExact{false};
        bool scalarWitnessReplays{false};
        bool originExact{false};
        bool originWitnessReplays{false};
        std::vector<PlanStockActionAudit> actions;
    };

    class PlanStockValuer
    {
    public:
        virtual ~PlanStockValuer() = default;
        virtual MilliFunds planStock(const TurnPlan & plan) = 0;
        virtual MilliFunds originStock() const = 0;
        virtual MilliFunds stockCeiling() const = 0;
        virtual bool affectsStock(std::int32_t engineUnitId) const = 0;

        virtual bool livePairSwapIntervals() const
        {
            return false;
        }

        virtual bool liveSettlingIntervals() const
        {
            return livePairSwapIntervals();
        }

        virtual LivePlanStockQuote livePlanStock(
            const TurnPlan &,
            MilliFunds,
            bool)
        {
            return LivePlanStockQuote{};
        }

        virtual bool refineLiveAtBoundary(AssignPhase)
        {
            return false;
        }

        virtual PlanStockAudit auditPlanStock(const TurnPlan &)
        {
            return PlanStockAudit{};
        }
    };
}
