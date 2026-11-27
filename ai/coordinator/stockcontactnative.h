#pragma once

#include "ai/coordinator/productionforecast.h"
#include "ai/coordinator/townnativebindings.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "ai/coreai.h"
#include "game/scopedcombatunitstate.h"
#include "game/gamemap.h"
#include "game/unit.h"
#include <QDataStream>
#include <QIODevice>

namespace Coordinator
{
    // A contact is evaluated at its two projected fields. Unit::setTerrain
    // writes only the detached unit's pointer. The live Terrain occupancies,
    // current map units, unique-ID stream and random state are untouched.
    inline auto nativeStockContactOracle(GameMap &map,const BattlefieldKnowledge &knowledge,const CombatTerrain &terrain)
    {
        struct Source {TilePoint origin;int id;};
        struct Book
        {
            std::map<std::tuple<int,int,QString>,std::unique_ptr<Unit>> participants;
            std::map<std::pair<NativeContactPiece,NativeContactPiece>,NativeContactDamage> quotes;
        };
        auto book=std::make_shared<Book>();std::vector<Source> sources;
        for(int i=0;i<static_cast<int>(terrain.units.size());++i)
            sources.push_back({{knowledge.units()[i].x,knowledge.units()[i].y},terrain.units[i].engineUnitId});
        auto quotes=std::shared_ptr<const TownContactBinding::Map>(book,&book->quotes);
        return TownContactBinding{std::move(quotes),[&map,observer=knowledge.observerId(),sources=std::move(sources),book=std::move(book)]
            (const NativeContactPiece &a,const NativeContactPiece &v,std::int64_t &work)->std::optional<NativeContactDamage>{
            // Identical candidate inputs have the same work charge, independent
            // of whether a previous squad warmed the detached oracle.
            constexpr int charge=townNativeContactTariff;
            if(work<charge){work=0;return {};}work-=charge;
            const auto key=std::pair{a,v};
            if(auto cached=book->quotes.find(key);cached!=book->quotes.end())return cached->second;
            const auto model=[&](const NativeContactPiece &p)->Unit*{
                if(p.side<0 || p.side>1 || p.hpTenths<=0 || p.hpTenths>100 ||
                   !map.onMap(p.at.x,p.at.y))return nullptr;
                auto *owner=map.getPlayer(p.side?1-observer:observer);
                Unit *live=nullptr;QString type=p.type;
                if(p.stock!=NO_UNIT)
                {
                    if(p.stock<0 || p.stock>=static_cast<int>(sources.size()))return nullptr;
                    const auto &s=sources[p.stock];
                    if(s.id<=0 || !map.onMap(s.origin.x,s.origin.y))return nullptr;
                    auto *field=map.getTerrain(s.origin.x,s.origin.y);
                    live=field?field->getUnit():nullptr;
                    if(!live || live->getUniqueID()!=s.id || live->getOwner()!=owner || live->getLoadedUnitCount()>0)return nullptr;
                    type=live->getUnitID();
                }
                if(!owner || type.isEmpty())return nullptr;
                const auto identity=std::tuple{p.stock,p.side,type};
                auto it=book->participants.find(identity);
                if(it==book->participants.end())
                {
                    auto instance=std::make_unique<Unit>(type,owner,false,&map);
                    if(live)
                    {
                        QByteArray state;QDataStream output(&state,QIODevice::WriteOnly);
                        live->serializeObject(output);
                        if(output.status()!=QDataStream::Ok)return nullptr;
                        QDataStream input(state);instance->deserializer(input,true);
                        if(input.status()!=QDataStream::Ok || instance->getUniqueID()!=live->getUniqueID())return nullptr;
                        if(instance->getUnitRank()!=live->getUnitRank())instance->setUnitRank(live->getUnitRank(),true);
                    }
                    it=book->participants.emplace(identity,std::move(instance)).first;
                }
                return it->second.get();
            };
            Unit *attacker=model(a),*defender=model(v);
            if(!attacker || !defender || attacker==defender)return {};
            auto *from=map.getTerrain(a.at.x,a.at.y),*to=map.getTerrain(v.at.x,v.at.y);
            if(!from || !to)return {};
            attacker->setTerrain(from);defender->setTerrain(to);
            const ScopedCombatUnitState first(*attacker,a.hpTenths/10.,a.ammo),second(*defender,v.hpTenths/10.,v.ammo);
            const auto raw=CoreAI::calcVirtuelUnitDamage(&map,attacker,0.,QPoint(a.at.x,a.at.y),GameEnums::LuckDamageMode_Average,
                defender,0.,QPoint(v.at.x,v.at.y),GameEnums::LuckDamageMode_Average,false,false);
            attacker->setTerrain(nullptr);defender->setTerrain(nullptr);
            if(!std::isfinite(raw.x()) || !std::isfinite(raw.width()))return {};
            NativeContactDamage result{raw.x(),raw.width(),static_cast<int>(raw.y()),static_cast<int>(raw.height())};
            if(book->quotes.size()<1024)book->quotes.emplace(key,result);
            return result;
        }};
    }
}
