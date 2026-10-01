add_library(robot_pm_core
    src/error.cpp
    src/collection_card.cpp
    src/onboarding.cpp
    src/member_reply.cpp
    src/feishu_access.cpp)
target_include_directories(robot_pm_core PUBLIC include)
target_link_libraries(robot_pm_core PUBLIC nlohmann_json::nlohmann_json)
robot_pm_set_warnings(robot_pm_core)
robot_pm_enable_sanitizers(robot_pm_core)
