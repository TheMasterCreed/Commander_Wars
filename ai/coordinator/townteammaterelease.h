#pragma once

#include "ai/coordinator/townassault.h"

namespace Coordinator
{
    struct TownTeammateRelease
    {
        enum class Status { NoLegalMove, Limited, NeedsExitDefense, NeedsDepartureReview, UnresolvedCombat, Priced };
        Status status{Status::NoLegalMove};
        std::optional<TownAssaultMove> vacancy,entry;
        std::optional<TownAssaultAlternatives> answers;
        QJsonArray possibleAttackers;
    };

    // Give the enemy every activation, its current undamaged weapon type
    // and unobstructed travel. A possible shot keeps this movement price open.
    // This check does not charge for a shot or require the enemy to take it.
    inline std::optional<bool> townReleaseCouldBeHit(const TownAssaultBoard &b,int holder,TilePoint end,
        int entry,StockRouteLowerBounds &lower,std::int64_t &work,QJsonArray &possible,bool collectAll)
    {
        const int opponent=1-int(b.terrain.units[holder].enemy),n=static_cast<int>(b.units.size());
        if(entry+1>b.cutoff)return false;
        bool threat=false;
        for(int i=0;i<n;++i)
        {
            const auto &u=b.terrain.units[i];const auto &facts=b.units[i];
            if(int(u.enemy)!=opponent || (!facts.canFire && !facts.hasWeapons))continue;
            // Grant even a locally destroyed shooter its original post. An
            // unmodelled transport, missing range or move profile cannot prove safety.
            if(facts.carrier!=KnownUnit::NO_CARRIER)
            {possible.append(QJsonObject{{"unit",u.engineUnitId},{"status","EXISTING_CARGO_ROUTE_UNPRICED"}});return true;}
            if(!u.present)continue;
            if(facts.maxRange<=0 || u.tile==INVALID_TILE)return true;
            const int grid=u.grid>=0?u.grid:u.travelGrid;
            bool disconnected=false,unknownMovement=false,foundPost=false;
            for(auto post:TownAssaultDetail::firingSquares(b.terrain,end,u.tile,facts.minRange,facts.maxRange))
            {
                if(work<=0)return {};--work;
                const int range=manhattanDistance(post,end);
                if(supportWeapon(b.terrain,i,holder,range,std::array<int,2>{-1,-1}).base<=0)continue;
                int travelCost=0;
                TilePoint source=u.tile;
                int sourceTick=0;
                if(post!=u.tile)
                {
                    if(grid<0){unknownMovement=true;continue;}
                    const auto &g=b.terrain.grids[grid];
                    const auto *dist=lower.costs(g,TownAssaultDetail::cell(b.terrain,post),work);
                    if(!dist)return {};
                    travelCost=(*dist)[TownAssaultDetail::cell(b.terrain,u.tile)];
                    const auto checkKnownPost=[&](TilePoint at,int tick){
                        if(travelCost!=std::numeric_limits<int>::max() || at==u.tile ||
                           at.x<0 || at.y<0 || at.x>=b.terrain.width || at.y>=b.terrain.height)return;
                        const int cost=(*dist)[TownAssaultDetail::cell(b.terrain,at)];
                        if(cost!=std::numeric_limits<int>::max()){source=at;sourceTick=tick;travelCost=cost;}
                    };
                    checkKnownPost(b.positions[i],0);
                    // A prior witnessed relocation can already have taken this gun
                    // across an impassable cut. Its work on that route remains payable.
                    if(b.at)for(int t=0;t<=b.cutoff && travelCost==std::numeric_limits<int>::max();++t)
                    {
                        if(work<=0)return {};--work;
                        checkKnownPost(b.at(i,t),t);
                    }
                    // The stock movement value on the original board can be
                    // reduced by today's fuel. A later resupply must not prove
                    // a presently slow enemy incapable of an attack.
                    if(travelCost==std::numeric_limits<int>::max())
                    {
                        disconnected=true;
                        continue;
                    }
                }
                possible.append(QJsonObject{{"unit",u.engineUnitId},{"atOrAfter",std::max(entry+1,sourceTick)},
                    {"source",QJsonArray{source.x,source.y}},{"sourceTick",sourceTick},
                    {"firingSquare",QJsonArray{post.x,post.y}},{"unobstructedTravelCost",travelCost},
                    {"attackSchedulePriced",false}});
                if(!collectAll)return true;
                foundPost=threat=true;break;
            }
            if(!foundPost && (unknownMovement || disconnected))
            {
                const bool other=std::any_of(b.terrain.units.begin(),b.terrain.units.end(),[&](const auto &v){
                    return &v!=&u && v.present && int(v.enemy)==opponent;
                }) || std::any_of(b.recruits.begin(),b.recruits.end(),[&](const auto &g){return g.side==opponent;});
                if(!unknownMovement && !other)continue;
                possible.append(QJsonObject{{"unit",u.engineUnitId},{"status",unknownMovement?
                    "STOCK_MOVEMENT_UNPRICED":"TRANSPORT_ALTERNATIVE_UNPRICED"}});
                if(!collectAll)return true;
                threat=true;
            }
        }
        for(const auto &g:b.recruits)
        {
            const int firstPossibleShot=std::max(entry+1,g.buildTick+2);
            if(g.side!=opponent || firstPossibleShot>b.cutoff)continue;
            if(!g.unit)return true;
            const auto &u=*g.unit;
            if(u.maxRange<=0)continue;
            if(work<=0)return {};--work;
            if(u.movementGrid && u.movement>0)
            {
                bool reachable=false;
                for(auto post:TownAssaultDetail::firingSquares(b.terrain,end,g.factory,u.minRange,u.maxRange))
                {
                    if(work<=0)return {};--work;
                    const auto *dist=lower.costs(*u.movementGrid,TownAssaultDetail::cell(b.terrain,post),work);
                    if(!dist)return {};
                    const int from=TownAssaultDetail::cell(b.terrain,g.factory);
                    if((*dist)[from]!=std::numeric_limits<int>::max()){reachable=true;break;}
                }
                if(!reachable)
                {
                    const bool other=std::any_of(b.terrain.units.begin(),b.terrain.units.end(),[&](const auto &v){
                        return v.present && int(v.enemy)==opponent;
                    }) || std::any_of(b.recruits.begin(),b.recruits.end(),[&](const auto &v){return &g!=&v && v.side==opponent;});
                    if(!other)continue;
                }
            }
            possible.append(QJsonObject{{"birth",QJsonArray{g.factory.x,g.factory.y}},{"born",g.buildTick},
                {"atOrAfter",firstPossibleShot},{"attackSchedulePriced",false}});
            if(!collectAll)return true;
            threat=true;
        }
        return threat;
    }

    // Both moves happen on the same friendly activation. The holder's
    // departure is explicit in the resulting line and can conflict with
    // another town initiative. The holder retains its earlier gun or screen
    // duties, and it cannot fire on the activation it spends vacating.
    inline TownTeammateRelease priceTownTeammateRelease(const TownAssaultBoard &b,TownAssaultTarget target,
        int holder,int cap,TilePoint end,int date,StockRouteLowerBounds &lower,const TownAssaultValue &value,
        std::int64_t &work,bool reportConditional=false)
    {
        using namespace TownAssaultDetail;
        TownTeammateRelease out;
        const int n=static_cast<int>(b.units.size()),side=1-target.defendingSide;
        const auto town=target.property.tile;
        if(holder<0 || cap<0 || holder>=n || cap>=n || holder==cap || date<0 || date>b.cutoff || date%2!=side ||
            int(b.terrain.units[holder].enemy)!=side || int(b.terrain.units[cap].enemy)!=side || !b.units[cap].canCapture ||
            b.positions[holder]!=town || b.at(holder,date)!=town || b.hp[holder]<=0 || b.hp[cap]<=0 || end==town ||
            end.x<0 || end.y<0 || end.x>=b.terrain.width || end.y>=b.terrain.height)return out;
        const auto &u=b.terrain.units[holder];const auto &right=b.rights[holder];
        if(right.readyTick<0 || right.readyTick>date || right.mobileFrom>date || right.lastFreeTick<b.cutoff ||
           (b.canRemove && !b.canRemove(holder,date)) || (b.units[holder].capturePoints>0 && u.tile==town))
        {out.status=TownTeammateRelease::Status::NeedsDepartureReview;return out;}
        const int grid=u.grid>=0?u.grid:u.travelGrid;
        if(grid<0 || b.rights[cap].readyTick<0 || b.rights[cap].readyTick>date || b.rights[cap].lastFreeTick<date)return out;
        // First replay one order on the existing board, then evaluate the
        // second body after that order has physically opened the property.
        for(int t=date;t<=b.cutoff;++t)if(!b.traffic.open(holder,side,cell(b.terrain,end),t,true))return out;
        auto moved=move(b,b.traffic,holder,side,b.terrain.grids[grid],u.movement,right.fuel,u.ignoresCollision,
            town,end,date,date,false,lower,work,true);
        if(!moved){out.status=TownTeammateRelease::Status::Limited;return out;}
        if(moved->destination==INVALID_TILE)return out;
        out.vacancy=*moved;
        auto traffic=b.traffic;reserve(b.terrain,traffic,*moved,b.cutoff);
        auto entered=stockMove(b,traffic,cap,town,date,false,lower,work);
        if(!entered){out.status=TownTeammateRelease::Status::Limited;return out;}
        if(entered->destination==INVALID_TILE)return out;
        const auto priorStop=[&](const TownAssaultMove &trip){
            auto pos=trip.origin;
            for(const auto &step:trip.path.steps){if(step.tick>=date)break;pos=step.tile;}
            return pos;
        };
        const auto ordersFit=[&](const TownAssaultMove &trip){
            const auto capAt=priorStop(trip);
            return capAt!=end;
        };
        if(!ordersFit(*entered)){out.status=TownTeammateRelease::Status::NeedsDepartureReview;return out;}
        out.entry=*entered;
        const auto canHit=townReleaseCouldBeHit(b,holder,end,date,lower,work,out.possibleAttackers,reportConditional);
        if(!canHit){out.status=TownTeammateRelease::Status::Limited;return out;}
        if(*canHit && !reportConditional){out.status=TownTeammateRelease::Status::NeedsExitDefense;return out;}
        auto positions=std::vector<TilePoint>(b.positions.begin(),b.positions.end());positions[holder]=end;
        auto rights=std::vector<TownAssaultRight>(b.rights.begin(),b.rights.end());
        rights[holder].readyTick=rights[holder].mobileFrom=date+2;
        rights[holder].displacedValue=0;
        if(rights[holder].fuel>=0)rights[holder].fuel-=moved->path.fuel;
        auto before=rights;
        if(!b.beforeConditionalDuty.empty())
        {before.assign(b.beforeConditionalDuty.begin(),b.beforeConditionalDuty.end());before[holder]=rights[holder];}
        // Fresh overlays own the local views for the duration of this quote.
        TownAssaultBoard changed{b.terrain,b.units,traffic,positions,b.hp,b.load,rights,b.recruits,
            [&](int id,int t){return id==holder && t>=date?end:b.at(id,t);},
            b.recruitFree,b.threshold,b.horizon,b.cutoff,b.canRemove};
        changed.beforeConditionalDuty=before;changed.conditionalClaims=b.conditionalClaims;
        changed.canLoseReactionGun=b.canLoseReactionGun;
        target.occupant=NO_UNIT;target.requiredCapper=b.terrain.units[cap].engineUnitId;
        target.occupiedFrom=target.occupiedThrough=date;
        const auto appendMove=[&](TownAssaultLine &line){
            if(line.capper<0 || line.stoppedBeforeEntry)return;
            line.moves.insert(line.moves.begin(),*moved);
            if(right.displacedValue)line.losses.emplace_back(date,(side?-1:1)*right.displacedValue);
        };
        const auto pairedValue=[&](const TownAssaultLine &line){
            for(const auto &m:line.moves)if(m.actor==cap && !ordersFit(m))return std::optional<MilliFunds>{};
            auto both=line;appendMove(both);return value(both);
        };
        auto answers=priceTownAssault(changed,target,lower,pairedValue,work);
        if(answers.assault)appendMove(*answers.assault);
        if(answers.response)appendMove(*answers.response);
        out.status=answers.workLimited?TownTeammateRelease::Status::Limited:*canHit?TownTeammateRelease::Status::NeedsExitDefense:
            answers.unresolved?TownTeammateRelease::Status::UnresolvedCombat:TownTeammateRelease::Status::Priced;
        out.answers=std::move(answers);return out;
    }

    struct TownReleaseSearch
    {
        TownAssaultAlternatives result;
        QJsonArray offers;
        std::vector<TownAssaultLine> equalChoices;
        int date{-1},cell{},cap{},attempts{},reviewed{};
        bool complete{};
    };

    inline TownAssaultAlternatives priceTeammateTownEntries(const TownAssaultBoard &b,const TownAssaultTarget &target,
        int holder,TilePoint home,TilePoint away,StockRouteLowerBounds &lower,const TownAssaultValue &value,
        std::int64_t &work,TownReleaseSearch &search,TilePoint prescribedExit=INVALID_TILE)
    {
        using Status=TownTeammateRelease::Status;
        if(search.complete)return search.result;
        auto &out=search.result;
        const auto stopped=[&](){auto incomplete=out;incomplete.workLimited=incomplete.unresolved=true;return incomplete;};
        const int side=1-target.defendingSide,n=static_cast<int>(b.units.size());
        const auto &right=b.rights[holder];const auto town=target.property.tile;
        TownAssaultLine stay;stay.side=side;stay.town=town;
        const auto decline=value(stay);
        if(!decline){out.unresolved=true;search.complete=true;return out;}
        out.declineValue=*decline;
        if(search.date<0)search.date=target.occupiedFrom+int(target.occupiedFrom%2!=side);
        const auto &pawn=b.terrain.units[holder];const int profile=pawn.grid>=0?pawn.grid:pawn.travelGrid;
        for(;search.date<=std::min(b.cutoff,target.occupiedThrough);search.date+=2)
        {
            const int at=search.date;
            if(right.readyTick<0 || right.readyTick>at || right.mobileFrom>at || (b.canRemove && !b.canRemove(holder,at)))
            {out.unresolved=true;continue;}
            if(profile<0 || pawn.movement<=0)continue;
            // A release before the capper arrives can alter an intervening
            // enemy action. Preserve that branch until it has an actual price.
            if(TownAssaultDetail::firstMoveTick(right,side)<at)out.unresolved=true;
            if(target.requiredCapper==NO_UNIT)
                for(const auto &g:b.recruits)if(g.side==side && g.buildTick<at &&
                    (!g.unit || g.unit->captureRate.contains(TownAssaultDetail::cell(b.terrain,town))))out.unresolved=true;
            const auto &grid=b.terrain.grids[profile];
            const int low=b.terrain.movementCache && b.terrain.movementCache->minimumEntryCost.contains(&grid)?
                b.terrain.movementCache->minimumEntryCost.at(&grid):0;
            for(;search.cell<b.terrain.width*b.terrain.height;++search.cell)
            {
                const TilePoint end{search.cell%b.terrain.width,search.cell/b.terrain.width};
                if(end==town || (prescribedExit!=INVALID_TILE && end!=prescribedExit) ||
                   static_cast<std::int64_t>(manhattanDistance(town,end))*low>pawn.movement)continue;
                for(;search.cap<n;++search.cap)
                {
                    const int cap=search.cap;const auto &p=b.terrain.units[cap];
                    if(cap==holder || int(p.enemy)!=side || !b.units[cap].canCapture || b.hp[cap]<=0 ||
                        (target.requiredCapper>=0 && target.requiredCapper!=p.engineUnitId) || b.positions[cap]==INVALID_TILE)continue;
                    if(b.rights[cap].readyTick<0 || b.rights[cap].readyTick>at)
                    {out.unresolved=true;continue;}
                    if(work<=0)return stopped();
                    --work;
                    const auto reaches=TownAssaultDetail::withinTravelBudget(b,cap,town,at,false,lower,work);
                    if(!reaches)return stopped();
                    if(!*reaches)continue;
                    auto quote=priceTownTeammateRelease(b,target,holder,cap,end,at,lower,value,work,prescribedExit!=INVALID_TILE);
                    if(quote.status==Status::Limited)return stopped();
                    ++search.attempts;
                    if(!quote.vacancy || !quote.entry)
                    {
                        if(quote.status==Status::NeedsDepartureReview)out.unresolved=true;
                        continue;
                    }
                    const bool purchasePost=b.terrain.production && std::any_of(b.terrain.production->sites.begin(),
                        b.terrain.production->sites.end(),[&](const auto &site){return site.tile==end;});
                    const bool outsideJob=purchasePost || end==home || end==away;
                    QJsonObject info{{"holder",pawn.engineUnitId},{"exit",QJsonArray{end.x,end.y}},{"exitFuel",quote.vacancy->path.fuel},
                        {"capper",p.engineUnitId},{"town",QJsonArray{town.x,town.y}},{"entryFuel",quote.entry->path.fuel},
                        {"tick",at},{"possibleAttacksOnExit",quote.possibleAttackers},
                        {"status",outsideJob?"RECHECK_SITE_DUTY":quote.status==Status::NeedsExitDefense?"NEEDS_EXIT_DEFENSE":
                            quote.status==Status::Priced?"PRICED_ON_RETAINED_CALENDAR":"UNRESOLVED_COUNTER"}};
                    if(outsideJob || quote.status!=Status::Priced)out.unresolved=true;
                    if(quote.status==Status::NeedsExitDefense && quote.answers && quote.answers->assault)
                    {
                        const auto &op=*quote.answers;
                        info["ifOpponentLeavesHolderAlone"]=QJsonObject{{"finishTick",op.assault->finishTick},
                            {"withCapperResponse",static_cast<qint64>(op.responseValue)},
                            {"withoutCapperResponse",static_cast<qint64>(op.assaultValue)},
                            {"capperResponseComplete",!op.workLimited && !op.unresolved},{"appliedToSchedule",false}};
                    }
                    if(!outsideJob && quote.status==Status::Priced && quote.answers)
                    {
                        const auto &a=*quote.answers;
                        if(a.bestUnanswered && (!out.bestUnanswered || TownAssaultDetail::better(*a.bestUnanswered,*out.bestUnanswered,side)))
                            out.bestUnanswered=a.bestUnanswered;
                        out.search.fullyPriced+=a.search.fullyPriced;
                        out.checkedCappers.insert(a.checkedCappers.begin(),a.checkedCappers.end());
                        if(a.assault)
                        {
                            info["completion"]=a.assault->finishTick;
                            info["unanswered"]=static_cast<qint64>(a.assaultValue);
                            info["withResponse"]=static_cast<qint64>(a.responseValue);
                            if(!out.assault || TownAssaultDetail::better(a.responseValue,out.responseValue,side))
                            {
                                out.assault=a.assault;out.response=a.response;
                                out.assaultValue=a.assaultValue;out.responseValue=a.responseValue;
                                out.selectsAssault=a.selectsAssault;out.selectsResponse=a.selectsResponse;
                                search.equalChoices.clear();
                            }
                            // Preserve every distinct tied departure for the full
                            // two-sided calendar. One exit may block another job.
                            else if(a.responseValue==out.responseValue && a.selectsAssault && out.selectsAssault)
                                search.equalChoices.push_back(a.selectsResponse && a.response?*a.response:*a.assault);
                        }
                    }
                    search.offers.append(info);++search.reviewed;
                }
                search.cap=0;
            }
            search.cell=0;
        }
        search.complete=true;return out;
    }
}
