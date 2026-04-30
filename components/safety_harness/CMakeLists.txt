idf_component_register(
    SRCS "safety_harness.c"
    INCLUDE_DIRS "include"
)

execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/tools/check_safety_rules.py" "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE safety_check_result
    OUTPUT_VARIABLE safety_check_output
    ERROR_VARIABLE safety_check_error
)

if(NOT safety_check_result EQUAL 0)
    message(FATAL_ERROR
        "Safety harness check failed\n${safety_check_output}${safety_check_error}"
    )
endif()
