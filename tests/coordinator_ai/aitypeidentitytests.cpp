#include "game/GameEnums.h"
#include "resource_management/gamemanager.h"

#include <array>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace
{
constexpr qint32 RAW_COORDINATED = -4;
constexpr qint32 RAW_DUMMY = -3;
constexpr qint32 RAW_MOVE_PLANNER = -2;
constexpr qint32 RAW_PROXY = -1;
constexpr qint32 RAW_HUMAN = 0;
constexpr qint32 RAW_VERY_EASY = 1;
constexpr qint32 RAW_NORMAL = 2;
constexpr qint32 RAW_NORMAL_OFFENSIVE = 3;
constexpr qint32 RAW_NORMAL_DEFENSIVE = 4;
constexpr qint32 RAW_HEAVY_BASE = 5;
constexpr qint32 RAW_CLOSED = 199;
constexpr qint32 RAW_OPEN = 200;
constexpr qint32 FIRST_HEAVY_HOLE = 194;
constexpr qint32 SECOND_HEAVY_HOLE = 195;
constexpr qint32 POST_HOLE_HEAVY_INDEX = 196;
constexpr qint32 POST_HOLE_HEAVY_RAW = 201;

static_assert(GameEnums::AiTypes_Coordinated == RAW_COORDINATED);
static_assert(GameEnums::AiTypes_DummyAi == RAW_DUMMY);
static_assert(GameEnums::AiTypes_MovePlanner == RAW_MOVE_PLANNER);
static_assert(GameEnums::AiTypes_ProxyAi == RAW_PROXY);
static_assert(GameEnums::AiTypes_Human == RAW_HUMAN);
static_assert(GameEnums::AiTypes_VeryEasy == RAW_VERY_EASY);
static_assert(GameEnums::AiTypes_Normal == RAW_NORMAL);
static_assert(GameEnums::AiTypes_NormalOffensive == RAW_NORMAL_OFFENSIVE);
static_assert(GameEnums::AiTypes_NormalDefensive == RAW_NORMAL_DEFENSIVE);
static_assert(GameEnums::AiTypes_Heavy == RAW_HEAVY_BASE);
static_assert(GameEnums::AiTypes_Closed == RAW_CLOSED);
static_assert(GameEnums::AiTypes_Open == RAW_OPEN);

int failureCount = 0;

void check(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

template <typename Actual, typename Expected>
void checkEqual(Actual actual, Expected expected, std::string_view message)
{
    if (actual != expected)
    {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        ++failureCount;
    }
}

qint32 raw(GameEnums::AiTypes type)
{
    return static_cast<qint32>(type);
}

void checkHeavyType(qint32 index, qint32 expectedRaw)
{
    const std::optional<GameEnums::AiTypes> type = GameManager::getHeavyAiType(index);
    check(type.has_value(), "heavy index is representable");
    if (type.has_value())
    {
        checkEqual(raw(*type), expectedRaw, "heavy index raw value");
    }
}

void checkHeavyIndex(qint32 rawType, qint32 expectedIndex)
{
    const std::optional<qint32> index =
        GameManager::getHeavyAiIndex(static_cast<GameEnums::AiTypes>(rawType));
    check(index.has_value(), "heavy raw value is representable");
    if (index.has_value())
    {
        checkEqual(*index, expectedIndex, "heavy raw index");
    }
}

void testFrozenRawValues()
{
    checkEqual(raw(GameEnums::AiTypes_Coordinated), RAW_COORDINATED, "Coordinated raw value");
    checkEqual(raw(GameEnums::AiTypes_DummyAi), RAW_DUMMY, "Dummy raw value");
    checkEqual(raw(GameEnums::AiTypes_MovePlanner), RAW_MOVE_PLANNER, "MovePlanner raw value");
    checkEqual(raw(GameEnums::AiTypes_ProxyAi), RAW_PROXY, "Proxy raw value");
    checkEqual(raw(GameEnums::AiTypes_Human), RAW_HUMAN, "Human raw value");
    checkEqual(raw(GameEnums::AiTypes_VeryEasy), RAW_VERY_EASY, "VeryEasy raw value");
    checkEqual(raw(GameEnums::AiTypes_Normal), RAW_NORMAL, "Normal raw value");
    checkEqual(raw(GameEnums::AiTypes_NormalOffensive), RAW_NORMAL_OFFENSIVE, "NormalOffensive raw value");
    checkEqual(raw(GameEnums::AiTypes_NormalDefensive), RAW_NORMAL_DEFENSIVE, "NormalDefensive raw value");
    checkEqual(raw(GameEnums::AiTypes_Heavy), RAW_HEAVY_BASE, "Heavy base raw value");
    checkEqual(raw(GameEnums::AiTypes_Closed), RAW_CLOSED, "Closed raw value");
    checkEqual(raw(GameEnums::AiTypes_Open), RAW_OPEN, "Open raw value");
}

void testSemanticEnums()
{
    check(GameEnums::isInternalAiType(GameEnums::AiTypes_DummyAi), "Dummy is internal");
    check(GameEnums::isInternalAiType(GameEnums::AiTypes_MovePlanner), "MovePlanner is internal");
    check(GameEnums::isInternalAiType(GameEnums::AiTypes_ProxyAi), "Proxy is internal");
    check(!GameEnums::isInternalAiType(GameEnums::AiTypes_Coordinated), "Coordinated is selectable");
    check(!GameEnums::isInternalAiType(GameEnums::AiTypes_Human), "Human is not internal");

    check(GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_Coordinated), "Coordinated is built in");
    check(GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_VeryEasy), "VeryEasy is built in");
    check(GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_Normal), "Normal is built in");
    check(GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_NormalOffensive), "NormalOffensive is built in");
    check(GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_NormalDefensive), "NormalDefensive is built in");
    check(!GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_Heavy), "Heavy is loaded");
    check(!GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_Closed), "Closed is not a computer");
    check(!GameEnums::isBuiltInComputerAiType(GameEnums::AiTypes_Open), "Open is not a computer");
}

void testHeavyConversions()
{
    constexpr std::array<qint32, 4> indices{0, 1, 9, POST_HOLE_HEAVY_INDEX};
    constexpr std::array<qint32, 4> rawTypes{5, 6, 14, POST_HOLE_HEAVY_RAW};
    for (std::size_t position = 0; position < indices.size(); ++position)
    {
    checkHeavyType(indices[position], rawTypes[position]);
        checkHeavyIndex(rawTypes[position], indices[position]);
    }
    checkHeavyType(193, 198);
    checkHeavyIndex(198, 193);

    check(!GameManager::getHeavyAiType(-1).has_value(), "negative heavy index is rejected");
    check(!GameManager::getHeavyAiType(FIRST_HEAVY_HOLE).has_value(), "Closed alias index is rejected");
    check(!GameManager::getHeavyAiType(SECOND_HEAVY_HOLE).has_value(), "Open alias index is rejected");
    check(!GameManager::getHeavyAiType(std::numeric_limits<qint32>::max()).has_value(),
          "overflowing heavy index is rejected");

    check(!GameManager::getHeavyAiIndex(GameEnums::AiTypes_Coordinated).has_value(),
          "Coordinated is not Heavy");
    check(!GameManager::getHeavyAiIndex(GameEnums::AiTypes_DummyAi).has_value(), "Dummy is not Heavy");
    check(!GameManager::getHeavyAiIndex(GameEnums::AiTypes_Human).has_value(), "Human is not Heavy");
    check(!GameManager::getHeavyAiIndex(GameEnums::AiTypes_Closed).has_value(), "Closed is not Heavy");
    check(!GameManager::getHeavyAiIndex(GameEnums::AiTypes_Open).has_value(), "Open is not Heavy");
}

void testLoadedHeavyClassification()
{
    check(!GameManager::isHeavyAiType(GameEnums::AiTypes_Heavy, 0), "unloaded Heavy 0 is rejected");
    check(GameManager::isHeavyAiType(GameEnums::AiTypes_Heavy, 1), "loaded Heavy 0 is accepted");
    check(!GameManager::isHeavyAiType(static_cast<GameEnums::AiTypes>(6), 1), "unloaded Heavy 1 is rejected");
    check(GameManager::isHeavyAiType(static_cast<GameEnums::AiTypes>(6), 2), "loaded Heavy 1 is accepted");
    check(GameManager::isHeavyAiType(static_cast<GameEnums::AiTypes>(14), 10), "loaded Heavy 9 is accepted");
    check(!GameManager::isHeavyAiType(static_cast<GameEnums::AiTypes>(POST_HOLE_HEAVY_RAW), 196),
          "unloaded post-hole Heavy is rejected");
    check(GameManager::isHeavyAiType(static_cast<GameEnums::AiTypes>(POST_HOLE_HEAVY_RAW), 197),
          "loaded post-hole Heavy is accepted");
    check(!GameManager::isHeavyAiType(GameEnums::AiTypes_Closed, std::numeric_limits<qint32>::max()),
          "Closed never aliases Heavy");
    check(!GameManager::isHeavyAiType(GameEnums::AiTypes_Open, std::numeric_limits<qint32>::max()),
          "Open never aliases Heavy");
    check(!GameManager::isHeavyAiType(GameEnums::AiTypes_Heavy, -1), "negative loaded count is rejected");
}

void testComputerAndKnownClassification()
{
    check(GameManager::isComputerAiType(GameEnums::AiTypes_Coordinated, 0), "Coordinated is a computer");
    check(GameManager::isComputerAiType(GameEnums::AiTypes_Normal, 0), "Normal is a computer");
    check(GameManager::isComputerAiType(GameEnums::AiTypes_Heavy, 1), "loaded Heavy is a computer");
    check(GameManager::isComputerAiType(static_cast<GameEnums::AiTypes>(POST_HOLE_HEAVY_RAW), 197),
          "loaded post-hole Heavy is a computer");
    check(!GameManager::isComputerAiType(GameEnums::AiTypes_Heavy, 0), "unloaded Heavy is not a computer");
    check(!GameManager::isComputerAiType(GameEnums::AiTypes_Human, 1), "Human is not a computer");
    check(!GameManager::isComputerAiType(GameEnums::AiTypes_DummyAi, 1), "Dummy is not a computer");
    check(!GameManager::isComputerAiType(GameEnums::AiTypes_Closed, 1), "Closed is not a computer");
    check(!GameManager::isComputerAiType(GameEnums::AiTypes_Open, 1), "Open is not a computer");

    check(GameManager::isKnownAiType(GameEnums::AiTypes_Coordinated, 0), "Coordinated is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_DummyAi, 0), "Dummy is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_MovePlanner, 0), "MovePlanner is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_ProxyAi, 0), "Proxy is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_Human, 0), "Human is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_Normal, 0), "Normal is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_Heavy, 1), "loaded Heavy is known");
    check(GameManager::isKnownAiType(static_cast<GameEnums::AiTypes>(POST_HOLE_HEAVY_RAW), 197),
          "loaded post-hole Heavy is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_Closed, 0), "Closed is known");
    check(GameManager::isKnownAiType(GameEnums::AiTypes_Open, 0), "Open is known");
    check(!GameManager::isKnownAiType(GameEnums::AiTypes_Heavy, 0), "unloaded Heavy is unknown");
    check(!GameManager::isKnownAiType(static_cast<GameEnums::AiTypes>(-5), 0), "unknown negative type is rejected");
    check(!GameManager::isKnownAiType(static_cast<GameEnums::AiTypes>(202), 197),
          "unloaded positive type is rejected");
}
}

int main()
{
    testFrozenRawValues();
    testSemanticEnums();
    testHeavyConversions();
    testLoadedHeavyClassification();
    testComputerAndKnownClassification();
    return failureCount == 0 ? 0 : 1;
}
