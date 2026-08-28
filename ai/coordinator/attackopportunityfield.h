#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ai/coordinator/mobilityfield.h"

namespace Coordinator
{
    struct AttackerSpec
    {
        std::int32_t unitIndex{-1};
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t movementPoints{0};
        std::int32_t minRange{1};
        std::int32_t maxRange{1};
        bool canMoveAndFire{false};
        // Caller owned, read during build() only.
        const MobilityCostGrid* grid{nullptr};
    };

    class AttackOpportunityField
    {
    public:
        AttackOpportunityField() = default;

        static AttackOpportunityField build(std::int32_t width, std::int32_t height, const std::vector<AttackerSpec> & attackers)
        {
            AttackOpportunityField field(width, height);
            field.m_attackers = attackers;
            const MobilityCostGrid blockedGrid(field.m_width, field.m_height);
            field.m_slots.reserve(attackers.size());
            for (const AttackerSpec & spec : attackers)
            {
                field.m_slots.push_back(field.buildSlot(spec, blockedGrid));
            }
            // Cleared so attacker() never hands out a grid the caller may have freed.
            for (AttackerSpec & spec : field.m_attackers)
            {
                spec.grid = nullptr;
            }
            field.buildTileIndex();
            return field;
        }

        std::int32_t width() const
        {
            return m_width;
        }

        std::int32_t height() const
        {
            return m_height;
        }

        std::int32_t attackerCount() const
        {
            return static_cast<std::int32_t>(m_attackers.size());
        }

        // Precondition for attacker() and reach(): 0 <= slot < attackerCount().
        const AttackerSpec & attacker(std::int32_t slot) const
        {
            return m_attackers[static_cast<std::size_t>(slot)];
        }

        std::span<const std::int32_t> attackerSlotsAt(std::int32_t x, std::int32_t y) const
        {
            if (!onGrid(x, y))
            {
                return {};
            }
            const std::size_t tile = index(x, y);
            const std::size_t begin = m_slotOffsets[tile];
            const std::size_t end = m_slotOffsets[tile + 1];
            return std::span<const std::int32_t>(m_tileSlots.data() + begin, end - begin);
        }

        bool canAttack(std::int32_t slot, std::int32_t x, std::int32_t y) const
        {
            if (!onGrid(x, y) || !onSlot(slot))
            {
                return false;
            }
            return m_slots[static_cast<std::size_t>(slot)].targets[index(x, y)];
        }

        bool canOriginateFrom(std::int32_t slot, std::int32_t x, std::int32_t y) const
        {
            if (!onGrid(x, y) || !onSlot(slot))
            {
                return false;
            }
            return m_slots[static_cast<std::size_t>(slot)].origins[index(x, y)];
        }

        const DistanceField & reach(std::int32_t slot) const
        {
            return m_slots[static_cast<std::size_t>(slot)].reach;
        }

        bool threatened(std::int32_t x, std::int32_t y) const
        {
            return !attackerSlotsAt(x, y).empty();
        }

    private:
        struct SlotData
        {
            DistanceField reach;
            std::vector<bool> origins;
            std::vector<bool> targets;
        };

        AttackOpportunityField(std::int32_t width, std::int32_t height)
            : m_width(std::max(width, 0)),
              m_height(std::max(height, 0))
        {
        }

        bool onGrid(std::int32_t x, std::int32_t y) const
        {
            return x >= 0 && y >= 0 && x < m_width && y < m_height;
        }

        bool onSlot(std::int32_t slot) const
        {
            return slot >= 0 && slot < attackerCount();
        }

        std::size_t index(std::int32_t x, std::int32_t y) const
        {
            return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
        }

        std::size_t tileCount() const
        {
            return static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);
        }

        bool isUsable(const AttackerSpec & spec) const
        {
            return spec.grid != nullptr && spec.grid->onGrid(spec.x, spec.y) && onGrid(spec.x, spec.y);
        }

        static DistanceField buildReach(const AttackerSpec & spec, const MobilityCostGrid & blockedGrid, bool usable)
        {
            if (usable)
            {
                return DistanceField::build(*spec.grid, {{spec.x, spec.y}}, FieldExpansion::FromSources);
            }
            return DistanceField::build(blockedGrid, {}, FieldExpansion::FromSources);
        }

        SlotData buildSlot(const AttackerSpec & spec, const MobilityCostGrid & blockedGrid) const
        {
            const bool usable = isUsable(spec);
            SlotData slot{buildReach(spec, blockedGrid, usable),
                          std::vector<bool>(tileCount(), false),
                          std::vector<bool>(tileCount(), false)};
            if (!usable)
            {
                return slot;
            }
            markOrigins(spec, slot);
            markTargets(spec, slot);
            return slot;
        }

        void markOrigins(const AttackerSpec & spec, SlotData & slot) const
        {
            // Firing without moving is always legal, even with a negative allowance.
            slot.origins[index(spec.x, spec.y)] = true;
            if (!spec.canMoveAndFire)
            {
                return;
            }
            for (std::int32_t y = 0; y < m_height; ++y)
            {
                for (std::int32_t x = 0; x < m_width; ++x)
                {
                    if (slot.reach.reachable(x, y, spec.movementPoints))
                    {
                        slot.origins[index(x, y)] = true;
                    }
                }
            }
        }

        void markTargets(const AttackerSpec & spec, SlotData & slot) const
        {
            for (std::int32_t originY = 0; originY < m_height; ++originY)
            {
                for (std::int32_t originX = 0; originX < m_width; ++originX)
                {
                    if (slot.origins[index(originX, originY)])
                    {
                        markRing(spec, originX, originY, slot);
                    }
                }
            }
        }

        static std::int32_t absoluteSteps(std::int32_t offset)
        {
            if (offset < 0)
            {
                return -offset;
            }
            return offset;
        }

        void markRing(const AttackerSpec & spec, std::int32_t originX, std::int32_t originY, SlotData & slot) const
        {
            for (std::int32_t dy = -spec.maxRange; dy <= spec.maxRange; ++dy)
            {
                const std::int32_t verticalSteps = absoluteSteps(dy);
                const std::int32_t horizontalBudget = spec.maxRange - verticalSteps;
                for (std::int32_t dx = -horizontalBudget; dx <= horizontalBudget; ++dx)
                {
                    const std::int32_t horizontalSteps = absoluteSteps(dx);
                    if (verticalSteps + horizontalSteps < spec.minRange)
                    {
                        continue;
                    }
                    const std::int32_t targetX = originX + dx;
                    const std::int32_t targetY = originY + dy;
                    if (onGrid(targetX, targetY))
                    {
                        slot.targets[index(targetX, targetY)] = true;
                    }
                }
            }
        }

        void buildTileIndex()
        {
            const std::size_t tiles = tileCount();
            m_slotOffsets.assign(tiles + 1, 0);
            for (const SlotData & slot : m_slots)
            {
                for (std::size_t tile = 0; tile < tiles; ++tile)
                {
                    if (slot.targets[tile])
                    {
                        ++m_slotOffsets[tile + 1];
                    }
                }
            }
            for (std::size_t tile = 0; tile < tiles; ++tile)
            {
                m_slotOffsets[tile + 1] += m_slotOffsets[tile];
            }
            m_tileSlots.assign(m_slotOffsets[tiles], 0);
            std::vector<std::size_t> cursors(m_slotOffsets.begin(), m_slotOffsets.end() - 1);
            // Slots ascend because the outer loop walks them in order.
            for (std::size_t slot = 0; slot < m_slots.size(); ++slot)
            {
                for (std::size_t tile = 0; tile < tiles; ++tile)
                {
                    if (m_slots[slot].targets[tile])
                    {
                        m_tileSlots[cursors[tile]] = static_cast<std::int32_t>(slot);
                        ++cursors[tile];
                    }
                }
            }
        }

        std::int32_t m_width{0};
        std::int32_t m_height{0};
        std::vector<AttackerSpec> m_attackers;
        std::vector<SlotData> m_slots;
        std::vector<std::size_t> m_slotOffsets;
        std::vector<std::int32_t> m_tileSlots;
    };
}
