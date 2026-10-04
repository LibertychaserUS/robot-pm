#include "robot_pm/app.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/collection_card.hpp"
#include "robot_pm/working_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace robot_pm {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using clock_tp = std::chrono::system_clock::time_point;

constexpr std::string_view kMainTable = "工作项";
constexpr std::string_view kRoleTable = "职责";
constexpr std::size_t kPromptCap = 4096;
constexpr int kOpenLimit = 5;
constexpr std::size_t kQueueLimit = 32;

struct Session {
    Config& config;
    Ports& ports;
    std::vector<std::string>& command_ids;
    std::set<std::string>& awaiting_role;
};

struct FieldSpec {
    int type;
    const char* ui;
};

struct LocatedPlan {
    fs::path directory;
    std::string owner;
    json context;
};

[[nodiscard]] Error fail(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

[[nodiscard]] std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string truncate_text(std::string text, std::size_t cap) {
    if (text.size() > cap) {
        text.resize(cap);
    }
    return text;
}

[[nodiscard]] std::string trim_copy(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

[[nodiscard]] std::string lower_copy(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

[[nodiscard]] std::string beijing_timestamp(clock_tp time_point) {
    using namespace std::chrono;
    const auto shifted = floor<seconds>(time_point + hours{8});
    const auto day_point = floor<days>(shifted);
    const year_month_day ymd{day_point};
    const hh_mm_ss hms{shifted - day_point};
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                       static_cast<int>(ymd.year()),
                       static_cast<int>(static_cast<unsigned>(ymd.month())),
                       static_cast<int>(static_cast<unsigned>(ymd.day())),
                       static_cast<int>(hms.hours().count()),
                       static_cast<int>(hms.minutes().count()),
                       static_cast<int>(hms.seconds().count()));
}

[[nodiscard]] std::int64_t unix_seconds(clock_tp time_point) {
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

[[nodiscard]] bool safe_relative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find("..") != std::string_view::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] bool claims_human(std::string_view text) {
    if (text.find("真人") != std::string_view::npos) {
        return true;
    }
    return text.find("我是") != std::string_view::npos && text.find("我是 robot PM") == std::string_view::npos;
}

[[nodiscard]] bool numbers_match(std::string_view text, int expected) {
    const std::string needle = std::to_string(expected);
    if (text.find(needle) == std::string_view::npos) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size()) {
        if (std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        if (text.substr(start, index - start) != needle) {
            return false;
        }
    }
    return true;
}

const FieldSpec kText{1, "Text"};
const FieldSpec kNumber{2, "Number"};
const FieldSpec kSelect{3, "SingleSelect"};
const FieldSpec kDate{5, "DateTime"};
const FieldSpec kCheck{7, "Checkbox"};
const FieldSpec kUser{11, "User"};
const FieldSpec kLink{18, "SingleLink"};

[[nodiscard]] const FieldSpec* field_spec(std::string_view name) {
    if (name == "业务id" || name == "标题" || name == "职责" || name == "待办" || name == "群id") {
        return &kText;
    }
    if (name == "层级" || name == "类型" || name == "状态" || name == "判断" || name == "决定") {
        return &kSelect;
    }
    if (name == "父记录" || name == "前置") {
        return &kLink;
    }
    if (name == "开始" || name == "结束" || name == "会议时间") {
        return &kDate;
    }
    if (name == "进度") {
        return &kNumber;
    }
    if (name == "推荐") {
        return &kCheck;
    }
    if (name == "人员") {
        return &kUser;
    }
    return nullptr;
}

[[nodiscard]] json field_body(std::string_view name, const FieldSpec& spec) {
    return json{{"field_name", name}, {"type", spec.type}, {"ui_type", spec.ui}};
}

[[nodiscard]] std::vector<std::string> mandatory_fields(std::string_view template_name) {
    std::vector<std::string> fields{"业务id", "标题", "层级", "父记录", "类型", "状态",
                                    "开始",   "结束", "前置", "进度"};
    if (template_name == "盯人待办") {
        fields.emplace_back("判断");
    }
    if (template_name == "会议决策") {
        fields.emplace_back("推荐");
        fields.emplace_back("会议时间");
        fields.emplace_back("决定");
        fields.emplace_back("待办");
    }
    return fields;
}

[[nodiscard]] bool is_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(text[index])) == 0) {
            return false;
        }
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

[[nodiscard]] bool allowed_key(std::string_view key, const std::set<std::string>& allowed) {
    return allowed.contains(std::string(key));
}

class DuplicateKeySax : public nlohmann::json_sax<json> {
public:
    bool duplicate = false;

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }
    bool start_object(std::size_t) override {
        keys_.emplace_back();
        return true;
    }
    bool key(string_t& value) override {
        if (keys_.empty() || !keys_.back().insert(value).second) {
            duplicate = true;
            return false;
        }
        return true;
    }
    bool end_object() override {
        if (!keys_.empty()) {
            keys_.pop_back();
        }
        return true;
    }
    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    std::vector<std::set<std::string>> keys_;
};

[[nodiscard]] std::expected<json, Error> parse_json_text(std::string_view text, bool reject_duplicates) {
    if (reject_duplicates) {
        DuplicateKeySax sax;
        const bool parsed = json::sax_parse(text, &sax);
        if (sax.duplicate || !parsed) {
            return std::unexpected(fail(sax.duplicate ? ErrorCode::kEditRejected : ErrorCode::kEditRejected,
                                        "JSON 不合格"));
        }
    }
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "JSON 不合格"));
    }
    return parsed;
}

[[nodiscard]] std::expected<std::vector<json>, Error> project_rows(const json& manifest) {
    if (!manifest.is_object()) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "清单必须是对象"));
    }
    for (auto it = manifest.begin(); it != manifest.end(); ++it) {
        if (it.key() != "schema_version" && it.key() != "documents") {
            return std::unexpected(fail(ErrorCode::kEditRejected, "清单顶层有多余字段"));
        }
    }
    if (!manifest.contains("schema_version") || !manifest.at("schema_version").is_number_integer() ||
        manifest.at("schema_version").get<int>() != 1) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "schema_version 只能是 1"));
    }
    if (!manifest.contains("documents") || !manifest.at("documents").is_array()) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "documents 必须是数组"));
    }

    static const std::set<std::string> kDocument{"id", "type", "title", "sections", "items", "parent_id", "rejected"};
    static const std::set<std::string> kSection{"id", "title", "items"};
    static const std::set<std::string> kItem{"id",     "title", "kind",   "status", "start",        "end",
                                             "predecessors", "owner_role", "meet", "source_quote", "node"};

    const json& documents = manifest.at("documents");
    std::set<std::string> document_ids;
    for (const json& document : documents) {
        if (!document.is_object() || !document.contains("id") || !document.at("id").is_string() ||
            document.at("id").get_ref<const std::string&>().empty()) {
            return std::unexpected(fail(ErrorCode::kUnknownField, "文档缺少 id"));
        }
        if (!document_ids.insert(document.at("id").get_ref<const std::string&>()).second) {
            return std::unexpected(fail(ErrorCode::kEditRejected, "清单里的 id 重复"));
        }
    }

    std::vector<json> rows;
    std::set<std::string> ids = document_ids;
    for (const json& document : documents) {
        if (!document.is_object()) {
            return std::unexpected(fail(ErrorCode::kUnsupportedType, "文档必须是对象"));
        }
        for (auto it = document.begin(); it != document.end(); ++it) {
            if (!allowed_key(it.key(), kDocument)) {
                return std::unexpected(fail(ErrorCode::kEditRejected, "文档有多余字段"));
            }
        }
        if (!document.contains("title") || !document.at("title").is_string()) {
            return std::unexpected(document.contains("title")
                                       ? fail(ErrorCode::kUnsupportedType, "文档标题类型不对")
                                       : fail(ErrorCode::kUnknownField, "文档缺少标题"));
        }
        if (!document.contains("type") || !document.at("type").is_string() ||
            document.at("type").get_ref<const std::string&>() != "prd") {
            return std::unexpected(fail(ErrorCode::kUnsupportedType, "文档类型只能是 prd"));
        }
        json parent = json::array();
        if (document.contains("parent_id")) {
            if (!document.at("parent_id").is_string()) {
                return std::unexpected(fail(ErrorCode::kUnsupportedType, "parent_id 类型不对"));
            }
            const std::string& parent_id = document.at("parent_id").get_ref<const std::string&>();
            if (!document_ids.contains(parent_id)) {
                return std::unexpected(fail(ErrorCode::kUnknownField, "父文档不存在"));
            }
            parent.push_back(parent_id);
        }
        const std::string& document_id = document.at("id").get_ref<const std::string&>();
        rows.push_back(json{{"业务id", document_id},
                            {"标题", document.at("title")},
                            {"层级", "document"},
                            {"父记录", parent}});

        std::map<std::string, std::vector<std::string>> predecessors;
        std::set<std::string> item_ids;
        const auto consume = [&](const json& items, const std::string& parent_id) -> std::expected<void, Error> {
            if (!items.is_array()) {
                return std::unexpected(fail(ErrorCode::kUnsupportedType, "工作项必须是数组"));
            }
            for (const json& item : items) {
                if (!item.is_object()) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "工作项必须是对象"));
                }
                for (auto it = item.begin(); it != item.end(); ++it) {
                    if (!allowed_key(it.key(), kItem)) {
                        return std::unexpected(fail(ErrorCode::kEditRejected, "工作项有多余字段"));
                    }
                }
                const auto require_string = [&](const char* key) -> std::expected<const std::string*, Error> {
                    if (!item.contains(key)) {
                        return std::unexpected(fail(ErrorCode::kUnknownField, std::string(key) + " 缺少"));
                    }
                    if (!item.at(key).is_string()) {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, std::string(key) + " 类型不对"));
                    }
                    return &item.at(key).get_ref<const std::string&>();
                };
                const auto id = require_string("id");
                if (!id) {
                    return std::unexpected(id.error());
                }
                const auto title = require_string("title");
                if (!title) {
                    return std::unexpected(title.error());
                }
                const auto kind = require_string("kind");
                if (!kind) {
                    return std::unexpected(kind.error());
                }
                const auto status = require_string("status");
                if (!status) {
                    return std::unexpected(status.error());
                }
                const auto start = require_string("start");
                if (!start) {
                    return std::unexpected(start.error());
                }
                const auto end = require_string("end");
                if (!end) {
                    return std::unexpected(end.error());
                }
                const auto owner = require_string("owner_role");
                if (!owner) {
                    return std::unexpected(owner.error());
                }
                if (!item.contains("predecessors")) {
                    return std::unexpected(fail(ErrorCode::kUnknownField, "缺少前置"));
                }
                if (!item.at("predecessors").is_array()) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "前置类型不对"));
                }
                if ((**kind != "work" && **kind != "milestone") ||
                    (**status != "todo" && **status != "doing" && **status != "done" && **status != "blocked")) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "类型或状态不在规格里"));
                }
                if (!is_date(**start) || !is_date(**end)) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "日期必须是 YYYY-MM-DD"));
                }
                if (**end < **start) {
                    return std::unexpected(fail(ErrorCode::kEditRejected, "结束早于开始"));
                }
                if (item.contains("meet")) {
                    if (!item.at("meet").is_string()) {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, "meet 类型不对"));
                    }
                    const std::string& meet = item.at("meet").get_ref<const std::string&>();
                    if (meet != "at_start" && meet != "at_end" && meet != "when_blocked") {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, "meet 不在规格里"));
                    }
                }
                if (item.contains("node")) {
                    if (!item.at("node").is_string()) {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, "node 类型不对"));
                    }
                    const std::string& node = item.at("node").get_ref<const std::string&>();
                    if (node != "flexible" && node != "deadline" && node != "release") {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, "node 不在规格里"));
                    }
                }
                if (item.contains("source_quote") && !item.at("source_quote").is_string()) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "source_quote 类型不对"));
                }
                if (!ids.insert(**id).second) {
                    return std::unexpected(fail(ErrorCode::kEditRejected, "清单里的 id 重复"));
                }
                std::vector<std::string> previous;
                for (const json& predecessor : item.at("predecessors")) {
                    if (!predecessor.is_string() || predecessor.get_ref<const std::string&>().empty()) {
                        return std::unexpected(fail(ErrorCode::kUnsupportedType, "前置类型不对"));
                    }
                    if (predecessor.get_ref<const std::string&>() == **id) {
                        return std::unexpected(fail(ErrorCode::kEditRejected, "工作项不能依赖自己"));
                    }
                    previous.push_back(predecessor.get_ref<const std::string&>());
                }
                predecessors[**id] = previous;
                item_ids.insert(**id);
                json row = {{"业务id", **id},
                            {"标题", **title},
                            {"层级", "item"},
                            {"父记录", json::array({parent_id})},
                            {"类型", **kind},
                            {"开始", **start},
                            {"结束", **end},
                            {"前置", item.at("predecessors")},
                            {"职责", **owner}};
                if (item.contains("node")) {
                    row["node"] = item.at("node");
                }
                rows.push_back(std::move(row));
            }
            return {};
        };

        if (document.contains("sections")) {
            if (!document.at("sections").is_array()) {
                return std::unexpected(fail(ErrorCode::kUnsupportedType, "节必须是数组"));
            }
            for (const json& section : document.at("sections")) {
                if (!section.is_object()) {
                    return std::unexpected(fail(ErrorCode::kUnsupportedType, "节必须是对象"));
                }
                for (auto it = section.begin(); it != section.end(); ++it) {
                    if (!allowed_key(it.key(), kSection)) {
                        return std::unexpected(fail(ErrorCode::kEditRejected, "节有多余字段"));
                    }
                }
                if (!section.contains("id") || !section.at("id").is_string() ||
                    section.at("id").get_ref<const std::string&>().empty()) {
                    return std::unexpected(fail(ErrorCode::kUnknownField, "节缺少 id"));
                }
                if (!section.contains("title") || !section.at("title").is_string()) {
                    return std::unexpected(section.contains("title")
                                               ? fail(ErrorCode::kUnsupportedType, "节标题类型不对")
                                               : fail(ErrorCode::kUnknownField, "节缺少标题"));
                }
                if (!section.contains("items")) {
                    return std::unexpected(fail(ErrorCode::kUnknownField, "节缺少工作项"));
                }
                const std::string& section_id = section.at("id").get_ref<const std::string&>();
                if (!ids.insert(section_id).second) {
                    return std::unexpected(fail(ErrorCode::kEditRejected, "清单里的 id 重复"));
                }
                rows.push_back(json{{"业务id", section_id},
                                    {"标题", section.at("title")},
                                    {"层级", "section"},
                                    {"父记录", json::array({document_id})}});
                const std::expected<void, Error> consumed = consume(section.at("items"), section_id);
                if (!consumed) {
                    return std::unexpected(consumed.error());
                }
            }
        }
        if (document.contains("items")) {
            const std::expected<void, Error> consumed = consume(document.at("items"), document_id);
            if (!consumed) {
                return std::unexpected(consumed.error());
            }
        }
        for (const auto& entry : predecessors) {
            for (const std::string& previous : entry.second) {
                if (!item_ids.contains(previous)) {
                    return std::unexpected(fail(ErrorCode::kEditRejected, "前置不在这份文档里"));
                }
            }
        }
        std::map<std::string, int> color;
        const auto visit = [&](auto&& self, const std::string& id) -> bool {
            color[id] = 1;
            const auto found = predecessors.find(id);
            if (found != predecessors.end()) {
                for (const std::string& next : found->second) {
                    const int seen = color[next];
                    if (seen == 1 || (seen == 0 && self(self, next))) {
                        return true;
                    }
                }
            }
            color[id] = 2;
            return false;
        };
        for (const auto& entry : predecessors) {
            if (color[entry.first] == 0 && visit(visit, entry.first)) {
                return std::unexpected(fail(ErrorCode::kEditRejected, "前置形成了环"));
            }
        }
    }
    return rows;
}

[[nodiscard]] std::string system_prompt(const Config& config, std::string_view step) {
    const std::string rules = read_text(config.repo_root / "prompts" / (std::string(step) + ".md"));
    const std::string identity = read_text(config.repo_root / "prompts/identity.md");
    const std::string overview =
            truncate_text(read_text(config.data_root / "semantic/overview.md"), kPromptCap);
    const std::string index =
            truncate_text(read_text(config.data_root / "semantic/manifests/index.txt"), kPromptCap);
    std::string system = rules;
    if (!identity.empty()) {
        system.push_back('\n');
        system += identity;
    }
    if (!overview.empty()) {
        system.push_back('\n');
        system += overview;
    }
    if (!index.empty()) {
        system.push_back('\n');
        system += index;
    }
    return system;
}

[[nodiscard]] std::expected<json, Error> ask(Session& session, std::string_view step, std::string user) {
    const std::string system = system_prompt(session.config, step);
    const std::expected<std::string, Error> raw = session.ports.model.complete(step, system, user);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    const json parsed = json::parse(*raw, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "模型输出不是 JSON"));
    }
    return parsed;
}

[[nodiscard]] std::expected<void, Error> send_message(Session& session, const json& message) {
    const std::expected<json, Error> sent = session.ports.feishu.send(message);
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> check_bridge(Session& session) {
    const std::expected<ProcessResult, Error> ran = session.ports.process.run(ProcessRequest{});
    if (!ran || ran->exit_code != 0) {
        return std::unexpected(fail(ErrorCode::kBridgeFailed, "桥接没有完成"));
    }
    const json parsed = json::parse(ran->stdout_text, nullptr, false);
    if (parsed.is_discarded()) {
        return std::unexpected(fail(ErrorCode::kBridgeFailed, "桥接没有完成"));
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> append_audit(Session& session, json line) {
    if (!line.contains("time")) {
        line["time"] = beijing_timestamp(session.ports.clock.now());
    }
    return append_jsonl(session.config.data_root / "episodic/audit.jsonl", line);
}

[[nodiscard]] std::expected<void, Error> append_tombstone(Session& session,
                                                         std::string_view interaction_id,
                                                         std::string_view actor,
                                                         std::string_view outcome) {
    const json line = {{"interaction_id", interaction_id},
                       {"actor", actor},
                       {"op", "end"},
                       {"outcome", outcome},
                       {"time", beijing_timestamp(session.ports.clock.now())}};
    return append_jsonl(session.config.data_root / "episodic/events.jsonl", line);
}

[[nodiscard]] int count_open(const fs::path& root) {
    int count = 0;
    const fs::path working = root / "working";
    std::error_code error;
    if (!fs::is_directory(working, error)) {
        return 0;
    }
    for (fs::directory_iterator person(working, error); !error && person != fs::directory_iterator();
         person.increment(error)) {
        if (!person->is_directory()) {
            continue;
        }
        std::error_code child_error;
        for (fs::directory_iterator interaction(person->path(), child_error);
             !child_error && interaction != fs::directory_iterator();
             interaction.increment(child_error)) {
            if (interaction->is_directory()) {
                ++count;
            }
        }
    }
    return count;
}

[[nodiscard]] std::optional<fs::path> find_person_dir(const fs::path& root, std::string_view open_id) {
    const fs::path person = root / "working" / std::string(open_id);
    std::error_code error;
    if (!fs::is_directory(person, error)) {
        return std::nullopt;
    }
    for (fs::directory_iterator it(person, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        if (!it->is_directory()) {
            continue;
        }
        std::error_code file_error;
        if (fs::is_regular_file(it->path() / "context.json", file_error) && !file_error) {
            return it->path();
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<LocatedPlan> find_plan(const fs::path& root, std::string_view interaction_id) {
    const fs::path working = root / "working";
    std::error_code error;
    if (!fs::is_directory(working, error)) {
        return std::nullopt;
    }
    for (fs::directory_iterator person(working, error); !error && person != fs::directory_iterator();
         person.increment(error)) {
        if (!person->is_directory()) {
            continue;
        }
        const fs::path directory = person->path() / std::string(interaction_id);
        std::error_code found;
        if (!fs::is_directory(directory, found)) {
            continue;
        }
        const json context = json::parse(read_text(directory / "context.json"), nullptr, false);
        if (context.is_discarded() || !context.is_object()) {
            continue;
        }
        return LocatedPlan{directory, person->path().filename().string(), context};
    }
    return std::nullopt;
}

void remove_empty_parent(const fs::path& person) {
    std::error_code error;
    if (!fs::is_directory(person, error)) {
        return;
    }
    const bool empty = fs::directory_iterator(person, error) == fs::directory_iterator();
    if (!error && empty) {
        fs::remove(person, error);
    }
}

[[nodiscard]] std::expected<void, Error> begin_plan(Session& session,
                                                   std::string_view open_id,
                                                   std::string_view interaction_id,
                                                   json context) {
    context["created_unix"] = unix_seconds(session.ports.clock.now());
    const std::expected<fs::path, Error> begun =
            begin_interaction(session.config.data_root, open_id, interaction_id, context);
    if (!begun) {
        return std::unexpected(begun.error());
    }
    return {};
}

[[nodiscard]] std::expected<json, Error> finish_plan(Session& session,
                                                    const LocatedPlan& plan,
                                                    std::string_view outcome,
                                                    bool notify) {
    const std::string interaction_id = plan.directory.filename().string();
    const std::expected<void, Error> tombstone =
            append_tombstone(session, interaction_id, plan.owner, outcome);
    if (!tombstone) {
        return std::unexpected(tombstone.error());
    }
    std::error_code error;
    fs::remove_all(plan.directory, error);
    if (error) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "交互目录没有删掉"));
    }
    remove_empty_parent(plan.directory.parent_path());
    if (notify) {
        const std::expected<void, Error> sent = send_message(
                session,
                json{{"text", plan.owner + " 这次已取消"}, {"mentions", json::array({plan.owner})}});
        if (!sent) {
            return std::unexpected(sent.error());
        }
    }
    return json::object();
}

[[nodiscard]] std::expected<json, Error> cancel_plan(Session& session, const LocatedPlan& plan, bool notify) {
    const std::expected<void, Error> audited = append_audit(
            session,
            json{{"op", "cancelled"},
                 {"ok", true},
                 {"actor", plan.owner},
                 {"interaction_id", plan.directory.filename().string()}});
    if (!audited) {
        return std::unexpected(audited.error());
    }
    return finish_plan(session, plan, "cancelled", notify);
}

[[nodiscard]] std::expected<json, Error> list_table(Session& session, std::string_view table) {
    return session.ports.bitable.list_records(table);
}

[[nodiscard]] const json* find_record(const json& records, std::string_view id) {
    if (!records.is_array()) {
        return nullptr;
    }
    for (const json& row : records) {
        if (row.is_object() && row.value("业务id", "") == id) {
            return &row;
        }
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> roles_of(const json& role_rows, std::string_view open_id) {
    std::vector<std::string> roles;
    if (!role_rows.is_array()) {
        return roles;
    }
    for (const json& row : role_rows) {
        if (!row.is_object() || !row.contains("人员") || !row.at("人员").is_array()) {
            continue;
        }
        bool matched = false;
        for (const json& person : row.at("人员")) {
            if (person.is_object() && person.value("id", "") == open_id) {
                matched = true;
            } else if (person.is_string() && person.get_ref<const std::string&>() == open_id) {
                matched = true;
            }
        }
        if (matched && row.contains("职责") && row.at("职责").is_string()) {
            roles.push_back(row.at("职责").get_ref<const std::string&>());
        }
    }
    return roles;
}

[[nodiscard]] bool can_edit(const std::vector<std::string>& roles, std::string_view item_role) {
    return std::find(roles.begin(), roles.end(), std::string(item_role)) != roles.end() ||
           std::find(roles.begin(), roles.end(), "pm") != roles.end();
}

[[nodiscard]] json attendees_for(const json& role_rows, std::string_view item_role) {
    json attendees = json::array();
    if (!role_rows.is_array()) {
        return attendees;
    }
    for (const json& row : role_rows) {
        if (!row.is_object() || row.value("职责", "") != item_role || !row.contains("人员") ||
            !row.at("人员").is_array()) {
            continue;
        }
        for (const json& person : row.at("人员")) {
            if (person.is_object() && person.contains("id") && person.at("id").is_string()) {
                attendees.push_back(json{{"id", person.at("id")}});
            }
        }
    }
    return attendees;
}

[[nodiscard]] json relevant_rows(const json& records, std::string_view text) {
    json rows = json::array();
    if (!records.is_array()) {
        return rows;
    }
    for (const json& row : records) {
        if (!row.is_object()) {
            continue;
        }
        const std::string id = row.value("业务id", "");
        const std::string title = row.value("标题", "");
        const bool id_hit = !id.empty() && text.find(id) != std::string_view::npos;
        const bool title_hit = !title.empty() && text.find(title) != std::string_view::npos;
        if (id_hit || title_hit) {
            rows.push_back(row);
        }
    }
    return rows;
}

[[nodiscard]] bool on_allowlist(const std::vector<std::string>& allowlist, std::string_view open_id) {
    return !allowlist.empty() &&
           std::find(allowlist.begin(), allowlist.end(), std::string(open_id)) != allowlist.end();
}

[[nodiscard]] std::expected<void, Error> commit_rows(Session& session, std::string_view table, const json& rows) {
    if (!rows.is_array()) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "行必须是数组"));
    }
    if (table == kRoleTable) {
        for (const json& row : rows) {
            if (!row.is_object() || !row.contains("群id") || !row.contains("人员") || !row.contains("职责") ||
                !row.at("群id").is_string() || row.at("群id").get_ref<const std::string&>().empty() ||
                !row.at("职责").is_string() || row.at("职责").get_ref<const std::string&>().empty()) {
                return std::unexpected(fail(ErrorCode::kEditRejected, "职责行不完整"));
            }
        }
        const std::expected<json, Error> written = session.ports.bitable.upsert(table, rows);
        if (!written) {
            const std::expected<void, Error> undone = session.ports.bitable.undo(json::object());
            if (!undone) {
                return std::unexpected(fail(ErrorCode::kBridgeFailed, "没有回到原状"));
            }
            return std::unexpected(written.error());
        }
        return {};
    }

    const std::expected<json, Error> listed = session.ports.bitable.list_fields(table);
    if (!listed || !listed->is_array()) {
        return std::unexpected(listed ? fail(ErrorCode::kBridgeFailed, "字段没有列出来") : listed.error());
    }
    std::map<std::string, json> fields;
    for (const json& field : *listed) {
        if (field.is_object() && field.contains("field_name") && field.at("field_name").is_string()) {
            fields.emplace(field.at("field_name").get_ref<const std::string&>(), field);
        }
    }
    for (const std::string& name : mandatory_fields(session.config.template_name)) {
        if (!fields.contains(name)) {
            return std::unexpected(fail(ErrorCode::kUnknownField, name + " 不在表里"));
        }
    }
    for (const json& row : rows) {
        if (!row.is_object()) {
            return std::unexpected(fail(ErrorCode::kUnsupportedType, "行必须是对象"));
        }
        for (auto it = row.begin(); it != row.end(); ++it) {
            const FieldSpec* spec = field_spec(it.key());
            const auto found = fields.find(it.key());
            if (spec == nullptr || found == fields.end()) {
                continue;
            }
            const json& field = found->second;
            if (!field.contains("type") || !field.at("type").is_number_integer() ||
                field.at("type").get<int>() != spec->type || field.value("ui_type", "") != spec->ui) {
                return std::unexpected(fail(ErrorCode::kUnsupportedType, it.key() + " 的字段类型不对"));
            }
        }
    }

    const std::expected<json, Error> existing = session.ports.bitable.list_records(table);
    if (!existing || !existing->is_array()) {
        return std::unexpected(existing ? fail(ErrorCode::kBridgeFailed, "记录没有列出来") : existing.error());
    }
    std::set<std::string> ids;
    for (const json& row : *existing) {
        if (row.is_object() && row.contains("业务id") && row.at("业务id").is_string()) {
            ids.insert(row.at("业务id").get_ref<const std::string&>());
        }
    }
    for (const json& row : rows) {
        if (row.contains("业务id") && row.at("业务id").is_string()) {
            ids.insert(row.at("业务id").get_ref<const std::string&>());
        }
    }
    std::set<std::string> recommended;
    for (const json& row : *existing) {
        if (row.is_object() && row.contains("推荐") && row.at("推荐").is_boolean() && row.at("推荐").get<bool>() &&
            row.contains("业务id") && row.at("业务id").is_string()) {
            recommended.insert(row.at("业务id").get_ref<const std::string&>());
        }
    }
    for (const json& row : rows) {
        if (row.contains("父记录")) {
            if (!row.at("父记录").is_array()) {
                return std::unexpected(fail(ErrorCode::kUnsupportedType, "父记录类型不对"));
            }
            for (const json& parent : row.at("父记录")) {
                if (!parent.is_string() || !ids.contains(parent.get_ref<const std::string&>())) {
                    return std::unexpected(fail(ErrorCode::kUnknownField, "父记录不存在"));
                }
            }
        }
        if (row.contains("推荐") && row.at("推荐").is_boolean() && row.at("推荐").get<bool>() &&
            row.contains("业务id") && row.at("业务id").is_string()) {
            recommended.insert(row.at("业务id").get_ref<const std::string&>());
        }
    }
    if (recommended.size() > 1) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "同时只能有一条推荐"));
    }

    bool created = false;
    std::set<std::string> created_names;
    for (const json& row : rows) {
        for (auto it = row.begin(); it != row.end(); ++it) {
            const FieldSpec* spec = field_spec(it.key());
            if (spec == nullptr || fields.contains(it.key()) || created_names.contains(it.key())) {
                continue;
            }
            const std::expected<void, Error> made =
                    session.ports.bitable.create_field(table, field_body(it.key(), *spec));
            if (!made) {
                return std::unexpected(made.error());
            }
            created_names.insert(it.key());
            created = true;
        }
    }
    if (created) {
        const std::expected<json, Error> again = session.ports.bitable.list_fields(table);
        if (!again) {
            return std::unexpected(again.error());
        }
    }
    const std::expected<json, Error> written = session.ports.bitable.upsert(table, rows);
    if (!written) {
        const std::expected<void, Error> undone = session.ports.bitable.undo(json::object());
        if (!undone) {
            return std::unexpected(fail(ErrorCode::kBridgeFailed, "没有回到原状"));
        }
        return std::unexpected(written.error());
    }
    return {};
}

[[nodiscard]] std::expected<json, Error> upsert_one(Session& session, std::string_view table, json row) {
    json rows = json::array();
    rows.push_back(std::move(row));
    const std::expected<json, Error> written = session.ports.bitable.upsert(table, rows);
    if (!written) {
        return std::unexpected(written.error());
    }
    return json::object();
}

[[nodiscard]] std::expected<void, Error> create_calendar(Session& session,
                                                        const json& attendees,
                                                        std::string title,
                                                        std::string start,
                                                        std::string end,
                                                        std::string& calendar_id,
                                                        std::string& event_id) {
    if (attendees.empty()) {
        return {};
    }
    if (session.config.calendar_id.empty()) {
        const std::expected<std::string, Error> primary = session.ports.feishu.primary_calendar_id();
        if (!primary) {
            return std::unexpected(primary.error());
        }
        calendar_id = *primary;
    } else {
        calendar_id = session.config.calendar_id;
    }
    const json event = {{"calendar_id", calendar_id},
                        {"summary", std::move(title)},
                        {"start", {{"date_time", std::move(start)}, {"timezone", "Asia/Shanghai"}}},
                        {"end", {{"date_time", std::move(end)}, {"timezone", "Asia/Shanghai"}}},
                        {"attendees", attendees}};
    const std::expected<json, Error> created = session.ports.feishu.create_calendar_event(event);
    if (!created) {
        return std::unexpected(created.error());
    }
    event_id = created->value("event_id", "");
    return {};
}

[[nodiscard]] bool meeting_worthy(const json& item, std::string_view today) {
    const std::string node = item.value("node", "");
    if (node == "release") {
        return true;
    }
    const std::string meet = item.value("meet", "");
    const std::string status = item.value("status", "");
    const std::string start = item.value("start", "");
    const std::string end = item.value("end", "");
    if (meet == "at_start" && !start.empty() && today >= start) {
        return true;
    }
    if (meet == "at_end" && !end.empty() && today >= end) {
        return true;
    }
    if (meet == "when_blocked" && status == "blocked") {
        return true;
    }
    const std::string kind = item.value("kind", item.value("类型", ""));
    return kind == "milestone" && !start.empty() && today >= start;
}

[[nodiscard]] bool merely_late(const json& items, const json& parsed, std::string_view today) {
    std::vector<std::string> ids;
    if (parsed.contains("meetings") && parsed.at("meetings").is_array()) {
        for (const json& meeting : parsed.at("meetings")) {
            if (meeting.is_object() && meeting.contains("item_id") && meeting.at("item_id").is_string()) {
                ids.push_back(meeting.at("item_id").get_ref<const std::string&>());
            }
        }
    }
    if (parsed.contains("ai_recommended_item_id") && parsed.at("ai_recommended_item_id").is_string()) {
        ids.push_back(parsed.at("ai_recommended_item_id").get_ref<const std::string&>());
    }
    if (ids.empty() || !items.is_array()) {
        return false;
    }
    for (const std::string& id : ids) {
        const json* found = nullptr;
        for (const json& item : items) {
            if (item.is_object() && item.value("id", "") == id) {
                found = &item;
                break;
            }
        }
        if (found != nullptr && meeting_worthy(*found, today)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_ids(const json& produced, const json& chase) {
    if (!produced.is_array() || !chase.is_array() || produced.size() != chase.size()) {
        return false;
    }
    for (std::size_t index = 0; index < chase.size(); ++index) {
        if (!produced.at(index).is_object() || !chase.at(index).is_object()) {
            return false;
        }
        if (produced.at(index).value("id", "") != chase.at(index).value("id", "")) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool pdf_has_text(std::string_view bytes) {
    return bytes.find("Tj") != std::string_view::npos || bytes.find("TJ") != std::string_view::npos;
}

[[nodiscard]] std::expected<json, Error> propose_status(Session& session,
                                                       const json& parsed,
                                                       const json& records,
                                                       const json& role_rows,
                                                       std::string_view open_id,
                                                       std::string_view message_id) {
    if (parsed.contains("title") || parsed.contains("predecessors") || parsed.contains("owner_role")) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "标题、前置和职责不能在对话里改"));
    }
    if (!parsed.contains("item_id") || !parsed.at("item_id").is_string()) {
        return std::unexpected(fail(ErrorCode::kUnknownField, "缺少工作项"));
    }
    const std::string& item_id = parsed.at("item_id").get_ref<const std::string&>();
    const json* item = find_record(records, item_id);
    if (item == nullptr) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "没有这件工作"));
    }
    if (!role_rows.empty() && !can_edit(roles_of(role_rows, open_id), item->value("职责", ""))) {
        return std::unexpected(fail(ErrorCode::kForbidden, "职责不符"));
    }
    const bool has_status = parsed.contains("status");
    const bool has_start = parsed.contains("start");
    const bool has_end = parsed.contains("end");
    if ((has_start || has_end) && item->value("node", "") != "flexible") {
        return json{{"action", "none"}};
    }
    json plan = {{"kind", "status"}, {"item_id", item_id}};
    if (has_status && parsed.at("status").is_string()) {
        plan["status"] = parsed.at("status");
    }
    if (has_start && parsed.at("start").is_string()) {
        plan["start"] = parsed.at("start");
    }
    if (has_end && parsed.at("end").is_string()) {
        plan["end"] = parsed.at("end");
    }
    const std::expected<void, Error> begun = begin_plan(session, open_id, message_id, plan);
    if (!begun) {
        return std::unexpected(begun.error());
    }
    const std::expected<void, Error> sent = send_message(
            session, json{{"text", "请确认"}, {"buttons", json::array({"确认", "取消"})}, {"open_id", open_id}});
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return parsed;
}

[[nodiscard]] std::expected<json, Error> propose_reserve(Session& session,
                                                        const json& parsed,
                                                        const json& records,
                                                        const json& role_rows,
                                                        std::string_view open_id,
                                                        std::string_view message_id) {
    if (!parsed.contains("item_id") || !parsed.at("item_id").is_string()) {
        return std::unexpected(fail(ErrorCode::kUnknownField, "缺少工作项"));
    }
    const std::string& item_id = parsed.at("item_id").get_ref<const std::string&>();
    const json* item = find_record(records, item_id);
    if (item == nullptr) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "没有这件工作"));
    }
    if (!role_rows.empty() && !can_edit(roles_of(role_rows, open_id), item->value("职责", ""))) {
        return std::unexpected(fail(ErrorCode::kForbidden, "职责不符"));
    }
    json plan = {{"kind", "reserve"},
                 {"item_id", item_id},
                 {"start", parsed.value("start", "")},
                 {"end", parsed.value("end", "")},
                 {"title", parsed.value("title", item->value("标题", ""))}};
    const std::expected<void, Error> begun = begin_plan(session, open_id, message_id, std::move(plan));
    if (!begun) {
        return std::unexpected(begun.error());
    }
    const std::expected<void, Error> sent = send_message(
            session, json{{"text", "请确认预定"}, {"buttons", json::array({"确认", "取消"})}});
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return parsed;
}

[[nodiscard]] std::expected<json, Error> continue_plan(Session& session,
                                                      std::string_view open_id,
                                                      const fs::path& directory,
                                                      std::string_view text) {
    const json context = json::parse(read_text(directory / "context.json"), nullptr, false);
    if (context.is_discarded()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "这次交互没有上下文"));
    }
    static_cast<void>(open_id);
    return ask(session, "status_update", context.dump() + "\n" + std::string(text));
}

[[nodiscard]] std::expected<json, Error> on_cancel_text(Session& session, std::string_view open_id) {
    if (const std::optional<fs::path> own = find_person_dir(session.config.data_root, open_id)) {
        const std::optional<LocatedPlan> plan = find_plan(session.config.data_root, own->filename().string());
        if (plan && plan->owner == open_id) {
            return cancel_plan(session, *plan, false);
        }
    }
    const std::expected<json, Error> roles = list_table(session, kRoleTable);
    if (!roles) {
        return std::unexpected(roles.error());
    }
    const std::vector<std::string> owned = roles_of(*roles, open_id);
    if (std::find(owned.begin(), owned.end(), "pm") != owned.end()) {
        if (const std::optional<fs::path> system = find_person_dir(session.config.data_root, "system")) {
            const std::optional<LocatedPlan> plan =
                    find_plan(session.config.data_root, system->filename().string());
            if (plan) {
                return cancel_plan(session, *plan, false);
            }
        }
    }
    return json::object();
}

[[nodiscard]] std::expected<json, Error> on_group(Session& session, const json& event) {
    if (!event.value("mentions_bot", false)) {
        return std::unexpected(fail(ErrorCode::kUnknownEvent, "群消息没有 @ 机器人"));
    }
    if (event.contains("attachments")) {
        return json{{"ignored", true}};
    }
    const std::string open_id = event.value("open_id", "");
    const std::string message_id = event.value("message_id", "");
    const std::string text = event.value("text", "");
    if (trim_copy(text) == "取消") {
        return on_cancel_text(session, open_id);
    }
    if (const std::optional<fs::path> open = find_person_dir(session.config.data_root, open_id)) {
        return continue_plan(session, open_id, *open, text);
    }
    if (session.awaiting_role.contains(open_id)) {
        session.awaiting_role.erase(open_id);
        json plan = {{"kind", "role"}, {"role", trim_copy(text)}};
        const std::expected<void, Error> begun = begin_plan(session, open_id, message_id, std::move(plan));
        if (!begun) {
            return std::unexpected(begun.error());
        }
        const std::expected<void, Error> sent =
                send_message(session, json{{"text", "请确认职责"}, {"buttons", json::array({"确认", "取消"})}});
        if (!sent) {
            return std::unexpected(sent.error());
        }
        return json{{"action", "confirm_role"}};
    }
    if (count_open(session.config.data_root) >= kOpenLimit) {
        const std::expected<void, Error> sent =
                send_message(session, json{{"text", "已经有 5 人在进行，请稍后再试"}});
        if (!sent) {
            return std::unexpected(sent.error());
        }
        return json::object();
    }

    const std::expected<json, Error> records = list_table(session, kMainTable);
    if (!records) {
        return std::unexpected(records.error());
    }
    const std::expected<json, Error> role_rows = list_table(session, kRoleTable);
    if (!role_rows) {
        return std::unexpected(role_rows.error());
    }
    const json payload = {{"text", text}, {"rows", relevant_rows(*records, text)}};
    const std::string step = text.find("预定") != std::string::npos ? "meeting_reserve" : "status_update";
    std::expected<json, Error> parsed = ask(session, step, payload.dump());
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->is_array()) {
        if (parsed->size() != 1 || !parsed->at(0).is_object()) {
            return json{{"action", "clarify"}};
        }
        parsed = parsed->at(0);
    }
    if (!parsed->is_object()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "模型输出不是对象"));
    }
    if (parsed->contains("text") && parsed->at("text").is_string() &&
        claims_human(parsed->at("text").get_ref<const std::string&>())) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "不能声称自己是真人"));
    }
    const std::string action = parsed->value("action", "");
    if (action == "update") {
        return propose_status(session, *parsed, *records, *role_rows, open_id, message_id);
    }
    if (action == "reserve") {
        return propose_reserve(session, *parsed, *records, *role_rows, open_id, message_id);
    }
    const std::expected<void, Error> begun =
            begin_plan(session, open_id, message_id, json{{"kind", "chat"}, {"action", action}});
    if (!begun && begun.error().message.find("已经有一次未结束的交互") == std::string::npos) {
        return std::unexpected(begun.error());
    }
    if (parsed->contains("text") && parsed->at("text").is_string()) {
        return json{{"text", parsed->at("text")}};
    }
    return *parsed;
}

[[nodiscard]] std::expected<json, Error> on_p2p(Session& session, const json& event) {
    const std::string text = event.value("text", "");
    const std::expected<json, Error> parsed = ask(session, "member_reply", json{{"text", text}}.dump());
    if (!parsed || !parsed->is_object() || !parsed->contains("text") || !parsed->at("text").is_string()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "成员回复不是 JSON"));
    }
    const std::string& reply = parsed->at("text").get_ref<const std::string&>();
    if (claims_human(reply)) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "不能声称自己是真人"));
    }
    if (event.contains("progress") && event.at("progress").is_number_integer() &&
        !numbers_match(reply, event.at("progress").get<int>())) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "进度数字不是输入里的数字"));
    }
    return json{{"text", reply}};
}

[[nodiscard]] std::expected<json, Error> apply_confirm(Session& session, const LocatedPlan& plan) {
    const std::expected<void, Error> audited =
            append_audit(session, json{{"op", "confirm"}, {"ok", true}, {"actor", plan.owner}});
    if (!audited) {
        return std::unexpected(audited.error());
    }
    if (session.config.stop_after_durable_audit) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "停在持久审计之后"));
    }
    const std::string kind = plan.context.value("kind", "");
    if (kind == "status") {
        json row = {{"业务id", plan.context.value("item_id", "")}};
        if (plan.context.contains("status")) {
            row["状态"] = plan.context.at("status");
        }
        if (plan.context.contains("start")) {
            row["开始"] = plan.context.at("start");
        }
        if (plan.context.contains("end")) {
            row["结束"] = plan.context.at("end");
        }
        const std::expected<json, Error> written = upsert_one(session, kMainTable, std::move(row));
        if (!written) {
            return std::unexpected(written.error());
        }
    } else if (kind == "role") {
        const std::expected<json, Error> written = upsert_one(
                session,
                kRoleTable,
                json{{"群id", session.config.group_id},
                     {"人员", json::array({json{{"id", plan.owner}}})},
                     {"职责", plan.context.value("role", "")}});
        if (!written) {
            return std::unexpected(written.error());
        }
    } else if (kind == "reserve") {
        const std::expected<json, Error> records = list_table(session, kMainTable);
        const std::expected<json, Error> roles = list_table(session, kRoleTable);
        if (!records || !roles) {
            return std::unexpected(records ? roles.error() : records.error());
        }
        const json* item = find_record(*records, plan.context.value("item_id", ""));
        const json attendees = attendees_for(*roles, item == nullptr ? "" : item->value("职责", ""));
        if (attendees.empty()) {
            return json::object();
        }
        std::string calendar_id;
        std::string event_id;
        const std::expected<void, Error> created =
                create_calendar(session,
                                attendees,
                                plan.context.value("title", ""),
                                plan.context.value("start", ""),
                                plan.context.value("end", ""),
                                calendar_id,
                                event_id);
        if (!created) {
            return std::unexpected(created.error());
        }
        const std::expected<json, Error> decision = upsert_one(
                session, kMainTable, json{{"业务id", plan.context.value("item_id", "")}, {"决定", "同意"}});
        if (!decision) {
            static_cast<void>(session.ports.feishu.delete_calendar_event(calendar_id, event_id));
            return std::unexpected(decision.error());
        }
    }
    return finish_plan(session, plan, "done", false);
}

[[nodiscard]] std::expected<json, Error> on_card(Session& session, const json& event) {
    const std::string action = event.value("action", "");
    if (action == "submit_role") {
        const json form = event.value("form", json::object());
        const std::string open_id = form.value("open_id", "");
        const std::string role = form.value("role", "");
        if (open_id.empty() || role.empty()) {
            return json::object();
        }
        const std::string group = event.value("group_id", session.config.group_id);
        return upsert_one(session,
                          kRoleTable,
                          json{{"群id", group},
                               {"人员", json::array({json{{"id", open_id}}})},
                               {"职责", role}});
    }
    const std::string actor = event.value("open_id", "");
    const std::string interaction_id = event.value("interaction_id", "");
    const std::optional<LocatedPlan> plan = find_plan(session.config.data_root, interaction_id);
    if (!plan) {
        return json::object();
    }
    if (action == "取消") {
        if (plan->owner != actor) {
            return json::object();
        }
        return cancel_plan(session, *plan, false);
    }
    if (action == "确认") {
        if (plan->owner != actor) {
            return json::object();
        }
        return apply_confirm(session, *plan);
    }
    if (action == "同意" || action == "先不办") {
        if (plan->context.value("kind", "") != "meeting") {
            return json::object();
        }
        const std::expected<json, Error> records = list_table(session, kMainTable);
        const std::expected<json, Error> roles = list_table(session, kRoleTable);
        if (!records || !roles) {
            return std::unexpected(records ? roles.error() : records.error());
        }
        const json* item = find_record(*records, plan->context.value("item_id", ""));
        const std::string item_role = item == nullptr ? "" : item->value("职责", "");
        if (!can_edit(roles_of(*roles, actor), item_role) && plan->owner != actor) {
            return json::object();
        }
        if (action == "先不办") {
            const std::expected<json, Error> written = upsert_one(
                    session, kMainTable, json{{"业务id", plan->context.value("item_id", "")}, {"决定", "先不办"}});
            if (!written) {
                return std::unexpected(written.error());
            }
            return finish_plan(session, *plan, "cancelled", false);
        }
        const json attendees = attendees_for(*roles, item_role);
        if (attendees.empty()) {
            return json::object();
        }
        std::string calendar_id;
        std::string event_id;
        const std::expected<void, Error> created = create_calendar(session,
                                                                   attendees,
                                                                   plan->context.value("title", ""),
                                                                   plan->context.value("start", ""),
                                                                   plan->context.value("end", ""),
                                                                   calendar_id,
                                                                   event_id);
        if (!created) {
            return std::unexpected(created.error());
        }
        const std::expected<json, Error> decision = upsert_one(
                session, kMainTable, json{{"业务id", plan->context.value("item_id", "")}, {"决定", "同意"}});
        if (!decision) {
            static_cast<void>(session.ports.feishu.delete_calendar_event(calendar_id, event_id));
            return std::unexpected(decision.error());
        }
        return finish_plan(session, *plan, "done", false);
    }
    return std::unexpected(fail(ErrorCode::kUnknownEvent, "不认识这张卡片"));
}

[[nodiscard]] std::expected<json, Error> on_timer(Session& session, const json& event) {
    const std::string message_id = event.value("message_id", "");
    if (event.value("step", "") == "follow_up") {
        const std::expected<json, Error> parsed = ask(session, "follow_up", event.dump());
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const std::expected<void, Error> begun =
                begin_plan(session, "system", message_id, json{{"kind", "follow_up"}});
        if (!begun) {
            return std::unexpected(begun.error());
        }
        return *parsed;
    }
    const std::expected<json, Error> parsed = ask(session, "meeting_recommendation", event.dump());
    if (!parsed || !parsed->is_object()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "会议建议不是 JSON"));
    }
    if (parsed->contains("meeting_end")) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "会议建议不接受 meeting_end"));
    }
    const bool recommends = (parsed->contains("meetings") && parsed->at("meetings").is_array() &&
                             !parsed->at("meetings").empty()) ||
                            (parsed->contains("ai_recommended_item_id") &&
                             !parsed->at("ai_recommended_item_id").is_null());
    if (event.contains("items") && recommends &&
        merely_late(event.at("items"), *parsed, event.value("today", ""))) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "只超期的工作不开会"));
    }
    if (!parsed->contains("meetings") || !parsed->at("meetings").is_array() || parsed->at("meetings").empty()) {
        return *parsed;
    }
    const auto clock_ok = [](const json& value) {
        if (!value.is_string()) {
            return false;
        }
        const std::string& text = value.get_ref<const std::string&>();
        if (text.size() != 16 || text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':') {
            return false;
        }
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (index == 4 || index == 7 || index == 10 || index == 13) {
                continue;
            }
            if (text[index] < '0' || text[index] > '9') {
                return false;
            }
        }
        return true;
    };
    for (const json& meeting : parsed->at("meetings")) {
        if (!meeting.is_object() || meeting.contains("meeting_end") || !meeting.contains("start") ||
            !meeting.contains("end") || !clock_ok(meeting.at("start")) || !clock_ok(meeting.at("end"))) {
            return std::unexpected(fail(ErrorCode::kEditRejected, "会议建议要带 start 和 end"));
        }
    }
    const std::expected<json, Error> roles = list_table(session, kRoleTable);
    if (!roles) {
        return std::unexpected(roles.error());
    }
    if (roles->empty()) {
        if (!session.config.group_id.empty()) {
            const std::expected<void, Error> sent = send_message(session, build_collection_card());
            if (!sent) {
                return std::unexpected(sent.error());
            }
        }
        return json::object();
    }
    if (session.config.group_id.empty()) {
        return *parsed;
    }
    const std::expected<json, Error> records = list_table(session, kMainTable);
    if (!records) {
        return std::unexpected(records.error());
    }
    std::string item_id;
    if (parsed->contains("ai_recommended_item_id") && parsed->at("ai_recommended_item_id").is_string()) {
        item_id = parsed->at("ai_recommended_item_id").get_ref<const std::string&>();
    } else if (parsed->at("meetings").at(0).is_object()) {
        item_id = parsed->at("meetings").at(0).value("item_id", "");
    }
    const json* item = find_record(*records, item_id);
    const json* chosen = nullptr;
    for (const json& meeting : parsed->at("meetings")) {
        if (!meeting.is_object() || meeting.contains("meeting_end") || !meeting.contains("start") ||
            !meeting.contains("end") || !clock_ok(meeting.at("start")) || !clock_ok(meeting.at("end"))) {
            return std::unexpected(fail(ErrorCode::kEditRejected, "会议建议要带 start 和 end"));
        }
        const std::string meeting_item = meeting.value("item_id", "");
        const json* meeting_row = find_record(*records, meeting_item);
        if (meeting_row != nullptr && meeting_row->value("node", "") == "release") {
            const std::string node_date = meeting_row->value("结束", meeting_row->value("end", ""));
            const std::string& meeting_end = meeting.at("end").get_ref<const std::string&>();
            if (node_date.size() < 10 || meeting_end.substr(0, 10) >= node_date.substr(0, 10)) {
                return std::unexpected(fail(ErrorCode::kEditRejected, "发布节点之前才开会"));
            }
        }
        if (chosen == nullptr || meeting_item == item_id) {
            chosen = &meeting;
        }
    }
    if (chosen == nullptr) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "会议建议要带 start 和 end"));
    }
    const std::string when = chosen->at("start").get_ref<const std::string&>();
    const std::string end = chosen->at("end").get_ref<const std::string&>();
    const std::string item_role = item == nullptr ? "" : item->value("职责", "");
    const json attendees = attendees_for(*roles, item_role);
    json mentions = json::array();
    for (const json& attendee : attendees) {
        if (attendee.contains("id")) {
            mentions.push_back(attendee.at("id"));
        }
    }
    const std::string title = parsed->at("meetings").at(0).is_object()
                                      ? parsed->at("meetings").at(0).value("title", "")
                                      : "";
    const std::expected<void, Error> sent = send_message(
            session,
            json{{"text", "AI推荐会议时间为" + when + "（北京时间）"},
                 {"buttons", json::array({"同意", "先不办"})},
                 {"mentions", mentions}});
    if (!sent) {
        return std::unexpected(sent.error());
    }
    const std::expected<void, Error> begun = begin_plan(session,
                                                       "system",
                                                       message_id,
                                                       json{{"kind", "meeting"},
                                                            {"item_id", item_id},
                                                            {"title", title},
                                                            {"start", when},
                                                            {"end", end}});
    if (!begun) {
        return std::unexpected(begun.error());
    }
    return *parsed;
}

[[nodiscard]] std::expected<json, Error> on_join(Session& session, const json& event) {
    const bool has_role = event.value("has_role", false);
    const json payload = {{"kind", event.value("kind", "")},
                          {"open_id", event.value("open_id", "")},
                          {"has_role", has_role},
                          {"text", event.value("text", "")}};
    const std::expected<json, Error> parsed = ask(session, "onboarding", payload.dump());
    if (!parsed || !parsed->is_object() || !parsed->contains("text") || !parsed->at("text").is_string() ||
        !parsed->contains("need_role") || !parsed->at("need_role").is_boolean()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "进群回复不是 JSON"));
    }
    const std::string& text = parsed->at("text").get_ref<const std::string&>();
    const bool need_role = parsed->at("need_role").get<bool>();
    if (claims_human(text) || need_role == has_role) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "进群回复不合格"));
    }
    json mentions = json::array();
    if (event.value("kind", "") == "join" && need_role && event.contains("open_id") &&
        event.at("open_id").is_string() && !event.at("open_id").get_ref<const std::string&>().empty()) {
        const std::string& open_id = event.at("open_id").get_ref<const std::string&>();
        mentions.push_back(open_id);
        session.awaiting_role.insert(open_id);
    }
    const std::expected<void, Error> sent = send_message(session, json{{"text", text}, {"mentions", mentions}});
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return json{{"text", text}, {"need_role", need_role}};
}

[[nodiscard]] std::expected<json, Error> on_review(Session& session, const json& event) {
    const std::expected<json, Error> parsed = ask(session, "discrepancy_review", event.dump());
    if (!parsed || !parsed->is_object() || !parsed->contains("conclusion") || !parsed->contains("reason") ||
        !parsed->at("conclusion").is_string() || !parsed->at("reason").is_string()) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "复查结论不合格"));
    }
    for (auto it = parsed->begin(); it != parsed->end(); ++it) {
        if (it.key() != "conclusion" && it.key() != "reason") {
            return std::unexpected(fail(ErrorCode::kEditRejected, "复查不能给出替代值"));
        }
    }
    const std::string& conclusion = parsed->at("conclusion").get_ref<const std::string&>();
    if (conclusion != "projector_defect" && conclusion != "prd_defect") {
        return std::unexpected(fail(ErrorCode::kEditRejected, "复查结论不在规格里"));
    }
    return json{{"conclusion", conclusion}, {"reason", parsed->at("reason")}};
}

[[nodiscard]] std::expected<json, Error> on_command(Session& session, const json& event) {
    const std::string name = event.value("name", "");
    const std::string open_id = event.value("open_id", "");
    if (name == "report") {
        return json::object();
    }
    if (name == "chase" && !event.contains("chase")) {
        if (!on_allowlist(session.config.chase_allowlist, open_id)) {
            return std::unexpected(fail(ErrorCode::kForbidden, "不在跟进名单里"));
        }
        return json::object();
    }
    if (name == "meet") {
        if (!on_allowlist(session.config.meet_allowlist, open_id)) {
            return std::unexpected(fail(ErrorCode::kForbidden, "不在会议名单里"));
        }
        return json::object();
    }
    if (name == "chase") {
        const std::expected<json, Error> parsed = ask(session, "follow_up", event.at("chase").dump());
        if (!parsed || !same_ids(*parsed, event.at("chase"))) {
            return std::unexpected(fail(ErrorCode::kEditRejected, "跟进名单的 id 变了"));
        }
        return *parsed;
    }
    return std::unexpected(fail(ErrorCode::kUnknownEvent, "不认识这个命令"));
}

[[nodiscard]] std::expected<json, Error> on_memory(Session& session, const json& event) {
    const std::string op = event.value("op", "");
    const std::string path = event.value("path", "");
    const std::string actor = event.value("actor", "");
    if (!safe_relative(path)) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "路径越界"));
    }
    const fs::path destination = session.config.data_root / path;
    if (op == "create" || op == "update") {
        const bool blocked = op == "update" && session.config.fail_semantic_replace;
        if (!blocked) {
            const std::expected<void, Error> written =
                    replace_file(destination, event.value("body", ""));
            if (!written) {
                static_cast<void>(append_audit(session, json{{"op", op}, {"ok", false}, {"actor", actor}}));
                return std::unexpected(written.error());
            }
        }
        const std::expected<void, Error> audited = append_audit(
                session, json{{"op", op}, {"ok", !blocked}, {"actor", actor}, {"path", path}});
        if (!audited) {
            return std::unexpected(audited.error());
        }
        if (blocked) {
            return std::unexpected(fail(ErrorCode::kEditRejected, "语义文件没有换成新的"));
        }
        return json::object();
    }
    if (op == "read") {
        static_cast<void>(read_text(destination));
        const std::expected<void, Error> audited =
                append_audit(session, json{{"op", "read"}, {"ok", true}, {"actor", actor}, {"path", path}});
        if (!audited) {
            return std::unexpected(audited.error());
        }
        return json::object();
    }
    return std::unexpected(fail(ErrorCode::kUnknownEvent, "不认识这个记忆操作"));
}

[[nodiscard]] std::expected<json, Error> dispatch_event(Session& session, const json& event) {
    if (!event.is_object() || !event.contains("kind") || !event.at("kind").is_string()) {
        return std::unexpected(fail(ErrorCode::kUnknownEvent, "不认识这个事件"));
    }
    const std::string& kind = event.at("kind").get_ref<const std::string&>();
    if (kind == "group_message") {
        return on_group(session, event);
    }
    if (kind == "p2p_message") {
        return on_p2p(session, event);
    }
    if (kind == "card_callback") {
        return on_card(session, event);
    }
    if (kind == "timer") {
        return on_timer(session, event);
    }
    if (kind == "join" || kind == "bot_added") {
        return on_join(session, event);
    }
    if (kind == "review") {
        return on_review(session, event);
    }
    if (kind == "command") {
        return on_command(session, event);
    }
    if (kind == "memory") {
        return on_memory(session, event);
    }
    return std::unexpected(fail(ErrorCode::kUnknownEvent, "不认识这个事件"));
}

[[nodiscard]] std::expected<json, Error> import_next(Session& session) {
    const std::expected<void, Error> bridge = check_bridge(session);
    if (!bridge) {
        return std::unexpected(bridge.error());
    }
    const fs::path inbox = session.config.data_root / "inbox";
    std::error_code error;
    if (!fs::is_directory(inbox, error)) {
        return json::object();
    }
    std::vector<fs::path> files;
    for (fs::directory_iterator it(inbox, error); !error && it != fs::directory_iterator(); it.increment(error)) {
        if (it->is_regular_file()) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return json::object();
    }
    const fs::path& file = files.front();
    const std::string ext = lower_copy(file.extension().string());
    const std::string bytes = read_text(file);
    if (ext == ".pdf") {
        if (!pdf_has_text(bytes)) {
            static_cast<void>(append_jsonl(session.config.data_root / "episodic/events.jsonl",
                                           json{{"path", "inbox/" + file.filename().string()},
                                                {"result", "no_text"}}));
            static_cast<void>(append_audit(session, json{{"op", "import"}, {"ok", false}}));
            return std::unexpected(fail(ErrorCode::kEditRejected, "PDF 没有文字层"));
        }
    }
    if (ext != ".json" && ext != ".md" && ext != ".docx" && ext != ".pdf") {
        return json{{"result", "unrecognized"}};
    }
    json manifest;
    if (ext == ".json") {
        const std::expected<json, Error> parsed = parse_json_text(bytes, true);
        if (!parsed) {
            static_cast<void>(append_audit(session, json{{"op", "import"}, {"ok", false}}));
            return std::unexpected(parsed.error());
        }
        manifest = *parsed;
    } else {
        const std::expected<json, Error> parsed = ask(session, "manifest_extract", bytes);
        if (!parsed || !parsed->is_object()) {
            static_cast<void>(append_audit(session, json{{"op", "import"}, {"ok", false}}));
            return std::unexpected(fail(ErrorCode::kEditRejected, "抽清单的输出不是 JSON"));
        }
        manifest = *parsed;
    }
    const std::expected<std::vector<json>, Error> rows = project_rows(manifest);
    if (!rows) {
        static_cast<void>(append_audit(session, json{{"op", "import"}, {"ok", false}}));
        return std::unexpected(rows.error());
    }
    json body = json::array();
    for (const json& row : *rows) {
        body.push_back(row);
    }
    const std::expected<void, Error> written = commit_rows(session, kMainTable, body);
    if (!written) {
        static_cast<void>(append_audit(session, json{{"op", "import"}, {"ok", false}}));
        return std::unexpected(written.error());
    }
    return json{{"rows", body}};
}

[[nodiscard]] std::expected<json, Error> run_sweep(Session& session) {
    const fs::path working = session.config.data_root / "working";
    std::error_code error;
    if (!fs::is_directory(working, error)) {
        return json::object();
    }
    std::vector<LocatedPlan> expired;
    const clock_tp now = session.ports.clock.now();
    for (fs::directory_iterator person(working, error); !error && person != fs::directory_iterator();
         person.increment(error)) {
        if (!person->is_directory()) {
            continue;
        }
        std::error_code child_error;
        for (fs::directory_iterator interaction(person->path(), child_error);
             !child_error && interaction != fs::directory_iterator();
             interaction.increment(child_error)) {
            if (!interaction->is_directory()) {
                continue;
            }
            const json context = json::parse(read_text(interaction->path() / "context.json"), nullptr, false);
            if (context.is_discarded() || !context.is_object() || !context.contains("created_unix") ||
                !context.at("created_unix").is_number_integer()) {
                continue;
            }
            const clock_tp created{std::chrono::seconds{context.at("created_unix").get<std::int64_t>()}};
            if (now - created >= std::chrono::minutes{30}) {
                expired.push_back(LocatedPlan{interaction->path(), person->path().filename().string(), context});
            }
        }
    }
    for (const LocatedPlan& plan : expired) {
        const std::expected<json, Error> cancelled = cancel_plan(session, plan, true);
        if (!cancelled) {
            return std::unexpected(cancelled.error());
        }
    }
    return json::object();
}

}  // namespace

App::App(Config config, Ports ports) : config_(std::move(config)), ports_(ports) {
    static_cast<void>(recover_finished_interactions(config_.data_root));
}

std::expected<json, Error> App::handle_event(const json& event) {
    Session session{config_, ports_, command_ids_, awaiting_role_};
    return dispatch_event(session, event);
}

std::expected<json, Error> App::import_inbox() {
    Session session{config_, ports_, command_ids_, awaiting_role_};
    return import_next(session);
}

std::expected<json, Error> App::project_manifest(const json& manifest) {
    const std::expected<std::vector<json>, Error> rows = project_rows(manifest);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return json{{"rows", *rows}};
}

std::expected<json, Error> App::project_manifest_text(std::string_view text) {
    const std::expected<json, Error> parsed = parse_json_text(text, true);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return project_manifest(*parsed);
}

std::expected<json, Error> App::compare_edits(const json& manifest, const json& edits) {
    const std::expected<std::vector<json>, Error> projected = project_rows(manifest);
    if (!projected) {
        return std::unexpected(projected.error());
    }
    if (!edits.contains("rows") || !edits.at("rows").is_array()) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "比较对象缺少 rows"));
    }
    json fields = json::array();
    for (const json& row : edits.at("rows")) {
        if (!row.is_object()) {
            continue;
        }
        const std::string id = row.value("业务id", "");
        const json* found = nullptr;
        for (const json& projected_row : *projected) {
            if (projected_row.value("业务id", "") == id) {
                found = &projected_row;
                break;
            }
        }
        for (auto it = row.begin(); it != row.end(); ++it) {
            if (it.key() == "业务id") {
                continue;
            }
            if (found == nullptr || !found->contains(it.key()) || found->at(it.key()) != it.value()) {
                fields.push_back(id + "." + it.key());
            }
        }
    }
    if (fields.empty()) {
        return json{{"consistent", true}};
    }
    return json{{"consistent", false}, {"fields", std::move(fields)}};
}

std::expected<json, Error> App::write_edits(const json& edits) {
    Session session{config_, ports_, command_ids_, awaiting_role_};
    const std::expected<void, Error> bridge = check_bridge(session);
    if (!bridge) {
        return std::unexpected(bridge.error());
    }
    if (!edits.contains("rows") || !edits.at("rows").is_array()) {
        return std::unexpected(fail(ErrorCode::kUnsupportedType, "写入缺少 rows"));
    }
    const std::string table = edits.value("table", std::string(kMainTable));
    const std::expected<void, Error> written = commit_rows(session, table, edits.at("rows"));
    if (!written) {
        return std::unexpected(written.error());
    }
    return json{{"written", edits.at("rows").size()}};
}

std::expected<json, Error> App::watch(const json& plan, const json& actual, std::string_view today) {
    static_cast<void>(plan);
    const json& items = actual.is_array() ? actual : plan;
    int done = 0;
    int total = 0;
    json chase = json::array();
    for (const json& item : items) {
        if (!item.is_object() || item.value("kind", "") != "work") {
            continue;
        }
        ++total;
        const std::string status = item.value("status", "");
        if (status == "done") {
            ++done;
            continue;
        }
        std::string judgment;
        if (status == "blocked") {
            judgment = "卡住";
        } else if (!item.value("end", "").empty() && today >= item.value("end", "")) {
            judgment = "超期";
        } else if (status == "todo" && today >= item.value("start", "")) {
            judgment = "还没开始";
        }
        if (!judgment.empty()) {
            chase.push_back(json{{"id", item.value("id", "")},
                                 {"title", item.value("title", "")},
                                 {"judgment", judgment},
                                 {"owner_role", item.value("owner_role", "")}});
        }
    }
    const int progress = total == 0 ? 0 : done * 100 / total;
    if (config_.template_name == "盯人待办" && !chase.empty()) {
        Session session{config_, ports_, command_ids_, awaiting_role_};
        json rows = json::array();
        for (const json& item : chase) {
            rows.push_back(json{{"业务id", item.value("id", "")}, {"判断", item.at("judgment")}, {"进度", progress}});
        }
        const std::expected<json, Error> written = session.ports.bitable.upsert(kMainTable, rows);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return json{{"progress", progress}, {"chase", std::move(chase)}};
}

std::expected<json, Error> App::audit_slices(const json& packet) {
    const std::string format = packet.value("format", "");
    const bool known = format == "markdown" || format == "docx" || format == "pdf";
    if (!known) {
        return json{{"ok", false}, {"slices", json::array({json{{"result", "unrecognized"}}})}};
    }
    const json item = packet.value("item", json::object());
    const std::string quote = item.value("source_quote", "");
    const std::string source = packet.value("source_text", "");
    if (quote.empty() || source.find(quote) == std::string::npos) {
        return json{{"ok", false},
                    {"slices", json::array({json{{"id", item.value("id", "")}, {"result", "mismatch"}}})}};
    }
    return json{{"ok", true}, {"slices", json::array()}};
}

std::expected<json, Error> App::enqueue(const json& command) {
    if (command_ids_.size() >= kQueueLimit) {
        return std::unexpected(fail(ErrorCode::kEditRejected, "队列已满"));
    }
    command_ids_.push_back(command.value("id", ""));
    return json::object();
}

std::vector<std::string> App::command_ids() const {
    return command_ids_;
}

std::expected<json, Error> App::sweep() {
    Session session{config_, ports_, command_ids_, awaiting_role_};
    return run_sweep(session);
}

std::expected<json, Error> App::preview_prompt(std::string_view step) {
    return json{{"system", system_prompt(config_, step)}};
}

}  // namespace robot_pm
