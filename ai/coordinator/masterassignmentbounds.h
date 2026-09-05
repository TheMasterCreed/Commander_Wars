#pragma once

#include "ai/coordinator/continuationpricing.h"
#include "ai/coordinator/masterassignmentstate.h"
#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/masterassignmentprofile.h"

namespace Coordinator
{
    enum class MasterBoundFamily : std::int8_t
    {
        Immediate,
        CurrentResource,
        EndpointCoupled,
        CombinedCurrentFuture,
    };

    enum class MasterBoundStatus : std::int8_t
    {
        Valid,
        Unavailable,
        InvalidIdentity,
        InvalidCertificate,
        ArithmeticFailure,
        BudgetExhausted,
    };

    struct MasterBoundCertificate
    {
        MasterAssignmentState state;
        std::weak_ptr<const std::uint8_t> basis;
        MasterBoundFamily family{MasterBoundFamily::Immediate};
        std::vector<MilliFunds> prices;
        MilliFunds upper{0};
    };

    struct MasterBoundResult
    {
        MasterBoundStatus status{MasterBoundStatus::Unavailable};
        std::optional<MasterBoundCertificate> certificate;

        bool valid() const
        {
            return status == MasterBoundStatus::Valid && certificate.has_value();
        }
    };

    struct MasterFutureCacheStats
    {
        std::uint64_t hits{0};
        std::uint64_t recomputations{0};
    };

    class MasterAssignmentBounds final
    {
    public:
        explicit MasterAssignmentBounds(const MasterAssignmentProblem & problem, MasterAssignmentProfile* profile = nullptr);

        MasterBoundStatus prepare(const CheapPricingModel* stock, RefinementLedger & ledger,
                                  const MasterFutureCatalog* future = nullptr);
        MasterBoundResult bound(const MasterAssignmentState & state, MasterBoundFamily family,
                                RefinementLedger & ledger) const;
        MasterBoundResult verify(const MasterBoundCertificate & certificate,
                                 RefinementLedger & ledger) const;
        MasterBoundResult tighten(const MasterBoundCertificate & initial, std::int32_t rounds,
                                  RefinementLedger & ledger) const;

        std::optional<MilliFunds> contribution(std::size_t actor, std::int32_t candidate) const;
        bool ready() const;
        MasterFutureCacheStats futureCacheStats() const;

    private:
        struct Option
        {
            std::optional<MilliFunds> economic;
            std::optional<MilliFunds> combined;
            std::vector<std::int32_t> resources;
            std::optional<std::size_t> futureAction;
            MilliFunds captureUpper{0};
        };

        struct ScanResult
        {
            MasterBoundStatus status{MasterBoundStatus::Unavailable};
            MilliFunds upper{0};
            std::vector<std::int64_t> demand;
        };

        struct PreparedOption
        {
            MasterBoundStatus status{MasterBoundStatus::Valid};
            Option option;
            MilliFunds stockUpper{0};
        };

        enum class ResourceScope : std::int8_t
        {
            Current,
            Future,
        };

        struct Resource
        {
            bool priceable{false};
            MilliFunds economicScale{1};
            MilliFunds combinedScale{1};
            ResourceScope scope{ResourceScope::Current};
        };

        ScanResult scan(const MasterBoundCertificate & certificate, RefinementLedger & ledger,
                        RefinementWork category, bool collectDemand) const;
        PreparedOption prepareOption(const MasterActor & actor, const MasterCandidate & candidate,
                                     bool included, const CheapPricingModel* stock,
                                     RefinementLedger & ledger) const;
        MasterBoundStatus scanActors(const MasterBoundCertificate & certificate,
                                     const MasterFuturePricing & future, RefinementLedger & ledger,
                                     RefinementWork category, bool collectDemand, ScanResult & result) const;
        MasterFutureStatus futurePrices(std::span<const MilliFunds> prices, RefinementLedger & ledger,
                                        RefinementWork category,
                                        std::shared_ptr<const MasterFuturePricing> & result) const;
        bool matches(const MasterBoundCertificate & certificate) const;

        const MasterAssignmentProblem & m_problem;
        MasterAssignmentProfile* m_profile;
        MasterAssignmentIdentity m_problemIdentity;
        std::shared_ptr<const std::uint8_t> m_basis;
        std::vector<std::vector<Option>> m_options;
        std::vector<Resource> m_resources;
        const MasterFutureCatalog* m_future{nullptr};
        std::vector<bool> m_actorRows;
        MilliFunds m_ownedBaseline{0};
        MilliFunds m_stockConstant{0};
        MilliFunds m_globalStockUpper{0};
        bool m_ready{false};
        mutable std::vector<MilliFunds> m_cachedFuturePrices;
        mutable std::shared_ptr<const MasterFuturePricing> m_cachedFuturePricing;
        mutable MasterFutureCacheStats m_futureCacheStats;
    };
}
