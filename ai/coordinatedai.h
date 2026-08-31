#ifndef COORDINATEDAI_H
#define COORDINATEDAI_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ai/normalai.h"
#include "ai/coordinator/attackopportunityfield.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coordinator/bundleassignment.h"
#include "ai/coordinator/damageoracle.h"
#include "ai/coordinator/mobilityfieldcache.h"
#include "ai/coordinator/propertyeconomics.h"
#include "ai/coordinator/propertystockbuilder.h"

class CoordinatedAi;
using spCoordinatedAi = std::shared_ptr<CoordinatedAi>;

class CoordinatedAi final : public NormalAi
{
    Q_OBJECT
public:
    explicit CoordinatedAi(GameMap* pMap);
    ~CoordinatedAi() override = default;

public slots:
    void process() override;

private:
    static constexpr qint32 UNBUILT_DAY = -1;

    bool ensureFactLayers();
    void buildFactLayers();
    void ensureTurnPlan();
    void buildTurnPlan();
    Unit* plannableUnit(const Coordinator::KnownUnit & known) const;
    std::vector<Coordinator::KnownUnitLink> linkKnownUnits(
        const Coordinator::BattlefieldKnowledge & knowledge) const;
    std::vector<Coordinator::CandidateBundle> candidatesFor(
        const Coordinator::BattlefieldKnowledge & knowledge,
        const Coordinator::AttackOpportunityField & enemyReach,
        std::span<const Coordinator::PropertyFacts> properties,
        const Coordinator::PropertyStockField & propertyStock,
        Coordinator::DamageOracle & oracle,
        qint32 actorUnitIndex,
        Coordinator::BundleBuildStats & stats);
    bool useStartOfDayCoPower();
    bool executeNextPlannedAction();
    bool performPlannedAction(qint32 actionIndex, bool & replanAllowed);
    void failPlannedAction(qint32 actionIndex);
    bool replanFailedAction(qint32 actionIndex);
    void recordCaptureDecisions(const Coordinator::AssignmentInput & input);
    const Coordinator::AssignmentResult::Selection* selectionForAction(
        qint32 actionIndex) const;

    Coordinator::BattlefieldKnowledge m_dayStartKnowledge;
    Coordinator::MobilityFieldCache m_mobilityFields;
    Coordinator::AttackOpportunityField m_attackOpportunities;
    std::vector<Coordinator::PropertyFacts> m_properties;
    Coordinator::PropertyStockField m_propertyStock;
    Coordinator::AssignmentResult m_assignment;
    std::size_t m_executionCursor{0};
    qint32 m_factLayersDay{UNBUILT_DAY};
    qint32 m_planDay{UNBUILT_DAY};
    qint32 m_coPowerDay{UNBUILT_DAY};
    std::unique_ptr<Coordinator::DecisionTrace> m_decisionTrace;
    std::uint64_t m_planSequence{0};
    std::int64_t m_factLayersNanos{0};
    std::int64_t m_propertyStockBuildNanos{0};
    std::int64_t m_coPowerCheckNanos{0};
};

#endif // COORDINATEDAI_H
