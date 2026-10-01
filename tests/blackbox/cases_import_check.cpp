#include "support.hpp"
// Skipped: missing symbol robot_pm::App. Assertions below are unchanged.

namespace bb {
namespace {

json audit_packet(std::string format, bool bad_quote) {
    json item = {
            {"id", "w1"},
            {"title", "接飞书"},
            {"kind", "work"},
            {"status", "todo"},
            {"start", "2026-10-01"},
            {"end", "2026-10-03"},
            {"predecessors", json::array()},
            {"owner_role", "接口"},
            {"source_quote", bad_quote ? "不在原文里" : "接飞书"}};
    return json{
            {"format", std::move(format)},
            {"source_text", "接飞书"},
            {"source_path", "inbox/spec.md"},
            {"manifest", one_work_manifest()},
            {"rows", json::array({json{
                             {"业务id", "w1"},
                             {"标题", "接飞书"},
                             {"类型", "work"},
                             {"开始", "2026-10-01"},
                             {"结束", "2026-10-03"},
                             {"前置", json::array()},
                             {"职责", "接口"}}})},
            {"expected", json::array({json{
                              {"id", "w1"},
                              {"title", "接飞书"},
                              {"kind", "work"},
                              {"start", "2026-10-01"},
                              {"end", "2026-10-03"},
                              {"predecessors", json::array()},
                              {"owner_role", "接口"}}})},
            {"item", item}};
}

}  // namespace

BB_TEST_CASE("check.all_pass_is_ok_and_empty_slices") {
    Fixture fixture;
    fixture.model.response = json{{"ok", true}, {"slices", json::array()}}.dump();
    auto result = fixture.app->audit_slices(audit_packet("markdown", false));
    expect_ok(result);
    CHECK(*result == json{{"ok", true}, {"slices", json::array()}});
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("check.one_bad_item_fails_the_document") {
    Fixture fixture;
    fixture.model.response = json{{"ok", true}, {"slices", json::array()}}.dump();
    auto packet = audit_packet("markdown", true);
    auto result = fixture.app->audit_slices(packet);
    expect_ok(result);
    CHECK(result->at("ok") == false);
    CHECK_FALSE(result->at("slices").empty());
    CHECK(fixture.bitable.upsert_calls == 0);
}

BB_TEST_CASE("check.unrecognized_format") {
    Fixture fixture;
    fixture.model.response = json{{"ok", true}, {"slices", json::array()}}.dump();
    auto result = fixture.app->audit_slices(audit_packet("rtf", false));
    expect_ok(result);
    CHECK(result->at("ok") == false);
    CHECK(result->at("slices")[0].at("result") == "unrecognized");
}

BB_TEST_CASE("check.markdown_docx_and_pdf_text_are_accepted") {
    Fixture fixture;
    for (const char* format : {"markdown", "docx", "pdf"}) {
        fixture.model.response = json{{"ok", true}, {"slices", json::array()}}.dump();
        auto result = fixture.app->audit_slices(audit_packet(format, false));
        expect_ok(result);
        CHECK(result->at("ok") == true);
    }
}

BB_TEST_CASE("import.valid_json_skips_the_model") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/ok.json", one_work_manifest().dump());
    expect_ok(fixture.app->import_inbox());
    CHECK(fixture.model.calls.empty());
}

BB_TEST_CASE("import.invalid_json_writes_nothing") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/bad.json", "{");
    auto result = fixture.app->import_inbox();
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.bitable.records.empty());
}

BB_TEST_CASE("import.markdown_heading_becomes_a_section") {
    Fixture fixture;
    write_text(
            fixture.config.data_root / "inbox/guide.md",
            "# 规格\n## 范围\n接飞书\n");
    fixture.model.response = one_work_manifest().dump();
    auto result = fixture.app->import_inbox();
    expect_ok(result);
    CHECK(result->dump().find("范围") != std::string::npos);
    REQUIRE_FALSE(fixture.model.calls.empty());
    CHECK(fixture.model.calls[0].user_message.find("接飞书") != std::string::npos);
}

BB_TEST_CASE("import.pdf_without_text_keeps_the_path") {
    Fixture fixture;
    const std::string empty_pdf = "%PDF-1.4\n1 0 obj<</Type/Catalog>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    write_text(fixture.config.data_root / "inbox/scan.pdf", empty_pdf);
    auto result = fixture.app->import_inbox();
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.bitable.records.empty());
    const auto slipped = read_text(fixture.config.data_root / "semantic/supervision");
    const auto events = fixture.events();
    const bool kept = slipped.find("scan.pdf") != std::string::npos ||
                      (!events.empty() && events.back().dump().find("scan.pdf") != std::string::npos);
    CHECK(kept);
}

BB_TEST_CASE("import.unknown_extension_is_unrecognized") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/notes.txt", "接飞书");
    auto result = fixture.app->import_inbox();
    expect_ok(result);
    CHECK(result->at("result") == "unrecognized");
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.bitable.records.empty());
}

BB_TEST_CASE("import.ocr_output_is_not_a_successful_parse") {
    Fixture fixture;
    write_text(fixture.config.data_root / "inbox/scan.pdf", "%PDF-1.4\n%%EOF\n");
    fixture.model.response = one_work_manifest().dump();
    auto result = fixture.app->import_inbox();
    REQUIRE_FALSE(result.has_value());
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.bitable.upsert_calls == 0);
}

}  // namespace bb
