#pragma once

#include "ai/coordinator/townassaultintegration.h"

namespace Coordinator
{
    // A forecast of the *next* market evaluation can mutate its copied cursors.
    // Preserve sharing between dates in this frame, and reject any mutable
    // cursor also owned by another frame which a later commit could disconnect.
    inline std::optional<TownMarketFrame> forkOwnedTownFrame(const TownMarketFrame &original)
    {
        const auto visitReleases=[](auto &frame,auto visit) {
            for(auto &[key,opportunity]:frame.opportunities) {
                for(auto &[date,work]:opportunity.dates)
                    if(work.release)visit(work.release);
                for(auto &[date,release]:opportunity.prefixes.friendlyReleases)
                    if(release)visit(release);
            }
        };
        struct Share {
            std::size_t occurrences{};
            std::size_t owners{};
            std::shared_ptr<TownReleaseSearch> copy;
        };
        std::map<const TownReleaseSearch *,Share> releases;
        visitReleases(original,[&](const std::shared_ptr<TownReleaseSearch> &value) {
            auto &entry=releases[value.get()];
            ++entry.occurrences;
            entry.owners=static_cast<std::size_t>(value.use_count());
        });
        for(const auto &[unused,entry]:releases)
            if(entry.owners!=entry.occurrences)return {};
        TownMarketFrame copy=original;
        visitReleases(copy,[&](std::shared_ptr<TownReleaseSearch> &value) {
            auto &cloned=releases.at(value.get()).copy;
            if(!cloned)cloned=std::make_shared<TownReleaseSearch>(*value);
            value=cloned;
        });
        return copy;
    }
}
