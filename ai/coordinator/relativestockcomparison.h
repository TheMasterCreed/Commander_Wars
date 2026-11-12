#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#ifdef COW_COORDINATED_REFINEMENT_TESTING
#include <optional>
#endif
#include <span>
#include <vector>

#include "ai/coordinator/continuationinterval.h"

namespace Coordinator
{
    class PropertyStockField;
    struct PropertyStockInstance;
    class RelativeStockSession;

    class RelativeStockIdentity final
    {
    public:
        RelativeStockIdentity() = default;

        bool established() const
        {
            const auto binding = m_binding.lock();
            return binding && binding->snapshot != nullptr && binding->enemyModel != nullptr;
        }

        bool sameLiveModel(const RelativeStockIdentity & other) const
        {
            const auto left = m_binding.lock();
            const auto right = other.m_binding.lock();
            return left && right && left == right && left->snapshot != nullptr &&
                left->enemyModel != nullptr && left->snapshot == right->snapshot &&
                left->enemyModel == right->enemyModel;
        }

    private:
        friend class RelativeStockSession;

        struct Binding
        {
            const PropertyStockField* snapshot;
            const PropertyStockInstance* enemyModel;
        };

        explicit RelativeStockIdentity(const std::shared_ptr<const Binding> & binding)
            : m_binding(binding)
        {
        }

        std::weak_ptr<const Binding> m_binding;
    };

    struct RelativeStockQuote
    {
        ContinuationKey key;
        std::vector<std::int32_t> capturedColumns;
        RelativeStockIdentity identity;
        MilliFunds economic{0};
        MilliFunds owned{0};
        StockInterval ours;
        StockInterval enemy;
        bool ourLowerWitnessReplays{false};
        bool enemyLowerWitnessReplays{false};
        bool ourSearchCompleted{false};
        bool enemySearchCompleted{false};
        std::uint64_t ourUpperCertificateId{0};
        bool valid{false};

        bool absoluteExact() const
        {
            return valid && identity.established() &&
                ourSearchCompleted && enemySearchCompleted &&
                ours.exact() && enemy.exact();
        }
    };

    enum class RelativeStockReason : std::int8_t
    {
        InvalidIdentity,
        InvalidQuote,
        NonCanonicalKey,
        InvalidInterval,
        ContradictoryCommonEnemy,
        MissingOurWitness,
        // Retired with start-of-turn enemy pricing; kept so recorded reason numbers stay stable.
        MissingEnemyWitness,
        ArithmeticOverflow,
        CertifiedPositive,
        CertifiedNonImprovement,
        Overlap,
    };

    struct RelativeStockComparison
    {
        MilliFunds deltaLower{0};
        MilliFunds deltaUpper{0};
        MilliFunds economicDelta{0};
        MilliFunds ownedDelta{0};
        bool commonEnemyCancelled{false};
        RelativeStockReason reason{RelativeStockReason::InvalidIdentity};

        bool valid() const
        {
            return reason == RelativeStockReason::CertifiedPositive ||
                reason == RelativeStockReason::CertifiedNonImprovement ||
                reason == RelativeStockReason::Overlap;
        }

        bool improves() const
        {
            return reason == RelativeStockReason::CertifiedPositive;
        }

        bool unresolved() const
        {
            return reason == RelativeStockReason::Overlap;
        }

        bool exact() const
        {
            return valid() && deltaLower == deltaUpper;
        }
    };

    namespace RelativeStockDetail
    {
        inline bool checkedAdd(MilliFunds & value, MilliFunds term)
        {
            constexpr auto minimum = std::numeric_limits<MilliFunds>::min();
            constexpr auto maximum = std::numeric_limits<MilliFunds>::max();
            if ((term > 0 && value > maximum - term) ||
                (term < 0 && value < minimum - term))
            {
                return false;
            }
            value += term;
            return true;
        }

        inline bool checkedSubtract(MilliFunds & value, MilliFunds term)
        {
            constexpr auto minimum = std::numeric_limits<MilliFunds>::min();
            constexpr auto maximum = std::numeric_limits<MilliFunds>::max();
            if ((term > 0 && value < minimum + term) ||
                (term < 0 && value > maximum + term))
            {
                return false;
            }
            value -= term;
            return true;
        }

        inline bool canonical(const RelativeStockQuote & quote)
        {
            for (std::size_t index = 0; index < quote.key.size(); ++index)
            {
                if (quote.key[index].row() < 0 ||
                    (index > 0 && quote.key[index - 1].row() >= quote.key[index].row()))
                {
                    return false;
                }
            }
            return std::is_sorted(quote.capturedColumns.begin(), quote.capturedColumns.end()) &&
                std::adjacent_find(quote.capturedColumns.begin(), quote.capturedColumns.end()) ==
                    quote.capturedColumns.end() &&
                std::all_of(quote.capturedColumns.begin(), quote.capturedColumns.end(),
                            [](std::int32_t column) { return column >= 0; });
        }

        inline bool coherent(const RelativeStockQuote & quote)
        {
            return quote.ours.lower <= quote.ours.upper && quote.enemy.lower <= quote.enemy.upper &&
                (!quote.ourSearchCompleted || quote.ours.exact()) &&
                (!quote.enemySearchCompleted || quote.enemy.exact());
        }
    }

    inline RelativeStockComparison compareRelativeStock(
        const RelativeStockQuote & incumbent, const RelativeStockQuote & challenger)
    {
        RelativeStockComparison result;
        if (!incumbent.identity.sameLiveModel(challenger.identity))
        {
            return result;
        }
        if (!incumbent.valid || !challenger.valid)
        {
            result.reason = RelativeStockReason::InvalidQuote;
            return result;
        }
        if (!RelativeStockDetail::canonical(incumbent) || !RelativeStockDetail::canonical(challenger))
        {
            result.reason = RelativeStockReason::NonCanonicalKey;
            return result;
        }
        if (!RelativeStockDetail::coherent(incumbent) || !RelativeStockDetail::coherent(challenger))
        {
            result.reason = RelativeStockReason::InvalidInterval;
            return result;
        }
        // One live model has one enemy value, so both intervals must bracket it and the term cancels.
        if (std::max(incumbent.enemy.lower, challenger.enemy.lower) >
            std::min(incumbent.enemy.upper, challenger.enemy.upper))
        {
            result.reason = RelativeStockReason::ContradictoryCommonEnemy;
            return result;
        }
        if (!incumbent.ourLowerWitnessReplays || !challenger.ourLowerWitnessReplays)
        {
            result.reason = RelativeStockReason::MissingOurWitness;
            return result;
        }
        result.economicDelta = challenger.economic;
        result.ownedDelta = challenger.owned;
        if (!RelativeStockDetail::checkedSubtract(result.economicDelta, incumbent.economic) ||
            !RelativeStockDetail::checkedSubtract(result.ownedDelta, incumbent.owned))
        {
            result.reason = RelativeStockReason::ArithmeticOverflow;
            return result;
        }
        MilliFunds constant = result.economicDelta;
        if (!RelativeStockDetail::checkedAdd(constant, result.ownedDelta))
        {
            result.reason = RelativeStockReason::ArithmeticOverflow;
            return result;
        }
        MilliFunds lower = constant;
        MilliFunds upper = constant;
        if (!RelativeStockDetail::checkedAdd(lower, challenger.ours.lower) ||
            !RelativeStockDetail::checkedSubtract(lower, incumbent.ours.upper) ||
            !RelativeStockDetail::checkedAdd(upper, challenger.ours.upper) ||
            !RelativeStockDetail::checkedSubtract(upper, incumbent.ours.lower))
        {
            result.reason = RelativeStockReason::ArithmeticOverflow;
            return result;
        }
        result.deltaLower = lower;
        result.deltaUpper = upper;
        result.commonEnemyCancelled = true;
        result.reason = lower > 0 ? RelativeStockReason::CertifiedPositive :
            upper <= 0 ? RelativeStockReason::CertifiedNonImprovement : RelativeStockReason::Overlap;
        return result;
    }

#ifdef COW_COORDINATED_REFINEMENT_TESTING
    inline std::optional<MilliFunds>
    commonEnemySequentialThreshold(
        const RelativeStockQuote & incumbent,
        const RelativeStockQuote & challenger)
    {
        if (!incumbent.identity.sameLiveModel(challenger.identity) ||
            !incumbent.valid || !challenger.valid ||
            !RelativeStockDetail::canonical(incumbent) ||
            !RelativeStockDetail::canonical(challenger) ||
            !RelativeStockDetail::coherent(incumbent) ||
            !RelativeStockDetail::coherent(challenger) ||
            incumbent.capturedColumns !=
                challenger.capturedColumns ||
            !challenger.ourLowerWitnessReplays)
        {
            return std::nullopt;
        }
        MilliFunds threshold = challenger.economic;
        if (!RelativeStockDetail::checkedSubtract(
                threshold, incumbent.economic) ||
            !RelativeStockDetail::checkedAdd(
                threshold, challenger.owned) ||
            !RelativeStockDetail::checkedSubtract(
                threshold, incumbent.owned) ||
            !RelativeStockDetail::checkedAdd(
                threshold, challenger.ours.lower))
        {
            return std::nullopt;
        }
        return threshold;
    }
#endif

    inline bool relativeStockUpperFirst(
        const RelativeStockComparison & left, std::span<const std::int32_t> leftTuple,
        const RelativeStockComparison & right, std::span<const std::int32_t> rightTuple)
    {
        if (left.deltaUpper != right.deltaUpper)
        {
            return left.deltaUpper > right.deltaUpper;
        }
        if (left.deltaLower != right.deltaLower)
        {
            return left.deltaLower > right.deltaLower;
        }
        return std::lexicographical_compare(
            leftTuple.begin(), leftTuple.end(), rightTuple.begin(), rightTuple.end());
    }

    inline bool sameRelativeStockQuote(const RelativeStockQuote & left, const RelativeStockQuote & right)
    {
        return left.valid && right.valid && left.identity.sameLiveModel(right.identity) &&
            left.key == right.key && left.capturedColumns == right.capturedColumns &&
            left.economic == right.economic && left.owned == right.owned &&
            left.ours.lower == right.ours.lower && left.ours.upper == right.ours.upper &&
            left.enemy.lower == right.enemy.lower && left.enemy.upper == right.enemy.upper &&
            left.ourLowerWitnessReplays == right.ourLowerWitnessReplays &&
            left.enemyLowerWitnessReplays == right.enemyLowerWitnessReplays &&
            left.ourSearchCompleted == right.ourSearchCompleted &&
            left.enemySearchCompleted == right.enemySearchCompleted &&
            left.ourUpperCertificateId ==
                right.ourUpperCertificateId;
    }
}
