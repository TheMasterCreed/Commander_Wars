#pragma once

#include "ai/coordinator/stockprogress.h"
#include "game/building.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/terrain.h"
#include "game/unit.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace Coordinator
{
    namespace FactoryOpeningDetail
    {
        inline QByteArray digest(const QJsonObject &value)
        {
            return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256).toHex();
        }

        inline std::optional<TilePoint> square(const QJsonValue &value,int width,int height)
        {
            const auto a=value.toArray();
            if(a.size()!=2 || !a[0].isDouble() || !a[1].isDouble())return {};
            const int x=a[0].toInt(-1),y=a[1].toInt(-1);
            if(x<0 || y<0 || x>=width || y>=height || a[0].toDouble()!=x || a[1].toDouble()!=y)return {};
            return TilePoint{x,y};
        }

        struct Range
        {
            int id{};TilePoint from;int movement{},range{};
            std::optional<std::vector<TilePoint>> nativeOrigins;
        };
        struct Post {TilePoint tile;QString reason;int actor{NO_UNIT};};

        inline bool bindHostileOrigins(const MobilityCostGrid &grid,Range &unit,std::int64_t &work)
        {
            if(!grid.onGrid(unit.from.x,unit.from.y))return false;
            const auto cost=6LL*grid.width()*grid.height();
            if(cost<1 || cost>work)return false;
            work-=cost;
            const auto field=DistanceField::build(grid,{unit.from},FieldExpansion::FromSources);
            std::vector<TilePoint> reachable;
            for(int y=0;y<grid.height();++y)for(int x=0;x<grid.width();++x){
                const int travel=field.distance(x,y);
                if(travel>=0 && travel<=unit.movement)reachable.push_back({x,y});
            }
            if(reachable.empty())return false;
            unit.nativeOrigins=std::move(reachable);
            return true;
        }

        inline QJsonObject assess(std::span<const Range> enemy,std::span<const Post> posts,std::int64_t &work)
        {
            QJsonArray distances;
            for(const auto &post:posts){
                std::int64_t closest=std::numeric_limits<std::int64_t>::max();
                QJsonObject row{{"at",QJsonArray{post.tile.x,post.tile.y}},{"role",post.reason},{"actor",post.actor}};
                for(const auto &unit:enemy){
                    if(work<1)return {{"status","OPEN"},{"reason","IMMEDIATE_REACH_WORK"},{"unpriced",row}};
                    --work;
                    const int distance=manhattanDistance(post.tile,unit.from);
                    std::int64_t margin=std::int64_t(distance)-unit.movement-unit.range;
                    TilePoint gun=unit.from;
                    if(unit.nativeOrigins){
                        if(work<static_cast<int>(unit.nativeOrigins->size()))return {{"status","OPEN"},{"reason","FIRING_POST_WORK"}};
                        work-=unit.nativeOrigins->size();
                        int nearestDistance=std::numeric_limits<int>::max();
                        for(const auto at:*unit.nativeOrigins){
                            const int gap=manhattanDistance(post.tile,at);
                            if(gap<nearestDistance){nearestDistance=gap;gun=at;}
                        }
                        margin=std::int64_t(nearestDistance)-unit.range;
                    }
                    if(margin>=closest)continue;
                    closest=margin;
                    row["enemy"]=unit.id;row["enemyAt"]=QJsonArray{unit.from.x,unit.from.y};
                    row["distance"]=distance;row["nativeMovementUpper"]=unit.movement;
                    row["nativeRangeUpper"]=unit.range;row["clearance"]=static_cast<qint64>(margin);
                    row["nativeTravelField"]=unit.nativeOrigins.has_value();
                    if(unit.nativeOrigins)row["nearestPossiblePost"]=QJsonArray{gun.x,gun.y};
                }
                if(closest<=0)return {{"status","OPEN"},{"reason","HOSTILE_CAN_REACH_POST_OR_DUTY"},{"threat",row}};
                distances.append(row);
            }
            return {{"status","NO_IMMEDIATE_DANGER"},{"checked",distances}};
        }
    }

    // Protect dated return/cover obligations that the ordinary comparison has
    // already identified. Each named witness is still tied to the replayed
    // team. Immediate native range remains independently checked below.
    inline QJsonArray factoryOpeningGuardDuties(const QJsonObject &plan)
    {
        QJsonArray duties;
        auto replies=plan["replyAlternatives"].toArray();
        if(replies.isEmpty())replies.append(plan);
        for(const auto &value:replies){
            const auto row=value.toObject();
            const auto guard=row["guardAction"].toObject();
            if(row["hqStatus"]=="NO_NEAR_APPROACH" || row["hqStatus"].toString().isEmpty())continue;
            const int id=row["hqGuard"].toInt(NO_UNIT);
            QJsonObject duty{{"kind","HQ_DEFENSE"},{"status",row["hqStatus"]},{"unit",id},
                {"namedReply",row["replyPolicy"]},{"from",guard["from"]},{"tile",guard["to"]},
                {"returnReserved",guard["reservedOnF1"]}};
            if(!duties.contains(duty))duties.append(duty);
        }
        return duties;
    }

    // Each physical team has already passed the planner's complete native
    // move/buy validation. Check the two identities and scheduled roles at the
    // same observation. This scope covers the first opposing activation only.
    inline QJsonObject factoryOpeningSecurity(GameMap &map,const BattlefieldKnowledge &knowledge,
        const CombatTerrain &terrain,const QJsonObject &reference,const QJsonObject &challenger,
        const QJsonArray &nextDutySites,std::int64_t &work)
    {
        using namespace FactoryOpeningDetail;
        const auto open=[](const QString &reason){return QJsonObject{{"status","OPEN"},{"reason",reason}};};
        const int width=map.getMapWidth(),height=map.getMapHeight();
        auto *owner=map.getCurrentPlayer();
        if(!owner || map.getPlayerCount()!=2 || owner->getPlayerID()!=knowledge.observerId() ||
           width!=terrain.width || height!=terrain.height || terrain.units.size()!=knowledge.units().size())
            return open("UNBOUND_OBSERVATION");
        const auto point=[&](const QJsonValue &v){return square(v,width,height);};
        std::map<int,int> atIndex;
        for(int i=0;i<int(terrain.units.size());++i){
            const auto &p=terrain.units[i];
            if(!p.present || p.engineUnitId<0 || !atIndex.emplace(p.engineUnitId,i).second)
                return open("INCOMPLETE_NATIVE_ROSTER");
        }
        std::vector<Post> posts;
        std::set<std::tuple<int,int,QString,int>> seen;
        const auto check=[&](TilePoint t,const QString &why,int actor=NO_UNIT){
            if(seen.emplace(t.x,t.y,why,actor).second)posts.push_back({t,why,actor});
        };
        std::array<std::map<int,QJsonObject>,2> bound;
        std::array<QJsonArray,2> purchases;
        const std::array<const QJsonObject *,2> teams{&reference,&challenger};
        for(int side=0;side<2;++side){
            const auto &team=*teams[side];
            if(team["day"].toInt(-1)!=map.getCurrentDay() || team["player"].toInt(-1)!=owner->getPlayerID() ||
               team["orders"].toArray().size()!=team["executionOrder"].toArray().size() ||
               team["planSequence"].toInt(-1)<0 || team["planSequence"]!=reference["planSequence"])
                return open("INCOMPLETE_NATIVE_TEAM");
            const auto rows=team["orders"].toArray(),sequence=team["executionOrder"].toArray();
            std::set<int> ordinals;
            for(const auto &item:sequence){
                const int slot=item.toInt(-1);
                if(!item.isDouble() || slot<0 || slot>=rows.size() || !ordinals.insert(slot).second)return open("UNBOUND_EXECUTION_ORDER");
            }
            for(const auto &item:rows){
                const auto r=item.toObject();const int id=r["unit"].toInt(-1);
                const auto origin=point(r["from"]),dest=point(r["to"]);
                if(!origin || !dest || !atIndex.contains(id) ||
                   (r["action"]!="ACTION_WAIT" && r["action"]!="ACTION_CAPTURE") || r["path"].toArray().isEmpty())
                    return open("UNBOUND_OPENING_ACTION_OR_FIRE");
                const int index=atIndex.at(id);
                const auto &u=knowledge.units()[index];
                if(u.ownerId!=owner->getPlayerID() || u.hasMoved || *origin!=terrain.units[index].tile ||
                   !bound[side].emplace(id,r).second)return open("OPENING_ACTOR_CHANGED");
            }
            for(const auto &[id,index]:atIndex){
                const auto &u=knowledge.units()[index];
                if(u.ownerId==owner->getPlayerID() && !u.hasMoved && !bound[side].contains(id))
                    return open("OMITTED_FRIENDLY_ACTIVATION");
            }
            const auto boundary=team["purchaseBoundary"].toObject();
            if(!boundary["modeledOpening"].isArray())return open("UNBOUND_OPENING_BUYER");
            purchases[side]=boundary["modeledOpening"].toArray();
            for(const auto &buy:purchases[side]){
                const auto q=buy.toObject();const auto t=point(q["factory"]);
                if(!t || q["type"].toString().isEmpty() || q["price"].toInteger(-1)<0)return open("UNBOUND_OPENING_BUYER");
                check(*t,"OPENING_PURCHASE");
            }
        }
        if(bound[0].size()!=bound[1].size())return open("NATIVE_TEAM_ROSTERS_DIFFER");
        struct Defender {int id{};std::int64_t radius{};std::vector<TilePoint> origins;};
        std::vector<Defender> changed;
        StockProgressPlan jurisdiction;
        for(const auto &[id,a]:bound[0]){
            if(!bound[1].contains(id))return open("NATIVE_TEAM_ROSTERS_DIFFER");
            const auto b=bound[1].at(id);
            if(a["action"]==b["action"] && a["path"]==b["path"] && a["to"]==b["to"] && a["target"]==b["target"])continue;
            const auto &pawn=terrain.units[atIndex.at(id)];const auto &u=knowledge.units()[atIndex.at(id)];
            if(pawn.supportsUnits || pawn.transportCapacity || pawn.movementBeforeFuel<0 || u.maxRange<0)
                return open("UNBOUND_CHANGED_DEFENSIVE_OR_SUPPORT_DUTY");
            jurisdiction.measures.append(QJsonObject{{"unit",id}});
            changed.push_back({id,std::int64_t(pawn.movementBeforeFuel)+std::max(1,u.maxRange),
                {*point(a["from"]),*point(a["to"]),*point(b["to"])}});
            for(const auto &leg:{a,b})
                for(const auto &px:leg["path"].toArray()){
                    const auto tile=point(px);
                    if(!tile)return open("UNBOUND_CHANGED_PATH");
                    check(*tile,"CHANGED_ACTIVATION",id);
                }
        }
        if(changed.empty())return open("NO_DIFFERENT_OPENING_ACTIVATION");
        const auto duty=[&](TilePoint t,const QString &role){
            for(const auto &u:changed)for(auto post:u.origins)if(manhattanDistance(post,t)<=u.radius){
                check(t,role,u.id);break;
            }
        };
        for(const auto &[id,index]:atIndex){
            const auto &u=knowledge.units()[index];
            if(u.ownerId!=owner->getPlayerID())continue;
            if(u.hasMoved)duty(terrain.units[index].tile,"NEARBY_FRIENDLY_BODY");
            else for(const auto &rows:bound)duty(*point(rows.at(id)["to"]),"NEARBY_FRIENDLY_BODY");
        }
        for(const auto &col:knowledge.buildings()){
            if(col.ownerId!=owner->getPlayerID())continue;
            auto *tile=map.getTerrain(col.x,col.y);auto *b=tile?tile->getBuilding():nullptr;
            if(!b || b->getOwner()!=owner)return open("UNBOUND_OWNED_PROPERTY");
            if(b->isHq())check({col.x,col.y},"OWN_HQ");
            else duty({col.x,col.y},"CHANGED_DEFENDER_PROPERTY");
        }
        for(const auto &v:nextDutySites){
            if(v.isObject()){
                const auto retained=v.toObject();
                if(retained["kind"]!="HQ_DEFENSE")return open("UNBOUND_DATED_DUTY");
                const int holder=retained["unit"].toInt(NO_UNIT);
                const auto at=point(retained["tile"]),from=point(retained["from"]);
                if(holder<0 || !at || !from || retained["status"]!="F1_RETURN_COVER" ||
                   !retained["returnReserved"].toBool() || !atIndex.contains(holder))
                    return QJsonObject{{"status","OPEN"},{"reason","UNRESOLVED_CRITICAL_DUTY"},{"duty",retained}};
                for(const auto &u:changed){
                    if(u.id==holder || std::find(u.origins.begin(),u.origins.end(),*at)!=u.origins.end() ||
                       std::find(u.origins.begin(),u.origins.end(),*from)!=u.origins.end())
                        return QJsonObject{{"status","OPEN"},{"reason","ALTERED_RESERVED_DEFENSE"},{"duty",retained}};
                }
                check(*at,"RESERVED_DEFENSIVE_POST",holder);
                check(*from,"RESERVED_DEFENSIVE_RETURN",holder);
                continue;
            }
            const auto t=point(v);if(!t)return open("INVALID_NEXT_DUTY");
            check(*t,"NEXT_COMMITTED_DUTY");
        }
        std::map<int,int> floors;
        std::vector<Range> threats;
        for(int i=0;i<int(terrain.units.size());++i){
            const auto &p=terrain.units[i];const auto &u=knowledge.units()[i];
            if(!p.enemy)continue;
            if(p.supportsUnits || p.transportCapacity || u.carrier!=KnownUnit::NO_CARRIER ||
               p.movementBeforeFuel<0 || u.maxRange<0)return open("UNBOUND_HOSTILE_SUPPORT_OR_MOVEMENT");
            const int grid=p.travelGrid>=0?p.travelGrid:p.grid;
            if(grid<0 || grid>=int(terrain.grids.size()))return open("MISSING_HOSTILE_TRAVEL_GRID");
            if(!floors.contains(grid)){
                const auto &g=terrain.grids[grid];
                const auto cost=4LL*g.width()*g.height();
                if(g.width()!=width || g.height()!=height || cost>work)return open("HOSTILE_GRID_WORK");
                work-=cost;
                int minimum=std::numeric_limits<int>::max();
                for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(auto dir:ENTRY_DIRECTIONS){
                    const int price=g.cost(x,y,dir);
                    if(price>=0)minimum=std::min(price,minimum);
                }
                floors[grid]=minimum;
            }
            Range unit{p.engineUnitId,p.tile,std::max(p.movement,p.movementBeforeFuel),std::max(1,u.maxRange),{}};
            const bool close=floors.at(grid)<=0 || std::any_of(posts.begin(),posts.end(),[&](const Post &site){
                return manhattanDistance(unit.from,site.tile)<=std::int64_t(unit.movement)+unit.range;
            });
            if(close && !bindHostileOrigins(terrain.grids[grid],unit,work))return open("HOSTILE_NATIVE_TRAVEL_FIELD_OPEN");
            threats.push_back(std::move(unit));
        }
        auto result=assess(threats,posts,work);
        if(result["status"]=="NO_IMMEDIATE_DANGER"){
            // Authenticate the active rules before any extra native menu
            // query. Both friendly portfolios and every already-owned enemy
            // shop are allowed. A newborn's init hook must be stock/passive
            // even though it cannot take another movement action on E0.
            auto possible=purchases[0];
            std::set<QString> types;
            for(const auto &v:possible)types.insert(v.toObject()["type"].toString());
            for(const auto &v:purchases[1])if(types.insert(v.toObject()["type"].toString()).second)possible.append(v);
            auto law=stockProgressOpeningScope(map,knowledge,jurisdiction,possible,work);
            if(!law.isEmpty())return open(law);
            const auto before=possible.size();
            for(const auto &col:knowledge.buildings()){
                if(col.ownerId!=1-owner->getPlayerID())continue;
                if(work<1)return open("HOSTILE_PRODUCTION_MENU_WORK");
                --work;
                auto *t=map.getTerrain(col.x,col.y);auto *b=t?t->getBuilding():nullptr;
                if(!b)return open("UNBOUND_HOSTILE_PRODUCTION_SOURCE");
                if(!b->isProductionBuilding())continue;
                for(const auto &type:b->getConstructionList()){
                    if(type.isEmpty())return open("UNBOUND_HOSTILE_BUILD_TYPE");
                    if(types.insert(type).second)possible.append(QJsonObject{{"type",type},{"enemy",true}});
                }
            }
            if(possible.size()>before){
                law=stockProgressOpeningScope(map,knowledge,jurisdiction,possible,work);
                if(!law.isEmpty())return open(law);
            }
            QJsonArray supported;
            for(const auto &type:types)supported.append(type);
            result["checkedBirthTypes"]=supported;
        }
        result["observation"]=QString::fromLatin1(map.getMapHash().toHex());
        result["referenceTeam"]=QString::fromLatin1(digest(reference));
        result["challengerTeam"]=QString::fromLatin1(digest(challenger));
        result["through"]="FIRST_OPPOSING_ACTIVATION";
        return result;
    }
}
