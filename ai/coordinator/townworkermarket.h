#pragma once

#include "ai/coordinator/datedformulaledger.h"
#include "ai/coordinator/townassaultintegration.h"
#include "ai/coordinator/townworkerfacts.h"

namespace Coordinator
{
    struct FrozenTownCatalog
    {
        std::shared_ptr<ProductionForecastBoard> menu=std::make_shared<ProductionForecastBoard>();
        std::map<const ProductionChoice *,const ProductionChoice *> blueprint;
    };

    inline bool sameTownOpeningPurchases(const std::optional<std::vector<FixedProductionPurchase>> &a,
        const std::optional<std::vector<FixedProductionPurchase>> &b)
    {
        if(bool(a)!=bool(b))return false;
        if(!a)return true;
        return a->size()==b->size() && std::equal(a->begin(),a->end(),b->begin(),[](const auto &x,const auto &y){
            return x.factory==y.factory && x.type==y.type && x.price==y.price;
        });
    }

    inline bool sameTownCatalogProjection(const ProductionForecastBoard &a,const ProductionForecastBoard &b)
    {
        return a.funds==b.funds && a.availableUnits==b.availableUnits && a.matureFactoryCells==b.matureFactoryCells &&
            a.resolvedOpeningFireSite==b.resolvedOpeningFireSite && sameTownOpeningPurchases(a.friendlyOpening,b.friendlyOpening) &&
            bool(a.friendlyDayStart)==bool(b.friendlyDayStart) && bool(a.canProposeOpening)==bool(b.canProposeOpening) &&
            bool(a.paidStockSlots)==bool(b.paidStockSlots) && bool(a.paidWeaponSlots)==bool(b.paidWeaponSlots) &&
            bool(a.contactDamage)==bool(b.contactDamage);
    }

    inline std::shared_ptr<const FrozenTownCatalog> freezeTownCatalog(const std::shared_ptr<const ProductionForecastBoard> &source)
    {
        if(!source)return {};
        // Only the owning immutable catalog is reused. Funding, dates and
        // wounded individual units are supplied separately by each request.
        static thread_local std::weak_ptr<const ProductionForecastBoard> previousOwner;
        static thread_local std::vector<std::weak_ptr<const FrozenTownCatalog>> previous;
        if(previousOwner.lock()!=source){previous.clear();previousOwner=source;}
        std::erase_if(previous,[](const auto &p){return p.expired();});
        for(const auto &weak:previous)if(auto prior=weak.lock();prior && sameTownCatalogProjection(*prior->menu,*source))return prior;
        auto copy=std::make_shared<FrozenTownCatalog>();
        auto &model=*copy->menu;
        model.funds=source->funds;model.availableUnits=source->availableUnits;model.sites=source->sites;
        model.matureFactoryCells=source->matureFactoryCells;model.friendlyOpening=source->friendlyOpening;
        model.resolvedOpeningFireSite=source->resolvedOpeningFireSite;
        if(source->canProposeOpening)model.canProposeOpening=[](const ProductionSite&,const ProductionChoice&)->bool{
            throw TownWorkerGap{"OWNER_OPENING_PROPOSAL"};};
        if(source->friendlyDayStart)model.friendlyDayStart=[](const auto&...)->std::optional<FriendlyServiceResult>{
            throw TownWorkerGap{"OWNER_FRIENDLY_SERVICE"};};
        for(std::size_t s=0;s<source->sites.size();++s)for(int side=0;side<2;++side)
        {
            const auto &from=source->sites[s].menu[side];const auto &to=model.sites[s].menu[side];
            for(std::size_t k=0;k<from.size();++k)copy->blueprint.emplace(&from[k],&to[k]);
        }
        if(source->paidStockSlots)
            model.paidStockSlots=[](const FundedCaptureGun &a,int stock,int distance)->std::optional<ProductionForecastBoard::StockPairPowers>{
                if(!a.unit)throw TownWorkerGap{"UNKNOWN_PAID_STOCK_UNIT"};
                return workerTownReads().paidStock.read({a.side,a.unit->type,stock,distance,a.unit->minRange,a.unit->maxRange},
                                                        "UNSEEN_PAID_STOCK_SLOTS");
            };
        if(source->paidWeaponSlots)
            model.paidWeaponSlots=[](const FundedCaptureGun &a,const FundedCaptureGun &v,int distance){
                if(!a.unit || !v.unit)throw TownWorkerGap{"UNKNOWN_PAID_PAIR_UNIT"};
                return workerTownReads().paidPair.read({a.side,a.unit->type,v.unit->type,distance,a.unit->minRange,a.unit->maxRange},
                                                       "UNSEEN_PAID_PAIR_SLOTS");
            };
        if(source->contactDamage)
            model.contactDamage=[](const NativeContactPiece &a,const NativeContactPiece &b,std::int64_t &work)->std::optional<NativeContactDamage>{
                if(work<townNativeContactTariff){work=0;return {};}
                work-=townNativeContactTariff;
                return workerTownReads().contact.read({a,b},"UNSEEN_NATIVE_CONTACT");
            };
        if(previous.size()<8)previous.emplace_back(copy);
        return copy;
    }

    struct FrozenTownMarketRequest
    {
        FrozenTownMarketRequest()=default;
        FrozenTownMarketRequest(const FrozenTownMarketRequest &)=delete;
        FrozenTownMarketRequest &operator=(const FrozenTownMarketRequest &)=delete;
        CombatTerrain terrain;
        FrozenTownReads reads;
        std::shared_ptr<const FrozenTownCatalog> catalog;
        std::weak_ptr<const ProductionForecastBoard> sourceMenu;
        std::map<const MobilityCostGrid *,const MobilityCostGrid *> gridMap;
        std::map<const MobilityCostGrid *,int> initialMinimum;
        std::optional<ScreenRuleDemand> initialScreen;
        std::shared_ptr<const ScreenFireProfile> initialFire;
        std::optional<OrderedOpeningPolicy> opening;
        std::vector<KnownUnit> units;
        ProductionProjection production;
        QJsonArray orders;
        StockCaptureSettlement settled;
        std::vector<ProductionProperty> properties;
        std::vector<TilePoint> positions,screenPositions;
        std::vector<double> hp,screenHp,screenExactHp;
        std::vector<int> fuel,freeDays,casualties,screenCarry,screenSteps,screenFuel;
        std::vector<std::array<int,2>> ammo;
        std::vector<MilliFunds> cash;
        std::vector<TownQuoteFocus> required;
        std::vector<int> branch;
        std::optional<TownQuoteFocus> focus;
        std::optional<QJsonArray> screenOrders;
        int guard{},horizon{},cutoff{},threshold{};
        TilePoint home{},away{};
        std::int64_t outerWork{};
        bool ownerUpper{},screenDiagnostic{},used{};
    };

    inline bool sameTownScreenDemand(const ScreenRuleDemand &a,const ScreenRuleDemand &b)
    {
        return std::tie(a.actor,a.holder,a.tick,a.requests,a.regular,a.audit,a.attempted)==
               std::tie(b.actor,b.holder,b.tick,b.requests,b.regular,b.audit,b.attempted);
    }

    inline bool refreshFrozenTownFacts(FrozenTownMarketRequest &r,const CombatTerrain &source)
    {
        if(r.used || bool(r.catalog)!=bool(source.production) || r.sourceMenu.lock()!=source.production || bool(r.terrain.weapon)!=bool(source.weapon) ||
            bool(r.terrain.weaponSlots)!=bool(source.weaponSlots) || bool(r.terrain.defense)!=bool(source.defense) ||
            bool(r.terrain.canRepairOnTerrain)!=bool(source.canRepairOnTerrain) ||
            bool(r.terrain.certifyRetainedWalkProfile)!=bool(source.certifyRetainedWalkProfile) ||
            (source.production && (!r.catalog || bool(r.catalog->menu->paidStockSlots)!=bool(source.production->paidStockSlots) ||
             bool(r.catalog->menu->paidWeaponSlots)!=bool(source.production->paidWeaponSlots) ||
             bool(r.catalog->menu->contactDamage)!=bool(source.production->contactDamage))))return false;
        r.reads.refresh(source);
        return true;
    }

    inline std::unique_ptr<FrozenTownMarketRequest> freezeTownMarketForWorker(
        const CombatTerrain &source,std::span<const KnownUnit> units,const ProductionProjection &production,
        const QJsonArray &orders,const StockCaptureSettlement &settled,std::span<const ProductionProperty> properties,
        std::span<const TilePoint> positions,std::span<const double> hp,std::span<const int> fuel,
        std::span<const std::array<int,2>> ammo,std::span<const int> freeDay,std::span<const int> casualty,
        int guard,TilePoint home,TilePoint away,int horizon,int cutoff,int threshold,std::span<const MilliFunds> cash,
        std::optional<TownQuoteFocus> focus,std::span<const TownQuoteFocus> required,std::span<const int> branch,
        bool ownerUpper,ScreenOpening screen)
    {
        if(source.openingRecruitTarget || !settled.townTraffic)return {};
        auto out=std::make_unique<FrozenTownMarketRequest>();auto &r=*out;auto &t=r.terrain;
        const auto store=[](auto &dest,const auto &from){dest.assign(from.begin(),from.end());};
        r.sourceMenu=source.production;r.catalog=freezeTownCatalog(source.production);
        if(r.catalog)t.production=r.catalog->menu;
        t.width=source.width;t.height=source.height;t.guardedHQ=source.guardedHQ;t.contestableSites=source.contestableSites;
        t.friendlyBirthSites=source.friendlyBirthSites;t.policy=source.policy;t.replyTarget=source.replyTarget;
        t.containShooter=source.containShooter;t.containFrom=source.containFrom;t.units=source.units;t.grids=source.grids;
        t.comparisonWork=source.comparisonWork;t.completeProductionMenu=source.completeProductionMenu;
        if(source.workLeft){r.outerWork=*source.workLeft;t.workLeft=&r.outerWork;}
        t.ordinaryCaptureInstallments=source.ordinaryCaptureInstallments;
        if(source.orderedOpening)
        {
            const auto &p=*source.orderedOpening;auto &o=r.opening.emplace();
            o.afterService=p.afterService;o.reservedShooter=p.reservedShooter;o.repair=p.repair;o.prefix=p.prefix;
            o.materialLessCash=p.materialLessCash;
            if(p.exchange)o.exchange=[](const auto&...)->std::optional<OpeningLaneExchange>{throw TownWorkerGap{"OWNER_ORDERED_OPENING"};};
            t.orderedOpening=&o;
        }
        t.screenFire=source.screenFire;r.initialFire=source.screenFire;
        if(source.screenDemand){r.initialScreen=*source.screenDemand;t.screenDemand=std::make_shared<ScreenRuleDemand>(*source.screenDemand);}
        for(std::size_t i=0;i<t.grids.size();++i)r.gridMap.emplace(&source.grids[i],&t.grids[i]);
        if(r.catalog)for(const auto &[src,dst]:r.catalog->blueprint)
            if(src->movementGrid)r.gridMap.emplace(src->movementGrid.get(),dst->movementGrid.get());
        r.production=production;
        for(auto &gun:r.production.fundedGuns)
        {
            if(!gun.unit)continue;
            if(production.damagedOpening && gun.unit==production.damagedOpening.get())
            {
                if(gun.unit->movementGrid)r.gridMap.emplace(gun.unit->movementGrid.get(),gun.unit->movementGrid.get());
                continue;
            }
            if(!r.catalog)return {};
            const auto found=r.catalog->blueprint.find(gun.unit);
            if(found==r.catalog->blueprint.end())return {};
            gun.unit=found->second;
        }
        if(source.movementCache)
        {
            t.movementCache=std::make_shared<CombatMovementCache>();
            for(const auto &[grid,low]:source.movementCache->minimumEntryCost)
            {
                const auto where=r.gridMap.find(grid);
                if(where==r.gridMap.end())return {};
                r.initialMinimum.emplace(grid,low);
                t.movementCache->minimumEntryCost.emplace(where->second,low);
            }
        }
        if(source.weapon)t.weapon=[](int a,int b,int distance){return workerTownReads().weapon.read({a,b,distance},"UNSEEN_STOCK_WEAPON");};
        if(source.weaponSlots)t.weaponSlots=[](int a,int b,int distance){return workerTownReads().slotsAtRange.read({a,b,distance},"UNSEEN_STOCK_SLOTS");};
        if(source.defense)t.defense=[](int a,TilePoint tile){
            auto &reads=workerTownReads();
            if(reads.defenseGrid)return reads.defenseGrid->lookup(a,tile);
            return reads.defense.read({a,tile.x,tile.y},"UNSEEN_STOCK_TERRAIN");
        };
        if(source.certifyRetainedWalkProfile)t.certifyRetainedWalkProfile=[](int actor,int tick,std::int64_t &work){
            auto &f=workerTownReads();if(!f.walk)throw TownWorkerGap{"UNKNOWN_WALK_PROFILE"};
            f.walkRead=true;return (*f.walk)(actor,tick,work);
        };
        if(source.canRepairOnTerrain)t.canRepairOnTerrain=[](int,TilePoint)->bool{throw TownWorkerGap{"OWNER_TERRAIN_REPAIR"};};
        // Native exchange/dummy recruitment paths are outside the town market.
        // An accidental new use must explicitly reach the owning thread.
        if(source.ownFire)t.ownFire=[](int,TilePoint,int,double,double,std::array<int,2>)->OwnFireReport{
            throw TownWorkerGap{"OWNER_OPENING_FIRE"};
        };
        store(r.units,units);r.orders=orders;r.settled=settled;store(r.properties,properties);
        store(r.positions,positions);store(r.hp,hp);store(r.fuel,fuel);store(r.ammo,ammo);
        store(r.freeDays,freeDay);store(r.casualties,casualty);store(r.cash,cash);
        r.guard=guard;r.home=home;r.away=away;r.horizon=horizon;r.cutoff=cutoff;r.threshold=threshold;
        r.focus=focus;store(r.required,required);store(r.branch,branch);r.ownerUpper=ownerUpper;
        store(r.screenPositions,screen.positions);store(r.screenHp,screen.hp);store(r.screenCarry,screen.openingCarry);
        store(r.screenSteps,screen.openingSteps);store(r.screenFuel,screen.openingFuel);store(r.screenExactHp,screen.openingExactHp);
        r.screenDiagnostic=screen.diagnostic;if(screen.orders)r.screenOrders=*screen.orders;
        if(!refreshFrozenTownFacts(r,source))return {};
        return out;
    }

    struct TownWorkerScreenRequest {bool diagnostic{};int actor{},holder{},tick{};};
    struct TownWorkerResult
    {
        std::optional<PricedTownEncounter> price;
        std::int64_t remaining{};
        QString gap;
        std::optional<TownWorkerScreenRequest> screenRequest;
    };

    inline bool validateFrozenTownReads(const FrozenTownMarketRequest &r,const CombatTerrain &source)
    {
        if(!r.used || bool(r.catalog)!=bool(source.production) || source.openingRecruitTarget || bool(source.orderedOpening)!=bool(r.opening) ||
           bool(source.workLeft)!=bool(r.terrain.workLeft) || source.production!=r.sourceMenu.lock() || source.screenFire!=r.initialFire ||
           bool(source.movementCache)!=bool(r.terrain.movementCache) ||
           bool(source.screenDemand)!=r.initialScreen.has_value() || !r.reads.matches(source))return false;
        if(source.production && (!r.catalog || !sameTownCatalogProjection(*r.catalog->menu,*source.production)))return false;
        if(r.initialScreen && !sameTownScreenDemand(*r.initialScreen,*source.screenDemand))return false;
        return !source.movementCache || source.movementCache->minimumEntryCost==r.initialMinimum;
    }

    inline TownWorkerResult runFrozenTownMarket(FrozenTownMarketRequest &r,StockRouteLowerBounds &localLower,
        TownMarketHistory &localHistory,const DetachedTownFrame &frame,std::int64_t offered)
    {
        TownWorkerResult result;result.remaining=offered;
        if(r.used || activeTownReads || forbidTownOuterState || !frame.frame || !frame.discovery){result.gap="BUSY_OR_MISSING_DETACHED_FRAME";return result;}
        r.used=true;activeTownReads=&r.reads;forbidTownOuterState=true;
        const auto reset=qScopeGuard([]{activeTownReads=nullptr;forbidTownOuterState=false;});
        const ScreenOpening screen{r.screenPositions,r.screenHp,r.screenOrders?&*r.screenOrders:nullptr,r.screenDiagnostic,
            r.screenCarry,r.screenSteps,r.screenFuel,r.screenExactHp};
        try
        {
            result.price=priceOrdinaryTownEncounters(r.terrain,r.units,r.production,r.orders,r.settled,r.properties,
                r.positions,r.hp,r.fuel,r.ammo,r.freeDays,r.casualties,r.guard,r.home,r.away,r.horizon,r.cutoff,r.threshold,
                localLower,r.cash,cashTimingAdjustment,result.remaining,r.focus,r.required,&localHistory,r.branch,r.ownerUpper,screen,&frame);
            if(r.terrain.movementCache && (!r.terrain.movementCache->fields.empty() || r.terrain.movementCache->hits ||
               r.terrain.movementCache->misses || r.terrain.movementCache->minimumEntryCost.size()!=r.initialMinimum.size()))
                throw TownWorkerGap{"MOVEMENT_CACHE_CHANGED"};
            if(r.terrain.movementCache)for(const auto &[grid,low]:r.initialMinimum)
            {
                const auto at=r.terrain.movementCache->minimumEntryCost.find(r.gridMap.at(grid));
                if(at==r.terrain.movementCache->minimumEntryCost.end() || at->second!=low)throw TownWorkerGap{"MOVEMENT_MINIMUM_CHANGED"};
            }
            if(r.initialScreen && !sameTownScreenDemand(*r.initialScreen,*r.terrain.screenDemand))
            {
                const auto &next=*r.terrain.screenDemand;
                auto check=*r.initialScreen;
                const bool diagnostic=r.focus.has_value();
                check.request(diagnostic,next.actor,next.holder,next.tick);
                if(!sameTownScreenDemand(check,next))throw TownWorkerGap{"UNSUPPORTED_SCREEN_DELTA"};
                result.screenRequest=TownWorkerScreenRequest{diagnostic,next.actor,next.holder,next.tick};
            }
        }
        catch(const TownWorkerGap &e){result.price.reset();result.screenRequest.reset();result.gap=QString::fromLatin1(e.reason);}
        return result;
    }
}
