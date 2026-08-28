#include "ai/airandom.h"

#include <QRandomGenerator>

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
constexpr quint32 STREAM_SEED = 0x5EED1234u;
constexpr quint32 ALTERNATE_STREAM_SEED = 0x0BADC0DEu;
constexpr std::size_t REPLAY_DRAWS = 100;
constexpr std::size_t RESUME_COUNTER = 40;
constexpr qint32 RANGE_LOW = -2;
constexpr qint32 RANGE_HIGH = 2;
constexpr std::size_t RANGE_DRAWS = 1000;
constexpr quint32 DERIVED_SEED = 1881681314u;
constexpr std::array<qint32, 12> EXPECTED_VALUES{
    594, 275, 153, 79, 566, 30, 359, 52, 111, 742, 986, 712,
};

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

void testPinnedSequence()
{
    AiRandom random;
    checkEqual(random.getSeed(), 0u, "default seed");
    checkEqual(random.getDrawCounter(), 0u, "default counter");

    random.reseed(STREAM_SEED);
    checkEqual(random.getSeed(), STREAM_SEED, "reseeded seed");
    checkEqual(random.getDrawCounter(), 0u, "reseeded counter");

    for (std::size_t index = 0; index < EXPECTED_VALUES.size(); ++index)
    {
        checkEqual(random.bounded(0, 1000), EXPECTED_VALUES[index], "pinned V2 draw");
        checkEqual(random.getSeed(), STREAM_SEED, "seed remains stable");
        checkEqual(random.getDrawCounter(), static_cast<quint32>(index + 1), "counter advances once");
    }
}

void testReplayAndRestore()
{
    std::array<qint32, REPLAY_DRAWS> expected{};
    AiRandom reference;
    reference.reseed(STREAM_SEED);
    for (qint32 & value : expected)
    {
        value = reference.bounded(-1000000, 1000000);
    }

    AiRandom replay;
    replay.reseed(STREAM_SEED);
    for (qint32 value : expected)
    {
        checkEqual(replay.bounded(-1000000, 1000000), value, "same seed replay");
    }

    AiRandom resumed;
    resumed.restore(STREAM_SEED, static_cast<quint32>(RESUME_COUNTER));
    checkEqual(resumed.getSeed(), STREAM_SEED, "restored seed");
    checkEqual(resumed.getDrawCounter(), static_cast<quint32>(RESUME_COUNTER), "restored counter");
    for (std::size_t index = RESUME_COUNTER; index < expected.size(); ++index)
    {
        checkEqual(resumed.bounded(-1000000, 1000000), expected[index], "restored continuation");
    }

    AiRandom alternate;
    alternate.reseed(ALTERNATE_STREAM_SEED);
    bool diverged = false;
    for (std::size_t index = 0; index < EXPECTED_VALUES.size(); ++index)
    {
        if (alternate.bounded(0, 1000) != EXPECTED_VALUES[index])
        {
            diverged = true;
            break;
        }
    }
    check(diverged, "different seeds diverge");
}

void testBoundsAndCounterWrap()
{
    AiRandom guarded;
    guarded.restore(STREAM_SEED, 11u);
    checkEqual(guarded.bounded(7, 3), 7, "reversed bounds return low");
    checkEqual(guarded.getDrawCounter(), 11u, "reversed bounds do not draw");
    checkEqual(guarded.bounded(-4, -4), -4, "equal bounds return low");
    checkEqual(guarded.getDrawCounter(), 11u, "equal bounds do not draw");

    AiRandom ranged;
    ranged.reseed(STREAM_SEED);
    std::array<bool, RANGE_HIGH - RANGE_LOW + 1> seen{};
    for (std::size_t index = 0; index < RANGE_DRAWS; ++index)
    {
        const qint32 value = ranged.bounded(RANGE_LOW, RANGE_HIGH);
        const bool inRange = value >= RANGE_LOW && value <= RANGE_HIGH;
        check(inRange, "bounded draw stays in range");
        if (inRange)
        {
            seen[static_cast<std::size_t>(value - RANGE_LOW)] = true;
        }
    }
    for (bool valueSeen : seen)
    {
        check(valueSeen, "inclusive range value is reachable");
    }
    checkEqual(ranged.getDrawCounter(), static_cast<quint32>(RANGE_DRAWS), "range draw counter");

    AiRandom fullRangeA;
    AiRandom fullRangeB;
    fullRangeA.reseed(STREAM_SEED);
    fullRangeB.reseed(STREAM_SEED);
    const qint32 fullRangeValueA =
        fullRangeA.bounded(std::numeric_limits<qint32>::min(), std::numeric_limits<qint32>::max());
    const qint32 fullRangeValueB =
        fullRangeB.bounded(std::numeric_limits<qint32>::min(), std::numeric_limits<qint32>::max());
    checkEqual(fullRangeValueA, fullRangeValueB, "full qint32 range is deterministic");
    checkEqual(fullRangeA.getDrawCounter(), 1u, "full qint32 range draws once");

    AiRandom wrappedA;
    AiRandom wrappedB;
    wrappedA.restore(STREAM_SEED, std::numeric_limits<quint32>::max());
    wrappedB.restore(STREAM_SEED, std::numeric_limits<quint32>::max());
    checkEqual(wrappedA.bounded(0, 1000), wrappedB.bounded(0, 1000), "wrapped counter is deterministic");
    checkEqual(wrappedA.getDrawCounter(), 0u, "counter wrap is defined");
}

quint32 deriveSeed(const QString & seedNamespace,
                   qint32 algorithmVersion,
                   qint32 playerId,
                   qint32 currentDay,
                   qint32 generation,
                   const QByteArray & mapHash)
{
    return AiRandom::deriveSeed(seedNamespace,
                                QDataStream::Qt_6_5,
                                algorithmVersion,
                                playerId,
                                currentDay,
                                generation,
                                mapHash);
}

void testSeedDerivation()
{
    const QString seedNamespace = QStringLiteral("coordinated-ai-core");
    const qint32 algorithmVersion = static_cast<qint32>(AiRandomAlgorithm::V2);
    const qint32 playerId = 3;
    const qint32 currentDay = 17;
    const qint32 generation = 2;
    const QByteArray mapHash = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const quint32 seed = deriveSeed(seedNamespace, algorithmVersion, playerId, currentDay, generation, mapHash);

    checkEqual(seed, DERIVED_SEED, "pinned Qt 6.5 seed derivation");
    checkEqual(deriveSeed(seedNamespace, algorithmVersion, playerId, currentDay, generation, mapHash),
               seed,
               "seed derivation repeats");
    check(deriveSeed(seedNamespace + QStringLiteral("-other"),
                     algorithmVersion,
                     playerId,
                     currentDay,
                     generation,
                     mapHash) != seed,
          "namespace affects derived seed");
    check(deriveSeed(seedNamespace,
                     static_cast<qint32>(AiRandomAlgorithm::V1),
                     playerId,
                     currentDay,
                     generation,
                     mapHash) != seed,
          "algorithm affects derived seed");
    check(deriveSeed(seedNamespace, algorithmVersion, playerId + 1, currentDay, generation, mapHash) != seed,
          "player affects derived seed");
    check(deriveSeed(seedNamespace, algorithmVersion, playerId, currentDay + 1, generation, mapHash) != seed,
          "day affects derived seed");
    check(deriveSeed(seedNamespace, algorithmVersion, playerId, currentDay, generation + 1, mapHash) != seed,
          "generation affects derived seed");
    check(deriveSeed(seedNamespace,
                     algorithmVersion,
                     playerId,
                     currentDay,
                     generation,
                     mapHash + QByteArrayLiteral("x")) != seed,
          "map hash affects derived seed");
}

void testQtGlobalGeneratorIsolation()
{
    QRandomGenerator globalSnapshot(*QRandomGenerator::global());

    AiRandom random;
    random.reseed(STREAM_SEED);
    for (std::size_t index = 0; index < REPLAY_DRAWS; ++index)
    {
        random.bounded(-1000, 1000);
    }
    random.restore(ALTERNATE_STREAM_SEED, 12u);
    random.bounded(std::numeric_limits<qint32>::min(), std::numeric_limits<qint32>::max());
    deriveSeed(QStringLiteral("coordinated-ai-core"),
               static_cast<qint32>(AiRandomAlgorithm::V2),
               3,
               17,
               2,
               QByteArrayLiteral("map"));

    check(globalSnapshot == *QRandomGenerator::global(), "AiRandom leaves Qt global generator untouched");
}
}

int main()
{
    testPinnedSequence();
    testReplayAndRestore();
    testBoundsAndCounterWrap();
    testSeedDerivation();
    testQtGlobalGeneratorIsolation();
    return failureCount == 0 ? 0 : 1;
}
