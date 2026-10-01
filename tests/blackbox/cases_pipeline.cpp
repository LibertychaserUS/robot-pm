#include "support.hpp"
// Skipped: missing symbol robot_pm::App. Assertions below are unchanged.

namespace bb {
namespace {

json two_works() {
    return json::array({
            json{{"id", "w1"},
                 {"title", "甲"},
                 {"kind", "work"},
                 {"status", "done"},
                 {"start", "2026-10-01"},
                 {"end", "2026-10-03"},
                 {"owner_role", "接口"}},
            json{{"id", "w2"},
                 {"title", "乙"},
                 {"kind", "work"},
                 {"status", "blocked"},
                 {"start", "2026-10-01"},
                 {"end", "2026-10-03"},
                 {"owner_role", "开发"}},
    });
}

}  // namespace

BB_TEST_CASE("watch.one_of_two_done_progress_is_50") {
    Fixture fixture;
    auto result = fixture.app->watch(two_works(), two_works(), "2026-10-02");
    expect_ok(result);
    CHECK(*result == json{
            {"progress", 50},
            {"chase",
             json::array({json{
                     {"id", "w2"},
                     {"title", "乙"},
                     {"judgment", "卡住"},
                     {"owner_role", "开发"}}})}});
}

BB_TEST_CASE("watch.zero_work_items_progress_is_zero") {
    Fixture fixture;
    auto result = fixture.app->watch(json::array(), json::array(), "2026-10-01");
    expect_ok(result);
    CHECK(*result == json{{"progress", 0}, {"chase", json::array()}});
}

BB_TEST_CASE("watch.today_equal_start_is_due") {
    Fixture fixture;
    json plan = json::array({json{
            {"id", "w1"},
            {"title", "甲"},
            {"kind", "work"},
            {"status", "todo"},
            {"start", "2026-10-01"},
            {"end", "2026-10-03"},
            {"owner_role", "接口"}}});
    auto result = fixture.app->watch(plan, plan, "2026-10-01");
    expect_ok(result);
    CHECK(result->at("chase") == json::array({json{
                                         {"id", "w1"},
                                         {"title", "甲"},
                                         {"judgment", "还没开始"},
                                         {"owner_role", "接口"}}}));
}

BB_TEST_CASE("watch.today_equal_end_is_overdue") {
    Fixture fixture;
    json plan = json::array({json{
            {"id", "w1"},
            {"title", "甲"},
            {"kind", "work"},
            {"status", "doing"},
            {"start", "2026-09-01"},
            {"end", "2026-10-01"},
            {"owner_role", "接口"}}});
    auto result = fixture.app->watch(plan, plan, "2026-10-01");
    expect_ok(result);
    CHECK(result->at("chase")[0].at("judgment") == "超期");
}

BB_TEST_CASE("watch.late_work_is_chase_not_a_meeting") {
    Fixture fixture;
    json plan = json::array({json{
            {"id", "w1"},
            {"title", "甲"},
            {"kind", "work"},
            {"status", "doing"},
            {"start", "2026-09-01"},
            {"end", "2026-09-30"},
            {"owner_role", "接口"}}});
    auto watched = fixture.app->watch(plan, plan, "2026-10-01");
    expect_ok(watched);
    CHECK(watched->at("chase")[0].at("id") == "w1");
    fixture.model.response =
            json{{"meetings", json::array()}, {"todos", json::array()}, {"ai_recommended_item_id", nullptr}}
                    .dump();
    auto meet = fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm"}, {"today", "2026-10-01"}});
    expect_ok(meet);
    CHECK(meet->at("meetings") == json::array());
    CHECK(meet->at("ai_recommended_item_id").is_null());
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("compare.matching_edits_are_consistent") {
    Fixture fixture;
    json edits = json{{"rows", json::array({json{
                                    {"业务id", "w1"},
                                    {"标题", "接飞书"},
                                    {"类型", "work"},
                                    {"开始", "2026-10-01"},
                                    {"结束", "2026-10-03"},
                                    {"前置", json::array()}}})}};
    auto result = fixture.app->compare_edits(one_work_manifest(), edits);
    expect_ok(result);
    CHECK(*result == json{{"consistent", true}});
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("compare.date_mismatch_does_not_write") {
    Fixture fixture;
    json edits = json{{"rows", json::array({json{
                                    {"业务id", "w1"},
                                    {"标题", "接飞书"},
                                    {"类型", "work"},
                                    {"开始", "2026-10-02"},
                                    {"结束", "2026-10-03"},
                                    {"前置", json::array()}}})}};
    auto result = fixture.app->compare_edits(one_work_manifest(), edits);
    expect_ok(result);
    CHECK(result->at("consistent") == false);
    CHECK(result->at("fields") == json::array({"w1.开始"}));
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("review.conclusion_does_not_write_the_table") {
    Fixture fixture;
    fixture.model.response =
            json{{"conclusion", "projector_defect"}, {"reason", "日期不一致"}}.dump();
    auto result = fixture.app->handle_event(json{
            {"kind", "review"},
            {"message_id", "rv1"},
            {"fields", json::array({"w1.开始"})}});
    expect_ok(result);
    CHECK(*result == json{{"conclusion", "projector_defect"}, {"reason", "日期不一致"}});
    CHECK(fixture.bitable.upsert_calls == 0);
    CHECK(fixture.bitable.records.empty());
}

BB_TEST_CASE("queue.thirty_two_accepted_and_thirty_third_rejected") {
    Fixture fixture;
    for (int index = 1; index <= 32; ++index) {
        expect_ok(fixture.app->enqueue(
                json{{"name", "watch"}, {"id", "c" + std::to_string(index)}}));
    }
    auto rejected = fixture.app->enqueue(json{{"name", "watch"}, {"id", "c33"}});
    expect_code(rejected, robot_pm::ErrorCode::kEditRejected);
    const auto ids = fixture.app->command_ids();
    CHECK(ids.size() == 32);
    CHECK(std::find(ids.begin(), ids.end(), "c33") == ids.end());
}

BB_TEST_CASE("queue.empty_allowlist_rejects_chase_and_meet") {
    Fixture fixture;
    fixture.config.chase_allowlist.clear();
    fixture.config.meet_allowlist.clear();
    fixture.reopen();
    expect_code(
            fixture.app->handle_event(
                    json{{"kind", "command"}, {"name", "chase"}, {"open_id", "ou_owner"}}),
            robot_pm::ErrorCode::kForbidden);
    expect_code(
            fixture.app->handle_event(
                    json{{"kind", "command"}, {"name", "meet"}, {"open_id", "ou_owner"}}),
            robot_pm::ErrorCode::kForbidden);
    expect_ok(fixture.app->handle_event(
            json{{"kind", "command"}, {"name", "report"}, {"open_id", "ou_owner"}}));
}

BB_TEST_CASE("ingress.unknown_event_ignored") {
    Fixture fixture;
    const auto before = fixture.audit().size();
    auto result = fixture.app->handle_event(json{{"kind", "sticker"}});
    expect_code(result, robot_pm::ErrorCode::kUnknownEvent);
    CHECK(fixture.audit().size() == before);
    CHECK(fixture.feishu.sent.empty());
}

BB_TEST_CASE("error.secrets_never_appear") {
    Fixture fixture;
    fixture.config.bitable_app_token.clear();
    fixture.reopen();
    fixture.process.exit_code = 1;
    fixture.process.stderr_text = "token super-secret-value-xyz cli_example bascnEXAMPLE";
    fixture.process.stdout_text = R"({"records":[{"业务id":"w1"})";
    auto result = fixture.app->import_inbox();
    REQUIRE_FALSE(result.has_value());
    expect_secret_hidden(result.error().message);
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("write.partial_batch_rolls_back") {
    Fixture fixture;
    fixture.bitable.records = json::array({json{{"业务id", "keep"}, {"状态", "todo"}}});
    const auto before = fixture.bitable.records;
    fixture.bitable.partial_batch = true;
    auto result = fixture.app->write_edits(json{{"rows", json::array({
                                                          json{{"业务id", "w1"}, {"标题", "甲"}},
                                                          json{{"业务id", "w2"}, {"标题", "乙"}},
                                                  })}});
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.records == before);
}

BB_TEST_CASE("write.failed_undo_is_not_restored") {
    Fixture fixture;
    fixture.bitable.partial_batch = true;
    fixture.bitable.undo_fails = true;
    auto result = fixture.app->write_edits(
            json{{"rows", json::array({json{{"业务id", "w1"}, {"标题", "甲"}}})}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("没有回到原状") != std::string::npos);
    expect_secret_hidden(result.error().message);
}

BB_TEST_CASE("bridge.partial_stdout_is_discarded") {
    Fixture fixture;
    fixture.process.exit_code = 1;
    fixture.process.stdout_text = R"({"rows":[{"业务id":"w1")";
    fixture.process.stderr_text = "super-secret-value-xyz";
    auto result = fixture.app->write_edits(json{{"rows", json::array()}});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == robot_pm::ErrorCode::kBridgeFailed);
    CHECK(fixture.bitable.records.empty());
    expect_secret_hidden(result.error().message);
}

}  // namespace bb
