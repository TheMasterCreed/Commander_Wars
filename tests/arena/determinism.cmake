foreach(requiredVar ARENA_EXE ARENA_WORKING_DIRECTORY ARENA_RUN_DIR_A
                    ARENA_RUN_DIR_B ARENA_RESULT_FILE_NAME)
    if (NOT DEFINED ${requiredVar})
        message(FATAL_ERROR "determinism.cmake requires -D${requiredVar}")
    endif()
endforeach()

foreach(runDir "${ARENA_RUN_DIR_A}" "${ARENA_RUN_DIR_B}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DARENA_EXE=${ARENA_EXE}
            -DARENA_WORKING_DIRECTORY=${ARENA_WORKING_DIRECTORY}
            -DARENA_RUN_DIR=${runDir}
            -DARENA_RESULT_FILE_NAME=${ARENA_RESULT_FILE_NAME}
            -DARENA_EXPECTED_STOP_DAY=${ARENA_EXPECTED_STOP_DAY}
            -DARENA_EXPECTED_STOP_PLAYER=${ARENA_EXPECTED_STOP_PLAYER}
            -P "${CMAKE_CURRENT_LIST_DIR}/arenarun.cmake"
        RESULT_VARIABLE runResult
    )
    if (NOT runResult EQUAL 0)
        message(FATAL_ERROR "arena match in ${runDir} failed")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${ARENA_RUN_DIR_A}/${ARENA_RESULT_FILE_NAME}"
        "${ARENA_RUN_DIR_B}/${ARENA_RESULT_FILE_NAME}"
    RESULT_VARIABLE compareResult
)
if (NOT compareResult EQUAL 0)
    message(FATAL_ERROR "arena match results differ")
endif()
