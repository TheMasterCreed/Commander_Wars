#include "ai/coordinator/masterassignmentfuture.h"

#include <algorithm>

#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace Coordinator;

    std::optional<MilliFunds> checkedPrize(const PropertyStockColumn & column,
                                          std::int32_t turn, std::int32_t horizon)
    {
        MilliFunds before = 0;
        if (column.ownerBefore == OwnerSign::Ours)
        {
            before = column.income.oursPerTurn;
        }
        else if (column.ownerBefore == OwnerSign::Enemy)
        {
            if (!RelativeStockDetail::checkedSubtract(before, column.income.enemyPerTurn))
            {
                return std::nullopt;
            }
        }
        else if (column.ownerBefore != OwnerSign::Neutral)
        {
            return std::nullopt;
        }
        MilliFunds swing = column.income.oursPerTurn;
        if (!RelativeStockDetail::checkedSubtract(swing, before))
        {
            return std::nullopt;
        }
        const std::int64_t turns = horizon - turn;
        if (turns < 0 || (turns > 0 &&
            (swing > std::numeric_limits<MilliFunds>::max() / turns ||
             swing < std::numeric_limits<MilliFunds>::min() / turns)))
        {
            return std::nullopt;
        }
        return columnStreamValue(column, turn, horizon);
    }

    std::size_t cellIndex(std::int32_t node, std::int32_t turn, std::int32_t horizon)
    {
        return static_cast<std::size_t>(node) * static_cast<std::size_t>(horizon) +
               static_cast<std::size_t>(turn);
    }

    class FuturePricing final
    {
    public:
        FuturePricing(const MasterFutureCatalog & catalog, std::span<const MilliFunds> prices,
                      RefinementLedger & ledger, RefinementWork category)
            : m_catalog(catalog),
              m_prices(prices),
              m_ledger(ledger),
              m_category(category)
        {
        }

        MasterFuturePricing run()
        {
            if (!m_catalog.complete)
            {
                return {};
            }
            if (m_catalog.horizon < 0 || m_catalog.columns.size() != m_prices.size() ||
                m_prices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            {
                return {MasterFutureStatus::Invalid};
            }
            if (!chargeMasterWork(m_ledger, m_prices.size() + m_catalog.classes.size() + 1, 1, m_category))
            {
                return {MasterFutureStatus::BudgetExhausted};
            }
            if (!MasterFutureDetail::validMasterFuturePrices(m_catalog.columns, m_prices))
            {
                return {MasterFutureStatus::Invalid};
            }
            if (!masterWorkProduct(m_prices.size(), static_cast<std::size_t>(m_catalog.horizon)))
            {
                return {MasterFutureStatus::ArithmeticFailure};
            }
            m_values.resize(m_catalog.classes.size());
            for (std::size_t index = 0; index < m_catalog.classes.size(); ++index)
            {
                const auto status = MasterFutureDetail::priceMasterFutureClass(m_catalog.classes[index],
                    m_catalog.columns, m_catalog.horizon, m_prices, m_ledger, m_category, m_values[index]);
                if (status != MasterFutureStatus::Valid)
                {
                    return {status};
                }
            }
            MasterFuturePricing result;
            for (const auto & row : m_catalog.dayRows)
            {
                MasterFutureRowPrice price;
                result.status = priceRow(row, price);
                if (result.status != MasterFutureStatus::Valid)
                {
                    return {result.status};
                }
                result.dayRows.push_back(std::move(price));
            }
            for (std::size_t action = 0; action < m_catalog.actions.size(); ++action)
            {
                const auto & source = m_catalog.actions[action];
                if (source.key.row() < 0 || static_cast<std::size_t>(source.key.row()) >= m_catalog.dayRows.size() ||
                    (action > 0 && !(m_catalog.actions[action - 1].key < source.key)))
                {
                    return {MasterFutureStatus::Invalid};
                }
                MasterFutureRowPrice price;
                result.status = priceRow(source.row, price);
                if (result.status != MasterFutureStatus::Valid)
                {
                    return {result.status};
                }
                result.actions.push_back(std::move(price));
            }
            result.status = MasterFutureStatus::Valid;
            return result;
        }

    private:
        MasterFutureStatus priceRow(const SequentialRow & row, MasterFutureRowPrice & result)
        {
            return MasterFutureDetail::priceMasterFutureRow(row, m_catalog.classes, m_values, m_catalog.horizon,
                                                            m_prices, m_ledger, m_category, result);
        }

        const MasterFutureCatalog & m_catalog;
        std::span<const MilliFunds> m_prices;
        RefinementLedger & m_ledger;
        RefinementWork m_category;
        std::vector<MasterFuturePricedClass> m_values;
    };
}

namespace Coordinator
{
    MasterFuturePricing priceMasterFuture(const MasterFutureCatalog & catalog,
        std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category)
    {
        return FuturePricing(catalog, nodePrices, ledger, category).run();
    }

    bool MasterFutureDetail::validMasterFuturePrices(std::span<const PropertyStockColumn> columns,
                                                     std::span<const MilliFunds> nodePrices)
    {
        for (std::size_t node = 0; node < nodePrices.size(); ++node)
        {
            if (nodePrices[node] < 0 || columns[node].slot < 0 ||
                (node > 0 && columns[node - 1].slot >= columns[node].slot))
            {
                return false;
            }
        }
        return true;
    }

    MasterFutureStatus MasterFutureDetail::priceMasterFutureClass(
        const SequentialClassTable & table, std::span<const PropertyStockColumn> columns, std::int32_t horizon,
        std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category,
        MasterFuturePricedClass & values)
    {
        if (!table.hasValidTransitions() || table.nodeCount() != static_cast<std::int32_t>(nodePrices.size()) ||
            table.horizonTurns() != horizon)
        {
            return MasterFutureStatus::Invalid;
        }
        const auto cells = masterWorkProduct(nodePrices.size(), static_cast<std::size_t>(horizon));
        if (!cells)
        {
            return MasterFutureStatus::ArithmeticFailure;
        }
        if (!chargeMasterWork(ledger, static_cast<std::size_t>(*cells) + 1, 1, category))
        {
            return MasterFutureStatus::BudgetExhausted;
        }
        MasterFuturePricedClass priced(static_cast<std::size_t>(*cells));
        for (std::int32_t turn = horizon - 1; turn >= 1; --turn)
        {
            for (std::int32_t node = 0; node < table.nodeCount(); ++node)
            {
                auto & best = priced[cellIndex(node, turn, horizon)];
                for (std::int32_t next = 0; next < table.nodeCount(); ++next)
                {
                    if (!chargeMasterWork(ledger, 1, 1, category))
                    {
                        return MasterFutureStatus::BudgetExhausted;
                    }
                    const auto nextTurn = table.continuationOwnedTurn(node, turn, next);
                    if (nextTurn == NO_CAPTURE_TURNS)
                    {
                        continue;
                    }
                    auto value = checkedPrize(columns[static_cast<std::size_t>(next)], nextTurn, horizon);
                    if (!value || !RelativeStockDetail::checkedSubtract(*value, nodePrices[static_cast<std::size_t>(next)]) ||
                        !RelativeStockDetail::checkedAdd(*value, priced[cellIndex(next, nextTurn, horizon)].value))
                    {
                        return MasterFutureStatus::ArithmeticFailure;
                    }
                    if (*value > best.value)
                    {
                        best = {*value, next};
                    }
                }
            }
        }
        values = std::move(priced);
        return MasterFutureStatus::Valid;
    }

    MasterFutureStatus MasterFutureDetail::priceMasterFutureRow(
        const SequentialRow & row, std::span<const SequentialClassTable> classes,
        std::span<const MasterFuturePricedClass> pricedClasses, std::int32_t horizon,
        std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category,
        MasterFutureRowPrice & result)
    {
        if (!chargeMasterWork(ledger, nodePrices.size() + 1, 1, category))
        {
            return MasterFutureStatus::BudgetExhausted;
        }
        if (row.classIndex < 0 || static_cast<std::size_t>(row.classIndex) >= classes.size() ||
            row.weight.size() != nodePrices.size() || row.ownedTurn.size() != nodePrices.size())
        {
            return MasterFutureStatus::Invalid;
        }
        const auto & values = pricedClasses[static_cast<std::size_t>(row.classIndex)];
        const auto & original = classes[static_cast<std::size_t>(row.classIndex)];
        std::int32_t selected = NO_SEQUENTIAL_NODE;
        for (std::int32_t node = 0; node < original.nodeCount(); ++node)
        {
            const auto slot = static_cast<std::size_t>(node);
            const auto turn = row.ownedTurn[slot];
            if (turn != NO_CAPTURE_TURNS && (turn < 1 || turn >= horizon))
            {
                return MasterFutureStatus::Invalid;
            }
            auto value = row.weight[slot];
            if (!RelativeStockDetail::checkedSubtract(value, nodePrices[slot]) ||
                (turn != NO_CAPTURE_TURNS &&
                 !RelativeStockDetail::checkedAdd(value, values[cellIndex(node, turn, horizon)].value)))
            {
                return MasterFutureStatus::ArithmeticFailure;
            }
            if (value > result.upper)
            {
                result.upper = value;
                selected = node;
            }
        }
        if (selected != NO_SEQUENTIAL_NODE)
        {
            auto turn = row.ownedTurn[static_cast<std::size_t>(selected)];
            while (selected != NO_SEQUENTIAL_NODE)
            {
                if (!chargeMasterWork(ledger, 1, 1, category))
                {
                    return MasterFutureStatus::BudgetExhausted;
                }
                result.nodes.push_back(selected);
                if (turn == NO_CAPTURE_TURNS)
                {
                    break;
                }
                const auto next = values[cellIndex(selected, turn, horizon)].next;
                if (next != NO_SEQUENTIAL_NODE)
                {
                    turn = original.continuationOwnedTurn(selected, turn, next);
                }
                selected = next;
            }
        }
        return MasterFutureStatus::Valid;
    }
}
