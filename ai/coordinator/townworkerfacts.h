#pragma once

#include "ai/coordinator/townnativebindings.h"
#include "ai/coordinator/townworkerfence.h"
#include <bit>

namespace Coordinator
{
    inline bool sameTownFact(double a,double b)
    { return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b); }

    template<class T,std::size_t N> bool sameTownFact(const std::array<T,N> &a,const std::array<T,N> &b)
    {
        for(std::size_t i=0;i<N;++i)if(!sameTownFact(a[i],b[i]))return false;
        return true;
    }

    inline bool sameTownFact(const TerrainDefenseRule &a,const TerrainDefenseRule &b)
    {
        return a.stars==b.stars && a.defensePerStar==b.defensePerStar && a.hpScaled==b.hpScaled &&
               a.formula==b.formula && sameTownFact(a.luck,b.luck);
    }

    inline bool sameTownFact(const NativeContactDamage &a,const NativeContactDamage &b)
    {
        return a.weapon==b.weapon && a.counterWeapon==b.counterWeapon && sameTownFact(a.attack,b.attack) &&
               sameTownFact(a.counter,b.counter);
    }

    template<class Binding> struct FrozenTownTable
    {
        using Key=typename Binding::Keys;
        using Value=typename Binding::Values;
        struct Fact {Value value;bool read{};};
        typename Binding::template RebindMap<Fact> frozen;
        bool available{};

        template<class Fn> void refresh(const Fn &fn)
        {
            available=bool(fn);frozen.clear();
            const auto *owner=fn.template target<Binding>();
            if(!owner)return;
            const auto *map=source(*owner);
            if(!map)return;
            for(const auto &[k,v]:*map)frozen.emplace(k,Fact{v});
        }

        Value read(const Key &key,const char *gap)
        {
            const auto it=frozen.find(key);
            if(it==frozen.end())throw TownWorkerGap{gap};
            it->second.read=true;
            return it->second.value;
        }

        template<class Fn> bool matches(const Fn &fn) const
        {
            if(bool(fn)!=available)return false;
            const auto *owner=fn.template target<Binding>();
            const auto *map=owner?source(*owner):nullptr;
            for(const auto &[k,v]:frozen)if(v.read)
            {
                if(!map)return false;
                const auto at=map->find(k);
                if(at==map->end() || !sameTownFact(v.value,at->second))return false;
            }
            return true;
        }

    private:
        static const typename Binding::Map *source(const Binding &b)
        {
            if constexpr(std::is_same_v<Binding,TownContactBinding>)return b.cached.get();
            else return &b.values;
        }
    };

    struct FrozenTownReads
    {
        FrozenTownTable<TownStockWeapon> weapon;
        FrozenTownTable<TownStockSlots> slotsAtRange;
        FrozenTownTable<TownStockDefense> defense;
        std::shared_ptr<const TownDefenseGrid> defenseGrid;
        FrozenTownTable<TownPaidStock> paidStock;
        FrozenTownTable<TownPaidPair> paidPair;
        FrozenTownTable<TownContactBinding> contact;
        std::optional<TownWalkBinding> walk;
        bool walkPresent{},walkRead{},repairPresent{},hasProduction{};

        // Called only by the owner, before dispatch. Native/cached Unit objects
        // and the callbacks that can touch them never enter these maps.
        void refresh(const CombatTerrain &t)
        {
            weapon.refresh(t.weapon);slotsAtRange.refresh(t.weaponSlots);defense.refresh(t.defense);
            defenseGrid.reset();
            if(const auto *owner=t.defense.target<TownStockDefense>())defenseGrid=owner->grid;
            walkPresent=bool(t.certifyRetainedWalkProfile);walkRead=false;walk.reset();
            if(const auto *known=t.certifyRetainedWalkProfile.target<TownWalkBinding>())walk=*known;
            repairPresent=bool(t.canRepairOnTerrain);hasProduction=bool(t.production);
            if(t.production)
            {
                paidStock.refresh(t.production->paidStockSlots);paidPair.refresh(t.production->paidWeaponSlots);
                contact.refresh(t.production->contactDamage);
            }
        }

        bool matches(const CombatTerrain &t) const
        {
            if(hasProduction!=bool(t.production) || repairPresent!=bool(t.canRepairOnTerrain) ||
                walkPresent!=bool(t.certifyRetainedWalkProfile) || !weapon.matches(t.weapon) ||
                !slotsAtRange.matches(t.weaponSlots) || !defense.matches(t.defense))return false;
            if(defenseGrid)
            {
                const auto *owner=t.defense.target<TownStockDefense>();
                if(!owner || owner->grid!=defenseGrid)return false;
            }
            if(walkRead)
            {
                const auto *source=t.certifyRetainedWalkProfile.target<TownWalkBinding>();
                if(!source || !walk || source->through!=walk->through)return false;
            }
            return !t.production || (paidStock.matches(t.production->paidStockSlots) &&
                paidPair.matches(t.production->paidWeaponSlots) && contact.matches(t.production->contactDamage));
        }
    };

    // Ordinary pricing can create a temporary copy of the menu for a paid
    // defender. Its worker callbacks resolve only this job's owning read set.
    inline thread_local FrozenTownReads *activeTownReads{};
    inline FrozenTownReads &workerTownReads()
    {
        if(!activeTownReads)throw TownWorkerGap{"NO_ACTIVE_TOWN_JOB"};
        return *activeTownReads;
    }
}
