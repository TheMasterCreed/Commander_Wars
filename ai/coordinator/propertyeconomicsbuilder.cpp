#include "ai/coordinator/propertyeconomicsbuilder.h"

#include <utility>

#include <QHash>

#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coreai.h"

#include "coreengine/gameconsole.h"

#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"

namespace
{
    Coordinator::ObjectiveKind buildingObjective(Building & building)
    {
        if (building.isHq())
        {
            return Coordinator::ObjectiveKind::Headquarters;
        }
        return Coordinator::ObjectiveKind::None;
    }

    qint32 repairTypeMask(Building & building)
    {
        qint32 mask = 0;
        for (const qint32 repairType : building.getRepairTypes())
        {
            mask |= repairType;
        }
        return mask;
    }

    // Script answers that depend on the building id alone; the action list does not, so production stays per building.
    struct BuildingIdFacts
    {
        qint32 baseIncome{0};
        qint32 repairTypeMask{0};
        Coordinator::ObjectiveKind objective{Coordinator::ObjectiveKind::None};
        bool capturable{false};
    };

    BuildingIdFacts buildingIdFacts(QHash<QString, BuildingIdFacts> & knownFacts, const QString & buildingId,
                                    Building & building)
    {
        const auto known = knownFacts.constFind(buildingId);
        if (known != knownFacts.constEnd())
        {
            return known.value();
        }
        BuildingIdFacts facts;
        facts.baseIncome = static_cast<qint32>(building.getBaseIncome());
        facts.repairTypeMask = repairTypeMask(building);
        facts.objective = buildingObjective(building);
        facts.capturable = building.isCaptureBuilding();
        knownFacts.insert(buildingId, facts);
        return facts;
    }

    qint32 coveredTileCount(Building & building)
    {
        return building.getBuildingWidth() * building.getBuildingHeigth();
    }

    // Player::calcIncome pays a multi tile building once per covered tile, so the fact matches what is paid.
    qint32 incomePerTurn(Building & building)
    {
        if (building.getOwner() == nullptr)
        {
            return 0;
        }
        return building.getIncome() * coveredTileCount(building);
    }

    // A neutral building runs no owner filter, so the observer who would capture it stands in for one.
    QStringList neutralProductionList(GameMap & map, const Coordinator::BattlefieldKnowledge & knowledge, Building & building)
    {
        Player* pObserver = map.getPlayer(knowledge.observerId());
        if (pObserver == nullptr)
        {
            AI_CONSOLE_PRINT("Coordinator::buildPropertyEconomics() no observer " +
                             QString::number(knowledge.observerId()), GameConsole::eDEBUG);
            return QStringList();
        }
        // The CO specific unit rules stay unapplied here, unlike the owned branch of getConstructionList.
        const QStringList observerBuildList = pObserver->getBuildList();
        QStringList productionList;
        for (const QString & unitId : building.getConstructionList())
        {
            if (observerBuildList.contains(unitId))
            {
                productionList.append(unitId);
            }
        }
        return productionList;
    }

    Building* buildingAt(GameMap & map, qint32 x, qint32 y)
    {
        Terrain* pTerrain = map.getTerrain(x, y);
        if (pTerrain == nullptr)
        {
            return nullptr;
        }
        return pTerrain->getBuilding();
    }

    void addCaptureProgress(Coordinator::PropertyFacts & facts, const Coordinator::BattlefieldKnowledge & knowledge)
    {
        const Coordinator::KnownUnit* pKnown = knowledge.unitAt(facts.x, facts.y);
        if (pKnown == nullptr || pKnown->capturePoints <= 0)
        {
            return;
        }
        facts.capturerIndex = static_cast<qint32>(pKnown - knowledge.units().data());
        facts.capturePoints = pKnown->capturePoints;
        facts.captureRate = pKnown->captureRate;
        facts.captureTurnsRemaining = Coordinator::captureTurnsFor(facts.capturePoints, facts.captureRate,
                                                                   Unit::MAX_CAPTURE_POINTS);
    }
}

namespace Coordinator
{
    std::vector<PropertyFacts> buildPropertyEconomics(GameMap & map, const BattlefieldKnowledge & knowledge)
    {
        const std::vector<KnownBuilding> & buildings = knowledge.buildings();
        std::vector<PropertyFacts> properties;
        properties.reserve(buildings.size());
        QHash<QString, BuildingIdFacts> knownFacts;
        for (std::size_t i = 0; i < buildings.size(); ++i)
        {
            const KnownBuilding & known = buildings[i];
            Building* pBuilding = buildingAt(map, known.x, known.y);
            if (pBuilding == nullptr)
            {
                AI_CONSOLE_PRINT("Coordinator::buildPropertyEconomics() no building at " + QString::number(known.x) +
                                 "," + QString::number(known.y), GameConsole::eDEBUG);
                continue;
            }
            const BuildingIdFacts idFacts = buildingIdFacts(knownFacts, known.buildingId, *pBuilding);
            PropertyFacts facts{
                .buildingIndex = static_cast<qint32>(i),
                .x = known.x,
                .y = known.y,
                .buildingId = known.buildingId,
                .ownerId = known.ownerId,
                .visible = knowledge.isVisible(known.x, known.y),
                .baseIncome = idFacts.baseIncome,
                .coveredTileCount = coveredTileCount(*pBuilding),
                .incomePerTurn = incomePerTurn(*pBuilding),
                .capturable = idFacts.capturable,
                .objective = idFacts.objective,
                .canProduce = pBuilding->isProductionBuilding(),
                .repairTypeMask = idFacts.repairTypeMask,
            };
            if (facts.canProduce && facts.ownerId == NO_OWNER)
            {
                facts.productionList = neutralProductionList(map, knowledge, *pBuilding);
            }
            else if (facts.canProduce)
            {
                facts.productionList = pBuilding->getConstructionList();
            }
            addCaptureProgress(facts, knowledge);
            properties.push_back(std::move(facts));
        }
        return properties;
    }
}
