#pragma once

#include "ai/coordinator/productionforecast.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace Coordinator
{
    struct TownStockKeyHash
    {
        std::size_t operator()(const std::tuple<int,int,int> &key) const noexcept
        {
            constexpr std::uint64_t MIX_A=0x9e3779b97f4a7c15ULL;
            constexpr std::uint64_t MIX_B=0xbf58476d1ce4e5b9ULL;
            constexpr int HASH_SHIFT=30;
            const auto a=static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::get<0>(key)));
            const auto b=static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::get<1>(key)));
            const auto c=static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::get<2>(key)));
            std::uint64_t h=a^(b*MIX_A);
            h^=c*MIX_B;
            h^=h>>HASH_SHIFT;
            return static_cast<std::size_t>(h);
        }
    };

    // The owner can inspect resident values without invoking any native rule.
    // Ordinary function copies retain the original by-value cache semantics.
    template<class Key, class Value, class Return, class... Arguments>
    struct TownNativeMemo
    {
        using Keys=Key;
        using Values=Value;
        static constexpr bool USE_HASH=std::is_same_v<Key,std::tuple<int,int,int>>;
        template<class V> using RebindMap=std::conditional_t<USE_HASH,std::unordered_map<Key,V,TownStockKeyHash>,std::map<Key,V>>;
        using Map=RebindMap<Value>;
        Map values;
        std::function<Return(Map &, Arguments...)> invoke;
        Return operator()(Arguments... args) { return invoke(values, args...); }
    };

    struct TownDefenseGrid
    {
        int width{},height{};
        TerrainDefenseRule base{};
        std::vector<int> tileStars;
        std::vector<std::uint8_t> unitUsesDefense;

        TerrainDefenseRule lookup(int actor,TilePoint tile) const
        {
            TerrainDefenseRule out=base;
            if(actor>=0 && actor<static_cast<int>(unitUsesDefense.size()) &&
               unitUsesDefense[static_cast<std::size_t>(actor)]!=0 &&
               tile.x>=0 && tile.x<width && tile.y>=0 && tile.y<height)
                out.stars=tileStars[static_cast<std::size_t>(tile.y*width+tile.x)];
            return out;
        }
    };

    using TownStockKey=std::tuple<int,int,int>;
    using TownStockWeapon=TownNativeMemo<TownStockKey,double,double,int,int,int>;
    using TownStockSlots=TownNativeMemo<TownStockKey,std::array<double,2>,std::array<double,2>,int,int,int>;
    struct TownStockDefense : TownNativeMemo<TownStockKey,TerrainDefenseRule,TerrainDefenseRule,int,TilePoint>
    {
        std::shared_ptr<const TownDefenseGrid> grid;
    };
    using TownPaidStockKey=std::tuple<int,QString,int,int,int,int>;
    using TownPaidPairKey=std::tuple<int,QString,QString,int,int,int>;
    using TownPaidStock=TownNativeMemo<TownPaidStockKey,ProductionForecastBoard::StockPairPowers,
        std::optional<ProductionForecastBoard::StockPairPowers>,const FundedCaptureGun&,int,int>;
    using TownPaidPair=TownNativeMemo<TownPaidPairKey,std::array<double,2>,std::array<double,2>,
        const FundedCaptureGun&,const FundedCaptureGun&,int>;
    using TownContactKey=std::pair<NativeContactPiece,NativeContactPiece>;
    inline constexpr int townNativeContactTariff=8;
    struct TownContactBinding
    {
        using Values=NativeContactDamage;
        using Keys=TownContactKey;
        template<class V> using RebindMap=std::map<TownContactKey,V>;
        using Map=std::map<TownContactKey,NativeContactDamage>;
        std::shared_ptr<const Map> cached;
        std::function<std::optional<NativeContactDamage>(const NativeContactPiece&,const NativeContactPiece&,std::int64_t&)> invoke;
        std::optional<NativeContactDamage> operator()(const NativeContactPiece &a,const NativeContactPiece &b,std::int64_t &work) const
        { return invoke(a,b,work); }
    };
    struct TownWalkBinding
    {
        std::vector<int> through;
        bool operator()(int actor,int tick,std::int64_t &work) const
        {
            if(work<1)return false;
            --work;
            return actor>=0 && actor<int(through.size()) && tick>=0 && tick<=through[actor];
        }
    };
}
