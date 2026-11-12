#include "ai/coordinator/masterassignmentfuture.h"

#include <algorithm>

#include "ai/coordinator/relativestockcomparison.h"

namespace
{
    using namespace Coordinator;

    struct PricedCell
    {
        MilliFunds value{0};
        std::int32_t next{NO_SEQUENTIAL_NODE};
    };

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
            for (std::size_t node = 0; node < m_prices.size(); ++node)
            {
                if (m_prices[node] < 0 || m_catalog.columns[node].slot < 0 ||
                    (node > 0 && m_catalog.columns[node - 1].slot >= m_catalog.columns[node].slot))
                {
                    return {MasterFutureStatus::Invalid};
                }
            }
            const auto cells = masterWorkProduct(m_prices.size(), static_cast<std::size_t>(m_catalog.horizon));
            if (!cells)
            {
                return {MasterFutureStatus::ArithmeticFailure};
            }
            for (const auto & original : m_catalog.classes)
            {
                if (!original.hasValidTransitions() || original.nodeCount() != static_cast<std::int32_t>(m_prices.size()) ||
                    original.horizonTurns() != m_catalog.horizon)
                {
                    return {MasterFutureStatus::Invalid};
                }
                const auto status = buildClass(original, static_cast<std::size_t>(*cells));
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
        MasterFutureStatus buildClass(const SequentialClassTable & original, std::size_t cells)
        {
            if (!chargeMasterWork(m_ledger, cells + 1, 1, m_category))
            {
                return MasterFutureStatus::BudgetExhausted;
            }
            std::vector<PricedCell> values(cells);
            for (std::int32_t turn = m_catalog.horizon - 1; turn >= 1; --turn)
            {
                for (std::int32_t node = 0; node < original.nodeCount(); ++node)
                {
                    auto & best = values[cellIndex(node, turn, m_catalog.horizon)];
                    for (std::int32_t next = 0; next < original.nodeCount(); ++next)
                    {
                        if (!chargeMasterWork(m_ledger, 1, 1, m_category))
                        {
                            return MasterFutureStatus::BudgetExhausted;
                        }
                        const auto nextTurn = original.continuationOwnedTurn(node, turn, next);
                        if (nextTurn == NO_CAPTURE_TURNS)
                        {
                            continue;
                        }
                        auto value = checkedPrize(m_catalog.columns[static_cast<std::size_t>(next)],
                                                  nextTurn, m_catalog.horizon);
                        if (!value || !RelativeStockDetail::checkedSubtract(*value, m_prices[static_cast<std::size_t>(next)]) ||
                            !RelativeStockDetail::checkedAdd(*value, values[cellIndex(next, nextTurn, m_catalog.horizon)].value))
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
            m_values.push_back(std::move(values));
            return MasterFutureStatus::Valid;
        }

        MasterFutureStatus priceRow(const SequentialRow & row, MasterFutureRowPrice & result)
        {
            if (!chargeMasterWork(m_ledger, m_prices.size() + 1, 1, m_category))
            {
                return MasterFutureStatus::BudgetExhausted;
            }
            if (row.classIndex < 0 || static_cast<std::size_t>(row.classIndex) >= m_catalog.classes.size() ||
                row.weight.size() != m_prices.size() || row.ownedTurn.size() != m_prices.size())
            {
                return MasterFutureStatus::Invalid;
            }
            const auto & values = m_values[static_cast<std::size_t>(row.classIndex)];
            const auto & original = m_catalog.classes[static_cast<std::size_t>(row.classIndex)];
            std::int32_t selected = NO_SEQUENTIAL_NODE;
            for (std::int32_t node = 0; node < original.nodeCount(); ++node)
            {
                const auto slot = static_cast<std::size_t>(node);
                const auto turn = row.ownedTurn[slot];
                if (turn != NO_CAPTURE_TURNS && (turn < 1 || turn >= m_catalog.horizon))
                {
                    return MasterFutureStatus::Invalid;
                }
                auto value = row.weight[slot];
                if (!RelativeStockDetail::checkedSubtract(value, m_prices[slot]) ||
                    (turn != NO_CAPTURE_TURNS &&
                     !RelativeStockDetail::checkedAdd(value, values[cellIndex(node, turn, m_catalog.horizon)].value)))
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
                    if (!chargeMasterWork(m_ledger, 1, 1, m_category))
                    {
                        return MasterFutureStatus::BudgetExhausted;
                    }
                    result.nodes.push_back(selected);
                    if (turn == NO_CAPTURE_TURNS)
                    {
                        break;
                    }
                    const auto next = values[cellIndex(selected, turn, m_catalog.horizon)].next;
                    if (next != NO_SEQUENTIAL_NODE)
                    {
                        turn = original.continuationOwnedTurn(selected, turn, next);
                    }
                    selected = next;
                }
            }
            return MasterFutureStatus::Valid;
        }

        const MasterFutureCatalog & m_catalog;
        std::span<const MilliFunds> m_prices;
        RefinementLedger & m_ledger;
        RefinementWork m_category;
        std::vector<std::vector<PricedCell>> m_values;
    };
}

namespace Coordinator
{
    MasterFuturePricing priceMasterFuture(const MasterFutureCatalog & catalog,
        std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category)
    {
        return FuturePricing(catalog, nodePrices, ledger, category).run();
    }

    const MasterFutureAction* findMasterFutureAction(const MasterFutureCatalog & catalog,
                                                     const CanonicalPlanActionKey & key)
    {
        const auto found = std::lower_bound(catalog.actions.begin(), catalog.actions.end(), key,
            [](const MasterFutureAction & action, const CanonicalPlanActionKey & sought) { return action.key < sought; });
        return found == catalog.actions.end() || found->key != key ? nullptr : &*found;
    }

    std::int32_t futureNodeOfColumn(std::span<const PropertyStockColumn> columns, std::int32_t slot)
    {
        const auto found = std::find_if(columns.begin(), columns.end(),
            [&](const PropertyStockColumn & column) { return column.slot == slot; });
        return found == columns.end() ? NO_SEQUENTIAL_NODE : static_cast<std::int32_t>(found - columns.begin());
    }
}
