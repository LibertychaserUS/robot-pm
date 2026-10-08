#include "listen.hpp"

#include "robot_pm/service.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

class SystemClock final : public robot_pm::Clock {
public:
    std::chrono::system_clock::time_point now() const override { return std::chrono::system_clock::now(); }
};

class QuietModel final : public robot_pm::Model {
public:
    std::expected<std::string, robot_pm::Error> complete(std::string_view, std::string_view, std::string_view) override {
        return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "这一步没接上"});
    }
};

class QuietFeishu final : public robot_pm::FeishuPort {
public:
    std::expected<nlohmann::json, robot_pm::Error> send(const nlohmann::json&) override {
        return nlohmann::json{{"message_id", "om_local"}};
    }

    std::expected<std::string, robot_pm::Error> primary_calendar_id() override {
        return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kConfigMissing, "没有主日历"});
    }

    std::expected<nlohmann::json, robot_pm::Error> create_calendar_event(const nlohmann::json&) override {
        return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kConfigMissing, "没有主日历"});
    }

    std::expected<void, robot_pm::Error> delete_calendar_event(std::string_view, std::string_view) override {
        return {};
    }
};

class QuietBitable final : public robot_pm::BitablePort {
public:
    std::expected<nlohmann::json, robot_pm::Error> list_fields(std::string_view) override {
        return nlohmann::json::array();
    }

    std::expected<void, robot_pm::Error> create_field(std::string_view, const nlohmann::json&) override { return {}; }

    std::expected<nlohmann::json, robot_pm::Error> list_records(std::string_view) override {
        return nlohmann::json::array();
    }

    std::expected<nlohmann::json, robot_pm::Error> upsert(std::string_view, const nlohmann::json& incoming) override {
        return nlohmann::json{{"written", incoming.size()}};
    }

    std::expected<void, robot_pm::Error> undo(const nlohmann::json&) override { return {}; }
};

class QuietProcess final : public robot_pm::ProcessRunner {
public:
    std::expected<robot_pm::ProcessResult, robot_pm::Error> run(const robot_pm::ProcessRequest&) override {
        robot_pm::ProcessResult result;
        result.exit_code = 0;
        result.stdout_text = "{}";
        return result;
    }
};

void on_signal(int) { robot_pm::note_process_stop(); }

[[nodiscard]] std::map<std::string, std::string> read_env() {
    std::map<std::string, std::string> env;
    static constexpr const char* kKeys[] = {
            "FEISHU_APP_ID",       "FEISHU_APP_SECRET",   "FEISHU_ENCRYPT_KEY", "FEISHU_VERIFICATION_TOKEN",
            "FEISHU_BASE_URL",     "FEISHU_BOT_OPEN_ID",  "FEISHU_GROUP_ID",    "FEISHU_CALENDAR_ID",
            "ROBOT_PM_DATA_ROOT",  "ROBOT_PM_TIMEZONE",   "ROBOT_PM_PORT"};
    for (const char* key : kKeys) {
        if (const char* value = std::getenv(key)) {
            env.emplace(key, value);
        }
    }
    return env;
}

[[nodiscard]] bool parse_port(std::string_view text, std::uint16_t& port) {
    if (text.empty()) {
        return false;
    }
    unsigned value = 0;
    const char* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > 65535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace

int main() {
    const std::map<std::string, std::string> env = read_env();
    std::uint16_t port = 8080;
    const auto configured = env.find("ROBOT_PM_PORT");
    if (configured != env.end() && !configured->second.empty() && !parse_port(configured->second, port)) {
        std::cout << "端口不对\n";
        return 1;
    }

    robot_pm::HttpEventPort events(port);
    if (!events.ok()) {
        std::cout << events.failure() << '\n';
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SystemClock clock;
    QuietModel model;
    QuietFeishu feishu;
    QuietBitable bitable;
    QuietProcess process;
    robot_pm::ServeDeps deps{clock, events, feishu, model, bitable, process, nullptr};
    robot_pm::ServicePaths paths;
    const auto data_root = env.find("ROBOT_PM_DATA_ROOT");
    if (data_root != env.end() && !data_root->second.empty()) {
        paths.data_root = data_root->second;
    }
    paths.repo_root = std::filesystem::current_path();
    return robot_pm::serve(env, deps, paths, {}, std::cout);
}
