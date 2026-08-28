#include "ai/airandom.h"

#include <QCryptographicHash>
#include <QIODevice>

void AiRandom::reseed(quint32 seed)
{
    m_seed = seed;
    m_drawCounter = 0;
}

void AiRandom::restore(quint32 seed, quint32 drawCounter)
{
    m_seed = seed;
    m_drawCounter = drawCounter;
}

qint32 AiRandom::bounded(qint32 low, qint32 high)
{
    if (high <= low)
    {
        return low;
    }
    const qint64 span = static_cast<qint64>(high) - static_cast<qint64>(low) + 1;
    const qint64 value = static_cast<qint64>(nextValue());
    return static_cast<qint32>(static_cast<qint64>(low) + value % span);
}

quint32 AiRandom::getSeed() const
{
    return m_seed;
}

quint32 AiRandom::getDrawCounter() const
{
    return m_drawCounter;
}

quint32 AiRandom::deriveSeed(const QString & seedNamespace,
                             QDataStream::Version streamVersion,
                             qint32 algorithmVersion,
                             qint32 playerId,
                             qint32 currentDay,
                             qint32 generation,
                             const QByteArray & mapHash)
{
    QByteArray seedData;
    QDataStream seedStream(&seedData, QIODevice::WriteOnly);
    seedStream.setVersion(streamVersion);
    seedStream << seedNamespace;
    seedStream << algorithmVersion;
    seedStream << playerId;
    seedStream << currentDay;
    seedStream << generation;
    seedStream << mapHash;
    const QByteArray hash = QCryptographicHash::hash(seedData, QCryptographicHash::Sha256);
    QDataStream hashStream(hash);
    hashStream.setVersion(streamVersion);
    quint32 seed = 0;
    hashStream >> seed;
    return seed;
}

quint32 AiRandom::nextValue()
{
    quint32 value = m_seed + COUNTER_MULTIPLIER * (m_drawCounter + 1);
    value ^= value << LEFT_SHIFT_A;
    value ^= value >> RIGHT_SHIFT;
    value ^= value << LEFT_SHIFT_B;
    value = (value ^ (value >> FINALIZER_SHIFT_A)) * (value | 1u);
    value ^= value + (value ^ (value >> FINALIZER_SHIFT_B)) * (value | FINALIZER_ODD_MASK);
    ++m_drawCounter;
    return value ^ (value >> FINALIZER_SHIFT_C);
}
