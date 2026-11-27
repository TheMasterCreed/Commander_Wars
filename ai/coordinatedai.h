#ifndef COORDINATEDAI_H
#define COORDINATEDAI_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ai/normalai.h"
#include "ai/coordinator/attackopportunityfield.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockbuilder.h"
#include "ai/coordinator/unifiedproduction.h"

class CoordinatedAi;
using spCoordinatedAi = std::shared_ptr<CoordinatedAi>;

namespace Coordinator
{
    struct MasterShadowConfig;
    class ReplyFactsBuilder;
}

class CoordinatedAi final : public NormalAi
{
    Q_OBJECT
public:
    explicit CoordinatedAi(GameMap* pMap);
    ~CoordinatedAi() override = default;

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    std::uint64_t testPlanSequence() const
    {
        return m_planSequence;
    }

    bool testTierAuditEnabled() const
    {
        return m_testTierAuditMode == Coordinator::SequentialTierAuditMode::Full;
    }

    void setStockControlTestHold(bool held)
    {
        m_testStockControlHeld = held;
    }

    void setRefinementTestControls(std::int64_t workLimit, std::shared_ptr<Coordinator::RelativeStockObserver> observer,
                                  Coordinator::RelativeStockSchedule schedule = Coordinator::RelativeStockSchedule::Current,
                                  qint32 scheduleDay = -1,
                                  Coordinator::SequentialTierAuditMode tierAudit = Coordinator::SequentialTierAuditMode::Full)
    {
        m_testRefinementWorkLimit = workLimit < 0 ? Coordinator::DEFAULT_RELATIVE_STOCK_WORK_LIMIT : workLimit;
        m_testRefinementObserver = std::move(observer);
        m_testRefinementSchedule = schedule;
        m_testRefinementScheduleDay = scheduleDay;
        m_testTierAuditMode = tierAudit;
    }
#endif

public slots:
    void process() override;

signals:
    void sigStockControlStopped(const QString & reason);

private:
    static constexpr qint32 UNBUILT_DAY = -1;

    bool ensureFactLayers();
    void buildFactLayers();
    void ensureTurnPlan();
    void buildTurnPlan();
    void stopStockControl(const Coordinator::MasterShadowConfig & config, const QString & reason);
    bool playStockControlAvailable() const;
    bool localSessionMenu() const;
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    void stopStockReference(const QString & reason);
#endif
    Unit* plannableUnit(const Coordinator::KnownUnit & known, bool changesHeadquartersRoute = false) const;
    std::vector<Coordinator::KnownUnitLink> linkKnownUnits(
        const Coordinator::BattlefieldKnowledge & knowledge) const;
    std::vector<Coordinator::CandidateBundle> candidatesFor(
        const Coordinator::BattlefieldKnowledge & knowledge,
        const Coordinator::AttackOpportunityField & enemyReach,
        std::span<const Coordinator::PropertyFacts> properties,
        const Coordinator::PropertyStockField & propertyStock,
        Coordinator::DamageOracle & oracle,
        qint32 actorUnitIndex,
        Coordinator::BundleBuildStats & stats,
        Coordinator::ReplyFactsBuilder* pReplyFacts,
        std::span<const Coordinator::TilePoint> followTiles = {},
        std::optional<std::size_t>* pTodayCount = nullptr,
        Coordinator::BundleBuildCache* pCache = nullptr);
    bool useStartOfDayCoPower();
    bool executeNextPlannedAction();
    bool buildUnits(spQmlVectorBuilding & pBuildings, spQmlVectorUnit & pUnits,
                    spQmlVectorUnit & pEnemyUnits, spQmlVectorBuilding & pEnemyBuildings) override;
    void invalidateSelectedTeam(qint32 actionIndex,qint32 unit,const QString &reason,bool &replanAllowed);
    bool performPlannedAction(qint32 actionIndex, bool & replanAllowed);
    void failPlannedAction(qint32 actionIndex);
    bool replanFailedAction(qint32 actionIndex);
    void recordCaptureDecisions(const Coordinator::AssignmentInput & input);
    void recordProductionBlocks(
        const Coordinator::AssignmentInput & input);
    const Coordinator::AssignmentResult::Selection* selectionForAction(
        qint32 actionIndex) const;
    bool useBuildingActions(spQmlVectorBuilding & pBuildings, spQmlVectorUnit & pUnits) override;
    bool strictCoordinatedFailureMode() const;
    bool unifiedSpecialActionsActive() const;
    bool tryPrePlanSpecialActions();
    bool tryPostPlanSpecialActions();
    bool tryExecuteUnifiedBuildingActions(bool allowFriendlySplash);
    bool tryExecuteUnifiedSiloAction(bool allowUnmovedFriendlySplash);
    bool tryExecuteUnifiedCoBoarding();
    bool tryExecuteUnifiedUnitSpecialActions();
    spGameAction maybeUpgradeMoveToJoinOrStealth(const Coordinator::PlannedAction & planned,
                                                const spGameAction & baseAction) const;

    Coordinator::BattlefieldKnowledge m_dayStartKnowledge;
    Coordinator::MobilityFieldCache m_mobilityFields;
    Coordinator::AttackOpportunityField m_attackOpportunities;
    std::vector<Coordinator::PropertyFacts> m_properties;
    Coordinator::PropertyStockField m_propertyStock;
    Coordinator::AssignmentResult m_assignment;
    std::size_t m_executionCursor{0};
    enum class TeamSelection { Ordinary, Ranked, FundedClear, StockProgress, FactoryProgram, OpenTactical };
    TeamSelection m_teamSelection{TeamSelection::Ordinary};
    struct ExpectedFireResult
    {
        qint32 shooter, target;
        double shooterHp, targetHp;
    };
    struct ExpectedTerrainFire
    {
        Coordinator::TilePoint tile{Coordinator::INVALID_TILE};
        qint32 shooter{Coordinator::NO_UNIT};
    };
    std::map<qint32,ExpectedFireResult> m_expectedFireResults;
    std::optional<qint32> m_pendingExpectedFire;
    std::optional<ExpectedTerrainFire> m_pendingTerrainFire;
    qint32 m_factLayersDay{UNBUILT_DAY};
    qint32 m_planDay{UNBUILT_DAY};
    qint32 m_turnLuckDay{UNBUILT_DAY};
    quint32 m_turnLuckNonce{0};
    bool m_directUnifiedTurnActive{false};
    bool m_planHasOmniscientPostKill{false};
    qint32 m_coPowerDay{UNBUILT_DAY};
    qint32 m_coBoardCheckedDay{UNBUILT_DAY};
    qint32 m_unifiedProductionDay{UNBUILT_DAY};
    qint32 m_unifiedProductionFunds{0};
    std::size_t m_unifiedOpenSiteCount{0};
    std::vector<Coordinator::ProductionBuildOrder> m_unifiedBuildQueue;
    std::size_t m_unifiedBuildCursor{0};
    std::unique_ptr<Coordinator::DecisionTrace> m_decisionTrace;
    std::uint64_t m_planSequence{0};
    std::int64_t m_factLayersNanos{0};
    std::int64_t m_propertyStockBuildNanos{0};
    std::int64_t m_coPowerCheckNanos{0};
    // Subtracted from planningCallNanos so the measure-only audit never counts as planning time.
    std::int64_t m_captureDangerAuditNanos{0};
    std::int64_t m_replyPassNanos{0};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
    std::int64_t m_testRefinementWorkLimit{Coordinator::DEFAULT_RELATIVE_STOCK_WORK_LIMIT};
    std::shared_ptr<Coordinator::RelativeStockObserver> m_testRefinementObserver;
    Coordinator::RelativeStockSchedule m_testRefinementSchedule{Coordinator::RelativeStockSchedule::Current};
    qint32 m_testRefinementScheduleDay{-1};
    Coordinator::SequentialTierAuditMode m_testTierAuditMode{Coordinator::SequentialTierAuditMode::Full};
    bool m_testStockControlHeld{false};
    bool m_testStockReferenceStopped{false};
#endif
};

#endif // COORDINATEDAI_H
