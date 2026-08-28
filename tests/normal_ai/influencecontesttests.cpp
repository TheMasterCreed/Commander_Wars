#include "ai/influencecontest.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    using InfluenceContest::getRatio;
    using InfluenceContest::NEUTRAL_RATIO;
    using InfluenceContest::TOTAL_CONTROL_RATIO;

    constexpr std::int32_t NO_INFLUENCE = 0;
    constexpr std::int32_t WEAK_INFLUENCE = 3;
    constexpr std::int32_t STRONG_INFLUENCE = 5;
    constexpr std::int32_t MINIMUM_INFLUENCE = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t MAXIMUM_INFLUENCE = std::numeric_limits<std::int32_t>::max();

    constexpr float ENEMY_LEAD_RATIO = 0.4f;
    constexpr float OWNER_LEAD_RATIO = -ENEMY_LEAD_RATIO;
    constexpr float RATIO_TOLERANCE = 0.000001f;
    constexpr std::int32_t SWEEP_BOUND = 8;

    static_assert(getRatio(NO_INFLUENCE, STRONG_INFLUENCE) == TOTAL_CONTROL_RATIO);

    int failureCount = 0;

    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void checkFinite(float actual, std::string_view message)
    {
        if (!std::isfinite(actual))
        {
            std::cerr << message << ": expected a finite ratio, got " << actual << '\n';
            ++failureCount;
        }
    }

    void checkNear(float actual, float expected, std::string_view message)
    {
        if (!std::isfinite(actual) || std::fabs(actual - expected) > RATIO_TOLERANCE)
        {
            std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
            ++failureCount;
        }
    }

    // The tile the defect fired on: no owner reach at all, so the old formula divided by zero.
    void testUncontestedEnemyTile()
    {
        checkNear(getRatio(NO_INFLUENCE, STRONG_INFLUENCE), TOTAL_CONTROL_RATIO, "total enemy control");
        checkNear(getRatio(NO_INFLUENCE, MAXIMUM_INFLUENCE), TOTAL_CONTROL_RATIO, "saturated enemy control");
    }

    void testEmptyAndOwnedTiles()
    {
        checkNear(getRatio(NO_INFLUENCE, NO_INFLUENCE), NEUTRAL_RATIO, "empty tile");
        checkNear(getRatio(STRONG_INFLUENCE, NO_INFLUENCE), NEUTRAL_RATIO, "owner only tile");
        checkNear(getRatio(STRONG_INFLUENCE, STRONG_INFLUENCE), NEUTRAL_RATIO, "evenly contested tile");
    }

    void testContestedTilesKeepTheirValue()
    {
        checkNear(getRatio(WEAK_INFLUENCE, STRONG_INFLUENCE), ENEMY_LEAD_RATIO, "enemy leads");
        checkNear(getRatio(STRONG_INFLUENCE, WEAK_INFLUENCE), OWNER_LEAD_RATIO, "owner leads");
    }

    void testRatioStaysNormalisedOverTheRealDomain()
    {
        for (std::int32_t ownInfluence = NO_INFLUENCE; ownInfluence <= SWEEP_BOUND; ++ownInfluence)
        {
            for (std::int32_t enemyInfluence = NO_INFLUENCE; enemyInfluence <= SWEEP_BOUND; ++enemyInfluence)
            {
                const float ratio = getRatio(ownInfluence, enemyInfluence);
                checkFinite(ratio, "non negative sweep");
                check(ratio >= -TOTAL_CONTROL_RATIO && ratio <= TOTAL_CONTROL_RATIO,
                      "non negative sweep leaves the normalised range");
            }
        }
    }

    // Influences are accumulated as non negative counts today; totality keeps a future change from reviving the inf.
    void testRatioIsTotal()
    {
        checkFinite(getRatio(MINIMUM_INFLUENCE, NO_INFLUENCE), "minimum own influence");
        checkFinite(getRatio(NO_INFLUENCE, MINIMUM_INFLUENCE), "minimum enemy influence");
        checkFinite(getRatio(MINIMUM_INFLUENCE, MAXIMUM_INFLUENCE), "opposed extremes");
        checkFinite(getRatio(MAXIMUM_INFLUENCE, MINIMUM_INFLUENCE), "reversed extremes");
        for (std::int32_t ownInfluence = -SWEEP_BOUND; ownInfluence <= SWEEP_BOUND; ++ownInfluence)
        {
            for (std::int32_t enemyInfluence = -SWEEP_BOUND; enemyInfluence <= SWEEP_BOUND; ++enemyInfluence)
            {
                checkFinite(getRatio(ownInfluence, enemyInfluence), "signed sweep");
            }
        }
    }
}

int main()
{
    testUncontestedEnemyTile();
    testEmptyAndOwnedTiles();
    testContestedTilesKeepTheirValue();
    testRatioStaysNormalisedOverTheRealDomain();
    testRatioIsTotal();
    return failureCount == 0 ? 0 : 1;
}
