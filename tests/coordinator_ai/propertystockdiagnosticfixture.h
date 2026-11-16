#pragma once

#include <array>
#include <cstring>
#include <cstdint>

#include "ai/coordinator/propertystock.h"
#include "ai/coordinator/propertystockbuilder.h"

namespace Coordinator
{
    struct PropertyStockDiagnosticFixture
    {
        static constexpr std::int32_t HORIZON = 8;
        static constexpr std::int32_t WIDTH = 3;
        static constexpr std::int32_t POINTS = 4;
        static constexpr std::int32_t RATE = 2;

        struct ReplyKeys
        {
            CanonicalPlanActionKey today;
            CanonicalPlanActionKey damaged;
            CanonicalPlanActionKey dead;
        };

        static ReplyKeys replyKeys()
        {
            return ReplyKeys{
                .today = CanonicalPlanActionKey::fromAction(0, 0, 0, true),
                .damaged = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, 1, false),
                // Destruction also removes a capture-rate bonus that would remain after hp loss.
                .dead = *CanonicalPlanActionKey::checkedFromAction(0, 0, 0, true, 1, true),
            };
        }

        struct Snapshot
        {
            PropertyStockField value;
            std::array<unsigned char, sizeof(PropertyStockField)> bytes;
        };

        static PropertyStockField make(std::int32_t rowCount = 1, std::int32_t carried = 0)
        {
            PropertyStockField field;
            field.m_horizonTurns = HORIZON;
            field.m_width = WIDTH;
            field.m_height = 1;
            field.m_capturePointsToCapture = POINTS;
            field.m_columnAtTile = {0, NO_STOCK_COLUMN, 1};
            field.m_propertyAtTile = {0, NO_PROPERTY_INDEX, 1};
            field.m_columnTiles = {{0, 0}, {WIDTH - 1, 0}};
            field.m_columnCapturer = {0, NO_UNIT};
            field.m_columnCapturePoints = {carried, 0};
            field.m_ours.columns = {
                PropertyStockColumn{.slot = 0, .tile = field.m_columnTiles[0], .income = {2, 2}},
                PropertyStockColumn{.slot = 1, .tile = field.m_columnTiles[1], .income = {1, 1}},
            };
            MobilityCostGrid grid(WIDTH, 1);
            for (std::int32_t x = 0; x < WIDTH; ++x)
            {
                grid.setCost(x, 0, EntryDirection::East, 1);
                grid.setCost(x, 0, EntryDirection::West, 1);
            }
            field.m_grids.push_back(std::move(grid));
            for (std::int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
            {
                const TilePoint tile{rowIndex == 0 ? (carried > 0 ? 0 : 1) : WIDTH - 1, 0};
                field.m_ours.rows.push_back(PropertyStockRow{
                    .knowledgeIndex = rowIndex, .gridIdentity = 0, .movementPoints = 1,
                    .capturePoints = carried, .captureRate = RATE, .tile = tile,
                });
                field.arrivalsFrom(0, 1, tile);
                constexpr std::array<MilliFunds, 2> CENTER_WEIGHTS{12, 6};
                constexpr std::array<MilliFunds, 2> CARRIED_WEIGHTS{14, 5};
                constexpr std::array<MilliFunds, 2> SECOND_WEIGHTS{10, 6};
                const auto & weights =
                    rowIndex == 0 ? (carried > 0 ? CARRIED_WEIGHTS : CENTER_WEIGHTS) : SECOND_WEIGHTS;
                field.m_ours.weights.insert(field.m_ours.weights.end(), weights.begin(), weights.end());
            }
            rebuildMarginals(field);
            return field;
        }

        static void rebuildMarginals(PropertyStockField & field)
        {
            field.m_marginals.clear();
            field.m_marginals.emplace(PropertyStockField::MarginalKey{0, NO_STOCK_COLUMN},
                                      buildMarginalTable(field.m_ours, 0, NO_STOCK_COLUMN));
        }

        static void setWeight(PropertyStockField & field, std::int32_t row, std::int32_t node, MilliFunds weight)
        {
            const std::size_t nodes = field.m_ours.columns.size();
            field.m_ours.weights[static_cast<std::size_t>(row) * nodes + static_cast<std::size_t>(node)] = weight;
            rebuildMarginals(field);
        }

        static Snapshot snapshot(const PropertyStockField & field)
        {
            Snapshot result{field, {}};
            std::memcpy(result.bytes.data(), &field, sizeof(field));
            return result;
        }

        static bool unchanged(const PropertyStockField & field, const Snapshot & before)
        {
            const auto & old = before.value;
            if (std::memcmp(before.bytes.data(), &field, sizeof(field)) != 0 ||
                field.m_ours.rows != old.m_ours.rows || field.m_ours.columns != old.m_ours.columns ||
                field.m_ours.weights != old.m_ours.weights || field.m_theirs.rows != old.m_theirs.rows ||
                field.m_theirs.columns != old.m_theirs.columns || field.m_theirs.weights != old.m_theirs.weights ||
                field.m_grids != old.m_grids || field.m_columnAtTile != old.m_columnAtTile ||
                field.m_propertyAtTile != old.m_propertyAtTile || field.m_columnTiles != old.m_columnTiles ||
                field.m_columnCapturer != old.m_columnCapturer ||
                field.m_columnCapturePoints != old.m_columnCapturePoints ||
                field.m_enemyArrivals != old.m_enemyArrivals ||
                field.m_enemyOptimaWithoutRow != old.m_enemyOptimaWithoutRow ||
                field.m_arrivals.size() != old.m_arrivals.size() || field.m_marginals.size() != old.m_marginals.size())
            {
                return false;
            }
            for (const auto & [key, value] : field.m_arrivals)
            {
                const auto found = old.m_arrivals.find(key);
                if (found == old.m_arrivals.end() || value.activations != found->second.activations)
                {
                    return false;
                }
            }
            for (const auto & [key, value] : field.m_marginals)
            {
                const auto found = old.m_marginals.find(key);
                if (found == old.m_marginals.end() || value.unmatched != found->second.unmatched ||
                    value.withoutColumn != found->second.withoutColumn || value.solves != found->second.solves ||
                    value.augmentations != found->second.augmentations)
                {
                    return false;
                }
            }
            for (std::int32_t x = 0; x < WIDTH; ++x)
            {
                if (field.m_reach.activations(x, 0) != old.m_reach.activations(x, 0) ||
                    field.m_reach.routeCost(x, 0) != old.m_reach.routeCost(x, 0))
                {
                    return false;
                }
            }
            return true;
        }
    };
}
