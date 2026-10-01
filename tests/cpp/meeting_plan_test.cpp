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
        ++calls;
        CHECK(event.at("timezone") == "Asia/Shanghai");
        return "evt_1";
    }
};

const nlohmann::json kRoles = nlohmann::json::array({{{"open_id", "ou_b"}, {"role", "接口"}}});

}  // namespace

TEST_CASE("meeting.group_card_is_not_sent_before_proposer_confirms") {
    // 失败：提出人同意前 group_card_sent 为真，或这时已经创建日程。
    ScriptedAct model;
    model.stdout_text =
        R"({"meetings":[{"item_id":"w3","title":"发布前会","agenda":"看发布","attendee_roles":["接口"],"start":"2026-10-02 10:00"}],"todos":[],"ai_recommended_item_id":"w3"})";
    RecordingCalendar calendar;

    const auto proposed = robot_pm::propose_meeting_plan(R"({"today":"2026-10-01"})", "prompt\n", "ou_a", kRoles, model);

    REQUIRE(proposed.has_value());
    CHECK(model.calls == 1);
    CHECK(proposed->proposer_card_sent);
    CHECK_FALSE(proposed->group_card_sent);
    CHECK_FALSE(proposed->meeting_created);
    CHECK(calendar.calls == 0);
    CHECK(proposed->group_card.is_null());

    const auto declined = robot_pm::confirm_meeting_plan("ou_a", "接口", "取消", proposed->plan);
    REQUIRE(declined.has_value());
    CHECK_FALSE(declined->group_card_sent);
    CHECK_FALSE(declined->meeting_created);
    CHECK(calendar.calls == 0);

    const auto too_early = robot_pm::answer_group_meeting("同意", proposed->plan, calendar);
    REQUIRE_FALSE(too_early.has_value());
    CHECK(calendar.calls == 0);

    const auto confirmed = robot_pm::confirm_meeting_plan("ou_a", "接口", "同意", proposed->plan);
    REQUIRE(confirmed.has_value());
    CHECK(confirmed->group_card_sent);
    CHECK_FALSE(confirmed->meeting_created);
    CHECK(calendar.calls == 0);
    CHECK(confirmed->group_card.at("msg_type") == "interactive");
    CHECK(confirmed->group_card.dump().find("先不办") != std::string::npos);

    const auto skipped = robot_pm::answer_group_meeting("先不办", confirmed->plan, calendar);
    REQUIRE(skipped.has_value());
    CHECK_FALSE(skipped->meeting_created);
    CHECK(calendar.calls == 0);

    const auto booked = robot_pm::answer_group_meeting("同意", confirmed->plan, calendar);
    REQUIRE(booked.has_value());
    CHECK(booked->meeting_created);
    CHECK(calendar.calls == 1);
}

TEST_CASE("meeting.other_person_cannot_release_the_group_card") {
    ScriptedAct model;
    model.stdout_text =
        R"({"meetings":[{"item_id":"w3","title":"发布前会","attendee_roles":["接口"],"start":"2026-10-02 10:00"}],"todos":[],"ai_recommended_item_id":"w3"})";
    const auto proposed = robot_pm::propose_meeting_plan("{}", "prompt\n", "ou_a", kRoles, model);
    REQUIRE(proposed.has_value());
    const auto confirmed = robot_pm::confirm_meeting_plan("ou_b", "接口", "同意", proposed->plan);
    REQUIRE_FALSE(confirmed.has_value());
    CHECK(confirmed.error().code == robot_pm::ErrorCode::kForbidden);
}
