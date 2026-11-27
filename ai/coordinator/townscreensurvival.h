#pragma once

#include "ai/coordinator/townretainedwalk.h"

namespace Coordinator
{
    struct ScreenAttacker
    {
        int actor{},birthTick{-1};
        TilePoint from{};
        QString type;
        int earliest{},gross{},count{};
    };

    struct ScreenSurvival
    {
        int holder{NO_UNIT},tick{},upper{};
        double hp{};
        bool complete{},survives{},openingJobsCompatible{};
        QString status;
        std::vector<ScreenAttacker> attacks;
        QJsonArray disputedDuties;

        QJsonObject record(const TownAssaultBoard &b) const
        {
            const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
            QJsonArray shots;
            for(const auto &a:attacks)
            {
                QJsonObject o{{"type",a.type},{"origin",point(a.from)},{"firstPossibleShot",a.earliest},
                    {"activations",a.count},{"maxDamagePerShot",a.gross}};
                if(a.birthTick>=0)o["birthTick"]=a.birthTick;
                else o["unit"]=b.terrain.units[a.actor].engineUnitId;
                shots.append(o);
            }
            QJsonObject r{{"unit",b.terrain.units[holder].engineUnitId},{"through",tick},{"startingHp",hp},
                {"status",status},{"damageSources",shots},{"unpricedCompetingActions",disputedDuties},
                {"scope","FIXED_OPENING_AND_PURCHASES_RETAINED_POST_ORDINARY_FIRE"},
                {"holderWithholdsOutboundFire",true},{"existingPaidJobsPermitWithholding",openingJobsCompatible},
                {"namedWalkBlockedIfRetained",survives},{"materialOrAlternativeOrdersPriced",false}};
            if(complete)r["grossDamageUpper"]=upper;
            return r;
        }
    };

    // Lower bound on travel to any firing post. The target field can be reused
    // only when EVERY adjacent on-map candidate could enter the target terrain.
    // The last edge then costs at most 'last', even if the target itself is
    // occupied. Range, fuel, ammunition, intermediate actions, endpoint sharing
    // and safety are relaxed in the attacker's favour.
    inline int screenTravelLower(const CombatTerrain &terrain,const StockRouteLowerBounds &bounds,
        StockRouteLowerBounds &local,const ScreenFireType &gun,TilePoint origin,TilePoint post,std::int64_t &work)
    {
        int distance=std::max(0,manhattanDistance(origin,post)-gun.range);
        if(gun.range!=1 || gun.grid<0 || gun.grid>=static_cast<int>(terrain.grids.size()))return distance;
        const auto &grid=terrain.grids[gun.grid];
        if(!grid.onGrid(origin.x,origin.y) || !grid.onGrid(post.x,post.y))return distance;
        const int destination=post.y*terrain.width+post.x;
        const auto *field=bounds.preparedCost(grid,destination);
        if(!field)
        {
            field=local.cached(grid,destination);
            if(!field && work>=terrain.width*terrain.height)
            {
                work-=terrain.width*terrain.height;
                field=local.costs(grid,destination,work);
            }
        }
        if(!field)return distance;
        int last=0;
        for(const auto d:ENTRY_DIRECTIONS)
        {
            const auto step=entryStep(d);
            if(!grid.onGrid(post.x-step.dx,post.y-step.dy))continue;
            const int c=grid.cost(post.x,post.y,d);
            if(c<0)return distance;
            last=std::max(last,c);
        }
        const int through=(*field)[origin.y*terrain.width+origin.x];
        if(through==std::numeric_limits<int>::max())return through;
        return std::max(distance,through-last);
    }

    inline ScreenSurvival priceRetainedScreen(const TownAssaultBoard &b,const TownWalkReview &walk,
        const TownWalkCut &cut,const ScreenOpening &opening,const StockRouteLowerBounds &bounds,std::int64_t &work)
    {
        ScreenSurvival out;out.holder=cut.holder;out.tick=cut.tick;
        const auto stop=[&](const char *reason){out.status=reason;return out;};
        const auto &t=b.terrain;const auto n=static_cast<int>(b.units.size());
        const auto rules=t.screenFire;
        if(!walk.rulesCertified || !rules || rules->through<cut.tick || cut.holder<0 || cut.holder>=n || !opening.orders ||
           opening.positions.size()!=b.units.size() || opening.hp.size()!=b.units.size() ||
           rules->original.size()!=b.units.size() || rules->ordinary.size()!=b.units.size() ||
           walk.actor<0 || walk.actor>=n || walk.departure!=2+walk.side ||
           opening.positions[walk.actor]!=walk.origin || cut.tick<=1 || walk.side!=cut.tick%2 ||
           opening.positions[cut.holder]!=cut.post ||
           !std::isfinite(opening.hp[cut.holder]) || opening.hp[cut.holder]<=0 || opening.hp[cut.holder]>10 ||
           !rules->ordinary[cut.holder])return stop("UNVERIFIED_OPENING_OR_NATIVE_RULES");
        const int holder=cut.holder;
        out.hp=opening.hp[holder];
        const auto rights=b.beforeConditionalDuty.size()==b.units.size()?b.beforeConditionalDuty:b.rights;
        const int firstOwn=2+int(t.units[holder].enemy);
        if(rights.size()==b.units.size())
            out.openingJobsCompatible=rights[holder].readyTick>=0 && rights[holder].readyTick<=firstOwn &&
                rights[holder].lastFreeTick>=cut.tick && !rights[holder].displacedValue &&
                b.hp[holder]>=out.hp && b.positions[holder]==cut.post;
        if(!out.openingJobsCompatible)
            out.disputedDuties.append(QJsonObject{{"shooter",t.units[holder].engineUnitId},{"through",cut.tick},
                {"status","STATIONARY_OUTGOING_JOB_OR_COUNTER_MUST_BE_REPRICED"}});
        if(b.conditionalClaims)
        {
            const auto claims=b.conditionalClaims->find(holder);
            if(claims!=b.conditionalClaims->end())for(const auto &claim:claims->second)
            {
                if(work<1)return stop("SCREEN_WORK_LIMIT");
                --work;
                if(claim.firstShot>cut.tick)continue;
                if(claim.claimant<0 || claim.claimant>=n)return stop("UNKNOWN_COMPETING_CLAIMANT");
                out.disputedDuties.append(QJsonObject{{"shooter",t.units[holder].engineUnitId},
                    {"targetUnit",t.units[claim.claimant].engineUnitId},
                    {"property",QJsonArray{claim.target.x,claim.target.y}},
                    {"firstShot",claim.firstShot},{"through",claim.through},
                    {"earlierRouteIncomplete",claim.incomplete},{"status","REPRICE_IF_POST_RETAINED"}});
            }
        }
        StockRouteLowerBounds local;
        const auto add=[&](int actor,const QString &kind,const ScreenFireType &type,TilePoint from,int first,int birth)
        {
            if(type.movement<0 || type.range<0 || from.x<0 || from.y<0 || from.x>=t.width || from.y>=t.height)return false;
            const int travel=screenTravelLower(t,bounds,local,type,from,cut.post,work);
            const int turns=(cut.tick-first)/2+1;
            if(travel>std::int64_t(type.movement)*turns)return true;
            const int needed=travel?1+(travel-1)/std::max(1,type.movement):1;
            const auto upper=type.maxDamage.find(b.units[holder].unitId);
            if(upper==type.maxDamage.end() || upper->second<0)return false;
            if(!upper->second)return true;
            const int count=turns-needed+1;
            const int per=std::min(100,upper->second);
            out.upper+=count*per;
            out.attacks.push_back({actor,birth,from,kind,first+2*(needed-1),per,count});
            return true;
        };
        for(int i=0;i<n;++i)
        {
            if(work<8)return stop("SCREEN_WORK_LIMIT");
            work-=8;
            if(int(t.units[i].enemy)!=walk.side)continue;
            if(b.units[i].carrier!=KnownUnit::NO_CARRIER)return stop("TRANSPORT_REASSIGNMENT_UNPRICED");
            if(!t.units[i].present || opening.positions[i]==INVALID_TILE)continue;
            // Every survivor has finished the fixed first reply. Grant all later
            // activations, including ones in use by the ordinary candidate.
            if(!rules->ordinary[i] || !add(i,b.units[i].unitId,rules->original[i],opening.positions[i],2+walk.side,-1))
                return stop("ORIGINAL_FIRE_OR_SERVICE_UNVERIFIED");
        }
        // CAPTURE and some DEPLOY orders can be absent from fundedGuns. Read
        // every paid birth on the actual schedule, regardless of later service.
        std::set<std::tuple<int,int,int>> births;
        for(const auto &entry:*opening.orders)
        {
            if(work<8)return stop("SCREEN_WORK_LIMIT");
            work-=8;
            const auto o=entry.toObject();const auto birth=o["buildHalfTurn"].toInt(-1);
            if(!o["enemy"].isBool() || birth<0 || birth%2!=int(o["enemy"].toBool()))return stop("PURCHASE_IDENTITY_UNVERIFIED");
            if(birth>cut.tick)continue;
            const auto xy=o["factory"].toArray();const auto type=o["type"].toString();
            if(xy.size()!=2 || type.isEmpty() || !rules->quietBirths.contains(type))return stop("PURCHASE_RULES_UNVERIFIED");
            if(int(o["enemy"].toBool())!=walk.side || birth+2>cut.tick)continue;
            const TilePoint p{xy[0].toInt(-1),xy[1].toInt(-1)};
            if(!births.emplace(birth,p.x,p.y).second)return stop("PURCHASE_IDENTITY_UNVERIFIED");
            const auto found=rules->types.find(type);
            if(found==rules->types.end() || !add(NO_UNIT,type,found->second,p,birth+2,birth))
                return stop("PAID_FIRE_OR_SERVICE_UNVERIFIED");
        }
        out.complete=true;out.survives=out.upper<10*out.hp;
        out.status=out.survives?"SURVIVES_ALL_ADMITTED_ORDINARY_FIRE":"DAMAGE_UPPER_PERMITS_REMOVAL";
        return out;
    }
}
