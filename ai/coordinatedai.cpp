#include "ai/coordinatedai.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "ai/coordinator/attackopportunitybuilder.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/decisiontrace.h"
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

QString relationName(Coordinator::Relation relation)
{
    switch (relation)
    {
        case Coordinator::Relation::Own:
            return QStringLiteral("OWN");
        case Coordinator::Relation::Allied:
            return QStringLiteral("ALLIED");
        case Coordinator::Relation::Enemy:
            return QStringLiteral("ENEMY");
        case Coordinator::Relation::Neutral:
            return QStringLiteral("NEUTRAL");
    }
    return QStringLiteral("UNKNOWN");
}

QString engineActionFailureName(
    Coordinator::EngineActionFailure failure)
{
    switch (failure)
    {
        case Coordinator::EngineActionFailure::None:
            return QStringLiteral("NONE");
        case Coordinator::EngineActionFailure::InvalidShape:
            return QStringLiteral("INVALID_SHAPE");
        case Coordinator::EngineActionFailure::ActorUnavailable:
            return QStringLiteral("ACTOR_UNAVAILABLE");
        case Coordinator::EngineActionFailure::OriginMismatch:
            return QStringLiteral("ORIGIN_MISMATCH");
        case Coordinator::EngineActionFailure::IllegalAction:
            return QStringLiteral("ILLEGAL_ACTION");
        case Coordinator::EngineActionFailure::TargetUnavailable:
            return QStringLiteral("TARGET_UNAVAILABLE");
        case Coordinator::EngineActionFailure::InvalidTargetStep:
            return QStringLiteral("INVALID_TARGET_STEP");
    }
    return QStringLiteral("UNKNOWN");
}

const Coordinator::BundleComponent* captureComponentOf(
    const Coordinator::CandidateBundle* pCandidate)
{
    if (pCandidate == nullptr)
    {
        return nullptr;
    }
    const Coordinator::BundleComponent* pComponent =
        Coordinator::singleComponentOf(pCandidate->bundle);
    if (pComponent == nullptr ||
        pComponent->kind != Coordinator::ComponentKind::Capture)
    {
        return nullptr;
    }
    return pComponent;
}

QString selectionDecisionBasis(
    Coordinator::AssignmentResult::SelectionPhase phase)
{
    switch (phase)
    {
        case Coordinator::AssignmentResult::SelectionPhase::None:
            return QStringLiteral("NONE");
        case Coordinator::AssignmentResult::SelectionPhase::Greedy:
            return QStringLiteral("GREEDY_SEATED_VALUE");
        case Coordinator::AssignmentResult::SelectionPhase::Settling:
            return QStringLiteral(
                "SETTLING_CHALLENGER_COMPLETE_VALUE");
        case Coordinator::AssignmentResult::SelectionPhase::
            PairRefinement:
            return QStringLiteral("PAIR_COMPARE_PLAN_BOUNDS");
        case Coordinator::AssignmentResult::SelectionPhase::Cluster:
            return QStringLiteral("CLUSTER_PLAN_COMPLETE_VALUE");
    }
    return QStringLiteral("UNKNOWN");
}

QString selectionPhaseName(
    Coordinator::AssignmentResult::SelectionPhase phase)
{
    switch (phase)
    {
        case Coordinator::AssignmentResult::SelectionPhase::None:
            return QStringLiteral("NONE");
        case Coordinator::AssignmentResult::SelectionPhase::Greedy:
            return QStringLiteral("GREEDY");
        case Coordinator::AssignmentResult::SelectionPhase::Settling:
            return QStringLiteral("SETTLING");
        case Coordinator::AssignmentResult::SelectionPhase::
            PairRefinement:
            return QStringLiteral("PAIR_REFINEMENT");
        case Coordinator::AssignmentResult::SelectionPhase::Cluster:
            return QStringLiteral("CLUSTER");
    }
    return QStringLiteral("UNKNOWN");
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
            const bool timed =
                Coordinator::decisionTraceEnabled() ||
                Coordinator::planningTimingAuditEnabled();
            std::chrono::steady_clock::time_point coPowerStart;
            if (timed)
            {
                coPowerStart = std::chrono::steady_clock::now();
            }
            const bool usedCoPower = useStartOfDayCoPower();
            if (timed)
            {
                m_coPowerCheckNanos =
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        coPowerStart)
                        .count();
            }
            else
            {
                m_coPowerCheckNanos = 0;
            }
            if (usedCoPower)
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
    using Clock = std::chrono::steady_clock;
    const bool timed =
        Coordinator::decisionTraceEnabled() ||
        Coordinator::planningTimingAuditEnabled();
    Clock::time_point factStart;
    if (timed)
    {
        factStart = Clock::now();
    }
    m_dayStartKnowledge =
        Coordinator::BattlefieldKnowledge::capture(*m_pMap, *m_pPlayer);
    m_mobilityFields.clear();
    m_attackOpportunities =
        Coordinator::buildAttackOpportunityField(
            *m_pMap, m_dayStartKnowledge, m_mobilityFields);
    m_properties =
        Coordinator::buildPropertyEconomics(*m_pMap, m_dayStartKnowledge);
    Clock::time_point propertyStockStart;
    if (timed)
    {
        propertyStockStart = Clock::now();
    }
    m_propertyStock = Coordinator::buildPropertyStockField(
        *m_pMap,
        m_dayStartKnowledge,
        m_properties,
        m_mobilityFields,
        Coordinator::makeValuationContext(*m_pMap).horizonTurns);
    if (timed)
    {
        const Clock::time_point factEnd = Clock::now();
        m_propertyStockBuildNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                factEnd - propertyStockStart)
                .count();
        m_factLayersNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                factEnd - factStart)
                .count();
    }
    else
    {
        m_propertyStockBuildNanos = 0;
        m_factLayersNanos = 0;
    }
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
    using Clock = std::chrono::steady_clock;
    struct ActorTiming
    {
        qint32 knowledgeUnitIndex{Coordinator::NO_UNIT};
        qint32 engineUnitId{Coordinator::NO_UNIT};
        std::int32_t generatedCandidateCount{0};
        std::int32_t validCandidateCount{0};
        std::int64_t nanos{0};
    };

    Clock::time_point planningStart;
    m_executionCursor = 0;
    ++m_planSequence;
    const Coordinator::DecisionTraceIdentity traceIdentity{
        .day = m_pMap->getCurrentDay(),
        .playerId = m_pPlayer->getPlayerID(),
        .planSequence = m_planSequence,
        .horizonTurns = m_propertyStock.horizonTurns(),
    };
    m_decisionTrace =
        Coordinator::openDecisionTrace(traceIdentity);
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("PLAN_BEGIN"),
            QStringLiteral(
                "mapWidth=%1 mapHeight=%2 properties=%3")
                .arg(m_dayStartKnowledge.width())
                .arg(m_dayStartKnowledge.height())
                .arg(m_properties.size()));
        if (!m_decisionTrace->flush())
        {
            m_decisionTrace.reset();
        }
    }
    const bool timed =
        m_decisionTrace != nullptr ||
        Coordinator::planningTimingAuditEnabled();
    if (timed)
    {
        planningStart = Clock::now();
    }
    if (m_decisionTrace != nullptr)
    {
        for (std::size_t propertyIndex = 0;
             propertyIndex < m_properties.size();
             ++propertyIndex)
        {
            const Coordinator::PropertyFacts & facts =
                m_properties[propertyIndex];
            const Coordinator::TilePoint tile{
                facts.x, facts.y};
            m_decisionTrace->record(
                QStringLiteral("PROPERTY"),
                QStringLiteral(
                    "property=%1 stockColumn=%2 coordinate=%3 ownerId=%4 ownerSign=%5 capturable=%6 income=%7 currentCapturePoints=%8 currentCapturerKnowledge=%9 captureRate=%10 captureTurnsRemaining=%11")
                    .arg(propertyIndex)
                    .arg(m_propertyStock.columnSlotAt(tile))
                    .arg(Coordinator::traceTile(tile))
                    .arg(facts.ownerId)
                    .arg(relationName(
                        m_dayStartKnowledge.relation(
                            facts.ownerId)))
                    .arg(Coordinator::traceBool(
                        facts.capturable))
                    .arg(facts.incomePerTurn)
                    .arg(facts.capturePoints)
                    .arg(facts.capturerIndex)
                    .arg(facts.captureRate)
                    .arg(facts.captureTurnsRemaining));
        }
    }
    Coordinator::AssignmentInput input;
    input.actionIds = Coordinator::engineActionIds();
    input.unitLinks = linkKnownUnits(m_dayStartKnowledge);
    Coordinator::JointPlanStockValuer stockValuer(
        m_propertyStock, input.unitLinks);
    input.pStockValuer = &stockValuer;
    input.pTrace = m_decisionTrace.get();
    input.measureTimings = timed;
    Coordinator::DamageOracle oracle(*m_pMap);
    oracle.clear();
    Coordinator::BundleBuildStats buildStats;
    std::vector<ActorTiming> actorTimings;
    std::int64_t candidateBuildNanos = 0;
    std::int32_t maxCandidatesPerActor = 0;
    const std::vector<Coordinator::KnownUnit> & units =
        m_dayStartKnowledge.units();
    for (std::size_t slot = 0; slot < units.size(); ++slot)
    {
        Unit* pUnit = plannableUnit(units[slot]);
        if (m_decisionTrace != nullptr &&
            m_dayStartKnowledge.relation(
                units[slot].ownerId) ==
                Coordinator::Relation::Own)
        {
            Unit* pLive = pUnit;
            if (pLive == nullptr)
            {
                pLive = Coordinator::liveUnitFor(
                    *m_pMap, units[slot]);
            }
            m_decisionTrace->record(
                QStringLiteral("ACTOR"),
                QStringLiteral(
                    "actorKnowledge=%1 actor=%2 type=%3 origin=%4 hpSteps=%5 movementPoints=%6 canCapture=%7 captureRate=%8 hasMoved=%9 aiMode=%10 plannable=%11")
                    .arg(slot)
                    .arg(
                        pLive == nullptr
                            ? Coordinator::NO_UNIT
                            : pLive->getUniqueID())
                    .arg(units[slot].unitId)
                    .arg(Coordinator::traceTile(
                        Coordinator::TilePoint{
                            units[slot].x,
                            units[slot].y}))
                    .arg(units[slot].hpRounded)
                    .arg(units[slot].movementPoints)
                    .arg(Coordinator::traceBool(
                        units[slot].canCapture))
                    .arg(units[slot].captureRate)
                    .arg(Coordinator::traceBool(
                        units[slot].hasMoved))
                    .arg(
                        pLive == nullptr
                            ? -1
                            : static_cast<qint32>(
                                  pLive->getAiMode()))
                    .arg(Coordinator::traceBool(
                        pUnit != nullptr)));
        }
        if (pUnit == nullptr)
        {
            continue;
        }
        Coordinator::AssignmentActor actor;
        actor.knowledgeUnitIndex = static_cast<qint32>(slot);
        actor.engineUnitId = pUnit->getUniqueID();
        Clock::time_point candidateStart;
        if (timed)
        {
            candidateStart = Clock::now();
        }
        const std::int32_t generatedBefore =
            buildStats.candidateCount;
        actor.candidates = candidatesFor(
            m_dayStartKnowledge,
            m_attackOpportunities,
            m_properties,
            m_propertyStock,
            oracle,
            actor.knowledgeUnitIndex,
            buildStats);
        if (timed)
        {
            const std::int32_t generatedCandidateCount =
                buildStats.candidateCount - generatedBefore;
            const std::int64_t actorCandidateNanos =
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    Clock::now() - candidateStart)
                    .count();
            candidateBuildNanos += actorCandidateNanos;
            maxCandidatesPerActor = std::max(
                maxCandidatesPerActor,
                generatedCandidateCount);
            if (m_decisionTrace != nullptr)
            {
                actorTimings.push_back(ActorTiming{
                    .knowledgeUnitIndex =
                        actor.knowledgeUnitIndex,
                    .engineUnitId = actor.engineUnitId,
                    .generatedCandidateCount =
                        generatedCandidateCount,
                    .validCandidateCount =
                        static_cast<std::int32_t>(
                            actor.candidates.size()),
                    .nanos = actorCandidateNanos,
                });
            }
        }
        input.actors.push_back(std::move(actor));
    }

    std::int32_t stockCoupledActors = 0;
    if (timed)
    {
        for (const Coordinator::AssignmentActor & actor :
             input.actors)
        {
            if (stockValuer.affectsStock(
                    actor.engineUnitId))
            {
                ++stockCoupledActors;
            }
        }
    }
    Clock::time_point continuationStart;
    if (timed)
    {
        continuationStart = Clock::now();
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
    const std::int64_t continuationPricingPrepareNanos =
        !timed
            ? 0
            : std::chrono::duration_cast<
                  std::chrono::nanoseconds>(
                  Clock::now() - continuationStart)
                  .count();
    m_assignment = Coordinator::MaximumValueAssignment::assign(input);
    recordCaptureDecisions(input);
    recordProductionBlocks(input);
    const std::int64_t turnPlanNanos =
        !timed
            ? 0
            : std::chrono::duration_cast<
                  std::chrono::nanoseconds>(
                  Clock::now() - planningStart)
                  .count();
    const Coordinator::AssignmentResult::PhaseTiming &
        assignmentTiming = m_assignment.phaseTiming;
    const Coordinator::AssignmentStats & stats =
        m_assignment.stats;
    QString phaseTimingFields;
    if (timed)
    {
        phaseTimingFields =
            QStringLiteral(
                "actorCount=%1 factLayersNanos=%2 propertyStockBuildNanos=%3 coPowerCheckNanos=%4 candidateBuildNanos=%5 continuationPricingPrepareNanos=%6 assignmentPrepareNanos=%7 greedyNanos=%8 settlingNanos=%9 pairRefinementNanos=%10 clusterNanos=%11 finishPlanNanos=%12 totalPlanningNanos=%13 totalCandidates=%14 maxCandidatesPerActor=%15 damageOracleCalls=%16 damageOracleHits=%17 stockCoupledActors=%18 settlingSweeps=%19 settlingMoves=%20 swapStates=%21 swapImprovements=%22 clustersTotal=%23 clustersEnumerated=%24 clustersCapped=%25 clustersSkippedStockBudget=%26 enumerationStates=%27 planSequence=%28")
                .arg(input.actors.size())
                .arg(m_factLayersNanos)
                .arg(m_propertyStockBuildNanos)
                .arg(m_coPowerCheckNanos)
                .arg(candidateBuildNanos)
                .arg(continuationPricingPrepareNanos)
                .arg(assignmentTiming.prepareNanos)
                .arg(assignmentTiming.greedyNanos)
                .arg(assignmentTiming.settlingNanos)
                .arg(assignmentTiming.pairRefinementNanos)
                .arg(assignmentTiming.clusterNanos)
                .arg(assignmentTiming.finishPlanNanos)
                .arg(m_factLayersNanos +
                     m_coPowerCheckNanos +
                     turnPlanNanos)
                .arg(buildStats.candidateCount)
                .arg(maxCandidatesPerActor)
                .arg(buildStats.oracleCalls)
                .arg(buildStats.oracleHits)
                .arg(stockCoupledActors)
                .arg(stats.settlingSweeps)
                .arg(stats.settlingMoves)
                .arg(stats.swapStates)
                .arg(stats.swapImprovements)
                .arg(stats.clustersTotal)
                .arg(stats.clustersEnumerated)
                .arg(stats.clustersCapped)
                .arg(stats.clustersSkippedStockBudget)
                .arg(stats.enumerationStates)
                .arg(m_planSequence);
    }
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("BUILD_STATS"),
            QStringLiteral(
                "actors=%1 totalCandidates=%2 validCandidates=%3 invalidCandidates=%4 missingUnits=%5 damageOracleCalls=%6 damageOracleHits=%7 maxCandidatesPerActor=%8 stockCoupledActors=%9")
                .arg(input.actors.size())
                .arg(buildStats.candidateCount)
                .arg(buildStats.candidateCount -
                     buildStats.invalidCount)
                .arg(buildStats.invalidCount)
                .arg(buildStats.missingUnits)
                .arg(buildStats.oracleCalls)
                .arg(buildStats.oracleHits)
                .arg(maxCandidatesPerActor)
                .arg(stockCoupledActors));
        m_decisionTrace->flush();

        m_decisionTrace->record(
            QStringLiteral("PHASE_TIMING"),
            phaseTimingFields);
        for (const ActorTiming & timing : actorTimings)
        {
            m_decisionTrace->record(
                QStringLiteral("ACTOR_TIMING"),
                QStringLiteral(
                    "actorKnowledge=%1 actor=%2 generatedCandidates=%3 validCandidates=%4 candidateGenerationNanos=%5")
                    .arg(timing.knowledgeUnitIndex)
                    .arg(timing.engineUnitId)
                    .arg(timing.generatedCandidateCount)
                    .arg(timing.validCandidateCount)
                    .arg(timing.nanos));
        }
        m_decisionTrace->flush();
    }
    Coordinator::writePlanningTimingAudit(
        traceIdentity,
        phaseTimingFields);
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
        .pTrace = m_decisionTrace.get(),
    };
    return Coordinator::buildCandidateBundles(
        context, actorUnitIndex, stats);
}

const Coordinator::AssignmentResult::Selection*
CoordinatedAi::selectionForAction(qint32 actionIndex) const
{
    for (const Coordinator::AssignmentResult::Selection &
         selection : m_assignment.selections)
    {
        if (selection.actionIndex == actionIndex)
        {
            return &selection;
        }
    }
    return nullptr;
}

void CoordinatedAi::recordCaptureDecisions(
    const Coordinator::AssignmentInput & input)
{
    if (m_decisionTrace == nullptr)
    {
        return;
    }
    const std::vector<Coordinator::KnownUnit> & units =
        m_dayStartKnowledge.units();
    for (std::size_t knowledgeIndex = 0;
         knowledgeIndex < units.size();
         ++knowledgeIndex)
    {
        const Coordinator::KnownUnit & known =
            units[knowledgeIndex];
        if (!known.canCapture ||
            m_dayStartKnowledge.relation(known.ownerId) !=
                Coordinator::Relation::Own)
        {
            continue;
        }
        const Coordinator::AssignmentActor* pActor =
            nullptr;
        for (const Coordinator::AssignmentActor & actor :
             input.actors)
        {
            if (actor.knowledgeUnitIndex ==
                static_cast<std::int32_t>(
                    knowledgeIndex))
            {
                pActor = &actor;
                break;
            }
        }
        if (pActor == nullptr)
        {
            Unit* pLive = Coordinator::liveUnitFor(
                *m_pMap, known);
            QString exclusionReason =
                QStringLiteral("NOT_PLANNABLE");
            if (known.hasMoved)
            {
                exclusionReason =
                    QStringLiteral("KNOWLEDGE_HAS_MOVED");
            }
            else if (pLive == nullptr)
            {
                exclusionReason =
                    QStringLiteral("LIVE_UNIT_MISSING");
            }
            else if (pLive->getHasMoved())
            {
                exclusionReason =
                    QStringLiteral("LIVE_UNIT_HAS_MOVED");
            }
            else if (pLive->getAiMode() !=
                     GameEnums::GameAi_Normal)
            {
                exclusionReason =
                    QStringLiteral("AI_MODE_NOT_NORMAL");
            }
            const Coordinator::TilePoint origin{
                known.x, known.y};
            const std::int32_t currentProperty =
                m_propertyStock.propertyIndexAt(origin);
            const Coordinator::PropertyFacts* pCurrent =
                currentProperty ==
                        Coordinator::NO_PROPERTY_INDEX
                    ? nullptr
                    : &m_properties[
                          static_cast<std::size_t>(
                              currentProperty)];
            const std::int32_t engineUnitId =
                knowledgeIndex <
                        input.unitLinks.size()
                    ? input.unitLinks[knowledgeIndex]
                          .engineUnitId
                    : Coordinator::NO_UNIT;
            m_decisionTrace->record(
                QStringLiteral("CAPTURE_DECISION"),
                QStringLiteral(
                    "actor=%1 actorKnowledge=%2 type=%3 origin=%4 currentProperty=%5 currentCapturePoints=%6 currentCapturerKnowledge=%7 incumbentCandidate=-1 incumbentKind=NONE incumbentDst=(-1,-1) incumbentCompleteValue=NA selectedCandidate=-1 selectedKind=NONE selectedDst=(-1,-1) selectedSeatedValue=0 selectedCompleteValue=NA selectionPhase=NONE stayCaptureCandidate=-1 stayCaptureCompleteValue=NA abandonedPartialCapture=NA partialCaptureAtPlanStart=%8 abandonmentReason=NO_COORDINATED_FINAL_ACTION incumbentCompleteKnown=false selectedCompleteKnown=false stayCaptureCompleteKnown=false completeValueScope=NONE decisionBasis=EXCLUDED diagnosticApplicable=false excluded=true exclusionReason=%9")
                    .arg(engineUnitId)
                    .arg(knowledgeIndex)
                    .arg(known.unitId)
                    .arg(Coordinator::traceTile(origin))
                    .arg(currentProperty)
                    .arg(
                        pCurrent == nullptr
                            ? 0
                            : pCurrent->capturePoints)
                    .arg(
                        pCurrent == nullptr
                            ? Coordinator::NO_CAPTURER
                            : pCurrent->capturerIndex)
                    .arg(Coordinator::traceBool(
                        pCurrent != nullptr &&
                        pCurrent->capturerIndex ==
                            static_cast<std::int32_t>(
                                knowledgeIndex) &&
                        pCurrent->capturePoints > 0))
                    .arg(exclusionReason));
            continue;
        }
        const Coordinator::AssignmentActor & actor =
            *pActor;
        const Coordinator::AssignmentResult::Selection*
            pSelection = nullptr;
        for (const Coordinator::AssignmentResult::Selection &
             selection : m_assignment.selections)
        {
            if (selection.engineUnitId ==
                actor.engineUnitId)
            {
                pSelection = &selection;
                break;
            }
        }
        const Coordinator::TilePoint origin{
            known.x, known.y};
        const std::int32_t currentProperty =
            m_propertyStock.propertyIndexAt(origin);
        const Coordinator::PropertyFacts* pCurrent =
            currentProperty == Coordinator::NO_PROPERTY_INDEX
                ? nullptr
                : &m_properties[
                      static_cast<std::size_t>(
                          currentProperty)];

        const auto candidateAt =
            [&](std::int32_t candidateIndex)
                -> const Coordinator::CandidateBundle*
        {
            if (candidateIndex < 0 ||
                candidateIndex >=
                    static_cast<std::int32_t>(
                        actor.candidates.size()))
            {
                return nullptr;
            }
            return &actor.candidates[
                static_cast<std::size_t>(
                    candidateIndex)];
        };
        const auto completeKnown =
            [&](std::int32_t candidateIndex)
        {
            return pSelection != nullptr &&
                   candidateIndex >= 0 &&
                   candidateIndex <
                       static_cast<std::int32_t>(
                           pSelection->completeValues.size()) &&
                   pSelection
                       ->completeValues[
                           static_cast<std::size_t>(
                               candidateIndex)]
                       .known;
        };
        const auto completeValue =
            [&](std::int32_t candidateIndex)
        {
            if (!completeKnown(candidateIndex))
            {
                return QStringLiteral("NA");
            }
            return QString::number(
                pSelection
                    ->completeValues[
                        static_cast<std::size_t>(
                            candidateIndex)]
                    .value);
        };

        const std::int32_t incumbentCandidate =
            pSelection == nullptr
                ? Coordinator::NO_CANDIDATE
                : pSelection->greedyCandidateIndex;
        const std::int32_t selectedCandidate =
            pSelection == nullptr
                ? Coordinator::NO_CANDIDATE
                : pSelection->candidateIndex;
        const Coordinator::CandidateBundle* pIncumbent =
            candidateAt(incumbentCandidate);
        const Coordinator::CandidateBundle* pSelected =
            candidateAt(selectedCandidate);
        std::int32_t stayCaptureCandidate =
            Coordinator::NO_CANDIDATE;
        for (std::size_t candidateIndex = 0;
             candidateIndex < actor.candidates.size();
             ++candidateIndex)
        {
            const Coordinator::CandidateBundle & candidate =
                actor.candidates[candidateIndex];
            if (candidate.bundle.destination == origin &&
                captureComponentOf(&candidate) != nullptr)
            {
                stayCaptureCandidate =
                    static_cast<std::int32_t>(
                        candidateIndex);
                break;
            }
        }
        const bool partialCapture =
            pCurrent != nullptr &&
            pCurrent->capturerIndex ==
                actor.knowledgeUnitIndex &&
            pCurrent->capturePoints > 0;
        const bool continuesCurrentCapture =
            pCurrent != nullptr &&
            pSelected != nullptr &&
            pSelected->bundle.destination == origin &&
            captureComponentOf(pSelected) != nullptr;
        const bool abandonedPartialCapture =
            partialCapture && !continuesCurrentCapture;

        m_decisionTrace->record(
            QStringLiteral("CAPTURE_DECISION"),
            QStringLiteral(
                "actor=%1 actorKnowledge=%2 type=%3 origin=%4 currentProperty=%5 currentCapturePoints=%6 currentCapturerKnowledge=%7 incumbentCandidate=%8 incumbentKind=%9 incumbentDst=%10 incumbentCompleteValue=%11 selectedCandidate=%12 selectedKind=%13 selectedDst=%14 selectedSeatedValue=%15 selectedCompleteValue=%16 selectionPhase=%17 stayCaptureCandidate=%18 stayCaptureCompleteValue=%19 abandonedPartialCapture=%20 incumbentCompleteKnown=%21 selectedCompleteKnown=%22 stayCaptureCompleteKnown=%23 completeValueScope=SETTLING_TRIAL_SNAPSHOT decisionBasis=%24 diagnosticApplicable=true excluded=false exclusionReason=NONE")
                .arg(actor.engineUnitId)
                .arg(actor.knowledgeUnitIndex)
                .arg(known.unitId)
                .arg(Coordinator::traceTile(origin))
                .arg(currentProperty)
                .arg(
                    pCurrent == nullptr
                        ? 0
                        : pCurrent->capturePoints)
                .arg(
                    pCurrent == nullptr
                        ? Coordinator::NO_CAPTURER
                        : pCurrent->capturerIndex)
                .arg(incumbentCandidate)
                .arg(
                    pIncumbent == nullptr
                        ? QStringLiteral("NONE")
                        : Coordinator::traceBundleKind(
                              Coordinator::planBundleKindOf(
                                  pIncumbent->bundle)))
                .arg(
                    pIncumbent == nullptr
                        ? Coordinator::traceTile(
                              Coordinator::INVALID_TILE)
                        : Coordinator::traceTile(
                              pIncumbent->bundle.destination))
                .arg(completeValue(
                    incumbentCandidate))
                .arg(selectedCandidate)
                .arg(
                    pSelected == nullptr
                        ? QStringLiteral("NONE")
                        : Coordinator::traceBundleKind(
                              Coordinator::planBundleKindOf(
                                  pSelected->bundle)))
                .arg(
                    pSelected == nullptr
                        ? Coordinator::traceTile(
                              Coordinator::INVALID_TILE)
                        : Coordinator::traceTile(
                              pSelected->bundle.destination))
                .arg(
                    pSelection == nullptr
                        ? 0
                        : pSelection->seatedValue)
                .arg(completeValue(
                    selectedCandidate))
                .arg(
                    pSelection == nullptr
                        ? QStringLiteral("NONE")
                        : Coordinator::traceSelectionPhase(
                              pSelection->phase))
                .arg(stayCaptureCandidate)
                .arg(completeValue(
                    stayCaptureCandidate))
                .arg(Coordinator::traceBool(
                    abandonedPartialCapture))
                .arg(Coordinator::traceBool(
                    completeKnown(incumbentCandidate)))
                .arg(Coordinator::traceBool(
                    completeKnown(selectedCandidate)))
                .arg(Coordinator::traceBool(
                    completeKnown(stayCaptureCandidate)))
                .arg(
                    pSelection == nullptr
                        ? QStringLiteral("NONE")
                        : selectionDecisionBasis(
                              pSelection->phase)));
    }
}

void CoordinatedAi::recordProductionBlocks(
    const Coordinator::AssignmentInput & input)
{
    if (m_decisionTrace == nullptr ||
        !m_decisionTrace->stockDetailsEnabled())
    {
        return;
    }
    for (const Coordinator::AssignmentResult::Selection &
         selection : m_assignment.selections)
    {
        if (selection.actionIndex ==
                Coordinator::NO_ACTION ||
            selection.candidateIndex < 0)
        {
            continue;
        }
        const Coordinator::AssignmentActor* pActor =
            nullptr;
        for (const Coordinator::AssignmentActor & actor :
             input.actors)
        {
            if (actor.engineUnitId ==
                selection.engineUnitId)
            {
                pActor = &actor;
                break;
            }
        }
        if (pActor == nullptr ||
            selection.candidateIndex >=
                static_cast<std::int32_t>(
                    pActor->candidates.size()))
        {
            continue;
        }
        const Coordinator::PlannedAction & selectedAction =
            m_assignment.plan.action(
                selection.actionIndex);
        if (!Coordinator::isLiveState(
                selectedAction.state))
        {
            continue;
        }
        const Coordinator::CandidateBundle & selected =
            pActor->candidates[
                static_cast<std::size_t>(
                    selection.candidateIndex)];
        const Coordinator::TilePoint origin =
            selected.bundle.origin;
        if (selectedAction.destination != origin)
        {
            continue;
        }
        const std::int32_t propertyIndex =
            m_propertyStock.propertyIndexAt(origin);
        if (propertyIndex ==
                Coordinator::NO_PROPERTY_INDEX ||
            propertyIndex >=
                static_cast<std::int32_t>(
                    m_properties.size()))
        {
            continue;
        }
        const Coordinator::PropertyFacts & property =
            m_properties[
                static_cast<std::size_t>(
                    propertyIndex)];
        if (!property.canProduce ||
            m_dayStartKnowledge.relation(
                property.ownerId) !=
                Coordinator::Relation::Own)
        {
            continue;
        }

        std::int32_t vacatingCandidates = 0;
        std::int32_t legalVacatingCandidates = 0;
        std::int32_t bestCandidate =
            Coordinator::NO_CANDIDATE;
        std::int32_t bestGenerated = -1;
        Coordinator::PlanBundleKind bestKind =
            Coordinator::PlanBundleKind::Wait;
        Coordinator::TilePoint bestDestination =
            Coordinator::INVALID_TILE;
        Coordinator::TerminalValue bestValue;
        for (std::size_t candidateIndex = 0;
             candidateIndex <
                 pActor->candidates.size();
             ++candidateIndex)
        {
            const Coordinator::CandidateBundle & candidate =
                pActor->candidates[candidateIndex];
            if (candidate.bundle.destination == origin)
            {
                continue;
            }
            ++vacatingCandidates;
            Coordinator::TurnPlan auditPlan =
                m_assignment.plan;
            Coordinator::AssignmentStats auditStats;
            const std::vector<Coordinator::CandidateBundle>
                singleCandidate{candidate};
            if (!Coordinator::replanAction(
                    auditPlan,
                    selection.actionIndex,
                    input.actionIds,
                    input.unitLinks,
                    singleCandidate,
                    auditStats))
            {
                continue;
            }
            ++legalVacatingCandidates;
            const Coordinator::TerminalValue value =
                auditPlan
                    .action(selection.actionIndex)
                    .marginalValue;
            const std::int32_t index =
                static_cast<std::int32_t>(
                    candidateIndex);
            if (bestCandidate ==
                    Coordinator::NO_CANDIDATE ||
                value > bestValue ||
                (value == bestValue &&
                 index < bestCandidate))
            {
                bestCandidate = index;
                bestGenerated =
                    candidate.generationIndex;
                bestKind =
                    Coordinator::planBundleKindOf(
                        candidate.bundle);
                bestDestination =
                    candidate.bundle.destination;
                bestValue = value;
            }
        }
        const bool hasLegalVacating =
            bestCandidate != Coordinator::NO_CANDIDATE;
        const bool equalValue =
            hasLegalVacating &&
            bestValue == selectedAction.marginalValue;
        const QString unitType =
            selection.knowledgeUnitIndex >= 0 &&
                    selection.knowledgeUnitIndex <
                        static_cast<std::int32_t>(
                            m_dayStartKnowledge
                                .units()
                                .size())
                ? m_dayStartKnowledge
                      .units()[
                          static_cast<std::size_t>(
                              selection
                                  .knowledgeUnitIndex)]
                      .unitId
                : QStringLiteral("UNKNOWN");
        m_decisionTrace->record(
            QStringLiteral("PRODUCTION_BLOCK"),
            QStringLiteral(
                "actor=%1 actorKnowledge=%2 unitType=%3 property=%4 productionTile=%5 selectedCandidate=%6 selectedGenerated=%7 selectedKind=%8 selectedDestination=%9 selectedTerminal=%10 selectedValue=%11 selectedEconomicValue=%11 selectedSeatedValue=%12 selectionPhase=%13 selectionBasis=%14 vacatingCandidates=%15 legalVacatingCandidates=%16 noLegalVacatingCandidate=%17 bestVacatingCandidate=%18 bestVacatingGenerated=%19 bestVacatingKind=%20 bestVacatingDestination=%21 bestVacatingTerminal=%22 bestVacatingCandidateValue=%23 bestVacatingEconomicValue=%23 equalValue=%24 equalEconomicValue=%24 comparisonBasis=MARGINAL_TERMINAL_VALUE")
                .arg(selection.engineUnitId)
                .arg(selection.knowledgeUnitIndex)
                .arg(unitType)
                .arg(propertyIndex)
                .arg(Coordinator::traceTile(origin))
                .arg(selection.candidateIndex)
                .arg(selected.generationIndex)
                .arg(Coordinator::traceBundleKind(
                    selectedAction.kind))
                .arg(Coordinator::traceTile(
                    selectedAction.destination))
                .arg(static_cast<std::int32_t>(
                    selectedAction
                        .marginalValue.terminal))
                .arg(selectedAction
                         .marginalValue
                         .economicValue)
                .arg(selection.seatedValue)
                .arg(selectionPhaseName(
                    selection.phase))
                .arg(selectionDecisionBasis(
                    selection.phase))
                .arg(vacatingCandidates)
                .arg(legalVacatingCandidates)
                .arg(Coordinator::traceBool(
                    !hasLegalVacating))
                .arg(bestCandidate)
                .arg(bestGenerated)
                .arg(
                    hasLegalVacating
                        ? Coordinator::traceBundleKind(
                              bestKind)
                        : QStringLiteral("NONE"))
                .arg(
                    hasLegalVacating
                        ? Coordinator::traceTile(
                              bestDestination)
                        : Coordinator::traceTile(
                              Coordinator::INVALID_TILE))
                .arg(
                    hasLegalVacating
                        ? QString::number(
                              static_cast<std::int32_t>(
                                  bestValue.terminal))
                        : QStringLiteral("NA"))
                .arg(
                    hasLegalVacating
                        ? QString::number(
                              bestValue.economicValue)
                        : QStringLiteral("NA"))
                .arg(Coordinator::traceBool(
                    equalValue)));
    }
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
    const Coordinator::PlannedAction original =
        m_assignment.plan.action(actionIndex);
    const Coordinator::AssignmentResult::Selection*
        pSelection = selectionForAction(actionIndex);
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("EXECUTION_ATTEMPT"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 candidate=%3 kind=%4 actionId=%5 path=%6 destination=%7 target=%8 targetUnit=%9")
                .arg(original.unitId)
                .arg(actionIndex)
                .arg(
                    pSelection == nullptr
                        ? Coordinator::NO_CANDIDATE
                        : pSelection->candidateIndex)
                .arg(Coordinator::traceBundleKind(
                    original.kind))
                .arg(original.actionId)
                .arg(Coordinator::tracePath(
                    original.path))
                .arg(Coordinator::traceTile(
                    original.destination))
                .arg(Coordinator::traceTile(
                    original.target))
                .arg(original.targetUnitId));
    }
    Coordinator::EngineActionBuildResult result =
        Coordinator::buildEngineAction(
            *m_pMap,
            *m_pPlayer,
            m_assignment.plan.action(actionIndex));
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("EXECUTION_BUILD"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 attempt=INITIAL success=%3 failure=%4")
                .arg(original.unitId)
                .arg(actionIndex)
                .arg(Coordinator::traceBool(
                    static_cast<bool>(result)))
                .arg(engineActionFailureName(
                    result.failure)));
    }
    if (!result)
    {
        if (!replanAllowed)
        {
            failPlannedAction(actionIndex);
            if (m_decisionTrace != nullptr)
            {
                m_decisionTrace->record(
                    QStringLiteral("EXECUTION_RESULT"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 success=false reason=REPLAN_ALREADY_USED")
                        .arg(original.unitId)
                        .arg(actionIndex));
                m_decisionTrace->flush();
            }
            return false;
        }
        replanAllowed = false;
        if (!replanFailedAction(actionIndex))
        {
            failPlannedAction(actionIndex);
            if (m_decisionTrace != nullptr)
            {
                m_decisionTrace->record(
                    QStringLiteral("EXECUTION_RESULT"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 success=false reason=REPLAN_FAILED")
                        .arg(original.unitId)
                        .arg(actionIndex));
                m_decisionTrace->flush();
            }
            return false;
        }
        result = Coordinator::buildEngineAction(
            *m_pMap,
            *m_pPlayer,
            m_assignment.plan.action(actionIndex));
        if (m_decisionTrace != nullptr)
        {
            m_decisionTrace->record(
                QStringLiteral("EXECUTION_BUILD"),
                QStringLiteral(
                    "actor=%1 actionIndex=%2 attempt=REPLAN success=%3 failure=%4")
                    .arg(original.unitId)
                    .arg(actionIndex)
                    .arg(Coordinator::traceBool(
                        static_cast<bool>(result)))
                    .arg(engineActionFailureName(
                        result.failure)));
        }
        if (!result)
        {
            failPlannedAction(actionIndex);
            if (m_decisionTrace != nullptr)
            {
                m_decisionTrace->record(
                    QStringLiteral("EXECUTION_RESULT"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 success=false reason=REPLAN_BUILD_FAILED")
                        .arg(original.unitId)
                        .arg(actionIndex));
                m_decisionTrace->flush();
            }
            return false;
        }
    }
    const bool committed =
        m_assignment.plan.commit(actionIndex);
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("EXECUTION_COMMIT"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 success=%3")
                .arg(original.unitId)
                .arg(actionIndex)
                .arg(Coordinator::traceBool(committed)));
    }
    if (!committed)
    {
        failPlannedAction(actionIndex);
        if (m_decisionTrace != nullptr)
        {
            m_decisionTrace->record(
                QStringLiteral("EXECUTION_RESULT"),
                QStringLiteral(
                    "actor=%1 actionIndex=%2 success=false reason=COMMIT_FAILED")
                    .arg(original.unitId)
                    .arg(actionIndex));
            m_decisionTrace->flush();
        }
        return false;
    }
    if (m_decisionTrace != nullptr)
    {
        const Coordinator::PlannedAction & finalAction =
            m_assignment.plan.action(actionIndex);
        m_decisionTrace->record(
            QStringLiteral("EXECUTION_RESULT"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 success=true finalKind=%3 finalActionId=%4 finalDestination=%5 finalTarget=%6")
                .arg(finalAction.unitId)
                .arg(actionIndex)
                .arg(Coordinator::traceBundleKind(
                    finalAction.kind))
                .arg(finalAction.actionId)
                .arg(Coordinator::traceTile(
                    finalAction.destination))
                .arg(Coordinator::traceTile(
                    finalAction.target)));
        m_decisionTrace->flush();
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
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("REPLAN_BEGIN"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 originalKind=%3 originalActionId=%4 originalPath=%5 originalDestination=%6 originalTarget=%7")
                .arg(previous.unitId)
                .arg(actionIndex)
                .arg(Coordinator::traceBundleKind(
                    previous.kind))
                .arg(previous.actionId)
                .arg(Coordinator::tracePath(
                    previous.path))
                .arg(Coordinator::traceTile(
                    previous.destination))
                .arg(Coordinator::traceTile(
                    previous.target)));
    }
    const qint32 engineUnitId =
        previous.unitId;
    Unit* pUnit = m_pMap->getUnit(engineUnitId);
    if (pUnit == nullptr ||
        pUnit->getOwner() != m_pPlayer ||
        pUnit->getHasMoved())
    {
        if (m_decisionTrace != nullptr)
        {
            m_decisionTrace->record(
                QStringLiteral("REPLAN_RESULT"),
                QStringLiteral(
                    "actor=%1 actionIndex=%2 success=false reason=ACTOR_UNAVAILABLE")
                    .arg(engineUnitId)
                    .arg(actionIndex));
        }
        return false;
    }
    const Coordinator::BattlefieldKnowledge knowledge =
        Coordinator::BattlefieldKnowledge::capture(
            *m_pMap, *m_pPlayer);
    const qint32 actorUnitIndex = knowledge.unitIndexAt(
        pUnit->Unit::getX(), pUnit->Unit::getY());
    if (actorUnitIndex == Coordinator::NO_UNIT)
    {
        if (m_decisionTrace != nullptr)
        {
            m_decisionTrace->record(
                QStringLiteral("REPLAN_RESULT"),
                QStringLiteral(
                    "actor=%1 actionIndex=%2 success=false reason=ACTOR_NOT_IN_FRESH_KNOWLEDGE")
                    .arg(engineUnitId)
                    .arg(actionIndex));
        }
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
    if (m_decisionTrace != nullptr)
    {
        m_decisionTrace->record(
            QStringLiteral("REPLAN_CANDIDATE_SET"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 actorKnowledge=%3 candidates=%4 generated=%5 invalid=%6")
                .arg(engineUnitId)
                .arg(actionIndex)
                .arg(actorUnitIndex)
                .arg(candidates.size())
                .arg(buildStats.candidateCount)
                .arg(buildStats.invalidCount));
    }
    const Coordinator::PlanActionIds actionIds =
        Coordinator::engineActionIds();
    const std::vector<Coordinator::KnownUnitLink> unitLinks =
        linkKnownUnits(knowledge);
    for (std::size_t candidateIndex = 0;
         candidateIndex < candidates.size();
         ++candidateIndex)
    {
        const Coordinator::CandidateBundle & candidate =
            candidates[candidateIndex];
        if (!Coordinator::isPlannableCandidate(
                actionIds, unitLinks, candidate))
        {
            if (m_decisionTrace != nullptr &&
                m_decisionTrace
                    ->candidateDetailsEnabled())
            {
                m_decisionTrace->record(
                    QStringLiteral("REPLAN_SAME_INTENT"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 candidate=%3 plannable=false sameIntent=false claim=NOT_ATTEMPTED selected=false")
                        .arg(engineUnitId)
                        .arg(actionIndex)
                        .arg(candidateIndex));
            }
            continue;
        }
        const Coordinator::PlannedAction fresh =
            Coordinator::plannedActionFrom(
                actionIds,
                unitLinks,
                engineUnitId,
                candidate);
        const bool sameIntent =
            sameExecutionIntent(previous, fresh);
        if (m_decisionTrace != nullptr &&
            m_decisionTrace
                ->candidateDetailsEnabled() &&
            !sameIntent)
        {
            m_decisionTrace->record(
                QStringLiteral("REPLAN_SAME_INTENT"),
                QStringLiteral(
                    "actor=%1 actionIndex=%2 candidate=%3 plannable=true sameIntent=false claim=NOT_ATTEMPTED selected=false")
                    .arg(engineUnitId)
                    .arg(actionIndex)
                    .arg(candidateIndex));
        }
        if (sameIntent)
        {
            const Coordinator::ReservationResult claim =
                Coordinator::installCandidate(
                    m_assignment.plan,
                    actionIndex,
                    fresh,
                    candidate);
            if (m_decisionTrace != nullptr)
            {
                m_decisionTrace->record(
                    QStringLiteral("REPLAN_SAME_INTENT"),
                    QStringLiteral(
                        "actor=%1 actionIndex=%2 candidate=%3 generated=%4 plannable=true sameIntent=true claim=%5 selected=%6")
                        .arg(engineUnitId)
                        .arg(actionIndex)
                        .arg(candidateIndex)
                        .arg(candidate.generationIndex)
                        .arg(Coordinator::
                                 traceReservationResult(
                                     claim))
                        .arg(Coordinator::traceBool(
                            claim ==
                            Coordinator::
                                ReservationResult::
                                    Granted)));
            }
            if (claim ==
                Coordinator::ReservationResult::Granted)
            {
                if (m_decisionTrace != nullptr)
                {
                    m_decisionTrace->record(
                        QStringLiteral("REPLAN_RESULT"),
                        QStringLiteral(
                            "actor=%1 actionIndex=%2 success=true mode=SAME_INTENT candidate=%3 finalKind=%4 finalDestination=%5")
                            .arg(engineUnitId)
                            .arg(actionIndex)
                            .arg(candidateIndex)
                            .arg(Coordinator::traceBundleKind(
                                fresh.kind))
                            .arg(Coordinator::traceTile(
                                fresh.destination)));
                }
                return true;
            }
            break;
        }
    }
    Coordinator::AssignmentStats assignmentStats;
    std::int32_t selectedCandidate =
        Coordinator::NO_CANDIDATE;
    const bool replanned = Coordinator::replanAction(
        m_assignment.plan,
        actionIndex,
        actionIds,
        unitLinks,
        candidates,
        assignmentStats,
        m_decisionTrace.get(),
        &selectedCandidate);
    if (m_decisionTrace != nullptr)
    {
        const Coordinator::PlannedAction & replacement =
            m_assignment.plan.action(actionIndex);
        m_decisionTrace->record(
            QStringLiteral("REPLAN_RESULT"),
            QStringLiteral(
                "actor=%1 actionIndex=%2 success=%3 mode=GENERIC candidate=%4 finalKind=%5 finalActionId=%6 finalDestination=%7")
                .arg(engineUnitId)
                .arg(actionIndex)
                .arg(Coordinator::traceBool(replanned))
                .arg(selectedCandidate)
                .arg(Coordinator::traceBundleKind(
                    replacement.kind))
                .arg(replacement.actionId)
                .arg(Coordinator::traceTile(
                    replacement.destination)));
    }
    return replanned;
}
