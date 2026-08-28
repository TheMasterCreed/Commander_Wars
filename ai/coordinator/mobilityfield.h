#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    // Travel direction from the previous tile into the entered tile, as PathFindingSystem::Directions.
    enum class EntryDirection : std::int8_t
    {
        West = 0,
        East = 1,
        South = 2,
        North = 3,
    };

    constexpr std::int32_t ENTRY_DIRECTION_COUNT = 4;

    constexpr std::array<EntryDirection, ENTRY_DIRECTION_COUNT> ENTRY_DIRECTIONS{
        EntryDirection::West,
        EntryDirection::East,
        EntryDirection::South,
        EntryDirection::North,
    };

    constexpr std::int32_t IMPASSABLE_COST = -1;
    constexpr std::int32_t UNREACHABLE = -1;

    struct StepVector
    {
        std::int32_t dx;
        std::int32_t dy;
    };

    // Only meaningful for a 4-adjacent step.
    constexpr EntryDirection entryDirection(std::int32_t fromX, std::int32_t fromY, std::int32_t toX, std::int32_t toY)
    {
        if (toX < fromX)
        {
            return EntryDirection::West;
        }
        if (toX > fromX)
        {
            return EntryDirection::East;
        }
        if (toY > fromY)
        {
            return EntryDirection::South;
        }
        return EntryDirection::North;
    }

    constexpr StepVector entryStep(EntryDirection direction)
    {
        switch (direction)
        {
            case EntryDirection::West:
                return StepVector{-1, 0};
            case EntryDirection::East:
                return StepVector{1, 0};
            case EntryDirection::South:
                return StepVector{0, 1};
            case EntryDirection::North:
                return StepVector{0, -1};
        }
        return StepVector{0, 0};
    }

    static_assert(entryStep(entryDirection(0, 0, 1, 0)).dx == 1, "east means x increased");
    static_assert(entryStep(entryDirection(0, 1, 0, 0)).dy == -1, "north means y decreased");

    class MobilityCostGrid
    {
    public:
        MobilityCostGrid() = default;

        MobilityCostGrid(std::int32_t width, std::int32_t height)
            : m_width(std::max(width, 0)),
              m_height(std::max(height, 0)),
              m_costs(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * ENTRY_DIRECTION_COUNT,
                      IMPASSABLE_COST)
        {
        }

        std::int32_t width() const
        {
            return m_width;
        }

        std::int32_t height() const
        {
            return m_height;
        }

        bool onGrid(std::int32_t x, std::int32_t y) const
        {
            return x >= 0 && y >= 0 && x < m_width && y < m_height;
        }

        std::int32_t cost(std::int32_t x, std::int32_t y, EntryDirection direction) const
        {
            if (!onGrid(x, y))
            {
                return IMPASSABLE_COST;
            }
            return m_costs[index(x, y, direction)];
        }

        void setCost(std::int32_t x, std::int32_t y, EntryDirection direction, std::int32_t value)
        {
            if (onGrid(x, y))
            {
                m_costs[index(x, y, direction)] = value;
            }
        }

        void setTileCost(std::int32_t x, std::int32_t y, std::int32_t value)
        {
            for (const EntryDirection direction : ENTRY_DIRECTIONS)
            {
                setCost(x, y, direction, value);
            }
        }

        friend bool operator==(const MobilityCostGrid &, const MobilityCostGrid &) = default;

    private:
        std::size_t index(std::int32_t x, std::int32_t y, EntryDirection direction) const
        {
            const std::size_t tile = static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
            return tile * ENTRY_DIRECTION_COUNT + static_cast<std::size_t>(direction);
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::vector<std::int32_t> m_costs;
    };

    enum class FieldExpansion
    {
        FromSources,
        ToTargets,
    };

    class DistanceField
    {
    public:
        static DistanceField build(const MobilityCostGrid & grid, const std::vector<TilePoint> & sources, FieldExpansion expansion)
        {
            DistanceField field(grid.width(), grid.height());
            std::vector<bool> settledTiles(field.m_distances.size(), false);
            std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapOrder> open;
            std::int64_t sequence = 0;
            for (const TilePoint & source : sources)
            {
                if (!grid.onGrid(source.x, source.y))
                {
                    continue;
                }
                // A mover already stands on a source, so its entry cost never applies.
                field.m_distances[field.index(source.x, source.y)] = 0;
                ++sequence;
                open.push(HeapEntry{0, sequence, source.x, source.y});
            }
            while (!open.empty())
            {
                const HeapEntry current = open.top();
                open.pop();
                const std::size_t currentIndex = field.index(current.x, current.y);
                if (settledTiles[currentIndex])
                {
                    continue;
                }
                settledTiles[currentIndex] = true;
                for (const EntryDirection direction : EXPANSION_ORDER)
                {
                    const StepVector step = entryStep(direction);
                    const std::int32_t neighbourX = current.x + step.dx;
                    const std::int32_t neighbourY = current.y + step.dy;
                    if (!grid.onGrid(neighbourX, neighbourY))
                    {
                        continue;
                    }
                    const std::size_t neighbourIndex = field.index(neighbourX, neighbourY);
                    if (settledTiles[neighbourIndex])
                    {
                        continue;
                    }
                    const std::int32_t cost = stepCost(grid, expansion, TilePoint{current.x, current.y}, TilePoint{neighbourX, neighbourY});
                    if (cost < 0)
                    {
                        continue;
                    }
                    const std::int32_t candidate = current.distance + cost;
                    const std::int32_t known = field.m_distances[neighbourIndex];
                    if (known == UNREACHABLE || candidate < known)
                    {
                        field.m_distances[neighbourIndex] = candidate;
                        ++sequence;
                        open.push(HeapEntry{candidate, sequence, neighbourX, neighbourY});
                    }
                }
            }
            return field;
        }

        std::int32_t distance(std::int32_t x, std::int32_t y) const
        {
            if (x < 0 || y < 0 || x >= m_width || y >= m_height)
            {
                return UNREACHABLE;
            }
            return m_distances[index(x, y)];
        }

        bool reachable(std::int32_t x, std::int32_t y, std::int32_t allowance) const
        {
            const std::int32_t value = distance(x, y);
            return value >= 0 && value <= allowance;
        }

    private:
        struct HeapEntry
        {
            std::int32_t distance;
            std::int64_t sequence;
            std::int32_t x;
            std::int32_t y;
        };

        // Push order breaks distance ties.
        struct HeapOrder
        {
            bool operator()(const HeapEntry & lhs, const HeapEntry & rhs) const
            {
                if (lhs.distance != rhs.distance)
                {
                    return lhs.distance > rhs.distance;
                }
                return lhs.sequence > rhs.sequence;
            }
        };

        // Both charge the tile being entered; ToTargets enters the settled tile, FromSources the neighbour.
        static std::int32_t stepCost(const MobilityCostGrid & grid, FieldExpansion expansion, const TilePoint & settled, const TilePoint & neighbour)
        {
            if (expansion == FieldExpansion::FromSources)
            {
                return grid.cost(neighbour.x, neighbour.y, entryDirection(settled.x, settled.y, neighbour.x, neighbour.y));
            }
            return grid.cost(settled.x, settled.y, entryDirection(neighbour.x, neighbour.y, settled.x, settled.y));
        }

        static constexpr std::array<EntryDirection, ENTRY_DIRECTION_COUNT> EXPANSION_ORDER{
            EntryDirection::East,
            EntryDirection::West,
            EntryDirection::South,
            EntryDirection::North,
        };

        DistanceField(std::int32_t width, std::int32_t height)
            : m_width(std::max(width, 0)),
              m_height(std::max(height, 0)),
              m_distances(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height), UNREACHABLE)
        {
        }

        std::size_t index(std::int32_t x, std::int32_t y) const
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::vector<std::int32_t> m_distances;
    };

    // Expand until the read set settles rather than until a turn count is reached.
    constexpr std::int32_t UNBOUNDED_ACTIVATIONS = -1;

    constexpr std::int32_t arrivalActivationsOf(std::int32_t completedBoundaries)
    {
        return completedBoundaries + 1;
    }

    // Turn boundaries, not summed cost. One instance is reused across builds; a generation stamp replaces clearing.
    class ActivationField
    {
    public:
        // An empty read set settles at once, since nothing is read from the result.
        void build(const MobilityCostGrid & grid, const TilePoint & origin, std::int32_t movementPoints,
                   std::int32_t activationLimit, const std::vector<TilePoint> & readTiles)
        {
            resize(grid.width(), grid.height());
            ++m_generation;
            m_heap.clear();
            if (!grid.onGrid(origin.x, origin.y))
            {
                return;
            }
            std::int32_t unsettledReadTiles = 0;
            for (const TilePoint & tile : readTiles)
            {
                if (!grid.onGrid(tile.x, tile.y))
                {
                    continue;
                }
                TileState & state = freshState(index(tile.x, tile.y));
                if (state.read)
                {
                    continue;
                }
                state.read = true;
                ++unsettledReadTiles;
            }
            std::int64_t sequence = 0;
            freshState(index(origin.x, origin.y)).label = ActivationLabel{0, 0, 0};
            push(HeapEntry{0, 0, 0, sequence, origin.x, origin.y});
            while (!m_heap.empty() && unsettledReadTiles > 0)
            {
                const HeapEntry current = pop();
                TileState & settled = freshState(index(current.x, current.y));
                if (settled.settled)
                {
                    continue;
                }
                settled.settled = true;
                if (settled.read)
                {
                    --unsettledReadTiles;
                }
                for (const EntryDirection direction : EXPANSION_ORDER)
                {
                    const StepVector step = entryStep(direction);
                    const std::int32_t neighbourX = current.x + step.dx;
                    const std::int32_t neighbourY = current.y + step.dy;
                    if (!grid.onGrid(neighbourX, neighbourY))
                    {
                        continue;
                    }
                    TileState & neighbour = freshState(index(neighbourX, neighbourY));
                    if (neighbour.settled)
                    {
                        continue;
                    }
                    const std::int32_t cost = grid.cost(neighbourX, neighbourY, direction);
                    // A tile costing more than a whole allowance is never enterable, at any turn count.
                    if (cost < 0 || cost > movementPoints)
                    {
                        continue;
                    }
                    const ActivationLabel candidate =
                        enteredLabel(ActivationLabel{current.boundaries, current.spent, current.cost}, cost,
                                     movementPoints);
                    if (exceedsLimit(candidate.boundaries, activationLimit))
                    {
                        continue;
                    }
                    if (neighbour.label.boundaries != UNREACHABLE && !isEarlier(candidate, neighbour.label))
                    {
                        continue;
                    }
                    neighbour.label = candidate;
                    ++sequence;
                    push(HeapEntry{candidate.boundaries, candidate.spent, candidate.cost, sequence, neighbourX,
                                   neighbourY});
                }
            }
        }

        // UNREACHABLE also covers a tile the activation limit cut off, which books the same exact zero.
        std::int32_t activations(std::int32_t x, std::int32_t y) const
        {
            const TileState* pState = readTileState(x, y);
            if (pState == nullptr || pState->label.boundaries == UNREACHABLE)
            {
                return UNREACHABLE;
            }
            return arrivalActivationsOf(pState->label.boundaries);
        }

        // Summed entry cost of the very route this arrival was credited to, not the cheapest route.
        std::int32_t routeCost(std::int32_t x, std::int32_t y) const
        {
            const TileState* pState = readTileState(x, y);
            if (pState == nullptr || pState->label.boundaries == UNREACHABLE)
            {
                return UNREACHABLE;
            }
            return pState->label.cost;
        }

    private:
        struct ActivationLabel
        {
            std::int32_t boundaries;
            std::int32_t spent;
            // Carried for diagnostics; it takes no part in the ordering.
            std::int32_t cost;
        };

        struct TileState
        {
            std::int64_t generation{0};
            ActivationLabel label{UNREACHABLE, 0, 0};
            bool settled{false};
            bool read{false};
        };

        struct HeapEntry
        {
            std::int32_t boundaries;
            std::int32_t spent;
            std::int32_t cost;
            std::int64_t sequence;
            std::int32_t x;
            std::int32_t y;
        };

        // Push order breaks label ties.
        struct HeapOrder
        {
            bool operator()(const HeapEntry & lhs, const HeapEntry & rhs) const
            {
                if (lhs.boundaries != rhs.boundaries)
                {
                    return lhs.boundaries > rhs.boundaries;
                }
                if (lhs.spent != rhs.spent)
                {
                    return lhs.spent > rhs.spent;
                }
                return lhs.sequence > rhs.sequence;
            }
        };

        static bool isEarlier(const ActivationLabel & candidate, const ActivationLabel & known)
        {
            if (candidate.boundaries != known.boundaries)
            {
                return candidate.boundaries < known.boundaries;
            }
            return candidate.spent < known.spent;
        }

        static ActivationLabel enteredLabel(const ActivationLabel & from, std::int32_t cost, std::int32_t movementPoints)
        {
            if (from.spent + cost <= movementPoints)
            {
                return ActivationLabel{from.boundaries, from.spent + cost, from.cost + cost};
            }
            return ActivationLabel{from.boundaries + 1, cost, from.cost + cost};
        }

        static bool exceedsLimit(std::int32_t boundaries, std::int32_t activationLimit)
        {
            if (activationLimit == UNBOUNDED_ACTIVATIONS)
            {
                return false;
            }
            // Strictly greater, so an arrival exactly on the limit still gets a label to be priced.
            return arrivalActivationsOf(boundaries) > activationLimit;
        }

        static constexpr std::array<EntryDirection, ENTRY_DIRECTION_COUNT> EXPANSION_ORDER{
            EntryDirection::East,
            EntryDirection::West,
            EntryDirection::South,
            EntryDirection::North,
        };

        void resize(std::int32_t width, std::int32_t height)
        {
            if (width == m_width && height == m_height)
            {
                return;
            }
            m_width = std::max(width, 0);
            m_height = std::max(height, 0);
            m_states.assign(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height), TileState{});
        }

        // A stale stamp means the tile was last touched by an older build, so it reads as untouched.
        TileState & freshState(std::size_t slot)
        {
            TileState & state = m_states[slot];
            if (state.generation != m_generation)
            {
                state = TileState{m_generation, ActivationLabel{UNREACHABLE, 0, 0}, false, false};
            }
            return state;
        }

        // Only tiles the caller asked for are settled when the loop stops, so nothing else may be read.
        const TileState* readTileState(std::int32_t x, std::int32_t y) const
        {
            if (x < 0 || y < 0 || x >= m_width || y >= m_height)
            {
                return nullptr;
            }
            const TileState & state = m_states[index(x, y)];
            if (state.generation != m_generation || !state.read)
            {
                return nullptr;
            }
            return &state;
        }

        void push(const HeapEntry & entry)
        {
            m_heap.push_back(entry);
            std::push_heap(m_heap.begin(), m_heap.end(), HeapOrder{});
        }

        HeapEntry pop()
        {
            std::pop_heap(m_heap.begin(), m_heap.end(), HeapOrder{});
            const HeapEntry entry = m_heap.back();
            m_heap.pop_back();
            return entry;
        }

        std::size_t index(std::int32_t x, std::int32_t y) const
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::int64_t m_generation{0};
        std::vector<TileState> m_states;
        std::vector<HeapEntry> m_heap;
    };
}
