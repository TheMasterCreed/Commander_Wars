#include "game/GameEnums.h"
#include "resource_management/gamemanager.h"

#include <QByteArray>
#include <QDataStream>

#include <array>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
constexpr qint32 LOADED_HEAVY_AI_COUNT = 3;
constexpr GameEnums::AiTypes SENTINEL_AI_TYPE = GameEnums::AiTypes_NormalDefensive;

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

QByteArray encodeRaw(qint32 rawType)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    stream << rawType;
    checkEqual(stream.status(), QDataStream::Ok, "raw encode status");
    return data;
}

QByteArray encodeType(GameEnums::AiTypes type)
{
    return encodeRaw(raw(type));
}

bool decodeType(const QByteArray & data,
                qint32 loadedHeavyAiCount,
                GameEnums::AiTypes & destination,
                QDataStream::Status & status)
{
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_5);

    qint32 rawType = 0;
    stream >> rawType;
    const auto candidate = static_cast<GameEnums::AiTypes>(rawType);
    if (stream.status() != QDataStream::Ok ||
        !GameManager::isKnownAiType(candidate, loadedHeavyAiCount))
    {
        stream.setStatus(QDataStream::ReadCorruptData);
        status = stream.status();
        return false;
    }

    destination = candidate;
    status = stream.status();
    return true;
}

void checkRoundTrip(GameEnums::AiTypes expected, qint32 loadedHeavyAiCount = LOADED_HEAVY_AI_COUNT)
{
    GameEnums::AiTypes decoded = SENTINEL_AI_TYPE;
    QDataStream::Status status = QDataStream::ReadCorruptData;
    const bool accepted = decodeType(encodeType(expected),
                                     loadedHeavyAiCount,
                                     decoded,
                                     status);
    check(accepted, "valid AI type is accepted");
    checkEqual(status, QDataStream::Ok, "valid AI type stream status");
    checkEqual(raw(decoded), raw(expected), "valid AI type round trip");
}

void checkRejected(qint32 rawType, qint32 loadedHeavyAiCount)
{
    GameEnums::AiTypes decoded = SENTINEL_AI_TYPE;
    QDataStream::Status status = QDataStream::Ok;
    const bool accepted = decodeType(encodeRaw(rawType),
                                     loadedHeavyAiCount,
                                     decoded,
                                     status);
    check(!accepted, "invalid AI type is rejected");
    check(status != QDataStream::Ok, "invalid AI type marks stream");
    checkEqual(raw(decoded), raw(SENTINEL_AI_TYPE), "invalid AI type does not mutate destination");
}

void testSignedWireRoundTrips()
{
    constexpr std::array<GameEnums::AiTypes, 14> validTypes{
        GameEnums::AiTypes_Coordinated,
        GameEnums::AiTypes_DummyAi,
        GameEnums::AiTypes_MovePlanner,
        GameEnums::AiTypes_ProxyAi,
        GameEnums::AiTypes_Human,
        GameEnums::AiTypes_VeryEasy,
        GameEnums::AiTypes_Normal,
        GameEnums::AiTypes_NormalOffensive,
        GameEnums::AiTypes_NormalDefensive,
        GameEnums::AiTypes_Heavy,
        static_cast<GameEnums::AiTypes>(6),
        static_cast<GameEnums::AiTypes>(7),
        GameEnums::AiTypes_Closed,
        GameEnums::AiTypes_Open,
    };

    for (GameEnums::AiTypes type : validTypes)
    {
        checkRoundTrip(type);
    }
}

void testRawIdentityPreservation()
{
    GameEnums::AiTypes decoded = SENTINEL_AI_TYPE;
    QDataStream::Status status = QDataStream::ReadCorruptData;

    check(decodeType(encodeRaw(-4), 0, decoded, status), "raw -4 is accepted");
    checkEqual(decoded, GameEnums::AiTypes_Coordinated, "raw -4 remains Coordinated");

    check(decodeType(encodeRaw(5), 1, decoded, status), "raw 5 is accepted");
    checkEqual(decoded, GameEnums::AiTypes_Heavy, "raw 5 remains Heavy 0");

    check(decodeType(encodeRaw(199), 0, decoded, status), "raw 199 is accepted");
    checkEqual(decoded, GameEnums::AiTypes_Closed, "raw 199 remains Closed");
    check(!GameManager::isHeavyAiType(decoded, std::numeric_limits<qint32>::max()),
          "Closed never decodes as Heavy");

    check(decodeType(encodeRaw(200), 0, decoded, status), "raw 200 is accepted");
    checkEqual(decoded, GameEnums::AiTypes_Open, "raw 200 remains Open");
    check(!GameManager::isHeavyAiType(decoded, std::numeric_limits<qint32>::max()),
          "Open never decodes as Heavy");
}

void testInvalidValuesFailBeforeMutation()
{
    constexpr std::array<qint32, 7> invalidTypes{
        std::numeric_limits<qint32>::min(),
        -5,
        8,
        198,
        201,
        202,
        std::numeric_limits<qint32>::max(),
    };
    for (qint32 rawType : invalidTypes)
    {
        checkRejected(rawType, LOADED_HEAVY_AI_COUNT);
    }

    checkRejected(5, 0);
    checkRejected(6, 1);
    checkRejected(7, 2);
}

void testSparseHeavyBoundaries()
{
    constexpr qint32 LAST_PRE_HOLE_HEAVY_INDEX = 193;
    constexpr qint32 FIRST_COLLIDING_HEAVY_INDEX = 194;
    constexpr qint32 SECOND_COLLIDING_HEAVY_INDEX = 195;
    constexpr qint32 FIRST_POST_HOLE_HEAVY_INDEX = 196;

    const auto preHole = GameManager::getHeavyAiType(LAST_PRE_HOLE_HEAVY_INDEX);
    check(preHole.has_value(), "pre-hole Heavy index is representable");
    if (preHole.has_value())
    {
        checkEqual(raw(*preHole), 198, "pre-hole Heavy raw value");
        checkRoundTrip(*preHole, LAST_PRE_HOLE_HEAVY_INDEX + 1);
    }

    check(!GameManager::getHeavyAiType(FIRST_COLLIDING_HEAVY_INDEX).has_value(),
          "Closed collision index is rejected");
    check(!GameManager::getHeavyAiType(SECOND_COLLIDING_HEAVY_INDEX).has_value(),
          "Open collision index is rejected");

    const auto postHole = GameManager::getHeavyAiType(FIRST_POST_HOLE_HEAVY_INDEX);
    check(postHole.has_value(), "post-hole Heavy index is representable");
    if (postHole.has_value())
    {
        checkEqual(raw(*postHole), 201, "post-hole Heavy raw value");

        GameEnums::AiTypes decoded = SENTINEL_AI_TYPE;
        QDataStream::Status status = QDataStream::ReadCorruptData;
        check(decodeType(encodeType(*postHole),
                         FIRST_POST_HOLE_HEAVY_INDEX + 1,
                         decoded,
                         status),
              "loaded post-hole Heavy is accepted");
        checkEqual(raw(decoded), 201, "post-hole Heavy round trip");
    }
}

void testTruncatedValueFailsBeforeMutation()
{
    GameEnums::AiTypes decoded = SENTINEL_AI_TYPE;
    QDataStream::Status status = QDataStream::Ok;
    const bool accepted = decodeType(QByteArray(),
                                     LOADED_HEAVY_AI_COUNT,
                                     decoded,
                                     status);
    check(!accepted, "truncated AI type is rejected");
    check(status != QDataStream::Ok, "truncated AI type marks stream");
    checkEqual(raw(decoded), raw(SENTINEL_AI_TYPE), "truncated AI type does not mutate destination");
}
}

int main()
{
    testSignedWireRoundTrips();
    testRawIdentityPreservation();
    testInvalidValuesFailBeforeMutation();
    testSparseHeavyBoundaries();
    testTruncatedValueFailsBeforeMutation();
    return failureCount == 0 ? 0 : 1;
}
