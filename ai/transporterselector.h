#pragma once

#include <QtGlobal>
#include "ai/coreai.h"
#include "ai/unloadselection.h"

class GameAction;
using spGameAction = std::shared_ptr<GameAction>;
class QmlVectorUnit;
using spQmlVectorUnit = std::shared_ptr<QmlVectorUnit>;

class TransporterSelector
{
public:
    TransporterSelector(CoreAI & owner);

    void prepareUnloadInformation(spGameAction &pAction, Unit *pUnit, spQmlVectorUnit &pEnemyUnits);
private:
    bool fillUnloadFields(spGameAction &pAction, Unit *pUnit, std::vector<qint32> & unloadedUnits,
                          std::vector<QList<QVariant>> & unloadFields, QVector<qint32> & costs, QStringList & actions,
                          std::vector<QPoint> & usedFields);
    bool fallbackUnload(spGameAction &pAction, Unit *pUnit, spQmlVectorUnit &pEnemyUnits, spMenuData & pDataMenu,
                        QStringList & actions, std::vector<QPoint> & usedFields);
    std::vector<QList<QVariant>> getUnloadFields(spGameAction &pAction, Unit *pUnit, QStringList & actions,
                                                 const std::vector<QPoint> & usedFields);
    std::vector<UnloadSelection::Candidate> buildCandidates(Unit *pUnit, const std::vector<QList<QVariant>> & unloadFields,
                                                            const QStringList & actions);
    qint32 findEnemyBuildingField(const QList<QVariant> & fields);
private:
    CoreAI & m_owner;
};
