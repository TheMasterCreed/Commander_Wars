#pragma once

#include <memory>

#include <QVariantList>
#include <QVariantMap>

#include "ai/coordinator/relativestockaudit.h"
#include "ai/coordinator/sequentialthreshold.h"

class ArenaRelativeStockSupportAudit;

class ArenaRelativeStockProbe final : public Coordinator::RelativeStockObserver
{
public:
    explicit ArenaRelativeStockProbe(bool frontierReceipts = false, qint32 frontierDay = -1);
    ~ArenaRelativeStockProbe() override;
    bool loadValuations(const QString & path, QString & error);
    void beginPlan(const Coordinator::RelativeStockPlanIdentity & identity) override;
    bool frontierReceiptsEnabled() const override;
    void model(const Coordinator::AssignmentInput & input, const Coordinator::PropertyStockField & field) override;
    void beginFrontier(Coordinator::RelativeStockPhase phase, std::int64_t count, std::int64_t work) override;
    void contender(const Coordinator::RelativeStockFrontierAudit & audit) override;
    void finishPlan(const Coordinator::AssignmentResult & result, const Coordinator::RelativeStockBudgetAudit & budget) override;
    void mixedBoundary(std::int64_t maximumWork, const Coordinator::LiveRefinementResult & result) override;
    void domain(const Coordinator::RelativeStockDomainAudit & audit) override;
    void step(Coordinator::RelativeStockPhase phase, const Coordinator::RelativeStockStep & step,
              const Coordinator::RelativeStockQuote & incumbent, const Coordinator::RelativeStockQuote & challenger) override;
    void comparison(Coordinator::RelativeStockPhase phase, std::span<const std::int32_t> tuple,
                    const Coordinator::RelativeStockQuote & incumbent, const Coordinator::RelativeStockQuote & challenger,
                    const Coordinator::RelativeStockRefinementResult & result, bool accepted) override;
    QVariantMap result() const;

private:
    struct Model;
    struct Valuation
    {
        enum class Operation : std::int8_t
        {
            Comparison,
            Threshold,
        };

        QString name;
        std::vector<std::int32_t> incumbent;
        std::vector<std::int32_t> challenger;
        std::vector<std::int64_t> challengerCaps;
        std::int64_t prefixWork{0};
        Coordinator::RelativeStockSlice slice{Coordinator::RelativeStockSlice::Direct};
        Coordinator::SequentialCacheAccounting cacheAccounting{
            Coordinator::SequentialCacheAccounting::
                RechargeEveryLookup
        };
        Coordinator::SequentialThresholdOptions thresholdOptions;
        Operation operation{Operation::Comparison};
    };

    QVariantMap evaluate(const Valuation & request) const;
    void append(QVariantMap record);
    std::unique_ptr<Model> m_model;
    std::unique_ptr<ArenaRelativeStockSupportAudit> m_supportAudit;
    std::vector<Valuation> m_valuations;
    QVariantMap m_identity;
    QVariantList m_records;
    bool m_frontierReceipts{false};
    qint32 m_frontierDay{-1};
    qint64 m_frontierIndex{0};
    qint64 m_frontierRecords{0};
    qint64 m_droppedFrontierRecords{0};
    QVariantList m_selectedTuple;
};
