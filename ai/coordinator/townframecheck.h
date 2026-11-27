#pragma once
#include "ai/coordinator/townassaultintegration.h"
#include "townframetie.h"
#include <QCborValue>
#include <bit>
#include <type_traits>

namespace Coordinator::TownFrameCheck
{
    struct Compared
    {
        QString reason;
        std::vector<int> path;
        std::size_t leaves{},calendars{},releaseAliases{},epochs{};
        std::map<const void*,const void*> mutableA,mutableB,epochA,epochB;

        bool fail(const char *where){if(reason.isEmpty())reason=QString::fromLatin1(where);return false;}
        bool priorIdentity(const void *a,const void *b,bool epoch,bool &already)
        {
            auto &left=epoch?epochA:mutableA;auto &right=epoch?epochB:mutableB;
            auto [l,lf]=left.emplace(a,b);auto [r,rf]=right.emplace(b,a);
            if(lf!=rf || l->second!=b || r->second!=a)return fail("identity-alias");
            already=!lf;
            if(lf){if(epoch)++epochs;else ++releaseAliases;}
            return true;
        }

        template<class X,class Y,std::size_t... I> bool tuple(const X &a,const Y &b,std::index_sequence<I...>)
        {
            return (item(std::get<I>(a),std::get<I>(b),int(I)) && ...);
        }
        template<class T>bool item(const T &a,const T &b,int ordinal)
        {
            if(value(a,b))return true;
            path.push_back(ordinal);
            return false;
        }
        template<class T> bool value(const T &a,const T &b)
        {
            using V=std::remove_cv_t<T>;
            if constexpr(std::is_same_v<V,QString> || std::is_same_v<V,QByteArray>)
            {++leaves;return (a.isNull()==b.isNull() && a==b) || fail("string-value");}
            else if constexpr(std::is_arithmetic_v<V> || std::is_enum_v<V>)
            {
                ++leaves;
                if constexpr(std::is_floating_point_v<V>){
                    using Bits=std::conditional_t<sizeof(V)==4,std::uint32_t,std::uint64_t>;
                    return std::bit_cast<Bits>(a)==std::bit_cast<Bits>(b) || fail("floating-bits");}
                else return a==b || fail("value");
            }
            else if constexpr(std::is_same_v<V,QJsonArray> || std::is_same_v<V,QJsonObject> || std::is_same_v<V,QJsonValue>)
            {
                ++leaves;
                return QCborValue::fromJsonValue(a).toCbor()==QCborValue::fromJsonValue(b).toCbor() || fail("typed-json");
            }
            else if constexpr(std::is_same_v<V,StockCaptureTraffic>)
            {
                ++calendars;
                if(a.calendarId()!=b.calendarId() || a.generation()!=b.generation() || a.cellCount()!=b.cellCount() || a.lastTick()!=b.lastTick())
                    return fail("traffic-header");
                for(int i=0;i<a.cellCount();++i)if(!item(a.uses(i),b.uses(i),i))return false;
                return true;
            }
            else if constexpr(requires {a.has_value();*a;})
            {
                if(bool(a)!=bool(b))return fail("optional-presence");
                return !a || value(*a,*b);
            }
            else if constexpr(requires {typename V::element_type;a.get();a.use_count();})
            {
                using E=typename V::element_type;
                if(bool(a)!=bool(b))return fail("pointer-presence");
                if(!a)return true;
                if constexpr(!std::is_const_v<E>)if(a.get()==b.get())return fail("mutable-snapshot-shares-owner");
                if constexpr(std::is_same_v<E,const int> || !std::is_const_v<E>)
                {
                    bool already=false;
                    if(!priorIdentity(a.get(),b.get(),std::is_same_v<E,const int>,already))return false;
                    if(already)return true;
                    if constexpr(std::is_same_v<E,const int>)return value(*a,*b);
                }
                return a.get()==b.get() || value(*a,*b);
            }
            else if constexpr(requires {a.top();a.empty();})
            {
                if(a.size()!=b.size())return fail("queue-size");
                auto first=a,second=b;
                for(int i=0;!first.empty();++i){
                    if(!item(first.top(),second.top(),i))return false;first.pop();second.pop();}
                return true;
            }
            else if constexpr(requires {std::tuple_size<V>::value;})
                return tuple(a,b,std::make_index_sequence<std::tuple_size_v<V>>{});
            else if constexpr(requires {a.size();a.begin();a.end();})
            {
                if(a.size()!=b.size())return fail("container-size");
                auto x=a.begin(),y=b.begin();int i=0;
                for(;x!=a.end();++x,++y,++i)if(!item(*x,*y,i))return false;
                return true;
            }
            else if constexpr(std::is_aggregate_v<V>)
            {
                const auto left=fields(a),right=fields(b);
                return tuple(left,right,std::make_index_sequence<std::tuple_size_v<std::decay_t<decltype(left)>>>{});
            }
            else static_assert(std::is_same_v<V,void>,"Explicitly check this newly retained non-aggregate state type");
        }
    };

    inline Compared compareRebasedTownFrames(const TownMarketFrame &real,const TownMarketFrame &shadow)
    {
        Compared out;
        if(real.world!=shadow.world)out.fail("frame-world");
        else out.value(real,shadow);
        std::reverse(out.path.begin(),out.path.end());
        return out;
    }
}
