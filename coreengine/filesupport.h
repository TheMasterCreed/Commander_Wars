#ifndef FILESUPPORT_H
#define FILESUPPORT_H

#include <QFile>
#include <QObject>
#include <QDataStream>
#include <QCryptographicHash>
#include <QMap>

class Filesupport final
{
public:
    struct StringList
    {
        QString name;
        QStringList items;
    };
    static const char* const LIST_FILENAME_ENDING;
    static constexpr qint32 LegacyRuntimeHashSize = 64;
    // Old clients read this field as a QByteArray length, so versions must stay small and never collide with 64.
    static constexpr qint32 CurrentHashPayloadVersion = 1;
    Filesupport() = delete;
    ~Filesupport() = delete;
    static QByteArray getLegacyRuntimeHash(const QStringList & mods);
    static QMap<QString, QByteArray> getPerModHashes(const QStringList & mods);
    static QMap<QString, QByteArray> getResourceFolderHashes();
    static QByteArray hashSingleFolder(const QString & folder, const QStringList & filter);
    static QByteArray getHash(const QStringList & filter, const QStringList & folders);
    static void addHash(QCryptographicHash & hash, const QString & folder, const QStringList & filter);
    /**
     * @brief writeByteArray
     * @param stream
     * @param array
     */
    static void writeByteArray(QDataStream& stream, const QByteArray& array);
    /**
     * @brief writeBytes
     * @param stream
     * @param array
     */
    static void writeBytes(QDataStream& stream, const QByteArray& array);
    /**
     * @brief readByteArray
     * @param stream
     * @param array
     */
    static QByteArray readByteArray(QDataStream& stream);

    template<typename _TVectorList>
    static void writeVectorList(QDataStream& stream, const _TVectorList & array)
    {
        stream << static_cast<qint32>(array.size());
        for (qint32 i = 0; i < array.size(); i++)
        {
            stream << array[i];
        }
    }

    template<typename _TType, template<typename T> class _TVectorList>
    static _TVectorList<_TType> readVectorList(QDataStream& stream)
    {
        _TVectorList<_TType> array;
        qint32 size = 0;
        stream >> size;
        for (qint32 i = 0; i < size; i++)
        {
            _TType value = 0;
            stream >> value;
            array.append(value);
        }
        return array;
    }
    /**
     * @brief storeList
     * @param file
     * @param items
     * @param folder
     */
    static void storeList(const QString & file, const QStringList  &items, const QString & folder);
    /**
     * @brief readList
     * @param file
     * @return
     */
    static StringList readList(const QString & file);
    /**
     * @brief readList
     * @param file
     * @param folder
     * @return
     */
    static StringList readList(const QString & file, const QString & folder);
    /**
     *
     */
    template<typename _TMap>
    static void writeMap(QDataStream& stream, const _TMap & map)
    {
        stream << static_cast<qint32>(map.size());
        auto iter = map.constBegin();
        while (iter != map.constEnd())
        {
            stream << iter.key();
            stream << iter.value();
            ++iter;
        }
    }
    /**
     *
     */
    template<typename _TKey, typename _TType, template<typename _T1, typename _T2> class _TMap>
    static _TMap<_TKey, _TType> readMap(QDataStream& stream)
    {
        _TMap<_TKey, _TType> map;
        qint32 size = 0;
        stream >> size;
        for (qint32 i = 0; i < size; i++)
        {
            _TKey key;
            _TType value;
            stream >> key;
            stream >> value;
            map.insert(key, value);
        }
        return map;
    }
};

#endif // FILESUPPORT_H
