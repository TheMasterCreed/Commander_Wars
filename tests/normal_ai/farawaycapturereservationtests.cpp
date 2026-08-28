#include "ai/farawaycapturereservations.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    using FarAwayCapture::Reservations;
    using FarAwayCapture::Target;
    constexpr std::int32_t FIRST_DAY = 3;
    constexpr std::int32_t NEXT_DAY = 4;
    constexpr std::int32_t LATER_DAY = 9;
    constexpr Target NEUTRAL_FACTORY{7, 11};
    constexpr Target SECOND_FACTORY{2, 5};
    constexpr std::int32_t SWEEP_BOUND = 4;
    static_assert(NEUTRAL_FACTORY == Target{7, 11});
    static_assert(NEUTRAL_FACTORY != SECOND_FACTORY);
    int failureCount = 0;
    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }
    void testReservationHoldsWithinTheTurn()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        check(reservations.isReserved(FIRST_DAY, NEUTRAL_FACTORY), "reservation lost inside its own turn");
    }
    void testDayChangeRetiresTheReservation()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        check(!reservations.isReserved(NEXT_DAY, NEUTRAL_FACTORY), "reservation survived the day change");
        check(!reservations.isReserved(LATER_DAY, NEUTRAL_FACTORY), "reservation survived several days");
    }
    void testQueryingTheOldDayDoesNotResurrectIt()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        check(!reservations.isReserved(NEXT_DAY, NEUTRAL_FACTORY), "reservation survived the day change");
        check(!reservations.isReserved(FIRST_DAY, NEUTRAL_FACTORY), "retired reservation came back");
    }
    void testReserveOnANewDayRetiresTheOldSet()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        reservations.reserve(NEXT_DAY, SECOND_FACTORY);
        check(!reservations.isReserved(NEXT_DAY, NEUTRAL_FACTORY), "reserving retired no stale target");
        check(reservations.isReserved(NEXT_DAY, SECOND_FACTORY), "fresh reservation was dropped");
    }
    void testUnrelatedTargetsAreIndependent()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        check(!reservations.isReserved(FIRST_DAY, SECOND_FACTORY), "an unreserved target read as reserved");
    }
    void testReserveIsIdempotentWithinTheDay()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        check(reservations.isReserved(FIRST_DAY, NEUTRAL_FACTORY), "repeated reserve dropped the target");
        check(!reservations.isReserved(NEXT_DAY, NEUTRAL_FACTORY), "a repeated reserve outlived the day");
    }
    void testResetDropsEveryReservation()
    {
        Reservations reservations;
        reservations.reserve(FIRST_DAY, NEUTRAL_FACTORY);
        reservations.reserve(FIRST_DAY, SECOND_FACTORY);
        reservations.reset();
        check(!reservations.isReserved(FIRST_DAY, NEUTRAL_FACTORY), "reset kept a reservation");
        check(!reservations.isReserved(FIRST_DAY, SECOND_FACTORY), "reset kept a second reservation");
    }

    void testEveryTargetRetiresOnTheNextDay()
    {
        Reservations reservations;
        for (std::int32_t x = 0; x <= SWEEP_BOUND; ++x)
        {
            for (std::int32_t y = 0; y <= SWEEP_BOUND; ++y)
            {
                reservations.reserve(FIRST_DAY, Target{x, y});
            }
        }
        bool allHeldWithinTheTurn = true;
        bool allRetired = true;
        for (std::int32_t x = 0; x <= SWEEP_BOUND; ++x)
        {
            for (std::int32_t y = 0; y <= SWEEP_BOUND; ++y)
            {
                allHeldWithinTheTurn = allHeldWithinTheTurn && reservations.isReserved(FIRST_DAY, Target{x, y});
            }
        }
        for (std::int32_t x = 0; x <= SWEEP_BOUND; ++x)
        {
            for (std::int32_t y = 0; y <= SWEEP_BOUND; ++y)
            {
                allRetired = allRetired && !reservations.isReserved(NEXT_DAY, Target{x, y});
            }
        }
        check(allHeldWithinTheTurn, "sweep lost a reservation inside its own turn");
        check(allRetired, "sweep kept a reservation past the day change");
    }
}

int main()
{
    testReservationHoldsWithinTheTurn();
    testDayChangeRetiresTheReservation();
    testQueryingTheOldDayDoesNotResurrectIt();
    testReserveOnANewDayRetiresTheOldSet();
    testUnrelatedTargetsAreIndependent();
    testReserveIsIdempotentWithinTheDay();
    testResetDropsEveryReservation();
    testEveryTargetRetiresOnTheNextDay();
    return failureCount == 0 ? 0 : 1;
}
