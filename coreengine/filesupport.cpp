#include "coreengine/filesupport.h"
#include "coreengine/settings.h"
#include "coreengine/gameconsole.h"

#include <QDirIterator>
#include <QCoreApplication>
#include <QSet>

const char* const Filesupport::LIST_FILENAME_ENDING = ".bl";

QByteArray Filesupport::getHash(const QStringList & filter, const QStringList & folders)
{
    QCryptographicHash myHash(QCryptographicHash::Sha512);
    QStringList fullList;

    QString userPath = Settings::getInstance()->getUserPath();
    for (const auto & folder : std::as_const(folders))
    {
        fullList.append(oxygine::Resource::RCC_PREFIX_PATH + folder);
        fullList.append(userPath + folder);
        if (!userPath.isEmpty())
        {
            fullList.append(folder);
        }
    }
    for (const auto & folder : std::as_const(fullList))
    {
        CONSOLE_PRINT_MODULE("Adding files for folder: " + folder, GameConsole::eDEBUG, GameConsole::eFileSupport);
        addHash(myHash, folder, filter);
    }
    return myHash.result();
}

void Filesupport::addHash(QCryptographicHash & hash, const QString & folder, const QStringList & filter)
{
    QDir dir(folder);
    auto list = dir.entryInfoList(filter, QDir::Files);
    for (auto & item : list)
    {
        QString filePath = item.filePath();
        CONSOLE_PRINT_MODULE("Adding file: " + filePath + " to hash", GameConsole::eDEBUG, GameConsole::eFileSupport);
        QFile file(filePath);
        file.open(QIODevice::ReadOnly);
        while (!file.atEnd())
        {
            hash.addData(file.readLine().trimmed());
        }
        file.close();
    }
    list = dir.entryInfoList(QStringList(), QDir::Dirs | QDir::NoDotAndDotDot);
    for (auto & item : list)
    {
        QString path = item.filePath();
        addHash(hash, path, filter);
    }
}

QByteArray Filesupport::getLegacyRuntimeHash(const QStringList & mods)
{
    QStringList folders = mods;
    folders.append("resources/scripts");
    folders.append("resources/aidata");
    QStringList filter = {"*.js", "*.csv"};
    return getHash(filter, folders);
}

QByteArray Filesupport::hashSingleFolder(const QString & folder, const QStringList & filter)
{
    return getHash(filter, QStringList{folder});
}

QMap<QString, QByteArray> Filesupport::getPerModHashes(const QStringList & mods)
{
    const QStringList filter = {"*.js", "*.csv"};
    QMap<QString, QByteArray> result;
    for (const auto & mod : std::as_const(mods))
    {
        result.insert(mod, hashSingleFolder(mod, filter));
    }
    return result;
}

QMap<QString, QByteArray> Filesupport::getResourceFolderHashes()
{
    const QStringList filter = {"*.js", "*.csv"};
    QMap<QString, QByteArray> result;
    result.insert(QStringLiteral("resources/scripts"), hashSingleFolder(QStringLiteral("resources/scripts"), filter));
    result.insert(QStringLiteral("resources/aidata"), hashSingleFolder(QStringLiteral("resources/aidata"), filter));
    return result;
}

bool Filesupport::validateModPath(const QString & modPath, qint32 maxLen)
{
    if (modPath.isEmpty() || modPath.size() > maxLen)
    {
        return false;
    }
    if (!modPath.startsWith(QStringLiteral("mods/")))
    {
        return false;
    }
    if (modPath.contains(QChar('\\')))
    {
        return false;
    }
    const QStringList segments = modPath.split(QChar('/'));
    if (segments.size() != 2)
    {
        return false;
    }
    static const QSet<QChar> kInvalidChars = {
        QChar('<'), QChar('>'), QChar(':'), QChar('"'),
        QChar('|'), QChar('?'), QChar('*'),
    };
    for (const auto & segment : segments)
    {
        if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral(".."))
        {
            return false;
        }
        // Windows strips trailing dots and spaces at create time; rejecting them avoids name collisions.
        if (segment.endsWith(QChar('.')) || segment.endsWith(QChar(' ')))
        {
            return false;
        }
        for (const QChar c : segment)
        {
            if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            {
                return false;
            }
            if (kInvalidChars.contains(c))
            {
                return false;
            }
        }
    }
    static const QSet<QString> kReservedNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
        QStringLiteral("COM0"), QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"),
        QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT0"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    const QString & nameSegment = segments[1];
    const qint32 dotIdx = nameSegment.indexOf(QChar('.'));
    const QString basename = (dotIdx >= 0) ? nameSegment.left(dotIdx) : nameSegment;
    if (kReservedNames.contains(basename.toUpper()))
    {
        return false;
    }
    return true;
}

void Filesupport::writeByteArray(QDataStream& stream, const QByteArray& array)
{
    stream << static_cast<qint32>(array.size());
    for (qint32 i = 0; i < array.size(); i++)
    {
        stream << static_cast<qint8>(array[i]);
    }
}

void Filesupport::writeBytes(QDataStream& stream, const QByteArray& array)
{
    for (qint32 i = 0; i < array.size(); i++)
    {
        stream << static_cast<qint8>(array[i]);
    }
}

QByteArray Filesupport::readByteArray(QDataStream& stream)
{
    QByteArray array;
    qint32 size = 0;
    stream >> size;
    for (qint32 i = 0; i < size; i++)
    {
        qint8 value = 0;
        stream >> value;
        array.append(value);
    }
    return array;
}

void Filesupport::storeList(const QString & file, const QStringList & items, const QString & folder)
{
    QDir dir(folder);
    dir.mkpath(".");
    QFile dataFile(folder + file + LIST_FILENAME_ENDING);
    dataFile.open(QIODevice::WriteOnly);
    QDataStream stream(&dataFile);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    stream << file;
    stream << static_cast<qint32>(items.size());
    for (qint32 i = 0; i < items.size(); i++)
    {
        stream << items[i];
    }
}

Filesupport::StringList Filesupport::readList(const QString & file, const QString & folder)
{
    return readList(folder + file);
}

Filesupport::StringList Filesupport::readList(const QString & file)
{
    QFile dataFile(file);
    StringList ret;
    if (dataFile.exists())
    {
        dataFile.open(QIODevice::ReadOnly);
        QDataStream stream(&dataFile);
        stream.setVersion(QDataStream::Version::Qt_6_5);
        stream >> ret.name;
        qint32 size = 0;
        stream >> size;
        for (qint32 i = 0; i < size; i++)
        {
            QString name;
            stream >> name;
            ret.items.append(name);
        }
    }
    else
    {
        CONSOLE_PRINT("Unable to open file: " + file + " using empty list", GameConsole::eWARNING);
    }
    return ret;
}
