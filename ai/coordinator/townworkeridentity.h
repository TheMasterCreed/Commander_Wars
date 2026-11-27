#pragma once

#include "ai/coordinator/townassaultintegration.h"

namespace Coordinator
{
    // The original route code compares these tokens for equality. It never
    // prices their numerical magnitude. Rebind only the predicted incoming
    // token; every independent child calendar keeps its unique token.
    struct FrozenTownCalendarBindings
    {
        static std::uint64_t liveCount()
        {
            return StockCaptureTraffic::nextCalendarId.load(std::memory_order_relaxed);
        }
        static bool canFork() {return liveCount()<TownWorkerCalendarId::privateBase;}
        static void markIncoming(StockCaptureTraffic &calendar)
        {
            calendar.m_calendarId=TownWorkerCalendarId::incoming;
        }
        static std::optional<std::uint64_t> reserve(std::uint64_t amount)
        {
            auto &counter=StockCaptureTraffic::nextCalendarId;
            auto first=counter.load(std::memory_order_relaxed);
            for(;;){
                if(first>=TownWorkerCalendarId::privateBase || amount>=TownWorkerCalendarId::privateBase-first)return {};
                if(!amount || counter.compare_exchange_weak(first,first+amount,std::memory_order_relaxed))return first;
            }
        }

        template<class Frame,class Fn> static void visit(Frame &frame,Fn fn)
        {
            const auto progress=[&](auto &s){
                if(s.afterFirst)fn(s.afterFirst->traffic.m_calendarId);
                if(s.empty && s.empty->cleared)fn(s.empty->cleared->traffic.m_calendarId);
            };
            for(auto &[key,slot]:frame.opportunities){
                for(auto &[date,work]:slot.dates){
                    progress(work.attack);
                    progress(work.suffixes.held.search);
                    progress(work.suffixes.afterFire.search);
                }
                for(auto &[key,policy]:slot.prefixes.open){
                    fn(policy.walkCalendar);
                    progress(policy.progress);
                }
                for(auto &[key,prefix]:slot.prefixes.known)progress(prefix.capture);
            }
        }
        static bool contains(const TownMarketFrame &frame,std::uint64_t id)
        {
            bool found=false;
            visit(frame,[&](const auto &value){found|=value==id;});
            return found;
        }
        static bool canRebind(const TownMarketFrame &frame,std::uint64_t allocations)
        {
            bool valid=true;
            visit(frame,[&](const auto &id){
                if(id>=TownWorkerCalendarId::privateBase && id!=TownWorkerCalendarId::incoming)
                    valid&=id-TownWorkerCalendarId::privateBase<allocations;
            });
            return valid;
        }
        static void rebind(TownMarketFrame &frame,std::uint64_t first,std::uint64_t current)
        {
            visit(frame,[&](auto &id){
                if(id==TownWorkerCalendarId::incoming)id=current;
                else if(id>=TownWorkerCalendarId::privateBase)id=first+(id-TownWorkerCalendarId::privateBase);
            });
        }
    };
}
