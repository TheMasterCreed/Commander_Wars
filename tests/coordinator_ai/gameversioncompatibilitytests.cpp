#include "coreengine/gameversion.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>

#include <iostream>
#include <string_view>

namespace
{
constexpr qint32 EXPECTED_MAJOR = 0;
constexpr qint32 EXPECTED_MINOR = 39;
constexpr qint32 PREVIOUS_REVISION = 0;
constexpr qint32 CURRENT_REVISION = 1;

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
        std::cerr << message << '\n';
        ++failureCount;
    }
}

GameVersion currentVersion()
{
    return GameVersion(EXPECTED_MAJOR,
                       EXPECTED_MINOR,
                       CURRENT_REVISION,
                       QStringLiteral("main"));
}

GameVersion previousVersion()
{
    return GameVersion(EXPECTED_MAJOR,
                       EXPECTED_MINOR,
                       PREVIOUS_REVISION,
                       QStringLiteral("main"));
}

GameVersion roundTrip(const GameVersion & source, QDataStream::Status & writeStatus, QDataStream::Status & readStatus)
{
    QByteArray data;
    QDataStream writer(&data, QIODevice::WriteOnly);
    writer.setVersion(QDataStream::Qt_6_5);
    source.serializeObject(writer);
    writeStatus = writer.status();

    GameVersion decoded;
    QDataStream reader(data);
    reader.setVersion(QDataStream::Qt_6_5);
    decoded.deserializeObject(reader);
    readStatus = reader.status();
    return decoded;
}

void testCompiledVersion()
{
    const GameVersion compiled;
    checkEqual(compiled.getMajor(), EXPECTED_MAJOR, "compiled major version");
    checkEqual(compiled.getMinor(), EXPECTED_MINOR, "compiled minor version");
    checkEqual(compiled.getRevision(), CURRENT_REVISION, "compiled revision");
    checkEqual(compiled.getSufix(), QStringLiteral("main"), "compiled suffix");
    checkEqual(compiled.toString(), QStringLiteral("0.39.1-main"), "compiled version string");
    check(compiled == currentVersion(), "compiled version equals explicit current version");
}

void testExactPeerCompatibility()
{
    const GameVersion current = currentVersion();
    const GameVersion previous = previousVersion();

    check(current == currentVersion(), "current peer is accepted");
    check(!(current != currentVersion()), "current peer inequality is false");
    check(previous != current, "previous peer is rejected");
    check(!(previous == current), "previous peer equality is false");

    check(GameVersion(1, EXPECTED_MINOR, CURRENT_REVISION, QStringLiteral("main")) != current,
          "different major version is rejected");
    check(GameVersion(EXPECTED_MAJOR, 40, CURRENT_REVISION, QStringLiteral("main")) != current,
          "different minor version is rejected");
    check(GameVersion(EXPECTED_MAJOR, EXPECTED_MINOR, 2, QStringLiteral("main")) != current,
          "different revision is rejected");
    check(GameVersion(EXPECTED_MAJOR, EXPECTED_MINOR, CURRENT_REVISION, QStringLiteral("beta")) != current,
          "different suffix is rejected");
}

void testSerializedCompatibility()
{
    QDataStream::Status writeStatus = QDataStream::ReadCorruptData;
    QDataStream::Status readStatus = QDataStream::ReadCorruptData;
    const GameVersion decodedCurrent = roundTrip(currentVersion(), writeStatus, readStatus);
    checkEqual(writeStatus, QDataStream::Ok, "current version write status");
    checkEqual(readStatus, QDataStream::Ok, "current version read status");
    check(decodedCurrent == currentVersion(), "current version round trip");

    const GameVersion decodedPrevious = roundTrip(previousVersion(), writeStatus, readStatus);
    checkEqual(writeStatus, QDataStream::Ok, "previous version write status");
    checkEqual(readStatus, QDataStream::Ok, "previous version read status");
    check(decodedPrevious == previousVersion(), "previous version round trip");
    check(decodedPrevious != currentVersion(), "serialized previous peer remains incompatible");
}

void testAndroidVersionCoherence()
{
    const QString androidVersionName = QString::fromLatin1(COW_ANDROID_VERSION_NAME);
    const QString androidVersionCode = QString::fromLatin1(COW_ANDROID_VERSION_CODE);

    checkEqual(androidVersionName, currentVersion().toString(), "Android and game version names match");
    checkEqual(androidVersionName, QStringLiteral("0.39.1-main"), "Android version name");
    checkEqual(androidVersionCode, QStringLiteral("70"), "Android version code");
}
}

int main()
{
    testCompiledVersion();
    testExactPeerCompatibility();
    testSerializedCompatibility();
    testAndroidVersionCoherence();
    return failureCount == 0 ? 0 : 1;
}
