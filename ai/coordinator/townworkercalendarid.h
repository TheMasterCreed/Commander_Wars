#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Coordinator::TownWorkerCalendarId
{
    inline constexpr std::uint64_t privateBase=std::uint64_t{1}<<63;
    inline constexpr std::uint64_t incoming=std::numeric_limits<std::uint64_t>::max();
    inline thread_local std::uint64_t *active{};

    // Unselected or interrupted forecasts never advance the live turn's clock.
    inline std::uint64_t allocate(std::atomic<std::uint64_t> &ordinary)
    {
        if(!active)return ordinary.fetch_add(1,std::memory_order_relaxed);
        if(*active>=privateBase-1)throw std::overflow_error("Private calendar identity space exhausted");
        return privateBase+(*active)++;
    }
    class Scope
    {
        std::uint64_t *prior{};
    public:
        explicit Scope(std::uint64_t &count):prior(active){active=&count;}
        ~Scope(){active=prior;}
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
    };
}
