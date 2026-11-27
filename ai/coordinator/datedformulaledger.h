#pragma once

#include "ai/coordinator/relativestockcomparison.h"
#include "ai/coordinator/terminalvalue.h"

#include <limits>
#include <span>
#include <tuple>

namespace Coordinator
{
    // Both the stock forecast and a native trajectory use these existing
    // booked components. A generated opportunity and its later replacement
    // must remain paired. A physical wallet difference cannot stand in for them.
    struct DatedFormulaTerms
    {
        MilliFunds stock, foregoneShot, ownedIncome, holdingLoss;
        MilliFunds survivorNextShotAdjustment, replacedNextShot, replacedCapperNextShot;
        MilliFunds replacedHomeCapperNextShot, returnValue, productionValue;
    };

    template<typename Terms>
    inline std::optional<MilliFunds> nominalContinuationValue(MilliFunds sum, const Terms &terms)
    {
        using RelativeStockDetail::checkedAdd;
        using RelativeStockDetail::checkedSubtract;
        if (!checkedAdd(sum, terms.stock) || !checkedSubtract(sum, terms.foregoneShot) ||
            !checkedAdd(sum, terms.ownedIncome) || !checkedSubtract(sum, terms.holdingLoss) ||
            !checkedAdd(sum, terms.survivorNextShotAdjustment) || !checkedSubtract(sum, terms.replacedNextShot) ||
            !checkedSubtract(sum, terms.replacedCapperNextShot) || !checkedSubtract(sum, terms.replacedHomeCapperNextShot) ||
            !checkedAdd(sum, terms.returnValue) || !checkedAdd(sum, terms.productionValue))
            return {};
        return sum;
    }

    // This is the stock formula's adjustment from nominal final balance to
    // average round balance. Round once after aggregating every flow channel.
    inline std::optional<MilliFunds> cashTimingAdjustment(std::span<const MilliFunds> flows)
    {
        MilliFunds suffix = 0, delay = 0;
        for (std::size_t i = flows.size(); i > 1; --i)
            if (!RelativeStockDetail::checkedAdd(suffix, flows[i - 1]) ||
                !RelativeStockDetail::checkedAdd(delay, suffix))
                return {};
        return flows.empty() ? MilliFunds{0} : -(delay / static_cast<MilliFunds>(flows.size()));
    }

    inline int projectedHqThreatRank(int side, int tick)
    {
        return side < 0 && tick >= 0 && tick <= 5 ? tick - 6 : 0;
    }

    struct NativePropertyPayday
    {
        int halfTurn;
        bool enemy;
        MilliFunds amount;
    };

    struct NativePropertySchedule
    {
        MilliFunds nominal{};
        std::vector<MilliFunds> perRound;
    };

    // Each observation is the separately identified property-income receipt
    // for the active side before it acts. Include zero-income turns. The first
    // friendly payday belongs to F1 because F0 starts after its income was paid.
    inline std::optional<NativePropertySchedule> nativePropertySchedule(int horizon, int lastPayday,
        std::span<const NativePropertyPayday> receipts)
    {
        if (horizon <= 0 || horizon > std::numeric_limits<int>::max() / 2 ||
            lastPayday < 0 || lastPayday > 2 * horizon || receipts.size() != static_cast<std::size_t>(lastPayday))
            return {};
        NativePropertySchedule schedule;
        schedule.perRound.resize(horizon);
        for (int i = 0; i < lastPayday; ++i)
        {
            const auto &receipt = receipts[i];
            if (receipt.halfTurn != i + 1 || receipt.enemy != (i % 2 == 0) || receipt.amount < 0)
                return {};
            const auto signedAmount = receipt.enemy ? -receipt.amount : receipt.amount;
            if (!RelativeStockDetail::checkedAdd(schedule.nominal, signedAmount) ||
                !RelativeStockDetail::checkedAdd(schedule.perRound[i / 2], signedAmount))
                return {};
        }
        return schedule;
    }

    struct NonPropertyContinuation
    {
        MilliFunds openingNonStock;
        // Zero the two property channels and remove the income earned by
        // recruits from productionValue. All property paydays are booked once
        // by nativePropertySchedule, irrespective of the capturer's identity.
        DatedFormulaTerms terms;
        std::span<const MilliFunds> roundFlows;
    };

    struct TrajectoryHeadquarters
    {
        TerminalClass openingTerminal;
        int security;
        int projectedSide;
        int projectedTick;
    };

    // Numerical reducer for a single fully bound continuation. The caller
    // supplies the ordinary opening HQ test and assessed guard duties. A later
    // win on a native path cannot replace that opening test. Missing source
    // opportunities or unsupported actions leave the complement unavailable.
    inline std::optional<std::tuple<TerminalClass, int, int, MilliFunds>> nativeTrajectoryRank(
        int horizon, std::span<const NativePropertyPayday> paydays,
        std::optional<TrajectoryHeadquarters> hq, std::optional<NonPropertyContinuation> remainder)
    {
        if (!hq || horizon <= 0 || horizon > std::numeric_limits<int>::max() / 2 ||
            hq->security < -2 || hq->security > 0)
            return {};
        if (hq->openingTerminal == TerminalClass::ForcedWin || hq->openingTerminal == TerminalClass::ForcedLoss)
            return std::tuple{hq->openingTerminal, hq->security, 0, MilliFunds{0}};
        if (hq->openingTerminal != TerminalClass::Unresolved || !remainder ||
            hq->projectedSide < -1 || hq->projectedSide > 1 ||
            (hq->projectedSide == 0 && hq->projectedTick != -1) ||
            (hq->projectedSide != 0 && (hq->projectedTick < 0 || hq->projectedTick > 2 * horizon)) ||
            remainder->terms.stock != 0 || remainder->terms.ownedIncome != 0)
            return {};
        const int cutoff = hq->projectedTick < 0 ? 2 * horizon : hq->projectedTick;
        auto property = nativePropertySchedule(horizon, cutoff, paydays);
        if (!property || property->perRound.size() != remainder->roundFlows.size())
            return {};
        for (std::size_t i = 0; i < property->perRound.size(); ++i)
            if (!RelativeStockDetail::checkedAdd(property->perRound[i], remainder->roundFlows[i]))
                return {};
        auto terms = remainder->terms;
        terms.ownedIncome = property->nominal;
        auto value = nominalContinuationValue(remainder->openingNonStock, terms);
        const auto timing = cashTimingAdjustment(property->perRound);
        if (!value || !timing || !RelativeStockDetail::checkedAdd(*value, *timing))
            return {};
        return std::tuple{hq->openingTerminal, hq->security, projectedHqThreatRank(hq->projectedSide, hq->projectedTick), *value};
    }
}
