#include "robot_pm/meeting_plan.hpp"

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

class RecordingCalendar final : public robot_pm::CalendarBook {
public:
    int calls{0};

    std::expected<std::string, robot_pm::Error> create_event(const nlohmann::json& event) override {
        static_cast<void>(event);
        ++calls;
        return "evt_1";
    }
};

const nlohmann::json kRoles = nlohmann::json::array({{{"open_id", "ou_b"}, {"role", "接口"}}});

std::string meeting_output(const char* end) {
    return std::string(
               R"({"meetings":[{"item_id":"rel","title":"发布前会","attendee_roles":["接口"],"start":"2026-10-07 10:00","end":")") +
           end + R"("}],"todos":[],"ai_recommended_item_id":"rel"})";
}

}  // namespace

TEST_CASE("scenario.meeting_books_only_after_group_agrees") {
    // 失败：提出人点确认后日历里已有这场会；先不办也创建了日程；群里同意却没有日程。
    ScriptedAct model;
    model.stdout_text =
        R"({"meetings":[{"item_id":"w2","title":"发布前会","attendee_roles":["接口"],"start":"2026-10-03 10:00","end":"2026-10-03 11:00"}],"todos":[],"ai_recommended_item_id":"w2"})";
    RecordingCalendar calendar;
    const auto proposed =
        robot_pm::propose_meeting_plan(R"({"today":"2026-10-01"})", "meet\n", "ou_a", kRoles, model);
    REQUIRE(proposed.has_value());
    CHECK(model.calls == 1);
    CHECK_FALSE(proposed->meeting_created);
    CHECK(calendar.calls == 0);

    const auto confirmed = robot_pm::confirm_meeting_plan("ou_a", "接口", "确认", proposed->plan);
    REQUIRE(confirmed.has_value());
    CHECK(confirmed->group_card_sent);
    CHECK_FALSE(confirmed->meeting_created);
    CHECK(calendar.calls == 0);

    const auto deferred = robot_pm::answer_group_meeting("先不办", confirmed->plan, calendar);
    REQUIRE(deferred.has_value());
    CHECK_FALSE(deferred->meeting_created);
    CHECK(deferred->plan.at("decision") == "先不办");
    CHECK(calendar.calls == 0);

    const auto booked = robot_pm::answer_group_meeting("同意", confirmed->plan, calendar);
    REQUIRE(booked.has_value());
    CHECK(booked->meeting_created);
    CHECK(calendar.calls == 1);
}

TEST_CASE("scenario.release_node_rejects_meeting_that_does_not_end_before_it") {
    // 失败：结束不早于发布节点的会议被收下。
    const char* payload = R"({
        "today": "2026-10-01",
        "rows": [{
            "id": "rel",
            "node": "release",
            "end": "2026-10-08"
        }]
    })";

    ScriptedAct early;
    early.stdout_text = meeting_output("2026-10-07 11:00");
    const auto before = robot_pm::propose_meeting_plan(payload, "meet\n", "ou_a", kRoles, early);
    REQUIRE(before.has_value());
    CHECK_FALSE(before->group_card_sent);
    CHECK_FALSE(before->meeting_created);
    CHECK(before->plan.at("meetings").size() == 1);

    for (const char* end : {"2026-10-08 09:00", "2026-10-09 10:00"}) {
        ScriptedAct late;
        late.stdout_text = meeting_output(end);
        const auto rejected = robot_pm::propose_meeting_plan(payload, "meet\n", "ou_a", kRoles, late);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == robot_pm::ErrorCode::kEditRejected);
        CHECK(late.calls == 1);
    }
}
