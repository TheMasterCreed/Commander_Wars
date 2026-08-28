#include <iostream>
#include <string_view>

#include "ai/coordinator/propertyeconomics.h"

#include "game/GameEnums.h"

namespace
{
    using Coordinator::NO_CAPTURE_TURNS;
    using Coordinator::NO_CAPTURER;
    using Coordinator::ObjectiveKind;
    using Coordinator::PropertyFacts;

    // Mirrors Unit::MAX_CAPTURE_POINTS, which this engine free target cannot include.
    constexpr qint32 POINTS_TO_CAPTURE = 20;
    constexpr qint32 FULL_HP_RATE = 10;
    constexpr qint32 DAMAGED_RATE = 7;
    constexpr qint32 STALLED_RATE = 0;
    constexpr qint32 HALF_POINTS = 10;
    constexpr qint32 ALMOST_DONE_POINTS = 15;
    constexpr qint32 OVERSHOT_POINTS = 25;
    constexpr qint32 TWO_TURNS = 2;
    constexpr qint32 THREE_TURNS = 3;
    constexpr qint32 GROUND_AND_INFANTRY_MASK = GameEnums::UnitType_Ground | GameEnums::UnitType_Infantry;
    constexpr qint32 EMPTY_REPAIR_MASK = 0;

    int failureCount = 0;

    void expect(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void testCaptureTurns()
    {
        expect(Coordinator::captureTurnsFor(0, FULL_HP_RATE, POINTS_TO_CAPTURE) == TWO_TURNS,
               "untouched building needs two turns");
        expect(Coordinator::captureTurnsFor(HALF_POINTS, FULL_HP_RATE, POINTS_TO_CAPTURE) == 1,
               "half captured building needs one turn");
        expect(Coordinator::captureTurnsFor(ALMOST_DONE_POINTS, FULL_HP_RATE, POINTS_TO_CAPTURE) == 1,
               "partial turn rounds up");
        expect(Coordinator::captureTurnsFor(POINTS_TO_CAPTURE, FULL_HP_RATE, POINTS_TO_CAPTURE) == 0,
               "finished capture needs no turn");
        expect(Coordinator::captureTurnsFor(0, DAMAGED_RATE, POINTS_TO_CAPTURE) == THREE_TURNS,
               "damaged capturer needs three turns");
        expect(Coordinator::captureTurnsFor(0, STALLED_RATE, POINTS_TO_CAPTURE) == NO_CAPTURE_TURNS,
               "zero rate never finishes");
        expect(Coordinator::captureTurnsFor(OVERSHOT_POINTS, FULL_HP_RATE, POINTS_TO_CAPTURE) == 0,
               "points above the cap need no turn");
    }

    void testRepairTypes()
    {
        expect(Coordinator::repairsUnitType(GROUND_AND_INFANTRY_MASK, GameEnums::UnitType_Ground),
               "ground repair is listed");
        expect(Coordinator::repairsUnitType(GROUND_AND_INFANTRY_MASK, GameEnums::UnitType_Infantry),
               "infantry repair is listed");
        expect(!Coordinator::repairsUnitType(GROUND_AND_INFANTRY_MASK, GameEnums::UnitType_Air),
               "air repair is not listed");
        expect(!Coordinator::repairsUnitType(EMPTY_REPAIR_MASK, GameEnums::UnitType_Ground),
               "empty mask repairs nothing");
    }

    void testDefaults()
    {
        const PropertyFacts facts;
        expect(facts.capturerIndex == NO_CAPTURER, "no capturer by default");
        expect(facts.captureTurnsRemaining == NO_CAPTURE_TURNS, "no capture estimate by default");
        expect(facts.capturePoints == 0, "no capture points by default");
        expect(facts.captureRate == 0, "no capture rate by default");
        expect(facts.objective == ObjectiveKind::None, "no objective by default");
        expect(!facts.visible, "not visible by default");
        expect(facts.productionList.isEmpty(), "no production list by default");
        expect(facts.ownerId == Coordinator::NO_OWNER, "no owner by default");
        expect(!facts.canProduce && !facts.capturable, "no capabilities by default");
    }
}

int main()
{
    testCaptureTurns();
    testRepairTypes();
    testDefaults();
    return failureCount;
}
