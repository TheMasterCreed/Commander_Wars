#pragma once

#include "ai/coordinator/planstockvaluer.h"
#include "ai/coordinator/relativestockrefinement.h"

namespace Coordinator
{
    struct AssignmentResult;
    struct AssignmentInput;
    class PropertyStockField;

    struct RelativeStockPlanIdentity
    {
        std::int32_t day{0};
        std::int32_t player{-1};
        std::uint64_t sequence{0};
        std::int32_t horizon{0};
    };

    struct RelativeStockBudgetAudit
    {
        std::int64_t initialAvailable{0};
        std::int64_t totalLimit{0};
        std::int64_t pairLimit{0};
        std::int64_t spent{0};
        std::int64_t remaining{0};
    };

    enum class RelativeStockPhase : std::int8_t
    {
        Pair,
        Joint,
    };

    enum class RelativeStockDomainStatus : std::int8_t
    {
        Complete,
        UnsupportedActors,
        ProductLimit,
        EnumerationBudget,
        RefinementBudget,
        IncompletePlan,
        InvalidQuote,
    };

    struct RelativeStockStats
    {
        std::int64_t candidateProduct{0};
        std::int64_t enumerationStates{0};
        std::int64_t claimableLeaves{0};
        std::int64_t statesUsed{0};
        std::int64_t enemyStatesUsed{0};
        std::int64_t workUsed{0};
        std::int32_t domains{0};
        std::int32_t skippedDomains{0};
        std::int32_t comparisons{0};
        std::int32_t improvements{0};
        std::int32_t unresolved{0};
        std::int32_t budgetExhaustions{0};

        friend bool operator==(const RelativeStockStats &, const RelativeStockStats &) = default;
    };

    struct RelativeStockTiming
    {
        std::int64_t enumerationNanos{0};
        std::int64_t refinementNanos{0};
        std::int64_t replayNanos{0};
    };

    struct RelativeStockDomainAudit
    {
        RelativeStockPhase phase{RelativeStockPhase::Pair};
        RelativeStockDomainStatus status{RelativeStockDomainStatus::Complete};
        std::vector<std::int32_t> actors;
        std::int64_t product{0};
        std::int64_t states{0};
        std::int64_t claimable{0};
        std::int64_t contenders{0};
    };

    struct RelativeStockActionAudit
    {
        std::int32_t actor{NO_UNIT};
        std::int32_t candidate{-1};
        PlanBundleKind kind{PlanBundleKind::Wait};
        TilePoint destination{INVALID_TILE};
        bool captures{false};
    };

    struct RelativeStockFrontierAudit
    {
        RelativeStockPhase phase{RelativeStockPhase::Pair};
        std::int64_t rank{0};
        std::vector<std::int32_t> actors;
        std::vector<std::int32_t> tuple;
        std::vector<RelativeStockActionAudit> actions;
        RelativeStockQuote incumbent;
        RelativeStockQuote challenger;
        RelativeStockComparison comparison;
        bool selected{false};
    };

    class RelativeStockObserver
    {
    public:
        virtual ~RelativeStockObserver() = default;
        virtual void beginPlan(const RelativeStockPlanIdentity &) {}
        virtual void finishPlan(const AssignmentResult &, const RelativeStockBudgetAudit &) {}
        virtual void mixedBoundary(std::int64_t, const LiveRefinementResult &) {}
        virtual bool frontierReceiptsEnabled() const { return false; }
        virtual void model(const AssignmentInput &, const PropertyStockField &) {}
        virtual void beginFrontier(RelativeStockPhase, std::int64_t, std::int64_t) {}
        virtual void contender(const RelativeStockFrontierAudit &) {}
        virtual void domain(const RelativeStockDomainAudit & audit) = 0;
        virtual void step(RelativeStockPhase phase, const RelativeStockStep & step,
                          const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger) = 0;
        virtual void comparison(RelativeStockPhase phase, std::span<const std::int32_t> tuple,
                                const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger,
                                const RelativeStockRefinementResult & result, bool accepted) = 0;
    };
}
