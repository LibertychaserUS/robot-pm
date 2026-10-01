#include "support.hpp"
// Assertions below are unchanged.

namespace bb {
namespace {

[[maybe_unused]] json due_item() {
    return json{
            {"id", "w1"},
            {"title", "接飞书"},
            {"kind", "milestone"},
            {"status", "todo"},
            {"start", "2026-10-01"},
            {"end", "2026-10-03"},
            {"owner_role", "接口"},
            {"meet", "at_start"},
            {"source_quote", "10:00 开会"}};
}

void seed(Fixture& fixture) {
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"层级", "item"},
            {"类型", "milestone"},
            {"状态", "todo"},
            {"开始", "2026-10-01"},
            {"结束", "2026-10-03"},
            {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
}

}  // namespace

BB_TEST_CASE("delivery.hold_does_not_create_a_calendar_event") {
    Fixture fixture;
    seed(fixture);
    CHECK(fixture.config.delivery.empty());
    fixture.model.response = json{
            {"meetings",
             json::array({json{
                     {"item_id", "w1"},
                     {"title", "接飞书"},
                     {"agenda", "开始"},
                     {"start", "2026-10-01 10:00"},
                     {"end", "2026-10-01 11:00"},
                     {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array({json{{"item_id", "w1"}, {"title", "接飞书"}}})},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    CHECK(fixture.feishu.events.empty());
    REQUIRE_FALSE(fixture.feishu.sent.empty());
    const auto card = fixture.feishu.sent.back().dump();
    CHECK(card.find("AI推荐会议时间为2026-10-01 10:00（北京时间）") != std::string::npos);
    CHECK(card.find("同意") != std::string::npos);
    CHECK(card.find("先不办") != std::string::npos);
    CHECK(card.find("ou_owner") != std::string::npos);
}

BB_TEST_CASE("delivery.nobody_clicks_creates_no_meeting") {
    Fixture fixture;
    seed(fixture);
    fixture.model.response = json{
            {"meetings", json::array({json{
                                 {"item_id", "w1"},
                                 {"title", "接飞书"},
                                 {"agenda", "开始"},
                                 {"start", "2026-10-01 10:00"},
                                 {"end", "2026-10-01 11:00"},
                                 {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    fixture.clock.current += std::chrono::hours{1};
    expect_ok(fixture.app->sweep());
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("delivery.agree_creates_one_meeting_not_a_second") {
    Fixture fixture;
    seed(fixture);
    fixture.config.calendar_id = "cal-explicit";
    fixture.reopen();
    fixture.model.response = json{
            {"meetings", json::array({json{
                                 {"item_id", "w1"},
                                 {"title", "接飞书"},
                                 {"agenda", "开始"},
                                 {"start", "2026-10-01 10:00"},
                                 {"end", "2026-10-01 11:00"},
                                 {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    CHECK(fixture.feishu.events.empty());
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "同意"},
            {"open_id", "ou_owner"},
            {"interaction_id", "tm1"},
            {"mentions_bot", false}}));
    CHECK(fixture.feishu.events.size() == 1);
    CHECK(fixture.feishu.events[0].at("start").at("timezone") == "Asia/Shanghai");
    CHECK(fixture.feishu.events[0].at("end").at("timezone") == "Asia/Shanghai");
    CHECK(fixture.feishu.primary_calls == 0);
}

BB_TEST_CASE("delivery.defer_records_decision_and_creates_no_meeting") {
    Fixture fixture;
    seed(fixture);
    fixture.model.response = json{
            {"meetings", json::array({json{
                                 {"item_id", "w1"},
                                 {"title", "接飞书"},
                                 {"agenda", "开始"},
                                 {"start", "2026-10-01 10:00"},
                                 {"end", "2026-10-01 11:00"},
                                 {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "先不办"},
            {"open_id", "ou_owner"},
            {"interaction_id", "tm1"},
            {"mentions_bot", false}}));
    CHECK(fixture.feishu.events.empty());
    bool deferred = false;
    for (const auto& row : fixture.bitable.records) {
        if (row.value("业务id", "") == "w1") {
            CHECK(row.at("决定") == "先不办");
            deferred = true;
        }
    }
    CHECK(deferred);
}

BB_TEST_CASE("delivery.missing_calendar_id_uses_primary") {
    Fixture fixture;
    seed(fixture);
    CHECK(fixture.config.calendar_id.empty());
    CHECK(fixture.config.timezone.empty());
    fixture.model.response = json{
            {"action", "reserve"},
            {"item_id", "w1"},
            {"start", "2026-10-02 15:00"},
            {"end", "2026-10-02 16:00"},
            {"title", "接飞书"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "预定 w1 2026-10-02 15:00 到 16:00", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fixture.feishu.primary_calls == 1);
    REQUIRE(fixture.feishu.events.size() == 1);
    CHECK(fixture.feishu.events[0].at("calendar_id") == "primary-cal");
    CHECK(fixture.feishu.events[0].at("start").at("timezone") == "Asia/Shanghai");
    CHECK(fixture.feishu.events[0].at("end").at("timezone") == "Asia/Shanghai");
}

BB_TEST_CASE("delivery.missing_attendee_creates_no_event") {
    Fixture fixture;
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"状态", "todo"},
            {"职责", "接口"}}});
    fixture.model.response = json{
            {"action", "reserve"},
            {"item_id", "w1"},
            {"start", "2026-10-02 10:00"},
            {"end", "2026-10-02 11:00"},
            {"title", "接飞书"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "预定 w1 2026-10-02", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("delivery.missing_confirm_creates_no_event") {
    Fixture fixture;
    seed(fixture);
    fixture.model.response = json{
            {"action", "reserve"},
            {"item_id", "w1"},
            {"start", "2026-10-02 10:00"},
            {"end", "2026-10-02 11:00"},
            {"title", "接飞书"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "预定 w1 2026-10-02", true, "m1")));
    CHECK(fixture.feishu.events.empty());
    CHECK(fixture.feishu.primary_calls == 0);
}

BB_TEST_CASE("delivery.no_group_id_sends_no_card") {
    Fixture fixture;
    seed(fixture);
    fixture.config.group_id.clear();
    fixture.reopen();
    fixture.model.response = json{
            {"meetings", json::array({json{
                                 {"item_id", "w1"},
                                 {"title", "接飞书"},
                                 {"agenda", "开始"},
                                 {"start", "2026-10-01 10:00"},
                                 {"end", "2026-10-01 11:00"},
                                 {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    CHECK(fixture.feishu.sent.empty());
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("delivery.midnight_beijing_not_utc") {
    Fixture fixture;
    fixture.clock.current = beijing_0010_0030();
    fixture.reopen();
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"层级", "item"},
            {"类型", "milestone"},
            {"状态", "todo"},
            {"开始", "2026-10-01"},
            {"结束", "2026-10-02"},
            {"职责", "接口"},
            {"node", "release"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response = json{
            {"meetings", json::array({json{
                                 {"item_id", "w1"},
                                 {"title", "接飞书"},
                                 {"agenda", "凌晨"},
                                 {"start", "2026-10-01 00:30"},
                                 {"end", "2026-10-01 01:00"},
                                 {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    auto result = fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}});
    expect_ok(result);
    CHECK(result->at("ai_recommended_item_id") == "w1");
    REQUIRE_FALSE(fixture.feishu.sent.empty());
    CHECK(fixture.feishu.sent.back().dump().find("2026-10-01 00:30") != std::string::npos);
    CHECK(fixture.feishu.sent.back().dump().find("2026-09-30 16:30") == std::string::npos);
}

BB_TEST_CASE("chase.does_not_emit_a_prefix") {
    Fixture fixture;
    fixture.model.response = json::array({json{
            {"id", "w1"}, {"owner_role", "接口"}, {"text", "接口 甲 卡住"}}})
                                      .dump();
    auto result = fixture.app->handle_event(json{
            {"kind", "command"},
            {"name", "chase"},
            {"open_id", "ou_owner"},
            {"chase",
             json::array({
                     json{{"id", "w1"},
                          {"title", "甲"},
                          {"judgment", "卡住"},
                          {"owner_role", "接口"}},
                     json{{"id", "w2"},
                          {"title", "乙"},
                          {"judgment", "超期"},
                          {"owner_role", "开发"}},
             })}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.feishu.sent.empty());
}

}  // namespace bb
