#pragma once

#include "ai/coordinator/hqopeningbindings.h"
#include "ai/coordinator/guardednativecode.h"
#include <cmath>

namespace Coordinator
{
    namespace ScreenFireDetail
    {
        inline constexpr HqBindingDetail::Asset join{"ACTION_JOIN","actions/ACTION_JOIN.js","6e87c384dae61f34777942455be5c55c99148b677515f45a6a828f07d0673454",false};
        inline constexpr HqBindingDetail::Asset missile{"ACTION_MISSILE","actions/ACTION_MISSILE.js","979b64ea5c67964d16e6d9472ca3bf5ee4cd4393e8ff638c7fa568f8d12f4f13",false};
        // These implementations only dispatch to the live two-column native
        // weapon table. A different implementation provides no damage bound.
        inline constexpr HqBindingDetail::Asset weapons[]{
            {"WEAPON_INFANTRY_MG","weapons/weapon_infantry_mg.js","1174fb3044820f69e05eb602d524c9f37df94dbfc17db56119350c6a43338c03",false},
            {"WEAPON_LIGHT_TANK_GUN","weapons/weapon_light_tank_gun.js","542e7747523ddf0cb3ce5a24252de968811a1f766200cb2fa04d660a86f92596",false},
            {"WEAPON_LIGHT_TANK_MG","weapons/weapon_light_tank_mg.js","e4fabf7f33f0830ab74ad6ea549cae421e812a53315a955156347773a4dfe6a2",false},
            {"WEAPON_VULCAN_CANNON","weapons/weapon_vulcan_cannon.js","5f0a7cb4ba10e8d0a1570a706e68528ad5daa59a1709ff6dbd00fa9523fd05eb",false},
            {"WEAPON_BAZOOKA","weapons/weapon_bazooka.js","a72630b0dcfd0993eaad95d5c507b67460dba0c04b6cee2ac9467e9bae16a502",false},
            {"WEAPON_MECH_MG","weapons/weapon_mech_mg.js","6824e4873f1865a6d41223c14e26af8fd4463838a108d0b1c7905cebfa68a331",false}
        };

        inline std::optional<std::map<QString,int>> damageRows(const GuardedScriptValueView &table,RefinementLedger &ledger)
        {
            const auto rows=guardedScriptArray(table,ledger);
            if(rows.status!=GuardedScriptPropertyStatus::Found)return {};
            std::map<QString,int> result;
            for(std::uint32_t i=0;i<rows.length;++i)
            {
                auto r=guardedScriptArrayElement(table,i,ledger);
                if(r.status!=GuardedScriptPropertyStatus::Found)return {};
                auto a=guardedScriptArray(r.value,ledger);
                if(a.status!=GuardedScriptPropertyStatus::Found || a.length!=2)return {};
                auto id=guardedScriptArrayElement(r.value,0,ledger),power=guardedScriptArrayElement(r.value,1,ledger);
                if(id.status!=GuardedScriptPropertyStatus::Found || power.status!=GuardedScriptPropertyStatus::Found)return {};
                const auto name=guardedScriptScalar(id.value,ledger),value=guardedScriptScalar(power.value,ledger);
                const auto *label=std::get_if<QString>(&name.value);const auto *d=std::get_if<double>(&value.value);
                if(name.status!=GuardedScriptScalarStatus::Verified || !label || label->isEmpty() ||
                   value.status!=GuardedScriptScalarStatus::Verified || !d || !std::isfinite(*d) ||
                   *d>10000 || *d< -1 || std::floor(*d)!=*d)return {};
                result.try_emplace(*label,static_cast<int>(*d)); // first matching native table row
            }
            return result;
        }
    }

    struct ScreenBoundRules
    {
        std::set<QString> admitted;
        bool ordinaryOpeningPurchases{};
        std::map<QString,std::optional<std::map<QString,int>>> rows;
        std::map<QString,std::map<QString,int>> costs;
        QString reason;
    };

    inline ScreenBoundRules readScreenBoundRules(QJSEngine &engine,RefinementLedger &ledger,HqBindingDetail::StockMethods &methods,
        const std::set<QString> &terrainTypes,const std::set<QString> &properties,const std::set<QString> &weather,
        const std::set<QString> &stockTypes,const std::set<QString> &allTypes,const std::vector<QString> &tileIds,
        const std::set<QString> *openingBuys=nullptr)
    {
        ScreenBoundRules out;
        const auto fail=[&](const QString &s){out.reason=s;return out;};
        using Kind=GuardedNativeCodeKind;
        constexpr auto bound=GuardedNativeBindingStatus::Bound;
        constexpr auto found=GuardedScriptPropertyStatus::Found;
        std::set<QString> names=allTypes;
        if(openingBuys){names.insert("ACTION_BUILD_UNITS");names.insert(openingBuys->begin(),openingBuys->end());}
        names.insert(terrainTypes.begin(),terrainTypes.end());names.insert(properties.begin(),properties.end());
        names.insert(weather.begin(),weather.end());names.insert({"UNIT","PLAYER","Global","ACTION","ACTION_FIRE","WEAPON",
            "ACTION_JOIN","ACTION_MISSILE","ACTION_NEXT_PLAYER","ACTION_WAIT","ACTION_CAPTURE"});
        for(const auto &a:ScreenFireDetail::weapons)names.insert(QString::fromLatin1(a.id));
        std::vector<QString> globals(names.begin(),names.end());
        auto vars=GuardedScriptGlobalIndex::prepare(engine,globals,ledger);
        GuardedNativeCodeCatalog catalog(ledger);auto frame=catalog.observe(engine);
        if(vars.status()!=found || frame.status()!=bound)return fail("UNVERIFIED_SCREEN_REALM");
        const auto alias=vars.find(engine,"Global",ledger);
        if(alias.status!=found || guardedScriptGlobalAlias(engine,alias.value,ledger).status!=found)return fail("UNVERIFIED_SCREEN_GLOBALS");
        GuardedScriptOwnIndexCache own(engine,ledger);GuardedScriptImageCache images(engine,ledger);
        const auto member=[&](const QString &id,const char *key){
            const auto obj=vars.find(engine,id,ledger);
            return obj.status==found?guardedScriptProperty(obj.value,QString::fromLatin1(key),ledger,&own):GuardedScriptProperty{};
        };
        const auto quiet=[&](const QString &id,const char *key){
            const auto v=member(id,key);return v.status==GuardedScriptPropertyStatus::Missing ||
                (v.status==found && (v.value.isNull() || v.value.kind()==GuardedScriptValueKind::Undefined));
        };
        const auto canonical=[&](const QString &id,const char *key,Kind kind){
            const auto v=member(id,key);
            return v.status==found && catalog.authenticateFunction(v.value,kind,QString::fromLatin1(key)).status==bound;
        };
        if(!quiet("PLAYER","startOfTurn") || !quiet("PLAYER","endOfTurn") ||
           !canonical("ACTION_NEXT_PLAYER","perform",Kind::NextPlayer) ||
           !canonical("ACTION_WAIT","perform",Kind::Wait) || !canonical("ACTION_CAPTURE","perform",Kind::Capture) ||
           !canonical("ACTION_CAPTURE","performPostAnimation",Kind::Capture))return fail("NONORDINARY_TURN_OR_MOVEMENT_EFFECTS");
        for(const auto *name:{"calcBattleDamage3","calcBattleDamage4","calcAttackerWeaponDamage","calcAttackerDamage",
                "calcDefenderWeaponDamage","calcDefenderDamage","calcDamage","getDefaultLuck","predictSupportDamageReduction",
                "doSupportDamageReduction","applyPostDamageReduction","perform","performPostAnimation","battle"})
            if(!canonical("ACTION_FIRE",name,Kind::Fire))return fail(QString("UNVERIFIED_FIRE_")+name);
        for(const auto *name:{"getDamageFromTable","calculateDamage"})
            if(!canonical("WEAPON",name,Kind::Weapon))return fail(QString("UNVERIFIED_WEAPON_")+name);
        for(const auto &type:properties)
            for(const auto *name:{"getOffensiveBonus","getDefensiveBonus","getOffensiveFieldBonus","getDeffensiveFieldBonus"})
                if(!quiet(type,name))return fail("ACTIVE_PROPERTY_COMBAT_MODIFIER");
        for(const auto &type:terrainTypes)
        {
            if(!quiet(type,"getOffensiveFieldBonus"))
            {
                const auto id=member(type,"baseTerrainId");
                const bool passive=id.status==GuardedScriptPropertyStatus::Missing ||
                    (id.status==found && guardedScriptScalar(id.value,ledger).status==GuardedScriptScalarStatus::Verified);
                if(!passive || !(canonical(type,"getOffensiveFieldBonus",Kind::BaseMountain) ||
                    canonical(type,"getOffensiveFieldBonus",Kind::BaseForest) ||
                    canonical(type,"getOffensiveFieldBonus",Kind::BaseStreet)))return fail("ACTIVE_TERRAIN_OFFENSE");
                // Those canonical functions return either zero or minus twenty.
            }
            for(const auto *name:{"getDeffensiveFieldBonus","getFirerangeModifier","getMinFirerangeModifier"})
                if(!quiet(type,name))return fail("ACTIVE_TERRAIN_COMBAT_MODIFIER");
        }
        for(const auto &type:weather)
            for(const auto *name:{"getOffensiveModifier","getDefensiveModifier","getFirerangeModifier","getMinFirerangeModifier"})
                if(!quiet(type,name))return fail("ACTIVE_WEATHER_COMBAT_MODIFIER");
        const auto matches=[&](const HqBindingDetail::Asset &asset,const char *key){
            const auto v=member(QString::fromLatin1(asset.id),key);
            return v.status==found && guardedScriptContextOperands(v.value,ledger).status==GuardedScriptContextOperands::GlobalScopeOnly &&
                methods.matches(asset,QString::fromLatin1(key),guardedScriptFunctionImage(v.value,ledger,&images),ledger);
        };
        if(!matches(ScreenFireDetail::join,"perform") || !matches(ScreenFireDetail::join,"performPostAnimation") ||
           !matches(ScreenFireDetail::missile,"canBePerformed") || !matches(ScreenFireDetail::missile,"getMissileBuildings"))
            return fail("UNVERIFIED_JOIN_OR_LAUNCH");
        if(openingBuys){
            const auto *build=HqBindingDetail::asset("ACTION_BUILD_UNITS");
            bool ordinary=build && matches(*build,"perform");
            for(const auto &type:*openingBuys){
                const auto *asset=HqBindingDetail::asset(type);
                if(!ordinary || !asset || !asset->recruit || !matches(*asset,"init") ||
                   !quiet(type,"initForMods") || !quiet(type,"buildedUnit") || !quiet(type,"predictSupportDamageReduction") ||
                   !quiet(type,"doSupportDamageReduction")){ordinary=false;break;}
            }
            out.ordinaryOpeningPurchases=ordinary;
        }
        auto &admitted=out.admitted;
        for(const auto &type:allTypes)
        {
            const auto *asset=HqBindingDetail::asset(type);
            bool ok=asset && asset->recruit && canonical(type,"getActions",Kind::Unit) &&
                canonical(type,"getUnitDamage",Kind::Unit);
            for(const auto *name:{"getUnitDamageID","getLoadingPlace","getBonusOffensive","getBonusDefensive","getFirstStrike",
                    "startOfTurn","endOfTurn","postAction","postBattleActions","buildedUnit","predictSupportDamageReduction","doSupportDamageReduction"})
                if(ok && !quiet(type,name))ok=false;
            if(ok)
            {
                const auto actions=member(type,"actionList");
                const auto a=guardedScriptArray(actions.value,ledger);
                ok=actions.status==found && a.status==found && a.length>0;
                for(std::uint32_t i=0;ok && i<a.length;++i)
                {
                    const auto v=guardedScriptArrayElement(actions.value,i,ledger);
                    const auto s=guardedScriptScalar(v.value,ledger);
                    const auto *name=std::get_if<QString>(&s.value);
                    ok=v.status==found && name && (*name=="ACTION_FIRE" || *name=="ACTION_JOIN" ||
                        *name=="ACTION_LOAD" || *name=="ACTION_UNLOAD" || *name=="ACTION_WAIT" ||
                        *name=="ACTION_CAPTURE" || *name=="ACTION_CO_UNIT_0" || *name=="ACTION_CO_UNIT_1" ||
                        *name=="ACTION_MISSILE"); // Only non-launch properties passed the movement certificate.
                }
            }
            if(ok && matches(*asset,"init") && matches(*asset,"getMovementType"))admitted.insert(type);
            else if(stockTypes.contains(type))return fail("UNVERIFIED_SCREEN_ROSTER_"+type);
        }
        auto &rows=out.rows;
        for(const auto &a:ScreenFireDetail::weapons)
        {
            const QString id=QString::fromLatin1(a.id);
            if(!matches(a,"getBaseDamage") || !canonical(id,"calculateDamage",Kind::Weapon))continue;
            const auto table=member(id,"damageTable");
            if(table.status==found)rows[id]=ScreenFireDetail::damageRows(table.value,ledger);
        }
        // Each supported movement table is immutable through this weather
        // window. All genuine entry edges pay at least one. Compare individual
        // directional costs before using a stronger reverse-distance field.
        auto &costs=out.costs;
        for(auto [id,kind]:{std::pair{QString("MOVE_FEET"),Kind::MoveFeet},std::pair{QString("MOVE_TANK"),Kind::MoveTank},
                std::pair{QString("MOVE_TIRE_A"),Kind::MoveTireA},std::pair{QString("MOVE_MECH"),Kind::MoveMech}})
        {
            if(catalog.authenticateStockMovementCallbacks(engine,kind).status!=bound)continue;
            const auto tables=catalog.authenticateStockMovementTables(engine,kind);
            if(tables.status!=bound || tables.tables.size()!=1)continue;
            std::map<QString,int> field;bool good=true;
            for(const auto &name:tileIds)if(!field.contains(name))
            {
                const auto v=guardedNativeIntegerTableValue(tables.tables.front().evidence,name,ledger);
                if(v.status!=bound || v.value.value_or(-1)==0){good=false;break;}
                field[name]=v.value.value_or(-1);
            }
            if(good)costs.emplace(id,std::move(field));
        }
        return out;
    }
}
