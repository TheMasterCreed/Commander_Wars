#pragma once

#include "ai/coordinator/openinglanemarket.h"
#include "ai/coordinator/openingvolleybound.h"
#include <QJsonArray>

namespace Coordinator
{
    inline QJsonObject boundOpeningRecruitLane(const CombatTerrain &terrain,std::span<const KnownUnit> units,
        const OpeningLaneHandoff &opening,const OpeningRecruitTarget &target,const ScreenFireProfile *rules,std::int64_t &work)
    {
        QJsonObject receipt{{"complete",false}};
        auto stop=[&](const char *reason){receipt["reason"]=reason;return receipt;};
        const auto &board=opening.afterService;const int n=static_cast<int>(board.size()),gun=target.shooter;
        if(!opening.paidOpeningKnown||!opening.stockServiceKnown||!opening.datedCombatModifiersKnown||!rules||
           rules->through<1 || !rules->ordinaryOpeningPurchases || rules->original.size()!=board.size() || rules->ordinary.size()!=board.size() ||
           units.size()!=board.size() || terrain.units.size()!=board.size() || opening.movement.size()!=board.size() ||
           gun<0 || gun>=n || board[gun].hp<=0 || board[gun].spent || !terrain.defense || !target.powers)
            return stop("ORDINARY_OPENING_BOUND_RULES_UNVERIFIED");
        const auto &shootRule=rules->original[gun];
        if(!rules->ordinary[gun] || shootRule.grid<0 || shootRule.grid>=static_cast<int>(terrain.grids.size()))
            return stop("NAMED_SHOOTER_MOVEMENT_UNVERIFIED");
        for(int i=0;i<n;++i){
            if(units[i].carrier!=KnownUnit::NO_CARRIER)return stop("CARGO_REASSIGNMENT_UNVERIFIED");
            if(board[i].hp<=0)continue;
            if(!rules->ordinary[i] || !rules->quietBirths.contains(units[i].unitId) ||
               terrain.units[i].transportCapacity || terrain.units[i].supportsUnits)
                return stop("OPENING_TRANSPORT_OR_SPECIAL_ACTION_UNVERIFIED");
        }
        if(!terrain.production || !terrain.production->friendlyOpening)return stop("OPENING_PURCHASES_UNVERIFIED");
        for(const auto &buy:*terrain.production->friendlyOpening)
            if(!rules->quietBirths.contains(buy.type))return stop("BIRTH_PASSIVE_EFFECT_UNVERIFIED");
        const auto &grid=terrain.grids[shootRule.grid];const int width=grid.width(),height=grid.height();
        if(width<=0 || height<=0 || width>std::numeric_limits<int>::max()/height ||
           width!=terrain.width || height!=terrain.height)return stop("OPENING_GRID_UNVERIFIED");
        if(work<3LL*width*height+n)return stop("OPENING_CLEARANCE_WORK_LIMIT");work-=3LL*width*height+n;
        const auto reach=DistanceField::build(grid,{board[gun].at},FieldExpansion::FromSources);
        std::vector<TilePoint> posts;
        const int limit=units[gun].canMoveAndFire?shootRule.movement:0;
        const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const auto sq=[&](TilePoint p){return p.y*width+p.x;};
        QJsonArray candidates;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){
            const TilePoint p{x,y};const int d=manhattanDistance(p,target.factory);
            if(d<target.minRange || d>target.maxRange || !reach.reachable(x,y,limit))continue;
            const auto powers=target.powers(d);if(!powers)return stop("NAMED_SHOOTER_WEAPON_UNVERIFIED");
            // Grant full clips, including weapons which an allied JOIN can refill.
            if(usableOpeningWeapon((*powers)[0],{-1,-1})<0)continue;
            posts.push_back(p);candidates.append(point(p));
        }
        receipt["possibleFiringTiles"]=candidates;
        if(posts.empty()){
            receipt["complete"]=true;return stop("NO_TERRAIN_REACHABLE_FIRING_TILE");
        }
        struct Helper{int actor;DistanceField reach;int mp;};
        std::vector<Helper> helpers;
        for(int i=0;i<n;++i){
            const auto &p=board[i];if(i==gun || p.side!=board[gun].side || p.hp<=0 || p.spent)continue;
            const auto &u=rules->original[i];if(u.grid<0 || u.grid>=static_cast<int>(terrain.grids.size()))
                return stop("HELPER_MOVEMENT_UNVERIFIED");
            if(work<3LL*width*height)return stop("OPENING_CLEARANCE_WORK_LIMIT");work-=3LL*width*height;
            helpers.push_back({i,DistanceField::build(terrain.grids[u.grid],{p.at},FieldExpansion::FromSources),
                               units[i].canMoveAndFire?u.movement:0});
        }
        std::vector<TilePoint> surviving;QJsonArray guards;
        for(int i=0;i<n;++i){
            const auto &g=board[i];if(g.side==board[gun].side || g.hp<=0 || !reach.reachable(g.at.x,g.at.y,limit))continue;
            // A useful guard is either an endpoint or lies on a possible path
            // from the shooter's origin. Ignore all other defenders for this test.
            const bool endpoint=std::find(posts.begin(),posts.end(),g.at)!=posts.end();
            if(!endpoint && (terrain.units[gun].ignoresCollision || std::none_of(posts.begin(),posts.end(),[&](TilePoint p){
                    return manhattanDistance(board[gun].at,g.at)+manhattanDistance(g.at,p)<=limit;})))continue;
            std::vector<OpeningVolleyGun> options;QJsonArray assistance;bool known=true;
            for(const auto &h:helpers){
                const auto &u=rules->original[h.actor];const auto attack=u.weaponBase.find(units[i].unitId);
                const auto counter=rules->original[i].weaponBase.find(units[h.actor].unitId);
                if(attack!=u.weaponBase.end() && attack->second<=0)continue;
                OpeningVolleyGun helper{h.actor,board[h.actor].hp,{}};QJsonArray at;
                for(int y=std::max(0,g.at.y-u.range);y<=std::min(height-1,g.at.y+u.range);++y)
                    for(int x=std::max(0,g.at.x-u.range);x<=std::min(width-1,g.at.x+u.range);++x){
                        if(work<1)return stop("OPENING_CLEARANCE_WORK_LIMIT");--work;
                        const TilePoint from{x,y};const int d=manhattanDistance(from,g.at);
                        if(d<units[h.actor].minRange || d>u.range || !h.reach.reachable(x,y,h.mp) ||
                           from==board[gun].at || from==g.at)continue;
                        // Grant a possible counter even when range, ammo or the
                        // selected slot would normally forbid it. A counter-
                        // casualty can help the enemy by freeing a firing post.
                        at.append(point(from));
                        if(attack!=u.weaponBase.end() && counter!=rules->original[i].weaponBase.end())
                            helper.attacks.push_back({sq(from),attack->second,counter->second,terrain.defense(h.actor,from)});
                    }
                // Missing weapon bindings can matter only after this original
                // can stop in range without spending the reserved shooter.
                if(at.isEmpty())continue;
                QJsonObject row{{"unit",terrain.units[h.actor].engineUnitId},{"hp",board[h.actor].hp},{"possibleAttackPosts",at}};
                if(attack==u.weaponBase.end() || counter==rules->original[i].weaponBase.end()){
                    row["missingAttack"]=attack==u.weaponBase.end();
                    row["missingCounter"]=counter==rules->original[i].weaponBase.end();
                    assistance.append(row);known=false;break;
                }
                assistance.append(row);options.push_back(std::move(helper));
            }
            if(!known){
                guards.append(QJsonObject{{"unit",terrain.units[i].engineUnitId},{"tile",point(g.at)},{"hp",g.hp},
                    {"helpers",assistance},{"survival","REACHABLE_WEAPON_OR_COUNTER_UNVERIFIED"}});
                continue;
            }
            const auto answer=boundOpeningVolley(options,terrain.defense(i,g.at),g.hp,work);
            guards.append(QJsonObject{{"unit",terrain.units[i].engineUnitId},{"tile",point(g.at)},{"hp",g.hp},
                {"helpers",assistance},{"survival",answer==OpeningVolleyAnswer::Survives?"PROVED":
                    answer==OpeningVolleyAnswer::PossibleClearance?"POSSIBLE_CLEARANCE":"WORK_OR_INPUT_OPEN"}});
            if(answer==OpeningVolleyAnswer::Survives)surviving.push_back(g.at);
        }
        receipt["guards"]=guards;
        if(surviving.empty())return stop("RELAXED_VOLLEY_DOES_NOT_EXCLUDE_CLEARANCE");
        if(work<3LL*width*height+static_cast<std::int64_t>(surviving.size()+posts.size()))
            return stop("OPENING_CLEARANCE_WORK_LIMIT");
        work-=3LL*width*height+static_cast<std::int64_t>(surviving.size()+posts.size());
        const auto protectedReach=DistanceField::build(terrain.units[gun].ignoresCollision?grid:grid.withBlockedTiles(surviving),
                                                       {board[gun].at},FieldExpansion::FromSources);
        for(const auto p:posts)if(std::find(surviving.begin(),surviving.end(),p)==surviving.end() &&
                                  protectedReach.reachable(p.x,p.y,limit))return stop("ALTERNATIVE_FIRING_TILE_OR_PATH_OPEN");
        receipt["complete"]=true;return stop("ALL_FIRING_TILES_OR_TRANSIT_CUT_BY_SURVIVING_GUARDS");
    }
}
