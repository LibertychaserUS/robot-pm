#include "support.hpp"
// Skipped: missing symbol robot_pm::App. Assertions below are unchanged.

namespace bb {
namespace {

json rows_of(const json& document) {
    return document.at("rows");
}

json expected_rows() {
    return json::array({
            json{{"业务id", "doc1"},
                 {"标题", "规格"},
                 {"层级", "document"},
                 {"父记录", json::array()}},
            json{{"业务id", "sec1"},
                 {"标题", "范围"},
                 {"层级", "section"},
                 {"父记录", json::array({"doc1"})}},
            json{{"业务id", "w1"},
                 {"标题", "接飞书"},
                 {"层级", "item"},
                 {"父记录", json::array({"sec1"})},
                 {"类型", "work"},
                 {"开始", "2026-10-01"},
                 {"结束", "2026-10-03"},
                 {"前置", json::array()},
                 {"职责", "接口"}},
    });
}

void expect_no_write(const Fixture& fixture) {
    CHECK(fixture.bitable.upsert_calls == 0);
    CHECK(fixture.bitable.records.empty());
    CHECK(fixture.events().empty());
}

}  // namespace

BB_TEST_CASE("project.happy_path_rows") {
    Fixture fixture;
    auto result = fixture.app->project_manifest(one_work_manifest());
    expect_ok(result);
    CHECK(result.value() == json{{"rows", expected_rows()}});
    expect_no_write(fixture);
}

BB_TEST_CASE("project.empty_documents_progress_zero") {
    Fixture fixture;
    auto result = fixture.app->project_manifest(manifest_with(json::array()));
    expect_ok(result);
    CHECK(result->at("rows") == json::array());
    auto watched = fixture.app->watch(json::array(), json::array(), "2026-10-01");
    expect_ok(watched);
    CHECK(*watched == json{{"progress", 0}, {"chase", json::array()}});
}

BB_TEST_CASE("project.missing_field_rejects_before_write") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0].erase("title");
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnknownField);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.wrong_type_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["predecessors"] = "w0";
    expect_code(
            fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnsupportedType);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.extra_field_does_not_write") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["note"] = "多余";
    auto result = fixture.app->project_manifest(manifest);
    expect_code(result, robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.duplicate_ids_do_not_write") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"].push_back(
            manifest["documents"][0]["sections"][0]["items"][0]);
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.cycle_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    auto second = item(
            "w2", "第二", "work", "todo", "2026-10-01", "2026-10-03", json::array({"w1"}), "接口",
            "第二");
    manifest["documents"][0]["sections"][0]["items"].push_back(second);
    manifest["documents"][0]["sections"][0]["items"][0]["predecessors"] = json::array({"w2"});
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.self_predecessor_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["predecessors"] = json::array({"w1"});
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.missing_parent_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["id"] = "missing-parent";
    manifest["documents"][0]["sections"][0]["items"][0]["predecessors"] = json::array();
    manifest["documents"][0].erase("sections");
    manifest["documents"][0]["items"] = json::array(
            {item("w1", "接飞书", "work", "todo", "2026-10-01", "2026-10-03", json::array(),
                  "接口", "接飞书")});
    manifest["documents"][0]["parent_id"] = "no-such-doc";
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnknownField);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.end_before_start_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["end"] = "2026-09-30";
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.illegal_meet_is_not_projected") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["meet"] = "tomorrow";
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnsupportedType);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.bad_date_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["start"] = "10月1日";
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnsupportedType);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.unknown_schema_version_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["schema_version"] = 2;
    expect_code(fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnsupportedType);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.idempotent_same_ids") {
    Fixture fixture;
    auto first = fixture.app->project_manifest(one_work_manifest());
    auto second = fixture.app->project_manifest(one_work_manifest());
    expect_ok(first);
    expect_ok(second);
    CHECK(first->at("rows") == second->at("rows"));
    CHECK(rows_of(*first) == expected_rows());
}

BB_TEST_CASE("project.unicode_and_long_title_preserved") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    std::string title;
    title.reserve(12004);
    for (int index = 0; index < 4000; ++index) {
        title += "项";
    }
    title += "🚀";
    manifest["documents"][0]["sections"][0]["items"][0]["title"] = title;
    manifest["documents"][0]["sections"][0]["items"][0]["source_quote"] = "接飞书";
    auto result = fixture.app->project_manifest(manifest);
    expect_ok(result);
    CHECK(result->at("rows")[2].at("标题") == title);
}

BB_TEST_CASE("project.nested_title_rejects") {
    Fixture fixture;
    auto manifest = one_work_manifest();
    manifest["documents"][0]["sections"][0]["items"][0]["title"] = json{{"nested", true}};
    expect_code(
            fixture.app->project_manifest(manifest), robot_pm::ErrorCode::kUnsupportedType);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.repeated_keys_reject") {
    Fixture fixture;
    const std::string raw =
            R"({"schema_version":1,"schema_version":2,"documents":[]})";
    expect_code(fixture.app->project_manifest_text(raw), robot_pm::ErrorCode::kEditRejected);
    expect_no_write(fixture);
}

BB_TEST_CASE("project.does_not_overwrite_existing_status") {
    Fixture fixture;
    fixture.bitable.records = json::array({json{
            {"业务id", "w1"},
            {"标题", "旧"},
            {"层级", "item"},
            {"状态", "doing"},
            {"开始", "2026-10-01"},
            {"结束", "2026-10-03"}}});
    expect_ok(fixture.app->write_edits(json{{"rows", expected_rows()}}));
    CHECK(fixture.bitable.records[0].at("状态") == "doing");
    for (const auto& row : expected_rows()) {
        CHECK_FALSE(row.contains("状态"));
    }
}

}  // namespace bb
