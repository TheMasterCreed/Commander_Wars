#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>
#include <QtGlobal>

enum class AiRandomAlgorithm : qint32
{
    V1 = 1,
    V2 = 2,
};

class AiRandom
{
public:
    AiRandom() = default;
    ~AiRandom() = default;

    void reseed(quint32 seed);
    void restore(quint32 seed, quint32 drawCounter);
    qint32 bounded(qint32 low, qint32 high);
    quint32 getSeed() const;
    quint32 getDrawCounter() const;
    static quint32 deriveSeed(const QString & seedNamespace,
                              QDataStream::Version streamVersion,
                              qint32 algorithmVersion,
                              qint32 playerId,
                              qint32 currentDay,
                              qint32 generation,
                              const QByteArray & mapHash);

private:
    quint32 nextValue();

    static constexpr quint32 COUNTER_MULTIPLIER = 1831565813;
    static constexpr quint32 LEFT_SHIFT_A = 13;
    static constexpr quint32 RIGHT_SHIFT = 17;
    static constexpr quint32 LEFT_SHIFT_B = 5;
    static constexpr quint32 FINALIZER_SHIFT_A = 15;
    static constexpr quint32 FINALIZER_SHIFT_B = 7;
    static constexpr quint32 FINALIZER_SHIFT_C = 14;
    static constexpr quint32 FINALIZER_ODD_MASK = 61;

    quint32 m_seed{0};
    quint32 m_drawCounter{0};
};
