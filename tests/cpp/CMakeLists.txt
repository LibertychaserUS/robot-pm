add_executable(robot_pm_unit_tests
    main.cpp
    onboarding_test.cpp
    member_reply_test.cpp
    feishu_access_test.cpp
    working_memory_test.cpp)
target_link_libraries(robot_pm_unit_tests PRIVATE robot_pm_core doctest::doctest)
target_compile_definitions(robot_pm_unit_tests PRIVATE
    ROBOT_PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
robot_pm_set_warnings(robot_pm_unit_tests)
robot_pm_enable_sanitizers(robot_pm_unit_tests)

add_test(NAME robot_pm_unit COMMAND robot_pm_unit_tests)
set_tests_properties(robot_pm_unit PROPERTIES LABELS "unit;regression")
