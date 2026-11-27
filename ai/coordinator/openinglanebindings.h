#pragma once

#include "ai/coordinator/hqopeningbindings.h"
#include "ai/coordinator/openingserviceportfolio.h"
#include "ai/coordinator/battlefieldknowledge.h"
#include "game/gamemap.h"
#include "game/gamescript.h"
#include "game/gamerules.h"
#include "game/player.h"
#include "game/terrain.h"

namespace Coordinator
{
    // Validate the loaded day-start methods that the funded service snapshot relies on. Reading a mod list or script path alone cannot do this.
    inline bool ordinaryOpeningServiceBindings(QJSEngine &engine,GameMap &map,const BattlefieldKnowledge &knowledge,
        const QJsonArray &opening,HqBindingDetail::StockMethods &stock,std::int64_t &work,QString &why,bool friendlyF1=false,bool *workRefused=nullptr)
    {
        const auto fail=[&](QString s){why=std::move(s);return false;};const int player=knowledge.observerId(),other=1-player;
        auto *rules=map.getGameRules();if(work<=0 && workRefused)*workRefused=true;if(work<=0 || map.getPlayerCount()!=2 || !rules || (map.getGameScript() && (map.getGameScript()->hasLoadedScript() || !map.getGameScript()->getScriptFile().isEmpty())))return fail("CUSTOM_MAP_SCRIPT_OR_WORK");
        if(rules->getWeatherAtDay(other<player?1:0,other)!=rules->getCurrentWeather())return fail("E0_WEATHER_CHANGED_OR_UNKNOWN");
        if(friendlyF1 && rules->getWeatherAtDay(1,player)!=rules->getCurrentWeather())return fail("F1_WEATHER_CHANGED_OR_UNKNOWN");
        RefinementLedger ledger;ledger.open(work);struct Debit{std::int64_t &cash;RefinementLedger &meter;bool *refused;~Debit(){cash-=meter.stats().spent();if(refused && meter.stats().refusals)*refused=true;}}debit{work,ledger,workRefused};
        if(!ledger.consume(256+4LL*map.getMapWidth()*map.getMapHeight(),RefinementWork::MasterPreparation))return fail("WORK_LIMIT");
        for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x)if(map.getTerrain(x,y)->getHasStartOfTurn())return fail("DATED_TERRAIN_EFFECT");
        std::set<QString> outgoing,incoming,buildings,owned;
        for(const auto &u:knowledge.units())(u.ownerId==player?outgoing:incoming).insert(u.unitId);
        const auto portfolio=openingServicePortfolio(opening,friendlyF1);
        outgoing.insert(portfolio.friendlyRecruits.begin(),portfolio.friendlyRecruits.end());
        for(const auto &b:knowledge.buildings()){buildings.insert(b.buildingId);if(b.ownerId==other)owned.insert(b.buildingId);}
        if(friendlyF1){
            // Swap the native END and START hooks. E0 purchases have not been
            // selected yet, so the caller supplies every possible enemy type.
            std::swap(outgoing,incoming);
            outgoing.insert(portfolio.enemyRecruits.begin(),portfolio.enemyRecruits.end());
            // A modeled E0 capture could change which repair sites start F1.
            owned=buildings;
        }
        std::set<QString> ids{"Global","BUILDING","UNIT","PLAYER","ACTION_NEXT_PLAYER"};
        ids.insert(outgoing.begin(),outgoing.end());ids.insert(incoming.begin(),incoming.end());ids.insert(buildings.begin(),buildings.end());
        const std::vector<QString> names(ids.begin(),ids.end());GuardedScriptOwnIndexCache own(engine,ledger);GuardedScriptImageCache images(engine,ledger);
        const auto index=GuardedScriptGlobalIndex::prepare(engine,names,ledger);if(index.status()!=GuardedScriptPropertyStatus::Found)return fail("GLOBAL_STATE_UNREADABLE");
        const auto alias=index.find(engine,"Global",ledger);if(alias.status!=GuardedScriptPropertyStatus::Found||guardedScriptGlobalAlias(engine,alias.value,ledger).status!=GuardedScriptPropertyStatus::Found)return fail("GLOBAL_ALIAS");
        const auto lookup=[&](const QString &id,const QString &key){const auto obj=index.find(engine,id,ledger);return obj.status==GuardedScriptPropertyStatus::Found?guardedScriptProperty(obj.value,key,ledger,&own):GuardedScriptProperty{};};
        const auto absent=[&](const QString &id,const QString &key){const auto p=lookup(id,key);if(p.status==GuardedScriptPropertyStatus::Missing||(p.status==GuardedScriptPropertyStatus::Found&&(p.value.isNull()||p.value.kind()==GuardedScriptValueKind::Undefined)))return true;return fail(id+"."+key);};
        const auto method=[&](const QString &id,const char *member,const char *canonical){const QString key=QString::fromLatin1(member);const auto p=lookup(id,key);const auto *source=HqBindingDetail::asset(QString::fromLatin1(canonical));
            if(!source||p.status!=GuardedScriptPropertyStatus::Found||guardedScriptContextOperands(p.value,ledger).status!=GuardedScriptContextOperands::GlobalScopeOnly)return fail(id+"."+key);
            const auto impl=guardedScriptFunctionImage(p.value,ledger,&images);return stock.matches(*source,key,impl,ledger)||fail(id+"."+key);};
        if(!method("ACTION_NEXT_PLAYER","perform","ACTION_NEXT_PLAYER") || !absent("PLAYER","endOfTurn") || !absent("PLAYER","startOfTurn"))return false;
        for(const auto &u:outgoing)if(!absent(u,"endOfTurn")||!absent(u,"postBattleActions"))return false;
        for(const auto &u:incoming)if(!absent(u,"startOfTurn")||!absent(u,"postBattleActions"))return false;
        for(const auto &b:buildings)if(!method(b,"startOfTurn","BUILDING")||!absent(b,"endOfTurn"))return false;
        for(const auto &b:owned){const auto source=b.toLatin1();
            if((friendlyF1 && !absent(b,"getCostReduction")) || !method(b,"canRepair","BUILDING")||!method(b,"getBaseIncome","BUILDING")||
               !method(b,"getConstructionList",source.constData())||!method(b,"getRepairTypes",source.constData()))return false;
        }
        if(!method("BUILDING","replenishUnit","BUILDING")||!method("BUILDING","repairUnit","BUILDING")||!method("UNIT","repairUnit","UNIT"))return false;
        return true;
    }
}
