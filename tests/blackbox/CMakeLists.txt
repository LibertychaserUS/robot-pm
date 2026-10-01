include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

add_executable(robot_pm_blackbox
    main.cpp
    cases_manifest.cpp
    cases_pipeline.cpp
    cases_delivery.cpp
    cases_roles_templates.cpp
    cases_import_check.cpp
    cases_prompts.cpp
    cases_status_reserve.cpp
    cases_memory.cpp
    cases_logic.cpp
    cases_projection_inbox.cpp)

target_include_directories(robot_pm_blackbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(robot_pm_blackbox PRIVATE robot_pm_core doctest::doctest)
robot_pm_set_warnings(robot_pm_blackbox)
robot_pm_enable_sanitizers(robot_pm_blackbox)

# Discovery only lists unskipped cases. Every case is skipped while robot_pm::App is missing,
# so the executable is also one regression test and must exit 0.
doctest_discover_tests(robot_pm_blackbox
    PROPERTIES LABELS "blackbox;regression")
add_test(NAME robot_pm_blackbox COMMAND robot_pm_blackbox)
set_tests_properties(robot_pm_blackbox PROPERTIES LABELS "blackbox;regression")
