#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// 一条事件。租户只拿来和本地占位比较。
struct Event {
    std::string tenant;
    std::string chat_type;
    bool mentioned{false};
    std::string message_type;
    std::string action;
    bool allowlist_empty{false};
    int in_flight{0};
    bool locked{false};
};

// 本地占位，不是真实企业号。
struct Config {
    std::string tenant;
};

struct Decision {
    bool allowed{false};
    const char* rule{""};
};

struct Rule {
    const char* name;
    const char* detail;
    bool (*holds)(const Event&, const Config&);
};

[[nodiscard]] std::optional<Config> load_config();
[[nodiscard]] std::optional<Event> parse_event(std::string_view text);
[[nodiscard]] Decision decide(const Event& event, const Config& config);
[[nodiscard]] std::span<const Rule> rules();

// 0 通过，1 拒绝，2 输入或用法不对。
[[nodiscard]] int run(int argc, char** argv, std::istream& in, std::ostream& out);
