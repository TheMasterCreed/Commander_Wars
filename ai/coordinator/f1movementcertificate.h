#pragma once

#include "ai/coordinator/survivorshot.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <queue>

namespace Coordinator
{
    // This certificate contains distances and their parent squares, never a
    // price. A legal parent chain proves attainability. Bellman inequalities
    // at every traversable boundary prove that no reachable square was omitted.
    struct F1MovementQuery
    {
        const SurvivorWeaponFacts &weapon;
        const SurvivorPosition &state;
        TilePoint start;
        int movement{},width{},height{};
        std::set<std::pair<int,int>> enemies,occupied;
        F1MovementQuery(const SurvivorWeaponFacts &w,const SurvivorPosition &p,TilePoint from)
            :weapon(w),state(p),start(from),movement(w.fuel<0?w.movement:std::min(w.movement,std::max(0,w.fuel))),
             width(w.grid?w.grid->width():0),height(w.grid?w.grid->height():0)
        {
            for(const auto &[id,t]:p.friendly)if(t!=INVALID_TILE&&t!=from)occupied.emplace(t.x,t.y);
            for(const auto &[id,e]:p.enemy)if(e.second>0&&e.first!=INVALID_TILE){enemies.emplace(e.first.x,e.first.y);occupied.emplace(e.first.x,e.first.y);}
        }
        int cell(TilePoint t)const{return t.x+t.y*width;}
        TilePoint point(int idx)const{return {idx%width,idx/width};}
        int edge(TilePoint a,TilePoint b)const
        {
            if(!weapon.grid || !weapon.grid->onGrid(b.x,b.y) || (!weapon.ignoresCollision && enemies.contains({b.x,b.y})))return -1;
            return weapon.grid->cost(b.x,b.y,entryDirection(a.x,a.y,b.x,b.y));
        }
        bool mobile()const{return weapon.grid && movement>0 && weapon.grid->onGrid(start.x,start.y);}
        QJsonArray build(std::int64_t &work)const
        {
            if(!mobile())return {};
            using Item=std::tuple<int,int,int>;
            std::priority_queue<Item,std::vector<Item>,std::greater<Item>> open;
            std::map<int,int> distances;const int origin=cell(start);
            open.emplace(0,origin,-1);distances[origin]=0;
            QJsonArray result;
            while(!open.empty()){
                if(work<5)return {};work-=5;
                const auto [d,at,parent]=open.top();open.pop();
                if(distances.at(at)!=d)continue;
                result.append(QJsonArray{at,d,parent});
                const auto a=point(at);
                for(const auto direction:ENTRY_DIRECTIONS){
                    const auto step=entryStep(direction);const TilePoint b{a.x+step.dx,a.y+step.dy};
                    const int cost=edge(a,b);
                    if(cost<0 || cost>movement-d)continue;
                    const int next=cell(b);const auto prior=distances.find(next);
                    if(prior!=distances.end() && prior->second<=d+cost)continue;
                    distances[next]=d+cost;open.emplace(d+cost,next,at);
                }
            }
            return result;
        }
        std::optional<std::vector<TilePoint>> verify(const QJsonArray &rows,std::int64_t &work)const
        {
            if(!mobile() || rows.empty() || rows.size()>std::int64_t(width)*height || work<6LL*rows.size())return {};
            work-=6LL*rows.size();const int origin=cell(start);std::map<int,int> dist;
            for(const auto &v:rows){
                const auto row=v.toArray();
                if(row.size()!=3)return {};
                const int id=row[0].toInt(-1),d=row[1].toInt(-1),parent=row[2].toInt(-2);
                if(id<0 || id>=std::int64_t(width)*height || d<0 || d>movement || dist.contains(id))return {};
                if(dist.empty()){if(id!=origin || d || parent!=-1)return {};}
                else{
                    auto found=dist.find(parent);if(found==dist.end())return {};
                    const auto a=point(parent),b=point(id);const int c=edge(a,b);
                    if(manhattanDistance(a,b)!=1 || c<0 || c>movement-found->second || found->second+c!=d)return {};
                }
                dist.emplace(id,d);
            }
            std::vector<TilePoint> ends{start};
            for(const auto &[id,d]:dist){
                const auto a=point(id);
                for(auto direction:ENTRY_DIRECTIONS){
                    const auto step=entryStep(direction);const TilePoint b{a.x+step.dx,a.y+step.dy};
                    const int c=edge(a,b);if(c<0 || c>movement-d)continue;
                    const auto it=dist.find(cell(b));if(it==dist.end() || it->second>d+c)return {};
                }
                // The original formula considers these legal stops, origin
                // first and then row-major, which preserves its tie handling.
                if(a!=start && std::abs(a.x-start.x)<=movement && std::abs(a.y-start.y)<=movement && !occupied.contains({a.x,a.y}))ends.push_back(a);
            }
            return ends;
        }
        // If the ordinary rectangular tariff does not fit, account for the
        // actually inspected legal region. An unfinished build proves nothing.
        std::optional<MilliFunds> priceSparse(int hp,std::int64_t &work,std::optional<SurvivorShotChoice> &choice,QJsonArray &certificate)const
        {
            std::vector<TilePoint> stops{start};
            if(mobile()){
                auto rows=build(work);if(rows.empty())return {};
                auto verified=verify(rows,work);if(!verified)return {};
                stops=std::move(*verified);certificate=std::move(rows);
            }
            const auto tested=qint64(stops.size()*weapon.targets.size());
            if(work<tested)return {};work-=tested;
            return survivorShotAtEnds(weapon,state,stops,hp,&choice);
        }
    };

    struct F1OpportunityShelf
    {
        struct Value{MilliFunds funds{};std::optional<SurvivorShotChoice> action;};
        std::map<QByteArray,Value> computed;
        QJsonObject offered,proved;
        qint64 liveHits{},restored{},fresh{},keyWork{},saved{},sparse{};
        static QByteArray hash(const QByteArray &b){return QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex();}
        QByteArray key(const SurvivorWeaponFacts &w,const SurvivorPosition &p,TilePoint from,int hp,
                       const MobilityCostGrid *immutableGrid,std::int64_t &budget)
        {
            qint64 cost=4+p.friendly.size()+p.enemy.size()+w.targets.size();
            for(const auto &t:w.targets)cost+=2*t.baseByRange.size();
            if(bool(w.grid)!=bool(immutableGrid))return {};
            if(immutableGrid)cost+=4LL*immutableGrid->width()*immutableGrid->height();
            if(budget<cost)return {};budget-=cost;keyWork+=cost;
            QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);
            const auto xy=[&](TilePoint t){stream<<qint32(t.x)<<qint32(t.y);};
            stream<<quint32(1)<<qint32(w.movement)<<qint32(w.fuel)<<w.ignoresCollision<<qint32(hp);xy(from);
            if(immutableGrid){
                QByteArray grid;QDataStream edges(&grid,QIODevice::WriteOnly);
                edges<<qint32(w.grid->width())<<qint32(w.grid->height());
                for(int y=0;y<w.grid->height();++y)for(int x=0;x<w.grid->width();++x)
                    for(auto direction:ENTRY_DIRECTIONS)edges<<qint32(w.grid->cost(x,y,direction));
                stream<<hash(grid);
            }else stream<<QByteArray{};
            stream<<qint32(p.ammo[0])<<qint32(p.ammo[1])<<quint32(p.friendly.size());
            for(const auto &[id,t]:p.friendly){stream<<qint32(id);xy(t);}
            stream<<quint32(p.enemy.size());
            for(const auto &[id,e]:p.enemy){stream<<qint32(id)<<qint32(e.second);xy(e.first);}
            stream<<quint32(w.targets.size());
            for(const auto &t:w.targets){stream<<qint32(t.index)<<qint32(t.hp)<<qint64(t.cost);xy(t.tile);
                stream<<quint32(t.baseByRange.size());for(const auto &base:t.baseByRange)stream<<base[0]<<base[1];}
            return hash(bytes);
        }
        std::optional<MilliFunds> price(const SurvivorWeaponFacts &w,const SurvivorPosition &p,TilePoint from,int hp,
            const MobilityCostGrid *immutableGrid,qint64 fullWork,std::int64_t &budget,std::optional<SurvivorShotChoice> &choice,
            QByteArray &identity,QString &source)
        {
            source="CURRENT_FORMULA";identity=key(w,p,from,hp,immutableGrid,budget);
            const auto found=computed.find(identity);
            if(!identity.isEmpty() && found!=computed.end()){
                if(budget<1)return {};--budget;++liveHits;saved+=fullWork-1;source="SAME_LIVE_QUERY";
                choice=found->second.action;return found->second.funds;
            }
            const auto label=QString::fromLatin1(identity);F1MovementQuery query(w,p,from);
            std::optional<MilliFunds> value;
            if(!identity.isEmpty() && offered.contains(label)){
                const auto rows=offered[label].toArray();const auto before=budget;
                const auto ends=query.verify(rows,budget);
                if(ends && budget>=qint64(ends->size()*w.targets.size())){
                    budget-=qint64(ends->size()*w.targets.size());value=survivorShotAtEnds(w,p,*ends,hp,&choice);
                    if(value){source="REVERIFIED_MOVEMENT_PROOF";++restored;saved+=fullWork-(before-budget);proved[label]=rows;}
                }
            }
            if(!value){
                // A failed optional witness may not consume the credits already
                // needed by a normal quote. An otherwise insufficient tranche
                // can all be used by the checked sparse procedure.
                const auto offer=budget<fullWork?budget:std::min<std::int64_t>(budget-fullWork,fullWork);
                auto remaining=offer;QJsonArray rows;
                value=query.priceSparse(hp,remaining,choice,rows);
                budget-=offer-remaining;
                if(value){
                    source="NEW_VERIFIED_MOVEMENT";++sparse;saved+=fullWork-(offer-remaining);
                    if(!identity.isEmpty() && !rows.empty() && proved.size()<128)proved[label]=rows;
                }
            }
            if(!value){
                if(budget<fullWork)return {};budget-=fullWork;value=survivorShotValue(w,p,from,0,hp,&choice);++fresh;
                if(value && !identity.isEmpty() && proved.size()<128 && query.mobile()){
                    const auto cap=budget/64;const auto offer=fullWork>cap/2?cap:2*fullWork;auto remaining=offer;
                    const auto rows=query.build(remaining);budget-=offer-remaining;if(!rows.empty())proved[label]=rows;
                }
            }
            if(value && !identity.isEmpty() && computed.size()<2048)computed.emplace(identity,Value{*value,choice});
            return value;
        }
    };
}
