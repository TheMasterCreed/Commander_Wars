#pragma once

#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>

#include "game/gameaction.h"

class GameMap;

class ArenaActionPrefix
{
public:
    bool load(const QString & path, QString & error);
    bool apply(spGameAction & action, GameMap & map, QString & error);
    bool observeBoundary(GameMap & map);
    quint32 initialSeed() const;
    bool complete() const;
    QVariantMap result() const;

private:
    QJsonArray m_actions;
    QJsonArray m_friendlyPaths;
    QVariantList m_receipts;
    QVariantMap m_boundary;
    qsizetype m_next{1};
    quint32 m_initialSeed{0};
};
