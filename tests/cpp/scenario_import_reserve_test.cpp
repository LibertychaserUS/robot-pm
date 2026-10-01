#include "robot_pm/inbox.hpp"
#include "robot_pm/reserve.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

class CountingNotice final : public robot_pm::GroupNotice {
public:
    int posts{0};

    void post(std::string_view text) override {
        static_cast<void>(text);
        ++posts;
    }
};

class CountingImport final : public robot_pm::TextImport {
public:
    int calls{0};
    nlohmann::json result{{"kind", "text"}, {"text", "接飞书"}};

    std::expected<nlohmann::json, robot_pm::Error> read(const std::filesystem::path& path) override {
        static_cast<void>(path);
        ++calls;
        return result;
    }
};

class CountingModel final : public robot_pm::ModelAct {
public:
    int calls{0};
    std::string stdout_text;

    std::expected<robot_pm::ModelResponse, robot_pm::Error> run(
        const robot_pm::ModelRequest& request) override {
        static_cast<void>(request);
        ++calls;
        return robot_pm::ModelResponse{0, stdout_text};
    }
};

class CountingUpload final : public robot_pm::BackgroundUpload {
public:
    int calls{0};

    std::expected<void, robot_pm::Error> upload(const std::vector<nlohmann::json>& rows) override {
        static_cast<void>(rows);
        ++calls;
        return {};
    }
};

class ScriptedCalendar final : public robot_pm::ReserveCalendar {
public:
    int primary_calls{0};
    int create_calls{0};
    int delete_calls{0};
    bool create_fails{false};
    nlohmann::json last_event = nullptr;
    std::string last_deleted;

    std::expected<std::string, robot_pm::Error> primary_calendar() override {
        ++primary_calls;
        return "primary";
    }

    std::expected<std::string, robot_pm::Error> create_event(const nlohmann::json& event) override {
        ++create_calls;
        last_event = event;
        if (create_fails) {
            return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "日程没有建成"});
        }
        return "evt_1";
    }

    std::expected<void, robot_pm::Error> delete_event(std::string_view calendar_id,
                                                      std::string_view event_id) override {
        static_cast<void>(calendar_id);
        ++delete_calls;
        last_deleted = std::string(event_id);
        return {};
    }
};

class ScriptedDecision final : public robot_pm::DecisionTable {
public:
    int calls{0};
    bool fail{false};
    std::string decision;

    std::expected<void, robot_pm::Error> set_decision(std::string_view item_id,
                                                      std::string_view value) override {
        static_cast<void>(item_id);
        ++calls;
        if (fail) {
            return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "决定没有写成"});
        }
        decision = std::string(value);
        return {};
    }
};

std::filesystem::path make_root(const char* name) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "inbox");
    return root;
}

const char* kManifest = R"({
  "schema_version": 1,
  "documents": [{
    "id": "d1",
    "type": "prd",
    "title": "文档",
    "items": [{
      "id": "w1",
      "title": "接飞书",
      "kind": "work",
      "status": "todo",
      "start": "2026-10-01",
      "end": "2026-10-03",
      "predecessors": [],
      "owner_role": "接口",
      "source_quote": "接飞书"
    }]
  }]
})";

const nlohmann::json kRows = nlohmann::json::array(
    {{{"id", "w1"}, {"owner_role", "接口"}, {"title", "接飞书"}}});
const nlohmann::json kRoles = nlohmann::json::array({{{"open_id", "ou_b"}, {"role", "接口"}}});

}  // namespace

TEST_CASE("scenario.inbox_import_fails_if_json_calls_model_or_group_is_notified") {
    // 失败：合法 JSON 调用了模型或文字提取；空文字 PDF 被上传；群里出现导入通知；复查单里除了路径还有别的字。
    const auto now = std::chrono::system_clock::time_point{};
    const auto root = make_root("robot-pm-scenario-inbox");
    {
        std::ofstream manifest(root / "inbox" / "a.json");
        manifest << kManifest;
    }
    {
        std::ofstream markdown(root / "inbox" / "b.md");
        markdown << "# 接飞书\n";
    }
    {
        std::ofstream pdf(root / "inbox" / "c.pdf");
        pdf << "scan";
    }
    CountingNotice notice;
    CountingImport text;
    CountingModel model;
    model.stdout_text = kManifest;
    CountingUpload upload;

    const auto json_file = robot_pm::import_next_inbox_file(root, now, &text, &model, upload, notice);
    REQUIRE(json_file.has_value());
    CHECK(json_file->projected);
    CHECK(json_file->uploaded);
    CHECK_FALSE(json_file->group_notified);
    CHECK(model.calls == 0);
    CHECK(text.calls == 0);
    CHECK(upload.calls == 1);
    CHECK(notice.posts == 0);

    const auto markdown_file = robot_pm::import_next_inbox_file(root, now, &text, &model, upload, notice);
    REQUIRE(markdown_file.has_value());
    CHECK(markdown_file->uploaded);
    CHECK(text.calls == 1);
    CHECK(model.calls == 1);
    CHECK(upload.calls == 2);
    CHECK(notice.posts == 0);

    text.result = {{"kind", "empty_text"}};
    const auto pdf_file = robot_pm::import_next_inbox_file(root, now, &text, &model, upload, notice);
    REQUIRE(pdf_file.has_value());
    CHECK_FALSE(pdf_file->projected);
    CHECK_FALSE(pdf_file->uploaded);
    CHECK(upload.calls == 2);
    CHECK(model.calls == 1);
    CHECK(notice.posts == 0);
    const auto review = root / "semantic" / "supervision" / "c.txt";
    REQUIRE(std::filesystem::is_regular_file(review));
    std::ifstream review_input(review);
    std::ostringstream review_text;
    review_text << review_input.rdbuf();
    const std::string expected = (root / "inbox" / "c.pdf").string() + "\n";
    CHECK(review_text.str() == expected);
}

TEST_CASE("scenario.reserve_fails_if_primary_calendar_skipped_or_not_rolled_back") {
    // 失败：ROBOT_PM_CALENDAR_ID 为空时不查主日历；开始或结束的时区不是 Asia/Shanghai；创建失败却把决定写成同意；表写入失败后没有删除日程。
    CountingModel model;
    model.stdout_text =
        R"({"action":"reserve","item_id":"w1","start":"2026-10-02","end":"2026-10-02","title":"接飞书"})";
    const auto proposed =
        robot_pm::propose_reserve(R"({"text":"明天下午开会","today":"2026-10-01"})", "prompt\n", "ou_a", "接口",
                                   kRows, kRoles, model);
    REQUIRE(proposed.has_value());
    CHECK(model.calls == 1);
    CHECK(proposed->confirm_card_sent);
    CHECK_FALSE(proposed->calendar_called);
    CHECK(proposed->decision == "未决");
    CHECK(proposed->plan.at("start") == "2026-10-02 10:00");
    CHECK(proposed->plan.at("end") == "2026-10-02 11:00");

    ScriptedCalendar skipped;
    ScriptedDecision untouched;
    const auto cancelled =
        robot_pm::confirm_reserve("ou_a", "取消", "", proposed->plan, skipped, untouched);
    REQUIRE(cancelled.has_value());
    CHECK(skipped.primary_calls == 0);
    CHECK(skipped.create_calls == 0);
    CHECK(untouched.calls == 0);
    CHECK(cancelled->decision == "未决");

    ScriptedCalendar calendar;
    ScriptedDecision table;
    const auto reserved = robot_pm::confirm_reserve("ou_a", "同意", "", proposed->plan, calendar, table);
    REQUIRE(reserved.has_value());
    CHECK(calendar.primary_calls == 1);
    CHECK(calendar.create_calls == 1);
    CHECK(reserved->event.at("calendar_id") == "primary");
    CHECK_FALSE(reserved->event.contains("timezone"));
    CHECK(reserved->event.at("start").at("timezone") == "Asia/Shanghai");
    CHECK(reserved->event.at("end").at("timezone") == "Asia/Shanghai");
    CHECK(reserved->decision == "同意");
    CHECK(table.decision == "同意");

    ScriptedCalendar broken;
    broken.create_fails = true;
    ScriptedDecision still_open;
    const auto failed = robot_pm::confirm_reserve("ou_a", "同意", "", proposed->plan, broken, still_open);
    REQUIRE_FALSE(failed.has_value());
    CHECK(broken.primary_calls == 1);
    CHECK(broken.create_calls == 1);
    CHECK(still_open.calls == 0);
    CHECK(still_open.decision != "同意");
    CHECK(broken.delete_calls == 0);

    ScriptedCalendar created;
    ScriptedDecision reject_write;
    reject_write.fail = true;
    const auto rolled = robot_pm::confirm_reserve("ou_a", "同意", "cal_fixed", proposed->plan, created, reject_write);
    REQUIRE_FALSE(rolled.has_value());
    CHECK(created.primary_calls == 0);
    CHECK(created.create_calls == 1);
    CHECK(created.last_event.at("calendar_id") == "cal_fixed");
    CHECK(created.delete_calls == 1);
    CHECK(created.last_deleted == "evt_1");
    CHECK(reject_write.decision != "同意");
}
