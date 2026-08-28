foreach(requiredVar ARENA_EXE ARENA_WORKING_DIRECTORY ARENA_RUN_DIR ARENA_RESULT_FILE_NAME)
    if (NOT DEFINED ${requiredVar})
        message(FATAL_ERROR "arenarun.cmake requires -D${requiredVar}")
    endif()
endforeach()

set(resultFile "${ARENA_RUN_DIR}/${ARENA_RESULT_FILE_NAME}")
file(REMOVE "${resultFile}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "COW_AI_ARENA_TEST=1"
        "${ARENA_EXE}"
        --noUi
        --noAudio
        --spawnAiProcess 0
        --mods=
        --userPath "${ARENA_RUN_DIR}/"
    WORKING_DIRECTORY "${ARENA_WORKING_DIRECTORY}"
    RESULT_VARIABLE exitCode
)
if (NOT exitCode EQUAL 0 OR NOT EXISTS "${resultFile}")
    message(FATAL_ERROR "arena match exited ${exitCode} or wrote no result")
endif()

file(READ "${resultFile}" result)
string(JSON version GET "${result}" version)
string(JSON passed GET "${result}" pass)
string(JSON watchdog GET "${result}" watchdog)
string(JSON resultExitCode GET "${result}" exitCode)
string(JSON actionCount GET "${result}" actionCount)
string(JSON finalStateHash GET "${result}" finalStateHash)
string(JSON mapSha256 GET "${result}" mapSha256)
if (NOT version EQUAL 2 OR NOT passed OR watchdog OR
    NOT resultExitCode EQUAL 0 OR actionCount LESS 1 OR
    finalStateHash STREQUAL "" OR mapSha256 STREQUAL "")
    message(FATAL_ERROR "arena result failed its terminal contract")
endif()

foreach(field seedTrace actionIds actionTargets preActionStateHashes
              actionPayloadSha256 actionPayloadBase64)
    string(JSON fieldCount LENGTH "${result}" ${field})
    if (NOT fieldCount EQUAL actionCount)
        message(FATAL_ERROR "${field} has ${fieldCount} entries for ${actionCount} actions")
    endif()
endforeach()

if (ARENA_EXPECTED_STOP_DAY STREQUAL "null")
    string(JSON stopDayType TYPE "${result}" stopAfterDay)
    string(JSON stopPlayerType TYPE "${result}" stopAfterPlayer)
    if (NOT stopDayType STREQUAL "NULL" OR NOT stopPlayerType STREQUAL "NULL")
        message(FATAL_ERROR "arena result unexpectedly reports a stop boundary")
    endif()
else()
    string(JSON stopDay GET "${result}" stopAfterDay)
    string(JSON stopPlayer GET "${result}" stopAfterPlayer)
    if (NOT stopDay EQUAL ARENA_EXPECTED_STOP_DAY OR
        NOT stopPlayer EQUAL ARENA_EXPECTED_STOP_PLAYER)
        message(FATAL_ERROR "arena result reports the wrong stop boundary")
    endif()
endif()
