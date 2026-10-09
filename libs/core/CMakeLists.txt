find_package(OpenSSL REQUIRED)

add_library(robot_pm_core
    src/error.cpp
    src/crypto.cpp
    src/frame.cpp
    src/durable.cpp
    src/decision_drive.cpp
    src/writer_lock.cpp
    src/http_exchange.cpp
    src/collection_card.cpp
    src/onboarding.cpp
    src/member_reply.cpp
    src/feishu_access.cpp
    src/audit.cpp
    src/working_memory.cpp
    src/project.cpp
    src/inbox.cpp
    src/plan_expire.cpp
    src/status_update.cpp
    src/meeting_plan.cpp
    src/reserve.cpp
    src/app.cpp
    src/decide.cpp
    src/act.cpp
    src/arrange.cpp
    src/service.cpp
    src/child_boot.cpp)
target_include_directories(robot_pm_core PUBLIC include)
target_link_libraries(robot_pm_core PUBLIC nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
robot_pm_set_warnings(robot_pm_core)
robot_pm_enable_sanitizers(robot_pm_core)
