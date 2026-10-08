#include "robot_pm/collection_card.hpp"
#include "robot_pm/service.hpp"
#include "robot_pm/working_memory.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

[[nodiscard]] int chinese_count(std::string_view text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = 0;
        std::size_t length = 1;
        if ((lead & 0x80U) == 0) {
            code = lead;
        } else if ((lead & 0xE0U) == 0xC0U && index + 1 < text.size()) {
            length = 2;
            code = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            length = 3;
            code = (lead & 0x0FU) << 12U;
            code |= (static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 6U;
            code |= static_cast<unsigned char>(text[index + 2]) & 0x3FU;
        } else if ((lead & 0xF8U) == 0xF0U && index + 3 < text.size()) {
            length = 4;
        }
        if (code >= 0x4E00U && code <= 0x9FFFU) {
            ++count;
        }
        index += length;
    }
    return count;
}

class FakeClock final : public robot_pm::Clock {
public:
    mutable int calls = 0;

    std::chrono::system_clock::time_point now() const override {
        ++calls;
        return std::chrono::sys_days{std::chrono::year{2026} / std::chrono::October / 1};
    }
};

class FakePort final : public robot_pm::EventPort {
public:
    std::optional<robot_pm::FeishuRequest> take(std::stop_token stop) override {
        std::stop_callback callback(stop, [&] { cv.notify_all(); });
        std::unique_lock lock(mu);
        ++entries;
        cv.notify_all();
        cv.wait(lock, [&] { return stop.stop_requested() || !queued.empty(); });
        if (queued.empty()) {
            return std::nullopt;
        }
        robot_pm::FeishuRequest request = std::move(queued.front());
        queued.pop_front();
        return request;
    }

    void reply(std::string_view body) override {
        std::lock_guard lock(mu);
        replies.emplace_back(body);
        cv.notify_all();
    }

    void push(robot_pm::FeishuRequest request) {
        {
            std::lock_guard lock(mu);
            queued.push_back(std::move(request));
        }
        cv.notify_all();
    }

    [[nodiscard]] bool wait_until(int count) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entries >= count; });
    }

    [[nodiscard]] std::vector<std::string> reply_text() {
        std::lock_guard lock(mu);
        return replies;
    }

private:
    std::mutex mu;
    std::condition_variable cv;
    std::deque<robot_pm::FeishuRequest> queued;
    std::vector<std::string> replies;
    int entries = 0;
};

class FakeModel final : public robot_pm::Model {
public:
    std::string response = R"({"action":"update","item_id":"w1","status":"doing"})";
    int calls = 0;
    std::string last_user;

    std::expected<std::string, robot_pm::Error> complete(std::string_view, std::string_view,
                                                         std::string_view user_message) override {
        ++calls;
        last_user = std::string(user_message);
        return response;
    }
};

class FakeFeishu final : public robot_pm::FeishuPort {
public:
    std::vector<json> sent;
    std::vector<json> events;

    std::expected<json, robot_pm::Error> send(const json& message) override {
        sent.push_back(message);
        return json{{"message_id", "om_sent"}};
    }

    std::expected<std::string, robot_pm::Error> primary_calendar_id() override { return std::string("primary"); }

    std::expected<json, robot_pm::Error> create_calendar_event(const json& event) override {
        events.push_back(event);
        return json{{"event_id", "evt-1"}};
    }

    std::expected<void, robot_pm::Error> delete_calendar_event(std::string_view, std::string_view) override {
        events.clear();
        return {};
    }
};

class FakeBitable final : public robot_pm::BitablePort {
public:
    json records = json::array({json{{"业务id", "w1"}, {"标题", "接飞书"}, {"状态", "todo"}, {"职责", "接口"}}});
    json role_rows = json::array();

    std::expected<json, robot_pm::Error> list_fields(std::string_view) override { return json::array(); }

    std::expected<void, robot_pm::Error> create_field(std::string_view, const json&) override { return {}; }

    std::expected<json, robot_pm::Error> list_records(std::string_view table) override {
        if (table == "职责") {
            return role_rows;
        }
        return records;
    }

    std::expected<json, robot_pm::Error> upsert(std::string_view table, const json& incoming) override {
        json& dest = table == "职责" ? role_rows : records;
        for (const json& row : incoming) {
            const std::string key = row.value("业务id", "");
            bool replaced = false;
            for (json& existing : dest) {
                if (!key.empty() && existing.value("业务id", "") == key) {
                    for (auto it = row.begin(); it != row.end(); ++it) {
                        existing[it.key()] = it.value();
                    }
                    replaced = true;
                }
            }
            if (!replaced) {
                dest.push_back(row);
            }
        }
        return json{{"written", incoming.size()}};
    }

    std::expected<void, robot_pm::Error> undo(const json&) override { return {}; }
};

class FakeProcess final : public robot_pm::ProcessRunner {
public:
    std::expected<robot_pm::ProcessResult, robot_pm::Error> run(const robot_pm::ProcessRequest&) override {
        robot_pm::ProcessResult result;
        result.exit_code = 0;
        result.stdout_text = "{}";
        return result;
    }
};

class CountObserver final : public robot_pm::ServiceObserver {
public:
    int calls = 0;
    json last = json::object();

    void before_library(const json& event) override {
        ++calls;
        last = event;
    }
};

class ExplodingPort final : public robot_pm::EventPort {
public:
    std::optional<robot_pm::FeishuRequest> take(std::stop_token) override {
        FAIL("缺配置时不能进入服务循环");
        return std::nullopt;
    }

    void reply(std::string_view) override {}
};

[[nodiscard]] std::map<std::string, std::string> full_env() {
    return {{"FEISHU_APP_ID", "cli_example"},
            {"FEISHU_APP_SECRET", "secret-value"},
            {"FEISHU_ENCRYPT_KEY", "encrypt-key"},
            {"FEISHU_VERIFICATION_TOKEN", "verify-token"},
            {"FEISHU_BOT_OPEN_ID", "ou_bot"}};
}

[[nodiscard]] robot_pm::FeishuRequest signed_request(const json& body) {
    robot_pm::FeishuRequest request;
    request.timestamp = "1710000000";
    request.nonce = "nonce-1";
    request.body = body.dump();
    request.signature =
            robot_pm::feishu_event_signature(request.timestamp, request.nonce, "encrypt-key", request.body);
    return request;
}

[[nodiscard]] json message_body(const char* chat_type, bool mention_bot, const char* message_type) {
    json body;
    body["header"]["event_type"] = "im.message.receive_v1";
    body["header"]["token"] = "verify-token";
    body["event"]["sender"]["sender_id"]["open_id"] = "ou_owner";
    body["event"]["sender"]["name"] = "张三";
    body["event"]["message"]["message_id"] = "m1";
    body["event"]["message"]["chat_id"] = "oc_group";
    body["event"]["message"]["chat_type"] = chat_type;
    body["event"]["message"]["chat_mode"] = "group";
    body["event"]["message"]["group_message_type"] = "chat";
    body["event"]["message"]["message_type"] = message_type;
    body["event"]["message"]["content"] = json{{"text", "把这项改成进行中"}}.dump();
    body["event"]["message"]["mentions"] = json::array();
    const char* mentioned = mention_bot ? "ou_bot" : "ou_other";
    body["event"]["message"]["mentions"].push_back(
            json{{"key", "@_user_1"}, {"id", {{"open_id", mentioned}}}});
    return body;
}

[[nodiscard]] json card_body(const char* open_id, const char* action) {
    json body;
    body["header"]["event_type"] = "card.action.trigger";
    body["header"]["token"] = "verify-token";
    body["event"]["operator"]["open_id"] = open_id;
    body["event"]["action"]["value"] = json{{"action", action}, {"interaction_id", "m1"}};
    return body;
}

struct Running {
    fs::path root;
    FakeClock clock;
    FakePort port;
    FakeFeishu feishu;
    FakeModel model;
    FakeBitable bitable;
    FakeProcess process;
    CountObserver observer;
    std::stop_source source;
    std::ostringstream out;
    std::thread worker;
    int code = -1;

    Running() {
        root = fs::temp_directory_path() / "robot-pm-service" /
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(root / "inbox");
    }

    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;

    ~Running() {
        source.request_stop();
        if (worker.joinable()) {
            worker.join();
        }
        std::error_code error;
        fs::remove_all(root, error);
    }

    void start() {
        robot_pm::ServeDeps deps{clock, port, feishu, model, bitable, process, &observer};
        robot_pm::ServicePaths paths{root, root};
        const std::map<std::string, std::string> env = full_env();
        worker = std::thread([this, env, deps, paths] { code = robot_pm::serve(env, deps, paths, source.get_token(), out); });
    }

    void stop() {
        source.request_stop();
        worker.join();
    }
};

[[nodiscard]] std::vector<std::string> plain_text(const json& card) {
    std::vector<std::string> lines;
    lines.push_back(card.at("header").at("title").at("content").get<std::string>());
    for (const json& element : card.at("elements")) {
        if (element.contains("text")) {
            lines.push_back(element.at("text").at("content").get<std::string>());
        }
        if (element.contains("actions")) {
            for (const json& action : element.at("actions")) {
                lines.push_back(action.at("text").at("content").get<std::string>());
            }
        }
    }
    return lines;
}

void check_short_card(const json& card, const char* title, const char* body, const char* person, const char* first,
                      const char* second) {
    const std::vector<std::string> lines = plain_text(card);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == title);
    CHECK(chinese_count(lines[0]) >= 2);
    CHECK(chinese_count(lines[0]) <= 4);
    CHECK(lines[1] == body);
    CHECK(lines[1].find('\n') == std::string::npos);
    CHECK(chinese_count(lines[1]) <= 20);
    CHECK(lines[1].find(person) == std::string::npos);
    CHECK(lines[0].find(person) == std::string::npos);
    CHECK(lines[2] == person);
    CHECK(lines[3] == first);
    CHECK(lines[4] == second);
    for (const std::string& line : lines) {
        CHECK(line.find("ou_") == std::string::npos);
        CHECK(line.find("secret-value") == std::string::npos);
        CHECK(line.find("allow") == std::string::npos);
        CHECK(line.find("deny") == std::string::npos);
    }
    CHECK(card.at("elements").size() == 3);
}

[[nodiscard]] json meeting_message(const char* chat_mode, const char* group_message_type) {
    return json{{"text", "开会时间 2026-10-01 10:00，北京时间"},
                {"buttons", json::array({"同意", "先不办"})},
                {"pressers", json::array({"ou_a", "ou_b"})},
                {"mentions", json::array({"ou_a", "ou_b", "ou_stranger"})},
                {"chat_id", "oc_group"},
                {"chat_mode", chat_mode},
                {"group_message_type", group_message_type},
                {"actor_name", "李四"},
                {"interaction_id", "tm1"}};
}

void check_not_public(const json& card) {
    const std::string url = card.at("url").get<std::string>();
    CHECK(url.find("receive_id_type=chat_id") == std::string::npos);
    CHECK_FALSE(card.at("body").contains("open_ids"));
}

}  // namespace

TEST_CASE("service stays up until stopped") {
    Running running;
    running.start();
    REQUIRE(running.port.wait_until(1));
    CHECK(running.code == -1);
    CHECK(running.clock.calls >= 1);
    CHECK(running.out.str().empty());
    running.stop();
    CHECK(running.code == 0);
    CHECK(running.out.str().find("robot-pm") == std::string::npos);
}

TEST_CASE("missing setting exits 1 and prints chinese names") {
    ExplodingPort port;
    FakeClock clock;
    FakeFeishu feishu;
    FakeModel model;
    FakeBitable bitable;
    FakeProcess process;
    robot_pm::ServeDeps deps{clock, port, feishu, model, bitable, process, nullptr};
    std::ostringstream out;
    const std::map<std::string, std::string> env{{"FEISHU_APP_ID", "cli_example"},
                                                 {"FEISHU_APP_SECRET", ""},
                                                 {"FEISHU_ENCRYPT_KEY", "encrypt-key"},
                                                 {"FEISHU_VERIFICATION_TOKEN", "verify-token"}};

    const int code = robot_pm::serve(env, deps, {}, {}, out);

    CHECK(code == 1);
    CHECK(out.str() == "应用密钥\n");
    CHECK(out.str().find("secret") == std::string::npos);
    CHECK(out.str().find("cli_example") == std::string::npos);
    CHECK(out.str().find("encrypt-key") == std::string::npos);
    CHECK(clock.calls == 0);

    std::ostringstream all;
    const int empty = robot_pm::serve({}, deps, {}, {}, all);
    CHECK(empty == 1);
    CHECK(all.str() == "应用编号\n应用密钥\n加密密钥\n校验口令\n");
}

TEST_CASE("mention reaches the library and a silent group message does not") {
    Running running;
    running.bitable.role_rows = json::array();
    REQUIRE(robot_pm::enter_group_form(running.root, "oc_group", "普通群").has_value());
    running.start();
    REQUIRE(running.port.wait_until(1));
    running.port.push(signed_request(message_body("group", false, "text")));
    REQUIRE(running.port.wait_until(2));
    CHECK(running.observer.calls == 0);
    CHECK(running.model.calls == 0);
    CHECK(running.feishu.sent.empty());

    running.port.push(signed_request(message_body("group", true, "text")));
    REQUIRE(running.port.wait_until(3));
    CHECK(running.observer.calls == 1);
    CHECK(running.observer.last.at("kind") == "group_message");
    CHECK(running.model.calls == 1);
    CHECK(running.model.last_user.find("secret-value") == std::string::npos);
    CHECK(running.model.last_user.find("encrypt-key") == std::string::npos);
    REQUIRE(running.feishu.sent.size() == 1);
    const json& sent = running.feishu.sent[0];
    CHECK(sent.at("method") == "POST");
    CHECK(sent.at("url") == "https://open.feishu.cn/open-apis/ephemeral/v1/send");
    CHECK(sent.at("body").at("open_id") == "ou_owner");
    CHECK(sent.at("body").at("open_id").is_string());
    CHECK_FALSE(sent.at("body").contains("open_ids"));
    CHECK(sent.at("url").get<std::string>().find("/open-apis/im/v1/messages") == std::string::npos);
    check_short_card(sent.at("body").at("card"), "改进度", "点确认才写入进度", "张三", "确认", "取消");
    running.stop();
}

TEST_CASE("recorded form maps to one delivery and a mismatch sends nothing") {
    fs::path root = fs::temp_directory_path() / "robot-pm-forms";
    fs::remove_all(root);
    fs::create_directories(root);
    REQUIRE(robot_pm::enter_group_form(root, "oc_normal", "普通群").has_value());
    REQUIRE(robot_pm::enter_group_form(root, "oc_topic", "话题群").has_value());
    CHECK(robot_pm::recorded_group_form(root, "oc_normal") == "普通群");
    CHECK(robot_pm::recorded_group_form(root, "oc_topic") == "话题群");
    CHECK_FALSE(robot_pm::recorded_group_form(root, "oc_missing").has_value());
    std::error_code error;
    fs::remove_all(root, error);

    for (const char* group_message_type : {"chat", "thread"}) {
        json message = meeting_message("group", group_message_type);
        message["text"] = "开会时间 secret-value 2026-10-01 10:00，北京时间";
        const robot_pm::CardDispatch sent = robot_pm::present_feishu_cards(message, "普通群");
        CHECK(sent.notice.empty());
        REQUIRE(sent.requests.size() == 2);
        CHECK(sent.requests[0].at("body").at("open_id") == "ou_a");
        CHECK(sent.requests[1].at("body").at("open_id") == "ou_b");
        for (const json& card : sent.requests) {
            CHECK(card.at("method") == "POST");
            CHECK(card.at("url") == "https://open.feishu.cn/open-apis/ephemeral/v1/send");
            CHECK(card.at("body").at("open_id").is_string());
            CHECK(card.at("url").get<std::string>().find("/open-apis/im/v1/messages") == std::string::npos);
            check_not_public(card);
            check_short_card(card.at("body").at("card"), "开会", "点同意才订上这场会", "李四", "同意", "先不办");
            CHECK(card.at("body").at("card").dump().find("secret-value") == std::string::npos);
            CHECK(card.at("body").at("open_id") != "ou_stranger");
        }
    }

    const robot_pm::CardDispatch topic = robot_pm::present_feishu_cards(meeting_message("topic", "chat"), "话题群");
    CHECK(topic.notice.empty());
    REQUIRE(topic.requests.size() == 2);
    CHECK(topic.requests[0].at("body").at("receive_id") == "ou_a");
    CHECK(topic.requests[1].at("body").at("receive_id") == "ou_b");
    for (const json& card : topic.requests) {
        CHECK(card.at("method") == "POST");
        const std::string url = card.at("url").get<std::string>();
        CHECK(url == "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=open_id");
        CHECK(url.find("/open-apis/ephemeral/v1/send") == std::string::npos);
        CHECK(card.at("body").at("receive_id").is_string());
        CHECK(card.at("body").at("receive_id") != "ou_stranger");
        check_not_public(card);
        const json face = json::parse(card.at("body").at("content").get<std::string>());
        check_short_card(face, "开会", "点同意才订上这场会", "李四", "同意", "先不办");
    }

    const robot_pm::CardDispatch missing = robot_pm::present_feishu_cards(meeting_message("group", "chat"), "");
    CHECK(missing.requests.empty());
    CHECK(missing.notice == "形式没记");

    const robot_pm::CardDispatch mismatch = robot_pm::present_feishu_cards(meeting_message("topic", "chat"), "普通群");
    CHECK(mismatch.requests.empty());
    CHECK(mismatch.notice == "形式不符");
    const robot_pm::CardDispatch other = robot_pm::present_feishu_cards(meeting_message("group", "thread"), "话题群");
    CHECK(other.requests.empty());
    CHECK(other.notice == "形式不符");

    json quiet = meeting_message("group", "chat");
    quiet["pressers"] = json::array();
    const robot_pm::CardDispatch nobody = robot_pm::present_feishu_cards(quiet, "普通群");
    CHECK(nobody.notice.empty());
    CHECK(nobody.requests.empty());

    json named = meeting_message("group", "thread");
    named.erase("actor_name");
    named["mention"] = "王五";
    named["pressers"] = json::array({"ou_owner"});
    const robot_pm::CardDispatch fallback = robot_pm::present_feishu_cards(named, "普通群");
    REQUIRE(fallback.requests.size() == 1);
    const std::vector<std::string> lines = plain_text(fallback.requests[0].at("body").at("card"));
    CHECK(lines[2] == "王五");
    CHECK(fallback.requests[0].at("body").at("card").dump().find("ou_") == std::string::npos);
}

TEST_CASE("someone else is refused and the table stays") {
    Running running;
    REQUIRE(robot_pm::begin_interaction(running.root, "ou_owner", "m1", json{{"kind", "status"}, {"item_id", "w1"}, {"status", "doing"}})
                    .has_value());
    running.start();
    REQUIRE(running.port.wait_until(1));
    running.port.push(signed_request(card_body("ou_other", "确认")));
    REQUIRE(running.port.wait_until(2));
    const std::vector<std::string> replies = running.port.reply_text();
    REQUIRE_FALSE(replies.empty());
    CHECK(replies.back() == "{\"toast\":{\"type\":\"error\",\"content\":\"拒\"}}");
    CHECK(replies.back().find("allow") == std::string::npos);
    CHECK(replies.back().find("deny") == std::string::npos);
    CHECK(running.bitable.records[0].at("状态") == "todo");
    CHECK(running.feishu.events.empty());
    running.stop();
}

TEST_CASE("group meeting press follows the task list") {
    Running running;
    REQUIRE(robot_pm::begin_interaction(running.root, "system", "m1",
                                        json{{"kind", "meeting"},
                                             {"item_id", "w1"},
                                             {"title", "接飞书"},
                                             {"start", "2026-10-02 10:00"},
                                             {"end", "2026-10-02 11:00"}})
                    .has_value());
    running.bitable.role_rows = json::array();
    running.start();
    REQUIRE(running.port.wait_until(1));
    running.port.push(signed_request(card_body("ou_owner", "同意")));
    REQUIRE(running.port.wait_until(2));
    CHECK(running.port.reply_text().back().find("拒") != std::string::npos);
    CHECK(running.feishu.events.empty());
    CHECK(running.bitable.records[0].value("决定", "") == "");

    running.bitable.role_rows = json::array(
            {json{{"群id", "oc_group"}, {"人员", json::array({json{{"id", "ou_owner"}}})}, {"职责", "接口"}}});
    running.port.push(signed_request(card_body("ou_other", "同意")));
    REQUIRE(running.port.wait_until(3));
    CHECK(running.port.reply_text().back().find("拒") != std::string::npos);
    CHECK(running.feishu.events.empty());

    running.port.push(signed_request(card_body("ou_owner", "先不办")));
    REQUIRE(running.port.wait_until(4));
    CHECK(running.port.reply_text().back() == "{\"toast\":{\"type\":\"info\",\"content\":\"过\"}}");
    CHECK(running.feishu.events.empty());
    CHECK(running.bitable.records[0].at("决定") == "先不办");
    running.stop();
}

TEST_CASE("a group file is not imported") {
    Running running;
    running.start();
    REQUIRE(running.port.wait_until(1));
    running.port.push(signed_request(message_body("group", true, "file")));
    REQUIRE(running.port.wait_until(2));
    CHECK(running.observer.calls == 1);
    CHECK(running.model.calls == 0);
    CHECK(running.feishu.sent.empty());
    CHECK(fs::is_empty(running.root / "inbox"));
    running.port.push(signed_request(message_body("group", false, "file")));
    REQUIRE(running.port.wait_until(3));
    CHECK(running.observer.calls == 1);
    CHECK(fs::is_empty(running.root / "inbox"));
    running.stop();
}

TEST_CASE("collection form is not broadcast to the group") {
    const robot_pm::CardDispatch sent = robot_pm::present_feishu_cards(robot_pm::build_collection_card(), "");
    CHECK(sent.requests.empty());
    CHECK(sent.notice == "形式没记");
}
