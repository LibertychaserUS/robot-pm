#pragma once

#include "robot_pm/app.hpp"

#include <doctest/doctest.h>

// The case body keeps its assertions and runs against robot_pm::App.
#define BB_TEST_CASE(name) TEST_CASE(name)

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bb {
namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_tp = std::chrono::system_clock::time_point;

inline clock_tp at_utc(
        int year,
        int month,
        int day,
        int hour,
        int minute,
        int second = 0) {
    using namespace std::chrono;
    return sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                    std::chrono::day{static_cast<unsigned>(day)}} +
           hours{hour} + minutes{minute} + seconds{second};
}

// 2026-10-01 00:30 in Asia/Shanghai.
inline clock_tp beijing_0010_0030() {
    return at_utc(2026, 9, 30, 16, 30, 0);
}

inline void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

inline std::vector<json> read_jsonl(const fs::path& path) {
    std::vector<json> rows;
    if (!fs::exists(path)) {
        return rows;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            rows.push_back(json::parse(line));
        }
    }
    return rows;
}

inline json item(
        std::string id,
        std::string title,
        std::string kind,
        std::string status,
        std::string start,
        std::string end,
        json predecessors,
        std::string owner_role,
        std::string source_quote) {
    return json{
            {"id", std::move(id)},
            {"title", std::move(title)},
            {"kind", std::move(kind)},
            {"status", std::move(status)},
            {"start", std::move(start)},
            {"end", std::move(end)},
            {"predecessors", std::move(predecessors)},
            {"owner_role", std::move(owner_role)},
            {"source_quote", std::move(source_quote)}};
}

inline json manifest_with(json documents) {
    return json{{"schema_version", 1}, {"documents", std::move(documents)}};
}

inline json one_work_manifest() {
    auto work = item(
            "w1",
            "接飞书",
            "work",
            "todo",
            "2026-10-01",
            "2026-10-03",
            json::array(),
            "接口",
            "接飞书");
    json section = {{"id", "sec1"}, {"title", "范围"}, {"items", json::array({work})}};
    json document = {
            {"id", "doc1"},
            {"type", "prd"},
            {"title", "规格"},
            {"sections", json::array({section})}};
    return manifest_with(json::array({document}));
}

inline json core_field(std::string name, int type, std::string ui, json options = json::array()) {
    json field = {{"field_name", std::move(name)}, {"type", type}, {"ui_type", std::move(ui)}};
    if (!options.empty()) {
        field["options"] = std::move(options);
    }
    return field;
}

inline json core_fields() {
    return json::array({
            core_field("业务id", 1, "Text"),
            core_field("标题", 1, "Text"),
            core_field("层级", 3, "SingleSelect", json::array({"document", "section", "item"})),
            core_field("父记录", 18, "SingleLink"),
            core_field("类型", 3, "SingleSelect", json::array({"milestone", "work"})),
            core_field("状态", 3, "SingleSelect", json::array({"todo", "doing", "done", "blocked"})),
            core_field("开始", 5, "DateTime"),
            core_field("结束", 5, "DateTime"),
            core_field("前置", 18, "SingleLink"),
            core_field("职责", 1, "Text"),
            core_field("进度", 2, "Number"),
    });
}

inline json chase_fields() {
    auto fields = core_fields();
    fields.push_back(core_field(
            "判断", 3, "SingleSelect", json::array({"完成", "卡住", "超期", "还没开始", "正常"})));
    return fields;
}

inline json meeting_fields() {
    auto fields = core_fields();
    fields.push_back(core_field("推荐", 7, "Checkbox"));
    fields.push_back(core_field("会议时间", 5, "DateTime"));
    fields.push_back(
            core_field("决定", 3, "SingleSelect", json::array({"未决", "同意", "先不办"})));
    fields.push_back(core_field("待办", 1, "Text"));
    return fields;
}

inline json role_fields() {
    return json::array({
            core_field("群id", 1, "Text"),
            core_field("人员", 11, "User"),
            core_field("职责", 1, "Text"),
    });
}

struct RecordedPrompt {
    std::string step;
    std::string system_prompt;
    std::string user_message;
};

class FakeClock final : public robot_pm::Clock {
public:
    clock_tp current = beijing_0010_0030();

    clock_tp now() const override { return current; }
};

class FakeModel final : public robot_pm::Model {
public:
    std::vector<RecordedPrompt> calls;
    std::string response = "{}";
    bool fail = false;

    std::expected<std::string, robot_pm::Error> complete(
            std::string_view step,
            std::string_view system_prompt,
            std::string_view user_message) override {
        calls.push_back(RecordedPrompt{
                std::string(step), std::string(system_prompt), std::string(user_message)});
        if (fail) {
            return std::unexpected(robot_pm::Error{
                    robot_pm::ErrorCode::kBridgeFailed, "model failed"});
        }
        return response;
    }
};

class FakeFeishu final : public robot_pm::FeishuPort {
public:
    std::vector<json> sent;
    std::vector<json> events;
    std::vector<std::string> deleted_events;
    std::string primary_id = "primary-cal";
    int primary_calls = 0;
    bool fail_create = false;

    std::expected<json, robot_pm::Error> send(const json& message) override {
        sent.push_back(message);
        return json{{"message_id", "om_sent"}};
    }

    std::expected<std::string, robot_pm::Error> primary_calendar_id() override {
        ++primary_calls;
        return primary_id;
    }

    std::expected<json, robot_pm::Error> create_calendar_event(const json& event) override {
        if (fail_create) {
            return std::unexpected(
                    robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "calendar create failed"});
        }
        events.push_back(event);
        return json{{"event_id", "evt-1"}};
    }

    std::expected<void, robot_pm::Error> delete_calendar_event(
            std::string_view calendar_id,
            std::string_view event_id) override {
        deleted_events.push_back(std::string(calendar_id) + "/" + std::string(event_id));
        events.clear();
        return {};
    }
};

class FakeBitable final : public robot_pm::BitablePort {
public:
    json fields = core_fields();
    json role_table_fields = role_fields();
    std::vector<json> records;
    std::vector<json> role_rows;
    int list_fields_calls = 0;
    int upsert_calls = 0;
    int create_field_calls = 0;
    bool partial_batch = false;
    bool undo_fails = false;
    bool reject_upsert = false;
    std::vector<json> snapshot;

    std::expected<json, robot_pm::Error> list_fields(std::string_view table) override {
        ++list_fields_calls;
        if (table == "职责") {
            return role_table_fields;
        }
        return fields;
    }

    std::expected<void, robot_pm::Error> create_field(
            std::string_view table,
            const json& field) override {
        ++create_field_calls;
        if (table == "职责") {
            role_table_fields.push_back(field);
        } else {
            fields.push_back(field);
        }
        return {};
    }

    std::expected<json, robot_pm::Error> list_records(std::string_view table) override {
        if (table == "职责") {
            return role_rows;
        }
        return records;
    }

    std::expected<json, robot_pm::Error> upsert(
            std::string_view table,
            const json& incoming) override {
        ++upsert_calls;
        snapshot = table == "职责" ? role_rows : records;
        if (reject_upsert) {
            return std::unexpected(
                    robot_pm::Error{robot_pm::ErrorCode::kEditRejected, "upsert rejected"});
        }
        auto& dest = table == "职责" ? role_rows : records;
        if (partial_batch) {
            if (!incoming.empty()) {
                dest.push_back(incoming.front());
            }
            return std::unexpected(
                    robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "partial batch"});
        }
        for (const auto& row : incoming) {
            const std::string key = row.value("业务id", row.value("群id", ""));
            bool replaced = false;
            for (auto& existing : dest) {
                const std::string existing_key =
                        existing.value("业务id", existing.value("群id", ""));
                if (!key.empty() && existing_key == key) {
                    for (auto it = row.begin(); it != row.end(); ++it) {
                        existing[it.key()] = it.value();
                    }
                    replaced = true;
                }
            }
            if (!replaced) {
                dest.push_back(row);
            }
        }
        return json{{"written", incoming.size()}};
    }

    std::expected<void, robot_pm::Error> undo(const json&) override {
        if (undo_fails) {
            return std::unexpected(
                    robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "undo failed"});
        }
        records = snapshot;
        return {};
    }
};

class FakeProcess final : public robot_pm::ProcessRunner {
public:
    int exit_code = 0;
    std::string stdout_text = "{}";
    std::string stderr_text;
    int calls = 0;

    std::expected<robot_pm::ProcessResult, robot_pm::Error> run(
            const robot_pm::ProcessRequest&) override {
        ++calls;
        robot_pm::ProcessResult result;
        result.exit_code = exit_code;
        result.stdout_text = stdout_text;
        result.stderr_text = stderr_text;
        return result;
    }
};

struct Fixture {
    fs::path root;
    FakeClock clock;
    FakeModel model;
    FakeFeishu feishu;
    FakeBitable bitable;
    FakeProcess process;
    robot_pm::Config config;
    std::unique_ptr<robot_pm::App> app;

    explicit Fixture(std::string template_name = "进度甘特") {
        root = fs::temp_directory_path() / "robot-pm-blackbox" /
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(root / "var/robot_pm/inbox");
        fs::create_directories(root / "var/robot_pm/semantic/manifests");
        fs::create_directories(root / "var/robot_pm/semantic/supervision");
        fs::create_directories(root / "var/robot_pm/episodic");
        fs::create_directories(root / "var/robot_pm/working/plans");
        fs::create_directories(root / "prompts");
        fs::create_directories(root / "docs/sop");
        write_text(root / "prompts/identity.md", "你是 robot PM。不声称自己是某个真人。\n");
        write_text(root / "docs/sop.md", "我是 robot PM。\n");
        config.data_root = root / "var/robot_pm";
        config.repo_root = root;
        config.bot_open_id = "ou_bot";
        config.app_id = "cli_example";
        config.app_secret = "super-secret-value-xyz";
        config.bitable_app_token = "bascnEXAMPLE";
        config.template_name = std::move(template_name);
        config.group_id = "oc_group";
        if (config.template_name == "盯人待办") {
            bitable.fields = chase_fields();
        } else if (config.template_name == "会议决策") {
            bitable.fields = meeting_fields();
        }
        open();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    void open() {
        robot_pm::Ports ports{model, feishu, bitable, process, clock};
        app = std::make_unique<robot_pm::App>(config, ports);
    }

    void reopen() { open(); }

    std::vector<json> audit() const {
        return read_jsonl(config.data_root / "episodic/audit.jsonl");
    }

    std::vector<json> events() const {
        return read_jsonl(config.data_root / "episodic/events.jsonl");
    }
};

inline json group_message(
        std::string open_id,
        std::string text,
        bool mentions_bot,
        std::string message_id = "m1") {
    return json{
            {"kind", "group_message"},
            {"chat_id", "oc_group"},
            {"open_id", std::move(open_id)},
            {"text", std::move(text)},
            {"mentions_bot", mentions_bot},
            {"message_id", std::move(message_id)},
            {"members", json::array({"ou_owner", "ou_other", "ou_stranger"})}};
}

inline void expect_ok(const std::expected<json, robot_pm::Error>& result) {
    if (!result) {
        INFO(result.error().message);
    }
    REQUIRE(result.has_value());
}

inline void expect_code(
        const std::expected<json, robot_pm::Error>& result,
        robot_pm::ErrorCode code) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == code);
}

inline std::vector<fs::path> child_dirs(const fs::path& directory) {
    std::vector<fs::path> found;
    if (!fs::exists(directory)) {
        return found;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_directory()) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

inline fs::path interaction_dir(
        const Fixture& fixture,
        std::string_view open_id,
        std::string_view interaction_id) {
    return fixture.config.data_root / "working" / open_id / interaction_id;
}

inline void expect_secret_hidden(std::string_view message) {
    CHECK(message.find("super-secret-value-xyz") == std::string_view::npos);
    CHECK(message.find("cli_example") == std::string_view::npos);
    CHECK(message.find("bascnEXAMPLE") == std::string_view::npos);
}

}  // namespace bb
