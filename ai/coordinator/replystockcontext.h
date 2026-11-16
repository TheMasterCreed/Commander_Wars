#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ai/coordinator/masterassignmentfuture.h"
#include "ai/coordinator/masterplanpricing.h"
#include "ai/coordinator/propertystockbuilder.h"

namespace Coordinator
{
    enum class ReplyStockStatus : std::int8_t
    {
        Valid,
        InvalidKey,
        PreparationFailed,
        NoPrices,
        Invalid,
        ArithmeticFailure,
        LimitReached,
        ReplayRejected,
        Inverted,
    };

    inline constexpr std::array REPLY_STOCK_STATUS_NAMES{
        "VALID", "INVALID_KEY", "PREPARATION_FAILED", "NO_PRICES", "INVALID", "ARITHMETIC_FAILURE", "LIMIT_REACHED",
        "REPLAY_REJECTED", "INVERTED",
    };

    enum class ReplyStockFloorSource : std::int8_t
    {
        None,
        Prefix,
        Tuner,
    };

    inline constexpr std::array REPLY_STOCK_FLOOR_SOURCE_NAMES{"NONE", "PREFIX", "TUNER"};

    enum class ReplyStockFloorPolish : std::int8_t
    {
        None,
        Unchanged,
        Improved,
        LimitReached,
        Rejected,
    };

    inline constexpr std::array REPLY_STOCK_FLOOR_POLISH_NAMES{"NONE", "UNCHANGED", "IMPROVED", "LIMIT_REACHED",
                                                               "REJECTED"};

    enum class ReplyStockTuneStart : std::int8_t
    {
        Zero,
        InstalledPrices,
    };

    struct ReplyStockPriceInstall
    {
        ReplyStockStatus status{ReplyStockStatus::NoPrices};
        std::uint64_t version{0};
    };

    struct ReplyStockCounters
    {
        std::int64_t actionsAdded{0};
        std::int64_t classesAdded{0};
        std::int64_t classTablesPriced{0};
        std::int64_t rowScans{0};
        std::int64_t tableHits{0};
        std::int64_t rowHits{0};
    };

    struct ReplyStockCeiling
    {
        ReplyStockStatus status{ReplyStockStatus::NoPrices};
        MilliFunds ceiling{0};
        std::uint64_t priceVersion{0};
        ReplyStockCounters counters;
        std::int64_t work{0};
    };

    struct ReplyStockOwned
    {
        ReplyStockStatus status{ReplyStockStatus::InvalidKey};
        MilliFunds owned{0};
    };

    struct ReplyStockFloorCounters
    {
        std::int64_t reusedRows{0};
        std::int64_t prefixRows{0};
        std::int64_t fallbackRows{0};
        std::int64_t emptyRows{0};
        std::int64_t steps{0};
        std::int64_t polishedRows{0};
        std::int64_t adaptedRows{0};
    };

    struct ReplyStockFloor
    {
        ReplyStockStatus status{ReplyStockStatus::NoPrices};
        MilliFunds floor{0};
        ContinuationKey key;
        std::vector<SequentialRowWitness> witness;
        std::uint64_t priceVersion{0};
        ReplyStockFloorCounters counters;
        ReplyStockFloorPolish polish{ReplyStockFloorPolish::None};
        ReplyStockCounters pricing;
        std::int64_t work{0};
    };

    inline constexpr MilliFunds REPLY_STOCK_PARTS_PER_MILLION = 1'000'000;

    // Splitting the division keeps the product from overflowing for any non-negative ceiling.
    constexpr std::optional<MilliFunds> replyStockTargetGap(MilliFunds ceiling, MilliFunds ppm)
    {
        if (ppm < 0 || ppm > REPLY_STOCK_PARTS_PER_MILLION)
        {
            return std::nullopt;
        }
        const MilliFunds base = ceiling < 0 ? 0 : ceiling;
        return base / REPLY_STOCK_PARTS_PER_MILLION * ppm +
            base % REPLY_STOCK_PARTS_PER_MILLION * ppm / REPLY_STOCK_PARTS_PER_MILLION;
    }

    struct ReplyStockTuneLimits
    {
        std::int64_t evaluationWork{0};
        std::int64_t pricingWork{0};
        std::int64_t buildWork{0};
        std::int64_t builds{0};
        MasterPlanPricingLimits limits;
    };

    struct ReplyStockTune
    {
        ReplyStockStatus status{ReplyStockStatus::Invalid};
        ReplyStockTuneStart start{ReplyStockTuneStart::Zero};
        MilliFunds targetGap{0};
        MasterPlanPricingStatus tunerStatus{MasterPlanPricingStatus::Unavailable};
        std::int32_t rounds{0};
        bool stoppedOnTarget{false};
        MilliFunds ceiling{0};
        ReplyStockFloor floor;
        ReplyStockFloorSource floorSource{ReplyStockFloorSource::None};
        ReplyStockTuneLimits limits;
        std::int64_t tunerWork{0};
#ifdef COW_COORDINATED_REFINEMENT_TESTING
        std::int64_t tunerPricingWork{0};
        std::int64_t tunerWitnessWork{0};
        MasterPlanWitnessStats witnessStats;
#endif
        std::int64_t work{0};
    };

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    struct DiagnosticPricingModels
    {
        std::size_t classes{0};
        std::vector<std::optional<MasterPlanPricingModel>> models;
        // The built catalog, its action rows as built and before any captured node is cleared.
        std::shared_ptr<const MasterFutureCatalog> catalog;
        LaterListMeasure laterList;
    };
#endif

    // Prices our side of one turn's reply keys on a private copy of the field; never touches a live session.
    class ReplyStockContext final
    {
    public:
        explicit ReplyStockContext(const PropertyStockField & field);

        bool valid() const
        {
            return m_valid;
        }

        ReplyStockPriceInstall installPrices(std::span<const MilliFunds> prices);
        ReplyStockCeiling ceiling(const ContinuationKey & key);
        ReplyStockOwned owned(const ContinuationKey & key);
        ReplyStockFloor floor(const ContinuationKey & key, const ReplyStockFloor * reference = nullptr);
        // Needs no prices: a witness's value depends only on the key's rows.
        ReplyStockFloor replayWitness(const ContinuationKey & key, std::span<const SequentialRowWitness> witness);
        ReplyStockTune tune(const ContinuationKey & key, std::int32_t rounds,
                            ReplyStockTuneStart start = ReplyStockTuneStart::Zero,
                            const ReplyStockFloor * reference = nullptr, MilliFunds targetGap = 0);

        std::size_t nodeCount() const
        {
            return m_nodeSlots.size();
        }

        std::size_t classCount() const
        {
            return m_classTables.size();
        }

        std::size_t actionCount() const
        {
            return m_actions.size();
        }

        std::size_t rowCount() const
        {
            return m_dayRows.size();
        }

        std::uint64_t priceVersion() const
        {
            return m_priceVersion;
        }

        // Any price installation, including the one tune makes, invalidates this view.
        std::span<const MilliFunds> installedPrices() const
        {
            return m_prices;
        }

        std::int64_t work() const
        {
            return m_meter.stats().granted;
        }

    private:
        struct PricedRow
        {
            MilliFunds upper{0};
            std::vector<std::int32_t> chain;
        };

        struct ActionEntry
        {
            SequentialRow row;
            std::int32_t capturedColumn{NO_STOCK_COLUMN};
            std::int32_t capturedNode{NO_SEQUENTIAL_NODE};
            std::optional<PricedRow> priced;
        };

        ReplyStockStatus extend(const ContinuationKey & key, ReplyStockCounters & counters);
        ReplyStockStatus claimKey(const ContinuationKey & key, std::vector<bool> & capturedNodes,
                                  std::vector<bool> & acting) const;
        ReplyStockStatus keyInstance(const ContinuationKey & key, SequentialInstance & instance) const;
        ReplyStockStatus composeCeiling(const ContinuationKey & key, ReplyStockCeiling & result);
        ReplyStockStatus composeFloor(const ContinuationKey & key, const ReplyStockFloor * reference,
                                      ReplyStockFloor & result);
        ReplyStockStatus tuneKey(const ContinuationKey & key, std::int32_t rounds, const ReplyStockFloor * reference,
                                 ReplyStockTune & result);
        std::optional<MasterPlanPricingModel> forcedModel(const ContinuationKey & key) const;
        ReplyStockStatus tuneLimits(const MasterPlanPricingModel & model, std::int32_t rounds,
                                    ReplyStockTuneLimits & result);
        bool chargeFloor(std::size_t work);
        ReplyStockStatus publishFloor(const SequentialInstance & instance,
                                      std::span<const SequentialRowWitness> source, ReplyStockFloor & result);
        void polishFloor(const ContinuationKey & key, const SequentialInstance & instance,
                         const std::vector<bool> & reused, ReplyStockFloor & result);
        ReplyStockStatus rowUpper(const SequentialRow & row, std::optional<PricedRow> & cached,
                                  ReplyStockCounters & counters);
        ReplyStockStatus priceClass(std::int32_t classIndex, ReplyStockCounters & counters);

        std::unique_ptr<PropertyStockField> m_field;
        std::vector<std::int32_t> m_nodeSlots;
        std::vector<PropertyStockColumn> m_nodeColumns;
        std::vector<SequentialClassKey> m_classKeys;
        std::vector<SequentialClassTable> m_classTables;
        std::vector<std::int32_t> m_classOfRow;
        std::vector<SequentialRow> m_dayRows;
        std::map<CanonicalPlanActionKey, ActionEntry> m_actions;
        std::vector<MilliFunds> m_prices;
        std::uint64_t m_priceVersion{0};
        std::vector<MasterFuturePricedClass> m_pricedClasses;
        std::vector<bool> m_classPriced;
        std::vector<std::optional<PricedRow>> m_pricedDayRows;
        RefinementLedger m_meter;
        bool m_valid{false};
    };
}
