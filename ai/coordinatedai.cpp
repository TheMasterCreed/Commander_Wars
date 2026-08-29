#include "ai/coordinatedai.h"

#include <utility>

#include "ai/coordinator/attackopportunitybuilder.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/engineactionbuilder.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
#include "ai/coordinator/refinementledger.h"

#include "coreengine/qmlvector.h"

#include "game/gamemap.h"
#include "game/player.h"
#include "game/unit.h"

namespace
{
bool sameExecutionIntent(
    const Coordinator::PlannedAction & left,
    const Coordinator::PlannedAction & right)
{
    return left.kind == right.kind &&
           left.actionId == right.actionId &&
           left.path == right.path &&
           left.destination == right.destination &&
           left.target == right.target &&
           left.targetUnitId == right.targetUnitId;
}
}

CoordinatedAi::CoordinatedAi(GameMap* pMap)
    : NormalAi(
          pMap,
          NormalAi::DEFAULT_INI_FILE,
          GameEnums::AiTypes_Coordinated,
          NormalAi::DEFAULT_JS_NAME)
{
#ifdef GRAPHICSUPPORT
    setObjectName("CoordinatedAi");
#endif
}

void CoordinatedAi::process()
{
    if (holdForPause())
    {
        return;
    }
    if (m_turnMode == GameEnums::AiTurnMode_StartOfDay &&
        ensureFactLayers())
    {
        if (m_coPowerDay != m_pMap->getCurrentDay())
        {
            m_coPowerDay = m_pMap->getCurrentDay();
            if (useStartOfDayCoPower())
            {
                m_factLayersDay = UNBUILT_DAY;
                return;
            }
        }
        ensureTurnPlan();
    }
    if (executeNextPlannedAction())
    {
        return;
    }
    NormalAi::process();
}

bool CoordinatedAi::ensureFactLayers()
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    if (m_factLayersDay != m_pMap->getCurrentDay())
    {
        buildFactLayers();
        m_factLayersDay = m_pMap->getCurrentDay();
    }
    return true;
}

void CoordinatedAi::buildFactLayers()
{
    m_dayStartKnowledge =
        Coordinator::BattlefieldKnowledge::capture(*m_pMap, *m_pPlayer);
    m_mobilityFields.clear();
    m_attackOpportunities =
        Coordinator::buildAttackOpportunityField(
            *m_pMap, m_dayStartKnowledge, m_mobilityFields);
    m_properties =
        Coordinator::buildPropertyEconomics(*m_pMap, m_dayStartKnowledge);
    m_propertyStock = Coordinator::buildPropertyStockField(
        *m_pMap,
        m_dayStartKnowledge,
        m_properties,
        m_mobilityFields,
        Coordinator::makeValuationContext(*m_pMap).horizonTurns);
}

void CoordinatedAi::ensureTurnPlan()
{
    if (m_planDay == m_pMap->getCurrentDay())
    {
        return;
    }
    buildTurnPlan();
    m_planDay = m_pMap->getCurrentDay();
}

void CoordinatedAi::buildTurnPlan()
{
    m_executionCursor = 0;
    Coordinator::AssignmentInput input;
    input.actionIds = Coordinator::engineActionIds();
    input.unitLinks = linkKnownUnits(m_dayStartKnowledge);
    Coordinator::JointPlanStockValuer stockValuer(
        m_propertyStock, input.unitLinks);
    input.pStockValuer = &stockValuer;
    Coordinator::DamageOracle oracle(*m_pMap);
    oracle.clear();
    Coordinator::BundleBuildStats buildStats;
    const std::vector<Coordinator::KnownUnit> & units =
        m_dayStartKnowledge.units();
    for (std::size_t slot = 0; slot < units.size(); ++slot)
    {
        Unit* pUnit = plannableUnit(units[slot]);
        if (pUnit == nullptr)
        {
            continue;
        }
        Coordinator::AssignmentActor actor;
        actor.knowledgeUnitIndex = static_cast<qint32>(slot);
        actor.engineUnitId = pUnit->getUniqueID();
        actor.candidates = candidatesFor(
            m_dayStartKnowledge,
            m_attackOpportunities,
            m_properties,
            m_propertyStock,
            oracle,
            actor.knowledgeUnitIndex,
            buildStats);
        input.actors.push_back(std::move(actor));
    }

    Coordinator::ContinuationPricingCatalog catalog =
        stockValuer.continuationPricingCatalog(input);
    Coordinator::RefinementLedger ledger;
    if (catalog.valid && ledger.open(catalog.ledgerTotal))
    {
        static_cast<void>(
            stockValuer.prepareContinuationPricing(
                std::move(catalog), ledger));
    }
    m_assignment = Coordinator::MaximumValueAssignment::assign(input);
}

Unit* CoordinatedAi::plannableUnit(
    const Coordinator::KnownUnit & known) const
{
    if (known.hasMoved ||
        m_dayStartKnowledge.relation(known.ownerId) !=
            Coordinator::Relation::Own)
    {
        return nullptr;
    }
    Unit* pUnit = Coordinator::liveUnitFor(*m_pMap, known);
    if (pUnit == nullptr ||
        pUnit->getHasMoved() ||
        pUnit->getAiMode() != GameEnums::GameAi_Normal)
    {
        return nullptr;
    }
    if (!known.canFire && !pUnit->canCapture())
    {
        return nullptr;
    }
    return pUnit;
}

std::vector<Coordinator::KnownUnitLink>
CoordinatedAi::linkKnownUnits(
    const Coordinator::BattlefieldKnowledge & knowledge) const
{
    std::vector<Coordinator::KnownUnitLink> links;
    links.reserve(knowledge.units().size());
    for (const Coordinator::KnownUnit & known : knowledge.units())
    {
        Coordinator::KnownUnitLink link;
        Unit* pUnit = Coordinator::liveUnitFor(*m_pMap, known);
        if (pUnit != nullptr)
        {
            link.engineUnitId = pUnit->getUniqueID();
            link.tile = Coordinator::TilePoint{known.x, known.y};
        }
        links.push_back(link);
    }
    return links;
}

std::vector<Coordinator::CandidateBundle>
CoordinatedAi::candidatesFor(
    const Coordinator::BattlefieldKnowledge & knowledge,
    const Coordinator::AttackOpportunityField & enemyReach,
    std::span<const Coordinator::PropertyFacts> properties,
    const Coordinator::PropertyStockField & propertyStock,
    Coordinator::DamageOracle & oracle,
    qint32 actorUnitIndex,
    Coordinator::BundleBuildStats & stats)
{
    const Coordinator::BundleBuildContext context{
        .map = *m_pMap,
        .player = *m_pPlayer,
        .knowledge = knowledge,
        .enemyReach = enemyReach,
        .properties = properties,
        .mobility = m_mobilityFields,
        .propertyStock = propertyStock,
        .oracle = oracle,
        .valuation = Coordinator::makeValuationContext(*m_pMap),
    };
    return Coordinator::buildCandidateBundles(
        context, actorUnitIndex, stats);
}

bool CoordinatedAi::useStartOfDayCoPower()
{
    spQmlVectorUnit pUnits = m_pPlayer->getSpUnits();
    spQmlVectorBuilding pBuildings = m_pPlayer->getSpBuildings();
    spQmlVectorUnit pEnemyUnits;
    spQmlVectorBuilding pEnemyBuildings;
    prepareEnemieData(
        pUnits, pBuildings, pEnemyUnits, pEnemyBuildings);
    return useCOPower(pUnits, pEnemyUnits);
}

bool CoordinatedAi::executeNextPlannedAction()
{
    bool replanAllowed = true;
    while (m_executionCursor <
           m_assignment.executionOrder.size())
    {
        const qint32 actionIndex =
            m_assignment.executionOrder[m_executionCursor];
        ++m_executionCursor;
        if (m_assignment.plan.action(actionIndex).state !=
            Coordinator::PlanActionState::Pending)
        {
            continue;
        }
        if (performPlannedAction(actionIndex, replanAllowed))
        {
            return true;
        }
    }
    return false;
}

bool CoordinatedAi::performPlannedAction(
    qint32 actionIndex, bool & replanAllowed)
{
    Coordinator::EngineActionBuildResult result =
        Coordinator::buildEngineAction(
            *m_pMap,
            *m_pPlayer,
            m_assignment.plan.action(actionIndex));
    if (!result)
    {
        if (!replanAllowed)
        {
            failPlannedAction(actionIndex);
            return false;
        }
        replanAllowed = false;
        if (!replanFailedAction(actionIndex))
        {
            failPlannedAction(actionIndex);
            return false;
        }
        result = Coordinator::buildEngineAction(
            *m_pMap,
            *m_pPlayer,
            m_assignment.plan.action(actionIndex));
        if (!result)
        {
            failPlannedAction(actionIndex);
            return false;
        }
    }
    if (!m_assignment.plan.commit(actionIndex))
    {
        failPlannedAction(actionIndex);
        return false;
    }
    emit sigPerformAction(result.action);
    return true;
}

void CoordinatedAi::failPlannedAction(qint32 actionIndex)
{
    static_cast<void>(
        m_assignment.plan.markFailed(actionIndex));
}

bool CoordinatedAi::replanFailedAction(qint32 actionIndex)
{
    const Coordinator::PlannedAction previous =
        m_assignment.plan.action(actionIndex);
    const qint32 engineUnitId =
        previous.unitId;
    Unit* pUnit = m_pMap->getUnit(engineUnitId);
    if (pUnit == nullptr ||
        pUnit->getOwner() != m_pPlayer ||
        pUnit->getHasMoved())
    {
        return false;
    }
    const Coordinator::BattlefieldKnowledge knowledge =
        Coordinator::BattlefieldKnowledge::capture(
            *m_pMap, *m_pPlayer);
    const qint32 actorUnitIndex = knowledge.unitIndexAt(
        pUnit->Unit::getX(), pUnit->Unit::getY());
    if (actorUnitIndex == Coordinator::NO_UNIT)
    {
        return false;
    }
    const Coordinator::AttackOpportunityField enemyReach =
        Coordinator::buildAttackOpportunityField(
            *m_pMap, knowledge, m_mobilityFields);
    const std::vector<Coordinator::PropertyFacts> properties =
        Coordinator::buildPropertyEconomics(*m_pMap, knowledge);
    Coordinator::PropertyStockField propertyStock =
        Coordinator::buildPropertyStockField(
            *m_pMap,
            knowledge,
            properties,
            m_mobilityFields,
            Coordinator::makeValuationContext(*m_pMap).horizonTurns);
    Coordinator::DamageOracle oracle(*m_pMap);
    oracle.clear();
    Coordinator::BundleBuildStats buildStats;
    const std::vector<Coordinator::CandidateBundle> candidates =
        candidatesFor(
            knowledge,
            enemyReach,
            properties,
            propertyStock,
            oracle,
            actorUnitIndex,
            buildStats);
    const Coordinator::PlanActionIds actionIds =
        Coordinator::engineActionIds();
    const std::vector<Coordinator::KnownUnitLink> unitLinks =
        linkKnownUnits(knowledge);
    for (const Coordinator::CandidateBundle & candidate : candidates)
    {
        if (!Coordinator::isPlannableCandidate(
                actionIds, unitLinks, candidate))
        {
            continue;
        }
        const Coordinator::PlannedAction fresh =
            Coordinator::plannedActionFrom(
                actionIds,
                unitLinks,
                engineUnitId,
                candidate);
        if (sameExecutionIntent(previous, fresh))
        {
            if (Coordinator::installCandidate(
                    m_assignment.plan,
                    actionIndex,
                    fresh,
                    candidate) ==
                Coordinator::ReservationResult::Granted)
            {
                return true;
            }
            break;
        }
    }
    Coordinator::AssignmentStats assignmentStats;
    return Coordinator::replanAction(
        m_assignment.plan,
        actionIndex,
        actionIds,
        unitLinks,
        candidates,
        assignmentStats);
}
