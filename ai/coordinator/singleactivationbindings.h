#pragma once
#include "ai/coordinator/openingsurvivalbindings.h"
#include "ai/coordinator/singleactivationassets.h"
#include "game/co.h"
#include "game/gameanimation/gameanimationfactory.h"
#include "coreengine/globalutils.h"
#include "coreengine/audiomanager.h"
#include "coreengine/mainapp.h"
#include <QScopeGuard>

namespace Coordinator
{
    inline bool singleActivationPowerBindings(QJSEngine &engine,GameMap &map,CO &officer,Player &enemy,
        const QString &raider,const QString &defender,const QString &ownAction,
        RefinementLedger &ledger,QString &why)
    {
        using namespace PowerBindingDetail;
        constexpr auto found=GuardedScriptPropertyStatus::Found;
        constexpr auto bound=GuardedNativeBindingStatus::Bound;
        const auto reject=[&](QString whyNot){why=std::move(whyNot);return false;};
        const auto type=officer.getCoID();
        if(!officer.getPerkList().empty() || officer.getCOUnit())return reject("UNSUPPORTED_PERK_OR_CO_UNIT");
        const auto profile=std::find_if(std::begin(powerProfiles),std::end(powerProfiles),
            [&](const auto &p){return type==QLatin1StringView(p.source.id);});
        if(profile==std::end(powerProfiles))return reject("NO_OBSERVED_STAT_ONLY_POWER_EFFECT_CONTRACT");
        const auto list=[](const char *s){return QString::fromLatin1(s).split(' ',Qt::SkipEmptyParts);};
        const auto methods=list(profile->methods),data=list(profile->scalars);
        const auto hooks=list(nativeHooks);
        static const QStringList checked=[]{
            auto keys=QString::fromLatin1(nativeHooks).split(' ',Qt::SkipEmptyParts);
            for(const auto &p:powerProfiles)keys+=QString::fromLatin1(p.methods).split(' ',Qt::SkipEmptyParts);
            keys+=QStringList{"loadAnimation","loadMovingCo","loadMovingText","randomDirections","isActive","replaceTextArgs","getMissileBuildings",
                "getActions","isFinalStep","perform","performPostAnimation","canBePerformed","doWalkingAnimation",
                "getMovementType","getUnitType","onDeath","onKilled","getAdditionalUnitActions","moveUnit"};
            keys.removeDuplicates();return keys;
        }();
        thread_local HqBindingDetail::StockMethods stock(QCoreApplication::applicationDirPath()+"/resources/scripts",checked);
        std::vector<QString> names{type,"CO","TAGPOWER","GAMEANIMATIONPOWER","globals","GameAnimationFactory","settings","audio","Global","qsTr","replaceTextArgs",
            "ACTION_FIRE","UNIT",raider,defender,"ACTION_WAIT","ACTION_CO_UNIT_0","ACTION_CO_UNIT_1","ACTION_MISSILE",
            "ACTION_BUILD_TEMP_AIRPORT","ACTION_BUILD_TEMP_HARBOUR","TEMPORARY_AIRPORT","TEMPORARY_HARBOUR","ACTION_JOIN","ACTION_LOAD","ACTION_UNLOAD"};
        const auto weather=map.getGameRules()->getCurrentWeather()->getWeatherId();
        std::set<QString> terrains;
        if(!ledger.consume(64+4LL*map.getMapHeight()*map.getMapWidth(),RefinementWork::MasterPreparation))return reject("WORK_LIMIT");
        for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x)
            terrains.insert(map.getTerrain(x,y)->getTerrainID());
        names.insert(names.end(),terrains.begin(),terrains.end());names.push_back(weather);
        std::sort(names.begin(),names.end());names.erase(std::unique(names.begin(),names.end()),names.end());
        auto idx=GuardedScriptGlobalIndex::prepare(engine,names,ledger);
        if(idx.status()!=found)return reject("UNBOUND_POWER_REALM");
        GuardedScriptOwnIndexCache own(engine,ledger);GuardedScriptImageCache images(engine,ledger);
        const auto member=[&](const QString &id,const QString &key){
            const auto obj=idx.find(engine,id,ledger);
            return obj.status==found?guardedScriptProperty(obj.value,key,ledger,&own):GuardedScriptProperty{};
        };
        const auto quiet=[&](const GuardedScriptProperty &p){return p.status==GuardedScriptPropertyStatus::Missing ||
            (p.status==found && (p.value.isNull() || p.value.kind()==GuardedScriptValueKind::Undefined));};
        // Native terrain and weather modifiers are read at F0 to price an E0 route.
        // A live override may depend on the active seat. The supported stock
        // surface has no such modifier, including on both unit types.
        for(const auto &terrain:terrains)
            if(!quiet(member(terrain,"getMovementcostModifier")))return reject("UNBOUND_TERRAIN_MOVEMENT_COST_"+terrain);
        for(const auto *key:{"getMovementCostModifier","getMovementpointModifier","getMovementFuelCostModifier"})
            if(!quiet(member(weather,key)))return reject("UNBOUND_OPENING_WEATHER_"+QString::fromLatin1(key));
        for(const auto &actor:{raider,defender})
            if(!quiet(member(actor,"getWeatherImmune")))return reject("UNBOUND_OPENING_ACTOR_WEATHER_"+actor);
        QString mismatch;
        const auto exact=[&](const QString &id,const QString &key,const HqBindingDetail::Asset &a){
            const auto fn=member(id,key);
            if(fn.status!=found){mismatch="missing";return false;}
            const auto context=guardedScriptContextOperands(fn.value,ledger);
            const auto code=guardedScriptFunctionImage(fn.value,ledger,&images);
            if(context.status!=GuardedScriptContextOperands::GlobalScopeOnly){mismatch="context "+QString::number(int(context.status))+" "+code.compilationName;return false;}
            if(!stock.matches<true>(a,key,code,ledger)){mismatch="source "+code.compilationName;return false;}
            return true;
        };
        for(const auto &key:methods)if(!exact(type,key,profile->source))return reject(type+"."+key+" "+mismatch);
        const auto text=idx.find(engine,"replaceTextArgs",ledger);
        if(text.status!=found || guardedScriptContextOperands(text.value,ledger).status!=GuardedScriptContextOperands::GlobalScopeOnly ||
            !stock.matches<true>(*HqBindingDetail::asset("Global"),"replaceTextArgs",guardedScriptFunctionImage(text.value,ledger,&images),ledger))
            return reject("UNBOUND_POWER_DESCRIPTION_ARGUMENTS");
        if(!ledger.consume(131072,RefinementWork::MasterPreparation))return reject("WORK_LIMIT");
        QJSEngine original;original.installExtensions(QJSEngine::TranslationExtension);
        const auto translation=idx.find(engine,"qsTr",ledger);
        if(translation.status!=found || !guardedScriptFunctionImage(translation.value,ledger,&images).sameImplementation(
            guardedScriptFunctionImage(original.globalObject().property("qsTr"),ledger)) ||
           guardedScriptQtValueConstructors(engine,ledger).status!=GuardedNativeReceiverStatus::Bound)
            return reject("UNBOUND_POWER_TRANSLATION_OR_COORDINATES");
        for(const auto &[kind,property]: {std::pair{GuardedScriptIntrinsicPrototype::Array,"push"},
                                          std::pair{GuardedScriptIntrinsicPrototype::String,"includes"},
                                          std::pair{GuardedScriptIntrinsicPrototype::String,"replace"},
                                          std::pair{GuardedScriptIntrinsicPrototype::String,"toLowerCase"},
                                          std::pair{GuardedScriptIntrinsicPrototype::Number,"toString"}}){
            const auto live=guardedScriptIntrinsicPrototype(engine,kind,ledger),base=guardedScriptIntrinsicPrototype(original,kind,ledger);
            if(live.status!=found||base.status!=found)return reject("POWER_INTRINSIC_PROTOTYPE");
            const auto lhs=guardedScriptBuiltinProperty(live.value,property,ledger),rhs=guardedScriptBuiltinProperty(base.value,property,ledger);
            if(lhs.status!=found||rhs.status!=found || !guardedScriptFunctionImage(lhs.value,ledger,&images).sameImplementation(
                guardedScriptFunctionImage(rhs.value,ledger)))return reject("POWER_INTRINSIC_"+QString::fromLatin1(property));
        }
        for(const auto &key:data){
            const auto v=member(type,key);
            if(v.status!=found || !v.value.number() || !std::isfinite(*v.value.number()) || std::abs(*v.value.number())>10000.)
                return reject("POWER_VALUE_"+key);
        }
        // Each active native CO callback must be the checked stat/presentation
        // method, a stock inherited callback, a stock tag helper, or absent.
        // An extra perk and every board-mutating override remain unsupported.
        for(const auto &key:hooks){
            if(!methods.contains(key) && !quiet(member(type,key)) && !exact(type,key,baseCO))return reject("UNMODELED_CO_HOOK_"+key);
            if(!quiet(member("TAGPOWER",key)) && !exact("TAGPOWER",key,tag))return reject("UNMODELED_TAG_HOOK_"+key);
        }
        for(const auto *key:{"loadAnimation","loadMovingCo","loadMovingText","randomDirections"})
            if(!exact("GAMEANIMATIONPOWER",key,anim))return reject("POWER_PRESENTATION_"+QString::fromLatin1(key));
        if(!exact("CO","isActive",baseCO))return reject("POWER_ACTIVATION_POLICY");
        // Global singleton receivers can belong to the application's main
        // thread. Bind the known native object and compile-owned dispatch
        // contract, exactly as the established native source reader does.
        const auto receiver=[&](const QString &id,const QObject &obj,const QMetaObject &meta,
            std::span<const GuardedNativeMethodRequirement> req){
            const auto g=idx.find(engine,id,ledger);
            if(g.status!=found)return false;
            auto contract=GuardedNativeMethodContract::prepare(meta,req,ledger);
            return contract.status()==GuardedNativeReceiverStatus::Bound &&
                contract.authenticate(engine,g.value,obj,ledger)==GuardedNativeReceiverStatus::Bound;
        };
        static constexpr std::array globals{GuardedNativeMethodRequirement{"randInt",2},GuardedNativeMethodRequirement{"randInt",3},
            GuardedNativeMethodRequirement{"isEven",1}};
        static constexpr std::array animations{GuardedNativeMethodRequirement{"createAnimation",3},
            GuardedNativeMethodRequirement{"createAnimation",4},GuardedNativeMethodRequirement{"createAnimation",5}};
        if(!receiver("globals",*GlobalUtils::getInstance(),GlobalUtils::staticMetaObject,globals))return reject("POWER_NATIVE_RECEIVER_globals");
        if(!receiver("GameAnimationFactory",*GameAnimationFactory::getInstance(),GameAnimationFactory::staticMetaObject,animations))
            return reject("POWER_NATIVE_RECEIVER_GameAnimationFactory");
        static constexpr std::array displays{GuardedNativeMethodRequirement{"getStageHeight",0},
            GuardedNativeMethodRequirement{"getStageWidth",0},GuardedNativeMethodRequirement{"getSmallScreenDevice",0}};
        if(!receiver("settings",*Settings::getInstance(),Settings::staticMetaObject,displays))return reject("POWER_NATIVE_RECEIVER_settings");
        static constexpr std::array music{GuardedNativeMethodRequirement{"addMusic",1},GuardedNativeMethodRequirement{"addMusic",2},
            GuardedNativeMethodRequirement{"addMusic",3}};
        auto *audio=Mainapp::getInstance()->getAudioManager();
        if(!audio || !receiver("audio",*audio,AudioManager::staticMetaObject,music))return reject("POWER_NATIVE_RECEIVER_audio");
        GuardedNativeCodeCatalog catalog(ledger);auto frame=catalog.observe(engine);
        if(frame.status()!=bound || catalog.authenticateCurrentFireCallbacks(engine).status!=bound ||
           catalog.authenticateStockMovementCallbacks(engine,GuardedNativeCodeKind::MoveFeet).status!=bound ||
           catalog.authenticateStockMovementTables(engine,GuardedNativeCodeKind::MoveFeet).status!=bound ||
           catalog.authenticateStockMovementCallbacks(engine,GuardedNativeCodeKind::MoveTank).status!=bound ||
           catalog.authenticateStockMovementTables(engine,GuardedNativeCodeKind::MoveTank).status!=bound ||
           catalog.authenticateStockActionCallbacks(engine,GuardedNativeCodeKind::Capture).status!=bound ||
           catalog.authenticateStockActionCallbacks(engine,GuardedNativeCodeKind::Wait).status!=bound ||
           catalog.authenticateIntrinsics(engine).status!=bound ||
           catalog.authenticateTerminalEnums(engine,GameEnums::nativeInstance(),GameEnums::staticMetaObject).status!=bound)
            return reject("UNBOUND_SINGLE_ACTIVATION_NATIVE_LAWS");
        if(!openingSurvivalGlobalBindings(engine,map,enemy,catalog,ledger,why))return false;
        const auto *pawn=HqBindingDetail::asset(raider),*guard=HqBindingDetail::asset(defender);
        if(!pawn || !guard || !exact(raider,"getMovementType",*pawn) || !exact(defender,"getMovementType",*guard) ||
           !exact(raider,"getUnitType",*pawn) || !exact(defender,"doWalkingAnimation",*guard) ||
           !exact(raider,"doWalkingAnimation",*pawn))return reject("UNBOUND_STOCK_ACTOR_MOVEMENT");
        for(const auto &id:{raider,defender}) {
            for(const auto *key:{"postAction","postBattleActions","onDeath","onKilled","doSupportDamageReduction","predictSupportDamageReduction"})
                if(!quiet(member(id,key)) && !exact(id,key,unit))return reject(id+"."+key);
            if(!exact(id,"moveUnit",unit))return reject(id+".moveUnit");
        }
        if(!exact(raider,"getActions",unit))return reject("HOSTILE_ACTION_LOOKUP");
        const auto offered=member(raider,"actionList");
        if(offered.status!=found)return reject("HOSTILE_ACTION_LIST");
        const auto options=guardedScriptArray(offered.value,ledger);
        if(options.status!=found)return reject("HOSTILE_ACTION_TABLE");
        const std::set<QString> safe{"ACTION_FIRE","ACTION_CAPTURE","ACTION_WAIT","ACTION_JOIN","ACTION_LOAD","ACTION_UNLOAD",
            "ACTION_CO_UNIT_0","ACTION_CO_UNIT_1","ACTION_MISSILE"};
        for(std::uint32_t i=0;i<options.length;++i){
            auto v=guardedScriptArrayElement(offered.value,i,ledger);
            if(v.status!=found)return reject("HOSTILE_ACTION_PROPERTY");
            const auto s=guardedScriptScalar(v.value,ledger);const auto *id=std::get_if<QString>(&s.value);
            if(s.status!=GuardedScriptScalarStatus::Verified||!id||!safe.contains(*id))return reject("UNMODELLED_HOSTILE_ACTION");
        }
        for(const auto &a:{join,load,unload,missile})if(!exact(a.id,"canBePerformed",a))return reject(QString::fromLatin1(a.id)+".canBePerformed");
        if(!exact("ACTION_MISSILE","getMissileBuildings",missile))return reject("MISSILE_LAUNCH_BUILDINGS");
        for(const auto *k:{"canBePerformed","isFinalStep","perform"})
            if(!exact("ACTION_WAIT",k,wait))return reject("UNBOUND_WAIT_"+QString::fromLatin1(k));
        for(const auto &a:{coUnit0,coUnit1}){
            if(!exact(a.id,"canBePerformed",a))return reject(QString::fromLatin1(a.id)+".canBePerformed");
        }
        if(ownAction!="ACTION_WAIT"){
            const auto &build=ownAction=="ACTION_BUILD_TEMP_AIRPORT"?tempAirport:tempHarbour;
            if(ownAction!=QLatin1StringView(build.id))return reject("UNSUPPORTED_FRIENDLY_ABILITY");
            for(const auto *k:{"canBePerformed","isFinalStep","perform"})
                if(!exact(ownAction,k,build))return reject(ownAction+"."+k);
            const auto &picture=ownAction=="ACTION_BUILD_TEMP_AIRPORT"?airportImage:harbourImage;
            if(!exact(picture.id,"addCaptureAnimationBuilding",picture) || !quiet(member(defender,"getCaptureBonus")))
                return reject("UNBOUND_FRIENDLY_CONSTRUCTION_EFFECT");
        }
        return true;
    }
}
