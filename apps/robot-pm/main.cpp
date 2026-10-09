#include "listen.hpp"
#include "live.hpp"

#include "robot_pm/service.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

class SystemClock final : public robot_pm::Clock {
public:
    std::chrono::system_clock::time_point now() const override { return std::chrono::system_clock::now(); }
};

void on_signal(int) { robot_pm::note_process_stop(); }

[[nodiscard]] std::map<std::string, std::string> read_env() {
    std::map<std::string, std::string> env;
    static constexpr const char* kKeys[] = {
            "FEISHU_APP_ID",           "FEISHU_APP_SECRET",
            "FEISHU_ENCRYPT_KEY",      "FEISHU_VERIFICATION_TOKEN",
            "FEISHU_BITABLE_APP_TOKEN","FEISHU_BITABLE_TABLE_ID",
            "FEISHU_BASE_URL",         "FEISHU_BOT_OPEN_ID",
            "FEISHU_GROUP_ID",         "FEISHU_CALENDAR_ID",
            "ROBOT_PM_DATA_ROOT",      "ROBOT_PM_TIMEZONE",
            "ROBOT_PM_PORT",           "ROBOT_PM_MODEL_URL"};
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

[[nodiscard]] std::string env_value(const std::map<std::string, std::string>& env, const char* key) {
    const auto found = env.find(key);
    if (found == env.end()) {
        return {};
    }
    return found->second;
}

[[nodiscard]] int prepare_data_root(const std::filesystem::path& root) {
    const pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "探针没有跑起来\n";
        return 1;
    }
    if (child == 0) {
        const std::string path = root.string();
        ::execlp("python3", "python3", "-c",
                 "import sys\n"
                 "from robot_pm.deploy import prepare\n"
                 "prepare(sys.argv[1]).release()\n",
                 path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        std::cerr << "探针没有跑起来\n";
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "探针没有跑起来\n";
        return 1;
    }
    return 0;
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
    if (robot_pm::missing_runtime_settings(env, std::cout) != 0) {
        return 1;
    }

    const std::string data_text = env_value(env, "ROBOT_PM_DATA_ROOT");
    const std::filesystem::path data_root = data_text.empty() ? std::filesystem::path("var/robot_pm")
                                                              : std::filesystem::path(data_text);
    if (prepare_data_root(data_root) != 0) {
        return 1;
    }

    robot_pm::HttpEventPort events(port);
    if (!events.ok()) {
        std::cout << events.failure() << '\n';
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string base = env_value(env, "FEISHU_BASE_URL").empty() ? std::string("https://open.feishu.cn")
                                                                        : env_value(env, "FEISHU_BASE_URL");
    SystemClock clock;
    robot_pm::LiveModel model(env_value(env, "ROBOT_PM_MODEL_URL"));
    robot_pm::LiveFeishu feishu(env_value(env, "FEISHU_APP_ID"), env_value(env, "FEISHU_APP_SECRET"), base);
    robot_pm::LiveBitable bitable(feishu, env_value(env, "FEISHU_BITABLE_APP_TOKEN"),
                                  env_value(env, "FEISHU_BITABLE_TABLE_ID"));
    robot_pm::LiveProcess process;
    robot_pm::ServeDeps deps{clock, events, feishu, model, bitable, process, nullptr};
    robot_pm::ServicePaths paths;
    paths.data_root = data_root;
    paths.repo_root = std::filesystem::current_path();
    return robot_pm::serve(env, deps, paths, {}, std::cout);
}
