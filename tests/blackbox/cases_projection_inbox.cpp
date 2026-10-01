#include "support.hpp"
// Skipped: missing symbol robot_pm::App. Assertions below are unchanged.

namespace bb {
namespace {

std::string valid_manifest_text() {
    return one_work_manifest().dump();
}

}  // namespace

BB_TEST_CASE("projection.success_writes_table_and_sends_no_feishu") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/plan.json", valid_manifest_text());
    expect_ok(fixture.app->import_inbox());
    CHECK_FALSE(fixture.bitable.records.empty());
    bool saw_item = false;
    for (const auto& row : fixture.bitable.records) {
        if (row.value("业务id", "") == "w1") {
            saw_item = true;
            CHECK(row.at("标题") == "接飞书");
            CHECK(row.at("开始") == "2026-10-01");
            CHECK(row.at("结束") == "2026-10-03");
        }
    }
    CHECK(saw_item);
    CHECK(fixture.feishu.sent.empty());
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("projection.failure_keeps_table_writes_audit_and_sends_no_feishu") {
    Fixture fixture;
    fixture.bitable.records = json::array({json{{"业务id", "keep"}, {"状态", "todo"}}});
    const auto before = fixture.bitable.records;
    write_text(fixture.config.data_root / "inbox/bad.json", "{");
    auto result = fixture.app->import_inbox();
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.bitable.records == before);
    CHECK(fixture.feishu.sent.empty());
    const auto audit = fixture.audit();
    REQUIRE_FALSE(audit.empty());
    CHECK(audit.back().at("ok") == false);
}

BB_TEST_CASE("projection.does_not_wait_and_is_not_cancelled_by_thirty_minutes") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/plan.json", valid_manifest_text());
    expect_ok(fixture.app->import_inbox());
    const auto records = fixture.bitable.records;
    fixture.clock.current += std::chrono::minutes{31};
    expect_ok(fixture.app->sweep());
    CHECK(fixture.bitable.records == records);
    CHECK(fixture.feishu.sent.empty());
    const auto root = fixture.config.data_root / "working";
    if (fs::exists(root)) {
        for (const auto& person : fs::directory_iterator(root)) {
            if (!person.is_directory()) {
                continue;
            }
            for (const auto& interaction : fs::directory_iterator(person.path())) {
                if (!interaction.is_directory()) {
                    continue;
                }
                const auto body = read_text(interaction.path());
                CHECK(body.find("waiting") == std::string::npos);
            }
        }
    }
}

BB_TEST_CASE("projection.status_update_still_waits_in_the_group") {
    Fixture fixture;
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"层级", "item"},
            {"状态", "todo"},
            {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    REQUIRE_FALSE(fixture.feishu.sent.empty());
}

BB_TEST_CASE("import.inbox_file_is_projected_with_no_feishu_message") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/spec.json", valid_manifest_text());
    expect_ok(fixture.app->import_inbox());
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.feishu.sent.empty());
    bool saw = false;
    for (const auto& row : fixture.bitable.records) {
        if (row.value("业务id", "") == "w1") {
            saw = true;
        }
    }
    CHECK(saw);
}

BB_TEST_CASE("import.group_attachment_is_not_imported") {
    Fixture fixture;
    const auto before = fixture.bitable.records;
    json event = group_message("ou_owner", "看附件", true, "m-file");
    event["attachments"] = json::array({json{
            {"name", "spec.json"}, {"content", valid_manifest_text()}}});
    expect_ok(fixture.app->handle_event(event));
    CHECK(fixture.bitable.records == before);
    CHECK(fixture.bitable.upsert_calls == 0);
    CHECK(fs::is_empty(fixture.config.data_root / "inbox"));
}

}  // namespace bb
