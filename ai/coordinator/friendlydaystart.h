#pragma once

#include "ai/coordinator/openingrepairbill.h"
#include "ai/coordinator/realcombatcapital.h"
#include "ai/coordinator/relativestockcomparison.h"
#include <array>
#include <limits>
#include <optional>
#include <vector>

namespace Coordinator
{
    // Indices address a single physical roster including purchases on F0.
    // Only eligible living units on their own service buildings belong here.
    struct FriendlyServiceSubject
    {
        OrdinaryOpeningRepair repair;
        MilliFunds price{};
        int fuel{}, maximumFuel{};
        std::array<int, 2> rounds{}, maximumRounds{};
        std::array<bool, 2> refillsSlot{};
    };

    struct FriendlyServiceResult
    {
        OpeningRepairBill bill;
        std::vector<FriendlyServiceSubject> subjects;
        MilliFunds materialGain{}, materialLessCash{};
    };

    inline std::optional<FriendlyServiceResult> settleFriendlyDayStart(
        std::int64_t wallet, std::vector<FriendlyServiceSubject> subjects)
    {
        std::vector<OrdinaryOpeningRepair> repairs;
        repairs.reserve(subjects.size());
        for (const auto &u : subjects)
        {
            if (u.price < 0 || u.repair.hpPercent < 1 || u.repair.hpPercent > 100 ||
                u.maximumFuel < -1 || u.fuel < -1 || (u.fuel < 0) != (u.maximumFuel < 0) || u.fuel > u.maximumFuel)
                return {};
            for (int slot = 0; slot < 2; ++slot)
                if (u.rounds[slot] < -1 || u.maximumRounds[slot] < -1 ||
                    (u.rounds[slot] < 0) != (u.maximumRounds[slot] < 0) || u.rounds[slot] > u.maximumRounds[slot])
                    return {};
            repairs.push_back(u.repair);
        }
        // Native tile visitation is randomized; the routine rejects competing
        // scarce repairs when the bound lacks that draw or order.
        auto bill = ordinaryOpeningRepairs(wallet, repairs);
        if (!bill) return {};
        FriendlyServiceResult result{std::move(*bill), std::move(subjects)};
        for (std::size_t i = 0; i < result.subjects.size(); ++i)
        {
            auto &u = result.subjects[i];
            const auto &row = result.bill.rows[i];
            if (!RelativeStockDetail::checkedAdd(result.materialGain,
                combatCapitalLoss(u.price, row.after / 10., row.before / 10.)))
                return {};
            if (u.maximumFuel >= 0) u.fuel = u.maximumFuel;
            for (int slot = 0; slot < 2; ++slot)
                if (u.refillsSlot[slot] && u.maximumRounds[slot] >= 0)
                    u.rounds[slot] = u.maximumRounds[slot];
            u.repair.hpPercent = row.after;
        }
        const auto debit = result.bill.before - result.bill.after;
        if (debit > std::numeric_limits<MilliFunds>::max() / MILLI_FUNDS_PER_FUND)
            return {};
        result.materialLessCash = result.materialGain;
        if (!RelativeStockDetail::checkedSubtract(result.materialLessCash, debit * MILLI_FUNDS_PER_FUND))
            return {};
        return result;
    }
}
