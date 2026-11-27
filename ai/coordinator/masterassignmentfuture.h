#pragma once

#include <algorithm>

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

    struct MasterFuturePricedCell
    {
        MilliFunds value{0};
        std::int32_t next{NO_SEQUENTIAL_NODE};
    };

    using MasterFuturePricedClass = std::vector<MasterFuturePricedCell>;

    struct MasterFuturePricing
    {
        MasterFutureStatus status{MasterFutureStatus::Unavailable};
        std::vector<MasterFutureRowPrice> dayRows;
        std::vector<MasterFutureRowPrice> actions;
        // Populated by successful future pricing, one table per catalog class.
        std::vector<MasterFuturePricedClass> classes;
    };

    struct MasterFutureSuccessor
    {
        std::int32_t next{NO_SEQUENTIAL_NODE};
        std::int32_t turn{NO_CAPTURE_TURNS};
        std::size_t targetCell{0};
        MilliFunds prize{0};
    };

    struct MasterFuturePreparedClass
    {
        // Feasible successors per cell, indexed like the priced cells; the list runs in recurrence order.
        std::vector<std::int32_t> successorCount;
        std::vector<MasterFutureSuccessor> successors;
    };

    // Price-independent lists of one unchanged catalog; holds indices and values only.
    struct MasterFuturePreparation
    {
        MasterFutureStatus status{MasterFutureStatus::Unavailable};
        std::int32_t nodes{0};
        std::int32_t horizon{0};
        std::vector<MasterFuturePreparedClass> classes;
        std::vector<std::vector<std::int32_t>> dayRows;
        std::vector<std::vector<std::int32_t>> actions;
    };

    MasterFuturePricing priceMasterFuture(
        const MasterFutureCatalog & catalog, std::span<const MilliFunds> nodePrices,
        RefinementLedger & ledger, RefinementWork category);
    // The preparation must come from this unchanged catalog.
    MasterFuturePricing priceMasterFuture(
        const MasterFutureCatalog & catalog, std::span<const MilliFunds> nodePrices,
        const MasterFuturePreparation & preparation, RefinementLedger & ledger, RefinementWork category);
    void priceMasterFutureInto(
        const MasterFutureCatalog & catalog, std::span<const MilliFunds> nodePrices,
        const MasterFuturePreparation * pPreparation, RefinementLedger & ledger, RefinementWork category,
        MasterFuturePricing & result);
    MasterFuturePreparation prepareMasterFuture(const MasterFutureCatalog & catalog, RefinementLedger & ledger,
                                                RefinementWork category);
    std::optional<std::int64_t> masterFuturePreparationWork(const MasterFutureCatalog & catalog);

    namespace MasterFutureDetail
    {
        // Callers pass at least one column per price, a priced table for every class a row names, and a fresh result.
        bool validMasterFuturePrices(std::span<const PropertyStockColumn> columns,
                                     std::span<const MilliFunds> nodePrices);
        MasterFutureStatus priceMasterFutureClass(
            const SequentialClassTable & table, std::span<const PropertyStockColumn> columns, std::int32_t horizon,
            std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category,
            MasterFuturePricedClass & values);
        MasterFutureStatus priceMasterFutureRow(
            const SequentialRow & row, std::span<const SequentialClassTable> classes,
            std::span<const MasterFuturePricedClass> pricedClasses, std::int32_t horizon,
            std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category,
            MasterFutureRowPrice & result);
    }

    inline const MasterFutureAction* findMasterFutureAction(const MasterFutureCatalog & catalog,
                                                            const CanonicalPlanActionKey & key)
    {
        const auto found = std::lower_bound(catalog.actions.begin(), catalog.actions.end(), key,
            [](const MasterFutureAction & action, const CanonicalPlanActionKey & sought) { return action.key < sought; });
        return found == catalog.actions.end() || found->key != key ? nullptr : &*found;
    }

    inline std::int32_t futureNodeOfColumn(std::span<const PropertyStockColumn> columns, std::int32_t slot)
    {
        const auto found = std::find_if(columns.begin(), columns.end(),
            [&](const PropertyStockColumn & column) { return column.slot == slot; });
        return found == columns.end() ? NO_SEQUENTIAL_NODE : static_cast<std::int32_t>(found - columns.begin());
    }
}
