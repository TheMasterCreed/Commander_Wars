#pragma once

#include <QByteArray>

#include "ai/coordinator/guardednativeinventory.h"

namespace Coordinator
{
    struct GuardedStockDamageInput
    {
        GuardedNativeDamageFormula formula{GuardedNativeDamageFormula::AdvanceWars4};
        std::int32_t baseDamage{0};
        std::int32_t maximumOffense{0};
        std::int32_t minimumDefense{0};
        double calculationHp{0};
        double targetActualHp{0};
    };

    struct GuardedStockDamageEnvelope
    {
        std::int32_t maximumDamage{0};
        double lowerHp{0};
        double upperHp{0};
    };

    struct GuardedStockDamageResult
    {
        GuardedNativeBindingStatus status{GuardedNativeBindingStatus::Unsupported};
        std::optional<GuardedStockDamageEnvelope> envelope;
    };

    std::int64_t guardedStockDamageEnvelopeWork();
    // Arithmetic only; supplied HP and no-counter/effect premises still need authentication.
    GuardedStockDamageResult boundGuardedStockDamage(
        const GuardedStockDamageInput &, RefinementLedger &);

    // A pointwise family, not an expectation or optimization over current native RNG.
    struct GuardedStockCurrentFamily
    {
        QByteArray nativeBefore;
        std::int32_t width{0};
        std::int32_t height{0};
        std::int32_t targetUnitId{NO_UNIT};
        double lowerHp{0};
        double upperHp{0};
        std::vector<std::int64_t> repairCostPerHp;
        std::vector<GuardedNativeAction> commonPrefix;
    };

    std::optional<std::int64_t> guardedStockFamilyWork(std::size_t actors, std::size_t prefixActions);
    GuardedRelationStatus verifyGuardedStockFamilyRelation(
        const GuardedStockRoot & incomingAnchor, const GuardedStockRoot & challengerAnchor,
        const GuardedStockCurrentFamily & incomingFamily, const GuardedStockCurrentFamily & challengerFamily,
        const GuardedStockRelation &, RefinementLedger &);
}
