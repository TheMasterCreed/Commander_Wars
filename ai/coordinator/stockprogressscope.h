#pragma once

#include "ai/coordinator/screenfirebindings.h"
#include "ai/coordinator/openingsurvivalbindings.h"
#include "ai/coordinator/openinglanebindings.h"
#include "ai/coordinator/stockprogress.h"
#include "coreengine/interpreter.h"
#include "game/gamemap.h"
#include "game/gamerules.h"
#include "game/gamerule.h"
#include "game/gamescript.h"
#include "game/terrain.h"
#include "game/unit.h"
#include "game/building.h"
#include "game/player.h"
#include "game/victoryrule.h"
#include <QScopeGuard>

namespace Coordinator
{
    // The stock pipe terrain's isolated combat modifier only reads its base
    // terrain and returns either zero or minus twenty. It cannot alter a
    // firing range or any unit state. Bind the loaded function itself using
    // both source representations supported by the native replay loader.
    inline bool stockProgressPassivePipe(QJSEngine &engine,const QString &type,RefinementLedger &ledger)
    {
        constexpr auto found=GuardedScriptPropertyStatus::Found;
        const std::array<QString,1> names{type};
        const auto vars=GuardedScriptGlobalIndex::prepare(engine,names,ledger);
        if(vars.status()!=found)return false;
        const auto obj=vars.find(engine,type,ledger);
        if(obj.status!=found)return false;
        GuardedScriptOwnIndexCache own(engine,ledger);
        const auto get=[&](const char *key){return guardedScriptProperty(obj.value,QString::fromLatin1(key),ledger,&own);};
        const auto base=get("baseTerrainId");
        if(base.status!=found || guardedScriptScalar(base.value,ledger).status!=GuardedScriptScalarStatus::Verified)return false;
        for(const auto *key:{"getDeffensiveFieldBonus","getFirerangeModifier","getMinFirerangeModifier","getMovementcostModifier"}){
            const auto p=get(key);
            if(p.status!=GuardedScriptPropertyStatus::Missing && !(p.status==found && (p.value.isNull() ||
               p.value.kind()==GuardedScriptValueKind::Undefined)))return false;
        }
        const auto fn=get("getOffensiveFieldBonus");
        if(fn.status!=found || guardedScriptContextOperands(fn.value,ledger).status!=GuardedScriptContextOperands::GlobalScopeOnly)return false;
        const auto actual=guardedScriptFunctionImage(fn.value,ledger);
        if(actual.status!=GuardedScriptFunctionStatus::Verified || actual.kind!=GuardedScriptFunctionKind::Script)return false;
        const QUrl uri(actual.compilationName);
        const auto input=uri.scheme()=="file"?uri.toLocalFile():uri.scheme()=="qrc"?":"+uri.path():QString{};
        const QString relative="/resources/scripts/terrain/__BASEPIPELINE.js";
        if(!QDir::fromNativeSeparators(input).endsWith(relative,Qt::CaseInsensitive))return false;
        QFile f(QCoreApplication::applicationDirPath()+relative);
        if(!f.open(QIODevice::ReadOnly)){f.setFileName(":"+relative);if(!f.open(QIODevice::ReadOnly))return false;}
        if(f.size()<=0 || f.size()>65536 || !ledger.consume(131328+68*f.size(),RefinementWork::MasterPreparation))return false;
        const auto bytes=f.readAll().replace("\r\n","\n");
        if(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()!="26759afef6f0a37e61bdea997bf6f46f74a41ae36e38bc73c7724b9c90445506")return false;
        const auto literal=QString::fromUtf8(bytes);
        QString replay;for(auto line:QStringView(literal).split(u'\n'))replay+=line.toString().simplified()+'\n';
        if(literal.endsWith(u'\n'))replay.chop(1);
        QJSEngine reference;reference.globalObject().setProperty("TERRAIN",reference.newObject());
        for(const auto &source:{literal,replay}){
            if(reference.evaluate(source,input).isError())continue;
            const auto bound=guardedScriptFunctionImage(reference.globalObject().property("__BASEPIPELINE").property("getOffensiveFieldBonus"),ledger);
            if(bound.status==GuardedScriptFunctionStatus::Verified && actual.sameImplementation(bound))return true;
        }
        return false;
    }

    QString stockProgressOpeningScope(GameMap &map,const BattlefieldKnowledge &knowledge,const StockProgressPlan &selection,
        const QJsonArray &purchases,std::int64_t &work)
    {
        const auto charge=32LL+12LL*map.getMapWidth()*map.getMapHeight()+16LL*knowledge.units().size();
        if(work<charge)return QStringLiteral("OPENING_RULE_WORK_LIMIT");
        work-=charge;
        auto *rules=map.getGameRules();auto *owner=map.getCurrentPlayer();auto *script=map.getGameScript();
        if(!owner || map.getPlayerCount()!=2 || !rules || rules->getVictory() || rules->getDrawVotingRunning() ||
           rules->getRoundTimeMs()!=0 || rules->getFogMode()!=GameEnums::Fog_Off ||
           rules->getRandomWeather() || !rules->getCurrentWeather() || map.getCampaign() ||
           (script && (script->hasLoadedScript() || !script->getScriptFile().isEmpty())))return QStringLiteral("UNSUPPORTED_OPENING_RULES");
        // This setting is read by the authenticated BUILDING.repairUnit. Its
        // presence affects the refill of unarmed material. Progress units may
        // leave service only with full HP, fuel and both ammunition slots.
        for(int i=0;i<rules->getGameRuleSize();++i){
            auto *rule=rules->getGameRuleAtIndex(i);
            if(!rule || rule->getRuleID()!="GAMERULE_REFILL_MATERIAL")return QStringLiteral("ADDITIONAL_GAME_RULE");
        }
        if(rules->getVictoryRuleSize()==0)return QStringLiteral("UNBOUND_TERMINAL_RULES");
        for(int i=0;i<rules->getVictoryRuleSize();++i)
        {
            const auto id=rules->getVictoryRuleAtIndex(i)->getRuleID();
            if(id!="VICTORYRULE_NOHQ" && id!="VICTORYRULE_NOUNITS")return QStringLiteral("ADDITIONAL_TERMINAL_RULE");
        }
        for(int i=0;i<2;++i)
        {
            auto *p=map.getPlayer(i);
            if(!p || p->getCO(0) || p->getCO(1) || p->getIsDefeated())return QStringLiteral("COMMANDER_OR_EXTRA_ACTIVATION_OPEN");
        }
        auto *weather=rules->getCurrentWeather();
        for(int tick=1;tick<=3;++tick){
            const int next=owner->getPlayerID()+tick;
            auto *later=rules->getWeatherAtDay(next/2,next%2);
            if(!later)later=rules->getWeather(rules->getStartWeather());
            if(!later || later!=weather)return QStringLiteral("WEATHER_CHANGE_OPEN");
        }
        std::set<QString> terrainTypes,properties,types,bought,movement;
        std::vector<QString> tileIds;
        for(const auto &order:purchases)bought.insert(order.toObject()["type"].toString());
        std::set<int> seen;
        for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x)
        {
            auto *t=map.getTerrain(x,y);if(!t)return QStringLiteral("INCOMPLETE_TERRAIN");
            auto *b=t->getBuilding();
            if(t->getHasStartOfTurn() || t->getHp()>0 || (b && b->getHp()>0))return QStringLiteral("ACTIVE_OR_DESTRUCTIBLE_TERRAIN");
            if(b && b->getBuildingID()=="SILO_ROCKET")return QStringLiteral("AREA_ACTION_AVAILABLE");
            tileIds.push_back(t->getID());terrainTypes.insert(t->getTerrainID());if(b)properties.insert(t->getID());
            if(auto *u=t->getUnit())
            {
                if(!u->getOwner() || u->getLoadedUnitCount() || u->hasTemporaryBonuses() || u->isStatusStealthed() ||
                   u->getHp()<=0 || !seen.insert(u->getUniqueID()).second)return QStringLiteral("HIDDEN_OR_SPECIAL_UNIT");
                types.insert(u->getUnitID());
                if(u->getOwner()!=owner)movement.insert(u->getMovementType());
            }
        }
        if(seen.size()!=knowledge.units().size() || int(seen.size())!=map.getPlayer(0)->getUnitCount()+map.getPlayer(1)->getUnitCount())
            return QStringLiteral("INCOMPLETE_OPENING_ROSTER");
        std::set<QString> capperTypes;
        for(const auto &event:selection.measures)
        {
            auto *u=map.getUnit(event.toObject()["unit"].toInt(-1));
            if(!u || u->getOwner()!=owner || u->getHasMoved())return QStringLiteral("PROGRESS_ACTOR_CHANGED");
            capperTypes.insert(u->getUnitID());
            auto *b=map.getTerrain(u->getX(),u->getY())->getBuilding();
            if(b && b->getOwner()==owner && (u->getHp()<UNIT_HP_STEPS ||
               (u->getMaxFuel()>0 && u->getFuel()<u->getMaxFuel()) ||
               (u->getMaxAmmo1()>0 && u->getAmmo1()<u->getMaxAmmo1()) ||
               (u->getMaxAmmo2()>0 && u->getAmmo2()<u->getMaxAmmo2())))return QStringLiteral("REPAIR_OR_RESUPPLY_DUTY_OPEN");
        }
        if(selection.measures.isEmpty())return QStringLiteral("NO_PROGRESS_WITNESS");
        auto &engine=*Interpreter::getInstance();
        QString why;
        thread_local HqBindingDetail::StockMethods captures;
        for(const auto &type:capperTypes)
            if(!ordinaryOpeningCaptureBindings(engine,type,purchases,work,captures,&why))
                return "UNVERIFIED_OPENING_CAPTURE_OR_TERMINAL_"+why;
        const auto scriptRoot=QCoreApplication::applicationDirPath()+"/resources/scripts";
        thread_local HqBindingDetail::StockMethods service(scriptRoot,true);
        if(!ordinaryOpeningServiceBindings(engine,map,knowledge,purchases,service,work,why))return why;
        RefinementLedger ledger;ledger.open(work);
        const auto debit=qScopeGuard([&]{work-=ledger.stats().spent();});
        std::vector<QString> geometryNames(terrainTypes.begin(),terrainTypes.end());
        geometryNames.push_back(weather->getWeatherId());
        const auto bindings=GuardedScriptGlobalIndex::prepare(engine,geometryNames,ledger);
        GuardedScriptOwnIndexCache owned(engine,ledger);
        const auto quiet=[&](const QString &id,const char *key){
            constexpr auto found=GuardedScriptPropertyStatus::Found;
            const auto obj=bindings.find(engine,id,ledger);
            if(obj.status!=found)return false;
            const auto p=guardedScriptProperty(obj.value,QString::fromLatin1(key),ledger,&owned);
            return p.status==GuardedScriptPropertyStatus::Missing || (p.status==found &&
                (p.value.isNull() || p.value.kind()==GuardedScriptValueKind::Undefined));
        };
        if(bindings.status()!=GuardedScriptPropertyStatus::Found ||
           !quiet(weather->getWeatherId(),"getMovementCostModifier") || !quiet(weather->getWeatherId(),"getMovementpointModifier") ||
           !std::ranges::all_of(terrainTypes,[&](const QString &id){return quiet(id,"getMovementcostModifier");}))
            return QStringLiteral("UNVERIFIED_DATED_MOVEMENT_MODIFIER");
        thread_local HqBindingDetail::StockMethods methods(QCoreApplication::applicationDirPath()+"/resources/scripts",
            {"getBaseDamage","getMissileBuildings","perform"});
        for(auto it=terrainTypes.begin();it!=terrainTypes.end();){
            if(stockProgressPassivePipe(engine,*it,ledger))it=terrainTypes.erase(it);else ++it;
        }
        auto all=types;all.insert(bought.begin(),bought.end());
        const auto law=readScreenBoundRules(*Interpreter::getInstance(),ledger,methods,terrainTypes,properties,{weather->getWeatherId()},
            types,all,tileIds,&bought);
        auto *enemy=map.getPlayer(1-knowledge.observerId());
        GuardedNativeCodeCatalog catalog(ledger);
        QStringList gaps;
        if(!law.reason.isEmpty())gaps.append(law.reason);
        else{
            if(!bought.empty() && !law.ordinaryOpeningPurchases)gaps.append("UNVERIFIED_PURCHASE_EFFECT");
            for(const auto &t:movement)if(!law.costs.contains(t))gaps.append("UNVERIFIED_HOSTILE_MOVEMENT_"+t);
        }
        if(!enemy || !openingSurvivalGlobalBindings(engine,map,*enemy,catalog,ledger,why,true))gaps.append("UNVERIFIED_GLOBAL_ACTION_"+why);
        return gaps.join(" / ");
    }
}
