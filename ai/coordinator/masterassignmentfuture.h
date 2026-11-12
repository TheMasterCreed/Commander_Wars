#pragma once

#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/propertystocksequential.h"

namespace Coordinator
{
    struct MasterFutureAction
    {
        CanonicalPlanActionKey key{CanonicalPlanActionKey::fromAction(NO_STOCK_ROW, 0, 0, false)};
        SequentialRow row;
    };

    struct MasterFutureCatalog
    {
        std::int32_t horizon{0};
        std::vector<PropertyStockColumn> columns;
        std::vector<SequentialClassTable> classes;
        std::vector<SequentialRow> dayRows;
        std::vector<MasterFutureAction> actions;
        bool complete{false};
    };

    enum class MasterFutureStatus : std::int8_t
    {
        Valid,
        Unavailable,
        Invalid,
        ArithmeticFailure,
        BudgetExhausted,
    };

    struct MasterFutureRowPrice
    {
        MilliFunds upper{0};
        std::vector<std::int32_t> nodes;
    };

    struct MasterFuturePricing
    {
        MasterFutureStatus status{MasterFutureStatus::Unavailable};
        std::vector<MasterFutureRowPrice> dayRows;
        std::vector<MasterFutureRowPrice> actions;
    };

    MasterFuturePricing priceMasterFuture(
        const MasterFutureCatalog & catalog, std::span<const MilliFunds> nodePrices,
        RefinementLedger & ledger, RefinementWork category);
    const MasterFutureAction* findMasterFutureAction(const MasterFutureCatalog & catalog,
                                                     const CanonicalPlanActionKey & key);
    std::int32_t futureNodeOfColumn(std::span<const PropertyStockColumn> columns, std::int32_t slot);
}
