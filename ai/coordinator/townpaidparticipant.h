#pragma once

#include "ai/coordinator/stockneutralrebid.h"

namespace Coordinator
{
    struct PaidTownIdentity
    {
        int stock{},purchase{};
        QJsonObject paid;
        QJsonValue restoreEvidence(QJsonValue value) const
        {
            if(value.isArray())
            {
                auto a=value.toArray();for(qsizetype i=0;i<a.size();++i)a[i]=restoreEvidence(a[i]);return a;
            }
            if(!value.isObject())return value;
            auto o=value.toObject();
            for(const auto &key:o.keys())
            {
                if(o[key].isObject() || o[key].isArray()){o[key]=restoreEvidence(o[key]);continue;}
                if(key!="unit" && key!="gun" && key!="capper" && key!="claimant" && key!="actor")continue;
                if(!o[key].isDouble() || o[key].toInt()!=-2-purchase)continue;
                auto field=key;field[0]=field[0].toUpper();o[key]=NO_UNIT;o["paid"+field]=paid;
            }
            return o;
        }
        int intoBoard(int actor)const
        {
            if(actor<stock)return actor;
            return actor==stock+purchase?stock:actor+1;
        }
        int intoPortfolio(int actor)const
        {
            if(actor<stock)return actor;
            return actor==stock?stock+purchase:actor-1;
        }
        void restore(TownAssaultLine &line) const
        {
            line.capper=intoPortfolio(line.capper);line.blocker=intoPortfolio(line.blocker);
            for(auto &m:line.moves)m.actor=intoPortfolio(m.actor);
            for(auto &s:line.shots){s.actor=intoPortfolio(s.actor);s.target=intoPortfolio(s.target);}
        }
        void restore(TownAssaultAlternatives &options) const
        {
            if(options.assault)restore(*options.assault);
            if(options.response)restore(*options.response);
            std::set<int> caps;
            for(int i:options.checkedCappers)caps.insert(intoPortfolio(i));
            options.checkedCappers=std::move(caps);
            decltype(options.intentions) plans;
            for(auto &[k,v]:options.intentions)
            {
                v.move.actor=intoPortfolio(v.move.actor);
                plans.emplace(std::pair{intoPortfolio(k.first),k.second},std::move(v));
            }
            options.intentions=std::move(plans);
            const auto restoreBounds=[&](auto &source){
                decltype(options.arrivalsRefuted) bounds;
                for(auto &[k,v]:source)
                {
                    v.defenders=restoreEvidence(v.defenders).toArray();
                    if(v.witness)
                    {
                        auto l=std::make_shared<TownAssaultLine>(*v.witness);restore(*l);v.witness=std::move(l);
                    }
                    bounds.emplace(std::pair{intoPortfolio(k.first),k.second},std::move(v));
                }
                source=std::move(bounds);
            };
            restoreBounds(options.arrivalsRefuted);
            restoreBounds(options.partialArrivalActions);
            options.focusedAttempts=restoreEvidence(options.focusedAttempts).toArray();
            options.stagingResponses=restoreEvidence(options.stagingResponses).toArray();
            options.conditionalReactions=restoreEvidence(options.conditionalReactions).toArray();
        }
    };
    struct PaidTownState
    {
        double hp{};
        std::array<int,2> ammo{};
        int fuel{};
    };
    inline std::optional<PaidTownState> paidTownState(const FundedCaptureGun &g,int first,int last)
    {
        if(!g.unit || last<first)return {};
        if(g.observedService && last>=g.observedService->departure && first<g.observedService->nextReady)return {};
        return PaidTownState{paidHpAfterService(g,first,g.unit->startingHp),
            paidLoadAfterService(g,first,g.unit->captureLoad),paidFuelAtRelease(g,first)};
    }

    // Only the owning town board shares these copies. Different dated calendars
    // can have the same mutation count; keep both their identity and revision.
    struct PaidTownCalendarCache
    {
        using TerrainKey=std::pair<const CombatTerrain*,const MobilityCostGrid*>;
        using CalendarKey=std::tuple<std::uint64_t,std::uint64_t,int,int,int,int,int>;
        std::map<TerrainKey,CombatTerrain> terrain;
        std::map<CalendarKey,StockCaptureTraffic> calendars;
        int terrainBuilds{},calendarBuilds{},terrainReads{},calendarReads{};
        // This limits retained copies only. Larger boards use the same
        // temporary snapshots, movement rules and score.
        std::size_t retainedBytes{},byteLimit{8*1024*1024};
        bool retain(std::size_t cost)
        {
            if(retainedBytes>byteLimit || cost>byteLimit-retainedBytes)return false;
            retainedBytes+=cost;return true;
        }
    };
    struct ScopedPaidTerrain
    {
        CombatTerrain &shadow;const CombatTerrain &source;
        ~ScopedPaidTerrain()
        {
            shadow.units.resize(source.units.size());shadow.weapon=source.weapon;
            shadow.weaponSlots=source.weaponSlots;shadow.defense=source.defense;
            shadow.movementCache.reset();
        }
    };

    // Promote this one already-paid guard to the role of an attackable body.
    // All other purchase indices keep their original identity on return. The
    // guard's canceled deployment after release is replaced expressly; stock
    // and unrelated paid movement/occupation reservations stay on the board.
    template<class Quote>
    QString priceWithPaidTownGuard(const TownAssaultBoard &b,const TownAssaultTarget &town,
        const NeutralOccupancy &rival,std::int64_t &work,Quote quote)
    {
        const int n=static_cast<int>(b.units.size()),k=rival.actor-n,S=b.terrain.width*b.terrain.height;
        if(k<0 || k>=static_cast<int>(b.recruits.size()) || !b.recruits[k].unit || b.terrain.units.size()!=b.units.size() ||
           !b.terrain.weaponSlots || !b.terrain.defense || !b.recruitFree || rival.originUnknown || rival.entry<0 ||
           rival.path.steps.empty() || rival.path.steps.back().tile!=town.property.tile ||
           !b.recruitFree(k,rival.departure,b.cutoff))return "PAID_GUARD_HAS_NO_FREE_DATED_RELEASE";
        const auto &g=b.recruits[k];const auto &body=*g.unit;const int side=g.side;
        const bool native=b.terrain.production && b.terrain.production->paidStockSlots;
        const auto state=paidTownState(g,rival.departure,b.cutoff);
        if(!state || g.targetUnit!=NO_UNIT || g.exposure!=0 || !body.movementGrid || state->hp<=0 || body.cost<=0 ||
           (!native && (body.againstCapper.size()!=b.units.size() || body.captureCounter.size()!=b.units.size())))
            return "PAID_GUARD_STATE_OR_WEAPON_SLOTS_UNKNOWN";
        auto origin=paidNeutralOrigin(b.traffic,rival.actor,g,rival.departure);
        if(!origin || origin->from!=rival.origin || origin->fuel!=rival.initialFuel ||
           (origin->fuel>=0 && rival.path.fuel>origin->fuel))return "PAID_GUARD_ORIGINAL_DEPLOYMENT_UNVERIFIED";
        const auto charge=[&](std::int64_t amount){if(work<amount)return false;work-=amount;return true;};
        if(!charge(2+4LL*(n+b.recruits.size())))return "PAID_GUARD_WORK_LIMIT";
        const PaidTownIdentity identity{n,k,{{"birth",QJsonArray{g.factory.x,g.factory.y}},
            {"buildTick",g.buildTick},{"type",g.unit->type},{"enemy",bool(g.side)}}};
        PaidTownCalendarCache local;auto &cache=b.fundedGuardCache?*b.fundedGuardCache:local;
        const auto ck=PaidTownCalendarCache::CalendarKey{b.traffic.calendarId(),b.traffic.generation(),rival.actor,
            rival.departure,TownAssaultDetail::cell(b.terrain,rival.origin),b.cutoff,side};
        auto cal=cache.calendars.find(ck);
        if(cal==cache.calendars.end())
        {
            if(!charge(S))return "PAID_GUARD_WORK_LIMIT";
            StockCaptureTraffic next(b.terrain.width,b.terrain.height,b.traffic.lastTick());
            std::size_t bytes=sizeof(StockCaptureTraffic)+std::size_t(S)*sizeof(std::vector<StockCaptureUse>)+sizeof(StockCaptureUse)*2+128;
            for(int sq=0;sq<S;++sq)for(auto use:b.traffic.uses(sq))
            {
                if(!charge(1))return "PAID_GUARD_WORK_LIMIT";
                bytes+=2*sizeof(StockCaptureUse);
                if(use.actor==rival.actor)use.last=std::min(use.last,rival.departure-1);
                next.reserve(sq,use.first,use.last,use.side,identity.intoBoard(use.actor),use.stopped);
            }
            next.reserve(TownAssaultDetail::cell(b.terrain,rival.origin),rival.departure,b.cutoff,side,n,true);
            auto &storage=cache.retain(bytes)?cache:local;
            cal=storage.calendars.emplace(ck,std::move(next)).first;++cache.calendarBuilds;
        }
        else ++cache.calendarReads;
        const auto &traffic=cal->second;
        const auto tk=PaidTownCalendarCache::TerrainKey{&b.terrain,body.movementGrid.get()};
        auto image=cache.terrain.find(tk);
        if(image==cache.terrain.end())
        {
            if(!charge(4LL*S*(1+b.terrain.grids.size())))return "PAID_GUARD_WORK_LIMIT";
            const std::size_t bytes=sizeof(CombatTerrain)+(n+1)*sizeof(CombatPawn)+
                std::size_t(S)*(4*(1+b.terrain.grids.size())*sizeof(int)+4*sizeof(TilePoint))+256;
            auto &storage=cache.retain(bytes)?cache:local;
            image=storage.terrain.emplace(tk,b.terrain).first;auto &fresh=image->second;
            fresh.grids.push_back(*body.movementGrid);++cache.terrainBuilds;
        }
        else ++cache.terrainReads;
        auto &shadow=image->second;ScopedPaidTerrain restore{shadow,b.terrain};
        shadow.movementCache=std::make_shared<CombatMovementCache>();
        const int grid=static_cast<int>(shadow.grids.size())-1;
        std::vector<KnownUnit> stock(b.units.begin(),b.units.end());
        KnownUnit funded{};funded.x=rival.origin.x;funded.y=rival.origin.y;funded.unitId=body.type;
        funded.hpRounded=ceilHpSteps(state->hp);funded.fuel=rival.initialFuel;funded.movementPoints=body.movement;
        funded.canCapture=body.captureRate.contains(TownAssaultDetail::cell(shadow,town.property.tile));
        if(funded.canCapture)funded.captureRate=replyCaptureRate(body.captureRate.at(TownAssaultDetail::cell(shadow,town.property.tile)),10-funded.hpRounded,false);
        funded.minRange=body.minRange;funded.maxRange=body.maxRange;funded.canMoveAndFire=body.mobileFire;
        funded.canFire=funded.hasWeapons=body.maxRange>0;funded.ignoresUnitCollision=body.ignoresUnitCollision;
        stock.push_back(funded);
        shadow.units.push_back({.tile=rival.origin,.present=true,.enemy=bool(side),.ignoresCollision=body.ignoresUnitCollision,
            .grid=body.mobileFire || funded.canCapture?grid:-1,.movement=body.movement,.engineUnitId=-2-k,
            .capper=funded.canCapture,.replacementCost=body.cost,.realHp=state->hp,.ammoByWeapon=state->ammo,.travelGrid=grid});
        bool missing=false,starved=false;
        shadow.weaponSlots=[&](int a,int v,int distance)->std::array<double,2>{
            if(a<0 || v<0 || a>n || v>n || (a==n && v==n)){missing=true;return {-1.,-1.};}
            if(distance<stock[a].minRange || distance>stock[a].maxRange)return {-1.,-1.};
            if(a==n || v==n)
            {
                const int other=a==n?v:a;
                const auto &unit=stock[a];
                if(!native)
                {
                    if(distance!=unit.minRange){missing=true;return {-1.,-1.};}
                    return a==n?body.captureCounter[v]:body.againstCapper[a];
                }
                if(!charge(2)){starved=true;return {-1.,-1.};}
                const auto q=shadow.production->paidStockSlots(g,other,distance);
                if(!q){missing=true;return {-1.,-1.};}
                return (*q)[int(a==n)];
            }
            return b.terrain.weaponSlots(a,v,distance);
        };
        shadow.weapon=[&](int a,int v,int distance){const auto q=shadow.weaponSlots(a,v,distance);return std::max(q[0],q[1]);};
        shadow.defense=[&](int actor,TilePoint from){
            if(actor!=n)return b.terrain.defense(actor,from);
            const int sq=TownAssaultDetail::cell(shadow,from);
            if(sq<0 || sq>=static_cast<int>(body.routeAt.size()) || body.routeAt[sq]<0 ||
               body.routeAt[sq]>=static_cast<int>(body.defense.size())){missing=true;return TerrainDefenseRule{};}
            return body.defense[body.routeAt[sq]];
        };
        std::vector<TilePoint> posts(b.positions.begin(),b.positions.end());posts.push_back(rival.origin);
        std::vector<double> hp(b.hp.begin(),b.hp.end());hp.push_back(state->hp);
        std::vector<std::array<int,2>> load(b.load.begin(),b.load.end());load.push_back(state->ammo);
        std::vector<TownAssaultRight> rights(b.rights.begin(),b.rights.end()),former(b.beforeConditionalDuty.begin(),b.beforeConditionalDuty.end());
        rights.push_back({rival.departure,b.cutoff,rival.initialFuel,0,rival.departure});
        if(!former.empty())former.push_back(rights.back());
        auto others=std::vector<FundedCaptureGun>(b.recruits.begin(),b.recruits.end());others[k].unit=nullptr;
        std::vector<ProductionChoice> menus(others.size());
        for(int i=0;i<static_cast<int>(others.size());++i)
        {
            auto &other=others[i];
            if(!other.unit)continue;
            const auto fresh=paidTownState(other,rival.departure,b.cutoff);
            if(!fresh)return "PAID_GUARD_OTHER_RECRUIT_STATE_CHANGES";
            if(other.side==side && !other.observedService)continue;
            if(!charge(1+other.unit->routeAt.size()+other.unit->routes.size()+4LL*n))return "PAID_GUARD_WORK_LIMIT";
            const auto &o=*other.unit;
            auto &option=menus[i];option=o;
            if(other.observedService)
            {
                // Older aggregate stock-victim attacks may have assumed a different
                // slot. Don't restore it to this guard's supporting or opposing gun.
                if(fresh->ammo!=o.captureLoad)return "PAID_GUARD_OTHER_RECRUIT_SPENT_WEAPON";
                option.captureLoad=fresh->ammo;option.startingHp=fresh->hp;other.fuelAtPost=fresh->fuel;
                other.unit=&option;
            }
            if(other.side==side)continue;
            if(!shadow.production || !shadow.production->paidWeaponSlots || o.attack.size()!=b.units.size() ||
               o.incoming.size()!=b.units.size() || o.incomingSlot.size()!=b.units.size())
                return "PAID_GUARD_OTHER_RECRUIT_MATCHUP_UNKNOWN";
            auto attack=shadow.production->paidWeaponSlots(other,g,o.minRange);
            auto counter=shadow.production->paidWeaponSlots(g,other,1);
            for(int d=o.minRange+1;d<=std::min(o.maxRange,shadow.width+shadow.height);++d)
            {
                if(!charge(2))return "PAID_GUARD_WORK_LIMIT";
                if(shadow.production->paidWeaponSlots(other,g,d)!=attack)return "PAID_GUARD_OTHER_RECRUIT_RANGE_DEPENDENT";
            }
            // The current paid-gun representation reserves one generic
            // round, so don't claim a typed secondary round on a future job.
            if(other.targetUnit!=NO_UNIT && std::max(attack[0],attack[1])>0)
                return "PAID_GUARD_COMPETING_FUNDED_GUN_DUTY";
            for(int slot=0;slot<2;++slot)
            {
                if(fresh->ammo[slot]==0)attack[slot]=-1.;
                if(state->ammo[slot]==0)counter[slot]=-1.;
            }
            const int primary=attack[1]>attack[0]?1:0;
            if(attack[primary]>0 && o.captureLoad[primary]>=0 && o.captureLoad[primary]<o.guaranteedShots)
                return "PAID_GUARD_OTHER_RECRUIT_TYPED_ROUNDS_UNKNOWN";
            option.attack.push_back(std::max(attack[0],attack[1]));
            option.incoming.push_back(std::max(counter[0],counter[1]));
            option.incomingSlot.push_back(std::max(counter[0],counter[1])>0 ? (counter[1]>counter[0]?1:0):-1);
            other.unit=&option;
        }
        const auto at=[&](int i,int t){
            return i==n ? (rival.departure<=t?rival.origin:INVALID_TILE):b.at(identity.intoPortfolio(i),t);
        };
        const auto free=[&](int i,int from,int to){return i!=k && b.recruitFree(i,from,to);};
        const auto remove=[&](int i,int tick){return i==n?tick>=rival.departure:!b.canRemove || b.canRemove(identity.intoPortfolio(i),tick);};
        const auto die=[&](int i,int tick){return i==n?tick>=rival.departure:!b.canLoseReactionGun || b.canLoseReactionGun(identity.intoPortfolio(i),tick);};
        std::map<int,std::vector<TownGunClaim>> duties;
        if(b.conditionalClaims)for(const auto &[gun,claims]:*b.conditionalClaims)
        {
            // A different paid gun cannot be restored through the stock
            // actor table. Its original recruitFree and reserved calendar
            // continue to own its shot; retain its key for identity.
            if(gun<0 || gun>=n+static_cast<int>(b.recruits.size()))return "PAID_GUARD_INVALID_GUN_DUTY";
            auto &into=duties[identity.intoBoard(gun)];
            for(auto claim:claims)
            {
                if(claim.claimant<0 || claim.claimant>=n+static_cast<int>(b.recruits.size()))
                    return "PAID_GUARD_INVALID_CLAIMANT_DUTY";
                if(gun<n && claim.claimant>=n && claim.claimant!=rival.actor)
                    return "PAID_GUARD_REQUIRES_ANOTHER_FUNDED_GUN_DUTY";
                claim.claimant=identity.intoBoard(claim.claimant);into.push_back(claim);
            }
        }
        TownAssaultBoard view{shadow,stock,traffic,posts,hp,load,rights,others,at,free,b.threshold,b.horizon,b.cutoff,
            remove,{},former,b.conditionalClaims?&duties:nullptr,die};
        if(b.withdrawalJob)view.withdrawalJob=[&](int i,TilePoint tile,int tick,double hp,std::int64_t &work){
            return b.withdrawalJob(identity.intoPortfolio(i),tile,tick,hp,work);
        };
        auto represented=rival;represented.actor=n;
        quote(view,represented,identity);
        return starved?"PAID_GUARD_WORK_LIMIT":missing?"PAID_GUARD_TERRAIN_OR_MATCHUP_UNKNOWN":QString{};
    }
}
