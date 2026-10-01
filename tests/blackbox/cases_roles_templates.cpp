#include "support.hpp"
// Assertions below are unchanged.

namespace bb {
namespace {

json role_submit(std::string open_id, std::string role) {
    return json{
            {"kind", "card_callback"},
            {"action", "submit_role"},
            {"open_id", "ou_owner"},
            {"group_id", "oc_group"},
            {"form", json{{"open_id", std::move(open_id)}, {"role", std::move(role)}}},
            {"mentions_bot", false},
            {"message_id", "role-1"}};
}

}  // namespace

BB_TEST_CASE("role.missing_table_sends_collection_card_only") {
    Fixture fixture;
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
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"状态", "todo"},
            {"职责", "接口"},
            {"类型", "milestone"},
            {"开始", "2026-10-01"}}});
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"today", "2026-10-01"}}));
    CHECK(fixture.feishu.events.empty());
    REQUIRE_FALSE(fixture.feishu.sent.empty());
    const auto card = fixture.feishu.sent.back().dump();
    CHECK(card.find("人员") != std::string::npos);
    CHECK(card.find("职责") != std::string::npos);
    CHECK(card.find("同意") == std::string::npos);
}

BB_TEST_CASE("role.submit_missing_open_id_writes_nothing") {
    Fixture fixture;
    expect_ok(fixture.app->handle_event(role_submit("", "接口")));
    CHECK(fixture.bitable.role_rows.empty());
}

BB_TEST_CASE("role.submit_missing_role_writes_nothing") {
    Fixture fixture;
    expect_ok(fixture.app->handle_event(role_submit("ou_owner", "")));
    CHECK(fixture.bitable.role_rows.empty());
}

BB_TEST_CASE("role.complete_row_limits_attendees") {
    Fixture fixture;
    expect_ok(fixture.app->handle_event(role_submit("ou_owner", "接口")));
    REQUIRE(fixture.bitable.role_rows.size() == 1);
    CHECK(fixture.bitable.role_rows[0].at("群id") == "oc_group");
    CHECK(fixture.bitable.role_rows[0].at("职责") == "接口");
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"状态", "todo"},
            {"职责", "接口"},
            {"类型", "milestone"},
            {"开始", "2026-10-01"},
            {"结束", "2026-10-03"}}});
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
            {"action", "同意"},
            {"open_id", "ou_owner"},
            {"interaction_id", "tm1"},
            {"mentions_bot", false}}));
    REQUIRE(fixture.feishu.events.size() == 1);
    const auto attendees = fixture.feishu.events[0].at("attendees").dump();
    CHECK(attendees.find("ou_owner") != std::string::npos);
    CHECK(attendees.find("ou_stranger") == std::string::npos);
    CHECK(attendees.find("ou_other") == std::string::npos);
}

BB_TEST_CASE("template.gantt_missing_core_field_writes_nothing") {
    Fixture fixture("进度甘特");
    fixture.bitable.fields.erase(fixture.bitable.fields.begin() + 1);
    auto result = fixture.app->write_edits(json{{"rows", json::array({json{
                                                          {"业务id", "w1"},
                                                          {"标题", "接飞书"},
                                                          {"层级", "item"},
                                                          {"类型", "work"},
                                                          {"开始", "2026-10-01"},
                                                          {"结束", "2026-10-03"},
                                                          {"职责", "接口"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("template.chase_missing_core_field_writes_nothing") {
    Fixture fixture("盯人待办");
    fixture.bitable.fields.erase(fixture.bitable.fields.begin());
    auto result = fixture.app->write_edits(
            json{{"rows", json::array({json{{"业务id", "w1"}, {"判断", "卡住"}, {"进度", 0}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("template.meeting_missing_core_field_writes_nothing") {
    Fixture fixture("会议决策");
    fixture.bitable.fields.erase(fixture.bitable.fields.begin() + 5);
    auto result = fixture.app->write_edits(
            json{{"rows", json::array({json{{"业务id", "w1"}, {"决定", "未决"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("template.parent_link_to_missing_business_id_writes_nothing") {
    Fixture fixture;
    auto result = fixture.app->write_edits(json{{"rows", json::array({json{
                                                          {"业务id", "w1"},
                                                          {"标题", "接飞书"},
                                                          {"层级", "item"},
                                                          {"父记录", json::array({"missing"})},
                                                          {"类型", "work"},
                                                          {"开始", "2026-10-01"},
                                                          {"结束", "2026-10-03"},
                                                          {"职责", "接口"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.records.empty());
}

BB_TEST_CASE("template.chase_writes_judgment_and_progress_together") {
    Fixture fixture("盯人待办");
    json plan = json::array({json{
            {"id", "w1"},
            {"title", "甲"},
            {"kind", "work"},
            {"status", "blocked"},
            {"start", "2026-10-01"},
            {"end", "2026-10-03"},
            {"owner_role", "接口"}}});
    expect_ok(fixture.app->watch(plan, plan, "2026-10-02"));
    REQUIRE(fixture.bitable.upsert_calls == 1);
    REQUIRE_FALSE(fixture.bitable.records.empty());
    CHECK(fixture.bitable.records[0].at("判断") == "卡住");
    CHECK(fixture.bitable.records[0].at("进度") == 0);
}

BB_TEST_CASE("template.meeting_has_at_most_one_recommendation") {
    Fixture fixture("会议决策");
    fixture.bitable.records = json::array({
            json{{"业务id", "w1"}, {"推荐", true}, {"决定", "未决"}},
            json{{"业务id", "w2"}, {"推荐", false}, {"决定", "未决"}},
    });
    auto result = fixture.app->write_edits(json{{"rows", json::array({
                                                          json{{"业务id", "w2"}, {"推荐", true}},
                                                  })}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.records[0].at("推荐") == true);
    CHECK(fixture.bitable.records[1].at("推荐") == false);
}

BB_TEST_CASE("template.decision_stays_undecided_until_a_button") {
    Fixture fixture("会议决策");
    fixture.bitable.records = json::array({json{{"业务id", "w1"}, {"决定", "未决"}}});
    fixture.clock.current += std::chrono::hours{2};
    expect_ok(fixture.app->sweep());
    CHECK(fixture.bitable.records[0].at("决定") == "未决");
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("template.role_table_rejects_a_missing_part") {
    Fixture fixture;
    auto result = fixture.app->write_edits(json{
            {"table", "职责"},
            {"rows", json::array({json{{"群id", "oc_group"}, {"人员", "ou_owner"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.role_rows.empty());
}

BB_TEST_CASE("upload.missing_field_is_created_then_records_upload") {
    Fixture fixture;
    fixture.bitable.fields.erase(
            std::remove_if(
                    fixture.bitable.fields.begin(),
                    fixture.bitable.fields.end(),
                    [](const json& field) { return field.at("field_name") == "职责"; }),
            fixture.bitable.fields.end());
    const auto lists_before = fixture.bitable.list_fields_calls;
    expect_ok(fixture.app->write_edits(json{{"rows", json::array({json{
                                                          {"业务id", "w1"},
                                                          {"标题", "接飞书"},
                                                          {"层级", "item"},
                                                          {"类型", "work"},
                                                          {"开始", "2026-10-01"},
                                                          {"结束", "2026-10-03"},
                                                          {"职责", "接口"}}})}}));
    CHECK(fixture.bitable.create_field_calls >= 1);
    CHECK(fixture.bitable.list_fields_calls > lists_before);
    CHECK(fixture.bitable.upsert_calls == 1);
}

BB_TEST_CASE("upload.type_mismatch_does_not_modify_or_upload") {
    Fixture fixture;
    for (auto& field : fixture.bitable.fields) {
        if (field.at("field_name") == "标题") {
            field["type"] = 2;
            field["ui_type"] = "Number";
        }
    }
    auto result = fixture.app->write_edits(
            json{{"rows", json::array({json{{"业务id", "w1"}, {"标题", "接飞书"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.create_field_calls == 0);
    CHECK(fixture.bitable.upsert_calls == 0);
    CHECK(fixture.model.calls.empty());
}

BB_TEST_CASE("upload.gantt_nondate_writes_nothing") {
    Fixture fixture("进度甘特");
    for (auto& field : fixture.bitable.fields) {
        if (field.at("field_name") == "开始") {
            field["type"] = 1;
            field["ui_type"] = "Text";
        }
    }
    auto result = fixture.app->write_edits(json{{"rows", json::array({json{
                                                          {"业务id", "w1"},
                                                          {"开始", "2026-10-01"},
                                                          {"结束", "2026-10-03"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.upsert_calls == 0);
}

}  // namespace bb
