add_library(supervise STATIC supervise.cpp)
target_include_directories(supervise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
robot_pm_set_warnings(supervise)
robot_pm_enable_sanitizers(supervise)

add_executable(robot-pm-supervise main.cpp)
target_link_libraries(robot-pm-supervise PRIVATE supervise)
robot_pm_set_warnings(robot-pm-supervise)
robot_pm_enable_sanitizers(robot-pm-supervise)
