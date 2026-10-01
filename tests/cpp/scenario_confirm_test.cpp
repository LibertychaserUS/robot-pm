#include "robot_pm/meeting_plan.hpp"
#include "robot_pm/status_update.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

class ScriptedAct final : public robot_pm::ModelAct {
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

class RecordingLedger final : public robot_pm::StatusLedger {
public:
    int calls{0};
    nlohmann::json last = nullptr;

    std::expected<void, robot_pm::Error> update_item(const nlohmann::json& fields) override {
        ++calls;
        last = fields;
        return {};
    }
};

class RecordingCalendar final : public robot_pm::CalendarBook {
public:
    int calls{0};

    std::expected<std::string, robot_pm::Error> create_event(const nlohmann::json& event) override {
        static_cast<void>(event);
        ++calls;
        return "evt_1";
    }
};

}  // namespace

TEST_CASE("scenario.confirm_gates_fail_if_table_or_group_card_moves_early") {
    // 失败：小节点日期在同意前写入；deadline 日期被改；会议卡片在提出人确认前发到群里；群里同意前就建了日程。
    const nlohmann::json rows = nlohmann::json::array(
        {{{"id", "w1"},
          {"owner_role", "接口"},
          {"node", "flexible"},
          {"status", "todo"},
          {"start", "2026-10-01"},
          {"end", "2026-10-03"}},
         {{"id", "w2"},
          {"owner_role", "接口"},
          {"node", "deadline"},
          {"status", "todo"},
          {"start", "2026-10-01"},
          {"end", "2026-10-08"}}});
    RecordingLedger ledger;
    ScriptedAct status_model;
    status_model.stdout_text = R"({"action":"update","item_id":"w1","start":"2026-10-02"})";
    const auto proposed = robot_pm::propose_status_update(R"({"text":"开始改到 2 日"})", "status\n", "ou_a", "接口",
                                                           rows, status_model);
    REQUIRE(proposed.has_value());
    CHECK(status_model.calls == 1);
    CHECK(ledger.calls == 0);
    CHECK(proposed->confirm_card_sent);
    const auto written = robot_pm::confirm_status_update("ou_a", "同意", proposed->plan, ledger);
    REQUIRE(written.has_value());
    CHECK(written->table_written);
    CHECK(ledger.calls == 1);
    CHECK(ledger.last.at("开始") == "2026-10-02");
    CHECK_FALSE(ledger.last.contains("标题"));
    CHECK_FALSE(ledger.last.contains("前置"));

    ScriptedAct blocked;
    blocked.stdout_text = R"({"action":"update","item_id":"w2","end":"2026-10-09"})";
    const auto deadline =
        robot_pm::propose_status_update(R"({"text":"对外节点往后一天"})", "status\n", "ou_a", "pm", rows, blocked);
    REQUIRE_FALSE(deadline.has_value());
    CHECK(ledger.calls == 1);

    ScriptedAct meeting_model;
    meeting_model.stdout_text =
        R"({"meetings":[{"item_id":"w2","title":"发布前会","attendee_roles":["接口"],"start":"2026-10-03 10:00","end":"2026-10-03 11:00"}],"todos":[],"ai_recommended_item_id":"w2"})";
    const nlohmann::json roles = nlohmann::json::array({{{"open_id", "ou_b"}, {"role", "接口"}}});
    RecordingCalendar calendar;
    const auto meeting =
        robot_pm::propose_meeting_plan(R"({"today":"2026-10-01"})", "meet\n", "ou_a", roles, meeting_model);
    REQUIRE(meeting.has_value());
    CHECK(meeting_model.calls == 1);
    CHECK_FALSE(meeting->group_card_sent);
    CHECK(calendar.calls == 0);
    const auto released = robot_pm::confirm_meeting_plan("ou_a", "接口", "同意", meeting->plan);
    REQUIRE(released.has_value());
    CHECK(released->group_card_sent);
    CHECK_FALSE(released->meeting_created);
    CHECK(calendar.calls == 0);
    const auto booked = robot_pm::answer_group_meeting("同意", released->plan, calendar);
    REQUIRE(booked.has_value());
    CHECK(booked->meeting_created);
    CHECK(calendar.calls == 1);
}
