#include "support.hpp"
// Assertions below are unchanged.

namespace bb {
namespace {

json work_row(std::string node = "") {
    json row = {
            {"业务id", "w1"},
            {"标题", "接飞书"},
            {"层级", "item"},
            {"类型", "work"},
            {"状态", "todo"},
            {"开始", "2026-10-01"},
            {"结束", "2026-10-03"},
            {"职责", "接口"}};
    if (!node.empty()) {
        row["node"] = std::move(node);
    }
    return row;
}

void seed_people(Fixture& fixture) {
    fixture.bitable.records = json::array({work_row()});
    fixture.bitable.role_rows = json::array({
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_owner"}}})},
                 {"职责", "接口"}},
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_pm"}}})},
                 {"职责", "pm"}},
            json{{"群id", "oc_group"},
                 {"人员", json::array({json{{"id", "ou_other"}}})},
                 {"职责", "测试"}},
    });
}

json tombstone(std::string interaction_id, std::string actor, std::string outcome) {
    return json{
            {"interaction_id", std::move(interaction_id)},
            {"actor", std::move(actor)},
            {"op", "end"},
            {"outcome", std::move(outcome)},
            {"time", "2026-10-01 00:30:00"}};
}

json update_reply() {
    return json{{"action", "update"}, {"item_id", "w1"}, {"status", "doing"}};
}

}  // namespace

BB_TEST_CASE("logic.new_message_one_model_call") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    CHECK(fixture.model.calls.size() == 1);
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    CHECK(fixture.events().empty());
}

BB_TEST_CASE("logic.next_message_one_call_uses_plan_not_transcript") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing。闲聊 ALPHA_CHAT", true, "m1")));
    fixture.model.response = json{{"action", "clarify"}, {"text", "确认改成 doing 吗"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "再说一遍", true, "m2")));
    REQUIRE(fixture.model.calls.size() == 2);
    const std::string& user = fixture.model.calls[1].user_message;
    CHECK(user.find("w1") != std::string::npos);
    CHECK(user.find("doing") != std::string::npos);
    CHECK(user.find("ALPHA_CHAT") == std::string::npos);
    CHECK(child_dirs(fixture.config.data_root / "working/ou_owner").size() == 1);
}

BB_TEST_CASE("logic.confirm_makes_zero_model_calls_and_completes") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto calls = fixture.model.calls.size();
    json callback = {
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"message_id", "cb1"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}};
    expect_ok(fixture.app->handle_event(callback));
    CHECK(fixture.model.calls.size() == calls);
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    REQUIRE(fixture.events().size() == 1);
    CHECK(fixture.events()[0] == tombstone("m1", "ou_owner", "done"));
    CHECK(fixture.bitable.records[0].at("状态") == "doing");
}

BB_TEST_CASE("logic.cancel_makes_zero_model_calls") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto calls = fixture.model.calls.size();
    const auto sent = fixture.feishu.sent.size();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "取消", true, "m-cancel")));
    CHECK(fixture.model.calls.size() == calls);
    CHECK(fixture.feishu.sent.size() == sent);
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    REQUIRE(fixture.events().size() == 1);
    CHECK(fixture.events()[0] == tombstone("m1", "ou_owner", "cancelled"));
    CHECK(fs::exists(fixture.config.data_root / "episodic/events.jsonl"));
}

BB_TEST_CASE("logic.timeout_makes_zero_model_calls") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto calls = fixture.model.calls.size();
    fixture.clock.current += std::chrono::minutes{30} + std::chrono::seconds{1};
    expect_ok(fixture.app->sweep());
    CHECK(fixture.model.calls.size() == calls);
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK(fixture.feishu.events.empty());
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    REQUIRE(fixture.events().size() == 1);
    CHECK(fixture.events()[0].at("outcome") == "cancelled");
    CHECK(fixture.events()[0].at("interaction_id") == "m1");
}

BB_TEST_CASE("logic.second_request_does_not_start_another_plan") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "done"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 done", true, "m2")));
    const auto dirs = child_dirs(fixture.config.data_root / "working/ou_owner");
    REQUIRE(dirs.size() == 1);
    CHECK(dirs[0].filename() == "m1");
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m2")));
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
}

BB_TEST_CASE("logic.after_done_next_message_starts_clean") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    const auto calls = fixture.model.calls.size();
    fixture.model.response =
            json{{"action", "update"}, {"item_id", "w1"}, {"status", "done"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 done", true, "m3")));
    CHECK(fixture.model.calls.size() == calls + 1);
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m3")));
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
}

BB_TEST_CASE("memory.interaction_directory_created_on_start") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto dir = interaction_dir(fixture, "ou_owner", "m1");
    CHECK(fs::is_directory(dir));
    CHECK(dir == fixture.config.data_root / "working/ou_owner/m1");
}

BB_TEST_CASE("memory.ending_appends_one_tombstone_then_directory_is_gone") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    const auto rows = fixture.events();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == tombstone("m1", "ou_owner", "done"));
}

BB_TEST_CASE("memory.tombstone_remains_after_directory_deletion") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK_FALSE(fs::exists(fixture.config.data_root / "working/ou_owner/m1"));
    const auto text = read_text(fixture.config.data_root / "episodic/events.jsonl");
    CHECK(text.find("\"interaction_id\":\"m1\"") != std::string::npos);
    CHECK(text.find("\"outcome\":\"done\"") != std::string::npos);
}

BB_TEST_CASE("memory.second_person_gets_a_different_directory") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing。机密 ALPHA_PRIVATE", true, "m1")));
    write_text(interaction_dir(fixture, "ou_owner", "m1") / "note.txt", "ALPHA_PRIVATE");
    fixture.model.response = json{{"action", "none"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_other", "进度是多少", true, "m2")));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_other", "m2")));
    CHECK(interaction_dir(fixture, "ou_other", "m2") !=
          interaction_dir(fixture, "ou_owner", "m1"));
    REQUIRE_FALSE(fixture.model.calls.empty());
    CHECK(fixture.model.calls.back().user_message.find("ALPHA_PRIVATE") == std::string::npos);
    CHECK(read_text(interaction_dir(fixture, "ou_owner", "m1") / "note.txt") == "ALPHA_PRIVATE");
}

BB_TEST_CASE("memory.restart_with_tombstone_removes_leftover_and_does_not_append") {
    Fixture fixture;
    fixture.app.reset();
    const auto dir = interaction_dir(fixture, "ou_owner", "m9");
    write_text(dir / "plan.json", "{\"stale\":true}");
    const json planted = tombstone("m9", "ou_owner", "cancelled");
    write_text(
            fixture.config.data_root / "episodic/events.jsonl",
            planted.dump() + "\n");
    fixture.open();
    CHECK_FALSE(fs::exists(dir));
    const auto rows = fixture.events();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == planted);
}

BB_TEST_CASE("memory.one_person_cannot_have_two_open_interaction_directories") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "再开一件", true, "m2")));
    const auto dirs = child_dirs(fixture.config.data_root / "working/ou_owner");
    CHECK(dirs.size() == 1);
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m2")));
}

BB_TEST_CASE("logic.release_node_can_recommend_meeting_before_its_date") {
    Fixture fixture;
    fixture.bitable.records = json::array({work_row("release")});
    fixture.bitable.records[0]["结束"] = "2026-10-10";
    fixture.bitable.role_rows = json::array({json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", "ou_owner"}}})},
            {"职责", "接口"}}});
    fixture.model.response = json{
            {"meetings",
             json::array({json{
                     {"item_id", "w1"},
                     {"title", "接飞书"},
                     {"agenda", "发布前"},
                     {"start", "2026-10-09 10:00"},
                     {"end", "2026-10-09 11:00"},
                     {"attendee_roles", json::array({"接口"})}}})},
            {"todos", json::array()},
            {"ai_recommended_item_id", "w1"}}
                                      .dump();
    auto result = fixture.app->handle_event(json{
            {"kind", "timer"},
            {"message_id", "tm1"},
            {"today", "2026-10-01"}});
    expect_ok(result);
    CHECK(result->at("ai_recommended_item_id") == "w1");
    CHECK_FALSE(result->contains("meeting_end"));
    CHECK(result->at("meetings")[0].at("start") == "2026-10-09 10:00");
    CHECK(result->at("meetings")[0].at("end") == "2026-10-09 11:00");
    CHECK(fixture.feishu.events.empty());
}

BB_TEST_CASE("logic.deadline_date_is_not_changed_by_dialogue") {
    Fixture fixture;
    seed_people(fixture);
    fixture.bitable.records[0]["node"] = "deadline";
    fixture.model.response = json{
            {"action", "update"},
            {"item_id", "w1"},
            {"end", "2026-11-01"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 的结束改成 2026-11-01", true, "m1")));
    CHECK(fixture.bitable.records[0].at("结束") == "2026-10-03");
    CHECK(fixture.feishu.events.empty());
    if (fs::exists(interaction_dir(fixture, "ou_owner", "m1"))) {
        const auto plan = read_text(interaction_dir(fixture, "ou_owner", "m1"));
        CHECK(plan.find("2026-11-01") == std::string::npos);
    }
}

BB_TEST_CASE("logic.flexible_date_proposal_waits_for_confirm") {
    Fixture fixture;
    seed_people(fixture);
    fixture.bitable.records[0]["node"] = "flexible";
    fixture.model.response = json{
            {"action", "update"},
            {"item_id", "w1"},
            {"end", "2026-10-05"}}
                                      .dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 的结束改到 2026-10-05", true, "m1")));
    CHECK(fixture.bitable.records[0].at("结束") == "2026-10-03");
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    CHECK(fixture.events().empty());
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fixture.bitable.records[0].at("结束") == "2026-10-05");
}

int count_open_directories(const Fixture& fixture) {
    int count = 0;
    const auto root = fixture.config.data_root / "working";
    if (!fs::exists(root)) {
        return 0;
    }
    for (const auto& person : fs::directory_iterator(root)) {
        if (!person.is_directory() || person.path().filename() == "plans") {
            continue;
        }
        for (const auto& interaction : fs::directory_iterator(person.path())) {
            if (interaction.is_directory()) {
                ++count;
            }
        }
    }
    return count;
}

void allow_owner(Fixture& fixture, const std::string& open_id) {
    fixture.bitable.role_rows.push_back(json{
            {"群id", "oc_group"},
            {"人员", json::array({json{{"id", open_id}}})},
            {"职责", "接口"}});
}

BB_TEST_CASE("logic.waiting_plan_has_confirm_and_cancel_buttons") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    REQUIRE_FALSE(fixture.feishu.sent.empty());
    const auto card = fixture.feishu.sent.back().dump();
    CHECK(card.find("确认") != std::string::npos);
    CHECK(card.find("取消") != std::string::npos);
}

BB_TEST_CASE("logic.cancel_button_cancels_waiting_plan") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    const auto calls = fixture.model.calls.size();
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "取消"},
            {"open_id", "ou_owner"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fixture.model.calls.size() == calls);
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_owner", "m1")));
    REQUIRE(fixture.events().size() == 1);
    CHECK(fixture.events()[0] == tombstone("m1", "ou_owner", "cancelled"));
}

BB_TEST_CASE("memory.five_open_directories_are_allowed") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    for (int index = 1; index <= 5; ++index) {
        const auto open_id = "ou_" + std::to_string(index);
        allow_owner(fixture, open_id);
        expect_ok(fixture.app->handle_event(group_message(
                open_id, "把 w1 改成 doing", true, "m" + std::to_string(index))));
        CHECK(fs::is_directory(interaction_dir(fixture, open_id, "m" + std::to_string(index))));
    }
    CHECK(count_open_directories(fixture) == 5);
}

BB_TEST_CASE("memory.sixth_person_gets_card_and_no_directory") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    for (int index = 1; index <= 5; ++index) {
        const auto open_id = "ou_" + std::to_string(index);
        allow_owner(fixture, open_id);
        expect_ok(fixture.app->handle_event(group_message(
                open_id, "把 w1 改成 doing", true, "m" + std::to_string(index))));
    }
    const auto calls = fixture.model.calls.size();
    const auto sent = fixture.feishu.sent.size();
    allow_owner(fixture, "ou_6");
    expect_ok(fixture.app->handle_event(
            group_message("ou_6", "把 w1 改成 doing", true, "m6")));
    CHECK(fixture.model.calls.size() == calls);
    CHECK_FALSE(fs::exists(fixture.config.data_root / "working/ou_6"));
    CHECK(count_open_directories(fixture) == 5);
    REQUIRE(fixture.feishu.sent.size() == sent + 1);
    CHECK(fixture.feishu.sent.back().dump().find("现在人满了，请稍后再试") !=
          std::string::npos);
}

BB_TEST_CASE("memory.other_open_id_cannot_read_first_directory") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    write_text(interaction_dir(fixture, "ou_owner", "m1") / "note.txt", "SECRET_A");
    fixture.model.response = json{{"action", "none"}}.dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_other", "现在进度是多少", true, "m2")));
    REQUIRE_FALSE(fixture.model.calls.empty());
    CHECK(fixture.model.calls.back().user_message.find("SECRET_A") == std::string::npos);
    CHECK(read_text(interaction_dir(fixture, "ou_owner", "m1") / "note.txt") == "SECRET_A");
}

BB_TEST_CASE("memory.other_open_id_cannot_confirm_first_directory") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_other"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK(fixture.events().empty());
}

BB_TEST_CASE("memory.other_open_id_cannot_cancel_first_directory") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    expect_ok(fixture.app->handle_event(
            group_message("ou_owner", "把 w1 改成 doing", true, "m1")));
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "取消"},
            {"open_id", "ou_other"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    expect_ok(fixture.app->handle_event(
            group_message("ou_other", "取消", true, "m-other-cancel")));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_owner", "m1")));
    CHECK(fixture.bitable.records[0].at("状态") == "todo");
    CHECK(fixture.events().empty());
}

BB_TEST_CASE("memory.freed_slot_after_tombstone_allows_new_person") {
    Fixture fixture;
    seed_people(fixture);
    fixture.model.response = update_reply().dump();
    for (int index = 1; index <= 5; ++index) {
        const auto open_id = "ou_" + std::to_string(index);
        allow_owner(fixture, open_id);
        expect_ok(fixture.app->handle_event(group_message(
                open_id, "把 w1 改成 doing", true, "m" + std::to_string(index))));
    }
    expect_ok(fixture.app->handle_event(json{
            {"kind", "card_callback"},
            {"action", "确认"},
            {"open_id", "ou_1"},
            {"interaction_id", "m1"},
            {"mentions_bot", false}}));
    CHECK_FALSE(fs::exists(interaction_dir(fixture, "ou_1", "m1")));
    REQUIRE_FALSE(fixture.events().empty());
    CHECK(fixture.events().back().at("outcome") == "done");
    allow_owner(fixture, "ou_6");
    expect_ok(fixture.app->handle_event(
            group_message("ou_6", "把 w1 改成 doing", true, "m6")));
    CHECK(fs::is_directory(interaction_dir(fixture, "ou_6", "m6")));
    CHECK(count_open_directories(fixture) == 5);
}

}  // namespace bb
