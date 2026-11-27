#pragma once

#include "ai/coordinator/guardedscriptfunction.h"
#include "ai/coordinator/hqopeningassets.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <map>
#include <set>

namespace Coordinator
{
    struct HeadquartersTurnUnits
    {
        std::set<QString> outgoing;
        std::map<QString,int> incomingMinimumFuel;
    };

    namespace HqBindingDetail
    {
        inline const Asset *asset(const QString &id)
        {
            for(const auto &a:stockAssets)if(id==QLatin1StringView(a.id))return &a;
            return nullptr;
        }

        // Only immutable, hash-checked stock scripts are evaluated, solely in
        // a detached standard JS engine without game objects. The live engine
        // is read through plain guarded data descriptors. Do not trust a
        // replaceable source-text cache, filename, or a function's toString.
        class StockMethods
        {
        public:
            explicit StockMethods(QString root=QCoreApplication::applicationDirPath()+"/resources/scripts",QStringList extraMethods={})
                :m_root(std::move(root)),m_extraMethods(std::move(extraMethods)){}
            StockMethods(QString root,bool serviceMembers)
                :StockMethods(std::move(root)){m_serviceMembers=serviceMembers;}

            template<bool withCo = false> bool matches(const Asset &a,const QString &member,const GuardedScriptFunctionImage &actual,RefinementLedger &ledger)
            {
                if(actual.status!=GuardedScriptFunctionStatus::Verified || actual.kind!=GuardedScriptFunctionKind::Script)return false;
                if(!ledger.consume(64+4*(actual.compilationName.size()+member.size()),RefinementWork::MasterPreparation))return false;
                const QUrl uri(actual.compilationName);
                const auto input=uri.scheme()=="file"?uri.toLocalFile():
                    uri.scheme()=="qrc"?":"+uri.path():QString{};
                if(!QDir::fromNativeSeparators(input).endsWith("/resources/scripts/"+QString::fromLatin1(a.file)))return false;
                const auto key=std::pair{QString::fromLatin1(a.id),actual.compilationName};
                auto module=m_modules.find(key);
                if(module==m_modules.end())
                {
                    if(m_modules.size()>=128)return false;
                    auto sampled=sample<withCo>(a,input,ledger);
                    if(sampled.empty())return false;
                    module=m_modules.emplace(key,std::move(sampled)).first;
                }
                const auto known=module->second.find(member);
                if(known==module->second.end())return false;
                return std::any_of(known->second.begin(),known->second.end(),[&](const auto &approved){
                    return actual.sameImplementation(approved);});
            }
            bool standardIndexOf(const GuardedScriptFunctionImage &actual,RefinementLedger &ledger)
            {
                if(!m_indexOf)
                {
                    if(!ledger.consume(131072,RefinementWork::MasterPreparation))return false;
                    QJSEngine reference;
                    const auto fn=reference.globalObject().property("Array").property("prototype").property("indexOf");
                    auto signature=guardedScriptFunctionImage(fn,ledger);
                    if(signature.status!=GuardedScriptFunctionStatus::Verified)return false;
                    m_indexOf=std::move(signature);
                }
                return actual.sameImplementation(*m_indexOf);
            }
        private:
            using MethodImages=std::map<QString,std::vector<GuardedScriptFunctionImage>>;
            template<bool withCo> MethodImages sample(const Asset &a,const QString &input,RefinementLedger &ledger) const
            {
                const auto at=QString::fromLatin1(a.file);
                QFile f(QDir(m_root).filePath(at));
                if(!f.open(QIODevice::ReadOnly)){f.setFileName(":/resources/scripts/"+at);if(!f.open(QIODevice::ReadOnly))return {};}
                if(f.size()<0 || f.size()>512*1024 ||
                   !ledger.consume(256+4*f.size(),RefinementWork::MasterPreparation))return {};
                auto bytes=f.readAll().replace("\r\n","\n");
                if(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()!=a.sha)return {};
                const auto literal=QString::fromUtf8(bytes);
                // Replays may evaluate the file with its lines simplified.
                QString replay;
                for(auto line:QStringView(literal).split(u'\n'))replay+=line.toString().simplified()+'\n';
                if(literal.endsWith(u'\n'))replay.chop(1);
                // Reuse the guarded native-code ledger's realm construction
                // tariff and source-byte compilation tariff. All trusted
                // members of the same module share the two compilations.
                if(!ledger.consume(131072,RefinementWork::MasterPreparation))return {};
                QJSEngine reference;
                for(const auto *parent:{"UNIT","ACTION","BUILDING","VICTORYRULE","GameEnums"})
                    reference.globalObject().setProperty(QString::fromLatin1(parent),reference.newObject());
                if(!m_extraMethods.isEmpty())reference.globalObject().setProperty("WEAPON",reference.newObject());
                if constexpr(withCo)reference.globalObject().setProperty("CO",reference.newObject());
                reference.installExtensions(QJSEngine::TranslationExtension);
                MethodImages out;
                for(const auto &source:{literal,replay})
                {
                    if(!ledger.consume(32*source.toUtf8().size()+2048,RefinementWork::MasterPreparation))return {};
                    if(reference.evaluate(source,input).isError())continue;
                    const auto receiver=reference.globalObject().property(QString::fromLatin1(a.id));
                    GuardedScriptImageCache images(reference,ledger);
                    const auto collect=[&](const QString &member){
                        const auto fn=receiver.property(member);
                        if(!fn.isCallable())return true;
                        auto img=guardedScriptFunctionImage(fn,ledger,&images);
                        if(img.status!=GuardedScriptFunctionStatus::Verified)return false;
                        out[member].push_back(std::move(img));return true;
                    };
                    if(m_serviceMembers){
                        for(const auto *key:{"startOfTurn","endOfTurn","replenishUnit","repairUnit","canRepair","getConstructionList","getRepairTypes","getBaseIncome","perform"})
                            if(!collect(QString::fromLatin1(key)))return {};
                    }else for(const auto *key:{"init","getMovementType","startOfTurn","moveUnit","doWalkingAnimation","perform",
                        "canBePerformed","performPostAnimation","getCapturableBuildings","isEmptyFieldAndHasNotMoved",
                        "checkDefeat","checkHQCount","checkUnitCount","getRuleValue","onCaptured","addCaptureAnimationBuilding",
                        "getArmyNameFromPlayerTable","getArmyNameFromTable","getDataFromTable"})if(!collect(QString::fromLatin1(key)))return {};
                    for(const auto &member:m_extraMethods)if(!collect(member))return {};
                }
                return out;
            }
            QString m_root;
            bool m_serviceMembers{};
            QStringList m_extraMethods;
            std::map<std::pair<QString,QString>,MethodImages> m_modules;
            std::optional<GuardedScriptFunctionImage> m_indexOf;
        };
    }

    // Verify the contracts for this specific current purchase and capture.
    // Other map/rules, CO, action effects and physical prerequisites still
    // belong to the standing-HQ caller. No identifying map or actor is used.
    inline bool ordinaryOpeningCaptureBindings(QJSEngine &engine,const QString &capper,const QJsonArray &opening,
        std::int64_t &budget,HqBindingDetail::StockMethods &stock,QString *failure=nullptr,const HeadquartersTurnUnits &turnUnits={},bool *workRefused=nullptr)
    {
        using namespace HqBindingDetail;
        const auto reject=[&](const QString &why){if(failure)*failure=why;return false;};
        if(budget<=0){if(workRefused)*workRefused=true;return reject("WORK_LIMIT");}
        RefinementLedger ledger;ledger.open(budget);
        struct Debit { std::int64_t &work;RefinementLedger &ledger;bool *refused;~Debit(){work-=ledger.stats().spent();if(refused && ledger.stats().refusals)*refused=true;} } debit{budget,ledger,workRefused};
        if(!ledger.consume(256+64*opening.size(),RefinementWork::MasterPreparation))return reject("WORK_LIMIT");
        const auto *cap=asset(capper);
        if(!cap || !cap->recruit)return reject("UNSUPPORTED_CAPPER");
        std::set<QString> recruits;
        for(const auto &o:opening)
        {
            const auto name=o.toObject()["type"].toString();const auto *born=asset(name);
            if(!born || !born->recruit)return reject("UNSUPPORTED_RECRUIT_"+name);
            recruits.insert(name);
        }
        GuardedScriptOwnIndexCache own(engine,ledger);GuardedScriptImageCache images(engine,ledger);
        std::set<QString> globals{capper,"ACTION_CAPTURE","ACTION_NEXT_PLAYER","ACTION","HQ","BUILDING","Global",
            "getArmyNameFromPlayerTable","getArmyNameFromTable","getDataFromTable",
            "VICTORYRULE_NOHQ","VICTORYRULE_NOUNITS"};
        if(!recruits.empty())globals.insert("ACTION_BUILD_UNITS");
        globals.insert(recruits.begin(),recruits.end());
        globals.insert(turnUnits.outgoing.begin(),turnUnits.outgoing.end());
        for(const auto &[type,fuel]:turnUnits.incomingMinimumFuel)globals.insert(type);
        std::vector<QString> keys(globals.begin(),globals.end());
        auto index=GuardedScriptGlobalIndex::prepare(engine,keys,ledger);
        if(index.status()!=GuardedScriptPropertyStatus::Found)return reject("GLOBAL_LOOKUP_UNAVAILABLE");
        const auto alias=index.find(engine,"Global",ledger);
        if(alias.status!=GuardedScriptPropertyStatus::Found ||
           guardedScriptGlobalAlias(engine,alias.value,ledger).status!=GuardedScriptPropertyStatus::Found)return reject("GLOBAL_ALIAS");
        const auto lookup=[&](const QString &id,const QString &name){
            const auto obj=index.find(engine,id,ledger);
            return obj.status==GuardedScriptPropertyStatus::Found?guardedScriptProperty(obj.value,name,ledger,&own):GuardedScriptProperty{};
        };
        const auto method=[&](const QString &id,const char *key,const char *source=nullptr,bool inherited=false){
            const auto name=QString::fromLatin1(key);
            const auto p=id=="Global"?index.find(engine,name,ledger):lookup(id,name);const auto *a=asset(source?QString::fromLatin1(source):id);
            if(a && p.status==GuardedScriptPropertyStatus::Found &&
               guardedScriptContextOperands(p.value,ledger).status==GuardedScriptContextOperands::GlobalScopeOnly)
            {
                const auto img=guardedScriptFunctionImage(p.value,ledger,&images);
                if(stock.matches(*a,name,img,ledger) || (inherited && stock.matches(*asset(id),name,img,ledger)))return true;
            }
            return reject(id+"."+name);
        };
        const auto absent=[&](const QString &id,const char *key){
            const auto p=lookup(id,QString::fromLatin1(key));
            if(p.status==GuardedScriptPropertyStatus::Missing || (p.status==GuardedScriptPropertyStatus::Found &&
               (p.value.isNull() || p.value.kind()==GuardedScriptValueKind::Undefined)))return true;
            return reject(id+"."+QString::fromLatin1(key));
        };
        if(!opening.isEmpty() && !method("ACTION_BUILD_UNITS","perform"))return false;
        if(!method("ACTION_NEXT_PLAYER","perform") || !method("ACTION","isEmptyFieldAndHasNotMoved") ||
           !method("VICTORYRULE_NOHQ","checkDefeat") || !method("VICTORYRULE_NOHQ","checkHQCount") ||
           !method("VICTORYRULE_NOUNITS","checkDefeat") || !method("VICTORYRULE_NOUNITS","checkUnitCount") ||
           !method("VICTORYRULE_NOUNITS","getRuleValue","VICTORYRULE") ||
           !method("HQ","onCaptured","BUILDING") || !method("HQ","addCaptureAnimationBuilding"))return false;
        for(const auto *key:{"getArmyNameFromPlayerTable","getArmyNameFromTable","getDataFromTable"})
            if(!method("Global",key))return false;
        const auto hqList=lookup("BUILDING","hqIds");
        if(hqList.status!=GuardedScriptPropertyStatus::Found ||
           guardedScriptIndexedArrayData(hqList.value,ledger).status!=GuardedScriptPropertyStatus::Found)return reject("HQ_RULE_TYPES");
        const auto hqShape=guardedScriptArray(hqList.value,ledger);
        if(hqShape.status!=GuardedScriptPropertyStatus::Found || hqShape.length!=3)return reject("HQ_RULE_TYPES");
        std::set<QString> hqTypes{"HQ","FORTHQ","FIELD_BASE"};
        for(std::uint32_t i=0;i<hqShape.length;++i)
        {
            const auto item=guardedScriptArrayElement(hqList.value,i,ledger);
            if(item.status!=GuardedScriptPropertyStatus::Found)return reject("HQ_RULE_TYPES");
            const auto value=guardedScriptScalar(item.value,ledger);
            if(value.status!=GuardedScriptScalarStatus::Verified || value.kind!=GuardedScriptValueKind::String ||
               !hqTypes.erase(std::get<QString>(value.value)))return reject("HQ_RULE_TYPES");
        }
        for(const auto *key:{"canBePerformed","perform","performPostAnimation","getCapturableBuildings"})
            if(!method("ACTION_CAPTURE",key))return false;
        const auto list=lookup("ACTION_CAPTURE","capturableBuildings");
        if(list.status!=GuardedScriptPropertyStatus::Found)return reject("CAPTURE_LIST");
        const auto arr=guardedScriptArray(list.value,ledger);bool hq=false;
        if(arr.status!=GuardedScriptPropertyStatus::Found ||
           guardedScriptIndexedArrayData(list.value,ledger).status!=GuardedScriptPropertyStatus::Found)return reject("CAPTURE_LIST");
        const auto proto=guardedScriptIntrinsicPrototype(engine,GuardedScriptIntrinsicPrototype::Array,ledger);
        const auto slot=guardedScriptBuiltinProperty(proto.value,"indexOf",ledger);
        if(proto.status!=GuardedScriptPropertyStatus::Found || slot.status!=GuardedScriptPropertyStatus::Found ||
           !stock.standardIndexOf(guardedScriptFunctionImage(slot.value,ledger,&images),ledger))return reject("CAPTURE_INDEX_OF");
        for(std::uint32_t i=0;i<arr.length;++i)
        {
            const auto item=guardedScriptArrayElement(list.value,i,ledger);
            if(item.status!=GuardedScriptPropertyStatus::Found)return reject("CAPTURE_LIST_ITEM");
            const auto scalar=guardedScriptScalar(item.value,ledger);
            if(scalar.status!=GuardedScriptScalarStatus::Verified || scalar.kind!=GuardedScriptValueKind::String)return reject("CAPTURE_LIST_ITEM");
            hq|=std::get<QString>(scalar.value)=="HQ";
        }
        if(!hq)return reject("HQ_NOT_CAPTURABLE");
        if(!method(capper,"moveUnit","UNIT",true) || !method(capper,"doWalkingAnimation","UNIT",true) ||
           !absent(capper,"getCaptureBonus"))return false;
        for(const auto &born:recruits)if(!method(born,"init") || !method(born,"getMovementType") ||
            !absent(born,"buildedUnit") || !absent(born,"initForMods"))return false;
        // End-of-turn belongs to F0 survivors and newborns. Start-of-turn
        // belongs to surviving E0 units, including any of the same type.
        recruits.insert(turnUnits.outgoing.begin(),turnUnits.outgoing.end());
        for(const auto &type:recruits)
            if(!absent(type,"endOfTurn") || !absent(type,"postAction"))return false;
        if(!absent(capper,"startOfTurn") || !absent(capper,"postAction"))return false;
        bool verifiedPoint=false;
        for(const auto &[type,fuel]:turnUnits.incomingMinimumFuel)
        {
            const auto hook=lookup(type,"startOfTurn");
            if(hook.status==GuardedScriptPropertyStatus::Missing || (hook.status==GuardedScriptPropertyStatus::Found &&
               (hook.value.isNull() || hook.value.kind()==GuardedScriptValueKind::Undefined)))continue;
            // These pinned functions only subtract the documented local fuel
            // debit. The caller excludes CO modifiers; require enough fuel to
            // avoid a death action and authenticate the point constructor.
            const auto *known=asset(type);
            if(!known || known->localFuelDebit<=0 || fuel<known->localFuelDebit ||
               !method(type,"startOfTurn"))return reject(type+".startOfTurn");
            if(!verifiedPoint)
            {
                verifiedPoint=guardedScriptQtValueConstructors(engine,ledger).status==GuardedNativeReceiverStatus::Bound;
                if(!verifiedPoint)return reject("Qt.point");
            }
        }
        return true;
    }

    inline bool ordinaryOpeningCaptureBindings(QJSEngine &engine,const QString &capper,const QJsonArray &opening,std::int64_t &budget,
        const HeadquartersTurnUnits &turnUnits,bool *workRefused=nullptr)
    {
        // Cache approved, detached code images only. Every live binding and
        // property is freshly observed on each call, including on a new realm.
        thread_local HqBindingDetail::StockMethods stock;
        return ordinaryOpeningCaptureBindings(engine,capper,opening,budget,stock,nullptr,turnUnits,workRefused);
    }
}
