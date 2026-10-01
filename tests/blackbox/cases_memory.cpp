#include "support.hpp"
// Assertions below are unchanged.

namespace bb {

BB_TEST_CASE("memory.create_appends_one_audit_line") {
    Fixture fixture;
    write_text(fixture.config.data_root / "semantic/manifests/doc.json", "{\"id\":\"doc1\"}\n");
    fixture.reopen();
    const auto before = read_text(fixture.config.data_root / "episodic/audit.jsonl");
    expect_ok(fixture.app->handle_event(json{
            {"kind", "memory"},
            {"op", "create"},
            {"path", "semantic/manifests/next.json"},
            {"body", "{\"id\":\"doc2\"}"},
            {"actor", "ou_owner"}}));
    const auto after = read_text(fixture.config.data_root / "episodic/audit.jsonl");
    CHECK(after.rfind(before, 0) == 0);
    const auto rows = fixture.audit();
    REQUIRE_FALSE(rows.empty());
    CHECK(rows.back().at("op") == "create");
    CHECK(rows.back().at("ok") == true);
    CHECK(rows.back().at("actor") == "ou_owner");
    CHECK(rows.back().at("time") == "2026-10-01 00:30:00");
}

BB_TEST_CASE("memory.update_does_not_rewrite_earlier_audit_lines") {
    Fixture fixture;
    write_text(fixture.config.data_root / "semantic/manifests/doc.json", "{\"id\":\"doc1\"}\n");
    expect_ok(fixture.app->handle_event(json{
            {"kind", "memory"},
            {"op", "update"},
            {"path", "semantic/manifests/doc.json"},
            {"body", "{\"id\":\"doc1\",\"title\":\"规格\"}"},
            {"actor", "ou_owner"}}));
    const auto first = read_text(fixture.config.data_root / "episodic/audit.jsonl");
    expect_ok(fixture.app->handle_event(json{
            {"kind", "memory"},
            {"op", "update"},
            {"path", "semantic/manifests/doc.json"},
            {"body", "{\"id\":\"doc1\",\"title\":\"规格二\"}"},
            {"actor", "ou_owner"}}));
    const auto second = read_text(fixture.config.data_root / "episodic/audit.jsonl");
    CHECK(second.rfind(first, 0) == 0);
    CHECK(second.size() > first.size());
}

BB_TEST_CASE("memory.failed_write_appends_ok_false_and_keeps_previous_file") {
    Fixture fixture;
    const std::string original = "{\"id\":\"doc1\"}\n";
    write_text(fixture.config.data_root / "semantic/manifests/doc.json", original);
    fixture.config.fail_semantic_replace = true;
    fixture.reopen();
    auto result = fixture.app->handle_event(json{
            {"kind", "memory"},
            {"op", "update"},
            {"path", "semantic/manifests/doc.json"},
            {"body", "{\"id\":\"broken\"}"},
            {"actor", "ou_owner"}});
    REQUIRE_FALSE(result.has_value());
    CHECK(read_text(fixture.config.data_root / "semantic/manifests/doc.json") == original);
    REQUIRE_FALSE(fixture.audit().empty());
    CHECK(fixture.audit().back().at("ok") == false);
}

BB_TEST_CASE("memory.read_for_execute_logs_one_read_progress_does_not") {
    Fixture fixture;
    write_text(
            fixture.config.data_root / "working/ou_owner/m1/plan.json",
            "{\"item_id\":\"w1\"}\n");
    const auto before = fixture.audit().size();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "进度是多少", true, "pq")));
    CHECK(fixture.audit().size() == before);
    expect_ok(fixture.app->handle_event(json{
            {"kind", "memory"},
            {"op", "read"},
            {"path", "working/ou_owner/m1/plan.json"},
            {"actor", "ou_owner"},
            {"purpose", "execute"}}));
    CHECK(fixture.audit().size() == before + 1);
    CHECK(fixture.audit().back().at("op") == "read");
}

BB_TEST_CASE("memory.confirm_removes_directory_only_after_durable_audit") {
    Fixture fixture;
    fixture.bitable.records = json::array(
            {json{{"业务id", "w1"}, {"状态", "todo"}, {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    fixture.config.stop_after_durable_audit = true;
    fixture.reopen();
    auto stopped = fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}});
    REQUIRE_FALSE(stopped.has_value());
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    const auto audit = read_text(fixture.config.data_root / "episodic/audit.jsonl");
    CHECK(audit.find("\"ok\":true") != std::string::npos);
    REQUIRE_FALSE(audit.empty());
    const char last = audit.back();
    CHECK(last == '\n');
}

BB_TEST_CASE("mention.group_message_without_mention_is_ignored") {
    Fixture fixture;
    const auto before = fixture.audit().size();
    auto result = fixture.app->handle_event(
            group_message("ou_owner", "你好", false, "m-ignore"));
    expect_code(result, robot_pm::ErrorCode::kUnknownEvent);
    CHECK(fixture.audit().size() == before);
    CHECK(fixture.model.calls.empty());
    CHECK(fixture.feishu.sent.empty());
}

BB_TEST_CASE("mention.group_message_at_bot_is_handled") {
    Fixture fixture;
    fixture.model.response = json{{"text", "可以问进度、职责，或看卡片。"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "在吗", true, "m-at")));
    CHECK(fixture.model.calls.size() == 1);
}

BB_TEST_CASE("mention.card_callback_without_mention_is_handled") {
    Fixture fixture;
    fixture.bitable.records = json::array(
            {json{{"业务id", "w1"}, {"状态", "todo"}, {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto calls = fixture.model.calls.size();
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fixture.model.calls.size() == calls);
    CHECK(fixture.bitable.records[0].at("状态") == "doing");
}

BB_TEST_CASE("mention.private_chat_without_mention_is_handled") {
    Fixture fixture;
    fixture.model.response = json{{"text", "可以问进度、职责，或看卡片。"}}.dump();
    expect_ok(fixture.app->handle_event(json{
            {"kind", "p2p_message"},
            {"open_id", "ou_owner"},
            {"text", "在吗"},
            {"mentions_bot", false},
            {"message_id", "p1"}}));
    CHECK(fixture.model.calls.size() == 1);
}

BB_TEST_CASE("context.system_prompt_has_index_and_overview_headings_not_bodies") {
    Fixture fixture;
    write_text(
            fixture.config.data_root / "semantic/manifests/body.md",
            "BODY_SECRET_SENTENCE_NOT_IN_PROMPT\n");
    write_text(
            fixture.config.data_root / "semantic/overview.md",
            "# 项目概览\n## 文档\n- doc1，规格\n## 工作项\n- w1，接飞书，接口，todo，"
            "2026-10-01，2026-10-03\n## 文件\n- semantic/manifests/body.md，一行说明\n");
    fixture.model.response = json{{"action", "none"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "在吗", true, "m-ctx")));
    REQUIRE_FALSE(fixture.model.calls.empty());
    const auto& system = fixture.model.calls.back().system_prompt;
    CHECK(system.find("# 项目概览") != std::string::npos);
    CHECK(system.find("## 文档") != std::string::npos);
    CHECK(system.find("## 工作项") != std::string::npos);
    CHECK(system.find("## 文件") != std::string::npos);
    CHECK(system.find("一行说明") != std::string::npos);
    CHECK(system.find("BODY_SECRET_SENTENCE_NOT_IN_PROMPT") == std::string::npos);
}

BB_TEST_CASE("context.user_message_omits_unrelated_items") {
    Fixture fixture;
    write_text(
            fixture.config.data_root / "semantic/overview.md",
            "# 项目概览\n## 文档\n- doc1，规格\n## 工作项\n- w1，接飞书，接口，todo，"
            "2026-10-01，2026-10-03\n- w2，无关标题二，测试，todo，2026-10-01，2026-10-03\n"
            "## 文件\n- semantic/overview.md，概览\n");
    fixture.bitable.records = json::array({
            json{{"业务id", "w1"}, {"标题", "接飞书"}, {"状态", "todo"}, {"职责", "接口"}},
            json{{"业务id", "w2"}, {"标题", "无关标题二"}, {"状态", "todo"}, {"职责", "测试"}},
    });
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    REQUIRE_FALSE(fixture.model.calls.empty());
    CHECK(fixture.model.calls.back().user_message.find("无关标题二") == std::string::npos);
    CHECK(fixture.model.calls.back().user_message.find("w1") != std::string::npos);
}

BB_TEST_CASE("prompt.cap_cuts_index_or_overview_and_keeps_step_rules") {
    Fixture fixture;
    const std::string rules =
            "只能建议修改 `状态`。取值只能是 `todo`、`doing`、`done`、`blocked`。一次只建议一条。\n";
    write_text(fixture.config.repo_root / "prompts/status_update.md", rules);
    write_text(
            fixture.config.data_root / "semantic/overview.md",
            std::string(200000, 'A') + "ZZZ_OVERVIEW_TAIL");
    write_text(
            fixture.config.data_root / "semantic/manifests/index.txt",
            std::string(200000, 'B') + "ZZZ_INDEX_TAIL");
    auto preview = fixture.app->preview_prompt("status_update");
    expect_ok(preview);
    const auto system = preview->at("system").get<std::string>();
    CHECK(system.find(rules) != std::string::npos);
    const bool overview_cut = system.find("ZZZ_OVERVIEW_TAIL") == std::string::npos;
    const bool index_cut = system.find("ZZZ_INDEX_TAIL") == std::string::npos;
    CHECK((overview_cut || index_cut));
}

BB_TEST_CASE("sop.waiting_over_thirty_minutes_cancels_only_that_plan") {
    Fixture fixture;
    fixture.bitable.records = json::array({
            json{{"业务id", "w1"}, {"状态", "todo"}, {"职责", "接口"}},
    });
    fixture.bitable.role_rows = json::array({
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_owner"}}})},
                 {"职责", "接口"}},
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_other"}}})},
                 {"职责", "接口"}},
    });
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "old")));
    fixture.clock.current += std::chrono::minutes{10};
    expect_ok(fixture.app->handle_event(
            group_message("ou_other", "把 w1 改成 doing", true, "fresh")));
    fixture.clock.current += std::chrono::minutes{21};
    expect_ok(fixture.app->sweep());
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "old")));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_other", "fresh")));
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK(fixture.feishu.events.empty());
    bool mentioned = false;
    for (const auto& message : fixture.feishu.sent) {
        const auto dumped = message.dump();
        const bool names_owner = dumped.find("ou_owner") != std::string::npos;
        const bool names_other = dumped.find("ou_other") != std::string::npos;
        const bool says_cancelled = dumped.find("已取消") != std::string::npos;
        if (names_owner && says_cancelled) {
            mentioned = true;
        }
        const bool cancels_other = names_other && says_cancelled;
        CHECK_FALSE(cancels_other);
    }
    CHECK(mentioned);
}

BB_TEST_CASE("cancel.proposer_text_cancels_waiting_plan") {
    Fixture fixture;
    fixture.bitable.records = json::array(
            {json{{"业务id", "w1"}, {"状态", "todo"}, {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto sent = fixture.feishu.sent.size();
    expect_ok(fixture.app->handle_event(group_message("ou_owner", "取消", true, "m2")));
    CHECK(fixture.feishu.sent.size() == sent);
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    REQUIRE_FALSE(fixture.audit().empty());
    CHECK(fixture.audit().back().at("op") == "cancelled");
}

BB_TEST_CASE("cancel.other_person_cannot_cancel") {
    Fixture fixture;
    fixture.bitable.records = json::array(
            {json{{"业务id", "w1"}, {"状态", "todo"}, {"职责", "接口"}}});
    fixture.bitable.role_rows = json::array({
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_owner"}}})},
                 {"职责", "接口"}},
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_other"}}})},
                 {"职责", "pm"}},
    });
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(group_message("ou_other", "取消", true, "m2")));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
}

BB_TEST_CASE("cancel.pm_can_cancel_timer_plan") {
    Fixture fixture;
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_pm"}}})},
            {"职责", "pm"}}});
    fixture.model.response = json{{"text", "跟进甲"}}.dump();
    expect_ok(fixture.app->handle_event(
            json{{"kind", "timer"}, {"message_id", "tm1"}, {"step", "follow_up"}}));
    expect_ok(fixture.app->handle_event(group_message("ou_pm", "取消", true, "m-pm")));
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "system", "tm1")));
}

BB_TEST_CASE("procedural.prompt_files_are_not_modified") {
    Fixture fixture;
    const auto before = read_text(fixture.config.repo_root / "prompts/identity.md");
    const auto sop = read_text(fixture.config.repo_root / "docs/sop.md");
    fixture.model.response = json{{"text", "你是 robot PM。"}}.dump();
    expect_ok(fixture.app->handle_event(group_message("ou_owner", "你是谁", true, "m1")));
    CHECK(read_text(fixture.config.repo_root / "prompts/identity.md") == before);
    CHECK(read_text(fixture.config.repo_root / "docs/sop.md") == sop);
}

}  // namespace bb
