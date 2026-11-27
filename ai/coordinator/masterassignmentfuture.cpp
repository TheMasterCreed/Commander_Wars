#include "ai/coordinator/masterassignmentfuture.h"

#include <algorithm>
#include <initializer_list>

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

    bool validClassShape(const SequentialClassTable & table, std::size_t nodes, std::int32_t horizon)
    {
        return table.hasValidTransitions() && table.nodeCount() == static_cast<std::int32_t>(nodes) &&
               table.horizonTurns() == horizon;
    }

    bool validRowShape(const SequentialRow & row, std::size_t classes, std::size_t nodes)
    {
        return row.classIndex >= 0 && static_cast<std::size_t>(row.classIndex) < classes &&
               row.weight.size() == nodes && row.ownedTurn.size() == nodes;
    }

    bool validOwnedTurn(std::int32_t turn, std::int32_t horizon)
    {
        return turn == NO_CAPTURE_TURNS || (turn >= 1 && turn < horizon);
    }

    std::optional<std::int64_t> sumWork(std::initializer_list<std::optional<std::int64_t>> terms)
    {
        std::int64_t total = 0;
        for (const auto & term : terms)
        {
            if (!term || !RelativeStockDetail::checkedAdd(total, *term))
            {
                return std::nullopt;
            }
        }
        return total;
    }

    // Charges every examined successor, feasible or not, exactly as the full recurrence always has.
    template <typename Visit>
    MasterFutureStatus forEachFeasibleSuccessor(const SequentialClassTable & table,
        std::span<const PropertyStockColumn> columns, std::int32_t horizon, RefinementLedger & ledger,
        RefinementWork category, Visit visit)
    {
        for (std::int32_t turn = horizon - 1; turn >= 1; --turn)
        {
            for (std::int32_t node = 0; node < table.nodeCount(); ++node)
            {
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
                    const auto prize = checkedPrize(columns[static_cast<std::size_t>(next)], nextTurn, horizon);
                    if (!prize || !visit(cellIndex(node, turn, horizon),
                                         MasterFutureSuccessor{next, nextTurn, cellIndex(next, nextTurn, horizon), *prize}))
                    {
                        return MasterFutureStatus::ArithmeticFailure;
                    }
                }
            }
        }
        return MasterFutureStatus::Valid;
    }

    bool offerSuccessor(MasterFuturePricedClass & priced, std::size_t cell, const MasterFutureSuccessor & successor,
                        std::span<const MilliFunds> nodePrices, std::int32_t horizon)
    {
        auto value = successor.prize;
        if (!RelativeStockDetail::checkedSubtract(value, nodePrices[static_cast<std::size_t>(successor.next)]) ||
            !RelativeStockDetail::checkedAdd(value, priced[successor.targetCell].value))
        {
            return false;
        }
        auto & best = priced[cell];
        if (value > best.value)
        {
            best = {value, successor.next};
        }
        return true;
    }

    bool offerRowNode(const SequentialRow & row, std::int32_t node, const MasterFuturePricedClass & values,
                      std::int32_t horizon, std::span<const MilliFunds> nodePrices, MasterFutureRowPrice & result,
                      std::int32_t & selected)
    {
        const auto slot = static_cast<std::size_t>(node);
        const auto turn = row.ownedTurn[slot];
        auto value = row.weight[slot];
        if (!RelativeStockDetail::checkedSubtract(value, nodePrices[slot]) ||
            (turn != NO_CAPTURE_TURNS &&
             !RelativeStockDetail::checkedAdd(value, values[cellIndex(node, turn, horizon)].value)))
        {
            return false;
        }
        if (value > result.upper)
        {
            result.upper = value;
            selected = node;
        }
        return true;
    }

    MasterFutureStatus reconstructRow(const SequentialRow & row, const SequentialClassTable & original,
                                      const MasterFuturePricedClass & values, std::int32_t horizon,
                                      std::int32_t selected, RefinementLedger & ledger, RefinementWork category,
                                      MasterFutureRowPrice & result)
    {
        if (selected == NO_SEQUENTIAL_NODE)
        {
            return MasterFutureStatus::Valid;
        }
        auto turn = row.ownedTurn[static_cast<std::size_t>(selected)];
        const std::int64_t maxSteps = std::max<std::int64_t>(horizon, 1);
        if (ledger.available() >= maxSteps)
        {
            std::int64_t steps = 0;
            while (selected != NO_SEQUENTIAL_NODE)
            {
                ++steps;
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
            ledger.consume(steps, category);
            return MasterFutureStatus::Valid;
        }
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
        return MasterFutureStatus::Valid;
    }

    // An untimed node without positive weight is worth its weight minus a price that is never negative: never selected.
    MasterFutureStatus prepareRow(const SequentialRow & row, std::size_t classes, std::size_t nodes,
                                  std::int32_t horizon, RefinementLedger & ledger, RefinementWork category,
                                  std::vector<std::int32_t> & listed)
    {
        if (!chargeMasterWork(ledger, nodes + 1, 1, category))
        {
            return MasterFutureStatus::BudgetExhausted;
        }
        if (!validRowShape(row, classes, nodes) ||
            !std::all_of(row.ownedTurn.begin(), row.ownedTurn.end(),
                         [&](std::int32_t turn) { return validOwnedTurn(turn, horizon); }))
        {
            return MasterFutureStatus::Invalid;
        }
        for (std::size_t slot = 0; slot < nodes; ++slot)
        {
            if (row.weight[slot] > 0 || row.ownedTurn[slot] != NO_CAPTURE_TURNS)
            {
                listed.push_back(static_cast<std::int32_t>(slot));
            }
        }
        return MasterFutureStatus::Valid;
    }

    template <typename RowOf, typename Entries>
    MasterFutureStatus prepareRows(const MasterFutureCatalog & catalog, const Entries & entries, RowOf rowOf,
                                   RefinementLedger & ledger, RefinementWork category,
                                   std::vector<std::vector<std::int32_t>> & lists)
    {
        for (const auto & entry : entries)
        {
            std::vector<std::int32_t> listed;
            const auto status = prepareRow(rowOf(entry), catalog.classes.size(), catalog.columns.size(),
                                           catalog.horizon, ledger, category, listed);
            if (status != MasterFutureStatus::Valid)
            {
                return status;
            }
            lists.push_back(std::move(listed));
        }
        return MasterFutureStatus::Valid;
    }

    class FuturePricing final
    {
    public:
        FuturePricing(const MasterFutureCatalog & catalog, std::span<const MilliFunds> prices,
                      const MasterFuturePreparation * pPreparation, RefinementLedger & ledger,
                      RefinementWork category)
            : m_catalog(catalog),
              m_prices(prices),
              m_pPreparation(pPreparation),
              m_ledger(ledger),
              m_category(category)
        {
        }

        void runInto(MasterFuturePricing & result)
        {
            const auto fail = [&](MasterFutureStatus status)
            {
                result.status = status;
                result.dayRows.clear();
                result.actions.clear();
                result.classes.clear();
            };
            if (!m_catalog.complete)
            {
                fail(MasterFutureStatus::Unavailable);
                return;
            }
            if (m_catalog.horizon < 0 || m_catalog.columns.size() != m_prices.size() ||
                m_prices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) ||
                (m_pPreparation != nullptr && !preparedForCatalog()))
            {
                fail(MasterFutureStatus::Invalid);
                return;
            }
            if (!chargeMasterWork(m_ledger, m_prices.size() + m_catalog.classes.size() + 1, 1, m_category))
            {
                fail(MasterFutureStatus::BudgetExhausted);
                return;
            }
            if (!MasterFutureDetail::validMasterFuturePrices(m_catalog.columns, m_prices))
            {
                fail(MasterFutureStatus::Invalid);
                return;
            }
            const auto cells = masterWorkProduct(m_prices.size(), static_cast<std::size_t>(m_catalog.horizon));
            if (!cells)
            {
                fail(MasterFutureStatus::ArithmeticFailure);
                return;
            }
            result.classes.resize(m_catalog.classes.size());
            for (std::size_t index = 0; index < m_catalog.classes.size(); ++index)
            {
                const auto status = m_pPreparation != nullptr ? pricePreparedClass(index, *cells, result.classes[index]) :
                    MasterFutureDetail::priceMasterFutureClass(m_catalog.classes[index], m_catalog.columns,
                        m_catalog.horizon, m_prices, m_ledger, m_category, result.classes[index]);
                if (status != MasterFutureStatus::Valid)
                {
                    fail(status);
                    return;
                }
            }
            result.dayRows.resize(m_catalog.dayRows.size());
            for (std::size_t index = 0; index < m_catalog.dayRows.size(); ++index)
            {
                MasterFutureRowPrice & price = result.dayRows[index];
                price.upper = 0;
                price.nodes.clear();
                const auto status = priceRow(m_catalog.dayRows[index], result.classes,
                    m_pPreparation != nullptr ? &m_pPreparation->dayRows[index] : nullptr, price);
                if (status != MasterFutureStatus::Valid)
                {
                    fail(status);
                    return;
                }
            }
            result.actions.resize(m_catalog.actions.size());
            for (std::size_t action = 0; action < m_catalog.actions.size(); ++action)
            {
                const auto & source = m_catalog.actions[action];
                if (source.key.row() < 0 || static_cast<std::size_t>(source.key.row()) >= m_catalog.dayRows.size() ||
                    (action > 0 && !(m_catalog.actions[action - 1].key < source.key)))
                {
                    fail(MasterFutureStatus::Invalid);
                    return;
                }
                MasterFutureRowPrice & price = result.actions[action];
                price.upper = 0;
                price.nodes.clear();
                const auto status = priceRow(source.row, result.classes,
                    m_pPreparation != nullptr ? &m_pPreparation->actions[action] : nullptr, price);
                if (status != MasterFutureStatus::Valid)
                {
                    fail(status);
                    return;
                }
            }
            result.status = MasterFutureStatus::Valid;
        }

    private:
        // A shape guard, not an authentication: a same-shaped preparation of another catalog passes it.
        bool preparedForCatalog() const
        {
            const auto & prepared = *m_pPreparation;
            return prepared.status == MasterFutureStatus::Valid &&
                prepared.nodes == static_cast<std::int32_t>(m_prices.size()) &&
                prepared.horizon == m_catalog.horizon && prepared.classes.size() == m_catalog.classes.size() &&
                prepared.dayRows.size() == m_catalog.dayRows.size() &&
                prepared.actions.size() == m_catalog.actions.size();
        }

        MasterFutureStatus pricePreparedClass(std::size_t index, std::int64_t cells, MasterFuturePricedClass & priced)
        {
            if (!chargeMasterWork(m_ledger, static_cast<std::size_t>(cells) + 1, 1, m_category))
            {
                return MasterFutureStatus::BudgetExhausted;
            }
            priced.assign(static_cast<std::size_t>(cells), MasterFuturePricedCell{});
            const auto & prepared = m_pPreparation->classes[index];
            const MasterFutureSuccessor* pSuccessor = prepared.successors.data();
            const std::int32_t* pCounts = prepared.successorCount.data();
            const MilliFunds* pPrices = m_prices.data();
            MasterFuturePricedCell* pPriced = priced.data();
            const auto horizon = m_catalog.horizon;
            const auto nodes = m_pPreparation->nodes;
            for (std::int32_t turn = horizon - 1; turn >= 1; --turn)
            {
                for (std::int32_t node = 0; node < nodes; ++node)
                {
                    const auto cell = cellIndex(node, turn, horizon);
                    const std::int32_t count = pCounts[cell];
                    if (count == 0)
                    {
                        continue;
                    }
                    if (count <= m_ledger.available())
                    {
                        MilliFunds bestValue = 0;
                        std::int32_t bestNext = NO_SEQUENTIAL_NODE;
                        for (std::int32_t i = 0; i < count; ++i)
                        {
                            const MasterFutureSuccessor & succ = pSuccessor[i];
                            auto value = succ.prize;
                            if (!RelativeStockDetail::checkedSubtract(value, pPrices[static_cast<std::size_t>(succ.next)]) ||
                                !RelativeStockDetail::checkedAdd(value, pPriced[succ.targetCell].value))
                            {
                                m_ledger.consume(static_cast<std::int64_t>(i) + 1, m_category);
                                return MasterFutureStatus::ArithmeticFailure;
                            }
                            if (value > bestValue)
                            {
                                bestValue = value;
                                bestNext = succ.next;
                            }
                        }
                        m_ledger.consume(count, m_category);
                        pPriced[cell] = {bestValue, bestNext};
                        pSuccessor += count;
                    }
                    else
                    {
                        for (std::int32_t i = 0; i < count; ++i, ++pSuccessor)
                        {
                            if (!chargeMasterWork(m_ledger, 1, 1, m_category))
                            {
                                return MasterFutureStatus::BudgetExhausted;
                            }
                            if (!offerSuccessor(priced, cell, *pSuccessor, m_prices, horizon))
                            {
                                return MasterFutureStatus::ArithmeticFailure;
                            }
                        }
                    }
                }
            }
            return MasterFutureStatus::Valid;
        }

        MasterFutureStatus priceRow(const SequentialRow & row,
                                    const std::vector<MasterFuturePricedClass> & classes,
                                    const std::vector<std::int32_t> * pListed,
                                    MasterFutureRowPrice & result)
        {
            if (pListed == nullptr)
            {
                return MasterFutureDetail::priceMasterFutureRow(row, m_catalog.classes, classes, m_catalog.horizon,
                                                                m_prices, m_ledger, m_category, result);
            }
            if (!chargeMasterWork(m_ledger, pListed->size() + 1, 1, m_category))
            {
                return MasterFutureStatus::BudgetExhausted;
            }
            const auto & values = classes[static_cast<std::size_t>(row.classIndex)];
            std::int32_t selected = NO_SEQUENTIAL_NODE;
            for (const auto node : *pListed)
            {
                if (!offerRowNode(row, node, values, m_catalog.horizon, m_prices, result, selected))
                {
                    return MasterFutureStatus::ArithmeticFailure;
                }
            }
            return reconstructRow(row, m_catalog.classes[static_cast<std::size_t>(row.classIndex)], values,
                                  m_catalog.horizon, selected, m_ledger, m_category, result);
        }

        const MasterFutureCatalog & m_catalog;
        std::span<const MilliFunds> m_prices;
        const MasterFuturePreparation * m_pPreparation;
        RefinementLedger & m_ledger;
        RefinementWork m_category;
    };
}

namespace Coordinator
{
    void priceMasterFutureInto(
        const MasterFutureCatalog & catalog, std::span<const MilliFunds> nodePrices,
        const MasterFuturePreparation * pPreparation, RefinementLedger & ledger, RefinementWork category,
        MasterFuturePricing & result)
    {
        FuturePricing(catalog, nodePrices, pPreparation, ledger, category).runInto(result);
    }

    MasterFuturePricing priceMasterFuture(const MasterFutureCatalog & catalog,
        std::span<const MilliFunds> nodePrices, RefinementLedger & ledger, RefinementWork category)
    {
        MasterFuturePricing result;
        FuturePricing(catalog, nodePrices, nullptr, ledger, category).runInto(result);
        return result;
    }

    MasterFuturePricing priceMasterFuture(const MasterFutureCatalog & catalog,
        std::span<const MilliFunds> nodePrices, const MasterFuturePreparation & preparation,
        RefinementLedger & ledger, RefinementWork category)
    {
        MasterFuturePricing result;
        FuturePricing(catalog, nodePrices, &preparation, ledger, category).runInto(result);
        return result;
    }

    MasterFuturePreparation prepareMasterFuture(const MasterFutureCatalog & catalog, RefinementLedger & ledger,
                                                RefinementWork category)
    {
        if (!catalog.complete)
        {
            return {};
        }
        const auto nodes = catalog.columns.size();
        const auto horizon = catalog.horizon;
        if (horizon < 0 || nodes > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            return {MasterFutureStatus::Invalid};
        }
        if (!chargeMasterWork(ledger, nodes + catalog.classes.size() + 1, 1, category))
        {
            return {MasterFutureStatus::BudgetExhausted};
        }
        const auto cells = masterWorkProduct(nodes, static_cast<std::size_t>(horizon));
        if (!cells)
        {
            return {MasterFutureStatus::ArithmeticFailure};
        }
        MasterFuturePreparation result;
        for (const auto & table : catalog.classes)
        {
            if (!validClassShape(table, nodes, horizon))
            {
                return {MasterFutureStatus::Invalid};
            }
            if (!chargeMasterWork(ledger, static_cast<std::size_t>(*cells) + 1, 1, category))
            {
                return {MasterFutureStatus::BudgetExhausted};
            }
            auto & prepared = result.classes.emplace_back();
            prepared.successorCount.assign(static_cast<std::size_t>(*cells), 0);
            const auto status = forEachFeasibleSuccessor(table, catalog.columns, horizon, ledger, category,
                [&](std::size_t cell, const MasterFutureSuccessor & successor)
            {
                ++prepared.successorCount[cell];
                prepared.successors.push_back(successor);
                return true;
            });
            if (status != MasterFutureStatus::Valid)
            {
                return {status};
            }
        }
        auto status = prepareRows(catalog, catalog.dayRows,
            [](const SequentialRow & row) -> const SequentialRow & { return row; }, ledger, category, result.dayRows);
        if (status == MasterFutureStatus::Valid)
        {
            status = prepareRows(catalog, catalog.actions,
                [](const MasterFutureAction & action) -> const SequentialRow & { return action.row; }, ledger, category,
                result.actions);
        }
        if (status != MasterFutureStatus::Valid)
        {
            return {status};
        }
        result.status = MasterFutureStatus::Valid;
        result.nodes = static_cast<std::int32_t>(nodes);
        result.horizon = horizon;
        return result;
    }

    std::optional<std::int64_t> masterFuturePreparationWork(const MasterFutureCatalog & catalog)
    {
        if (catalog.horizon < 0)
        {
            return std::nullopt;
        }
        const auto nodes = catalog.columns.size();
        const auto horizon = static_cast<std::size_t>(catalog.horizon);
        const auto squared = masterWorkProduct(nodes, nodes);
        const auto transitions = squared ?
            masterWorkProduct(static_cast<std::size_t>(*squared), horizon > 0 ? horizon - 1 : 0) : std::nullopt;
        const auto perClass = sumWork({masterWorkProduct(nodes, horizon), 1, transitions});
        const auto classes = perClass ?
            masterWorkProduct(catalog.classes.size(), static_cast<std::size_t>(*perClass)) : std::nullopt;
        const auto perRow = sumWork({masterWorkProduct(nodes, 1), 1});
        const auto rows = perRow ?
            sumWork({masterWorkProduct(catalog.dayRows.size(), static_cast<std::size_t>(*perRow)),
                     masterWorkProduct(catalog.actions.size(), static_cast<std::size_t>(*perRow))}) : std::nullopt;
        return sumWork({masterWorkProduct(nodes, 1), masterWorkProduct(catalog.classes.size(), 1), 1, classes, rows});
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
        if (!validClassShape(table, nodePrices.size(), horizon))
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
        const auto status = forEachFeasibleSuccessor(table, columns, horizon, ledger, category,
            [&](std::size_t cell, const MasterFutureSuccessor & successor)
        {
            return offerSuccessor(priced, cell, successor, nodePrices, horizon);
        });
        if (status != MasterFutureStatus::Valid)
        {
            return status;
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
        if (!validRowShape(row, classes.size(), nodePrices.size()))
        {
            return MasterFutureStatus::Invalid;
        }
        const auto & values = pricedClasses[static_cast<std::size_t>(row.classIndex)];
        const auto & original = classes[static_cast<std::size_t>(row.classIndex)];
        std::int32_t selected = NO_SEQUENTIAL_NODE;
        for (std::int32_t node = 0; node < original.nodeCount(); ++node)
        {
            if (!validOwnedTurn(row.ownedTurn[static_cast<std::size_t>(node)], horizon))
            {
                return MasterFutureStatus::Invalid;
            }
            if (!offerRowNode(row, node, values, horizon, nodePrices, result, selected))
            {
                return MasterFutureStatus::ArithmeticFailure;
            }
        }
        return reconstructRow(row, original, values, horizon, selected, ledger, category, result);
    }
}
