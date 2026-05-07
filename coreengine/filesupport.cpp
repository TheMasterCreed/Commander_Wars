#include "coreengine/filesupport.h"
#include "coreengine/settings.h"
#include "coreengine/gameconsole.h"

#include <QDirIterator>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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

namespace
{
    constexpr qint32 kModSyncDisabled = 1;
    constexpr qint32 kModSyncUnknownMod = 2;
    constexpr qint32 kModSyncSizeCapExceeded = 3;
    constexpr qint32 kModSyncFileCountCapExceeded = 4;
    constexpr qint32 kModSyncInvalidPath = 5;
    constexpr qint32 kModSyncInternalError = 6;

    bool segmentClean(const QString & seg)
    {
        static const QSet<QChar> kInvalid = {
            QChar('<'), QChar('>'), QChar(':'), QChar('"'),
            QChar('|'), QChar('?'), QChar('*'),
        };
        if (seg.isEmpty() || seg == QStringLiteral(".") || seg == QStringLiteral(".."))
        {
            return false;
        }
        if (seg.endsWith(QChar('.')) || seg.endsWith(QChar(' ')))
        {
            return false;
        }
        for (const QChar c : seg)
        {
            if (c.unicode() < 0x20 || c.unicode() == 0x7F)
            {
                return false;
            }
            if (kInvalid.contains(c))
            {
                return false;
            }
        }
        static const QSet<QString> kReserved = {
            QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
            QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
            QStringLiteral("COM0"), QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
            QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"),
            QStringLiteral("COM8"), QStringLiteral("COM9"),
            QStringLiteral("LPT0"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
            QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
            QStringLiteral("LPT8"), QStringLiteral("LPT9"),
        };
        const qint32 dotIdx = seg.indexOf(QChar('.'));
        const QString basename = (dotIdx >= 0) ? seg.left(dotIdx) : seg;
        if (kReserved.contains(basename.toUpper()))
        {
            return false;
        }
        return true;
    }

    QString joinPath(const QString & a, const QString & b)
    {
        if (a.isEmpty())
        {
            return b;
        }
        if (a.endsWith(QChar('/')))
        {
            return a + b;
        }
        return a + QChar('/') + b;
    }
}

bool Filesupport::validateRelativeFilePath(const QString & relPath, qint32 maxLen)
{
    if (relPath.isEmpty() || relPath.size() > maxLen)
    {
        return false;
    }
    if (relPath.startsWith(QChar('/')) || relPath.contains(QChar('\\')))
    {
        return false;
    }
    if (relPath.size() >= 2 && relPath[1] == QChar(':'))
    {
        return false;
    }
    const QStringList segments = relPath.split(QChar('/'));
    for (const auto & seg : segments)
    {
        if (!segmentClean(seg))
        {
            return false;
        }
    }
    return true;
}

Filesupport::ModSyncPackage Filesupport::buildModSyncPackage(const QString & installRoot, const QString & modPath, const ModSyncCaps & caps)
{
    ModSyncPackage pkg;
    if (!validateModPath(modPath))
    {
        pkg.rejectReason = kModSyncInvalidPath;
        return pkg;
    }
    const QString modRoot = joinPath(installRoot, modPath);
    QDir modDir(modRoot);
    if (!modDir.exists())
    {
        pkg.rejectReason = kModSyncUnknownMod;
        return pkg;
    }
    QMap<QString, QByteArray> files;
    qint64 uncompressedTotal = 0;
    qint32 fileCount = 0;
    QDirIterator it(modRoot, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString absolute = it.next();
        const QString rel = QDir(modRoot).relativeFilePath(absolute);
        // Filter VCS metadata, build artefacts, and our own staging/backup dirs.
        if (rel.startsWith(QStringLiteral(".git/")) ||
            rel.startsWith(QStringLiteral(".svn/")) ||
            rel.startsWith(QStringLiteral("__pycache__/")) ||
            rel.contains(QStringLiteral(".sync-staging-")) ||
            rel.contains(QStringLiteral(".bak-")))
        {
            continue;
        }
        if (!validateRelativeFilePath(rel, caps.relPathMaxLen))
        {
            pkg.rejectReason = kModSyncInvalidPath;
            return pkg;
        }
        QFile f(absolute);
        if (!f.open(QIODevice::ReadOnly))
        {
            pkg.rejectReason = kModSyncInternalError;
            return pkg;
        }
        const qint64 size = f.size();
        if (size > caps.perModBytes)
        {
            pkg.rejectReason = kModSyncSizeCapExceeded;
            return pkg;
        }
        uncompressedTotal += size;
        if (uncompressedTotal > caps.perModBytes)
        {
            pkg.rejectReason = kModSyncSizeCapExceeded;
            return pkg;
        }
        ++fileCount;
        if (fileCount > caps.fileCountMax)
        {
            pkg.rejectReason = kModSyncFileCountCapExceeded;
            return pkg;
        }
        files.insert(rel, f.readAll());
        f.close();
    }
    QByteArray serialized;
    {
        QDataStream stream(&serialized, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Version::Qt_6_5);
        writeMap(stream, files);
    }
    pkg.declaredUncompressedSize = static_cast<qint32>(serialized.size());
    pkg.compressedBlob = qCompress(serialized);
    pkg.fileCount = fileCount;
    if (pkg.compressedBlob.size() > caps.perModBytes)
    {
        pkg.rejectReason = kModSyncSizeCapExceeded;
        pkg.compressedBlob.clear();
        return pkg;
    }
    return pkg;
}

QMap<QString, QByteArray> Filesupport::extractModSyncPackage(const QByteArray & compressedBlob, qint32 declaredUncompressedSize, const ModSyncCaps & caps, qint32 & rejectReason)
{
    rejectReason = 0;
    QMap<QString, QByteArray> files;
    if (compressedBlob.size() > caps.perModBytes)
    {
        rejectReason = kModSyncSizeCapExceeded;
        return files;
    }
    if (declaredUncompressedSize <= 0 || declaredUncompressedSize > caps.perModBytes)
    {
        rejectReason = kModSyncSizeCapExceeded;
        return files;
    }
    const QByteArray serialized = qUncompress(compressedBlob);
    if (serialized.isEmpty() || serialized.size() != declaredUncompressedSize)
    {
        rejectReason = kModSyncInternalError;
        return files;
    }
    QByteArray mut = serialized;
    QDataStream stream(&mut, QIODevice::ReadOnly);
    stream.setVersion(QDataStream::Version::Qt_6_5);
    auto map = readMap<QString, QByteArray, QMap>(stream);
    if (stream.status() != QDataStream::Ok)
    {
        rejectReason = kModSyncInternalError;
        return files;
    }
    qint32 fileCount = 0;
    qint64 uncompressedTotal = 0;
    for (auto iter = map.constBegin(); iter != map.constEnd(); ++iter)
    {
        if (!validateRelativeFilePath(iter.key(), caps.relPathMaxLen))
        {
            rejectReason = kModSyncInvalidPath;
            files.clear();
            return files;
        }
        if (iter.value().size() > caps.perModBytes)
        {
            rejectReason = kModSyncSizeCapExceeded;
            files.clear();
            return files;
        }
        uncompressedTotal += iter.value().size();
        if (uncompressedTotal > caps.perModBytes)
        {
            rejectReason = kModSyncSizeCapExceeded;
            files.clear();
            return files;
        }
        ++fileCount;
        if (fileCount > caps.fileCountMax)
        {
            rejectReason = kModSyncFileCountCapExceeded;
            files.clear();
            return files;
        }
    }
    files = map;
    return files;
}

QString Filesupport::stageModSync(const QString & installRoot, const QString & modPath, const QMap<QString, QByteArray> & files, qint32 & rejectReason)
{
    rejectReason = 0;
    if (!validateModPath(modPath))
    {
        rejectReason = kModSyncInvalidPath;
        return QString();
    }
    const qint64 pid = QCoreApplication::applicationPid();
    const QString stagingPath = joinPath(installRoot, modPath) + QStringLiteral(".sync-staging-") + QString::number(pid);
    QDir stagingDir(stagingPath);
    if (stagingDir.exists())
    {
        stagingDir.removeRecursively();
    }
    if (!QDir().mkpath(stagingPath))
    {
        rejectReason = kModSyncInternalError;
        return QString();
    }
    for (auto iter = files.constBegin(); iter != files.constEnd(); ++iter)
    {
        const QString full = joinPath(stagingPath, iter.key());
        const QFileInfo fi(full);
        if (!QDir().mkpath(fi.absolutePath()))
        {
            QDir(stagingPath).removeRecursively();
            rejectReason = kModSyncInternalError;
            return QString();
        }
        QSaveFile f(full);
        if (!f.open(QIODevice::WriteOnly))
        {
            QDir(stagingPath).removeRecursively();
            rejectReason = kModSyncInternalError;
            return QString();
        }
        f.write(iter.value());
        if (!f.commit())
        {
            QDir(stagingPath).removeRecursively();
            rejectReason = kModSyncInternalError;
            return QString();
        }
    }
    return stagingPath;
}

void Filesupport::reapModSyncFolders(const QString & installRoot, qint32 backupKeep)
{
    const QString modsRoot = joinPath(installRoot, QStringLiteral("mods"));
    QDir modsDir(modsRoot);
    if (!modsDir.exists())
    {
        return;
    }
    const auto entries = modsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    QMap<QString, QList<QFileInfo>> backupsByMod;
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-3600);
    for (const auto & entry : entries)
    {
        const QString name = entry.fileName();
        const qint32 stagingIdx = name.indexOf(QStringLiteral(".sync-staging-"));
        if (stagingIdx > 0)
        {
            // Mtime fallback heuristic; cheap, no platform-specific PID liveness check.
            if (entry.lastModified() < cutoff)
            {
                CONSOLE_PRINT("Reaping stale staging dir: " + entry.absoluteFilePath(), GameConsole::eINFO);
                QDir(entry.absoluteFilePath()).removeRecursively();
            }
            continue;
        }
        const qint32 bakIdx = name.indexOf(QStringLiteral(".bak-"));
        if (bakIdx > 0)
        {
            backupsByMod[name.left(bakIdx)].append(entry);
        }
    }
    for (auto iter = backupsByMod.begin(); iter != backupsByMod.end(); ++iter)
    {
        auto & list = iter.value();
        if (list.size() <= backupKeep)
        {
            continue;
        }
        std::sort(list.begin(), list.end(), [](const QFileInfo & a, const QFileInfo & b)
        {
            return a.lastModified() > b.lastModified();
        });
        for (qint32 i = backupKeep; i < list.size(); ++i)
        {
            CONSOLE_PRINT("Pruning old mod-sync backup: " + list[i].absoluteFilePath(), GameConsole::eINFO);
            QDir(list[i].absoluteFilePath()).removeRecursively();
        }
    }
}

QString Filesupport::pendingModSyncManifestPath(const QString & userDataPath)
{
    return joinPath(userDataPath, QStringLiteral(".pending-mod-sync.json"));
}

bool Filesupport::writePendingModSyncManifest(const QString & userDataPath, const QList<QPair<QString, QString>> & swaps)
{
    QJsonArray jsonSwaps;
    for (const auto & pair : swaps)
    {
        QJsonObject entry;
        entry.insert(QStringLiteral("staging"), pair.first);
        entry.insert(QStringLiteral("final"), pair.second);
        jsonSwaps.append(entry);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("swaps"), jsonSwaps);
    const QString path = pendingModSyncManifestPath(userDataPath);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        CONSOLE_PRINT("Failed to open pending mod-sync manifest for write: " + path, GameConsole::eERROR);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!f.commit())
    {
        CONSOLE_PRINT("Failed to commit pending mod-sync manifest: " + path, GameConsole::eERROR);
        return false;
    }
    return true;
}

void Filesupport::executePendingModSyncManifest(const QString & installRoot, const QString & userDataPath)
{
    const QString path = pendingModSyncManifestPath(userDataPath);
    QFile f(path);
    if (!f.exists())
    {
        return;
    }
    if (!f.open(QIODevice::ReadOnly))
    {
        CONSOLE_PRINT("Pending mod-sync manifest exists but cannot be read: " + path, GameConsole::eERROR);
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
    {
        CONSOLE_PRINT("Pending mod-sync manifest is invalid JSON: " + parseErr.errorString(), GameConsole::eERROR);
        QFile::remove(path);
        return;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(0) != 1)
    {
        CONSOLE_PRINT("Pending mod-sync manifest has unknown version, discarding", GameConsole::eERROR);
        QFile::remove(path);
        return;
    }
    const QJsonArray swaps = root.value(QStringLiteral("swaps")).toArray();
    const QString isoStamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    for (const auto & v : swaps)
    {
        const QJsonObject entry = v.toObject();
        const QString stagingRel = entry.value(QStringLiteral("staging")).toString();
        const QString finalRel = entry.value(QStringLiteral("final")).toString();
        if (!validateModPath(finalRel))
        {
            CONSOLE_PRINT("Manifest entry has invalid final path, skipping: " + finalRel, GameConsole::eERROR);
            continue;
        }
        const QString stagingAbs = joinPath(installRoot, stagingRel);
        const QString finalAbs = joinPath(installRoot, finalRel);
        if (!QFileInfo::exists(stagingAbs))
        {
            CONSOLE_PRINT("Manifest staging missing, skipping: " + stagingAbs, GameConsole::eERROR);
            continue;
        }
        if (QFileInfo::exists(finalAbs))
        {
            const QString backupAbs = finalAbs + QStringLiteral(".bak-") + isoStamp;
            if (!QDir().rename(finalAbs, backupAbs))
            {
                CONSOLE_PRINT("Failed to back up existing mod folder: " + finalAbs + " -> " + backupAbs, GameConsole::eERROR);
                continue;
            }
        }
        if (!QDir().rename(stagingAbs, finalAbs))
        {
            CONSOLE_PRINT("Failed to swap staging into place: " + stagingAbs + " -> " + finalAbs, GameConsole::eERROR);
            continue;
        }
        CONSOLE_PRINT("Mod sync applied: " + finalRel, GameConsole::eINFO);
    }
    QFile::remove(path);
}
