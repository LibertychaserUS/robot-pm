#include "robot_pm/decision_drive.hpp"
#include "robot_pm/durable.hpp"
#include "robot_pm/frame.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

[[nodiscard]] std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] bool exact_state(const robot_pm::DecisionView& view, const robot_pm::DecisionPerson& person, bool next) {
    if (!view.present) {
        return false;
    }
    if (!next) {
        return view.generation == 1 && view.feedback == robot_pm::kFeedbackOld && view.status == "todo" &&
               view.decision == "d0" && view.table == "todo" && view.calendar.empty() &&
               view.card == robot_pm::kFeedbackOld && person.feedback == robot_pm::kFeedbackOld && person.table == 0 &&
               person.calendar == 0 && person.card == 0;
    }
    return view.generation == 2 && view.feedback == robot_pm::kFeedbackNew && view.status == "doing" &&
           view.decision == "d1" && view.table == "doing" && view.calendar == "evt-1" &&
           view.card == robot_pm::kFeedbackNew && person.feedback == robot_pm::kFeedbackNew && person.table == 1 &&
           person.calendar == 1 && person.card == 1;
}

void check_stored(const std::filesystem::path& dir) {
    const std::string log = file_bytes(dir / "decisions.jsonl");
    const auto records = robot_pm::read_durable_bytes(log);
    CHECK_FALSE(records.empty());
    CHECK(records.size() <= 2);
    for (const auto& record : records) {
        const std::string feedback = record.value("feedback", "");
        CHECK((feedback == robot_pm::kFeedbackOld || feedback == robot_pm::kFeedbackNew));
        CHECK(record.value("decision", "") != "");
    }
    if (!log.empty() && log.back() != '\n') {
        CHECK(records.size() == robot_pm::read_durable_bytes(log.substr(0, log.rfind('\n') + 1)).size());
    }
    const std::string journal = file_bytes(dir / "journal");
    CHECK(robot_pm::read_durable_bytes(journal).empty());
}

}  // namespace

TEST_CASE("http framing is content length or the last chunk") {
    const std::string json = "{\"ok\":true}";
    const std::string short_message = "POST / HTTP/1.1\r\nContent-Length: " + std::to_string(json.size() + 4) +
                                      "\r\n\r\n" + json;
    CHECK(robot_pm::read_http_frame(short_message, true).kind == robot_pm::FrameKind::kAbsent);
    const std::string full = "POST / HTTP/1.1\r\nContent-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
    const robot_pm::HttpFrame complete = robot_pm::read_http_frame(full, false);
    CHECK(complete.kind == robot_pm::FrameKind::kComplete);
    CHECK(complete.body == json);
    std::ostringstream size;
    size << std::hex << json.size();
    const std::string open_chunk = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" + size.str() + "\r\n" +
                                   json + "\r\n";
    CHECK(robot_pm::read_http_frame(open_chunk, true).kind == robot_pm::FrameKind::kAbsent);
    const std::string closed_chunk = open_chunk + "0\r\n\r\n";
    const robot_pm::HttpFrame chunked = robot_pm::read_http_frame(closed_chunk, false);
    CHECK(chunked.kind == robot_pm::FrameKind::kComplete);
    CHECK(chunked.body == json);
    CHECK_FALSE(robot_pm::model_reply_text("data: {\"feedback\":\"新的回复\"}\n").has_value());
    const auto done = robot_pm::model_reply_text("data: {\"feedback\":\"新的回复\"}\n\ndata: [DONE]\n");
    REQUIRE(done.has_value());
    CHECK(done->find("新的回复") != std::string::npos);
}

TEST_CASE("decision cut is only the previous state or the next one") {
    for (const robot_pm::DecisionCut cut : robot_pm::kDecisionCuts) {
        SUBCASE(robot_pm::decision_cut_name(cut)) {
            const std::filesystem::path dir = std::filesystem::temp_directory_path() / "robot-pm-cut" /
                                              robot_pm::decision_cut_name(cut);
            std::filesystem::remove_all(dir);
            robot_pm::plant_state_n(dir);
            robot_pm::DecisionPerson person;
            const robot_pm::DriveResult driven = robot_pm::run_decision(dir, cut);
            CHECK_FALSE(driven.violated);
            if (cut == robot_pm::DecisionCut::kRead) {
                CHECK(driven.read_returned);
                CHECK((driven.read_view.feedback == robot_pm::kFeedbackOld ||
                       driven.read_view.feedback == robot_pm::kFeedbackNew));
            }
            if (cut == robot_pm::DecisionCut::kReadStopped) {
                CHECK_FALSE(driven.read_returned);
            }
            const robot_pm::DecisionView opened = robot_pm::reopen_decision(dir, person);
            check_stored(dir);
            const bool old_state = exact_state(opened, person, false);
            const bool new_state = exact_state(opened, person, true);
            const bool committed = cut == robot_pm::DecisionCut::kAfterLocalCommitBeforeAck;
            CHECK(committed ? new_state : old_state);

            const robot_pm::DriveResult retried = robot_pm::run_decision(dir, std::nullopt);
            CHECK_FALSE(retried.violated);
            const robot_pm::DecisionView again = robot_pm::reopen_decision(dir, person);
            check_stored(dir);
            CHECK(exact_state(again, person, true));
            CHECK(person.table == 1);
            CHECK(person.calendar == 1);
            CHECK(person.card == 1);
            CHECK(person.feedback == robot_pm::kFeedbackNew);
            std::filesystem::remove_all(dir);
        }
    }
}
