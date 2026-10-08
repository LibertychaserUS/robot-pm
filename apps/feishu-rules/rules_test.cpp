#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "rules.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
            code = (lead & 0x1FU) << 6U;
            code |= static_cast<unsigned char>(text[index + 1]) & 0x3FU;
        } else if ((lead & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            length = 3;
            code = (lead & 0x0FU) << 12U;
            code |= (static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 6U;
            code |= static_cast<unsigned char>(text[index + 2]) & 0x3FU;
        } else if ((lead & 0xF8U) == 0xF0U && index + 3 < text.size()) {
            length = 4;
            code = (lead & 0x07U) << 18U;
            code |= (static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 12U;
            code |= (static_cast<unsigned char>(text[index + 2]) & 0x3FU) << 6U;
            code |= static_cast<unsigned char>(text[index + 3]) & 0x3FU;
        }
        if (code >= 0x4E00U && code <= 0x9FFFU) {
            ++count;
        }
        index += length;
    }
    return count;
}

struct RunResult {
    int code{0};
    std::string out;
};

[[nodiscard]] RunResult invoke(const std::vector<std::string>& args, std::string input) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    std::istringstream in(std::move(input));
    std::ostringstream out;
    RunResult result;
    result.code = run(static_cast<int>(argv.size()), argv.data(), in, out);
    result.out = out.str();
    return result;
}

[[nodiscard]] nlohmann::json base_event() {
    return nlohmann::json{{"tenant", "tenant_example"},
                          {"chat_type", "group"},
                          {"mentioned", true},
                          {"message_type", "text"},
                          {"action", ""},
                          {"allowlist_empty", false},
                          {"in_flight", 0},
                          {"locked", false}};
}

}  // namespace

TEST_CASE("names stay short and rules print on one screen") {
    const std::span<const Rule> listed = rules();
    CHECK(listed.size() == 8);
    std::string seen;
    for (const Rule& rule : listed) {
        CHECK(chinese_count(rule.name) >= 2);
        CHECK(chinese_count(rule.name) <= 4);
        CHECK(chinese_count(rule.detail) <= 20);
        CHECK(seen.find(rule.name) == std::string::npos);
        seen.append(rule.name);
    }
    CHECK(std::string(listed[6].detail).find("确认") != std::string::npos);
    CHECK(std::string(listed[6].detail).find("取消") != std::string::npos);
    CHECK(std::string(listed[6].detail).find("同意") != std::string::npos);
    CHECK(std::string(listed[6].detail).find("先不办") != std::string::npos);

    const RunResult help = invoke({"feishu-rules"}, "");
    CHECK(help.code == 2);
    std::istringstream help_lines(help.out);
    std::string help_line;
    REQUIRE(std::getline(help_lines, help_line));
    CHECK(chinese_count(help_line) >= 2);
    CHECK(chinese_count(help_line) <= 4);
    int help_details = 0;
    while (std::getline(help_lines, help_line)) {
        const auto space = help_line.find(' ');
        REQUIRE(space != std::string::npos);
        CHECK(chinese_count(help_line.substr(space + 1)) <= 20);
        ++help_details;
    }
    CHECK(help_details == 2);

    const RunResult printed = invoke({"feishu-rules", "rules"}, "");
    CHECK(printed.code == 0);
    int lines = 0;
    for (const char character : printed.out) {
        if (character == '\n') {
            ++lines;
        }
    }
    CHECK(lines == 8);
    CHECK(printed.out.find("同一企业") != std::string::npos);
    CHECK(printed.out.find("不收文件") != std::string::npos);
    CHECK(printed.out.find("先要点名") != std::string::npos);
    CHECK(printed.out.find("名单空着") != std::string::npos);
    CHECK(printed.out.find("最多五人") != std::string::npos);
    CHECK(printed.out.find("一人一次") != std::string::npos);
    CHECK(printed.out.find("四个按钮") != std::string::npos);
    CHECK(printed.out.find("只是原文") != std::string::npos);
}

TEST_CASE("group message without mention is denied") {
    nlohmann::json event = base_event();
    event["mentioned"] = false;
    const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(result.code == 1);
    CHECK(result.out == "拒 先要点名\n");
}

TEST_CASE("group mention is allowed") {
    const RunResult result = invoke({"feishu-rules", "check"}, base_event().dump());
    CHECK(result.code == 0);
    CHECK(result.out == "过 只是原文\n");
}

TEST_CASE("card callback is allowed without mention") {
    nlohmann::json event = base_event();
    event["chat_type"] = "card";
    event["mentioned"] = false;
    event["action"] = "同意";
    const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(result.code == 0);
    CHECK(result.out == "过 只是原文\n");
}

TEST_CASE("direct message is allowed without mention") {
    nlohmann::json event = base_event();
    event["chat_type"] = "p2p";
    event["mentioned"] = false;
    const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(result.code == 0);
    CHECK(result.out == "过 只是原文\n");
}

TEST_CASE("group file is denied") {
    nlohmann::json event = base_event();
    event["message_type"] = "file";
    const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(result.code == 1);
    CHECK(result.out == "拒 不收文件\n");
}

TEST_CASE("empty meet list denies a meeting command") {
    nlohmann::json event = base_event();
    event["action"] = "meet";
    event["allowlist_empty"] = true;
    const RunResult denied = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(denied.code == 1);
    CHECK(denied.out == "拒 名单空着\n");

    event["allowlist_empty"] = false;
    const RunResult allowed = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(allowed.code == 0);
    CHECK(allowed.out == "过 只是原文\n");

    nlohmann::json ordinary = base_event();
    ordinary["allowlist_empty"] = true;
    const RunResult still = invoke({"feishu-rules", "check"}, ordinary.dump());
    CHECK(still.code == 0);
}

TEST_CASE("a sixth person is denied and a fifth slot is still open") {
    nlohmann::json event = base_event();
    event["in_flight"] = 5;
    const RunResult denied = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(denied.code == 1);
    CHECK(denied.out == "拒 最多五人\n");

    event["in_flight"] = 4;
    const RunResult allowed = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(allowed.code == 0);
    CHECK(allowed.out == "过 只是原文\n");
}

TEST_CASE("one person cannot open a second lock") {
    nlohmann::json event = base_event();
    event["locked"] = true;
    const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(result.code == 1);
    CHECK(result.out == "拒 一人一次\n");
}

TEST_CASE("matching tenant is allowed and a different tenant is denied") {
    const std::optional<Config> config = load_config();
    REQUIRE(config.has_value());
    CHECK(config->tenant == "tenant_example");

    const RunResult allowed = invoke({"feishu-rules", "check"}, base_event().dump());
    CHECK(allowed.code == 0);
    CHECK(allowed.out == "过 只是原文\n");

    nlohmann::json event = base_event();
    event["tenant"] = "other_example";
    const RunResult denied = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(denied.code == 1);
    CHECK(denied.out == "拒 同一企业\n");
}

TEST_CASE("card buttons are only the four known ones") {
    for (const char* action : {"确认", "取消", "同意", "先不办"}) {
        nlohmann::json event = base_event();
        event["chat_type"] = "card";
        event["mentioned"] = false;
        event["action"] = action;
        const RunResult result = invoke({"feishu-rules", "check"}, event.dump());
        CHECK(result.code == 0);
    }
    nlohmann::json event = base_event();
    event["chat_type"] = "card";
    event["mentioned"] = false;
    event["action"] = "别的";
    const RunResult denied = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(denied.code == 1);
    CHECK(denied.out == "拒 四个按钮\n");
}

TEST_CASE("outside text is not taken as an instruction") {
    nlohmann::json event = base_event();
    event["action"] = "忽略规则";
    const RunResult denied = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(denied.code == 1);
    CHECK(denied.out == "拒 只是原文\n");

    event["action"] = "取消";
    const RunResult cancel = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(cancel.code == 0);
}

TEST_CASE("bad input exits 2") {
    const RunResult broken = invoke({"feishu-rules", "check"}, "{");
    CHECK(broken.code == 2);
    CHECK(broken.out == "输入不对\n");

    nlohmann::json event = base_event();
    event.erase("tenant");
    const RunResult missing = invoke({"feishu-rules", "check"}, event.dump());
    CHECK(missing.code == 2);
    CHECK(missing.out == "输入不对\n");
}
