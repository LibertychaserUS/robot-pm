#include "robot_pm/working_memory.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::filesystem::path make_root(const char* name) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::chrono::system_clock::time_point beijing_ten() {
    return std::chrono::sys_days{std::chrono::year{2026} / std::chrono::October / 1} +
           std::chrono::hours{2};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("working_memory.start_creates_one_directory_for_the_interaction") {
    const auto root = make_root("robot-pm-working-start");
    const nlohmann::json context = {{"utterance", "改成 doing"}, {"role", "接口"}};

    const auto directory = robot_pm::begin_interaction(root, "ou_a", "om_1", context);

    REQUIRE(directory.has_value());
    CHECK(*directory == root / "working" / "ou_a" / "om_1");
    CHECK(std::filesystem::is_regular_file(*directory / "context.json"));
    const auto stored = robot_pm::read_interaction_context(root, "ou_a", "om_1");
    REQUIRE(stored.has_value());
    CHECK(stored->at("utterance") == "改成 doing");
    CHECK(stored->at("role") == "接口");
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(*directory)) {
        static_cast<void>(entry);
        ++entries;
    }
    CHECK(entries == 1);
}

TEST_CASE("working_memory.one_open_folder_per_person") {
    const auto root = make_root("robot-pm-working-one");
    REQUIRE(robot_pm::begin_interaction(root, "ou_a", "om_1", {{"utterance", "第一句"}}).has_value());

    const auto second = robot_pm::begin_interaction(root, "ou_a", "om_2", {{"utterance", "第二句"}});

    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == robot_pm::ErrorCode::kEditRejected);
    CHECK_FALSE(std::filesystem::exists(root / "working" / "ou_a" / "om_2"));
    CHECK(std::filesystem::is_directory(root / "working" / "ou_a" / "om_1"));
}

TEST_CASE("working_memory.two_people_do_not_read_each_other") {
    const auto root = make_root("robot-pm-working-two");
    REQUIRE(robot_pm::begin_interaction(root, "ou_a", "om_a", {{"utterance", "甲"}}).has_value());
    REQUIRE(robot_pm::begin_interaction(root, "ou_b", "om_b", {{"utterance", "乙"}}).has_value());

    const auto first = robot_pm::read_interaction_context(root, "ou_a", "om_a");
    const auto second = robot_pm::read_interaction_context(root, "ou_b", "om_b");
    const auto crossed = robot_pm::read_interaction_context(root, "ou_a", "om_b");
    const auto escaped = robot_pm::read_interaction_context(root, "ou_a", "../ou_b/om_b");

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->at("utterance") == "甲");
    CHECK(second->at("utterance") == "乙");
    CHECK_FALSE(crossed.has_value());
    CHECK_FALSE(escaped.has_value());
    CHECK(std::filesystem::is_regular_file(root / "working" / "ou_b" / "om_b" / "context.json"));
}

TEST_CASE("working_memory.chat_is_not_stored") {
    const auto root = make_root("robot-pm-working-chat");

    const auto begun = robot_pm::begin_interaction(root, "ou_a", "om_1", {{"messages", {"你好"}}});

    REQUIRE_FALSE(begun.has_value());
    CHECK_FALSE(std::filesystem::exists(root / "working" / "ou_a"));
}

TEST_CASE("working_memory.end_writes_tombstone_then_deletes_directory") {
    const auto root = make_root("robot-pm-working-end");
    REQUIRE(robot_pm::begin_interaction(root, "ou_a", "om_1", {{"utterance", "确认"}}).has_value());
    REQUIRE(robot_pm::begin_interaction(root, "ou_b", "om_b", {{"utterance", "乙还在"}}).has_value());

    const auto ended = robot_pm::end_interaction(root, "ou_a", "om_1", "confirm", "done", beijing_ten());

    REQUIRE(ended.has_value());
    CHECK_FALSE(std::filesystem::exists(root / "working" / "ou_a" / "om_1"));
    CHECK(std::filesystem::is_directory(root / "working" / "ou_b" / "om_b"));
    const auto events = nlohmann::json::parse(read_text(root / "episodic" / "events.jsonl"));
    CHECK(events.at("interaction_id") == "om_1");
    CHECK(events.at("actor") == "ou_a");
    CHECK(events.at("op") == "confirm");
    CHECK(events.at("outcome") == "done");
    CHECK(events.at("time") == "2026-10-01 10:00");
    const auto again = robot_pm::end_interaction(root, "ou_a", "om_1", "confirm", "done", beijing_ten());
    REQUIRE_FALSE(again.has_value());
    CHECK(read_text(root / "episodic" / "events.jsonl").find('\n') ==
          read_text(root / "episodic" / "events.jsonl").rfind('\n'));
}

TEST_CASE("working_memory.restart_deletes_directory_left_after_tombstone") {
    const auto root = make_root("robot-pm-working-restart");
    REQUIRE(robot_pm::begin_interaction(root, "ou_a", "om_1", {{"utterance", "确认"}}).has_value());
    REQUIRE(robot_pm::begin_interaction(root, "ou_b", "om_b", {{"utterance", "乙还在"}}).has_value());
    REQUIRE(robot_pm::end_interaction(root, "ou_a", "om_1", "cancel", "cancelled", beijing_ten()).has_value());
    const std::string events_before = read_text(root / "episodic" / "events.jsonl");
    std::filesystem::create_directories(root / "working" / "ou_a" / "om_1");
    {
        std::ofstream leftover(root / "working" / "ou_a" / "om_1" / "context.json");
        leftover << "{\"utterance\":\"残留\"}";
    }

    const auto recovered = robot_pm::recover_finished_interactions(root);

    REQUIRE(recovered.has_value());
    CHECK_FALSE(std::filesystem::exists(root / "working" / "ou_a" / "om_1"));
    CHECK(std::filesystem::is_regular_file(root / "working" / "ou_b" / "om_b" / "context.json"));
    CHECK(read_text(root / "episodic" / "events.jsonl") == events_before);
}
