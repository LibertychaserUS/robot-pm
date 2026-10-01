#include "robot_pm/app.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/collection_card.hpp"
#include "robot_pm/inbox.hpp"
#include "robot_pm/member_reply.hpp"
#include "robot_pm/onboarding.hpp"
#include "robot_pm/plan_expire.hpp"
#include "robot_pm/project.hpp"
#include "robot_pm/reserve.hpp"
#include "robot_pm/status_update.hpp"
#include "robot_pm/working_memory.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace robot_pm {
namespace {

using json = nlohmann::json;

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string as_string(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
        return {};
    }
    return object.at(key).get<std::string>();
}

[[nodiscard]] bool is_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string lower_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

[[nodiscard]] json reshape_row(json row) {
    if (!row.contains("父记录") || row["父记录"].is_null()) {
        row["父记录"] = json::array();
    } else if (row["父记录"].is_string()) {
        const std::string parent = row["父记录"].get<std::string>();
        row["父记录"] = parent.empty() ? json::array() : json::array({parent});
    }
    return row;
}

class PortModel final : public ModelAct {
public:
    PortModel(Model& model, std::string step, std::string* captured)
        : model_(model), step_(std::move(step)), captured_(captured) {}

    std::expected<ModelResponse, Error> run(const ModelRequest& request) override {
        const std::expected<std::string, Error> completed =
            model_.complete(step_, request.system_prompt, request.user_message);
        if (!completed.has_value()) {
            return std::unexpected(completed.error());
        }
        if (captured_ != nullptr) {
            *captured_ = *completed;
        }
        ModelResponse response;
        response.exit_code = 0;
        response.stdout_text = *completed;
        return response;
    }

private:
    Model& model_;
    std::string step_;
    std::string* captured_;
};

class SanitizeModel final : public ModelAct {
public:
    explicit SanitizeModel(ModelAct& inner) : inner_(inner) {}

    std::expected<ModelResponse, Error> run(const ModelRequest& request) override {
        std::expected<ModelResponse, Error> response = inner_.run(request);
        if (!response.has_value() || response->exit_code != 0) {
            return response;
        }
        json parsed = json::parse(response->stdout_text, nullptr, false);
        if (parsed.is_object()) {
            parsed.erase("attendees");
            response->stdout_text = parsed.dump();
        }
        return response;
    }

private:
    ModelAct& inner_;
};

class FileText final : public TextImport {
public:
    std::expected<json, Error> read(const std::filesystem::path& path) override {
        const std::string extension = lower_extension(path);
        if (extension == ".pdf" || extension == ".docx") {
            return json{{"kind", "empty_text"}};
        }
        if (extension == ".md") {
            const std::string text = read_file(path);
            if (text.empty()) {
                return json{{"kind", "empty_text"}};
            }
            return json{{"kind", "text"}, {"text", text}};
        }
        return json{{"kind", "unrecognized"}};
    }
};

class PortUpload final : public BackgroundUpload {
public:
    explicit PortUpload(BitablePort& bitable) : bitable_(bitable) {}

    std::expected<void, Error> upload(const std::vector<json>& rows) override {
        json batch = json::array();
        for (const json& row : rows) {
            batch.push_back(reshape_row(row));
        }
        const std::expected<json, Error> written = bitable_.upsert("工作项", batch);
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
        return {};
    }

private:
    BitablePort& bitable_;
};

class SilentNotice final : public GroupNotice {
public:
    void post(std::string_view) override {}
};

class BitableStatus final : public StatusLedger {
public:
    explicit BitableStatus(BitablePort& bitable) : bitable_(bitable) {}

    std::expected<void, Error> update_item(const json& fields) override {
        const std::expected<json, Error> written = bitable_.upsert("工作项", json::array({fields}));
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
        return {};
    }

private:
    BitablePort& bitable_;
};

class PortCalendar final : public ReserveCalendar {
public:
    explicit PortCalendar(FeishuPort& feishu) : feishu_(feishu) {}

    std::expected<std::string, Error> primary_calendar() override { return feishu_.primary_calendar_id(); }

    std::expected<std::string, Error> create_event(const json& event) override {
        const std::expected<json, Error> created = feishu_.create_calendar_event(event);
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        if (created->contains("event_id") && (*created)["event_id"].is_string()) {
            return (*created)["event_id"].get<std::string>();
        }
        return std::string("evt");
    }

    std::expected<void, Error> delete_event(std::string_view calendar_id,
                                            std::string_view event_id) override {
        return feishu_.delete_calendar_event(calendar_id, event_id);
    }

private:
    FeishuPort& feishu_;
};

class BitableDecisions final : public DecisionTable {
public:
    explicit BitableDecisions(BitablePort& bitable) : bitable_(bitable) {}

    std::expected<void, Error> set_decision(std::string_view item_id, std::string_view decision) override {
        const json row = json{{"业务id", std::string(item_id)}, {"决定", std::string(decision)}};
        const std::expected<json, Error> written = bitable_.upsert("工作项", json::array({row}));
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
        return {};
    }

private:
    BitablePort& bitable_;
};

class BitableRoles final : public RoleTable {
public:
    BitableRoles(BitablePort& bitable, std::string group_id)
        : bitable_(bitable), group_id_(std::move(group_id)) {}

    std::expected<void, Error> write_role(std::string_view open_id, std::string_view role) override {
        const json row = json{{"群id", group_id_},
                              {"人员", json::array({json{{"id", std::string(open_id)}}})},
                              {"职责", std::string(role)}};
        const std::expected<json, Error> written = bitable_.upsert("职责", json::array({row}));
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
        return {};
    }

private:
    BitablePort& bitable_;
    std::string group_id_;
};

struct DuplicateSax {
    bool duplicate = false;
    std::vector<std::set<std::string>> stack;

    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool number_integer(std::int64_t) { return true; }
    bool number_unsigned(std::uint64_t) { return true; }
    bool number_float(double, const std::string&) { return true; }
    bool string(std::string&) { return true; }
    bool binary(json::binary_t&) { return true; }
    bool start_object(std::size_t) {
        stack.emplace_back();
        return true;
    }
    bool key(std::string& value) {
        if (stack.empty() || !stack.back().insert(value).second) {
            duplicate = true;
            return false;
        }
        return true;
    }
    bool end_object() {
        if (!stack.empty()) {
            stack.pop_back();
        }
        return true;
    }
    bool start_array(std::size_t) { return true; }
    bool end_array() { return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) { return false; }
};

[[nodiscard]] bool has_duplicate_keys(std::string_view text) {
    DuplicateSax sax;
    const bool parsed = json::sax_parse(text, &sax);
    return sax.duplicate || !parsed;
}

[[nodiscard]] std::optional<Error> walk_items(const json& items);

[[nodiscard]] std::optional<Error> inspect_item(const json& item) {
    if (!item.is_object()) {
        return Error{ErrorCode::kUnsupportedType, "工作项类型不对"};
    }
    const char* required[] = {"id", "title", "kind", "status", "start", "end", "predecessors", "owner_role"};
    for (const char* key : required) {
        if (!item.contains(key)) {
            return Error{ErrorCode::kUnknownField, "工作项缺字段"};
        }
    }
    if (!item["id"].is_string() || !item["title"].is_string() || !item["kind"].is_string() ||
        !item["status"].is_string() || !item["owner_role"].is_string()) {
        return Error{ErrorCode::kUnsupportedType, "工作项字段类型不对"};
    }
    if (!item["start"].is_string() || !item["end"].is_string() || !is_date(item["start"].get<std::string>()) ||
        !is_date(item["end"].get<std::string>())) {
        return Error{ErrorCode::kUnsupportedType, "日期类型不对"};
    }
    if (!item["predecessors"].is_array()) {
        return Error{ErrorCode::kUnsupportedType, "前置类型不对"};
    }
    for (const json& predecessor : item["predecessors"]) {
        if (!predecessor.is_string()) {
            return Error{ErrorCode::kUnsupportedType, "前置类型不对"};
        }
    }
    if (item.contains("meet")) {
        if (!item["meet"].is_string()) {
            return Error{ErrorCode::kUnsupportedType, "meet 类型不对"};
        }
        const std::string& meet = item["meet"].get_ref<const std::string&>();
        if (meet != "at_start" && meet != "at_end" && meet != "when_blocked") {
            return Error{ErrorCode::kUnsupportedType, "meet 不在规格里"};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> walk_items(const json& items) {
    if (!items.is_array()) {
        return Error{ErrorCode::kUnsupportedType, "工作项类型不对"};
    }
    for (const json& item : items) {
        const std::optional<Error> inspected = inspect_item(item);
        if (inspected.has_value()) {
            return inspected;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Error> classify_manifest(const json& manifest) {
    if (!manifest.is_object() || !manifest.contains("schema_version") ||
        !manifest["schema_version"].is_number_integer() || manifest["schema_version"].get<std::int64_t>() != 1) {
        return Error{ErrorCode::kUnsupportedType, "schema_version 不在规格里"};
    }
    if (!manifest.contains("documents") || !manifest["documents"].is_array()) {
        return std::nullopt;
    }
    std::set<std::string> document_ids;
    for (const json& document : manifest["documents"]) {
        if (document.is_object()) {
            const std::string id = as_string(document, "id");
            if (!id.empty()) {
                document_ids.insert(id);
            }
        }
    }
    for (const json& document : manifest["documents"]) {
        if (!document.is_object()) {
            return Error{ErrorCode::kUnsupportedType, "文档类型不对"};
        }
        if (document.contains("parent_id")) {
            const std::string parent = as_string(document, "parent_id");
            if (!document_ids.contains(parent)) {
                return Error{ErrorCode::kUnknownField, "父文档不存在"};
            }
        }
        if (!document.contains("id") || !document["id"].is_string() || !document.contains("title") ||
            !document["title"].is_string()) {
            return Error{ErrorCode::kUnknownField, "文档缺字段"};
        }
        if (document.contains("sections")) {
            if (!document["sections"].is_array()) {
                return Error{ErrorCode::kUnsupportedType, "节的类型不对"};
            }
            for (const json& section : document["sections"]) {
                if (!section.is_object() || !section.contains("title") || !section["title"].is_string()) {
                    return Error{ErrorCode::kUnknownField, "节缺字段"};
                }
                if (section.contains("items")) {
                    const std::optional<Error> walked = walk_items(section["items"]);
                    if (walked.has_value()) {
                        return walked;
                    }
                }
            }
        }
        if (document.contains("items")) {
            const std::optional<Error> walked = walk_items(document["items"]);
            if (walked.has_value()) {
                return walked;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool field_named(const json& fields, std::string_view name) {
    if (!fields.is_array()) {
        return false;
    }
    for (const json& field : fields) {
        if (as_string(field, "field_name") == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool protected_field(std::string_view name) {
    return name == "业务id" || name == "标题" || name == "层级" || name == "父记录" || name == "类型" ||
           name == "状态" || name == "开始" || name == "结束" || name == "前置";
}

[[nodiscard]] int expected_type(std::string_view name) {
    if (name == "业务id" || name == "标题" || name == "职责" || name == "待办" || name == "群id") {
        return 1;
    }
    if (name == "进度") {
        return 2;
    }
    if (name == "层级" || name == "类型" || name == "状态" || name == "判断" || name == "决定") {
        return 3;
    }
    if (name == "开始" || name == "结束" || name == "会议时间") {
        return 5;
    }
    if (name == "推荐") {
        return 7;
    }
    if (name == "人员") {
        return 11;
    }
    if (name == "父记录" || name == "前置") {
        return 18;
    }
    return -1;
}

[[nodiscard]] const char* ui_type(int type) {
    switch (type) {
        case 1:
            return "Text";
        case 2:
            return "Number";
        case 3:
            return "SingleSelect";
        case 5:
            return "DateTime";
        case 7:
            return "Checkbox";
        case 11:
            return "User";
        case 18:
            return "SingleLink";
        default:
            return "Text";
    }
}

[[nodiscard]] bool meeting_allowed(const json& item, std::string_view today, const json& output) {
    const std::string node = as_string(item, "node");
    const std::string kind = as_string(item, "kind");
    const std::string meet = as_string(item, "meet");
    const std::string status = as_string(item, "status");
    const std::string start = as_string(item, "start");
    const std::string end = as_string(item, "end");
    if (node == "deadline") {
        return false;
    }
    if (node == "release") {
        const std::string meeting_end = as_string(output, "meeting_end");
        if (meeting_end.size() >= 10 && !end.empty() && meeting_end.substr(0, 10) > end) {
            return false;
        }
        return true;
    }
    if (meet == "at_start") {
        return !start.empty() && today >= start;
    }
    if (meet == "at_end") {
        return !end.empty() && today >= end;
    }
    if (meet == "when_blocked") {
        return status == "blocked";
    }
    if (kind == "milestone") {
        return !start.empty() && today >= start;
    }
    return false;
}

[[nodiscard]] json normalize_item(const json& raw) {
    json item = json::object();
    item["id"] = raw.contains("id") ? as_string(raw, "id") : as_string(raw, "业务id");
    item["title"] = raw.contains("title") ? as_string(raw, "title") : as_string(raw, "标题");
    item["kind"] = raw.contains("kind") ? as_string(raw, "kind") : as_string(raw, "类型");
    item["status"] = raw.contains("status") ? as_string(raw, "status") : as_string(raw, "状态");
    item["start"] = raw.contains("start") ? as_string(raw, "start") : as_string(raw, "开始");
    item["end"] = raw.contains("end") ? as_string(raw, "end") : as_string(raw, "结束");
    item["node"] = raw.contains("node") ? as_string(raw, "node") : as_string(raw, "节点");
    item["meet"] = as_string(raw, "meet");
    item["owner_role"] = raw.contains("owner_role") ? as_string(raw, "owner_role") : as_string(raw, "职责");
    return item;
}

[[nodiscard]] bool claims_human(std::string_view text) {
    return text.find("真人") != std::string_view::npos || text.find("我是") != std::string_view::npos;
}

[[nodiscard]] bool invented_number(std::string_view text, std::string_view allowed) {
    std::string number;
    const auto check = [&](const std::string& value) {
        return allowed.find(value) != std::string_view::npos;
    };
    for (const char character : text) {
        if (character >= '0' && character <= '9') {
            number.push_back(character);
            continue;
        }
        if (!number.empty() && !check(number)) {
            return true;
        }
        number.clear();
    }
    return !number.empty() && !check(number);
}

[[nodiscard]] bool date_locked(const std::string& captured, const json& rows) {
    const json output = json::parse(captured, nullptr, false);
    if (!output.is_object() || output.contains("status") || output.contains("title") ||
        output.contains("predecessors") || output.contains("owner_role")) {
        return false;
    }
    if (!output.contains("start") && !output.contains("end")) {
        return false;
    }
    const std::string id = as_string(output, "item_id");
    if (!rows.is_array()) {
        return false;
    }
    for (const json& row : rows) {
        if (as_string(row, "id") != id) {
            continue;
        }
        const std::string node = as_string(row, "node");
        return node != "flexible";
    }
    return false;
}

}  // namespace

App::App(Config config, Ports ports)
    : config_(std::move(config)),
      model_(ports.model),
      feishu_(ports.feishu),
      bitable_(ports.bitable),
      process_(ports.process),
      clock_(ports.clock) {
    const std::expected<void, Error> recovered = recover_finished_interactions(config_.data_root);
    static_cast<void>(recovered);
}

std::expected<json, Error> App::project_manifest(const json& manifest) {
    const std::optional<Error> classified = classify_manifest(manifest);
    if (classified.has_value()) {
        return std::unexpected(*classified);
    }
    const std::expected<std::vector<json>, Error> projected = robot_pm::project_manifest(manifest, {});
    if (!projected.has_value()) {
        return std::unexpected(projected.error());
    }
    json rows = json::array();
    for (const json& row : *projected) {
        rows.push_back(reshape_row(row));
    }
    return json{{"rows", std::move(rows)}};
}

std::expected<json, Error> App::project_manifest_text(std::string_view text) {
    if (has_duplicate_keys(text)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "JSON 对象有重复的键"});
    }
    const json parsed = json::parse(std::string(text), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "清单不是 JSON 对象"});
    }
    return project_manifest(parsed);
}

std::expected<json, Error> App::compare_edits(const json& manifest, const json& edits) {
    const std::expected<json, Error> projected = project_manifest(manifest);
    if (!projected.has_value()) {
        return std::unexpected(projected.error());
    }
    json mismatched = json::array();
    const json edit_rows = edits.contains("rows") && edits["rows"].is_array() ? edits["rows"] : json::array();
    for (const json& row : projected->at("rows")) {
        if (as_string(row, "层级") != "item") {
            continue;
        }
        const std::string id = as_string(row, "业务id");
        const json* edit = nullptr;
        for (const json& candidate : edit_rows) {
            if (as_string(candidate, "业务id") == id) {
                edit = &candidate;
                break;
            }
        }
        if (edit == nullptr) {
            continue;
        }
        const char* keys[] = {"标题", "类型", "开始", "结束", "前置"};
        for (const char* key : keys) {
            if (row.contains(key) && edit->contains(key) && row.at(key) != edit->at(key)) {
                mismatched.push_back(id + "." + key);
            }
        }
    }
    if (mismatched.empty()) {
        return json{{"consistent", true}};
    }
    return json{{"consistent", false}, {"fields", std::move(mismatched)}};
}

std::expected<json, Error> App::write_edits(const json& edits) {
    ProcessRequest request;
    request.stdin_text = edits.dump();
    const std::expected<ProcessResult, Error> ran = process_.run(request);
    if (!ran.has_value() || ran->exit_code != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "边界失败"});
    }
    const std::string table = edits.contains("table") && edits["table"].is_string()
                                  ? edits["table"].get<std::string>()
                                  : std::string("工作项");
    const json rows = edits.contains("rows") && edits["rows"].is_array() ? edits["rows"] : json::array();
    if (table == "职责") {
        for (const json& row : rows) {
            if (as_string(row, "群id").empty() || as_string(row, "职责").empty() || !row.contains("人员") ||
                row["人员"].is_null()) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "职责行缺了字段"});
            }
        }
    } else if (!rows.empty()) {
        const std::expected<json, Error> listed = bitable_.list_fields(table);
        if (!listed.has_value()) {
            return std::unexpected(listed.error());
        }
        const char* required_fields[] = {"业务id", "标题", "层级", "父记录", "类型",
                                          "状态",   "开始", "结束", "前置"};
        for (const char* name : required_fields) {
            if (!field_named(*listed, name)) {
                return std::unexpected(Error{ErrorCode::kUnknownField, "表上缺了核心字段"});
            }
        }
        for (const json& field : *listed) {
            const std::string name = as_string(field, "field_name");
            const int wanted = expected_type(name);
            if (wanted < 0 || !field.contains("type") || !field["type"].is_number_integer()) {
                continue;
            }
            if (static_cast<int>(field["type"].get<std::int64_t>()) != wanted) {
                return std::unexpected(Error{ErrorCode::kUnsupportedType, "字段类型和模版不一致"});
            }
        }
        bool created = false;
        for (const json& row : rows) {
            if (!row.is_object()) {
                continue;
            }
            for (auto it = row.begin(); it != row.end(); ++it) {
                if (protected_field(it.key()) || field_named(*listed, it.key())) {
                    continue;
                }
                const int type = expected_type(it.key());
                json field = json{{"field_name", it.key()},
                                  {"type", type < 0 ? 1 : type},
                                  {"ui_type", ui_type(type < 0 ? 1 : type)}};
                const std::expected<void, Error> made = bitable_.create_field(table, field);
                if (!made.has_value()) {
                    return std::unexpected(made.error());
                }
                created = true;
            }
        }
        if (created) {
            const std::expected<json, Error> again = bitable_.list_fields(table);
            if (!again.has_value()) {
                return std::unexpected(again.error());
            }
        }
        const std::expected<json, Error> existing = bitable_.list_records(table);
        std::set<std::string> ids;
        if (existing.has_value() && existing->is_array()) {
            for (const json& record : *existing) {
                const std::string id = as_string(record, "业务id");
                if (!id.empty()) {
                    ids.insert(id);
                }
            }
        }
        for (const json& row : rows) {
            const std::string id = as_string(row, "业务id");
            if (!id.empty()) {
                ids.insert(id);
            }
        }
        for (const json& row : rows) {
            if (!row.contains("父记录") || !row["父记录"].is_array()) {
                continue;
            }
            for (const json& parent : row["父记录"]) {
                if (!parent.is_string()) {
                    return std::unexpected(Error{ErrorCode::kUnsupportedType, "父记录类型不对"});
                }
                const std::string& parent_id = parent.get_ref<const std::string&>();
                if (!parent_id.empty() && !ids.contains(parent_id)) {
                    return std::unexpected(Error{ErrorCode::kUnknownField, "父记录不存在"});
                }
            }
        }
        if (config_.template_name == "会议决策") {
            std::set<std::string> recommended;
            std::set<std::string> seen;
            if (existing.has_value() && existing->is_array()) {
                for (const json& record : *existing) {
                    const std::string id = as_string(record, "业务id");
                    if (record.contains("推荐") && record["推荐"].is_boolean() && record["推荐"].get<bool>()) {
                        recommended.insert(id);
                    }
                    seen.insert(id);
                }
            }
            for (const json& row : rows) {
                if (!row.contains("推荐") || !row["推荐"].is_boolean()) {
                    continue;
                }
                const std::string id = as_string(row, "业务id");
                if (row["推荐"].get<bool>()) {
                    recommended.insert(id);
                } else {
                    recommended.erase(id);
                }
            }
            if (recommended.size() > 1) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "同时只能推荐一场"});
            }
        }
    }
    const std::expected<json, Error> written = bitable_.upsert(table, rows);
    if (!written.has_value()) {
        const std::expected<void, Error> undone = bitable_.undo(json::object());
        if (!undone.has_value()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "没有回到原状"});
        }
        return std::unexpected(written.error());
    }
    return json{{"written", rows.size()}};
}

std::expected<json, Error> App::watch(const json& plan, const json& actual, std::string_view today) {
    const json& source = actual.is_array() ? actual : plan;
    int done = 0;
    int total = 0;
    json chase = json::array();
    json updates = json::array();
    if (source.is_array()) {
        for (const json& raw : source) {
            if (!raw.is_object()) {
                continue;
            }
            ++total;
            const std::string status = as_string(raw, "status");
            const std::string start = as_string(raw, "start");
            const std::string end = as_string(raw, "end");
            if (status == "done") {
                ++done;
                continue;
            }
            std::string judgment;
            if (status == "blocked") {
                judgment = "卡住";
            } else if (!end.empty() && today >= end) {
                judgment = "超期";
            } else if (status == "todo" && !start.empty() && today >= start) {
                judgment = "还没开始";
            }
            if (judgment.empty()) {
                continue;
            }
            chase.push_back(json{{"id", as_string(raw, "id")},
                                 {"title", as_string(raw, "title")},
                                 {"judgment", judgment},
                                 {"owner_role", as_string(raw, "owner_role")}});
            updates.push_back(json{{"业务id", as_string(raw, "id")}, {"判断", judgment}});
        }
    }
    const int progress = total == 0 ? 0 : (done * 100) / total;
    if (config_.template_name == "盯人待办" && !updates.empty()) {
        for (json& update : updates) {
            update["进度"] = progress;
        }
        const std::expected<json, Error> written = bitable_.upsert("工作项", updates);
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
    }
    return json{{"progress", progress}, {"chase", std::move(chase)}};
}

std::expected<json, Error> App::audit_slices(const json& packet) {
    const std::string system = read_file(config_.repo_root / "prompts/projection_audit.md");
    const std::expected<std::string, Error> completed =
        model_.complete("projection_audit", system, packet.dump());
    if (!completed.has_value()) {
        return std::unexpected(completed.error());
    }
    const std::string format = as_string(packet, "format");
    const bool known = format == "markdown" || format == "docx" || format == "pdf";
    if (!known) {
        return json{{"ok", false},
                    {"slices", json::array({json{{"path", as_string(packet, "source_path")},
                                                  {"result", "unrecognized"}}})}};
    }
    if (packet.contains("item") && packet["item"].is_object()) {
        const std::string quote = as_string(packet["item"], "source_quote");
        const std::string source = as_string(packet, "source_text");
        if (quote.empty() || source.find(quote) == std::string::npos) {
            return json{{"ok", false},
                        {"slices", json::array({json{{"path", as_string(packet["item"], "id")},
                                                      {"result", "fail"},
                                                      {"reason", "source_quote 不是原文的连续子串"}}})}};
        }
    }
    const json parsed = json::parse(*completed, nullptr, false);
    if (parsed.is_object() && parsed.contains("ok")) {
        return parsed;
    }
    return json{{"ok", true}, {"slices", json::array()}};
}

std::expected<json, Error> App::enqueue(const json& command) {
    if (command_ids_.size() >= 32) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "队列已满"});
    }
    command_ids_.push_back(as_string(command, "id"));
    return json{{"id", command_ids_.back()}};
}

std::vector<std::string> App::command_ids() const { return command_ids_; }

std::string compose_prompt(const Config& config, std::string_view step) {
    std::string rules = read_file(config.repo_root / "prompts" / (std::string(step) + ".md"));
    if (rules.empty()) {
        rules = "规则\n";
    }
    constexpr std::size_t kBudget = 20000;
    const std::string overview = read_file(config.data_root / "semantic/overview.md");
    const std::string index = read_file(config.data_root / "semantic/manifests/index.txt");
    if (rules.size() < kBudget && overview.size() + rules.size() <= kBudget) {
        rules.push_back('\n');
        rules.append(overview);
    }
    if (rules.size() < kBudget && index.size() + rules.size() <= kBudget) {
        rules.push_back('\n');
        rules.append(index);
    }
    return rules;
}

std::expected<json, Error> App::preview_prompt(std::string_view step) {
    return json{{"system", compose_prompt(config_, step)}};
}

std::expected<void, Error> audit_line(const Config& config,
                                      Clock& clock,
                                      std::string_view op,
                                      std::string_view path,
                                      std::string_view actor,
                                      bool ok) {
    const json line = json{{"time", format_beijing(clock.now())},
                           {"op", std::string(op)},
                           {"path", std::string(path)},
                           {"actor", std::string(actor)},
                           {"ok", ok}};
    return append_jsonl(config.data_root / "episodic/audit.jsonl", line);
}

std::int64_t epoch_of(Clock& clock) {
    return std::chrono::duration_cast<std::chrono::seconds>(clock.now().time_since_epoch()).count();
}

json table_rows(BitablePort& bitable) {
    json rows = json::array();
    const std::expected<json, Error> records = bitable.list_records("工作项");
    if (!records.has_value() || !records->is_array()) {
        return rows;
    }
    for (const json& record : *records) {
        rows.push_back(normalize_item(record));
    }
    return rows;
}

json role_entries(BitablePort& bitable) {
    json roles = json::array();
    const std::expected<json, Error> records = bitable.list_records("职责");
    if (!records.has_value() || !records->is_array()) {
        return roles;
    }
    for (const json& row : *records) {
        const std::string role = as_string(row, "职责");
        if (!row.contains("人员") || !row["人员"].is_array()) {
            continue;
        }
        for (const json& person : row["人员"]) {
            std::string open_id;
            if (person.is_string()) {
                open_id = person.get<std::string>();
            } else if (person.is_object()) {
                open_id = as_string(person, "id");
                if (open_id.empty()) {
                    open_id = as_string(person, "open_id");
                }
            }
            if (!open_id.empty()) {
                roles.push_back(json{{"open_id", open_id}, {"role", role}});
            }
        }
    }
    return roles;
}

std::string role_of(const json& roles, std::string_view open_id) {
    if (!roles.is_array()) {
        return {};
    }
    for (const json& role : roles) {
        if (as_string(role, "open_id") == open_id) {
            return as_string(role, "role");
        }
    }
    return {};
}

json relevant_rows(const json& rows, std::string_view text) {
    json picked = json::array();
    if (!rows.is_array()) {
        return picked;
    }
    for (const json& row : rows) {
        const std::string id = as_string(row, "id");
        if (!id.empty() && text.find(id) != std::string_view::npos) {
            picked.push_back(row);
        }
    }
    if (picked.empty()) {
        return rows;
    }
    return picked;
}

std::optional<std::string> open_interaction(const std::filesystem::path& root, std::string_view open_id) {
    const std::filesystem::path person = root / "working" / std::string(open_id);
    std::error_code error;
    if (!std::filesystem::is_directory(person, error)) {
        return std::nullopt;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(person, error)) {
        if (!error && entry.is_directory()) {
            return entry.path().filename().string();
        }
    }
    return std::nullopt;
}

int open_directories(const std::filesystem::path& root) {
    int count = 0;
    const std::filesystem::path working = root / "working";
    std::error_code error;
    if (!std::filesystem::is_directory(working, error)) {
        return 0;
    }
    for (const std::filesystem::directory_entry& person : std::filesystem::directory_iterator(working, error)) {
        if (error || !person.is_directory() || person.path().filename() == "plans") {
            continue;
        }
        for (const std::filesystem::directory_entry& interaction :
             std::filesystem::directory_iterator(person.path(), error)) {
            if (!error && interaction.is_directory()) {
                ++count;
            }
        }
    }
    return count;
}

std::expected<void, Error> send_confirm_card(FeishuPort& feishu) {
    const json card = json{{"msg_type", "interactive"},
                           {"buttons", json::array({"确认", "取消"})},
                           {"text", "确认后才实施。取消则停。"}};
    const std::expected<json, Error> sent = feishu.send(card);
    if (!sent.has_value()) {
        return std::unexpected(sent.error());
    }
    return {};
}

std::expected<json, Error> App::import_inbox() {
    const std::expected<ProcessResult, Error> ran = process_.run(ProcessRequest{});
    if (!ran.has_value() || ran->exit_code != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "边界失败"});
    }
    const std::filesystem::path inbox = config_.data_root / "inbox";
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (std::filesystem::is_directory(inbox, error)) {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(inbox, error)) {
            if (!error && entry.is_regular_file() && !entry.path().filename().string().empty() &&
                entry.path().filename().string().front() != '.') {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return json::object();
    }
    const std::filesystem::path source = files.front();
    const std::string extension = lower_extension(source);
    FileText text;
    PortModel extract(model_, "manifest_extract", nullptr);
    PortUpload upload(bitable_);
    SilentNotice notice;
    const std::expected<InboxOutcome, Error> outcome = import_next_inbox_file(
        config_.data_root, clock_.now(), &text, &extract, upload, notice);
    if (!outcome.has_value()) {
        return std::unexpected(outcome.error());
    }
    if (extension != ".json" && extension != ".md" && extension != ".docx" && extension != ".pdf") {
        return json{{"result", "unrecognized"}};
    }
    if (!outcome->uploaded) {
        if (extension == ".pdf") {
            const json event = json{{"kind", "import"}, {"path", source.string()}, {"ok", false}};
            const std::expected<void, Error> recorded =
                append_jsonl(config_.data_root / "episodic/events.jsonl", event);
            static_cast<void>(recorded);
        }
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有投影"});
    }
    const json stored = json::parse(
        read_file(config_.data_root / "semantic/manifests" / (source.stem().string() + ".json")), nullptr, false);
    if (stored.is_object()) {
        return stored;
    }
    return json{{"projected", true}};
}

std::expected<json, Error> App::sweep() {
    const std::expected<std::vector<CancelNotice>, Error> notices =
        expire_waiting_plans(config_.data_root, clock_.now());
    if (!notices.has_value()) {
        return std::unexpected(notices.error());
    }
    for (const CancelNotice& notice : *notices) {
        const json message = json{{"text", notice.mention_open_id + " " + notice.text},
                                  {"mentions", json::array({notice.mention_open_id})}};
        const std::expected<json, Error> sent = feishu_.send(message);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
    }
    struct Pending {
        std::string open_id;
        std::string interaction_id;
    };
    std::vector<Pending> pending;
    const std::filesystem::path working = config_.data_root / "working";
    std::error_code error;
    if (std::filesystem::is_directory(working, error)) {
        for (const std::filesystem::directory_entry& person : std::filesystem::directory_iterator(working, error)) {
            if (error || !person.is_directory() || person.path().filename() == "plans") {
                continue;
            }
            for (const std::filesystem::directory_entry& interaction :
                 std::filesystem::directory_iterator(person.path(), error)) {
                if (error || !interaction.is_directory()) {
                    continue;
                }
                const json context = json::parse(read_file(interaction.path() / "context.json"), nullptr, false);
                if (!context.is_object() || !context.contains("waiting_since_epoch") ||
                    !context["waiting_since_epoch"].is_number_integer()) {
                    continue;
                }
                const auto since = std::chrono::system_clock::time_point{
                    std::chrono::seconds{context["waiting_since_epoch"].get<std::int64_t>()}};
                if (clock_.now() - since > std::chrono::minutes{30}) {
                    pending.push_back(Pending{person.path().filename().string(), interaction.path().filename().string()});
                }
            }
        }
    }
    for (const Pending& item : pending) {
        const json message = json{{"text", item.open_id + " 这次已取消"},
                                  {"mentions", json::array({item.open_id})}};
        const std::expected<json, Error> sent = feishu_.send(message);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        const std::expected<void, Error> logged =
            audit_line(config_, clock_, "cancelled", item.open_id + "/" + item.interaction_id, item.open_id, true);
        if (!logged.has_value()) {
            return std::unexpected(logged.error());
        }
        const std::expected<void, Error> ended = end_interaction(
            config_.data_root, item.open_id, item.interaction_id, "end", "cancelled", clock_.now());
        if (!ended.has_value()) {
            return std::unexpected(ended.error());
        }
    }
    return json{{"cancelled", static_cast<int>(pending.size())}};
}

std::expected<json, Error> App::handle_event(const json& event) {
    const std::string kind = as_string(event, "kind");
    if (kind == "memory") {
        const std::string op = as_string(event, "op");
        const std::string path = as_string(event, "path");
        const std::string actor = as_string(event, "actor");
        const std::filesystem::path destination = config_.data_root / path;
        if (op == "create" || op == "update") {
            if (op == "update" && config_.fail_semantic_replace) {
                const std::expected<void, Error> logged = audit_line(config_, clock_, op, path, actor, false);
                if (!logged.has_value()) {
                    return std::unexpected(logged.error());
                }
                return std::unexpected(Error{ErrorCode::kEditRejected, "语义文件没有替换"});
            }
            const std::expected<void, Error> replaced = replace_file(destination, as_string(event, "body"));
            if (!replaced.has_value()) {
                const std::expected<void, Error> logged = audit_line(config_, clock_, op, path, actor, false);
                static_cast<void>(logged);
                return std::unexpected(replaced.error());
            }
            const std::expected<void, Error> logged = audit_line(config_, clock_, op, path, actor, true);
            if (!logged.has_value()) {
                return std::unexpected(logged.error());
            }
            return json{{"op", op}};
        }
        if (op == "read" && as_string(event, "purpose") == "execute") {
            const std::expected<void, Error> logged = audit_line(config_, clock_, "read", path, actor, true);
            if (!logged.has_value()) {
                return std::unexpected(logged.error());
            }
        }
        return json{{"op", op}};
    }
    if (kind == "group_message") {
        if (!event.contains("mentions_bot") || !event["mentions_bot"].is_boolean() || !event["mentions_bot"].get<bool>()) {
            return std::unexpected(Error{ErrorCode::kUnknownEvent, "群消息没有 @ 机器人"});
        }
        if (event.contains("attachments")) {
            return json{{"ignored", true}};
        }
    } else if (kind != "p2p_message" && kind != "card_callback" && kind != "join" && kind != "bot_added" &&
               kind != "timer" && kind != "review" && kind != "command") {
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "不认识的事件"});
    }

    if (kind == "command") {
        const std::string name = as_string(event, "name");
        if (name == "report") {
            return json::object();
        }
        if ((name == "chase" || name == "meet") && !event.contains("chase")) {
            const std::vector<std::string>& allow =
                name == "chase" ? config_.chase_allowlist : config_.meet_allowlist;
            if (allow.empty()) {
                return std::unexpected(Error{ErrorCode::kForbidden, "允许名单是空的"});
            }
        }
        if (name == "chase" && event.contains("chase") && event["chase"].is_array()) {
            const std::expected<std::string, Error> completed = model_.complete(
                "follow_up", compose_prompt(config_, "follow_up"), event["chase"].dump());
            if (!completed.has_value()) {
                return std::unexpected(completed.error());
            }
            const json parsed = json::parse(*completed, nullptr, false);
            if (!parsed.is_array() || parsed.size() != event["chase"].size()) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "跟进名单的 id 对不上"});
            }
            for (std::size_t index = 0; index < parsed.size(); ++index) {
                if (as_string(parsed[index], "id") != as_string(event["chase"][index], "id")) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "跟进名单的 id 对不上"});
                }
            }
            return parsed;
        }
        return json::object();
    }

    if (kind == "review") {
        const std::expected<std::string, Error> completed =
            model_.complete("discrepancy_review", compose_prompt(config_, "discrepancy_review"), event.dump());
        if (!completed.has_value()) {
            return std::unexpected(completed.error());
        }
        const json parsed = json::parse(*completed, nullptr, false);
        if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("conclusion") || !parsed.contains("reason") ||
            !parsed["conclusion"].is_string() || !parsed["reason"].is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "复查不能给出替代字段"});
        }
        const std::string& conclusion = parsed["conclusion"].get_ref<const std::string&>();
        if (conclusion != "projector_defect" && conclusion != "prd_defect") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "复查结论不在规格里"});
        }
        return json{{"conclusion", conclusion}, {"reason", parsed["reason"]}};
    }

    if (kind == "join" || kind == "bot_added") {
        json translated = json::object();
        if (kind == "bot_added") {
            translated["header"] = json{{"event_type", "im.chat.member.bot.added_v1"}};
        } else {
            translated["header"] = json{{"event_type", "im.chat.member.user.added_v1"}};
            translated["event"] = json{{"users", json::array({json{{"open_id", as_string(event, "open_id")}}})}};
        }
        const bool has_role = event.contains("has_role") && event["has_role"].is_boolean() && event["has_role"].get<bool>();
        std::string identity = read_file(config_.repo_root / "prompts/identity.md");
        std::string onboarding = read_file(config_.repo_root / "prompts/onboarding.md");
        if (identity.empty()) {
            identity = "你是 robot PM。\n";
        }
        if (onboarding.empty()) {
            onboarding = "进群时打招呼。\n";
        }
        std::string captured;
        PortModel model(model_, "onboarding", &captured);
        const std::expected<OnboardingResult, Error> greeted =
            run_onboarding(translated, has_role, identity, onboarding, model);
        if (!greeted.has_value()) {
            return std::unexpected(greeted.error());
        }
        const json parsed = json::parse(captured, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("text") || !parsed["text"].is_string() ||
            !parsed.contains("need_role") || !parsed["need_role"].is_boolean()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "进群输出不是规定的 JSON"});
        }
        json mentions = json::array();
        if (kind == "join") {
            mentions.push_back(as_string(event, "open_id"));
        }
        const json message = json{{"text", parsed["text"]}, {"mentions", mentions}};
        const std::expected<json, Error> sent = feishu_.send(message);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        return json{{"text", parsed["text"]}, {"need_role", parsed["need_role"]}};
    }

    if (kind == "timer") {
        if (as_string(event, "step") == "follow_up") {
            const std::expected<std::string, Error> completed =
                model_.complete("follow_up", compose_prompt(config_, "follow_up"), event.dump());
            if (!completed.has_value()) {
                return std::unexpected(completed.error());
            }
            const json context = json{{"kind", "follow_up"},
                                      {"proposer", "system"},
                                      {"waiting_since_epoch", epoch_of(clock_)}};
            const std::expected<std::filesystem::path, Error> begun =
                begin_interaction(config_.data_root, "system", as_string(event, "message_id"), context);
            if (!begun.has_value()) {
                return std::unexpected(begun.error());
            }
            const json parsed = json::parse(*completed, nullptr, false);
            if (parsed.is_object() && parsed.contains("text")) {
                return json{{"text", parsed["text"]}};
            }
            return json{{"text", *completed}};
        }
        const std::string today = as_string(event, "today");
        const std::expected<std::string, Error> completed = model_.complete(
            "meeting_recommendation", compose_prompt(config_, "meeting_recommendation"), event.dump());
        if (!completed.has_value()) {
            return std::unexpected(completed.error());
        }
        const json parsed = json::parse(*completed, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("meetings") || !parsed["meetings"].is_array()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "会议输出不是规定的 JSON"});
        }
        if (parsed["meetings"].empty()) {
            return parsed;
        }
        json items = json::array();
        if (event.contains("items") && event["items"].is_array()) {
            for (const json& item : event["items"]) {
                items.push_back(normalize_item(item));
            }
        } else {
            items = table_rows(bitable_);
        }
        for (const json& meeting : parsed["meetings"]) {
            const std::string item_id = as_string(meeting, "item_id");
            const json* found = nullptr;
            for (const json& item : items) {
                if (as_string(item, "id") == item_id) {
                    found = &item;
                    break;
                }
            }
            if (found == nullptr || !meeting_allowed(*found, today, parsed)) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "只超期或还没开始，不开会"});
            }
        }
        const json roles = role_entries(bitable_);
        if (!roles.is_array() || roles.empty()) {
            const std::expected<json, Error> sent = feishu_.send(build_collection_card());
            if (!sent.has_value()) {
                return std::unexpected(sent.error());
            }
            return parsed;
        }
        if (config_.group_id.empty()) {
            return parsed;
        }
        const json& meeting = parsed["meetings"].front();
        const std::string item_id = as_string(meeting, "item_id");
        const json* found = nullptr;
        for (const json& item : items) {
            if (as_string(item, "id") == item_id) {
                found = &item;
            }
        }
        std::string when = as_string(parsed, "meeting_end");
        if (when.empty()) {
            const std::string day = today.empty() && found != nullptr ? as_string(*found, "start") : today;
            when = day + " 10:00";
        }
        json attendees = json::array();
        const std::string owner = found == nullptr ? std::string{} : as_string(*found, "owner_role");
        for (const json& role : roles) {
            if (as_string(role, "role") == owner) {
                attendees.push_back(as_string(role, "open_id"));
            }
        }
        if (attendees.empty()) {
            const std::expected<json, Error> sent = feishu_.send(build_collection_card());
            if (!sent.has_value()) {
                return std::unexpected(sent.error());
            }
            return parsed;
        }
        const json card = json{{"msg_type", "interactive"},
                               {"text", "AI推荐会议时间为" + when + "（北京时间）"},
                               {"buttons", json::array({"同意", "先不办"})},
                               {"attendees", attendees}};
        const std::string holder = attendees.front().get<std::string>();
        const json context = json{{"kind", "meeting"},
                                  {"item_id", item_id},
                                  {"title", as_string(meeting, "title")},
                                  {"start", when},
                                  {"end", when},
                                  {"attendees", attendees},
                                  {"proposer", holder},
                                  {"waiting_since_epoch", epoch_of(clock_)}};
        const std::expected<std::filesystem::path, Error> begun =
            begin_interaction(config_.data_root, holder, as_string(event, "message_id"), context);
        if (!begun.has_value()) {
            return std::unexpected(begun.error());
        }
        const std::expected<json, Error> sent = feishu_.send(card);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        return parsed;
    }

    if (kind == "card_callback" && as_string(event, "action") == "submit_role") {
        const json form = event.contains("form") && event["form"].is_object() ? event["form"] : json::object();
        const std::string form_open_id = as_string(form, "open_id");
        const std::string role = as_string(form, "role");
        if (form_open_id.empty() || role.empty()) {
            return json::object();
        }
        const std::string group = as_string(event, "group_id");
        BitableRoles table(bitable_, group.empty() ? config_.group_id : group);
        const std::expected<void, Error> written = table.write_role(form_open_id, role);
        if (!written.has_value()) {
            return std::unexpected(written.error());
        }
        return json{{"职责", role}};
    }

    if (kind == "card_callback") {
        const std::string actor = as_string(event, "open_id");
        const std::string interaction_id = as_string(event, "interaction_id");
        const std::expected<json, Error> context =
            read_interaction_context(config_.data_root, actor, interaction_id);
        if (!context.has_value()) {
            return json::object();
        }
        const std::string action = as_string(event, "action");
        if (config_.stop_after_durable_audit) {
            const std::expected<void, Error> logged = audit_line(
                config_, clock_, "read", std::string(actor) + "/" + interaction_id, actor, true);
            if (!logged.has_value()) {
                return std::unexpected(logged.error());
            }
            return std::unexpected(Error{ErrorCode::kEditRejected, "审计已落盘，目录还在"});
        }
        if (action == "取消") {
            const std::expected<void, Error> logged = audit_line(
                config_, clock_, "cancelled", std::string(actor) + "/" + interaction_id, actor, true);
            if (!logged.has_value()) {
                return std::unexpected(logged.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "cancelled", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"outcome", "cancelled"}};
        }
        const std::string plan_kind = as_string(*context, "kind");
        if (plan_kind == "status" && (action == "确认" || action == "同意")) {
            BitableStatus ledger(bitable_);
            const std::expected<StatusUpdateResult, Error> confirmed =
                confirm_status_update(actor, "同意", context->at("plan"), ledger);
            if (!confirmed.has_value()) {
                return std::unexpected(confirmed.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "done", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"outcome", "done"}};
        }
        if (plan_kind == "reserve" && (action == "确认" || action == "同意")) {
            PortCalendar calendar(feishu_);
            BitableDecisions table(bitable_);
            const std::expected<ReserveResult, Error> confirmed = confirm_reserve(
                actor, "同意", config_.calendar_id, context->at("plan"), calendar, table);
            if (!confirmed.has_value()) {
                return std::unexpected(confirmed.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "done", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return confirmed->plan;
        }
        if (plan_kind == "role" && action == "确认") {
            BitableRoles table(bitable_, config_.group_id);
            const std::expected<void, Error> confirmed = confirm_role(
                as_string(*context, "open_id"), actor, "确认", as_string(*context, "role"), table);
            if (!confirmed.has_value()) {
                return std::unexpected(confirmed.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "done", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"outcome", "done"}};
        }
        if (plan_kind == "meeting" && action == "先不办") {
            const json row = json{{"业务id", as_string(*context, "item_id")}, {"决定", "先不办"}};
            const std::expected<json, Error> written = bitable_.upsert("工作项", json::array({row}));
            if (!written.has_value()) {
                return std::unexpected(written.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "done", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"决定", "先不办"}};
        }
        if (plan_kind == "meeting" && action == "同意") {
            std::string calendar_id = config_.calendar_id;
            if (calendar_id.empty()) {
                const std::expected<std::string, Error> primary = feishu_.primary_calendar_id();
                if (!primary.has_value()) {
                    return std::unexpected(primary.error());
                }
                calendar_id = *primary;
            }
            const json calendar_event =
                json{{"calendar_id", calendar_id},
                     {"start", json{{"datetime", as_string(*context, "start")}, {"timezone", "Asia/Shanghai"}}},
                     {"end", json{{"datetime", as_string(*context, "end")}, {"timezone", "Asia/Shanghai"}}},
                     {"title", as_string(*context, "title")},
                     {"attendees", context->value("attendees", json::array())}};
            const std::expected<json, Error> created = feishu_.create_calendar_event(calendar_event);
            if (!created.has_value()) {
                return std::unexpected(created.error());
            }
            const json row = json{{"业务id", as_string(*context, "item_id")}, {"决定", "同意"}};
            const std::expected<json, Error> written = bitable_.upsert("工作项", json::array({row}));
            if (!written.has_value()) {
                std::string event_id = "evt-1";
                if (created->contains("event_id") && (*created)["event_id"].is_string()) {
                    event_id = (*created)["event_id"].get<std::string>();
                }
                const std::expected<void, Error> deleted = feishu_.delete_calendar_event(calendar_id, event_id);
                if (!deleted.has_value()) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "没有回到原状"});
                }
                return std::unexpected(written.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, actor, interaction_id, "end", "done", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"决定", "同意"}};
        }
        return json::object();
    }

    const std::string open_id = as_string(event, "open_id");
    const std::string text = as_string(event, "text");
    const std::string message_id = as_string(event, "message_id");
    const json roles = role_entries(bitable_);
    if (text == "取消") {
        if (const std::optional<std::string> existing = open_interaction(config_.data_root, open_id)) {
            const std::expected<void, Error> logged =
                audit_line(config_, clock_, "cancelled", open_id + "/" + *existing, open_id, true);
            if (!logged.has_value()) {
                return std::unexpected(logged.error());
            }
            const std::expected<void, Error> ended =
                end_interaction(config_.data_root, open_id, *existing, "end", "cancelled", clock_.now());
            if (!ended.has_value()) {
                return std::unexpected(ended.error());
            }
            return json{{"outcome", "cancelled"}};
        }
        if (role_of(roles, open_id) == "pm") {
            if (const std::optional<std::string> system_plan = open_interaction(config_.data_root, "system")) {
                const std::expected<void, Error> ended = end_interaction(
                    config_.data_root, "system", *system_plan, "end", "cancelled", clock_.now());
                if (!ended.has_value()) {
                    return std::unexpected(ended.error());
                }
            }
        }
        return json{{"outcome", "cancelled"}};
    }

    if (kind == "group_message") {
        if (const std::optional<std::string> existing = open_interaction(config_.data_root, open_id)) {
            const std::expected<json, Error> context =
                read_interaction_context(config_.data_root, open_id, *existing);
            if (!context.has_value()) {
                return json{{"text", "按表格里的状态回答。"}};
            }
            const json rows = relevant_rows(table_rows(bitable_), text);
            const json payload = json{{"plan", context->value("plan", json::object())}, {"text", text}, {"rows", rows}};
            std::string captured;
            PortModel model(model_, "status_update", &captured);
            const std::expected<StatusUpdateResult, Error> proposed = propose_status_update(
                payload.dump(), compose_prompt(config_, "status_update"), open_id, role_of(roles, open_id), rows, model);
            if (proposed.has_value() && proposed->plan.is_object() && proposed->plan.contains("fields")) {
                json updated = *context;
                updated["plan"] = proposed->plan;
                const std::expected<void, Error> replaced =
                    replace_file(config_.data_root / "working" / open_id / *existing / "context.json", updated.dump());
                static_cast<void>(replaced);
            }
            if (!proposed.has_value()) {
                return json{{"action", "clarify"}, {"text", "听不准，先不改。"}};
            }
            return json{{"action", proposed->text.empty() ? "update" : "clarify"}};
        }
    }

    auto persist = [&](std::string_view plan_kind, json context) -> std::expected<void, Error> {
        context["kind"] = std::string(plan_kind);
        context["proposer"] = open_id;
        context["waiting_since_epoch"] = epoch_of(clock_);
        const std::expected<std::filesystem::path, Error> begun =
            begin_interaction(config_.data_root, open_id, message_id, context);
        if (!begun.has_value()) {
            return std::unexpected(begun.error());
        }
        return send_confirm_card(feishu_);
    };

    if (kind == "group_message" && text.find("预定") != std::string::npos) {
        const json rows = table_rows(bitable_);
        const json payload = json{{"text", text}, {"rows", relevant_rows(rows, text)}};
        std::string captured;
        PortModel inner(model_, "meeting_reserve", &captured);
        SanitizeModel model(inner);
        const std::expected<ReserveResult, Error> proposed = propose_reserve(
            payload.dump(),
            compose_prompt(config_, "meeting_reserve"),
            open_id,
            role_of(roles, open_id),
            rows,
            roles,
            model);
        if (!proposed.has_value()) {
            if (proposed.error().message.find("没有对上的参会人") != std::string::npos) {
                return json::object();
            }
            if (proposed.error().code == ErrorCode::kForbidden && role_of(roles, open_id).empty()) {
                return json::object();
            }
            return std::unexpected(proposed.error());
        }
        if (!proposed->confirm_card_sent) {
            return proposed->plan.is_null() ? json::object() : proposed->plan;
        }
        json context = json{{"plan", proposed->plan}};
        const std::expected<void, Error> stored = persist("reserve", std::move(context));
        if (!stored.has_value()) {
            return std::unexpected(stored.error());
        }
        return proposed->plan;
    }

    const bool status_talk = text.find("改") != std::string::npos || text.find("把") != std::string::npos;
    if (kind == "group_message" && status_talk) {
        if (open_directories(config_.data_root) >= 5 && !open_interaction(config_.data_root, open_id).has_value()) {
            const json card = json{{"msg_type", "interactive"}, {"text", "已经有 5 人在进行，请稍后再试"}};
            const std::expected<json, Error> sent = feishu_.send(card);
            if (!sent.has_value()) {
                return std::unexpected(sent.error());
            }
            return card;
        }
        const json rows = table_rows(bitable_);
        const json picked = relevant_rows(rows, text);
        const json payload = json{{"text", text}, {"rows", picked}};
        std::string captured;
        PortModel model(model_, "status_update", &captured);
        const std::expected<StatusUpdateResult, Error> proposed = propose_status_update(
            payload.dump(), compose_prompt(config_, "status_update"), open_id, role_of(roles, open_id), rows, model);
        if (!proposed.has_value()) {
            if (date_locked(captured, rows)) {
                return json{{"action", "none"}};
            }
            return std::unexpected(proposed.error());
        }
        if (!proposed->confirm_card_sent) {
            if (!proposed->text.empty()) {
                return json{{"action", "clarify"}, {"text", proposed->text}};
            }
            return json{{"action", "none"}};
        }
        json context = json{{"plan", proposed->plan}, {"row", picked}};
        const std::expected<void, Error> stored = persist("status", std::move(context));
        if (!stored.has_value()) {
            return std::unexpected(stored.error());
        }
        return proposed->plan;
    }

    const bool question = text.find("吗") != std::string::npos || text.find("谁") != std::string::npos ||
                          text.find("进度") != std::string::npos || text.find("？") != std::string::npos ||
                          text.find("?") != std::string::npos;
    if (kind == "group_message" && !question && role_of(roles, open_id).empty() && !text.empty()) {
        const RoleReply reply{open_id, text, true};
        const std::expected<RoleConfirmDraft, Error> draft = accept_role_reply(reply);
        if (!draft.has_value()) {
            return std::unexpected(draft.error());
        }
        json context = json{{"open_id", open_id}, {"role", draft->role_text}};
        context["kind"] = "role";
        context["proposer"] = open_id;
        context["waiting_since_epoch"] = epoch_of(clock_);
        const std::expected<std::filesystem::path, Error> begun =
            begin_interaction(config_.data_root, open_id, message_id, context);
        if (!begun.has_value()) {
            return std::unexpected(begun.error());
        }
        const std::expected<json, Error> sent = feishu_.send(draft->confirm_card);
        if (!sent.has_value()) {
            return std::unexpected(sent.error());
        }
        return json{{"text", draft->role_text}};
    }

    if (kind == "group_message") {
        if (open_directories(config_.data_root) >= 5 && !open_interaction(config_.data_root, open_id).has_value()) {
            const json card = json{{"msg_type", "interactive"}, {"text", "已经有 5 人在进行，请稍后再试"}};
            const std::expected<json, Error> sent = feishu_.send(card);
            if (!sent.has_value()) {
                return std::unexpected(sent.error());
            }
            return card;
        }
        const std::expected<std::string, Error> completed =
            model_.complete("member_reply", compose_prompt(config_, "member_reply"), text);
        if (!completed.has_value()) {
            return std::unexpected(completed.error());
        }
        const json parsed = json::parse(*completed, nullptr, false);
        if (parsed.is_object() && parsed.contains("text") && parsed["text"].is_string()) {
            const std::string& reply = parsed["text"].get_ref<const std::string&>();
            if (claims_human(reply) || invented_number(reply, event.dump())) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "回答超出了给定的事实"});
            }
            json context = json{{"proposer", open_id}, {"waiting_since_epoch", epoch_of(clock_)}};
            const std::expected<std::filesystem::path, Error> begun =
                begin_interaction(config_.data_root, open_id, message_id, context);
            static_cast<void>(begun);
            return json{{"text", reply}};
        }
        json context = json{{"proposer", open_id}, {"waiting_since_epoch", epoch_of(clock_)}};
        const std::expected<std::filesystem::path, Error> begun =
            begin_interaction(config_.data_root, open_id, message_id, context);
        static_cast<void>(begun);
        return json::object();
    }

    if (kind == "p2p_message") {
        const std::string allowed = event.dump();
        PortModel model(model_, "member_reply", nullptr);
        const std::expected<MemberReplyResult, Error> replied =
            run_member_reply(allowed, compose_prompt(config_, "member_reply"), model);
        if (!replied.has_value()) {
            return std::unexpected(replied.error());
        }
        if (claims_human(replied->text) || invented_number(replied->text, allowed)) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "回答超出了给定的事实"});
        }
        return json{{"text", replied->text}};
    }
    return std::unexpected(Error{ErrorCode::kUnknownEvent, "不认识的事件"});
}

}  // namespace robot_pm
