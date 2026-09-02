#pragma once

#include <atomic>
#include <memory>

#include "ai/coordinator/packingdualcertificate.h"
#include "ai/coordinator/relativestockcomparison.h"

namespace Coordinator
{
    class TurnPlan;

    struct RelativeStockRefinedTerm
    {
        StockInterval interval;
        std::int64_t statesUsed{0};
        std::int64_t workUsed{0};
        bool searchCompleted{false};
        bool lowerWitnessReplays{false};
        bool budgetExhausted{false};
        bool valid{false};
    };

    struct RelativeStockDualTightening
    {
        StockInterval intervalBefore;
        StockInterval intervalAfter;
        MilliFunds certifiedUpper{0};
        std::shared_ptr<const PackingDualCertificate> certificate;
        std::uint64_t certificateId{0};
        std::int64_t optimizerWork{0};
        std::int64_t verifierWork{0};
        std::int64_t catalogWork{0};
        std::int64_t tariffWork{0};
        std::int64_t totalWork{0};
        bool catalogComplete{false};
        bool certificateValid{false};
        bool optimizerCompleted{false};
        bool verifierCompleted{false};
        bool reused{false};
        bool locallyBudgetExhausted{false};
        bool globalFailure{false};

        bool tightened() const
        {
            return certificateValid &&
                intervalAfter.upper < intervalBefore.upper;
        }
    };

    class RelativeStockSession
    {
    public:
        virtual ~RelativeStockSession() = default;
        RelativeStockSession(const RelativeStockSession &) = delete;
        RelativeStockSession & operator=(const RelativeStockSession &) = delete;
        RelativeStockSession(RelativeStockSession &&) = delete;
        RelativeStockSession & operator=(RelativeStockSession &&) = delete;

        virtual RelativeStockQuote quote(const TurnPlan & plan, MilliFunds economic) = 0;
        virtual RelativeStockQuote quote(const ContinuationKey & key, MilliFunds economic) = 0;
        virtual RelativeStockRefinedTerm refineOur(const ContinuationKey & key, std::int64_t stateCap) = 0;
        virtual RelativeStockRefinedTerm refineEnemy(
            const std::vector<std::int32_t> & capturedColumns, std::int64_t stateCap) = 0;
        virtual bool supportsVerifiedDual() const
        {
            return false;
        }
        virtual RelativeStockDualTightening
        tightenOurUpperWithVerifiedDual(
            const RelativeStockQuote & quote,
            std::int64_t maximumWork)
        {
            RelativeStockDualTightening result;
            result.intervalBefore = quote.ours;
            result.intervalAfter = quote.ours;
            result.locallyBudgetExhausted = maximumWork <= 0;
            return result;
        }
        virtual bool replayOurWitness(const RelativeStockQuote & quote) = 0;
        virtual bool replayEnemyWitness(const RelativeStockQuote & quote) = 0;
        virtual std::int64_t remainingWork() const = 0;
        virtual std::int64_t ourRerunWork() const = 0;
        virtual std::int64_t enemyRerunWork(const std::vector<std::int32_t> & capturedColumns) const = 0;

        RelativeStockComparison compare(
            const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger) const
        {
            if (!owns(incumbent) || !owns(challenger) ||
                !verifiedUpperMatches(incumbent) ||
                !verifiedUpperMatches(challenger))
            {
                return {};
            }
            return compareRelativeStock(incumbent, challenger);
        }

        bool owns(const RelativeStockQuote & quote) const
        {
            return identity().sameLiveModel(quote.identity);
        }

    protected:
        RelativeStockSession(const PropertyStockField & snapshot, const PropertyStockInstance & enemyModel)
            : m_binding(std::make_shared<const RelativeStockIdentity::Binding>(
                  RelativeStockIdentity::Binding{&snapshot, &enemyModel}))
        {
        }

        RelativeStockIdentity identity() const
        {
            return RelativeStockIdentity(m_binding);
        }

        std::uint64_t sessionIdentity() const
        {
            return m_sessionIdentity;
        }

        virtual bool verifiedUpperMatches(
            const RelativeStockQuote & quote) const
        {
            return quote.ourUpperCertificateId == 0;
        }

    private:
        static std::uint64_t nextSessionIdentity()
        {
            static std::atomic<std::uint64_t> next{1};
            return next.fetch_add(1, std::memory_order_relaxed);
        }

        const std::shared_ptr<const RelativeStockIdentity::Binding> m_binding;
        const std::uint64_t m_sessionIdentity{
            nextSessionIdentity()
        };
    };
}
