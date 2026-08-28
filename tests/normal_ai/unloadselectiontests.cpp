#include "ai/unloadselection.h"

#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace
{
    using UnloadSelection::Candidate;
    using UnloadSelection::Choice;
    using UnloadSelection::FALLBACK;
    using UnloadSelection::FieldSource;
    using UnloadSelection::FIRST_FIELD;
    using UnloadSelection::NO_FIELD;
    using UnloadSelection::resolveLoadedIndex;
    using UnloadSelection::select;

    constexpr std::int32_t FIRST_CANDIDATE = 0;
    constexpr std::int32_t SECOND_CANDIDATE = 1;

    constexpr std::int32_t FIRST_LOADED_UNIT = 0;
    constexpr std::int32_t SECOND_LOADED_UNIT = 1;
    constexpr std::int32_t THIRD_LOADED_UNIT = 2;

    constexpr std::int32_t ZERO_UNLOAD_FIELDS = 0;
    constexpr std::int32_t ONE_UNLOAD_FIELD = 1;
    constexpr std::int32_t TWO_UNLOAD_FIELDS = 2;
    constexpr std::int32_t THREE_UNLOAD_FIELDS = 3;

    constexpr std::int32_t SECOND_FIELD = 1;
    constexpr std::int32_t THIRD_FIELD = 2;

    constexpr bool NEEDS_REFUEL = true;
    constexpr bool SUPPLIED = false;
    constexpr bool CAN_CAPTURE = true;
    constexpr bool CANNOT_CAPTURE = false;

    constexpr bool PARSED = true;
    constexpr bool NOT_PARSED = false;
    constexpr std::int32_t LOADED_UNIT_COUNT = 3;
    constexpr std::int32_t NEGATIVE_ACTION_ID = -1;
    constexpr std::int32_t FIRST_MENU_POSITION = 0;
    constexpr std::int32_t SECOND_MENU_POSITION = 1;

    constexpr Candidate BLOCKED_CAPTURER{FIRST_LOADED_UNIT, SUPPLIED, CAN_CAPTURE, THREE_UNLOAD_FIELDS, NO_FIELD};
    constexpr Candidate SINGLE_FIELD_CAPTURER{FIRST_LOADED_UNIT, SUPPLIED, CAN_CAPTURE, ONE_UNLOAD_FIELD, FIRST_FIELD};

    constexpr Candidate SINGLE_FIELD_ONLY[]{SINGLE_FIELD_CAPTURER};
    static_assert(select(SINGLE_FIELD_ONLY, {}) == Choice{FIRST_CANDIDATE, FIRST_FIELD, FieldSource::MarkedFieldData});

    int failureCount = 0;

    void check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void checkChoice(const Choice & actual, const Choice & expected, std::string_view message)
    {
        if (actual != expected)
        {
            std::cerr << message
                      << ": expected candidate " << expected.candidate
                      << " field " << expected.field
                      << " source " << static_cast<std::int32_t>(expected.source)
                      << ", got candidate " << actual.candidate
                      << " field " << actual.field
                      << " source " << static_cast<std::int32_t>(actual.source)
                      << '\n';
            ++failureCount;
        }
    }

    void testLaterSingleFieldCandidateStaysReachable()
    {
        constexpr Candidate candidates[]{
            BLOCKED_CAPTURER,
            {SECOND_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        checkChoice(select(candidates, {}),
                    Choice{SECOND_CANDIDATE, FIRST_FIELD, FieldSource::MarkedFieldData},
                    "later single field candidate");
    }

    void testLaterCaptureTargetStaysReachable()
    {
        constexpr Candidate candidates[]{
            BLOCKED_CAPTURER,
            {SECOND_LOADED_UNIT, SUPPLIED, CAN_CAPTURE, TWO_UNLOAD_FIELDS, SECOND_FIELD},
        };
        checkChoice(select(candidates, {}),
                    Choice{SECOND_CANDIDATE, SECOND_FIELD, FieldSource::CandidateField},
                    "later capture target");
    }

    void testDedupeKeysOnLoadedIndex()
    {
        constexpr Candidate candidates[]{
            {SECOND_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
            {THIRD_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        constexpr std::int32_t alreadyUnloaded[]{SECOND_LOADED_UNIT};
        checkChoice(select(candidates, alreadyUnloaded),
                    Choice{SECOND_CANDIDATE, FIRST_FIELD, FieldSource::MarkedFieldData},
                    "dedupe on loaded index");
    }

    void testRefuelCandidateIsSkipped()
    {
        constexpr Candidate candidates[]{
            {FIRST_LOADED_UNIT, NEEDS_REFUEL, CAN_CAPTURE, TWO_UNLOAD_FIELDS, FIRST_FIELD},
            {SECOND_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        checkChoice(select(candidates, {}),
                    Choice{SECOND_CANDIDATE, FIRST_FIELD, FieldSource::MarkedFieldData},
                    "refuel candidate skipped");
    }

    void testNoReachableTileFallsBack()
    {
        constexpr Candidate candidates[]{
            {FIRST_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ZERO_UNLOAD_FIELDS, NO_FIELD},
        };
        checkChoice(select(candidates, {}), FALLBACK, "no reachable tile");
        checkChoice(select({}, {}), FALLBACK, "no candidates at all");
    }

    void testUnselectableCandidateKeepsItsSlot()
    {
        constexpr Candidate candidates[]{
            Candidate{},
            {SECOND_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        checkChoice(select(candidates, {}),
                    Choice{SECOND_CANDIDATE, FIRST_FIELD, FieldSource::MarkedFieldData},
                    "unselectable placeholder keeps its slot");
    }

    void testEarlyCaptureTargetKeepsPriority()
    {
        constexpr Candidate candidates[]{
            {FIRST_LOADED_UNIT, SUPPLIED, CAN_CAPTURE, THREE_UNLOAD_FIELDS, THIRD_FIELD},
            {SECOND_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        checkChoice(select(candidates, {}),
                    Choice{FIRST_CANDIDATE, THIRD_FIELD, FieldSource::CandidateField},
                    "early capture target keeps priority");
    }

    void testEveryUnloadedCandidateFallsBack()
    {
        constexpr Candidate candidates[]{
            {FIRST_LOADED_UNIT, SUPPLIED, CANNOT_CAPTURE, ONE_UNLOAD_FIELD, NO_FIELD},
        };
        constexpr std::int32_t alreadyUnloaded[]{FIRST_LOADED_UNIT};
        checkChoice(select(candidates, alreadyUnloaded), FALLBACK, "already unloaded candidate");
        check(UnloadSelection::isUnloaded(alreadyUnloaded, FIRST_LOADED_UNIT), "isUnloaded finds a known index");
        check(!UnloadSelection::isUnloaded(alreadyUnloaded, SECOND_LOADED_UNIT), "isUnloaded rejects an unknown index");
    }

    void checkLoadedIndex(std::int32_t actual, std::int32_t expected, std::string_view message)
    {
        if (actual != expected)
        {
            std::cerr << message << ": expected loaded index " << expected << ", got " << actual << '\n';
            ++failureCount;
        }
    }

    void testLoadedIndexResolution()
    {
        checkLoadedIndex(resolveLoadedIndex(PARSED, THIRD_LOADED_UNIT, FIRST_MENU_POSITION, LOADED_UNIT_COUNT),
                         THIRD_LOADED_UNIT, "in range action id");
        checkLoadedIndex(resolveLoadedIndex(NOT_PARSED, THIRD_LOADED_UNIT, FIRST_MENU_POSITION, LOADED_UNIT_COUNT),
                         FIRST_MENU_POSITION, "unparsable action id");
        checkLoadedIndex(resolveLoadedIndex(PARSED, LOADED_UNIT_COUNT, FIRST_MENU_POSITION, LOADED_UNIT_COUNT),
                         FIRST_MENU_POSITION, "action id past the last loaded unit");
        checkLoadedIndex(resolveLoadedIndex(PARSED, NEGATIVE_ACTION_ID, FIRST_MENU_POSITION, LOADED_UNIT_COUNT),
                         FIRST_MENU_POSITION, "negative action id");
        checkLoadedIndex(resolveLoadedIndex(PARSED, FIRST_LOADED_UNIT, SECOND_MENU_POSITION, LOADED_UNIT_COUNT),
                         FIRST_LOADED_UNIT, "action id wins over the menu position");
    }
}

int main()
{
    testLaterSingleFieldCandidateStaysReachable();
    testLaterCaptureTargetStaysReachable();
    testDedupeKeysOnLoadedIndex();
    testRefuelCandidateIsSkipped();
    testNoReachableTileFallsBack();
    testUnselectableCandidateKeepsItsSlot();
    testEarlyCaptureTargetKeepsPriority();
    testEveryUnloadedCandidateFallsBack();
    testLoadedIndexResolution();
    return failureCount == 0 ? 0 : 1;
}
