#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include <bit>
#include <type_traits>

namespace Coordinator
{
    // The caller creates one scope for an immutable map/rule/CO snapshot and
    // timing function. Different scopes cannot reuse a market. No callbacks
    // or spans in an earlier price call are retained here.
    struct TownMarketWorld {};

    struct TownMarketIdentity
    {
        std::shared_ptr<const TownMarketWorld> world;
        // Canonical funded type indices below refer to this owning menu.
        std::shared_ptr<const ProductionForecastBoard> menu;
        QByteArray facts;
        std::vector<QJsonArray> records;
        bool operator==(const TownMarketIdentity &) const = default;
    };

    namespace TownMarketIdentityDetail
    {
        // A damaged newborn may differ from its native type only in the already
        // priced health, loaded clips and guaranteed shot count. Check all
        // immutable fields before identifying a newly allocated clone with it.
        inline bool sameFundedBlueprint(const ProductionChoice &a,const ProductionChoice &b)
        {
            if(std::tie(a.type,a.cost,a.mobileFire,a.minRange,a.maxRange,a.captureRate,a.attack,a.incoming,a.routeAt,
                  a.defense,a.incomingSlot,a.movementGrid,a.movement,a.initialFuel,a.minimumMoveCost,
                  a.captureCounter,a.againstCapper,a.ignoresUnitCollision,a.transportCapacity,a.supportsUnits)!=
               std::tie(b.type,b.cost,b.mobileFire,b.minRange,b.maxRange,b.captureRate,b.attack,b.incoming,b.routeAt,
                  b.defense,b.incomingSlot,b.movementGrid,b.movement,b.initialFuel,b.minimumMoveCost,
                  b.captureCounter,b.againstCapper,b.ignoresUnitCollision,b.transportCapacity,b.supportsUnits) || a.routes.size()!=b.routes.size())return false;
            return std::equal(a.routes.begin(),a.routes.end(),b.routes.begin(),[](const auto &x,const auto &y){
                return std::tie(x.destination,x.firstStop,x.activations,x.cells,x.exits,x.overnightStops)==
                       std::tie(y.destination,y.firstStop,y.activations,y.cells,y.exits,y.overnightStops);
            });
        }

        class Writer
        {
        public:
            explicit Writer(TownMarketIdentity &out):m_out(out) {}

            template<class... T> void add(const T &...values) { (write(values),...); }
            template<class C,class F> void list(const C &entries,F put)
            {
                write(entries.size());
                for(const auto &item:entries)put(item);
            }
            void record(const QJsonArray &a) { m_out.records.push_back(a); }
            void use(const StockCaptureUse &u) { add(u.cell,u.first,u.last,u.side,u.actor,u.stopped); }
            void path(const StockCapturePath &p)
            {
                add(p.entryTick,p.fuel);
                list(p.steps,[&](const auto &s){add(s.tick,s.tile,s.cells);});
                add(p.blockedCells);
            }
            void shot(const CaptureRouteShot &s)
            {
                add(s.gun,s.shotTick,s.weapon,s.counterWeapon,s.from,s.targetBefore,s.targetAfter,s.shooterAfter,
                    s.damageValue,s.counterValue,s.alreadyPriced,s.deployment.from,s.deployment.forward,s.deployment.back,
                    s.deployment.fuel,s.motion.origin,s.motion.fuel,s.motion.cells);
            }
            template<class Gun> void gun(const Gun &g)
            {
                add(g.factory,g.destination,g.side,g.buildTick,g.readyTick,g.impactTick,g.targetUnit,g.exposure,
                    g.targetTile,g.fuelAtPost,g.exposureBeforeFiring);
                if constexpr(requires{g.observedService;})
                {
                    add(g.observedService.has_value());
                    if(g.observedService)
                    {
                        const auto &s=*g.observedService;
                        add(s.departure,s.shot,s.returned,s.nextReady,s.hpAfterFirst,s.hpAfterReply,
                            s.fuelAfterReturn,s.ammoAfterFirst,s.ammoAfterReply);
                    }
                }
            }
        private:
            template<class V> requires(std::is_integral_v<V> || std::is_enum_v<V>)
            void write(V value)
            {
                const auto number=static_cast<std::uint64_t>(value);
                m_out.facts.append(reinterpret_cast<const char*>(&number),sizeof number);
            }
            void write(double value){write(std::bit_cast<std::uint64_t>(value));}
            void write(TilePoint tile){add(tile.x,tile.y);}
            void write(const QString &text)
            {
                write(text.size());
                m_out.facts.append(reinterpret_cast<const char*>(text.utf16()),text.size()*sizeof(char16_t));
            }
            template<class A,class B> void write(const std::pair<A,B> &p){add(p.first,p.second);}
            template<class T> void write(const std::optional<T> &p){write(p.has_value());if(p)write(*p);}
            template<class T,std::size_t N> void write(const std::array<T,N> &a){for(const auto &x:a)write(x);}
            template<class T> void write(const std::vector<T> &v){list(v,[&](const auto &x){write(x);});}
            template<class T> void write(const std::set<T> &v){list(v,[&](const auto &x){write(x);});}
            template<class T> void write(std::span<const T> v){list(v,[&](const auto &x){write(x);});}
            TownMarketIdentity &m_out;
        };
    }

    inline std::optional<TownMarketIdentity> identifyTownMarket(const CombatTerrain &t,std::span<const KnownUnit> units,
        const ProductionProjection &p,const QJsonArray &orders,const StockCaptureSettlement &s,
        std::span<const ProductionProperty> properties,std::span<const TilePoint> positions,std::span<const double> hp,
        std::span<const int> fuel,std::span<const std::array<int,2>> ammo,std::span<const int> freeDay,
        std::span<const int> casualty,int guard,TilePoint home,TilePoint away,int horizon,int cutoff,int threshold,
        std::span<const MilliFunds> cash,const std::shared_ptr<const TownMarketWorld> &world,const ScreenOpening &screen={})
    {
        const auto n=units.size();
        if(!world || t.openingRecruitTarget || t.units.size()!=n || positions.size()!=n || hp.size()!=n || fuel.size()!=n ||
           ammo.size()!=n || freeDay.size()!=n || casualty.size()!=n || !s.townTraffic ||
           t.width<=0 || t.height<=0 || std::int64_t(t.width)*t.height!=s.townTraffic->cellCount())return {};
        TownMarketIdentity key{world,t.production,{},{}};
        TownMarketIdentityDetail::Writer w(key);
        w.add(QStringLiteral("CONDITIONAL_RIVAL_COMMITMENTS_V3"));
        w.add(t.width,t.height,t.ordinaryCaptureInstallments,static_cast<bool>(t.certifyRetainedWalkProfile),static_cast<bool>(t.screenFire),t.guardedHQ,t.contestableSites,t.friendlyBirthSites,t.policy,t.replyTarget,
              t.containShooter,t.containFrom,guard,home,away,horizon,cutoff,threshold,cash,
              positions,hp,fuel,ammo,freeDay,casualty,screen.positions,screen.hp,screen.openingCarry,screen.openingSteps,screen.openingFuel,screen.openingExactHp);
        // The scoped native damage/movement rules are immutable; the actual
        // per-actor facts and held capture points can change between replies.
        w.list(t.units,[&](const auto &u){w.add(u.tile,u.present,u.enemy,u.ignoresCollision,u.grid,u.movement,
            u.engineUnitId,u.capper,u.carrying,u.availableShots,u.replacementCost,u.realHp,u.ammoByWeapon,u.travelGrid,u.needsNativeTownExchange,u.movementBeforeFuel,u.transportCapacity,u.supportsUnits);});
        w.list(units,[&](const auto &u){w.add(u.x,u.y,u.unitId,u.ownerId,u.hpRounded,u.ammo1,u.ammo2,u.fuel,
            u.hasMoved,u.ignoresUnitCollision,u.carrier,u.unitValue,u.hasWeapons,u.capturePoints,u.movementPoints,
            u.minRange,u.maxRange,u.canMoveAndFire,u.canFire,u.canCapture,u.captureRate);});
        w.list(properties,[&](const auto &v){
            const auto &c=v.column;
            w.add(v.tile,c.slot,c.tile,c.income.oursPerTurn,c.income.enemyPerTurn,c.ownerBefore,c.buildingIndex,
                  v.existingCompletion,v.existingEnemy,v.existingEntry,v.capturer,v.rate,v.carry);
            w.list(v.ownershipChanges,[&](const auto &e){w.add(e.entryTick,e.completionTick,e.owner);});
        });
        w.record(orders);w.record(p.orders);w.add(p.net,p.flow,p.availableSites,p.affordableOrders,p.limited);
        if(t.production)
        {
            const auto &b=*t.production;
            w.add(b.funds,b.availableUnits,b.resolvedOpeningFireSite,b.matureFactoryCells,b.friendlyOpening.has_value());
            if(b.friendlyOpening)w.list(*b.friendlyOpening,[&](const auto &o){w.add(o.factory,o.type,o.price);});
        }
        w.list(p.traffic,[&](const auto &u){w.use(u);});
        w.list(p.departures,[&](const auto &d){w.add(d.actor,d.tick,d.end);});
        w.add(p.fundedGuns.size());
        for(const auto &g:p.fundedGuns)
        {
            if(!g.unit || !t.production || g.side<0 || g.side>1)return {};
            const auto &sites=t.production->sites;
            const auto site=std::find_if(sites.begin(),sites.end(),[&](const auto &v){return v.tile==g.factory;});
            if(site==sites.end())return {};
            const auto &options=site->menu[g.side];
            const bool wounded=p.damagedOpening && g.unit==p.damagedOpening.get();
            const auto type=std::find_if(options.begin(),options.end(),[&](const auto &entry){
                return &entry==g.unit || (wounded && TownMarketIdentityDetail::sameFundedBlueprint(entry,*g.unit));
            });
            if(type==options.end())return {};
            w.add(site-sites.begin(),wounded,static_cast<int>(type-options.begin()),g.unit->type,
                g.unit->cost,g.unit->startingHp,g.unit->captureLoad,g.unit->guaranteedShots);
            w.gun(g);
        }
        const auto &calendar=*s.townTraffic;
        w.add(calendar.lastTick(),calendar.cellCount(),calendar.generation());
        for(int cell=0;cell<calendar.cellCount();++cell)
        {
            const auto &uses=calendar.uses(cell);
            if(uses.empty())continue;
            w.add(cell);w.list(uses,[&](const auto &u){w.use(u);});
        }
        w.add(-1);
        w.list(s.closures,[&](const auto &c){w.add(c.entry,c.completion,c.departure,c.rate,c.from,c.replanned);});
        w.add(s.dependentSearchIncomplete);w.add(s.interceptRouteIncomplete);
        w.list(s.entryObligations,[&](const auto &o){
            w.add(o.claimant,o.target,o.generation);
            w.list(o.posts,[&](const auto &post){w.add(post.cell,post.actor,post.from,post.through);});
        });
        w.add(s.losses,s.townBusyStock,s.townBusyPaid,s.townProtectedPosts,s.townUncertainGunDuty);
        w.list(s.attemptedCasualties,[&](const auto &d){w.add(d.actor,d.side,d.requested,d.death,d.proof,d.target,d.post);});
        for(const auto *releases:{&s.townAfterCapture,&s.townAfterFactoryExit})
            w.list(*releases,[&](const auto &item){const auto &[i,r]=item;w.add(i,r.tile,r.readyTick,r.fuel,r.hp,r.ammo,r.protectsPost);});
        w.list(s.townPostDependencies,[&](const auto &item){w.add(item.first);w.list(item.second,[&](const auto &d){
            w.add(d.claimant,d.holder,d.actionTick,d.occupiedTick,d.target,d.claim,d.stopping);});});
        w.list(s.screenClaims,[&](const auto &c){
            w.add(c.actor,c.side,c.grid,c.movement,c.departure,c.latest,c.lastTick,c.hold,c.from,c.target,c.ignoresCollision,c.posts);
        });
        w.list(s.townGunDutyThrough,[&](const auto &d){w.add(d.first,d.second);});
        w.list(s.townGunClaims,[&](const auto &item){w.add(item.first);w.list(item.second,[&](const auto &c){
            w.add(c.claimant,c.target,c.firstShot,c.through,c.incomplete,c.entry);});});
        w.list(s.paidHits,[&](const auto &item){w.add(item.first);w.shot(item.second);});
        w.list(s.fundedFollowups,[&](const auto &item){w.add(item.first);w.record(item.second);});
        // A repeated search may receive more work and reach the identical
        // dated result. Keep routes, resources, statuses and every obligation.
        // Only the display counters for how the original trip was visited can vary.
        QJsonArray physicalProofs;
        for(const auto &v:s.proofs)
        {
            if(!v.isObject()){physicalProofs.append(v);continue;}
            auto proof=v.toObject();
            proof.remove("stockBaseAllowance");proof.remove("stockDutyAllowance");proof.remove("stockWorkSpent");
            proof.remove("matchedStockReplayAllowance");
            proof.remove("routeAttempt");proof.remove("unfinishedRoute");
            physicalProofs.append(proof);
        }
        w.record(physicalProofs);w.record(s.paidCaptureChecks);
        w.record(QJsonArray{s.paidCaptureWorkGap,s.paidCaptureUnsupported});
        w.list(s.paidCaptures,[&](const auto &b){
            w.add(b.gun,b.entry,b.finish,b.property,b.status,b.utility);w.path(b.path);
            w.add(b.survivor.hp,b.survivor.carry,b.survivor.completedTick);
            w.list(b.survivor.shots,[&](const auto &hit){w.shot(hit);});
            for(const auto *r:{&b.installments,&b.pursuitChecks,&b.defenderResources,&b.paidDuelChecks,&b.paidDuelUses})w.record(*r);
        });
        return key;
    }
}
