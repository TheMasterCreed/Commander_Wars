#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Coordinator::StockRouteDetail
{
    struct Node
    {
        int tick, cell, spent, fuel, parent;
        int next{-1};
    };

    class SparseFrontier
    {
    public:
        SparseFrontier(int count, int movement)
            : cells(count), movementStates(static_cast<std::int64_t>(movement) + 1) {}

        bool improve(const Node &node)
        {
            auto [it, fresh] = best.try_emplace(key(node), node.fuel);
            if (!fresh && it->second <= node.fuel) return false;
            it->second = node.fuel;
            return true;
        }

        void push(const Node &node, int arrival, int remaining)
        {
            pending.emplace(arrival, remaining, node.fuel, static_cast<int>(nodes.size()));
            nodes.push_back(node);
        }

        int pop()
        {
            if (pending.empty()) return -1;
            const int id = std::get<3>(pending.top());
            pending.pop();
            return id;
        }

        bool current(const Node &node) const { return best.at(key(node)) == node.fuel; }
        std::vector<Node> nodes;

    private:
        std::int64_t key(const Node &node) const
        {
            return (static_cast<std::int64_t>(node.tick) * cells + node.cell) * movementStates + node.spent;
        }

        using Item = std::tuple<int, int, int, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pending;
        std::unordered_map<std::int64_t, int> best;
        int cells;
        std::int64_t movementStates;
    };

    class BucketFrontier
    {
        struct Visit { std::uint32_t generation{}; int fuel{}; };
        struct Ends { int head, tail; };
        static constexpr int maxVisits = 256 * 1024;
        static constexpr int maxBuckets = 64 * 64 * 64;

    public:
        bool begin(int count, int movement, int departure, int last)
        {
            if (active || count <= 0 || movement <= 0 || departure < 0 || last < departure) return false;
            const std::int64_t turns = 1 + (std::int64_t(last) - departure) / 2;
            const auto movementStates = std::int64_t(movement) + 1;
            if (turns > maxVisits / count / movementStates) return false;
            const std::int64_t costStates = 1 + turns * movement;
            if (turns > maxBuckets / costStates / costStates) return false;
            const int stateCount = static_cast<int>(turns * count * movementStates);
            const int keys = static_cast<int>(turns * costStates * costStates);
            const int words = (keys + 63) / 64;
            const int groups = (words + 63) / 64;
            if (visits.size() < static_cast<std::size_t>(stateCount)) visits.resize(stateCount);
            if (buckets.size() < static_cast<std::size_t>(keys)) buckets.resize(keys);
            level1.assign(words, 0);
            level2.assign(groups, 0);
            top = 0;
            if (++generation == 0)
            {
                std::fill(visits.begin(), visits.end(), Visit{});
                generation = 1;
            }
            cells = count;
            spends = static_cast<int>(movementStates);
            costs = static_cast<int>(costStates);
            first = departure;
            nodes.clear();
            active = true;
            return true;
        }

        void finish()
        {
            active = false;
            if (nodes.capacity() > maxVisits) std::vector<Node>().swap(nodes);
        }

        bool improve(const Node &node)
        {
            auto &entry = visits[key(node)];
            if (entry.generation == generation && entry.fuel <= node.fuel) return false;
            entry = {generation, node.fuel};
            return true;
        }

        void push(const Node &node, int arrival, int remaining)
        {
            // Every accepted route spends at most `turns * movement` past
            // and future fuel. The sorted integer key has the original order
            // (arrival, remaining terrain cost, fuel already used).
            const int bucket = (((arrival - first) / 2) * costs + remaining) * costs + node.fuel;
            const int word = bucket >> 6, group = word >> 6;
            const auto flag = bit(bucket);
            auto &ends = buckets[bucket];
            const int id = static_cast<int>(nodes.size());
            nodes.push_back(node);
            if (level1[word] & flag)
                nodes[ends.tail].next = id;
            else
            {
                ends.head = id;
                level1[word] |= flag;
                level2[group] |= bit(word);
                top |= bit(group);
            }
            ends.tail = id;
        }

        int pop()
        {
            if (!top) return -1;
            const int group = std::countr_zero(top);
            const int word = group * 64 + std::countr_zero(level2[group]);
            const int bucket = word * 64 + std::countr_zero(level1[word]);
            auto &ends = buckets[bucket];
            // Equal (arrival, remaining, fuel) entries originally popped by
            // increasing node index. The per-bucket append order is that index.
            const int id = ends.head;
            ends.head = nodes[id].next;
            if (ends.head < 0)
            {
                level1[word] &= ~bit(bucket);
                if (!level1[word])
                {
                    level2[group] &= ~bit(word);
                    if (!level2[group]) top &= ~bit(group);
                }
            }
            return id;
        }

        bool current(const Node &node) const { return visits[key(node)].fuel == node.fuel; }
        std::vector<Node> nodes;

    private:
        static std::uint64_t bit(int n) { return std::uint64_t{1} << (n & 63); }
        int key(const Node &node) const
        {
            return (((node.tick - first) / 2) * cells + node.cell) * spends + node.spent;
        }
        std::vector<Visit> visits;
        std::vector<Ends> buckets;
        std::vector<std::uint64_t> level1, level2;
        std::uint64_t top{};
        std::uint32_t generation{};
        int first{}, cells{}, spends{}, costs{};
        bool active{};
    };

    struct EndBucketSearch
    {
        BucketFrontier &frontier;
        ~EndBucketSearch() { frontier.finish(); }
    };
}
