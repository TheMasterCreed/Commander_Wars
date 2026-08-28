#include "ai/counterdamagememo.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
    using CounterDamage::Memo;
    using CounterDamage::NO_VALUE;
    const std::string INFANTRY = "INFANTRY";
    const std::string ARTILLERY = "ARTILLERY";
    constexpr std::int32_t FIRST_KEY = 0;
    constexpr std::int32_t SECOND_KEY = 1;
    constexpr double SOFTENED_COUNTER_DAMAGE = 42.0;
    constexpr double HEALTHY_COUNTER_DAMAGE = 55.0;
    constexpr double FRACTIONAL_COUNTER_DAMAGE = 7.9;
    constexpr float TRUNCATED_COUNTER_DAMAGE = 7.0f;
    constexpr float NO_TAKEN_DAMAGE = 0.0f;
    constexpr float TAKEN_DAMAGE = 3.5f;
    constexpr float NEGATIVE_TAKEN_DAMAGE = -1.0f;
    int failureCount = 0;
    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }
    void testTaintedStoreIsRefused()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, SOFTENED_COUNTER_DAMAGE, TAKEN_DAMAGE);
        check(!memo.contains(INFANTRY), "a tainted value entered the memo");
        check(memo.value(INFANTRY) == NO_VALUE, "a tainted value was replayed");
    }
    void testTaintedStoreLeavesAnEarlierValueAlone()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, HEALTHY_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        memo.store(INFANTRY, SOFTENED_COUNTER_DAMAGE, TAKEN_DAMAGE);
        check(memo.value(INFANTRY) == static_cast<float>(HEALTHY_COUNTER_DAMAGE), "a tainted store overwrote an untainted value");
    }
    void testUntaintedStoreRoundTrips()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, HEALTHY_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        check(memo.contains(INFANTRY), "an untainted value was refused");
        check(memo.value(INFANTRY) == static_cast<float>(HEALTHY_COUNTER_DAMAGE), "an untainted value did not round trip");
    }
    void testNegativeTakenDamageStillStores()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, HEALTHY_COUNTER_DAMAGE, NEGATIVE_TAKEN_DAMAGE);
        check(memo.contains(INFANTRY), "a negative taken damage was treated as taint");
        check(memo.value(INFANTRY) == static_cast<float>(HEALTHY_COUNTER_DAMAGE), "a negative taken damage lost the value");
    }
    void testStoredValueTruncates()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, FRACTIONAL_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        check(memo.value(INFANTRY) == TRUNCATED_COUNTER_DAMAGE, "the stored value did not truncate");
    }
    void testUntaintedRestoreOverwrites()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, SOFTENED_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        memo.store(INFANTRY, HEALTHY_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        check(memo.value(INFANTRY) == static_cast<float>(HEALTHY_COUNTER_DAMAGE), "an untainted restore did not overwrite");
    }
    void testMissingKeyReadsAsNoValue()
    {
        Memo<std::string> memo;
        memo.store(INFANTRY, HEALTHY_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        check(!memo.contains(ARTILLERY), "an unstored key read as present");
        check(memo.value(ARTILLERY) == NO_VALUE, "an unstored key did not read as no value");
    }
    void testKeysAreIndependent()
    {
        Memo<std::int32_t> memo;
        memo.store(FIRST_KEY, HEALTHY_COUNTER_DAMAGE, NO_TAKEN_DAMAGE);
        memo.store(SECOND_KEY, SOFTENED_COUNTER_DAMAGE, TAKEN_DAMAGE);
        check(memo.value(FIRST_KEY) == static_cast<float>(HEALTHY_COUNTER_DAMAGE), "an untainted key lost its value");
        check(!memo.contains(SECOND_KEY), "a tainted key was stored beside an untainted one");
    }
}

int main()
{
    testTaintedStoreIsRefused();
    testTaintedStoreLeavesAnEarlierValueAlone();
    testUntaintedStoreRoundTrips();
    testNegativeTakenDamageStillStores();
    testStoredValueTruncates();
    testUntaintedRestoreOverwrites();
    testMissingKeyReadsAsNoValue();
    testKeysAreIndependent();
    return failureCount == 0 ? 0 : 1;
}
