#include "game/gameaction.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QRandomGenerator>

#include <array>
#include <iostream>
#include <string_view>

namespace
{
constexpr quint32 ACTION_SEED = 0x5A17C0DEu;
constexpr quint32 OUTER_SEED = 0x13579BDFu;
constexpr quint32 SENTINEL_SEED = 0xFFFFFFFFu;

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
void checkEqual(const Actual & actual, const Expected & expected, std::string_view message)
{
    if (actual != expected)
    {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        ++failureCount;
    }
}

class SeedContext final
{
public:
    bool isActive() const { return m_active; }

    void activate(quint32 seed)
    {
        m_generator.seed(seed);
        m_active = true;
        m_lastSeed = seed;
        ++m_activationCount;
    }

    void deactivate() { m_active = false; ++m_deactivationCount; }
    quint32 next() { return m_generator.generate(); }
    quint32 lastSeed() const { return m_lastSeed; }
    qint32 activationCount() const { return m_activationCount; }
    qint32 deactivationCount() const { return m_deactivationCount; }

private:
    QRandomGenerator m_generator;
    bool m_active{false};
    quint32 m_lastSeed{0};
    qint32 m_activationCount{0};
    qint32 m_deactivationCount{0};
};

QByteArray serializeSeed(quint32 seed)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_5);
    GameActionSeed::write(stream, seed);
    checkEqual(stream.status(), QDataStream::Ok, "seed write status");
    return data;
}

bool restoreSeed(const QByteArray & data, quint32 & seed, QDataStream::Status & status)
{
    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_5);
    const bool restored = GameActionSeed::read(stream, seed);
    status = stream.status();
    return restored;
}

std::array<quint32, 3> replay(quint32 seed)
{
    SeedContext context;
    std::array<quint32, 3> values{};
    {
        GameActionSeed::Scope scope(context, seed);
        for (quint32 & value : values)
        {
            value = context.next();
        }
    }
    check(!context.isActive(), "owned seed scope restores inactive state");
    checkEqual(context.activationCount(), 1, "owned seed activation count");
    checkEqual(context.deactivationCount(), 1, "owned seed deactivation count");
    return values;
}

void testSeedRoundTripAndCopy()
{
    quint32 restored = SENTINEL_SEED;
    QDataStream::Status status = QDataStream::ReadCorruptData;
    check(restoreSeed(serializeSeed(ACTION_SEED), restored, status), "serialized seed restores");
    checkEqual(status, QDataStream::Ok, "seed read status");
    checkEqual(restored, ACTION_SEED, "restored seed value");

    const quint32 copied = restored;
    check(replay(copied) == replay(ACTION_SEED), "copied seed replays exactly");
}

void testTruncatedSeedDoesNotMutate()
{
    quint32 restored = SENTINEL_SEED;
    QDataStream::Status status = QDataStream::Ok;
    check(!restoreSeed(QByteArray(), restored, status), "truncated seed is rejected");
    check(status != QDataStream::Ok, "truncated seed marks stream");
    checkEqual(restored, SENTINEL_SEED, "truncated seed preserves destination");
}

void testOwnedScopeUsesActionSeed()
{
    SeedContext context;
    {
        GameActionSeed::Scope scope(context, ACTION_SEED);
        check(context.isActive(), "owned seed scope activates context");
        checkEqual(context.lastSeed(), ACTION_SEED, "owned seed scope uses action seed");
    }
    check(!context.isActive(), "owned seed scope deactivates context");
}

void testNestedScopePreservesOuterSeed()
{
    SeedContext actual;
    SeedContext expected;
    actual.activate(OUTER_SEED);
    expected.activate(OUTER_SEED);

    {
        GameActionSeed::Scope scope(actual, ACTION_SEED);
        checkEqual(actual.next(), expected.next(), "nested scope keeps outer random stream");
        checkEqual(actual.lastSeed(), OUTER_SEED, "nested scope does not replace outer seed");
    }

    check(actual.isActive(), "nested scope preserves active state");
    checkEqual(actual.activationCount(), 1, "nested scope does not reactivate context");
    checkEqual(actual.deactivationCount(), 0, "nested scope does not deactivate context");
}
}

int main()
{
    testSeedRoundTripAndCopy();
    testTruncatedSeedDoesNotMutate();
    testOwnedScopeUsesActionSeed();
    testNestedScopePreservesOuterSeed();
    return failureCount == 0 ? 0 : 1;
}
