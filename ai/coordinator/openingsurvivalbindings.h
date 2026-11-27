#pragma once
#include "ai/coordinator/openingsurvivalassets.h"
#include "ai/coordinator/screenfirebindings.h"
#include "game/gamemap.h"
#include "game/player.h"
#include "game/building.h"
#include "game/terrain.h"

namespace Coordinator
{
    inline bool openingSurvivalGlobalBindings(QJSEngine &engine,GameMap &map,Player &enemy,
        GuardedNativeCodeCatalog &catalog,RefinementLedger &ledger,QString &why,bool openingPurchases=false)
    {
        constexpr auto found=GuardedScriptPropertyStatus::Found;
        constexpr auto bound=GuardedNativeBindingStatus::Bound;
        const auto reject=[&](QString s){why=std::move(s);return false;};
        const auto &root=QCoreApplication::applicationDirPath()+"/resources/scripts";
        thread_local HqBindingDetail::StockMethods stock(root,QStringList{"getEmptyFieldActions","getActions","increaseRang","getMaxRang",
            "canBePerformed","perform","isFinalStep","getStepData"});
        std::vector<QString> names{"ACTION","BUILDING","HQ","TOWN","Global","UNITRANKINGSYSTEM"};
        if(openingPurchases)
            for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x)
                if(auto *site=map.getTerrain(x,y)->getBuilding();site && std::find(names.begin(),names.end(),site->getBuildingID())==names.end())
                    names.push_back(site->getBuildingID());
        for(const auto &a:openingGlobalCommands)names.push_back(QString::fromLatin1(a.id));
        auto idx=GuardedScriptGlobalIndex::prepare(engine,names,ledger);
        if(idx.status()!=found)return reject("UNBOUND_GLOBAL_COMMAND_ENVIRONMENT");
        GuardedScriptOwnIndexCache own(engine,ledger);GuardedScriptImageCache images(engine,ledger);
        const auto member=[&](const QString &id,const char *name){
            const auto g=idx.find(engine,id,ledger);
            return g.status==found?guardedScriptProperty(g.value,QString::fromLatin1(name),ledger,&own):GuardedScriptProperty{};
        };
        const auto method=[&](const QString &id,const char *name,const HqBindingDetail::Asset &source){
            const auto fn=member(id,name);
            return fn.status==found && guardedScriptContextOperands(fn.value,ledger).status==GuardedScriptContextOperands::GlobalScopeOnly &&
                stock.matches(source,QString::fromLatin1(name),guardedScriptFunctionImage(fn.value,ledger,&images),ledger);
        };
        if(!method("UNITRANKINGSYSTEM","increaseRang",openingRanking) || !method("UNITRANKINGSYSTEM","getMaxRang",openingRanking))
            return reject("UNVERIFIED_KILL_PROMOTION");
        if(!method("ACTION","getEmptyFieldActions",*HqBindingDetail::asset("ACTION")))return reject("GLOBAL_COMMAND_LOOKUP");
        const auto commands=member("ACTION","emptyFieldActions");
        if(commands.status!=found)return reject("GLOBAL_COMMANDS_NOT_DATA");
        const auto list=guardedScriptArray(commands.value,ledger);
        if(list.status!=found || list.length>std::size(openingGlobalCommands))return reject("GLOBAL_COMMAND_LIST");
        for(std::uint32_t i=0;i<list.length;++i){
            const auto v=guardedScriptArrayElement(commands.value,i,ledger);
            if(v.status!=found)return reject("GLOBAL_COMMAND_ITEM");
            const auto s=guardedScriptScalar(v.value,ledger);auto id=std::get_if<QString>(&s.value);
            if(s.status!=GuardedScriptScalarStatus::Verified || !id)return reject("GLOBAL_COMMAND_IDENTITY");
            const auto a=std::find_if(std::begin(openingGlobalCommands),std::end(openingGlobalCommands),
                [&](const auto &source){return *id==QLatin1StringView(source.id);});
            if(a==std::end(openingGlobalCommands))return reject("UNSUPPORTED_GLOBAL_COMMAND_"+*id);
            if(!map.getGameRules()->getAllowedActions().contains(*id))continue;
            // Stock no-army transfer/delete/sell and no-CO powers are disabled.
            // Surrender affects its own side; draw and local UI orders cannot
            // defeat the other side. The ordinary turn transition is bound too.
            for(const auto *key:{"canBePerformed","isFinalStep","perform"})
                if(!method(*id,key,*a))return reject(*id+"."+key);
            // Disband/sell select only an unspent friendly unit. A shot that
            // survived its counter continues to occupy its firing square.
            if(openingPurchases && (*id=="ACTION_DELETE_UNIT" || *id=="ACTION_SELL_UNIT" || *id=="ACTION_TRANSFER_UNIT") &&
               !method(*id,"getStepData",*a))return reject(*id+".getStepData");
        }
        for(int y=0;y<map.getMapHeight();++y)for(int x=0;x<map.getMapWidth();++x){
            auto *b=map.getTerrain(x,y)->getBuilding();
            if(!b || (!openingPurchases && b->getOwner()!=&enemy))continue;
            auto kind=b->getBuildingID();const auto action=member(kind,"getActions"),menu=member(kind,"actionList");
            if(action.status!=found || catalog.authenticateFunction(action.value,GuardedNativeCodeKind::Building,"getActions").status!=bound)
                return reject("HOSTILE_PROPERTY_COMMAND_LOOKUP_"+kind);
            if(menu.status!=found)return reject("HOSTILE_PROPERTY_COMMAND_LIST_"+kind);
            const auto options=guardedScriptArray(menu.value,ledger);
            if(options.status!=found || (!openingPurchases && options.length!=0))return reject("HOSTILE_PROPERTY_CAN_ACT_"+kind);
            for(std::uint32_t j=0;j<options.length;++j){
                const auto item=guardedScriptArrayElement(menu.value,j,ledger);
                if(item.status!=found)return reject("HOSTILE_PROPERTY_COMMAND_ITEM_"+kind);
                const auto v=guardedScriptScalar(item.value,ledger);const auto *id=std::get_if<QString>(&v.value);
                if(v.status!=GuardedScriptScalarStatus::Verified || !id || *id!="ACTION_BUILD_UNITS")
                    return reject("HOSTILE_PROPERTY_CAN_ACT_"+kind);
            }
        }
        return true;
    }
}
