foreach(requiredVar SCHEMA_EXE ACTION_CONTROL_RESULT)
    if (NOT DEFINED ${requiredVar})
        message(FATAL_ERROR
            "actioncontrol.cmake requires -D${requiredVar}")
    endif()
endforeach()

if (NOT EXISTS "${SCHEMA_EXE}")
    message(FATAL_ERROR "Missing schema executable: ${SCHEMA_EXE}")
endif()
if (NOT EXISTS "${ACTION_CONTROL_RESULT}")
    message(FATAL_ERROR
        "Missing action-control result: ${ACTION_CONTROL_RESULT}")
endif()

execute_process(
    COMMAND "${SCHEMA_EXE}" "${ACTION_CONTROL_RESULT}"
    RESULT_VARIABLE schemaResult
    OUTPUT_VARIABLE schemaOutput
    ERROR_VARIABLE schemaError
)
if (NOT schemaResult EQUAL 0)
    message(FATAL_ERROR
        "Action-control schema failed (${schemaResult})\n"
        "${schemaOutput}${schemaError}")
endif()

message(STATUS "Action-control result schema passed")
