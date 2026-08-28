#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "game/GameEnums.h"

struct ArenaMatchPlayerConfig
{
    GameEnums::AiTypes aiType{GameEnums::AiTypes_Normal};
    QString aiTypeName;
    QString coId;
};

struct ArenaMatchConfig
{
    static const QString FILE_NAME;

    static bool load(const QString & path, ArenaMatchConfig & config, QString & error);
    static bool load(const QString & path, ArenaMatchConfig & config, QString & error,
                     const QStringList & loadedHeavyAiIds);

    bool validatePlayerCount(qint32 playerCount, QString & error) const;
    bool hasStopBoundary() const;

    QString mapPath;
    QString resultFileName;
    QVector<ArenaMatchPlayerConfig> players;
    GameEnums::AiBehavior aiBehaviorMode{GameEnums::AiBehavior_Standard};
    quint32 masterSeed{0};
    qint32 turnLimit{0};
    qint32 watchdogMs{0};
    qint32 stopAfterDay{-1};
    qint32 stopAfterPlayer{-1};
    bool deterministicCounterpointSeed{false};
};

class ArenaStopBoundary
{
public:
    void configure(const ArenaMatchConfig & config);
    void observeAction(qint32 day, qint32 player);
    bool shouldStop(qint32 day, qint32 player) const;

private:
    qint32 m_day{-1};
    qint32 m_player{-1};
    bool m_actionSeen{false};
};
