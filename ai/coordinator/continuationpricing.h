#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ai/coordinator/continuationinterval.h"

namespace Coordinator
{
    enum class ContinuationPricingState : std::int8_t
    {
        Unprepared,
        Prepared,
        Failed,
    };

    struct CheapChainStep
    {
        std::int32_t column{-1};
        std::int32_t turn{-1};
        MilliFunds gain{0};

        friend constexpr bool operator==(const CheapChainStep &, const CheapChainStep &) = default;
    };

    struct CheapChain
    {
        std::vector<CheapChainStep> steps;
        MilliFunds value{0};

        friend bool operator==(const CheapChain &, const CheapChain &) = default;
    };

    struct CheapRowTerms
    {
        std::int32_t row{-1};
        std::vector<CheapChain> chains;

        friend bool operator==(const CheapRowTerms &, const CheapRowTerms &) = default;
    };

    struct CheapActionTerms
    {
        CanonicalPlanActionKey key{
            CanonicalPlanActionKey::fromAction(-1, -1, -1, false)
        };
        std::int32_t row{-1};
        std::int32_t capturedColumn{-1};
        CheapRowTerms rowTerms;

        friend bool operator==(const CheapActionTerms &,
                               const CheapActionTerms &) = default;
    };

    class CheapPricingModel;

    struct CheapUpperState
    {
        const CheapPricingModel* pModel{nullptr};
        std::vector<const CheapActionTerms*> actionOfRow;
        std::vector<const CheapRowTerms*> termsOfRow;
        std::vector<MilliFunds> rowUpper;
        std::vector<std::int32_t> capturedColumnRefs;
        std::vector<bool> excludedColumns;
        MilliFunds owned{0};
        MilliFunds oursUpper{0};

        friend bool operator==(const CheapUpperState &,
                               const CheapUpperState &) = default;
    };

    struct CheapInitialPrice
    {
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        MilliFunds owned{0};
        StockInterval ours;
        StockInterval enemy;
        StockInterval stock;
        std::vector<RowWitness> witness;
    };

    class CheapPricingModel
    {
    public:
        bool install(std::int32_t rowCount,
                     std::int32_t columnCount,
                     MilliFunds ownedBaseline,
                     std::vector<MilliFunds> capturedColumnSwing,
                     std::vector<CheapRowTerms> dayRows,
                     std::vector<CheapActionTerms> actions,
                     MilliFunds enemyCeiling)
        {
            if (rowCount < 0 || columnCount < 0 || enemyCeiling < 0 ||
                capturedColumnSwing.size() !=
                    static_cast<std::size_t>(columnCount) ||
                dayRows.size() != static_cast<std::size_t>(rowCount))
            {
                return false;
            }
            for (std::int32_t row = 0; row < rowCount; ++row)
            {
                CheapRowTerms & terms = dayRows[static_cast<std::size_t>(row)];
                if (terms.row != row || !normalizeRow(terms, columnCount))
                {
                    return false;
                }
            }
            for (CheapActionTerms & action : actions)
            {
                if (action.row < 0 || action.row >= rowCount ||
                    action.key.row() != action.row ||
                    action.rowTerms.row != action.row ||
                    action.capturedColumn < -1 ||
                    action.capturedColumn >= columnCount ||
                    !normalizeRow(action.rowTerms, columnCount))
                {
                    return false;
                }
            }
            std::sort(actions.begin(),
                      actions.end(),
                      [](const CheapActionTerms & lhs,
                         const CheapActionTerms & rhs)
            {
                return lhs.key < rhs.key;
            });
            for (std::size_t slot = 1; slot < actions.size(); ++slot)
            {
                if (actions[slot - 1].key == actions[slot].key)
                {
                    return false;
                }
            }
            m_rowCount = rowCount;
            m_columnCount = columnCount;
            m_ownedBaseline = ownedBaseline;
            m_capturedColumnSwing = std::move(capturedColumnSwing);
            m_dayRows = std::move(dayRows);
            m_actions = std::move(actions);
            m_enemyCeiling = enemyCeiling;
            m_installed = true;
            return true;
        }

        bool installed() const
        {
            return m_installed;
        }

        std::int32_t rowCount() const
        {
            return m_rowCount;
        }

        std::int32_t columnCount() const
        {
            return m_columnCount;
        }

        std::int64_t actionCount() const
        {
            return static_cast<std::int64_t>(m_actions.size());
        }

        MilliFunds enemyCeiling() const
        {
            return m_enemyCeiling;
        }

        CheapUpperState initialState() const
        {
            CheapUpperState state;
            if (!m_installed)
            {
                return state;
            }
            state.pModel = this;
            state.actionOfRow.assign(
                static_cast<std::size_t>(m_rowCount), nullptr);
            state.termsOfRow.reserve(static_cast<std::size_t>(m_rowCount));
            state.rowUpper.assign(static_cast<std::size_t>(m_rowCount), 0);
            state.capturedColumnRefs.assign(
                static_cast<std::size_t>(m_columnCount), 0);
            state.excludedColumns.assign(
                static_cast<std::size_t>(m_columnCount), false);
            state.owned = m_ownedBaseline;
            for (std::int32_t row = 0; row < m_rowCount; ++row)
            {
                state.termsOfRow.push_back(
                    &m_dayRows[static_cast<std::size_t>(row)]);
                const MilliFunds upper =
                    upperOf(*state.termsOfRow.back(), state.excludedColumns);
                state.rowUpper[static_cast<std::size_t>(row)] = upper;
                state.oursUpper += upper;
            }
            return state;
        }

        bool apply(ContinuationKey key, CheapUpperState & state) const
        {
            if (!m_installed || state.pModel != this)
            {
                return false;
            }
            std::sort(key.begin(), key.end());
            std::vector<const CheapActionTerms*> desired(
                static_cast<std::size_t>(m_rowCount), nullptr);
            for (std::size_t slot = 0; slot < key.size(); ++slot)
            {
                if (slot > 0 && key[slot - 1] == key[slot])
                {
                    return false;
                }
                const CheapActionTerms* pAction = findAction(key[slot]);
                if (pAction == nullptr ||
                    desired[static_cast<std::size_t>(pAction->row)] != nullptr)
                {
                    return false;
                }
                desired[static_cast<std::size_t>(pAction->row)] = pAction;
            }

            std::vector<std::int32_t> nextRefs =
                state.capturedColumnRefs;
            std::vector<std::int32_t> changedRows;
            changedRows.reserve(static_cast<std::size_t>(m_rowCount));
            for (std::int32_t row = 0; row < m_rowCount; ++row)
            {
                const std::size_t slot = static_cast<std::size_t>(row);
                const CheapActionTerms* pOld = state.actionOfRow[slot];
                const CheapActionTerms* pNext = desired[slot];
                if (pOld == pNext)
                {
                    continue;
                }
                changedRows.push_back(row);
                if (pOld != nullptr && pOld->capturedColumn >= 0)
                {
                    --nextRefs[
                        static_cast<std::size_t>(pOld->capturedColumn)];
                }
                if (pNext != nullptr && pNext->capturedColumn >= 0)
                {
                    ++nextRefs[
                        static_cast<std::size_t>(pNext->capturedColumn)];
                }
            }
            if (std::any_of(nextRefs.begin(),
                            nextRefs.end(),
                            [](std::int32_t refs)
            {
                return refs < 0;
            }))
            {
                return false;
            }

            bool capturedSetChanged = false;
            for (std::int32_t column = 0; column < m_columnCount; ++column)
            {
                const std::size_t slot = static_cast<std::size_t>(column);
                if ((state.capturedColumnRefs[slot] == 0) !=
                    (nextRefs[slot] == 0))
                {
                    capturedSetChanged = true;
                    break;
                }
            }
            for (const std::int32_t row : changedRows)
            {
                const std::size_t slot = static_cast<std::size_t>(row);
                state.actionOfRow[slot] = desired[slot];
                state.termsOfRow[slot] =
                    desired[slot] == nullptr
                        ? &m_dayRows[slot]
                        : &desired[slot]->rowTerms;
            }
            state.capturedColumnRefs = std::move(nextRefs);
            if (capturedSetChanged)
            {
                state.owned = m_ownedBaseline;
                for (std::int32_t column = 0;
                     column < m_columnCount;
                     ++column)
                {
                    const std::size_t slot =
                        static_cast<std::size_t>(column);
                    state.excludedColumns[slot] =
                        state.capturedColumnRefs[slot] > 0;
                    if (state.excludedColumns[slot])
                    {
                        state.owned += m_capturedColumnSwing[slot];
                    }
                }
                state.oursUpper = 0;
                for (std::int32_t row = 0; row < m_rowCount; ++row)
                {
                    const std::size_t slot =
                        static_cast<std::size_t>(row);
                    state.rowUpper[slot] =
                        upperOf(*state.termsOfRow[slot],
                                state.excludedColumns);
                    state.oursUpper += state.rowUpper[slot];
                }
                return true;
            }
            for (const std::int32_t row : changedRows)
            {
                const std::size_t slot = static_cast<std::size_t>(row);
                state.oursUpper -= state.rowUpper[slot];
                state.rowUpper[slot] =
                    upperOf(*state.termsOfRow[slot],
                            state.excludedColumns);
                state.oursUpper += state.rowUpper[slot];
            }
            return true;
        }

        bool price(ContinuationKey key,
                   CheapUpperState & state,
                   CheapInitialPrice & result) const
        {
            std::sort(key.begin(), key.end());
            if (!apply(key, state))
            {
                return false;
            }
            result = CheapInitialPrice{};
            result.key = std::move(key);
            result.owned = state.owned;
            for (std::int32_t column = 0;
                 column < m_columnCount;
                 ++column)
            {
                if (state.capturedColumnRefs[
                        static_cast<std::size_t>(column)] > 0)
                {
                    result.capturedColumns.push_back(column);
                }
            }
            result.ours.lower = chainGreedy(state, result.witness);
            result.ours.upper = state.oursUpper;
            result.enemy = StockInterval{0, m_enemyCeiling};
            result.stock =
                composeStockInterval(result.owned,
                                     result.ours,
                                     result.enemy);
            return result.ours.lower <= result.ours.upper;
        }

        bool replayWitness(const CheapUpperState & state,
                           std::span<const RowWitness> witness,
                           MilliFunds expected) const
        {
            if (state.pModel != this)
            {
                return false;
            }
            std::vector<bool> usedRows(
                static_cast<std::size_t>(m_rowCount), false);
            std::vector<bool> usedColumns(
                static_cast<std::size_t>(m_columnCount), false);
            MilliFunds total = 0;
            std::int32_t previousRow = -1;
            for (const RowWitness & rowWitness : witness)
            {
                if (rowWitness.row < 0 ||
                    rowWitness.row >= m_rowCount ||
                    rowWitness.row <= previousRow ||
                    rowWitness.nodes.empty())
                {
                    return false;
                }
                previousRow = rowWitness.row;
                const std::size_t rowSlot =
                    static_cast<std::size_t>(rowWitness.row);
                if (usedRows[rowSlot])
                {
                    return false;
                }
                usedRows[rowSlot] = true;
                const CheapChain* pMatch = nullptr;
                for (const CheapChain & chain :
                     state.termsOfRow[rowSlot]->chains)
                {
                    if (chain.steps.size() < rowWitness.nodes.size())
                    {
                        continue;
                    }
                    bool same = true;
                    MilliFunds prefixValue = 0;
                    for (std::size_t step = 0;
                         step < rowWitness.nodes.size();
                         ++step)
                    {
                        if (chain.steps[step].column !=
                            rowWitness.nodes[step])
                        {
                            same = false;
                            break;
                        }
                        prefixValue += chain.steps[step].gain;
                    }
                    if (same && prefixValue == rowWitness.value)
                    {
                        pMatch = &chain;
                        break;
                    }
                }
                if (pMatch == nullptr)
                {
                    return false;
                }
                MilliFunds rowTotal = 0;
                for (std::size_t step = 0;
                     step < rowWitness.nodes.size();
                     ++step)
                {
                    const CheapChainStep & source =
                        pMatch->steps[step];
                    const std::size_t column =
                        static_cast<std::size_t>(source.column);
                    if (state.excludedColumns[column] ||
                        usedColumns[column])
                    {
                        return false;
                    }
                    usedColumns[column] = true;
                    rowTotal += source.gain;
                }
                if (rowTotal != rowWitness.value)
                {
                    return false;
                }
                total += rowTotal;
            }
            return total == expected;
        }

    private:
        static bool chainColumnsLess(const CheapChain & lhs,
                                     const CheapChain & rhs)
        {
            return std::lexicographical_compare(
                lhs.steps.begin(),
                lhs.steps.end(),
                rhs.steps.begin(),
                rhs.steps.end(),
                [](const CheapChainStep & left,
                   const CheapChainStep & right)
            {
                return left.column < right.column;
            });
        }

        static bool chainOrder(const CheapChain & lhs,
                               const CheapChain & rhs)
        {
            if (lhs.value != rhs.value)
            {
                return lhs.value > rhs.value;
            }
            if (lhs.steps.front().column != rhs.steps.front().column)
            {
                return lhs.steps.front().column <
                       rhs.steps.front().column;
            }
            return chainColumnsLess(lhs, rhs);
        }

        static bool normalizeRow(CheapRowTerms & terms,
                                 std::int32_t columnCount)
        {
            for (const CheapChain & chain : terms.chains)
            {
                if (chain.steps.empty())
                {
                    return false;
                }
                MilliFunds total = 0;
                for (const CheapChainStep & step : chain.steps)
                {
                    if (step.column < 0 ||
                        step.column >= columnCount)
                    {
                        return false;
                    }
                    total += step.gain;
                }
                if (total != chain.value)
                {
                    return false;
                }
            }
            std::stable_sort(terms.chains.begin(),
                             terms.chains.end(),
                             chainOrder);
            return true;
        }

        const CheapActionTerms* findAction(
            const CanonicalPlanActionKey & key) const
        {
            const auto found = std::lower_bound(
                m_actions.begin(),
                m_actions.end(),
                key,
                [](const CheapActionTerms & action,
                   const CanonicalPlanActionKey & sought)
            {
                return action.key < sought;
            });
            if (found == m_actions.end() || found->key != key)
            {
                return nullptr;
            }
            return &*found;
        }

        static MilliFunds upperOf(
            const CheapRowTerms & terms,
            const std::vector<bool> & excluded)
        {
            for (const CheapChain & chain : terms.chains)
            {
                if (!excluded[
                        static_cast<std::size_t>(
                            chain.steps.front().column)])
                {
                    return std::max<MilliFunds>(chain.value, 0);
                }
            }
            return 0;
        }

        MilliFunds chainGreedy(
            const CheapUpperState & state,
            std::vector<RowWitness> & witness) const
        {
            std::vector<bool> usedColumns(
                static_cast<std::size_t>(m_columnCount), false);
            MilliFunds total = 0;
            for (std::int32_t row = 0; row < m_rowCount; ++row)
            {
                const CheapChain* pSelected = nullptr;
                for (const CheapChain & chain :
                     state.termsOfRow[static_cast<std::size_t>(row)]
                         ->chains)
                {
                    const std::size_t head =
                        static_cast<std::size_t>(
                            chain.steps.front().column);
                    if (!state.excludedColumns[head] &&
                        !usedColumns[head] &&
                        chain.value > 0)
                    {
                        pSelected = &chain;
                        break;
                    }
                }
                if (pSelected == nullptr)
                {
                    continue;
                }
                RowWitness accepted;
                accepted.row = row;
                accepted.nodes.reserve(pSelected->steps.size());
                for (const CheapChainStep & step : pSelected->steps)
                {
                    const std::size_t column =
                        static_cast<std::size_t>(step.column);
                    if (state.excludedColumns[column] ||
                        usedColumns[column])
                    {
                        break;
                    }
                    usedColumns[column] = true;
                    accepted.nodes.push_back(step.column);
                    accepted.value += step.gain;
                }
                if (accepted.nodes.empty() || accepted.value <= 0)
                {
                    for (const std::int32_t column : accepted.nodes)
                    {
                        usedColumns[
                            static_cast<std::size_t>(column)] = false;
                    }
                    continue;
                }
                total += accepted.value;
                witness.push_back(std::move(accepted));
            }
            return total;
        }

        std::int32_t m_rowCount{0};
        std::int32_t m_columnCount{0};
        MilliFunds m_ownedBaseline{0};
        std::vector<MilliFunds> m_capturedColumnSwing;
        std::vector<CheapRowTerms> m_dayRows;
        std::vector<CheapActionTerms> m_actions;
        MilliFunds m_enemyCeiling{0};
        bool m_installed{false};
    };
}
