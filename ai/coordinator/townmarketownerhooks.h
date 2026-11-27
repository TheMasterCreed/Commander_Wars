#pragma once

// Included after the original market types. Views are synchronous owner-thread
// inputs only. A job may retain only verified, owned copies of these values.
namespace Coordinator
{
    struct TownMarketOwnerView
    {
        const CombatTerrain &terrain;
        std::span<const KnownUnit> units;
        const ProductionProjection &production;
        const QJsonArray &orders;
        const StockCaptureSettlement &settled;
        std::span<const ProductionProperty> properties;
        std::span<const TilePoint> positions;
        std::span<const double> hp;
        std::span<const int> fuel;
        std::span<const std::array<int,2>> ammo;
        std::span<const int> freeDay,casualty;
        int guard;
        TilePoint home,away;
        int horizon,cutoff,threshold;
        StockRouteLowerBounds &lower;
        std::span<const MilliFunds> cash;
        std::optional<TownQuoteFocus> focus;
        std::span<const TownQuoteFocus> required;
        std::span<const int> branch;
        bool recordOwnerUpper;
        ScreenOpening screen;
        TownMarketHistory &history;
        TownMarketFrame &frame;
        std::shared_ptr<const TownMarketFrame::Discovery> discovery;
        QByteArray originalRecords;
        std::uint64_t previousRevision;
        std::int64_t offered;
    };

    struct TownMarketOwnerHooks
    {
        virtual ~TownMarketOwnerHooks()=default;
        virtual bool tracking(const TownMarketHistory &history) const=0;
        virtual std::optional<PricedTownEncounter> consume(TownMarketOwnerView &view,std::int64_t &remaining)=0;
        virtual void completed(const TownMarketOwnerView &view,std::int64_t remaining)=0;
        virtual void published(const PricedTownEncounter &result,std::int64_t remaining)=0;
    };
}
