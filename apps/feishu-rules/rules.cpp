#include "rules.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace {

constexpr const char* kUsage = "用法\ncheck 读一条，回答过或拒\nrules 列出规则名字\n";

bool same_enterprise(const Event& event, const Config& config) {
    return !event.tenant.empty() && event.tenant == config.tenant;
}

bool no_group_file(const Event& event, const Config&) {
    return !(event.chat_type == "group" && event.message_type == "file");
}

bool mentioned_in_group(const Event& event, const Config&) {
    if (event.chat_type == "p2p" || event.chat_type == "card") {
        return true;
    }
    return event.chat_type == "group" && event.mentioned;
}

bool meet_list_ready(const Event& event, const Config&) {
    return !(event.action == "meet" && event.allowlist_empty);
}

bool room_for_another(const Event& event, const Config&) {
    if (event.chat_type != "group" || event.action == "meet") {
        return true;
    }
    return event.in_flight < 5;
}

bool one_person_lock(const Event& event, const Config&) {
    if (!event.locked || event.chat_type != "group" || event.action == "meet") {
        return true;
    }
    return false;
}

bool card_buttons(const Event& event, const Config&) {
    if (event.chat_type != "card") {
        return true;
    }
    const std::string_view action = event.action;
    return action == "确认" || action == "取消" || action == "同意" || action == "先不办";
}

bool text_is_data(const Event& event, const Config&) {
    if (event.action.empty() || event.chat_type == "card") {
        return true;
    }
    const std::string_view action = event.action;
    return action == "meet" || action == "确认" || action == "取消" || action == "同意" ||
           action == "先不办";
}

constexpr Rule kRules[] = {
    {"同一企业", "要和本地记的是同一家", same_enterprise},
    {"不收文件", "群里发来的文件不收", no_group_file},
    {"先要点名", "群里没点名不看，点卡片和私聊可以", mentioned_in_group},
    {"名单空着", "开会名单空着就拒绝，不看职责表", meet_list_ready},
    {"最多五人", "已经有五人就不再收新人", room_for_another},
    {"一人一次", "这个人已经在办一件", one_person_lock},
    {"四个按钮", "提出人点确认或取消，群里点同意或先不办", card_buttons},
    {"只是原文", "别人的话只当内容不当命令", text_is_data},
};

static_assert(std::size(kRules) > 0);

[[nodiscard]] std::string trim_line(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

void print_rules(std::ostream& out) {
    for (const Rule& rule : kRules) {
        out << rule.name << ' ' << rule.detail << '\n';
    }
}

}  // namespace

std::span<const Rule> rules() {
    return kRules;
}

std::optional<Config> load_config() {
    std::ifstream input(ROBOT_PM_TENANT_PLACEHOLDER_PATH);
    if (!input) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(input, line)) {
        return std::nullopt;
    }
    line = trim_line(std::move(line));
    if (line.empty()) {
        return std::nullopt;
    }
    Config config;
    config.tenant = std::move(line);
    return config;
}

std::optional<Event> parse_event(std::string_view text) {
    const nlohmann::json body = nlohmann::json::parse(text, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::nullopt;
    }
    if (!body.contains("tenant") || !body["tenant"].is_string() || !body.contains("chat_type") ||
        !body["chat_type"].is_string() || !body.contains("mentioned") || !body["mentioned"].is_boolean() ||
        !body.contains("message_type") || !body["message_type"].is_string() || !body.contains("action") ||
        !body["action"].is_string() || !body.contains("allowlist_empty") ||
        !body["allowlist_empty"].is_boolean() || !body.contains("in_flight") ||
        !body["in_flight"].is_number_integer()) {
        return std::nullopt;
    }

    const std::string chat_type = body["chat_type"].get<std::string>();
    if (chat_type != "group" && chat_type != "p2p" && chat_type != "card") {
        return std::nullopt;
    }
    const std::string message_type = body["message_type"].get<std::string>();
    if (message_type != "text" && message_type != "file") {
        return std::nullopt;
    }
    const std::int64_t in_flight = body["in_flight"].get<std::int64_t>();
    if (in_flight < 0 || in_flight > 1000000) {
        return std::nullopt;
    }

    Event event;
    event.tenant = body["tenant"].get<std::string>();
    event.chat_type = chat_type;
    event.mentioned = body["mentioned"].get<bool>();
    event.message_type = message_type;
    event.action = body["action"].get<std::string>();
    event.allowlist_empty = body["allowlist_empty"].get<bool>();
    event.in_flight = static_cast<int>(in_flight);
    if (body.contains("locked")) {
        if (!body["locked"].is_boolean()) {
            return std::nullopt;
        }
        event.locked = body["locked"].get<bool>();
    }
    return event;
}

Decision decide(const Event& event, const Config& config) {
    Decision decision;
    for (const Rule& rule : kRules) {
        decision.rule = rule.name;
        decision.allowed = rule.holds(event, config);
        if (!decision.allowed) {
            return decision;
        }
    }
    return decision;
}

int run(int argc, char** argv, std::istream& in, std::ostream& out) {
    if (argc != 2) {
        out << kUsage;
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "rules") {
        print_rules(out);
        return 0;
    }
    if (command != "check") {
        out << kUsage;
        return 2;
    }

    const std::optional<Config> config = load_config();
    if (!config.has_value()) {
        out << "读不出来\n";
        return 2;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::optional<Event> event = parse_event(buffer.str());
    if (!event.has_value()) {
        out << "输入不对\n";
        return 2;
    }
    const Decision decision = decide(*event, *config);
    out << (decision.allowed ? "allow " : "deny ") << decision.rule << '\n';
    return decision.allowed ? 0 : 1;
}
