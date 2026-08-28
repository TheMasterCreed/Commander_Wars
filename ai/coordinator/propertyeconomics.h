#pragma once

#include <algorithm>
#include <cstdint>

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "ai/coordinator/coordinatorcommon.h"

namespace Coordinator
{
    enum class ObjectiveKind : std::int8_t
    {
        None,
        Headquarters,
    };

    constexpr qint32 NO_CAPTURER = -1;
    constexpr qint32 NO_CAPTURE_TURNS = -1;

    struct PropertyFacts
    {
        // Index into BattlefieldKnowledge::buildings().
        qint32 buildingIndex{-1};
        qint32 x{0};
        qint32 y{0};
        QString buildingId;
        qint32 ownerId{NO_OWNER};
        // Separates an empty tile from one fog keeps unseen.
        bool visible{false};
        qint32 baseIncome{0};
        // Player::calcIncome pays a multi tile building once per covered tile.
        qint32 coveredTileCount{1};
        qint32 incomePerTurn{0};
        bool capturable{false};
        ObjectiveKind objective{ObjectiveKind::None};
        bool canProduce{false};
        QStringList productionList;
        qint32 repairTypeMask{0};
        // Index into BattlefieldKnowledge::units().
        qint32 capturerIndex{NO_CAPTURER};
        qint32 capturePoints{0};
        qint32 captureRate{0};
        qint32 captureTurnsRemaining{NO_CAPTURE_TURNS};
    };

    constexpr qint32 captureTurnsFor(qint32 points, qint32 ratePerTurn, qint32 pointsToCapture)
    {
        if (ratePerTurn <= 0)
        {
            return NO_CAPTURE_TURNS;
        }
        const qint32 remaining = std::max(pointsToCapture - points, 0);
        return (remaining + ratePerTurn - 1) / ratePerTurn;
    }

    constexpr bool repairsUnitType(qint32 repairTypeMask, qint32 unitTypeFlag)
    {
        return (repairTypeMask & unitTypeFlag) != 0;
    }
}
