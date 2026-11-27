#pragma once

#include "ai/coordinator/propertystocksequential.h"
#include "ai/coordinator/refinementledger.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <vector>

namespace Coordinator
{
    struct CompatiblePackingResult
    {
        MilliFunds lower{}, upper{};
        std::int64_t work{};
        // Indices in the caller's complete row menus, with -1 for an idle row.
        std::vector<int> witness;
    };

    // Bounds the friendly stock term for a single, fixed set of first orders.
    // The caller supplies COMPLETE row menus from stockRowOptions. Every option
    // owns its listed sites. A site can be used by at most one friendly row.
    inline std::optional<CompatiblePackingResult> branchStockPacking(
        const SequentialInstance &instance, RefinementLedger &ledger,
        const std::vector<std::vector<SequentialDetail::RowOption>> &menus)
    {
        using Word = std::uint64_t;
        using Domain = std::vector<Word>;
        using Price = MilliFunds;
        const auto started = ledger.stats().spent();
        const auto pay = [&](std::size_t units) {
            return units <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) &&
                   ledger.consume(static_cast<std::int64_t>(units), RefinementWork::MasterWitness);
        };
        const std::size_t sites = instance.nodeColumns.size(), count = menus.size();
        if (count != instance.rows.size() || sites != instance.capturedNodes.size() ||
            count > std::numeric_limits<int>::max() || sites > std::numeric_limits<int>::max() ||
            !pay(count + sites + 1)) return std::nullopt;

        struct Row { std::size_t begin{}, words{}; std::vector<int> order; };
        std::vector<Row> rows(count);
        std::size_t stride = 0;
        Price rootUpper = 0;
        for (std::size_t r = 0; r < count; ++r)
        {
            const auto &menu = menus[r];
            if (menu.size() >= std::numeric_limits<int>::max() ||
                !pay((menu.size() + 1) * (std::bit_width(menu.size()) + 2))) return std::nullopt;
            auto &row = rows[r];
            row.order.resize(menu.size());
            std::iota(row.order.begin(), row.order.end(), 0);
            std::sort(row.order.begin(), row.order.end(), [&](int a, int b) {
                if (menu[a].value != menu[b].value) return menu[a].value > menu[b].value;
                return a < b;
            });
            // Nonpositive options are dominated by the idle route (zero payoff, no sites).
            std::erase_if(row.order, [&](int index) { return menu[index].value <= 0; });
            const auto best = row.order.empty() ? 0 : menu[row.order.front()].value;
            if (best > std::numeric_limits<Price>::max() - rootUpper) return std::nullopt;
            rootUpper += best;
            row.order.push_back(-1);
            row.begin = stride;
            row.words = (row.order.size() + 63) / 64;
            stride += row.words;
        }
        // Packed bits range over row OPTIONS, so the number of map sites is unrestricted.
        if (sites && stride > std::numeric_limits<std::size_t>::max() / sites) return std::nullopt;
        if (!pay(sites * stride) || !pay(stride + count)) return std::nullopt;
        Domain claims(sites * stride), initial(stride);
        for (std::size_t r = 0; r < count; ++r)
        {
            const auto &row = rows[r];
            for (std::size_t k = 0; k < row.order.size(); ++k)
            {
                const auto word = row.begin + k / 64;
                const Word bit = Word{1} << (k % 64);
                initial[word] |= bit;
                const int index = row.order[k];
                if (index < 0) continue;
                const auto &nodes = menus[r][index].nodes;
                if (!pay(nodes.size() + 1)) return std::nullopt;
                for (const int n : nodes)
                {
                    if (n < 0 || std::size_t(n) >= sites || instance.capturedNodes[n]) return std::nullopt;
                    auto &bits = claims[std::size_t(n) * stride + word];
                    if (bits & bit) return std::nullopt;
                    bits |= bit;
                }
            }
        }

        CompatiblePackingResult result{0, rootUpper, 0, std::vector<int>(count, -1)};
        const auto finish = [&]() {
            result.work = ledger.stats().spent() - started;
            return std::optional<CompatiblePackingResult>{std::move(result)};
        };
        // Return the rank of the best allowed route. Optional exclusion is for
        // choosing a productive split only. It cannot change either bound.
        const auto top = [&](const Domain &allowed, const Row &row, int exclude = -1) -> int {
            for (std::size_t j = 0; j < row.words; ++j)
            {
                auto bits = allowed[row.begin + j];
                if (exclude >= 0) bits &= ~claims[std::size_t(exclude) * stride + row.begin + j];
                if (bits) return static_cast<int>(64 * j + std::countr_zero(bits));
            }
            return -1;
        };
        const auto value = [&](std::size_t r, int rank) -> Price {
            if (rank < 0) return 0;
            const auto index = rows[r].order[rank];
            return index < 0 ? 0 : menus[r][index].value;
        };
        const auto greedy = [&](const Domain &allowed, bool reverse) {
            if (!pay(stride + 3 * count + 1)) return false;
            auto available = allowed;
            Price total = 0;
            std::vector<int> choices(count, -1);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto r = reverse ? count - 1 - i : i;
                if (!pay(rows[r].words)) return false;
                const int rank = top(available, rows[r]);
                if (rank < 0) return true; // This greedy prefix cannot satisfy the remaining forced row.
                const int index = rows[r].order[rank];
                choices[r] = index;
                if (index < 0) continue;
                total += menus[r][index].value;
                for (int n : menus[r][index].nodes)
                {
                    if (!pay(stride + 1)) return false;
                    for (std::size_t j = 0; j < stride; ++j) available[j] &= ~claims[std::size_t(n) * stride + j];
                }
            }
            if (total > result.lower) { result.lower = total; result.witness = std::move(choices); }
            return true;
        };
        struct State { Domain allowed; Price upper; };
        std::vector<State> pending;
        pending.push_back({std::move(initial), rootUpper});
        while (!pending.empty())
        {
            auto state = std::move(pending.back());
            pending.pop_back();
            if (state.upper <= result.lower) continue;
            if (!greedy(state.allowed, false) || !greedy(state.allowed, true)) return finish();
            if (state.upper <= result.lower) continue;
            if (!pay(sites + 4 * count + stride + 1)) return finish();
            std::vector<int> tops(count);
            std::vector<unsigned> usage(sites);
            for (std::size_t r = 0; r < count; ++r)
            {
                tops[r] = top(state.allowed, rows[r]);
                const auto index = rows[r].order[tops[r]];
                if (index < 0) continue;
                const auto &nodes = menus[r][index].nodes;
                if (!pay(nodes.size())) return finish();
                for (int n : nodes) ++usage[n];
            }
            std::tuple<Price, unsigned, Price, int, int> chosen{0,0,0,-1,-1};
            for (std::size_t n = 0; n < sites; ++n)
            {
                if (usage[n] < 2) continue;
                if (!pay(count + stride + 1)) return finish();
                Price lossSum = 0, maximumLoss = -1;
                int owner = -1;
                for (std::size_t r = 0; r < count; ++r)
                {
                    const int k = tops[r];
                    if (!(claims[n * stride + rows[r].begin + k / 64] & (Word{1} << (k % 64)))) continue;
                    const auto loss = value(r, k) - value(r, top(state.allowed, rows[r], static_cast<int>(n)));
                    lossSum += loss;
                    if (loss > maximumLoss) { maximumLoss = loss; owner = static_cast<int>(r); }
                }
                chosen = std::max(chosen, {lossSum - maximumLoss, usage[n], maximumLoss, static_cast<int>(n), owner});
            }
            const int site = std::get<3>(chosen), owner = std::get<4>(chosen);
            if (site < 0)
            {
                // All row maxima coexist. They witness the relaxation itself.
                result.lower = state.upper;
                for (std::size_t r = 0; r < count; ++r) result.witness[r] = rows[r].order[tops[r]];
                continue;
            }
            const auto begin = pending.size();
            for (bool usesSite : {false, true})
            {
                if (!pay(3 * stride + 2 * count + 1)) return finish();
                State child{state.allowed, 0};
                bool feasible = true;
                for (std::size_t r = 0; r < count; ++r)
                {
                    const bool selected = static_cast<int>(r) == owner;
                    if (selected || usesSite)
                    {
                        for (auto j = rows[r].begin; j < rows[r].begin + rows[r].words; ++j)
                        {
                            const auto bits = claims[std::size_t(site) * stride + j];
                            child.allowed[j] &= selected && usesSite ? bits : ~bits;
                        }
                    }
                    const int k = top(child.allowed, rows[r]);
                    if (k < 0) { feasible = false; break; }
                    child.upper += value(r, k);
                }
                if (feasible && child.upper > result.lower) pending.push_back(std::move(child));
            }
            if (pending.size() == begin + 2 && pending[begin].upper > pending.back().upper)
                std::swap(pending[begin], pending.back());
        }
        result.upper = result.lower;
        return finish();
    }
}
