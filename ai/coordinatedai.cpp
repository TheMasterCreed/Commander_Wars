#include "ai/coordinatedai.h"
#include "ai/coordinator/combatmilestone.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QJsonDocument>

#include "ai/coordinator/attackopportunitybuilder.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/capturedangeraudit.h"
#include "ai/coordinator/decisiontrace.h"
#include "ai/coordinator/headquarterstransit.h"
#include "game/building.h"
#include "game/unitpathfindingsystem.h"
#include "ai/coordinator/engineactionbuilder.h"
#include "ai/coordinator/propertyeconomicsbuilder.h"
#include "ai/coordinator/refinementledger.h"
#include "ai/coordinator/replyfactsbuilder.h"
#include "ai/coordinator/replypassrecord.h"
#include "ai/coordinator/masterassignmentcontrol.h"
#include "ai/coordinator/masterassignmentshadow.h"
#include "ai/coordinator/masterassignmenttrace.h"
#include "ai/coordinator/unifiedproductionlive.h"
#include "ai/coordinator/unifiedspecialactions.h"

#include "coreengine/globalutils.h"
#include "coreengine/interpreter.h"
#include "coreengine/mainapp.h"
#include "coreengine/memorymanagement.h"
#include "coreengine/qmlvector.h"
#include "coreengine/settings.h"

#include <QRandomGenerator>
#include <QThread>

#include "game/actionperformer.h"
#include "game/co.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/unit.h"
#include "gameinput/markedfielddata.h"
#include "menue/gamemenue.h"
#include "objects/dialogs/dialogmessagebox.h"

#ifdef COW_COORDINATED_REFINEMENT_TESTING
#include "ai/coordinator/factoryprefixcommands.h"
#include "tests/arena/masterassignmentprobe.h"
#include "tests/arena/mastercomparisonprobe.h"
#endif

namespace
{
constexpr std::chrono::milliseconds COORDINATED_UI_PUMP_INTERVAL{8};
constexpr std::uint32_t PUMP_CHECK_MASK = 7;

// Keeps Qt/Oxygine responsive while CoordinatedAi thinks in-process, matching NormalAi.
void pumpCoordinatedUiEvents()
{
    thread_local std::uint32_t pumpCheckCalls = 0;
    if ((++pumpCheckCalls & PUMP_CHECK_MASK) != 0)
    {
        return;
    }
    thread_local auto lastPump = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPump < COORDINATED_UI_PUMP_INTERVAL)
    {
        return;
    }
    lastPump = now;
    Mainapp * pApp = Mainapp::getInstance();
    Interpreter * pInterpreter = Interpreter::getInstance();
    if (pApp != nullptr && !pApp->getNoUi() && pInterpreter != nullptr &&
        pInterpreter->thread() == QThread::currentThread())
    {
        pInterpreter->threadProcessEvents();
    }
}

std::int64_t planningNanosSince(std::chrono::steady_clock::time_point start, bool timed)
{
    if (!timed)
    {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
}

QString optionalPlanningValue(std::optional<std::int64_t> value)
{
    if (!value)
    {
        return QStringLiteral("unavailable");
    }
    return QString::number(*value);
}

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

std::int64_t combatShare(std::int64_t total, const Coordinator::AssignmentInput &input,
                         const Coordinator::BattlefieldKnowledge &knowledge)
{
    const auto ordinary = total / 4;
    const auto maximum = total / 2;
    bool contact = false;
    std::int64_t candidates = 0;
    for (const auto &actor : input.actors)
    {
        candidates += static_cast<std::int64_t>(actor.candidates.size());
        if (contact) continue;
        for (const auto &choice : actor.candidates)
        {
            const auto *part = Coordinator::singleComponentOf(choice.bundle);
            if ((part && part->kind == Coordinator::ComponentKind::Fire) ||
                (choice.replyFacts && choice.replyFacts->tile && !choice.replyFacts->tile->shooters.empty()))
            {
                contact = true;
                break;
            }
        }
    }
    if (!contact || candidates == 0 || maximum <= 0) return ordinary;
    // Allow for both sides' movement on the incumbent and a complete challenger.
    // A crowded turn can shift work from stock search, but cannot exceed the
    // configured total or take more than half away from the stock solver.
    std::int64_t estimate = 4;
    const std::array<std::int64_t,3> factors{static_cast<std::int64_t>(knowledge.width())*knowledge.height(),
        static_cast<std::int64_t>(input.actors.size()),candidates};
    for (const auto factor : factors)
    {
        if (factor<=0) return ordinary;
        if (estimate > maximum / factor) return maximum;
        estimate *= factor;
    }
    return std::clamp(estimate,ordinary,maximum);
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
        case Coordinator::AssignmentResult::SelectionPhase::Master:
            return QStringLiteral("MASTER_CERTIFIED_PLAN_BOUNDS");
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
        case Coordinator::AssignmentResult::SelectionPhase::Master:
            return QStringLiteral("MASTER");
    }
    return QStringLiteral("UNKNOWN");
}
}

namespace
{
    Coordinator::HeadquartersTransitTiles nearbyHeadquartersTransit(GameMap &map,
        const Coordinator::BattlefieldKnowledge &knowledge, Coordinator::MobilityFieldCache &mobility)
    {
        Coordinator::HeadquartersTransitTiles posts;
        if (!map.getGameRules()->hasVictoryRule("VICTORYRULE_NOHQ")) return posts;
        Coordinator::TilePoint home = Coordinator::INVALID_TILE;
        for (const auto &b : knowledge.buildings())
        {
            if (knowledge.relation(b.ownerId) != Coordinator::Relation::Own) continue;
            auto *site = map.getTerrain(b.x,b.y)->getBuilding();
            if (!site || !site->isHq()) continue;
            if (home != Coordinator::INVALID_TILE) return posts;
            home = {b.x,b.y};
        }
        if (home == Coordinator::INVALID_TILE) return posts;
        for (const auto &u : knowledge.units())
        {
            const Coordinator::TilePoint from{u.x,u.y};
            if (u.carrier != Coordinator::KnownUnit::NO_CARRIER || !u.canCapture || u.ignoresUnitCollision ||
                knowledge.relation(u.ownerId) != Coordinator::Relation::Enemy ||
                Coordinator::manhattanDistance(from,home) > 2LL*u.movementPoints) continue;
            const auto &grid = mobility.grid(map,*map.getPlayer(u.ownerId),u.unitId);
            Coordinator::addHeadquartersTransitTiles(posts,grid,from,home,u.movementPoints);
        }
        return posts;
    }

    bool canChangeHeadquartersPost(GameMap &map, const Coordinator::BattlefieldKnowledge &knowledge,
        const Coordinator::KnownUnit &known, const Coordinator::HeadquartersTransitTiles &posts)
    {
        if (posts.empty() || known.hasMoved || known.canFire || known.canCapture ||
            known.carrier != Coordinator::KnownUnit::NO_CARRIER || knowledge.relation(known.ownerId) != Coordinator::Relation::Own)
            return false;
        auto *unit = Coordinator::liveUnitFor(map,known);
        if (!unit || unit->getHasMoved() || unit->getAiMode() != GameEnums::GameAi_Normal) return false;
        const bool onPost = posts.contains({known.x,known.y});
        UnitPathFindingSystem paths(&map,unit,unit->getOwner());
        paths.explore();
        for (const QPoint &to : paths.getAllNodePointsFast())
        {
            if ((to.x()==known.x && to.y()==known.y) || map.getTerrain(to.x(),to.y())->getUnit()) continue;
            if (onPost || posts.contains({to.x(),to.y()})) return true;
        }
        return false;
    }
}

CoordinatedAi::CoordinatedAi(GameMap* pMap)
    : NormalAi(
          pMap,
          NormalAi::DEFAULT_INI_FILE,
          GameEnums::AiTypes_Coordinated,
          NormalAi::DEFAULT_JS_NAME,
          ScriptBinding::Deferred)
{
#ifdef GRAPHICSUPPORT
    setObjectName("CoordinatedAi");
#endif
    Coordinator::uiEventPumpHook() = &pumpCoordinatedUiEvents;
    setupJsThis(this);
}

void CoordinatedAi::process()
{
    if (holdForPause())
    {
        return;
    }
    if (m_pMenu != nullptr &&
        ((m_pMenu->getActionPerformer() != nullptr && m_pMenu->getActionPerformer()->getActionRunning()) ||
         GameAnimationFactory::getAnimationCount() > 0))
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
                m_coPowerCheckNanos +=
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
        if (unifiedSpecialActionsActive() &&
            m_planDay != m_pMap->getCurrentDay() &&
            tryPrePlanSpecialActions())
        {
            m_factLayersDay = UNBUILT_DAY;
            return;
        }
        ensureTurnPlan();
    }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (m_testStockReferenceStopped)
    {
        return;
    }
#endif
    if (m_assignment.stockConstructionStatus != Coordinator::StockConstructionStatus::NotRequested &&
        m_assignment.stockConstructionStatus != Coordinator::StockConstructionStatus::Complete)
    {
        return;
    }
    if (executeNextPlannedAction())
    {
        return;
    }
    if (unifiedSpecialActionsActive() && tryPostPlanSpecialActions())
    {
        return;
    }
    if (m_directUnifiedTurnActive)
    {
        spQmlVectorBuilding pBuildings = m_pPlayer->getSpBuildings();
        spQmlVectorUnit pUnits = m_pPlayer->getSpUnits();
        if (useBuildingActions(pBuildings, pUnits))
        {
            return;
        }
        spQmlVectorUnit pEnemyUnits;
        spQmlVectorBuilding pEnemyBuildings;
        prepareEnemieData(pUnits, pBuildings, pEnemyUnits, pEnemyBuildings);
        m_turnMode = GameEnums::AiTurnMode_DuringDay;
        if (buildUnits(pBuildings, pUnits, pEnemyUnits, pEnemyBuildings))
        {
            return;
        }
        m_turnMode = GameEnums::AiTurnMode_EndOfDay;
        if (useCOPower(pUnits, pEnemyUnits))
        {
            m_turnMode = GameEnums::AiTurnMode_DuringDay;
            return;
        }
        m_turnMode = GameEnums::AiTurnMode_StartOfDay;
        finishTurn();
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
    Coordinator::pumpUiEventsIfDue();
    // MobilityFieldCache validates MobilityEpoch on lookup and invalidates on terrain, weather, CO, or owner changes.
    m_attackOpportunities =
        Coordinator::buildAttackOpportunityField(
            *m_pMap, m_dayStartKnowledge, m_mobilityFields);
    Coordinator::pumpUiEventsIfDue();
    m_properties =
        Coordinator::buildPropertyEconomics(*m_pMap, m_dayStartKnowledge);
    Coordinator::pumpUiEventsIfDue();
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
        m_propertyStockBuildNanos +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                factEnd - propertyStockStart)
                .count();
        m_factLayersNanos +=
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
    const bool timed = Coordinator::planningTimingAuditEnabled() || Coordinator::decisionTraceEnabled();
    const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto preparationNanos = m_factLayersNanos + m_coPowerCheckNanos;
    buildTurnPlan();
    m_planDay = m_pMap->getCurrentDay();
    if (timed)
    {
        const auto nanos =
            preparationNanos + planningNanosSince(started, true) - m_captureDangerAuditNanos - m_replyPassNanos;
        const Coordinator::DecisionTraceIdentity identity{
            .day = m_planDay, .playerId = m_pPlayer->getPlayerID(),
            .planSequence = m_planSequence, .horizonTurns = m_propertyStock.horizonTurns()};
        const auto fields = QStringLiteral("planningCallNanos=%1 planSequence=%2").arg(nanos).arg(m_planSequence);
        if (m_decisionTrace)
        {
            m_decisionTrace->record(QStringLiteral("PLANNING_CALL"), fields);
            m_decisionTrace->flush();
        }
        Coordinator::writePlanningTimingAudit(identity, fields, Coordinator::PlanningTimingKind::Call);
    }
}

void CoordinatedAi::stopStockControl(const Coordinator::MasterShadowConfig & config, const QString & reason)
{
    qCritical() << "StockControl stopped before plan execution:" << reason;
    if (m_pMenu != nullptr)
    {
        m_pMenu->setGameStarted(false);
    }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (config.runtime == Coordinator::MasterStockControlRuntime::Arena)
    {
        ArenaMasterAssignmentProbe::failStockControlSetup(reason);
        emit sigStockControlStopped(reason);
        return;
    }
#endif
    if (m_pMenu != nullptr && !Mainapp::getInstance()->getNoUi())
    {
        auto dialog = MemoryManagement::create<DialogMessageBox>(
            tr("StockControl could not construct a complete legal plan. This game has been stopped without "
               "executing a partial plan or switching optimizers. You can save the game and exit.\n\n%1").arg(reason));
        m_pMenu->addChild(dialog);
    }
    emit sigStockControlStopped(reason);
}

bool CoordinatedAi::playStockControlAvailable() const
{
    auto* settings = Settings::getInstance();
    if (!settings->getCoordinatedAiStockControl() || settings->getSpawnAiProcess() || settings->getAiSlave() ||
        Mainapp::getSlave())
    {
        return false;
    }
    return localSessionMenu();
}

bool CoordinatedAi::localSessionMenu() const
{
    auto* menu = dynamic_cast<GameMenue*>(m_pMenu);
    return menu != nullptr && !menu->isNetworkGame() && !menu->getIsReplay();
}

#ifdef COW_COORDINATED_REFINEMENT_TESTING
void CoordinatedAi::stopStockReference(const QString & reason)
{
    m_testStockReferenceStopped = true;
    if (m_pMenu != nullptr)
    {
        m_pMenu->setGameStarted(false);
    }
    emit sigStockControlStopped(reason);
}
#endif

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
    m_captureDangerAuditNanos = 0;
    m_replyPassNanos = 0;
    m_executionCursor = 0;
    m_teamSelection = TeamSelection::Ordinary;
    m_planHasOmniscientPostKill = false;
    m_expectedFireResults.clear();
    m_pendingExpectedFire.reset();
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
    const auto factLayersNanos = std::exchange(m_factLayersNanos, 0);
    const auto propertyStockBuildNanos = std::exchange(m_propertyStockBuildNanos, 0);
    const auto coPowerCheckNanos = std::exchange(m_coPowerCheckNanos, 0);
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
    const auto combat = Coordinator::combatMode();
    bool legacyHarnessRequested =
        qgetenv("COW_AI_ARENA_TEST") == "1" ||
        qgetenv("COW_COMBAT_EXECUTE_FIXED_ORDERS") == "1" ||
        qgetenv("COW_COORDINATED_LEGACY_TRIALS") == "1" ||
        !localSessionMenu();
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (m_testRefinementObserver != nullptr || m_testStockControlHeld || ArenaMasterAssignmentProbe::enabled())
    {
        legacyHarnessRequested = true;
    }
#endif
    const bool directUnifiedActing = (combat == Coordinator::CombatMode::Acting) && !legacyHarnessRequested;
    m_directUnifiedTurnActive = directUnifiedActing;

    auto masterShadow = Coordinator::readMasterShadowConfig();
    if (!directUnifiedActing &&
        masterShadow.mode == Coordinator::MasterAssignmentMode::Disabled &&
        masterShadow.error == Coordinator::MasterShadowConfigError::None &&
        masterShadow.runtime == Coordinator::MasterStockControlRuntime::None && playStockControlAvailable())
    {
        masterShadow = Coordinator::playMasterStockControlConfig();
        if (combat != Coordinator::CombatMode::Off)
        {
            masterShadow.replyPrediction = Coordinator::ReplyPrediction::Recorded;
            masterShadow.replyPass = Coordinator::ReplyPassMode::Recorded;
            masterShadow.captureDangerAudit = Coordinator::CaptureDangerAudit::Recorded;
            masterShadow.exportMode = Coordinator::MasterShadowExportMode::Full;
            masterShadow.outputPath = Settings::getInstance()->getUserPath() + QStringLiteral("master-stock-control.jsonl");
        }
    }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (m_testStockControlHeld && masterShadow.controlsStock())
    {
        masterShadow.mode = Coordinator::MasterAssignmentMode::Disabled;
    }
#endif
    if (!directUnifiedActing &&
        (masterShadow.mode == Coordinator::MasterAssignmentMode::StockControl ||
         (masterShadow.runtime == Coordinator::MasterStockControlRuntime::PlayableTrial &&
          masterShadow.error != Coordinator::MasterShadowConfigError::None)))
    {
        const bool unsupportedSession = masterShadow.runtime == Coordinator::MasterStockControlRuntime::PlayableTrial &&
            !localSessionMenu();
        if (masterShadow.error != Coordinator::MasterShadowConfigError::None || unsupportedSession)
        {
            m_assignment = Coordinator::AssignmentResult{};
            m_assignment.stockConstructionStatus = Coordinator::StockConstructionStatus::InvalidInput;
            stopStockControl(masterShadow, unsupportedSession
                ? tr("The playable trial supports local single-player sessions only.")
                : tr("Invalid StockControl configuration (%1).").arg(static_cast<int>(masterShadow.error)));
            return;
        }
    }
    Coordinator::AssignmentInput input;
    input.actionIds = Coordinator::engineActionIds();
    input.unitLinks = linkKnownUnits(m_dayStartKnowledge);
    auto tierAudit = Coordinator::SequentialTierAuditMode::Full;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    tierAudit = m_testTierAuditMode;
#endif
    std::optional<Coordinator::JointPlanStockValuer> stockValuer;
    stockValuer.emplace(m_propertyStock, input.unitLinks, tierAudit);
    input.pStockValuer = &*stockValuer;
    input.pTrace = m_decisionTrace.get();
    input.measureTimings = timed;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    input.retainSelections = ArenaMasterAssignmentProbe::enabled();
#endif
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    input.relativeStockWorkLimit = m_testRefinementWorkLimit;
    if (m_testRefinementScheduleDay < 0 || traceIdentity.day == m_testRefinementScheduleDay)
    {
        input.relativeStockSchedule = m_testRefinementSchedule;
    }
    input.pRelativeStockObserver = m_testRefinementObserver.get();
    if (m_testRefinementObserver && !masterShadow.controlsStock())
    {
        m_testRefinementObserver->beginPlan({
            traceIdentity.day, traceIdentity.playerId, traceIdentity.planSequence, traceIdentity.horizonTurns,
        });
    }
#endif
    Coordinator::DamageOracle oracle(*m_pMap);
    oracle.clear();
    if (directUnifiedActing)
    {
        if (m_turnLuckDay != m_pMap->getCurrentDay())
        {
            m_turnLuckDay = m_pMap->getCurrentDay();
            if (GlobalUtils::getUseSeed())
            {
                m_turnLuckNonce = Coordinator::computePeekedShotSeed(
                    GlobalUtils::getSeed(), m_pPlayer->getPlayerID(),
                    Coordinator::TilePoint{m_turnLuckDay, m_pMap->getMapWidth()},
                    Coordinator::TilePoint{m_pMap->getMapHeight(), static_cast<qint32>(m_properties.size())},
                    0.0, 0.0);
            }
            else
            {
                m_turnLuckNonce = QRandomGenerator::global()->bounded(1u, std::numeric_limits<quint32>::max());
            }
        }
        oracle.setLuckPeek(m_turnLuckNonce, m_pPlayer->getPlayerID());
    }
    // A separate oracle keeps reply facts from warming the planner's damage memo.
    std::optional<Coordinator::DamageOracle> replyOracle;
    std::optional<Coordinator::ReplyFactsBuilder> replyFacts;
    std::shared_ptr<const Coordinator::ReplyBoard> replyBoard;
    if (!directUnifiedActing && combat != Coordinator::CombatMode::Off && masterShadow.controlsStock() && localSessionMenu())
    {
        masterShadow.replyPrediction = Coordinator::ReplyPrediction::Recorded;
        masterShadow.replyPass = Coordinator::ReplyPassMode::Recorded;
    }
    const bool predictionRecorded = directUnifiedActing || masterShadow.replyPrediction == Coordinator::ReplyPrediction::Recorded;
    // Asked only when a reply switch is on, so the Off path makes no call it did not make before.
    const bool localReplySession =
        (directUnifiedActing || predictionRecorded || masterShadow.replyPass == Coordinator::ReplyPassMode::Recorded) &&
        localSessionMenu();
    const bool replyRecorded = predictionRecorded && localReplySession;
    const auto replyPassWiring = directUnifiedActing
        ? Coordinator::ReplyPassWiring{true, false, false}
        : Coordinator::replyPassWiring(masterShadow.replyPrediction, masterShadow.replyPass, localReplySession);
    if (replyPassWiring.replyFacts)
    {
        replyOracle.emplace(*m_pMap);
        replyOracle->clear();
        replyFacts.emplace(*m_pMap, m_dayStartKnowledge, m_attackOpportunities, m_properties, m_propertyStock,
                           *replyOracle);
        replyBoard = std::make_shared<const Coordinator::ReplyBoard>(Coordinator::replyBoardOf(m_dayStartKnowledge));
    }
    if (replyRecorded)
    {
        input.replyPrediction = {
            .mode = Coordinator::ReplyPrediction::Recorded,
            .board = replyBoard,
            .tally = std::make_shared<Coordinator::ReplyPredictionTally>(),
        };
    }
    Coordinator::BundleBuildStats buildStats;
    Coordinator::BundleBuildCache buildCache;
    std::vector<ActorTiming> actorTimings;
    std::int64_t candidateBuildNanos = 0;
    std::int32_t maxCandidatesPerActor = 0;
    const std::vector<Coordinator::KnownUnit> & units =
        m_dayStartKnowledge.units();
    // Follow tiles must be populated for both stock-control and direct unified acting so vacate chains generate.
    const bool stockControl = !directUnifiedActing && masterShadow.controlsStock();
    const bool buildFollowTiles = directUnifiedActing || stockControl;
    std::vector<Unit*> plannableUnits;
    std::vector<Coordinator::TilePoint> plannableTiles;
    if (buildFollowTiles)
    {
        Coordinator::HeadquartersTransitTiles transit;
        if (!directUnifiedActing)
        {
            const bool passiveAvailable = std::any_of(units.begin(),units.end(),[&](const auto &u) {
                return !u.hasMoved && !u.canFire && !u.canCapture && u.carrier==Coordinator::KnownUnit::NO_CARRIER &&
                       m_dayStartKnowledge.relation(u.ownerId)==Coordinator::Relation::Own;
            });
            if (passiveAvailable && combat != Coordinator::CombatMode::Off && qgetenv("COW_COORDINATED_OPPONENT_CAPTURE") != "0")
                transit = nearbyHeadquartersTransit(*m_pMap,m_dayStartKnowledge,m_mobilityFields);
        }
        plannableUnits.reserve(units.size());
        for (const Coordinator::KnownUnit & known : units)
        {
            const bool allowHqChange = directUnifiedActing || canChangeHeadquartersPost(*m_pMap,m_dayStartKnowledge,known,transit);
            Unit* pUnit = plannableUnit(known, allowHqChange);
            plannableUnits.push_back(pUnit);
            if (pUnit != nullptr)
            {
                plannableTiles.push_back(Coordinator::TilePoint{known.x, known.y});
            }
        }
    }
    const std::vector<Coordinator::TilePoint> followTiles =
        Coordinator::followTilesFor(buildFollowTiles, std::move(plannableTiles));
    for (std::size_t slot = 0; slot < units.size(); ++slot)
    {
        Coordinator::pumpUiEventsIfDue();
        Unit* pUnit = buildFollowTiles ? plannableUnits[slot] : plannableUnit(units[slot], directUnifiedActing);
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
            buildStats,
            replyFacts ? &*replyFacts : nullptr,
            followTiles,
            &actor.todayCandidates,
            &buildCache);
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

    // Stage 2: append 1-pass breakthrough candidates across enemies killable this turn.
    if (directUnifiedActing && !input.actors.empty())
    {
        std::vector<std::int32_t> maxDmgByEnemyKi(units.size(), 0);
        for (const Coordinator::AssignmentActor & actor : input.actors)
        {
            std::map<std::int32_t, std::int32_t> actorBestByEnemy;
            for (const Coordinator::CandidateBundle & cand : actor.candidates)
            {
                if (!cand.valuation.valid || cand.bundle.components.size() != 1) continue;
                const auto & comp = cand.bundle.components.front();
                if (comp.kind != Coordinator::ComponentKind::Fire ||
                    Coordinator::isTerrainTargetId(comp.fire.targetUnitId))
                    continue;
                const std::int32_t eKi = comp.fire.targetUnitId;
                if (eKi < 0 || eKi >= static_cast<std::int32_t>(units.size())) continue;
                auto & best = actorBestByEnemy[eKi];
                best = std::max(best, comp.fire.damageSteps);
            }
            for (const auto & [eKi, dmg] : actorBestByEnemy)
                maxDmgByEnemyKi[static_cast<std::size_t>(eKi)] += dmg;
        }
        std::vector<Coordinator::KillableEnemyTarget> killableEnemies;
        for (std::size_t eKi = 0; eKi < units.size(); ++eKi)
        {
            if (maxDmgByEnemyKi[eKi] < units[eKi].hpRounded) continue;
            if (eKi >= input.unitLinks.size()) continue;
            const auto & link = input.unitLinks[eKi];
            if (link.engineUnitId == Coordinator::NO_UNIT || link.tile == Coordinator::INVALID_TILE) continue;
            killableEnemies.push_back({static_cast<std::int32_t>(eKi), link.engineUnitId, link.tile});
        }
        if (!killableEnemies.empty())
        {
            const Coordinator::BundleBuildContext breakthroughCtx{
                .map = *m_pMap,
                .player = *m_pPlayer,
                .knowledge = m_dayStartKnowledge,
                .enemyReach = m_attackOpportunities,
                .properties = m_properties,
                .mobility = m_mobilityFields,
                .propertyStock = m_propertyStock,
                .oracle = oracle,
                .valuation = Coordinator::makeValuationContext(*m_pMap),
                .pTrace = m_decisionTrace.get(),
                .pReplyFacts = replyFacts ? &*replyFacts : nullptr,
                .followTiles = followTiles,
                .pCache = &buildCache,
            };
            for (Coordinator::AssignmentActor & actor : input.actors)
            {
                Clock::time_point btStart;
                if (timed) btStart = Clock::now();
                const std::int32_t btBefore = buildStats.candidateCount;
                Coordinator::appendKillBreakthroughCandidates(
                    breakthroughCtx, killableEnemies, actor.knowledgeUnitIndex,
                    actor.candidates, &actor.todayCandidates, buildStats);
                if (timed)
                {
                    candidateBuildNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - btStart).count();
                    maxCandidatesPerActor = std::max<std::int32_t>(
                        maxCandidatesPerActor, buildStats.candidateCount - btBefore);
                }
            }
        }
    }

    std::int32_t stockCoupledActors = 0;
    if (timed)
    {
        for (const Coordinator::AssignmentActor & actor :
             input.actors)
        {
            if (stockValuer->affectsStock(
                    actor.engineUnitId))
            {
                ++stockCoupledActors;
            }
        }
    }
    Coordinator::RefinementLedger ledger;
    Coordinator::RefinementLedger masterLedger;
    Coordinator::MasterShadowTiming masterTiming;
    Coordinator::MasterStockControlTiming controlTiming;
    Coordinator::MasterPlanHandoffResult planHandoff;
    std::int64_t stockControlCallNanos = 0;
    std::int64_t continuationPricingPrepareNanos = 0;
    std::int64_t legacyPreparationNanos = 0;
    std::int64_t masterPreparationNanos = 0;
    std::int64_t legacyAssignmentNanos = 0;
    std::int64_t arenaProbeNanos = 0;
    std::int64_t combatCompareNanos = 0;
    Clock::time_point phaseStart;
    if (directUnifiedActing)
    {
        m_directUnifiedTurnActive = true;
        m_assignment = Coordinator::AssignmentResult{};
        if (input.actors.empty())
        {
            m_teamSelection = TeamSelection::OpenTactical;
            recordCaptureDecisions(input);
            return;
        }
        std::vector<Coordinator::MasterActorSelection> selections;
        selections.reserve(input.actors.size());
        m_assignment.selections.reserve(input.actors.size());
        bool seedValid = true;
        for (const Coordinator::AssignmentActor & actor : input.actors)
        {
            int waitOpt = Coordinator::NO_CANDIDATE;
            for (int opt = 0; opt < static_cast<int>(actor.candidates.size()); ++opt)
            {
                const auto & cand = actor.candidates[static_cast<std::size_t>(opt)];
                if (!Coordinator::isPlannableCandidate(input.actionIds, input.unitLinks, cand) ||
                    Coordinator::isFollowCandidate(actor, static_cast<std::size_t>(opt)))
                    continue;
                if (Coordinator::planBundleKindOf(cand.bundle) == Coordinator::PlanBundleKind::Wait &&
                    cand.bundle.destination == cand.bundle.origin &&
                    cand.bundle.components.empty())
                {
                    waitOpt = opt;
                    break;
                }
                if (waitOpt == Coordinator::NO_CANDIDATE)
                    waitOpt = opt;
            }
            if (waitOpt == Coordinator::NO_CANDIDATE)
            {
                seedValid = false;
                break;
            }
            const auto & cand = actor.candidates[static_cast<std::size_t>(waitOpt)];
            const Coordinator::PlannedAction action = Coordinator::plannedActionFrom(
                input.actionIds, input.unitLinks, actor.engineUnitId, cand);
            const std::int32_t actionIndex = m_assignment.plan.addAction(action);
            if (Coordinator::claimFor(m_assignment.plan, actionIndex, cand) !=
                Coordinator::ReservationResult::Granted)
            {
                seedValid = false;
                break;
            }
            const Coordinator::MilliFunds seated = Coordinator::checkedSeatedCandidateValue(
                cand, m_assignment.plan.action(actionIndex).plannedDamageSteps).value_or(0);
            m_assignment.selections.push_back(Coordinator::AssignmentResult::Selection{
                .knowledgeUnitIndex = actor.knowledgeUnitIndex,
                .engineUnitId = actor.engineUnitId,
                .actionIndex = actionIndex,
                .candidateIndex = waitOpt,
                .greedyCandidateIndex = waitOpt,
                .seatedValue = seated,
                .stockCoupled = stockValuer->affectsStock(actor.engineUnitId),
                .phase = Coordinator::AssignmentResult::SelectionPhase::Master,
            });
            selections.push_back(Coordinator::MasterActorSelection{
                .engineUnitId = actor.engineUnitId,
                .actionIndex = actionIndex,
                .candidateIndex = waitOpt,
                .role = Coordinator::MasterActorRole::Mutable,
                .stockRow = stockValuer->stockRow(actor.engineUnitId),
                .seatedValue = std::nullopt,
            });
        }
        if (seedValid)
        {
            static_cast<void>(m_assignment.plan.rebuildVacateEdges(Coordinator::planOccupancy(m_assignment.plan)));
            m_assignment.executionOrder = m_assignment.plan.executionOrder().order;
            static std::atomic<std::uint64_t> s_unifiedSessionId{1};
            const std::uint64_t sid = s_unifiedSessionId.fetch_add(1, std::memory_order_relaxed);
            const auto built = Coordinator::buildMasterAssignmentProblem(
                input,
                Coordinator::MasterAssignmentBuildRequest{
                    .sessionIdentity = sid,
                    .selections = selections,
                    .pIncumbentPlan = &m_assignment.plan,
                    .incumbentExecutionOrder = m_assignment.executionOrder,
                });
            const auto handoff = Coordinator::handOffMasterPlan(
                built.valid() && built.problem ? &*built.problem : nullptr, m_assignment);
            if (handoff.status == Coordinator::MasterPlanHandoffStatus::Ready && handoff.handoff && replyBoard)
            {
                const auto cfg = Coordinator::playMasterStockControlConfig();
                const auto turnAllowance = Coordinator::masterTurnAllowance(cfg, input.actors.size());
                const auto combatAllowance = combatShare(turnAllowance, input, m_dayStartKnowledge);
                const auto combatStart = Clock::now();
                auto evidence = Coordinator::planUnifiedTurn(
                    traceIdentity, *m_pMap, m_dayStartKnowledge, m_mobilityFields, *handoff.handoff,
                    *replyBoard, m_propertyStock, m_assignment, m_properties, combatAllowance,
                    replyFacts ? &*replyFacts : nullptr, m_turnLuckNonce);
                combatCompareNanos = planningNanosSince(combatStart, true);
                evidence["elapsedNanos"] = static_cast<qint64>(combatCompareNanos);
                evidence["allocatedCombatWork"] = static_cast<qint64>(combatAllowance);
                const auto combatPath =
                    Settings::getInstance()->getUserPath() + QStringLiteral("coordinated-ai-combat.jsonl");
                if (!Coordinator::appendTraceRecords(
                        combatPath, {QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact))}))
                    qWarning() << "Combat comparison record write failed:" << combatPath;
                if (evidence.value("installed").toBool())
                {
                    m_teamSelection = TeamSelection::OpenTactical;
                    m_planHasOmniscientPostKill = directUnifiedActing;
                    const auto selectedTeam =
                        evidence.value("openTacticalHearing").toObject().value("selectedPhysicalTeam").toObject();
                    for (const auto & entry : selectedTeam.value("orders").toArray())
                    {
                        const auto order = entry.toObject();
                        const auto after = order.value("combatAfter").toObject();
                        if (!after.isEmpty())
                        {
                            m_expectedFireResults.emplace(order["actionIndex"].toInt(), ExpectedFireResult{
                                order["unit"].toInt(), order["targetUnit"].toInt(),
                                after["actorHp"].toDouble(), after["targetHp"].toDouble()});
                        }
                    }
                    recordCaptureDecisions(input);
                    if (m_decisionTrace != nullptr)
                    {
                        m_decisionTrace->record(
                            QStringLiteral("BUILD_STATS"),
                            QStringLiteral(
                                "actors=%1 totalCandidates=%2 validCandidates=%3 invalidCandidates=%4 missingUnits=%5 damageOracleCalls=%6 damageOracleHits=%7 maxCandidatesPerActor=%8 stockCoupledActors=%9")
                                .arg(input.actors.size())
                                .arg(buildStats.candidateCount)
                                .arg(buildStats.candidateCount - buildStats.invalidCount)
                                .arg(buildStats.invalidCount)
                                .arg(buildStats.missingUnits)
                                .arg(buildStats.oracleCalls)
                                .arg(buildStats.oracleHits)
                                .arg(maxCandidatesPerActor)
                                .arg(stockCoupledActors));
                        m_decisionTrace->flush();
                    }
                    return;
                }
            }
        }
        const QString errMsg = QStringLiteral("[COORD][ERROR] Direct unified planning failed on day %1 player %2 (actors=%3)")
            .arg(m_pMap->getCurrentDay())
            .arg(m_pPlayer->getPlayerID())
            .arg(input.actors.size());
        CONSOLE_PRINT(errMsg, GameConsole::eERROR);
        const QString errPath = Settings::getInstance()->getUserPath() + QStringLiteral("coordinated-ai-combat.jsonl");
        const QJsonObject errObj{
            {"status", "DIRECT_UNIFIED_PLAN_FAILED"},
            {"day", m_pMap->getCurrentDay()},
            {"player", m_pPlayer->getPlayerID()},
            {"actors", static_cast<qint64>(input.actors.size())},
            {"strict", strictCoordinatedFailureMode()},
        };
        Coordinator::appendTraceRecords(errPath, {QString::fromUtf8(QJsonDocument(errObj).toJson(QJsonDocument::Compact))});
        m_teamSelection = TeamSelection::OpenTactical;
        m_assignment = Coordinator::AssignmentResult{};
        Coordinator::dropFollowCandidates(input);
        if (strictCoordinatedFailureMode())
        {
            constexpr int STRICT_COORD_FAILURE_EXIT_CODE = 4;
            QCoreApplication::exit(STRICT_COORD_FAILURE_EXIT_CODE);
        }
        return;
    }
    const bool combatRequested = !directUnifiedActing && combat != Coordinator::CombatMode::Off && masterShadow.controlsStock() && localSessionMenu();
    const auto turnAllowance = Coordinator::masterTurnAllowance(masterShadow,input.actors.size());
    const auto combatAllowance = combatRequested ? combatShare(turnAllowance,input,m_dayStartKnowledge) : 0;
    if (combatRequested && combat == Coordinator::CombatMode::Acting)
    {
        masterShadow.allowance = turnAllowance - combatAllowance;
        masterShadow.allowancePerActor = 0;
    }
    bool legacyPlanning = directUnifiedActing || !masterShadow.controlsStock();
    if (!directUnifiedActing && masterShadow.controlsStock())
    {
        if (timed)
        {
            phaseStart = Clock::now();
        }
        controlTiming = Coordinator::controlMasterAssignment(
            masterShadow, *m_pMap, input, m_assignment, m_propertyStock, m_properties, traceIdentity,
            replyPassWiring.handoff ? &planHandoff : nullptr,
            combatRequested && combat == Coordinator::CombatMode::Acting &&
                qgetenv("COW_COORDINATED_OPPONENT_CAPTURE") != "0");
        stockControlCallNanos = planningNanosSince(phaseStart, timed);
        if (timed)
        {
            stockControlCallNanos -= planHandoff.nanos;
        }
        if (masterShadow.runtime == Coordinator::MasterStockControlRuntime::Play &&
            controlTiming.status != Coordinator::MasterStockControlStatus::Installed)
        {
            if (!controlTiming.constructorComplete)
            {
                qWarning() << "StockControl could not construct a plan:"
                           << Coordinator::masterStockControlStatusName(controlTiming.status);
            }
            // A fresh valuer keeps the unproven attempt's cached pricing out of the standard planner.
            stockValuer.emplace(m_propertyStock, input.unitLinks, tierAudit);
            input.pStockValuer = &*stockValuer;
            legacyPlanning = true;
            planHandoff.handoff.reset();
        }
        else if (masterShadow.runtime != Coordinator::MasterStockControlRuntime::Play &&
                 !controlTiming.constructorComplete)
        {
            stopStockControl(masterShadow, Coordinator::masterStockControlStatusName(controlTiming.status));
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        else
        {
            const auto reference = ArenaMasterComparisonProbe::recordStockReference(
                masterShadow, input, m_assignment, m_propertyStock, traceIdentity);
            if (reference != ArenaMasterComparisonProbe::StockReferenceResult::NotRequested)
            {
                stopStockReference(ArenaMasterComparisonProbe::stockReferenceStopReason(reference));
                return;
            }
        }
#endif
    }
    if (legacyPlanning)
    {
        if (stockControl)
        {
            Coordinator::dropFollowCandidates(input);
        }
        Clock::time_point continuationStart;
        if (timed)
        {
            continuationStart = Clock::now();
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (m_testRefinementObserver && m_testRefinementObserver->frontierReceiptsEnabled())
        {
            m_testRefinementObserver->model(input, m_propertyStock);
        }
#endif
        Coordinator::ContinuationPricingCatalog catalog =
            stockValuer->continuationPricingCatalog(input);
        if (catalog.valid && ledger.open(catalog.ledgerTotal))
        {
            static_cast<void>(
                stockValuer->prepareContinuationPricing(
                    std::move(catalog), ledger));
        }
        continuationPricingPrepareNanos = planningNanosSince(continuationStart, timed);
        legacyPreparationNanos = planningNanosSince(planningStart, timed) - stockControlCallNanos;
        if (timed)
        {
            phaseStart = Clock::now();
        }
        std::unique_ptr<Coordinator::MasterShadowInspection> shadowInspection;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        if (masterShadow.enabled())
        {
            shadowInspection = ArenaMasterAssignmentProbe::openShadowInspection();
        }
#endif
        Coordinator::prepareMasterShadow(masterShadow, input, masterLedger);
        masterPreparationNanos = planningNanosSince(phaseStart, timed);
        if (timed)
        {
            phaseStart = Clock::now();
        }
        m_assignment = Coordinator::MaximumValueAssignment::assign(input);
        legacyAssignmentNanos = planningNanosSince(phaseStart, timed);
        masterTiming = Coordinator::observeMasterAssignment(
            masterShadow, input, m_assignment, m_propertyStock, ledger, masterLedger, traceIdentity, shadowInspection.get());
        if (timed)
        {
            phaseStart = Clock::now();
        }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        ArenaMasterAssignmentProbe::record(input, m_assignment, m_propertyStock, traceIdentity);
#endif
        arenaProbeNanos = planningNanosSince(phaseStart, timed);
    }
    if (replyPassWiring.record)
    {
        const auto passStart = Clock::now();
        const std::int64_t handoffNanos = planHandoff.nanos;
        {
            const Coordinator::ReplyPassRun replyPass = Coordinator::recordedReplyPass(
                localReplySession, legacyPlanning, planHandoff, replyBoard.get(), m_propertyStock);
            const auto record = QJsonDocument(Coordinator::replyPassRecord(traceIdentity, replyPass));
            const QString path =
                Settings::getInstance()->getUserPath() + QLatin1String(Coordinator::REPLY_PASS_RECORD_FILE);
            if (!Coordinator::appendTraceRecords(path, {QString::fromUtf8(record.toJson(QJsonDocument::Compact))}))
            {
                qWarning() << "Reply pass record write failed:" << path;
            }
            if (combat != Coordinator::CombatMode::Off && localReplySession)
            {
                const auto* reference = planHandoff.handoff && !legacyPlanning ? &*planHandoff.handoff : nullptr;
                const auto combatStart = Clock::now();
                auto evidence = Coordinator::compareCombatPlans(combat, traceIdentity, *m_pMap, m_dayStartKnowledge,
                    m_mobilityFields, reference, replyBoard.get(), m_propertyStock, m_assignment, combatAllowance, getVariables(),
                    combat == Coordinator::CombatMode::Acting ? getSimpleProductionSystem() : nullptr,turnAllowance,
                    replyFacts ? &*replyFacts : nullptr,&input,m_properties);
                evidence["configuredTurnWork"] = static_cast<qint64>(turnAllowance);
                evidence["allocatedStockWork"] = static_cast<qint64>(combat == Coordinator::CombatMode::Acting ?
                    turnAllowance - combatAllowance : turnAllowance);
                evidence["allocatedCombatWork"] = static_cast<qint64>(combatAllowance);
                combatCompareNanos = planningNanosSince(combatStart,true);
                evidence["elapsedNanos"] = static_cast<qint64>(combatCompareNanos);
                evidence["planSource"] = evidence["forcedArenaBranch"].toBool() ? "experimental" :
                    evidence["status"]=="OPEN_TACTICAL_REFINEMENT" ? "open_tactical" :
                    evidence["status"]=="FACTORY_PROGRAM_ON_OPEN" || evidence["status"]=="FACTORY_PROGRAM_RETAINED_ON_OPEN" ? "factory_program" :
                    evidence["status"]=="STOCK_PROGRESS_ON_OPEN" ? "stock_progress" :
                    evidence["installed"].toBool() ? "combat" : "incoming";
                const auto arm = evidence.value("selectedPairArm").toInt(-1);
                const auto decision = evidence.value("selectedPairDecision").toObject();
                if (combat == Coordinator::CombatMode::Acting && !evidence.value("forcedArenaBranch").toBool())
                {
                    QJsonObject selectedTeam;
                    const bool installed = evidence.value("installed").toBool();
                    if ((arm == 0 && decision.value("status") == "FIRST_PROVED") ||
                        (arm == 1 && installed && decision.value("status") == "SECOND_PROVED"))
                    {
                        m_teamSelection = TeamSelection::Ranked;
                        const auto bindings = decision.value("bindings").toArray();
                        if (arm < bindings.size())
                            selectedTeam = bindings[arm].toObject();
                    }
                    else if (installed && evidence.value("status") == "FUNDED_CLEAR_ON_OPEN")
                    {
                        m_teamSelection = TeamSelection::FundedClear;
                        selectedTeam = evidence.value("fundedClearPreference").toObject().value("selectedPhysicalTeam").toObject();
                    }
                    else if (installed && evidence.value("status") == "STOCK_PROGRESS_ON_OPEN")
                    {
                        m_teamSelection = TeamSelection::StockProgress;
                        selectedTeam = evidence.value("stockProgressHearing").toObject().value("selectedPhysicalTeam").toObject();
                    }
                    else if (installed && evidence.value("status") == "OPEN_TACTICAL_REFINEMENT")
                    {
                        m_teamSelection = TeamSelection::OpenTactical;
                        selectedTeam = evidence.value("openTacticalHearing").toObject().value("selectedPhysicalTeam").toObject();
                    }
                    else if ((installed && evidence.value("status") == "FACTORY_PROGRAM_ON_OPEN") ||
                             evidence.value("status") == "FACTORY_PROGRAM_RETAINED_ON_OPEN")
                    {
                        m_teamSelection = TeamSelection::FactoryProgram;
                        selectedTeam = evidence.value("factoryProgramHearing").toObject().value("selectedPhysicalTeam").toObject();
                    }
                    for (const auto &entry : selectedTeam.value("orders").toArray())
                    {
                        const auto order = entry.toObject();
                        const auto after = order.value("combatAfter").toObject();
                        if (!after.isEmpty())
                            m_expectedFireResults.emplace(order["actionIndex"].toInt(), ExpectedFireResult{
                                order["unit"].toInt(), order["targetUnit"].toInt(),
                                after["actorHp"].toDouble(), after["targetHp"].toDouble()});
                    }
                }
                const auto combatPath = Settings::getInstance()->getUserPath() + QStringLiteral("coordinated-ai-combat.jsonl");
                if (!Coordinator::appendTraceRecords(combatPath, {QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact))}))
                    qWarning() << "Combat comparison record write failed:" << combatPath;
            }
            // The handoff copy is torn down here so its cost stays inside the measured pass time.
            planHandoff = {};
        }
        m_replyPassNanos = handoffNanos + planningNanosSince(passStart, true);
    }
    if (timed)
    {
        phaseStart = Clock::now();
    }
    recordCaptureDecisions(input);
    recordProductionBlocks(input);
    const auto decisionRecordingNanos = planningNanosSince(phaseStart, timed);
    const std::int64_t turnPlanNanos =
        !timed
            ? 0
            : std::chrono::duration_cast<
                  std::chrono::nanoseconds>(
                  Clock::now() - planningStart)
                  .count() - m_replyPassNanos;
    const Coordinator::AssignmentResult::PhaseTiming &
        assignmentTiming = m_assignment.phaseTiming;
    const Coordinator::AssignmentStats & stats =
        m_assignment.stats;
    Coordinator::ReplyPredictionTelemetry replyTelemetry;
    if (replyRecorded)
    {
        replyTelemetry = {
            .mode = Coordinator::ReplyPrediction::Recorded,
            .facts = replyFacts->stats(),
            .replays = *input.replyPrediction.tally,
        };
    }
    QString phaseTimingFields;
    if (timed)
    {
        phaseTimingFields =
            QStringLiteral(
                "actorCount=%1 factLayersNanos=%2 propertyStockBuildNanos=%3 coPowerCheckNanos=%4 candidateBuildNanos=%5 continuationPricingPrepareNanos=%6 assignmentPrepareNanos=%7 greedyNanos=%8 settlingNanos=%9 pairRefinementNanos=%10 clusterNanos=%11 finishPlanNanos=%12 totalPlanningNanos=%13 totalCandidates=%14 maxCandidatesPerActor=%15 damageOracleCalls=%16 damageOracleHits=%17 stockCoupledActors=%18 settlingSweeps=%19 settlingMoves=%20 swapStates=%21 swapImprovements=%22 clustersTotal=%23 clustersEnumerated=%24 clustersCapped=%25 clustersSkippedStockBudget=%26 enumerationStates=%27 planSequence=%28")
                .arg(input.actors.size())
                .arg(factLayersNanos)
                .arg(propertyStockBuildNanos)
                .arg(coPowerCheckNanos)
                .arg(candidateBuildNanos)
                .arg(continuationPricingPrepareNanos)
                .arg(assignmentTiming.prepareNanos)
                .arg(assignmentTiming.greedyNanos)
                .arg(assignmentTiming.settlingNanos)
                .arg(assignmentTiming.pairRefinementNanos)
                .arg(assignmentTiming.clusterNanos)
                .arg(assignmentTiming.finishPlanNanos)
                .arg(factLayersNanos +
                     coPowerCheckNanos +
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
        phaseTimingFields += QStringLiteral(
            " legacyPlanningNanos=%1 legacyPreparationNanos=%2 legacyAssignmentNanos=%3 masterPreparationNanos=%4 arenaProbeNanos=%5 decisionRecordingNanos=%6 turnPlanNanos=%7")
            .arg(factLayersNanos + coPowerCheckNanos + legacyPreparationNanos + legacyAssignmentNanos)
            .arg(legacyPreparationNanos).arg(legacyAssignmentNanos).arg(masterPreparationNanos)
            .arg(arenaProbeNanos).arg(decisionRecordingNanos).arg(turnPlanNanos);
        phaseTimingFields += QStringLiteral(
            " stockPairEnumerationNanos=%1 stockPairRelativeNanos=%2 stockPairReplayNanos=%3 stockJointEnumerationNanos=%4 stockJointRelativeNanos=%5 stockJointReplayNanos=%6 stockPairStates=%7 stockPairEnemyStates=%8 stockPairWork=%9 stockPairImprovements=%10 stockPairUnresolved=%11 stockPairBudgetExhaustions=%12 stockJointStates=%13 stockJointEnemyStates=%14 stockJointWork=%15 stockJointImprovements=%16 stockJointUnresolved=%17 stockJointBudgetExhaustions=%18 stockJointSkipped=%19 ledgerTotal=%20 ledgerSpent=%21 ledgerRemaining=%22")
            .arg(assignmentTiming.stockPairs.enumerationNanos).arg(assignmentTiming.stockPairs.refinementNanos)
            .arg(assignmentTiming.stockPairs.replayNanos).arg(assignmentTiming.stockJoints.enumerationNanos)
            .arg(assignmentTiming.stockJoints.refinementNanos).arg(assignmentTiming.stockJoints.replayNanos)
            .arg(stats.stockPairs.statesUsed).arg(stats.stockPairs.enemyStatesUsed).arg(stats.stockPairs.workUsed)
            .arg(stats.stockPairs.improvements).arg(stats.stockPairs.unresolved).arg(stats.stockPairs.budgetExhaustions)
            .arg(stats.stockJoints.statesUsed).arg(stats.stockJoints.enemyStatesUsed).arg(stats.stockJoints.workUsed)
            .arg(stats.stockJoints.improvements).arg(stats.stockJoints.unresolved).arg(stats.stockJoints.budgetExhaustions)
            .arg(stats.stockJoints.skippedDomains).arg(ledger.stats().total).arg(ledger.stats().spent()).arg(ledger.balance());
        phaseTimingFields += QStringLiteral(
            " mixedPairDomains=%1 mixedPairCalls=%2 mixedPairKeys=%3 mixedPairStates=%4 mixedPairEnemyStates=%5 mixedPairWork=%6 mixedPairBudgetExhaustions=%7 mixedPairFailures=%8 allPairWork=%9")
            .arg(stats.mixedPairs.domains).arg(stats.mixedPairs.calls).arg(stats.mixedPairs.keysRefined)
            .arg(stats.mixedPairs.statesUsed).arg(stats.mixedPairs.enemyStatesUsed).arg(stats.mixedPairs.workUsed)
            .arg(stats.mixedPairs.budgetExhaustions).arg(stats.mixedPairs.failures)
            .arg(stats.stockPairs.workUsed + stats.mixedPairs.workUsed);
        if (replyRecorded)
        {
            for (const Coordinator::ReplyTelemetryField & field : Coordinator::replyTelemetryFields(replyTelemetry))
            {
                phaseTimingFields += QStringLiteral(" %1=%2").arg(QLatin1String(field.name)).arg(field.value);
            }
        }
        if (masterShadow.enabled())
        {
            phaseTimingFields += QStringLiteral(" masterShadowNanos=%1 masterShadowWork=%2 masterShadowAllowance=%3")
                .arg(masterTiming.totalNanos).arg(masterLedger.stats().spent()).arg(masterShadow.allowance);
            phaseTimingFields += QStringLiteral(
                " masterComputationNanos=%1 masterRecordNanos=%2 masterEncodingNanos=%3 masterOutputNanos=%4 masterInspectionNanos=%5 masterExportMode=%6 masterExportRecords=%7 masterExportBytes=%8 masterExportSucceeded=%9 masterOutcome=%10 masterFirstImprovementWork=%11 masterImprovementLower=%12 masterGap=%13 masterLegacyPlanUnchanged=%14 masterLegacyLedgerUnchanged=%15")
                .arg(masterTiming.computationNanos).arg(masterTiming.recordNanos).arg(masterTiming.encodingNanos)
                .arg(masterTiming.outputNanos).arg(masterTiming.inspectionNanos)
                .arg(static_cast<int>(masterShadow.exportMode)).arg(masterTiming.exportRecords)
                .arg(masterTiming.exportBytes).arg(masterTiming.exportSucceeded)
                .arg(Coordinator::masterShadowOutcomeName(masterTiming.outcome))
                .arg(optionalPlanningValue(masterTiming.firstImprovementWork))
                .arg(optionalPlanningValue(masterTiming.improvementLower)).arg(optionalPlanningValue(masterTiming.gap))
                .arg(masterTiming.legacyPlanUnchanged).arg(masterTiming.legacyLedgerUnchanged);
            std::optional<std::int64_t> afterFirstImprovement;
            if (masterTiming.firstImprovementNanos)
            {
                afterFirstImprovement =
                    masterTiming.phases[static_cast<std::size_t>(Coordinator::MasterPhase::Shadow)].inclusiveNanos -
                    *masterTiming.firstImprovementNanos;
            }
            phaseTimingFields += QStringLiteral(
                " masterFirstImprovementNanos=%1 masterAfterFirstImprovementNanos=%2 masterRefusal=%3 masterRefinementRefusal=%4")
                .arg(optionalPlanningValue(masterTiming.firstImprovementNanos))
                .arg(optionalPlanningValue(afterFirstImprovement))
                .arg(QString::fromUtf8(QJsonDocument(Coordinator::masterRefusalRecord(
                    masterLedger.lastRefusal())).toJson(QJsonDocument::Compact)))
                .arg(QString::fromUtf8(QJsonDocument(Coordinator::masterRefinementRefusalRecord(
                    masterTiming.refinementRefusal)).toJson(QJsonDocument::Compact)));
            for (std::size_t phase = 0; phase < masterTiming.phases.size(); ++phase)
            {
                const auto & sample = masterTiming.phases[phase];
                phaseTimingFields += QStringLiteral(" master_%1_calls=%2 master_%1_inclusiveNanos=%3 master_%1_exclusiveNanos=%4")
                    .arg(QLatin1String(Coordinator::MASTER_PHASE_NAMES[phase])).arg(sample.calls)
                    .arg(sample.inclusiveNanos).arg(sample.exclusiveNanos);
            }
        }
        if (masterShadow.controlsStock())
        {
            phaseTimingFields += QStringLiteral(" stockControlCallNanos=%1 stockControlAuthority=%2 stockControlGuardedPaymentShift=%3")
                .arg(stockControlCallNanos).arg(Coordinator::masterStockAuthorityName(controlTiming.selectedAuthority))
                .arg(optionalPlanningValue(controlTiming.guardedPaymentShift));
            phaseTimingFields += QStringLiteral(
                " stockControlNanos=%1 stockControlConstructorNanos=%2 stockControlSearchNanos=%3 stockControlFinalizationNanos=%4 stockControlExportNanos=%5 stockControlWork=%6 stockControlAllowance=%7 stockControlReservedWork=%8 stockControlFinalizationWork=%9 stockControlConstructorComplete=%10 stockControlStatus=%11 stockControlInstallationStatus=%12 stockControlExportSucceeded=%13")
                .arg(controlTiming.totalNanos).arg(controlTiming.constructorNanos).arg(controlTiming.searchNanos)
                .arg(controlTiming.finalizationNanos).arg(controlTiming.exportNanos).arg(controlTiming.work.spent())
                .arg(controlTiming.work.total).arg(controlTiming.reservedWork).arg(controlTiming.finalizationWork)
                .arg(controlTiming.constructorComplete).arg(Coordinator::masterStockControlStatusName(controlTiming.status))
                .arg(static_cast<int>(controlTiming.installationStatus)).arg(controlTiming.exportSucceeded);
            phaseTimingFields += QStringLiteral(
                " stockControlFirstImprovementWork=%1 stockControlFirstImprovementNanos=%2 stockControlFirstImprovementLower=%3 stockControlImprovementLower=%4 stockControlGap=%5 stockControlWorkCategories=%6")
                .arg(optionalPlanningValue(controlTiming.firstImprovementWork))
                .arg(optionalPlanningValue(controlTiming.firstImprovementNanos))
                .arg(optionalPlanningValue(controlTiming.firstImprovementLower))
                .arg(optionalPlanningValue(controlTiming.improvementLower)).arg(optionalPlanningValue(controlTiming.gap))
                .arg(QString::fromUtf8(QJsonDocument(
                    Coordinator::masterWorkRecord(controlTiming.work)).toJson(QJsonDocument::Compact)));
            phaseTimingFields += QStringLiteral(" stockControlFallback=%1").arg(legacyPlanning);
        }
        if (combat != Coordinator::CombatMode::Off)
        {
            phaseTimingFields += QStringLiteral(" combatCompareNanos=%1").arg(combatCompareNanos);
        }
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
    // Network peers would not run the audit's extra script hooks, so it is local sessions only.
    if (masterShadow.captureDangerAudit == Coordinator::CaptureDangerAudit::Recorded && localSessionMenu())
    {
        const auto auditStart = Clock::now();
        Coordinator::recordCaptureDangerAudit({
            .map = *m_pMap,
            .knowledge = m_dayStartKnowledge,
            .enemyReach = m_attackOpportunities,
            .properties = m_properties,
            .propertyStock = m_propertyStock,
            .actors = input.actors,
            .unitLinks = input.unitLinks,
            .plan = m_assignment.plan,
            .identity = traceIdentity,
            .reply = replyTelemetry,
        });
        m_captureDangerAuditNanos = planningNanosSince(auditStart, true);
    }
}

Unit* CoordinatedAi::plannableUnit(
    const Coordinator::KnownUnit & known, bool changesHeadquartersRoute) const
{
    // Each fact is read only where every earlier one admits the unit, so the engine and script calls stop early.
    Coordinator::PlannableUnitFacts facts{.movedInSnapshot = known.hasMoved};
    if (!facts.movedInSnapshot)
    {
        facts.relation = m_dayStartKnowledge.relation(known.ownerId);
    }
    Unit* pUnit = nullptr;
    if (!facts.movedInSnapshot && facts.relation == Coordinator::Relation::Own)
    {
        pUnit = Coordinator::liveUnitFor(*m_pMap, known);
        facts.mapped = pUnit != nullptr;
    }
    if (facts.mapped)
    {
        facts.movedLive = pUnit->getHasMoved();
    }
    if (facts.mapped && !facts.movedLive)
    {
        facts.normalAi = pUnit->getAiMode() == GameEnums::GameAi_Normal;
    }
    if (facts.normalAi)
    {
        Terrain * pActorTerrain = m_pMap->onMap(known.x, known.y) ? m_pMap->getTerrain(known.x, known.y) : nullptr;
        Building * pActorBuilding = pActorTerrain != nullptr ? pActorTerrain->getBuilding() : nullptr;
        const bool occupiesOwnFactory =
            pActorBuilding != nullptr && pActorBuilding->getOwner() == m_pPlayer && pActorBuilding->isProductionBuilding();
        facts.firesOrCaptures = known.canFire || pUnit->canCapture() || occupiesOwnFactory;
    }
    if (!Coordinator::unitPlannable(facts,changesHeadquartersRoute))
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
    Coordinator::BundleBuildStats & stats,
    Coordinator::ReplyFactsBuilder* pReplyFacts,
    std::span<const Coordinator::TilePoint> followTiles,
    std::optional<std::size_t>* pTodayCount,
    Coordinator::BundleBuildCache* pCache)
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
        .pReplyFacts = pReplyFacts,
        .followTiles = followTiles,
        .pCache = pCache,
    };
    return Coordinator::buildCandidateBundles(
        context, actorUnitIndex, stats, pTodayCount);
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

void CoordinatedAi::invalidateSelectedTeam(qint32 actionIndex,qint32 unit,const QString &reason,bool &replanAllowed)
{
    const auto status = m_teamSelection == TeamSelection::FundedClear
        ? QStringLiteral("FUNDED_CLEAR_TEAM_INVALIDATED") : m_teamSelection == TeamSelection::StockProgress
        ? QStringLiteral("STOCK_PROGRESS_TEAM_INVALIDATED") : m_teamSelection == TeamSelection::OpenTactical
        ? QStringLiteral("OPEN_TACTICAL_TEAM_INVALIDATED") : m_teamSelection == TeamSelection::FactoryProgram
        ? QStringLiteral("FACTORY_PROGRAM_TEAM_INVALIDATED") : QStringLiteral("RANKED_TEAM_INVALIDATED");
    const QJsonObject invalidation{{"status", status},
        {"day", m_pMap->getCurrentDay()}, {"player", m_pPlayer->getPlayerID()},
        {"planSequence", static_cast<qint64>(m_planSequence)}, {"unit", unit},
        {"actionIndex", actionIndex}, {"reason", reason}};
    const auto path = Settings::getInstance()->getUserPath() + QStringLiteral("coordinated-ai-combat.jsonl");
    if (!Coordinator::appendTraceRecords(path, {QString::fromUtf8(QJsonDocument(invalidation).toJson(QJsonDocument::Compact))}))
        qWarning() << "Selected team invalidation record write failed:" << path;
    const bool directActive = m_directUnifiedTurnActive;
    m_teamSelection = directActive ? TeamSelection::OpenTactical : TeamSelection::Ordinary;
    m_pendingExpectedFire.reset();
    m_pendingTerrainFire.reset();
    m_expectedFireResults.clear();
    m_assignment = Coordinator::AssignmentResult{};
    m_executionCursor = 0;
    if (replanAllowed)
    {
        replanAllowed = false;
        m_factLayersDay = UNBUILT_DAY;
        m_planDay = UNBUILT_DAY;
        if (ensureFactLayers()) ensureTurnPlan();
    }
    else if (directActive)
    {
        const QString errMsg = QStringLiteral("[COORD][ERROR] Direct unified replan budget exhausted on day %1 player %2 unit %3 reason=%4")
            .arg(m_pMap->getCurrentDay())
            .arg(m_pPlayer->getPlayerID())
            .arg(unit)
            .arg(reason);
        CONSOLE_PRINT(errMsg, GameConsole::eERROR);
        if (strictCoordinatedFailureMode())
        {
            constexpr int STRICT_COORD_FAILURE_EXIT_CODE = 4;
            QCoreApplication::exit(STRICT_COORD_FAILURE_EXIT_CODE);
        }
    }
}

bool CoordinatedAi::executeNextPlannedAction()
{
    bool replanAllowed = true;
    if(m_pendingExpectedFire)
    {
        const auto index=*m_pendingExpectedFire;
        const auto outcome=m_expectedFireResults.at(index);
        m_pendingExpectedFire.reset();
        // Native fire and counterfire can realize a different draw while every
        // later order remains legal. Only replan when alive/dead or rounded HP steps diverge.
        const auto observed=[&](qint32 id){
            auto *unit=m_pMap->getUnit(id);
            return unit?std::max(0.0,double(unit->getHp())):0.0;
        };
        constexpr double COMBAT_HP_STEP_EPSILON = 1e-6;
        const auto roundedSteps=[&](double hp){
            return hp <= 0.0 ? 0 : static_cast<qint32>(std::ceil(hp - COMBAT_HP_STEP_EPSILON));
        };
        const double obsShooter = observed(outcome.shooter);
        const double obsTarget = observed(outcome.target);
        // Only replan on step divergence when outcomes worsened or another queued unit still targets this victim.
        bool laterActionSharesTarget = false;
        for (std::size_t k = m_executionCursor; k < m_assignment.executionOrder.size(); ++k)
        {
            const auto & pending = m_assignment.plan.action(m_assignment.executionOrder[k]);
            if (pending.state == Coordinator::PlanActionState::Pending &&
                pending.targetUnitId == outcome.target)
            {
                laterActionSharesTarget = true;
                break;
            }
        }
        const bool shooterWorse = roundedSteps(obsShooter) < roundedSteps(outcome.shooterHp);
        const bool targetStronger = roundedSteps(obsTarget) > roundedSteps(outcome.targetHp);
        const bool sharedTargetStepsChanged =
            laterActionSharesTarget && roundedSteps(obsTarget) != roundedSteps(outcome.targetHp);
        const bool outcomeChanged =
            (obsShooter <= 0.0) != (outcome.shooterHp <= 0.0) ||
            (obsTarget <= 0.0) != (outcome.targetHp <= 0.0) ||
            shooterWorse || targetStronger || sharedTargetStepsChanged;
        // Replan after a kill if a remaining unmoved friendly can now traverse the opened tile or cleared ZoC.
        bool killOpenedPathForRemaining = false;
        if (!outcomeChanged && !m_planHasOmniscientPostKill && obsTarget <= 0.0 && m_executionCursor < m_assignment.executionOrder.size())
        {
            const Coordinator::TilePoint clearedTile = m_assignment.plan.action(index).target;
            if (clearedTile != Coordinator::INVALID_TILE)
            {
                for (std::size_t k = m_executionCursor; k < m_assignment.executionOrder.size(); ++k)
                {
                    const auto & pending = m_assignment.plan.action(m_assignment.executionOrder[k]);
                    if (pending.state != Coordinator::PlanActionState::Pending)
                        continue;
                    Unit * pPendingUnit = m_pMap->getUnit(pending.unitId);
                    if (pPendingUnit == nullptr || pPendingUnit->getHasMoved())
                        continue;
                    UnitPathFindingSystem pendingPfs(m_pMap, pPendingUnit, m_pPlayer);
                    pendingPfs.explore();
                    if (pendingPfs.isReachable(clearedTile.x, clearedTile.y))
                    {
                        killOpenedPathForRemaining = true;
                        break;
                    }
                    for (const Coordinator::EntryDirection dir : Coordinator::ENTRY_DIRECTIONS)
                    {
                        const Coordinator::StepVector step = Coordinator::entryStep(dir);
                        const qint32 nx = clearedTile.x + step.dx;
                        const qint32 ny = clearedTile.y + step.dy;
                        if (m_pMap->onMap(nx, ny) && pendingPfs.isReachable(nx, ny))
                        {
                            killOpenedPathForRemaining = true;
                            break;
                        }
                    }
                    if (killOpenedPathForRemaining)
                        break;
                }
            }
        }
        if (outcomeChanged)
            invalidateSelectedTeam(index, outcome.shooter, QStringLiteral("NATIVE_COMBAT_OUTCOME_CHANGED"), replanAllowed);
        else if (killOpenedPathForRemaining)
            invalidateSelectedTeam(index, outcome.shooter, QStringLiteral("ENEMY_KILLED_TILE_OPENED"), replanAllowed);
    }
    if (m_pendingTerrainFire)
    {
        const auto terrainFire = *m_pendingTerrainFire;
        m_pendingTerrainFire.reset();
        Terrain * pTargetTerrain = m_pMap->onMap(terrainFire.tile.x, terrainFire.tile.y)
            ? m_pMap->getTerrain(terrainFire.tile.x, terrainFire.tile.y)
            : nullptr;
        if (pTargetTerrain == nullptr || pTargetTerrain->getHp() <= 0)
        {
            invalidateSelectedTeam(
                static_cast<qint32>(m_executionCursor > 0 ? m_executionCursor - 1 : 0),
                terrainFire.shooter,
                QStringLiteral("TERRAIN_DESTROYED_TILE_OPENED"),
                replanAllowed);
        }
    }
    replanAllowed = true;
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
    return m_assignment.stockConstructionStatus!=Coordinator::StockConstructionStatus::NotRequested &&
           m_assignment.stockConstructionStatus!=Coordinator::StockConstructionStatus::Complete;
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
    const std::optional<quint32> luckPeekNonce =
        (m_teamSelection == TeamSelection::OpenTactical && m_turnLuckDay == m_pMap->getCurrentDay())
            ? std::optional<quint32>(m_turnLuckNonce)
            : std::nullopt;
    Coordinator::EngineActionBuildResult result =
        Coordinator::buildEngineAction(
            *m_pMap,
            *m_pPlayer,
            m_assignment.plan.action(actionIndex),
            luckPeekNonce);
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
        if (m_teamSelection != TeamSelection::Ordinary || m_directUnifiedTurnActive)
        {
            invalidateSelectedTeam(actionIndex,original.unitId,engineActionFailureName(result.failure),replanAllowed);
            return false;
        }
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
        if(m_teamSelection != TeamSelection::Ordinary || m_directUnifiedTurnActive)
        {
            invalidateSelectedTeam(actionIndex,original.unitId,QStringLiteral("COMMIT_FAILED"),replanAllowed);
            return false;
        }
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
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (qgetenv("COW_NATIVE_FACTORY_PREFIX_COMMANDS") == "1" && qgetenv("COW_AI_ARENA_TEST") == "1")
        Coordinator::recordFactoryPrefixCommands(*m_pMap, *m_pPlayer, m_assignment.plan.action(actionIndex),
                                                  actionIndex, m_planSequence, *result.action);
#endif
    const Coordinator::PlannedAction & finalActionRef = m_assignment.plan.action(actionIndex);
    spGameAction actionToEmit = result.action;
    if (unifiedSpecialActionsActive())
    {
        if (spGameAction upgraded = maybeUpgradeMoveToJoinOrStealth(finalActionRef, result.action);
            upgraded.get() != nullptr)
        {
            actionToEmit = std::move(upgraded);
        }
    }
    if (m_teamSelection != TeamSelection::Ordinary)
    {
        if (m_expectedFireResults.contains(actionIndex))
            m_pendingExpectedFire = actionIndex;
        else if (Coordinator::isTerrainTargetId(finalActionRef.targetUnitId))
            m_pendingTerrainFire = ExpectedTerrainFire{finalActionRef.target, finalActionRef.unitId};
    }
    emit sigPerformAction(actionToEmit);
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
    if (m_teamSelection == TeamSelection::OpenTactical && m_turnLuckDay == m_pMap->getCurrentDay())
    {
        oracle.setLuckPeek(m_turnLuckNonce, m_pPlayer->getPlayerID());
    }
    Coordinator::BundleBuildStats buildStats;
    const std::vector<Coordinator::CandidateBundle> candidates =
        candidatesFor(
            knowledge,
            enemyReach,
            properties,
            propertyStock,
            oracle,
            actorUnitIndex,
            buildStats,
            nullptr);
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

bool CoordinatedAi::buildUnits(spQmlVectorBuilding & pBuildings, spQmlVectorUnit & pUnits,
                               spQmlVectorUnit & pEnemyUnits, spQmlVectorBuilding & pEnemyBuildings)
{
    Q_UNUSED(pUnits)
    Q_UNUSED(pEnemyUnits)
    Q_UNUSED(pEnemyBuildings)
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    m_aiStep = AISteps::buildUnits;
    const qint32 currentDay = m_pMap->getCurrentDay();
    const qint32 currentFunds = m_pPlayer->getFunds();
    std::vector<Coordinator::TilePoint> openSites;
    spGameAction probeAction = MemoryManagement::create<GameAction>(ACTION_BUILD_UNITS, m_pMap);
    for (qint32 i = 0; i < pBuildings->size(); ++i)
    {
        Building * pBuilding = pBuildings->at(i);
        if (pBuilding == nullptr || !pBuilding->isProductionBuilding() ||
            pBuilding->getTerrain()->getUnit() != nullptr)
        {
            continue;
        }
        const qint32 bx = pBuilding->Building::getX();
        const qint32 by = pBuilding->Building::getY();
        probeAction->setTarget(QPoint(bx, by));
        if (probeAction->canBePerformed())
        {
            openSites.push_back({bx, by});
        }
    }
    // Plan once per day, or replan if mid-day funds or newly vacated/captured production sites open up.
    if (m_unifiedProductionDay != currentDay || currentFunds > m_unifiedProductionFunds ||
        openSites.size() > m_unifiedOpenSiteCount)
    {
        m_unifiedBuildQueue.clear();
        m_unifiedBuildCursor = 0;
        m_unifiedProductionDay = currentDay;
        m_unifiedProductionFunds = currentFunds;
        m_unifiedOpenSiteCount = openSites.size();
        if (!openSites.empty() && currentFunds > 0)
        {
            const auto liveKnowledge = Coordinator::BattlefieldKnowledge::capture(*m_pMap, *m_pPlayer);
            const auto liveProperties = Coordinator::buildPropertyEconomics(*m_pMap, liveKnowledge);
            Coordinator::DamageOracle liveOracle(*m_pMap);
            const auto model = Coordinator::buildLiveUnifiedProductionModel(
                *m_pMap, *m_pPlayer, liveKnowledge, m_mobilityFields, liveOracle, liveProperties, m_propertyStock, false);
            const auto plan = model.planBuilds(openSites, Coordinator::toMilliFunds(currentFunds));
            m_unifiedBuildQueue = plan.orders;

            QJsonArray orderArr;
            for (const auto & ord : plan.orders)
            {
                orderArr.append(QJsonObject{
                    {"tile", QJsonArray{ord.tile.x, ord.tile.y}},
                    {"unitId", ord.unitId},
                    {"cost", qint64(ord.cost)},
                    {"utility", qint64(ord.utility)},
                    {"capperGain", qint64(ord.capperGain)},
                    {"combatGain", qint64(ord.combatGain)},
                    {"arrivalTurns", ord.arrivalTurns},
                });
            }
            const QJsonObject prodRecord{
                {"mode", "UNIFIED_PRODUCTION"},
                {"day", currentDay},
                {"player", m_pPlayer->getPlayerID()},
                {"funds", currentFunds},
                {"nextTurnIncome", qint64(model.nextTurnIncome())},
                {"openSites", static_cast<qint32>(openSites.size())},
                {"choseSave", plan.choseSave},
                {"todayUtility", qint64(plan.todayUtility)},
                {"tomorrowUtility", qint64(plan.tomorrowUtility)},
                {"totalValue", qint64(plan.totalValue)},
                {"saveOnlyValue", qint64(plan.saveOnlyValue)},
                {"spentFunds", qint64(plan.spentFunds)},
                {"orders", orderArr},
            };
            const QString prodPath =
                Settings::getInstance()->getUserPath() + QStringLiteral("coordinated-ai-combat.jsonl");
            Coordinator::appendTraceRecords(
                prodPath, {QString::fromUtf8(QJsonDocument(prodRecord).toJson(QJsonDocument::Compact))});
        }
    }

    while (m_unifiedBuildCursor < m_unifiedBuildQueue.size())
    {
        const Coordinator::ProductionBuildOrder & order = m_unifiedBuildQueue[m_unifiedBuildCursor++];
        spGameAction pAction = MemoryManagement::create<GameAction>(ACTION_BUILD_UNITS, m_pMap);
        pAction->setTarget(QPoint(order.tile.x, order.tile.y));
        if (!pAction->canBePerformed())
        {
            continue;
        }
        spMenuData pData = pAction->getMenuStepData();
        if (!pData->validData())
        {
            continue;
        }
        const auto actionIds = pData->getActionIDs();
        const auto enableList = pData->getEnabledList();
        const auto costList = pData->getCostList();
        qint32 matched = -1;
        for (qint32 i = 0; i < actionIds.size(); ++i)
        {
            if (enableList[i] && actionIds[i] == order.unitId)
            {
                matched = i;
                break;
            }
        }
        if (matched < 0)
        {
            continue;
        }
        CoreAI::addMenuItemData(pAction, order.unitId, costList[matched]);
        if (pAction->isFinalStep() && pAction->canBePerformed())
        {
            m_unifiedProductionFunds = std::max<qint32>(0, m_pPlayer->getFunds() - costList[matched]);
            emit sigPerformAction(pAction);
            return true;
        }
    }
    m_unifiedProductionFunds = m_pPlayer->getFunds();
    return false;
}

bool CoordinatedAi::strictCoordinatedFailureMode() const
{
    const QByteArray envStrict = qgetenv("COW_COORDINATED_STRICT");
    if (envStrict == "1")
        return true;
    if (envStrict == "0")
        return false;
    Mainapp * pApp = Mainapp::getInstance();
    return pApp != nullptr && pApp->getNoUi();
}

bool CoordinatedAi::unifiedSpecialActionsActive() const
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    if (Coordinator::combatMode() != Coordinator::CombatMode::Acting)
    {
        return false;
    }
    if (qgetenv("COW_AI_ARENA_TEST") == "1" ||
        qgetenv("COW_COMBAT_EXECUTE_FIXED_ORDERS") == "1" ||
        qgetenv("COW_COORDINATED_LEGACY_TRIALS") == "1" ||
        !localSessionMenu())
    {
        return false;
    }
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    if (m_testStockControlHeld || m_testRefinementObserver != nullptr)
    {
        return false;
    }
#endif
    return true;
}

bool CoordinatedAi::useBuildingActions(spQmlVectorBuilding & pBuildings, spQmlVectorUnit & pUnits)
{
    Q_UNUSED(pBuildings)
    Q_UNUSED(pUnits)
    if (unifiedSpecialActionsActive())
    {
        return tryExecuteUnifiedBuildingActions(true);
    }
    return CoreAI::useBuilding(pBuildings, pUnits);
}

bool CoordinatedAi::tryPrePlanSpecialActions()
{
    if (tryExecuteUnifiedBuildingActions(false))
    {
        return true;
    }
    if (tryExecuteUnifiedCoBoarding())
    {
        return true;
    }
    if (tryExecuteUnifiedSiloAction(false))
    {
        return true;
    }
    return false;
}

bool CoordinatedAi::tryPostPlanSpecialActions()
{
    if (tryExecuteUnifiedUnitSpecialActions())
    {
        return true;
    }
    if (tryExecuteUnifiedSiloAction(true))
    {
        return true;
    }
    if (tryExecuteUnifiedBuildingActions(true))
    {
        return true;
    }
    return false;
}

bool CoordinatedAi::tryExecuteUnifiedBuildingActions(bool allowFriendlySplash)
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    static const QString ACTION_CANNON_FIRE_ID = QStringLiteral("ACTION_CANNON_FIRE");
    static const QString ACTION_LASER_FIRE_ID = QStringLiteral("ACTION_LASER_FIRE");
    static const QString ACTION_DEATHRAY_FIRE_ID = QStringLiteral("ACTION_DEATHRAY_FIRE");
    static const QString ACTION_TALON_GUN_ID = QStringLiteral("ACTION_TALON_GUN");
    static const QString ACTION_CRYSTALL_HEAL_ID = QStringLiteral("ACTION_CRYSTALL_HEAL");
    static const QString ACTION_NEST_LASER_ID = QStringLiteral("ACTION_NEST_LASER");
    static const QString ACTION_NEST_MORTAR_ID = QStringLiteral("ACTION_NEST_MORTAR");

    const qint32 width = m_pMap->getMapWidth();
    const qint32 height = m_pMap->getMapHeight();
    std::vector<Coordinator::SpecialActionUnit> boardUnits;
    for (qint32 y = 0; y < height; ++y)
    {
        for (qint32 x = 0; x < width; ++x)
        {
            Terrain* pTerrain = m_pMap->getTerrain(x, y);
            Unit* pUnit = pTerrain != nullptr ? pTerrain->getUnit() : nullptr;
            if (pUnit == nullptr || pUnit->getHpRounded() <= 0)
            {
                continue;
            }
            boardUnits.push_back(Coordinator::SpecialActionUnit{
                .tile = Coordinator::TilePoint{x, y},
                .replacementCost = Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts())),
                .hpSteps = pUnit->getHpRounded(),
                .isEnemy = m_pPlayer->isEnemyUnit(pUnit),
                .hasMoved = pUnit->getHasMoved(),
                .needsSupplyRefill = needsRefuel(pUnit),
                .engineUnitId = pUnit->getUniqueID(),
            });
        }
    }

    const GameRules* pRules = m_pMap->getGameRules();
    const bool specialDestruction = pRules != nullptr && pRules->getSpecialDestruction();
    spQmlVectorBuilding pBuildings = m_pPlayer->getSpBuildings();
    std::optional<Coordinator::UnifiedProductionModel> cachedProdModel;
    const auto ensureProdModel = [&]() -> const Coordinator::UnifiedProductionModel & {
        if (!cachedProdModel.has_value())
        {
            const auto liveKnowledge = Coordinator::BattlefieldKnowledge::capture(*m_pMap, *m_pPlayer);
            const auto liveProperties = Coordinator::buildPropertyEconomics(*m_pMap, liveKnowledge);
            Coordinator::DamageOracle liveOracle(*m_pMap);
            cachedProdModel.emplace(Coordinator::buildLiveUnifiedProductionModel(
                *m_pMap, *m_pPlayer, liveKnowledge, m_mobilityFields, liveOracle, liveProperties, m_propertyStock));
        }
        return *cachedProdModel;
    };

    for (qint32 bi = 0; bi < pBuildings->size(); ++bi)
    {
        Building* pBuilding = pBuildings->at(bi);
        if (pBuilding == nullptr)
        {
            continue;
        }
        const qint32 bx = pBuilding->Building::getX();
        const qint32 by = pBuilding->Building::getY();
        const QStringList actions = pBuilding->getActionList();
        for (const QString & actionId : actions)
        {
            if (actionId.isEmpty() || actionId == QLatin1String(ACTION_BUILD_UNITS))
            {
                continue;
            }
            const bool isBhDoor1 = actionId == QLatin1String(ACTION_BLACKHOLEFACTORY_DOOR1);
            const bool isBhDoor2 = actionId == QLatin1String(ACTION_BLACKHOLEFACTORY_DOOR2);
            const bool isBhDoor3 = actionId == QLatin1String(ACTION_BLACKHOLEFACTORY_DOOR3);
            const bool isNestDoor = actionId == QLatin1String(ACTION_NEST_FACTORY_DOOR);
            const bool isFreeOozium = actionId == QLatin1String(ACTION_PRODUCE_OOZIUM_FREE);
            const bool isSpawner = isBhDoor1 || isBhDoor2 || isBhDoor3 || isNestDoor || isFreeOozium;
            if (!allowFriendlySplash && (isNestDoor || isFreeOozium))
            {
                continue;
            }
            spGameAction pAction = MemoryManagement::create<GameAction>(actionId, m_pMap);
            pAction->setTarget(QPoint(bx, by));
            if (!pAction->canBePerformed())
            {
                continue;
            }

            if (isSpawner)
            {
                spMenuData pMenu = pAction->getMenuStepData();
                if (!pMenu->validData())
                {
                    continue;
                }
                const QStringList menuIds = pMenu->getActionIDs();
                const QVector<qint32> costList = pMenu->getCostList();
                const QVector<bool> enableList = pMenu->getEnabledList();
                QStringList enabledIds;
                for (qint32 i = 0; i < menuIds.size() && i < enableList.size(); ++i)
                {
                    if (enableList[i])
                    {
                        enabledIds.push_back(menuIds[i]);
                    }
                }
                if (enabledIds.isEmpty())
                {
                    continue;
                }
                Coordinator::TilePoint spawnTile{bx, by};
                if (isBhDoor1)
                {
                    spawnTile = Coordinator::TilePoint{bx + Coordinator::UNIFIED_BLACKHOLE_DOOR1_DX, by + 1};
                }
                else if (isBhDoor2)
                {
                    spawnTile = Coordinator::TilePoint{bx + Coordinator::UNIFIED_BLACKHOLE_DOOR2_DX, by + 1};
                }
                else if (isBhDoor3)
                {
                    spawnTile = Coordinator::TilePoint{bx + Coordinator::UNIFIED_BLACKHOLE_DOOR3_DX, by + 1};
                }
                else if (isFreeOozium || isNestDoor)
                {
                    const QPoint offset = pBuilding->getActionTargetOffset();
                    spawnTile = Coordinator::TilePoint{bx + offset.x(), by + offset.y()};
                }
                const QString bestId = Coordinator::selectZeroCostDoorUnit(
                    ensureProdModel(), spawnTile, enabledIds);
                for (qint32 i = 0; i < menuIds.size() && i < enableList.size() && i < costList.size(); ++i)
                {
                    if (enableList[i] && menuIds[i] == bestId)
                    {
                        addMenuItemData(pAction, bestId, costList[i]);
                        break;
                    }
                }
                if (pAction->isFinalStep() && pAction->canBePerformed())
                {
                    emit sigPerformAction(pAction);
                    return true;
                }
                continue;
            }

            if (actionId == ACTION_CANNON_FIRE_ID)
            {
                spMarkedFieldData pData = pAction->getMarkedFieldStepData();
                if (pData.get() == nullptr)
                {
                    continue;
                }
                QPoint bestTarget(-1, -1);
                Coordinator::MilliFunds bestGain = 0;
                const QVector<QPoint>* pPoints = pData->getPoints();
                if (pPoints != nullptr)
                {
                    for (const QPoint & pt : *pPoints)
                    {
                        if (!m_pMap->onMap(pt.x(), pt.y()))
                        {
                            continue;
                        }
                        Unit* pTarget = m_pMap->getTerrain(pt.x(), pt.y())->getUnit();
                        if (pTarget == nullptr || !m_pPlayer->isEnemyUnit(pTarget))
                        {
                            continue;
                        }
                        const std::int32_t dmgSteps = std::max<std::int32_t>(
                            0, static_cast<std::int32_t>(std::llround(pBuilding->getDamage(pTarget))));
                        const Coordinator::MilliFunds gain = Coordinator::specialDamageCapitalDelta(
                            Coordinator::toMilliFunds(std::max<qint32>(0, pTarget->getUnitCosts())),
                            pTarget->getHpRounded(),
                            dmgSteps,
                            specialDestruction);
                        if (gain > bestGain)
                        {
                            bestGain = gain;
                            bestTarget = pt;
                        }
                    }
                }
                if (bestGain > 0 && m_pMap->onMap(bestTarget.x(), bestTarget.y()))
                {
                    addSelectedFieldData(pAction, bestTarget);
                    if (pAction->isFinalStep() && pAction->canBePerformed())
                    {
                        emit sigPerformAction(pAction);
                        return true;
                    }
                }
                continue;
            }

            const bool isCrystal = (actionId == ACTION_CRYSTALL_HEAL_ID);
            const bool isLaser = (actionId == ACTION_LASER_FIRE_ID || actionId == ACTION_NEST_LASER_ID);
            const bool isDeathray = (actionId == ACTION_DEATHRAY_FIRE_ID);
            const bool isTalon = (actionId == ACTION_TALON_GUN_ID);
            const bool isNestMortar = (actionId == ACTION_NEST_MORTAR_ID);
            if (!isCrystal && !isLaser && !isDeathray && !isTalon && !isNestMortar)
            {
                continue;
            }

            QmlVectorPoint* pRawFields = pBuilding->getActionTargetFields();
            if (pRawFields == nullptr)
            {
                continue;
            }
            const QPoint offset = (isLaser || isNestMortar) ? QPoint(0, 0) : pBuilding->getActionTargetOffset();
            std::vector<Coordinator::TilePoint> areaTiles;
            areaTiles.reserve(static_cast<std::size_t>(pRawFields->size()));
            for (const QPoint & rel : pRawFields->getVector())
            {
                const qint32 tx = bx + offset.x() + rel.x();
                const qint32 ty = by + offset.y() + rel.y();
                if (m_pMap->onMap(tx, ty))
                {
                    areaTiles.push_back(Coordinator::TilePoint{tx, ty});
                }
            }
            const std::int32_t dmgSteps = static_cast<std::int32_t>(std::llround(pBuilding->getDamage(nullptr)));

            if (isNestMortar)
            {
                Coordinator::SiloStrikeQuote bestMortar;
                for (const Coordinator::TilePoint center : areaTiles)
                {
                    Coordinator::MilliFunds enemyLoss = 0;
                    Coordinator::MilliFunds friendlyLoss = 0;
                    bool hitsUnmovedFriendly = false;
                    for (const auto & unit : boardUnits)
                    {
                        if (Coordinator::manhattanDistance(unit.tile, center) > Coordinator::UNIFIED_SILO_BLAST_RADIUS)
                        {
                            continue;
                        }
                        const Coordinator::MilliFunds delta = Coordinator::specialDamageCapitalDelta(
                            unit.replacementCost, unit.hpSteps, std::max<std::int32_t>(0, dmgSteps), specialDestruction);
                        if (unit.isEnemy)
                        {
                            enemyLoss += delta;
                        }
                        else
                        {
                            friendlyLoss += delta;
                            if (!unit.hasMoved && delta > 0)
                            {
                                hitsUnmovedFriendly = true;
                            }
                        }
                    }
                    const Coordinator::MilliFunds net = enemyLoss - friendlyLoss;
                    if (net > bestMortar.netValue)
                    {
                        bestMortar.netValue = net;
                        bestMortar.friendlyLoss = friendlyLoss;
                        bestMortar.hitsUnmovedFriendly = hitsUnmovedFriendly;
                    }
                }
                const bool splashOk = allowFriendlySplash ||
                    (!bestMortar.hitsUnmovedFriendly && bestMortar.friendlyLoss == 0);
                if (bestMortar.netValue > 0 && splashOk && pAction->isFinalStep())
                {
                    emit sigPerformAction(pAction);
                    return true;
                }
                continue;
            }

            const GameEnums::BuildingTarget mode =
                isCrystal ? GameEnums::BuildingTarget_Own : pBuilding->getBuildingTargets();
            const Coordinator::AreaBuildingActionQuote quote = Coordinator::evaluateAreaBuildingAction(
                areaTiles, boardUnits, dmgSteps, mode, specialDestruction);

            bool shouldFire = false;
            if (isCrystal || isTalon)
            {
                shouldFire = quote.netValue > 0;
            }
            else if (isDeathray || isLaser)
            {
                shouldFire = quote.netValue > 0 && (allowFriendlySplash || quote.friendlyLoss == 0);
            }
            if (shouldFire && pAction->isFinalStep())
            {
                emit sigPerformAction(pAction);
                return true;
            }
        }
    }
    return false;
}

bool CoordinatedAi::tryExecuteUnifiedSiloAction(bool allowUnmovedFriendlySplash)
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    std::vector<Coordinator::TilePoint> openSilos;
    const qint32 width = m_pMap->getMapWidth();
    const qint32 height = m_pMap->getMapHeight();
    for (qint32 y = 0; y < height; ++y)
    {
        for (qint32 x = 0; x < width; ++x)
        {
            Terrain* pTerrain = m_pMap->getTerrain(x, y);
            Building* pBuilding = pTerrain != nullptr ? pTerrain->getBuilding() : nullptr;
            if (pBuilding != nullptr && pBuilding->isMissile() && pTerrain->getUnit() == nullptr)
            {
                openSilos.push_back(Coordinator::TilePoint{x, y});
            }
        }
    }
    if (openSilos.empty())
    {
        return false;
    }

    std::vector<Coordinator::SpecialActionUnit> boardUnits;
    std::vector<Unit*> ownSiloActors;
    for (qint32 y = 0; y < height; ++y)
    {
        for (qint32 x = 0; x < width; ++x)
        {
            Terrain* pTerrain = m_pMap->getTerrain(x, y);
            Unit* pUnit = pTerrain != nullptr ? pTerrain->getUnit() : nullptr;
            if (pUnit == nullptr || pUnit->getHpRounded() <= 0)
            {
                continue;
            }
            const bool isEnemy = m_pPlayer->isEnemyUnit(pUnit);
            boardUnits.push_back(Coordinator::SpecialActionUnit{
                .tile = Coordinator::TilePoint{x, y},
                .replacementCost = Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts())),
                .hpSteps = pUnit->getHpRounded(),
                .isEnemy = isEnemy,
                .hasMoved = pUnit->getHasMoved(),
                .needsSupplyRefill = needsRefuel(pUnit),
                .engineUnitId = pUnit->getUniqueID(),
            });
            if (!isEnemy && pUnit->getOwner() == m_pPlayer && !pUnit->getHasMoved() &&
                pUnit->getAiMode() == GameEnums::GameAi_Normal && pUnit->canCapture() &&
                pUnit->getActionList().contains(QLatin1String(ACTION_MISSILE)))
            {
                ownSiloActors.push_back(pUnit);
            }
        }
    }
    if (ownSiloActors.empty())
    {
        return false;
    }

    const GameRules* pRules = m_pMap->getGameRules();
    const bool specialDestruction = pRules != nullptr && pRules->getSpecialDestruction();
    const std::int32_t horizon = std::max<std::int32_t>(
        1, m_propertyStock.horizonTurns() > 0 ? m_propertyStock.horizonTurns() : Coordinator::DEFAULT_ECONOMIC_RELEVANCE_HORIZON);
    const std::int32_t capPts = m_propertyStock.capturePointsToCapture() > 0
        ? m_propertyStock.capturePointsToCapture() : Unit::MAX_CAPTURE_POINTS;
    Coordinator::DamageOracle oracle(*m_pMap);

    const auto estimateExposureAt = [&](Unit* pUnit, qint32 actorKi, Coordinator::TilePoint tile) -> Coordinator::MilliFunds {
        if (actorKi < 0 || !m_pMap->onMap(tile.x, tile.y))
        {
            return 0;
        }
        const Coordinator::MilliFunds unitCost =
            Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts()));
        Coordinator::MilliFunds worstLoss = 0;
        for (const std::int32_t slot : m_attackOpportunities.attackerSlotsAt(tile.x, tile.y))
        {
            const auto & spec = m_attackOpportunities.attacker(slot);
            if (spec.unitIndex < 0 || spec.unitIndex >= static_cast<std::int32_t>(m_dayStartKnowledge.units().size()))
            {
                continue;
            }
            const auto & kEnemy = m_dayStartKnowledge.units()[static_cast<std::size_t>(spec.unitIndex)];
            Unit* pEnemy = m_pMap->getTerrain(kEnemy.x, kEnemy.y) != nullptr
                ? m_pMap->getTerrain(kEnemy.x, kEnemy.y)->getUnit() : nullptr;
            if (pEnemy == nullptr || pEnemy->getHpRounded() <= 0 || !m_pPlayer->isEnemyUnit(pEnemy))
            {
                continue;
            }
            const auto est = oracle.estimate(spec.unitIndex, *pEnemy, {kEnemy.x, kEnemy.y}, actorKi, *pUnit, tile);
            if (est.possible && est.damageSteps > 0)
            {
                const Coordinator::MilliFunds loss = Coordinator::bookDamage(
                    unitCost, std::min(pUnit->getHpRounded(), est.damageSteps));
                worstLoss = std::max(worstLoss, loss);
            }
        }
        return worstLoss;
    };

    spGameAction bestAction;
    Coordinator::TilePoint bestTargetCenter = Coordinator::INVALID_TILE;
    Coordinator::MilliFunds bestSurplus = 0;

    for (Unit* pUnit : ownSiloActors)
    {
        const Coordinator::TilePoint origin{pUnit->Unit::getX(), pUnit->Unit::getY()};
        const Coordinator::MilliFunds unitCost =
            Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts()));
        const std::int32_t actorKi = m_dayStartKnowledge.unitIndexAt(origin.x, origin.y);
        UnitPathFindingSystem pfs(m_pMap, pUnit, m_pPlayer);
        pfs.explore();

        Coordinator::MilliFunds origForfeit = 0;
        Coordinator::MilliFunds bestAltCapture = 0;
        Coordinator::MilliFunds bestAltFire = 0;
        for (const auto & prop : m_properties)
        {
            const Coordinator::Relation rel = m_dayStartKnowledge.relation(prop.ownerId);
            if (!prop.capturable || rel == Coordinator::Relation::Own || rel == Coordinator::Relation::Allied)
            {
                continue;
            }
            const Coordinator::OwnerSign beforeSign = Coordinator::captureOwnerBefore(prop.ownerId, rel);
            if (prop.x == origin.x && prop.y == origin.y && pUnit->getCapturePoints() > 0)
            {
                const std::int32_t remOwnedTurns = std::max<std::int32_t>(
                    1, horizon - std::max<std::int32_t>(0, prop.captureTurnsRemaining));
                origForfeit = std::max<Coordinator::MilliFunds>(
                    0, Coordinator::incomeSwing(Coordinator::captureIncome(prop), remOwnedTurns, beforeSign,
                                                Coordinator::OwnerSign::Ours));
                continue;
            }
            if (QPoint(prop.x, prop.y) != pUnit->getPosition() && !pfs.isReachable(prop.x, prop.y))
            {
                continue;
            }
            const auto pathPts = pfs.getPathFast(prop.x, prop.y);
            if (pathPts.empty() && QPoint(prop.x, prop.y) != pUnit->getPosition())
            {
                continue;
            }
            const std::int32_t rate = std::max<std::int32_t>(1, pUnit->getCaptureRate(QPoint(prop.x, prop.y)));
            const std::int32_t turns = Coordinator::captureTurnsFor(rate, rate, capPts);
            if (turns < horizon)
            {
                const Coordinator::MilliFunds capVal = std::max<Coordinator::MilliFunds>(
                    0, Coordinator::incomeSwing(Coordinator::captureIncome(prop), horizon - turns, beforeSign,
                                                Coordinator::OwnerSign::Ours));
                bestAltCapture = std::max(bestAltCapture, capVal);
            }
        }
        const Coordinator::MilliFunds originExposure = estimateExposureAt(pUnit, actorKi, origin);
        const Coordinator::MilliFunds baseOpportunity = origForfeit + std::max(bestAltCapture, bestAltFire);

        for (const Coordinator::TilePoint siloTile : openSilos)
        {
            std::vector<QPoint> enginePath;
            if (origin == siloTile)
            {
                enginePath.push_back(QPoint(origin.x, origin.y));
            }
            else
            {
                if (!pfs.isReachable(siloTile.x, siloTile.y))
                {
                    continue;
                }
                enginePath = pfs.getPathFast(siloTile.x, siloTile.y);
                if (enginePath.empty())
                {
                    continue;
                }
            }
            const qint32 moveCost = pfs.getCosts(enginePath);
            spGameAction probe = MemoryManagement::create<GameAction>(ACTION_MISSILE, m_pMap);
            probe->setTarget(QPoint(origin.x, origin.y));
            probe->setMovepath(enginePath, moveCost);
            if (!probe->canBePerformed(ACTION_MISSILE, false, m_pPlayer))
            {
                continue;
            }
            const Coordinator::SiloStrikeQuote quote = Coordinator::evaluateBestSiloStrike(
                width, height, origin, siloTile, unitCost, pUnit->getHpRounded(), boardUnits, specialDestruction);
            if (quote.netValue <= 0 || (!allowUnmovedFriendlySplash && quote.hitsUnmovedFriendly) ||
                quote.targetCenter == Coordinator::INVALID_TILE)
            {
                continue;
            }
            const Coordinator::MilliFunds siloExposure = estimateExposureAt(pUnit, actorKi, siloTile);
            const Coordinator::MilliFunds exposureDelta = std::max<Coordinator::MilliFunds>(0, siloExposure - originExposure);
            const Coordinator::MilliFunds surplus = quote.netValue - (baseOpportunity + exposureDelta);
            if (surplus > bestSurplus)
            {
                bestSurplus = surplus;
                bestTargetCenter = quote.targetCenter;
                bestAction = probe;
            }
        }
    }

    if (bestSurplus > 0 && bestAction.get() != nullptr && bestTargetCenter != Coordinator::INVALID_TILE)
    {
        addSelectedFieldData(bestAction, QPoint(bestTargetCenter.x, bestTargetCenter.y));
        if (bestAction->isFinalStep() && bestAction->canBePerformed())
        {
            emit sigPerformAction(bestAction);
            return true;
        }
    }
    return false;
}

bool CoordinatedAi::tryExecuteUnifiedCoBoarding()
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    const qint32 currentDay = m_pMap->getCurrentDay();
    if (m_coBoardCheckedDay == currentDay)
    {
        return false;
    }
    for (quint8 slot = 0; slot < Coordinator::MobilityEpoch::CO_SLOT_COUNT; ++slot)
    {
        CO* pCO = m_pPlayer->getCO(slot);
        if (pCO == nullptr || pCO->getCOUnit() != nullptr || !pCO->getCoRangeEnabled() || pCO->getGlobalCoZone())
        {
            continue;
        }
        const QString actionId = QString::fromLatin1(slot == 0 ? ACTION_CO_UNIT_0 : ACTION_CO_UNIT_1);
        const qint32 coRange = std::max<qint32>(0, pCO->getCORange());
        const qint32 width = m_pMap->getMapWidth();
        const qint32 height = m_pMap->getMapHeight();

        Unit* bestUnit = nullptr;
        Coordinator::MilliFunds bestScore = 0;
        for (qint32 y = 0; y < height; ++y)
        {
            for (qint32 x = 0; x < width; ++x)
            {
                Terrain* pTerrain = m_pMap->getTerrain(x, y);
                Unit* pUnit = pTerrain != nullptr ? pTerrain->getUnit() : nullptr;
                if (pUnit == nullptr || pUnit->getOwner() != m_pPlayer || pUnit->getHasMoved() ||
                    pUnit->getAiMode() != GameEnums::GameAi_Normal || !pUnit->hasWeapons() ||
                    pUnit->getHpRounded() <= 0)
                {
                    continue;
                }
                spGameAction probe = MemoryManagement::create<GameAction>(actionId, m_pMap);
                probe->setTarget(QPoint(x, y));
                if (!probe->canBePerformed() || !probe->isFinalStep())
                {
                    continue;
                }
                const Coordinator::MilliFunds unitCost =
                    Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts()));
                const Coordinator::MilliFunds boardCost = unitCost / 2;
                const qint32 zoneReach = coRange + std::max<qint32>(0, pUnit->getBaseMovementPoints());
                Coordinator::MilliFunds supportedCapital =
                    (unitCost * pUnit->getHpRounded()) / Coordinator::UNIT_HP_STEPS;
                for (qint32 oy = 0; oy < height; ++oy)
                {
                    for (qint32 ox = 0; ox < width; ++ox)
                    {
                        if (ox == x && oy == y)
                        {
                            continue;
                        }
                        Unit* pAlly = m_pMap->getTerrain(ox, oy) != nullptr
                            ? m_pMap->getTerrain(ox, oy)->getUnit() : nullptr;
                        if (pAlly != nullptr && pAlly->getOwner() == m_pPlayer && pAlly->hasWeapons() &&
                            Coordinator::manhattanDistance({x, y}, {ox, oy}) <= zoneReach)
                        {
                            supportedCapital +=
                                (Coordinator::toMilliFunds(std::max<qint32>(0, pAlly->getUnitCosts())) *
                                 pAlly->getHpRounded()) / Coordinator::UNIT_HP_STEPS;
                        }
                    }
                }
                const Coordinator::MilliFunds netScore = supportedCapital - unitCost - boardCost;
                if (netScore > bestScore)
                {
                    bestScore = netScore;
                    bestUnit = pUnit;
                }
            }
        }
        if (bestUnit != nullptr)
        {
            spGameAction pAction = MemoryManagement::create<GameAction>(actionId, m_pMap);
            pAction->setTarget(bestUnit->getPosition());
            if (pAction->canBePerformed() && pAction->isFinalStep())
            {
                emit sigPerformAction(pAction);
                return true;
            }
        }
    }
    m_coBoardCheckedDay = currentDay;
    return false;
}

bool CoordinatedAi::tryExecuteUnifiedUnitSpecialActions()
{
    if (m_pMap == nullptr || m_pPlayer == nullptr)
    {
        return false;
    }
    const qint32 width = m_pMap->getMapWidth();
    const qint32 height = m_pMap->getMapHeight();
    const GameRules* pRules = m_pMap->getGameRules();
    const bool specialDestruction = pRules != nullptr && pRules->getSpecialDestruction();
    const Coordinator::MilliFunds playerFunds = Coordinator::toMilliFunds(std::max<qint32>(0, m_pPlayer->getFunds()));

    Coordinator::MilliFunds bestNet = 0;
    spGameAction bestAction;

    for (qint32 y = 0; y < height; ++y)
    {
        for (qint32 x = 0; x < width; ++x)
        {
            Terrain* pOriginTerrain = m_pMap->getTerrain(x, y);
            Unit* pUnit = pOriginTerrain != nullptr ? pOriginTerrain->getUnit() : nullptr;
            if (pUnit == nullptr || pUnit->getOwner() != m_pPlayer || pUnit->getHasMoved() ||
                pUnit->getAiMode() != GameEnums::GameAi_Normal || pUnit->getHpRounded() <= 0)
            {
                continue;
            }
            const QStringList actions = pUnit->getActionList();
            const QPoint origin(x, y);
            const Coordinator::MilliFunds actorCost =
                Coordinator::toMilliFunds(std::max<qint32>(0, pUnit->getUnitCosts()));
            const Coordinator::MilliFunds actorCap =
                (actorCost * pUnit->getHpRounded()) / Coordinator::UNIT_HP_STEPS;

            if (actions.contains(QLatin1String(ACTION_EXPLODE)))
            {
                Interpreter* pInterpreter = Interpreter::getInstance();
                const QJSValue dmgVal = pInterpreter != nullptr
                    ? pInterpreter->doFunction(QLatin1String(ACTION_EXPLODE), QStringLiteral("getDamage")) : QJSValue();
                const QJSValue rngVal = pInterpreter != nullptr
                    ? pInterpreter->doFunction(QLatin1String(ACTION_EXPLODE), QStringLiteral("getRange")) : QJSValue();
                const std::int32_t explodeDmg = std::max<std::int32_t>(
                    1, static_cast<std::int32_t>(std::llround(dmgVal.isNumber() ? dmgVal.toNumber() : 0.0)));
                const std::int32_t explodeRange = std::max<std::int32_t>(
                    1, static_cast<std::int32_t>(std::llround(rngVal.isNumber() ? rngVal.toNumber() : 1.0)));
                UnitPathFindingSystem pfs(m_pMap, pUnit, m_pPlayer);
                pfs.explore();
                for (const QPoint & pt : pfs.getAllNodePointsFast())
                {
                    if (!m_pMap->onMap(pt.x(), pt.y()))
                        continue;
                    const std::vector<QPoint> path = (pt == origin) ? std::vector<QPoint>{origin} : pfs.getPathFast(pt.x(), pt.y());
                    if (path.empty())
                        continue;
                    spGameAction probe = MemoryManagement::create<GameAction>(ACTION_EXPLODE, m_pMap);
                    probe->setTarget(origin);
                    probe->setMovepath(path, pfs.getCosts(path));
                    addSelectedFieldData(probe, pt);
                    if (!probe->canBePerformed() || !probe->isFinalStep())
                        continue;

                    Coordinator::MilliFunds enemyLoss = 0;
                    Coordinator::MilliFunds allyLoss = actorCap;
                    const qint32 y0 = std::max<qint32>(0, pt.y() - explodeRange);
                    const qint32 y1 = std::min<qint32>(height - 1, pt.y() + explodeRange);
                    const qint32 x0 = std::max<qint32>(0, pt.x() - explodeRange);
                    const qint32 x1 = std::min<qint32>(width - 1, pt.x() + explodeRange);
                    for (qint32 ty = y0; ty <= y1; ++ty)
                    {
                        for (qint32 tx = x0; tx <= x1; ++tx)
                        {
                            const qint32 md = std::abs(tx - pt.x()) + std::abs(ty - pt.y());
                            if (md < 1 || md > explodeRange)
                                continue;
                            Unit* pVictim = m_pMap->getTerrain(tx, ty)->getUnit();
                            if (pVictim == nullptr || pVictim == pUnit || pVictim->getHpRounded() <= 0)
                                continue;
                            const Coordinator::MilliFunds vCost =
                                Coordinator::toMilliFunds(std::max<qint32>(0, pVictim->getUnitCosts()));
                            const Coordinator::MilliFunds delta = Coordinator::specialDamageCapitalDelta(
                                vCost, pVictim->getHpRounded(), explodeDmg, specialDestruction);
                            if (m_pPlayer->isEnemyUnit(pVictim))
                                enemyLoss += delta;
                            else
                                allyLoss += delta;
                        }
                    }
                    const Coordinator::MilliFunds net = enemyLoss - allyLoss;
                    if (net > bestNet)
                    {
                        bestNet = net;
                        bestAction = probe;
                    }
                }
            }

            if (actions.contains(QLatin1String(ACTION_HOELLIUM_WAIT)))
            {
                std::vector<QVector3D> enemyFields;
                for (qint32 ey = 0; ey < height; ++ey)
                {
                    for (qint32 ex = 0; ex < width; ++ex)
                    {
                        Unit* pEnemy = m_pMap->getTerrain(ex, ey)->getUnit();
                        if (pEnemy != nullptr && m_pPlayer->isEnemyUnit(pEnemy) && pEnemy->getHpRounded() > 0)
                            enemyFields.emplace_back(ex, ey, 1.0f);
                    }
                }
                if (!enemyFields.empty())
                {
                    TargetedUnitPathFindingSystem tgtPfs(m_pMap, pUnit, enemyFields, &m_MoveCostMap);
                    tgtPfs.explore();
                    const qint32 movePts = pUnit->getMovementpoints(origin);
                    const QPoint crushTile = tgtPfs.getReachableTargetField(movePts);
                    if (m_pMap->onMap(crushTile.x(), crushTile.y()))
                    {
                        Unit* pVictim = m_pMap->getTerrain(crushTile.x(), crushTile.y())->getUnit();
                        if (pVictim != nullptr && m_pPlayer->isEnemyUnit(pVictim) && pVictim->getHpRounded() > 0)
                        {
                            UnitPathFindingSystem turnPfs(m_pMap, pUnit, m_pPlayer);
                            turnPfs.explore();
                            const auto path = turnPfs.getClosestReachableMovePath(crushTile, movePts);
                            if (!path.empty())
                            {
                                spGameAction probe = MemoryManagement::create<GameAction>(ACTION_HOELLIUM_WAIT, m_pMap);
                                probe->setTarget(origin);
                                probe->setMovepath(path, turnPfs.getCosts(path));
                                if (probe->canBePerformed() && probe->isFinalStep())
                                {
                                    const Coordinator::MilliFunds vCost =
                                        Coordinator::toMilliFunds(std::max<qint32>(0, pVictim->getUnitCosts()));
                                    const Coordinator::MilliFunds net =
                                        (vCost * pVictim->getHpRounded()) / Coordinator::UNIT_HP_STEPS;
                                    if (net > bestNet)
                                    {
                                        bestNet = net;
                                        bestAction = probe;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            const auto evalSupportId = [&](const QString & supportId, bool singleTarget, bool freeHeal, bool rationAll) {
                if (!actions.contains(supportId))
                    return;
                UnitPathFindingSystem pfs(m_pMap, pUnit, m_pPlayer);
                pfs.explore();
                for (const QPoint & pt : pfs.getAllNodePointsFast())
                {
                    if (!m_pMap->onMap(pt.x(), pt.y()))
                        continue;
                    const std::vector<QPoint> path = (pt == origin) ? std::vector<QPoint>{origin} : pfs.getPathFast(pt.x(), pt.y());
                    if (path.empty())
                        continue;
                    const qint32 moveCost = pfs.getCosts(path);
                    for (const Coordinator::EntryDirection dir : Coordinator::ENTRY_DIRECTIONS)
                    {
                        const Coordinator::StepVector step = Coordinator::entryStep(dir);
                        const QPoint adj(pt.x() + step.dx, pt.y() + step.dy);
                        if (!m_pMap->onMap(adj.x(), adj.y()))
                            continue;
                        Unit* pAlly = m_pMap->getTerrain(adj.x(), adj.y())->getUnit();
                        if (pAlly == nullptr || pAlly == pUnit || pAlly->getOwner() != m_pPlayer || pAlly->getHpRounded() <= 0)
                            continue;
                        spGameAction probe = MemoryManagement::create<GameAction>(supportId, m_pMap);
                        probe->setTarget(origin);
                        probe->setMovepath(path, moveCost);
                        if (singleTarget)
                            addSelectedFieldData(probe, adj);
                        if (!probe->canBePerformed() || !probe->isFinalStep())
                            continue;

                        const Coordinator::MilliFunds allyCost =
                            Coordinator::toMilliFunds(std::max<qint32>(0, pAlly->getUnitCosts()));
                        const Coordinator::MilliFunds allyLiveCap =
                            (allyCost * pAlly->getHpRounded()) / Coordinator::UNIT_HP_STEPS;
                        Coordinator::MilliFunds gain = 0;
                        if (supportId == QLatin1String(ACTION_SUPPORTSINGLE_FREEREPAIR))
                        {
                            const std::int32_t healSteps = std::min<std::int32_t>(
                                1, std::max<std::int32_t>(0, Coordinator::UNIT_HP_STEPS - pAlly->getHpRounded()));
                            gain = (allyCost * healSteps) / Coordinator::UNIT_HP_STEPS;
                        }
                        else if (supportId == QLatin1String(ACTION_SUPPORTSINGLE_REPAIR))
                        {
                            const std::int32_t healSteps = std::min<std::int32_t>(
                                1, std::max<std::int32_t>(0, Coordinator::UNIT_HP_STEPS - pAlly->getHpRounded()));
                            const Coordinator::MilliFunds repairCost = (allyCost * healSteps) / Coordinator::UNIT_HP_STEPS;
                            if (healSteps > 0 && playerFunds >= repairCost)
                            {
                                // Restoring HP + fuel/ammo on the front avoids paying deployment/supply replacement.
                                gain = needsRefuel(pAlly) ? (allyLiveCap / Coordinator::UNIT_HP_STEPS) : 0;
                            }
                        }
                        else if (freeHeal || rationAll || supportId == QLatin1String(ACTION_SUPPORTSINGLE_SUPPLY))
                        {
                            if (needsRefuel(pAlly))
                                gain = allyLiveCap / Coordinator::UNIT_HP_STEPS;
                        }
                        if (gain > bestNet)
                        {
                            bestNet = gain;
                            bestAction = probe;
                        }
                        if (!singleTarget)
                            break;
                    }
                }
            };
            evalSupportId(QLatin1String(ACTION_SUPPORTSINGLE_FREEREPAIR), true, true, false);
            evalSupportId(QLatin1String(ACTION_SUPPORTSINGLE_REPAIR), true, false, false);
            evalSupportId(QLatin1String(ACTION_SUPPORTSINGLE_SUPPLY), true, false, false);
            evalSupportId(QLatin1String(ACTION_SUPPORTALL_RATION), false, false, true);
            evalSupportId(QLatin1String(ACTION_SUPPORTALL_RATION_MONEY), false, false, true);
        }
    }

    if (bestNet > 0 && bestAction.get() != nullptr && bestAction->isFinalStep() && bestAction->canBePerformed())
    {
        emit sigPerformAction(bestAction);
        return true;
    }
    return false;
}

spGameAction CoordinatedAi::maybeUpgradeMoveToJoinOrStealth(
    const Coordinator::PlannedAction & planned, const spGameAction & baseAction) const
{
    if (m_pMap == nullptr || m_pPlayer == nullptr || baseAction.get() == nullptr)
    {
        return spGameAction();
    }
    if (planned.kind != Coordinator::PlanBundleKind::Wait &&
        planned.kind != Coordinator::PlanBundleKind::Move)
    {
        return spGameAction();
    }
    Unit* pActor = m_pMap->getUnit(planned.unitId);
    if (pActor == nullptr || pActor->getOwner() != m_pPlayer || pActor->getHasMoved())
    {
        return spGameAction();
    }
    const QStringList actorActions = pActor->getActionList();

    if (pActor->getHpRounded() < Coordinator::UNIT_HP_STEPS &&
        pActor->getCapturePoints() == 0 &&
        pActor->getLoadedUnitCount() == 0 &&
        actorActions.contains(QLatin1String(ACTION_JOIN)))
    {
        UnitPathFindingSystem pfs(m_pMap, pActor, m_pPlayer);
        pfs.explore();
        const QPoint origin(pActor->Unit::getX(), pActor->Unit::getY());
        const Coordinator::MilliFunds unitCost =
            Coordinator::toMilliFunds(std::max<qint32>(0, pActor->getUnitCosts()));
        spGameAction bestJoin;
        Coordinator::MilliFunds bestJoinGain = std::max<Coordinator::MilliFunds>(0, planned.marginalValue.economicValue);

        for (const QPoint & pt : pfs.getAllNodePointsFast())
        {
            if (pt == origin || !m_pMap->onMap(pt.x(), pt.y()))
            {
                continue;
            }
            Unit* pPartner = m_pMap->getTerrain(pt.x(), pt.y())->getUnit();
            if (pPartner == nullptr || pPartner == pActor || pPartner->getOwner() != m_pPlayer ||
                !pPartner->getHasMoved() || pPartner->getUnitID() != pActor->getUnitID() ||
                pPartner->getHpRounded() >= Coordinator::UNIT_HP_STEPS ||
                pPartner->getLoadedUnitCount() > 0)
            {
                continue;
            }
            const Coordinator::TilePoint partnerTile{pt.x(), pt.y()};
            if (m_assignment.plan.destinationClaimant(partnerTile) != Coordinator::NO_ACTION)
            {
                continue;
            }
            const std::vector<QPoint> joinPath = pfs.getPathFast(pt.x(), pt.y());
            if (joinPath.empty())
            {
                continue;
            }
            spGameAction joinProbe = MemoryManagement::create<GameAction>(ACTION_JOIN, m_pMap);
            joinProbe->setTarget(origin);
            joinProbe->setMovepath(joinPath, pfs.getCosts(joinPath));
            if (!joinProbe->isFinalStep() || !joinProbe->canBePerformed())
            {
                continue;
            }

            Coordinator::MilliFunds gain = 0;
            const std::int32_t sumHp = pActor->getHpRounded() + pPartner->getHpRounded();
            const std::int32_t overHeal = std::max<std::int32_t>(0, sumHp - Coordinator::UNIT_HP_STEPS);
            const Coordinator::MilliFunds cashRefund = (unitCost * overHeal) / Coordinator::UNIT_HP_STEPS;

            if (pPartner->getCapturePoints() > 0)
            {
                for (const auto & prop : m_properties)
                {
                    if (prop.x == pt.x() && prop.y == pt.y() && prop.capturable)
                    {
                        const std::int32_t oldRate = std::max<std::int32_t>(1, pPartner->getCaptureRate(pt));
                        const std::int32_t combinedHp = std::min<std::int32_t>(Coordinator::UNIT_HP_STEPS, sumHp);
                        const std::int32_t newRate = std::max<std::int32_t>(
                            oldRate, (oldRate * combinedHp) / std::max<std::int32_t>(1, pPartner->getHpRounded()));
                        const std::int32_t beforeT = Coordinator::captureTurnsFor(
                            pPartner->getCapturePoints(), oldRate, Unit::MAX_CAPTURE_POINTS);
                        const std::int32_t afterT = Coordinator::captureTurnsFor(
                            pPartner->getCapturePoints(), newRate, Unit::MAX_CAPTURE_POINTS);
                        const Coordinator::MilliFunds swing = std::max<Coordinator::MilliFunds>(
                            0, Coordinator::incomeSwing(
                                Coordinator::captureIncome(prop), 1,
                                Coordinator::captureOwnerBefore(
                                    prop.ownerId, m_dayStartKnowledge.relation(prop.ownerId)),
                                Coordinator::OwnerSign::Ours));
                        gain += static_cast<Coordinator::MilliFunds>(std::max<std::int32_t>(0, beforeT - afterT)) * swing;
                        break;
                    }
                }
            }

            if (gain > bestJoinGain)
            {
                bestJoinGain = gain;
                bestJoin = joinProbe;
            }
        }
        if (bestJoin.get() != nullptr)
        {
            return bestJoin;
        }
    }

    if (!planned.path.empty())
    {
        const Coordinator::TilePoint dest = planned.destination;
        const Coordinator::TilePoint origin = planned.path.front();
        const qint32 moveFuel = baseAction->getCosts();
        const qint32 reserveFuel = std::max<qint32>(1, pActor->getBaseMovementPoints()) * 2;
        const bool hasFuelBuffer = pActor->getMaxFuel() <= 0 || (pActor->getFuel() - moveFuel) >= reserveFuel;
        if (!pActor->getHidden() && actorActions.contains(QLatin1String(ACTION_STEALTH)))
        {
            Terrain* pDestTerrain = m_pMap->onMap(dest.x, dest.y) ? m_pMap->getTerrain(dest.x, dest.y) : nullptr;
            Building* pDestBuilding = pDestTerrain != nullptr ? pDestTerrain->getBuilding() : nullptr;
            const bool repairingAtDest =
                pDestBuilding != nullptr && pDestBuilding->canRepair(pActor, true) &&
                pActor->canBeRepaired(QPoint(dest.x, dest.y));
            if (!repairingAtDest && hasFuelBuffer)
            {
                spGameAction stealthAction = MemoryManagement::create<GameAction>(ACTION_STEALTH, m_pMap);
                stealthAction->setTarget(QPoint(origin.x, origin.y));
                stealthAction->setMovepath(Coordinator::pathToEngineOrder(planned.path), moveFuel);
                if (stealthAction->isFinalStep() && stealthAction->canBePerformed())
                {
                    return stealthAction;
                }
            }
        }
        else if (pActor->getHidden() && !hasFuelBuffer && actorActions.contains(QLatin1String(ACTION_UNSTEALTH)) &&
                 m_attackOpportunities.attackerSlotsAt(dest.x, dest.y).empty())
        {
            spGameAction unstealthAction = MemoryManagement::create<GameAction>(ACTION_UNSTEALTH, m_pMap);
            unstealthAction->setTarget(QPoint(origin.x, origin.y));
            unstealthAction->setMovepath(Coordinator::pathToEngineOrder(planned.path), moveFuel);
            if (unstealthAction->isFinalStep() && unstealthAction->canBePerformed())
            {
                return unstealthAction;
            }
        }
    }
    return spGameAction();
}
