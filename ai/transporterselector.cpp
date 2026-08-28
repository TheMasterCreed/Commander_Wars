#include "ai/transporterselector.h"

#include "coreengine/interpreter.h"
#include "coreengine/gameconsole.h"

#include "game/gamemap.h"

namespace
{
    // The loaded unit index lives in the menu action id, not in the cost list, which feeds transport fuel.
    qint32 loadedUnitIndex(const QStringList & actions, qint32 menuItem, qint32 loadedUnitCount)
    {
        bool ok = false;
        const qint32 index = actions[menuItem].toInt(&ok);
        return UnloadSelection::resolveLoadedIndex(ok, index, menuItem, loadedUnitCount);
    }

    // Marked field data is the raw list minus the tiles this action already took, in raw order, so both stay aligned.
    void removeUsedFields(QList<QVariant> & fields, const std::vector<QPoint> & usedFields)
    {
        for (qint32 i = fields.size() - 1; i >= 0; --i)
        {
            if (GlobalUtils::contains(usedFields, fields[i].toPoint()))
            {
                fields.removeAt(i);
            }
        }
    }
}

TransporterSelector::TransporterSelector(CoreAI & owner)
    : m_owner(owner)
{
}

void TransporterSelector::prepareUnloadInformation(spGameAction &pAction, Unit *pUnit, spQmlVectorUnit &pEnemyUnits)
{
    bool unloaded = false;
    std::vector<qint32> unloadedUnits;
    std::vector<QPoint> usedFields;
    do
    {
        unloaded = false;
        spMenuData pDataMenu = pAction->getMenuStepData();
        QVector<qint32> costs = pDataMenu->getCostList();
        if (pDataMenu->validData())
        {
            QStringList actions = pDataMenu->getActionIDs();
            std::vector<QList<QVariant>> unloadFields = getUnloadFields(pAction, pUnit, actions, usedFields);
            if (actions.size() > 1)
            {
                unloaded = fillUnloadFields(pAction, pUnit, unloadedUnits, unloadFields, costs, actions, usedFields);
                if (unloaded == false)
                {
                    unloaded = fallbackUnload(pAction, pUnit, pEnemyUnits, pDataMenu, actions, usedFields);
                }
            }
        }
        else
        {
            AI_CONSOLE_PRINT("Error invalid menu data received while unloading units", GameConsole::eERROR);
            break;
        }
    } while (unloaded);
    m_owner.addMenuItemData(pAction, CoreAI::ACTION_WAIT, 0);
}

std::vector<QList<QVariant>> TransporterSelector::getUnloadFields(spGameAction &pAction, Unit *pUnit, QStringList & actions,
                                                                  const std::vector<QPoint> & usedFields)
{
    Interpreter *pInterpreter = Interpreter::getInstance();
    std::vector<QList<QVariant>> unloadFields;
    const qint32 loadedUnitCount = pUnit->getLoadedUnitCount();
    // the trailing menu entry is the wait item and has no loaded unit behind it
    for (qint32 i = 0; i < actions.size() - 1; i++)
    {
        QString function1 = "getUnloadFields";
        QJSValueList args({
            JsThis::getJsThis(pAction.get()),
            loadedUnitIndex(actions, i, loadedUnitCount),
            GameMap::getMapJsThis(m_owner.getMap()),
        });
        QJSValue ret = pInterpreter->doFunction(CoreAI::ACTION_UNLOAD, function1, args);
        QList<QVariant> fields = ret.toVariant().toList();
        removeUsedFields(fields, usedFields);
        unloadFields.push_back(fields);
    }
    return unloadFields;
}

qint32 TransporterSelector::findEnemyBuildingField(const QList<QVariant> & fields)
{
    for (qint32 i = 0; i < fields.size(); ++i)
    {
        const QPoint unloadField = fields[i].toPoint();
        Terrain* pTerrain = m_owner.getMap()->getTerrain(unloadField.x(), unloadField.y());
        Building *pBuilding = pTerrain->getBuilding();
        if (pBuilding != nullptr && m_owner.getPlayer()->isEnemy(pBuilding->getOwner()))
        {
            return i;
        }
    }
    return UnloadSelection::NO_FIELD;
}

std::vector<UnloadSelection::Candidate> TransporterSelector::buildCandidates(Unit *pUnit, const std::vector<QList<QVariant>> & unloadFields,
                                                                             const QStringList & actions)
{
    std::vector<UnloadSelection::Candidate> candidates;
    candidates.reserve(unloadFields.size());
    const qint32 loadedUnitCount = pUnit->getLoadedUnitCount();
    for (qint32 i = 0; i < static_cast<qint32>(unloadFields.size()); ++i)
    {
        UnloadSelection::Candidate candidate;
        candidate.loadedIndex = loadedUnitIndex(actions, i, loadedUnitCount);
        Unit *pLoadedUnit = pUnit->getLoadedUnit(candidate.loadedIndex);
        // an entry without a loaded unit keeps its slot so choices stay menu aligned, and stays unselectable
        if (pLoadedUnit != nullptr)
        {
            candidate.needsRefuel = m_owner.needsRefuel(pLoadedUnit);
            candidate.canCapture = pLoadedUnit->getActionList().contains(CoreAI::ACTION_CAPTURE);
            candidate.fieldCount = static_cast<qint32>(unloadFields[i].size());
            if (candidate.canCapture && candidate.fieldCount > 0)
            {
                candidate.enemyBuildingField = findEnemyBuildingField(unloadFields[i]);
            }
        }
        candidates.push_back(candidate);
    }
    return candidates;
}

bool TransporterSelector::fillUnloadFields(spGameAction &pAction, Unit *pUnit, std::vector<qint32> & unloadedUnits,
                                           std::vector<QList<QVariant>> & unloadFields, QVector<qint32> & costs,
                                           QStringList & actions, std::vector<QPoint> & usedFields)
{
    const std::vector<UnloadSelection::Candidate> candidates = buildCandidates(pUnit, unloadFields, actions);
    const UnloadSelection::Choice choice = UnloadSelection::select(candidates, unloadedUnits);
    if (choice.source == UnloadSelection::FieldSource::None)
    {
        return false;
    }
    m_owner.addMenuItemData(pAction, actions[choice.candidate], costs[choice.candidate]);
    QPoint unloadField;
    if (choice.source == UnloadSelection::FieldSource::MarkedFieldData)
    {
        spMarkedFieldData pFields = pAction->getMarkedFieldStepData();
        unloadField = pFields->getPoints()->at(choice.field);
    }
    else
    {
        unloadField = unloadFields[choice.candidate][choice.field].toPoint();
    }
    m_owner.addSelectedFieldData(pAction, unloadField);
    usedFields.push_back(unloadField);
    unloadedUnits.push_back(candidates[choice.candidate].loadedIndex);
    return true;
}

bool TransporterSelector::fallbackUnload(spGameAction &pAction, Unit *pUnit, spQmlVectorUnit &pEnemyUnits, spMenuData & pDataMenu,
                                         QStringList & actions, std::vector<QPoint> & usedFields)
{
    bool unloaded = false;
    const qint32 loadedIndex = loadedUnitIndex(actions, 0, pUnit->getLoadedUnitCount());
    if (!m_owner.needsRefuel(pUnit->getLoadedUnit(loadedIndex)))
    {
        qint32 costs = pDataMenu->getCostList()[0];
        m_owner.addMenuItemData(pAction, actions[0], costs);
        unloaded = true;
        spMarkedFieldData pFields = pAction->getMarkedFieldStepData();
        qint32 field = 0;
        qint32 bestDistance = std::numeric_limits<qint32>::max();
        for (qint32 i = 0; i < pFields->getPoints()->size(); ++i)
        {
            QPoint unloadPos = pFields->getPoints()->at(i);
            qint32 currentBestDistance = std::numeric_limits<qint32>::max();
            for (auto &pEnemy : pEnemyUnits->getVector())
            {
                qint32 distance = GlobalUtils::getDistance(unloadPos, pEnemy->Unit::getPosition());
                if (distance < currentBestDistance)
                {
                    currentBestDistance = distance;
                }
            }
            if (currentBestDistance < bestDistance ||
                (currentBestDistance == bestDistance && m_owner.randomInt(0, 1) == 1))
            {
                bestDistance = currentBestDistance;
                field = i;
            }
        }
        const QPoint unloadField = pFields->getPoints()->at(field);
        m_owner.addSelectedFieldData(pAction, unloadField);
        usedFields.push_back(unloadField);
    }
    return unloaded;
}
