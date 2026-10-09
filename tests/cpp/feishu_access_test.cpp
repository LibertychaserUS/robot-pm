#include "robot_pm/feishu_access.hpp"

#include "robot_pm/crypto.hpp"

#include <algorithm>
#include <doctest/doctest.h>

#include <map>
#include <string>

namespace {

robot_pm::FeishuAccessConfig test_config() {
    robot_pm::FeishuAccessConfig config;
    config.app_id = "cli_example";
    config.app_secret = "secret-value";
    config.encrypt_key = "encrypt-key";
    config.verification_token = "verify-token";
    config.base_url = "https://open.feishu.cn";
    return config;
}

robot_pm::FeishuRequest signed_request(const std::string& body) {
    robot_pm::FeishuRequest request;
    request.timestamp = "1710000000";
    request.nonce = "nonce-1";
    request.body = body;
    request.signature = robot_pm::feishu_event_signature(
        request.timestamp, request.nonce, "encrypt-key", request.body);
    return request;
}

nlohmann::json message_event(const char* chat_type, const char* message_type, bool mention_bot) {
    nlohmann::json body;
    body["schema"] = "2.0";
    body["header"]["event_type"] = "im.message.receive_v1";
    body["header"]["token"] = "verify-token";
    body["event"]["message"]["chat_type"] = chat_type;
    body["event"]["message"]["message_type"] = message_type;
    body["event"]["message"]["content"] = "{\"file_key\":\"file_example\",\"file_name\":\"a.pdf\"}";
    body["event"]["message"]["mentions"] = nlohmann::json::array();
    if (mention_bot) {
        body["event"]["message"]["mentions"].push_back(
            nlohmann::json{{"key", "@_user_1"}, {"id", {{"open_id", "ou_bot"}}}});
    } else {
        body["event"]["message"]["mentions"].push_back(
            nlohmann::json{{"key", "@_user_1"}, {"id", {{"open_id", "ou_other"}}}});
    }
    return body;
}

}  // namespace

TEST_CASE("feishu.signature_is_sha256") {
    CHECK(robot_pm::feishu_event_signature("", "", "", "") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(robot_pm::feishu_event_signature("", "", "", "abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("feishu.credentials_come_from_environment_names_only") {
    const std::map<std::string, std::string> env{{"FEISHU_APP_ID", "cli_example"},
                                                 {"FEISHU_APP_SECRET", ""},
                                                 {"FEISHU_ENCRYPT_KEY", "encrypt-key"},
                                                 {"FEISHU_VERIFICATION_TOKEN", "verify-token"}};

    const auto config = robot_pm::feishu_access_from_environ(env);

    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == robot_pm::ErrorCode::kConfigMissing);
    CHECK(config.error().message == "缺少环境变量：FEISHU_APP_SECRET");
    CHECK(config.error().message.find("encrypt-key") == std::string::npos);
}

TEST_CASE("feishu.bad_signature_is_dropped_before_handling") {
    const auto config = test_config();
    auto request = signed_request(message_event("group", "text", true).dump());
    request.signature = "0000000000000000000000000000000000000000000000000000000000000000";

    const auto decision = robot_pm::admit_feishu_event(config, "ou_bot", request);

    REQUIRE_FALSE(decision.has_value());
    CHECK(decision.error().code == robot_pm::ErrorCode::kForbidden);
    CHECK(decision.error().message.find("encrypt-key") == std::string::npos);
    CHECK(decision.error().message.find("secret-value") == std::string::npos);
}

TEST_CASE("feishu.group_without_bot_mention_is_dropped") {
    const auto decision = robot_pm::admit_feishu_event(
        test_config(), "ou_bot", signed_request(message_event("group", "text", false).dump()));

    REQUIRE(decision.has_value());
    CHECK(decision->dropped);
    CHECK_FALSE(decision->handled);
    CHECK_FALSE(decision->onboarding);
    CHECK_FALSE(decision->import_file);
    CHECK(decision->route == "dropped");
}

TEST_CASE("feishu.group_file_is_not_imported") {
    const auto mentioned = robot_pm::admit_feishu_event(
        test_config(), "ou_bot", signed_request(message_event("group", "file", true).dump()));
    const auto silent = robot_pm::admit_feishu_event(
        test_config(), "ou_bot", signed_request(message_event("group", "file", false).dump()));

    REQUIRE(mentioned.has_value());
    CHECK(mentioned->handled);
    CHECK(mentioned->route == "group_mention");
    CHECK_FALSE(mentioned->import_file);
    REQUIRE(silent.has_value());
    CHECK(silent->dropped);
    CHECK_FALSE(silent->import_file);
}

TEST_CASE("feishu.private_card_and_join_are_handled") {
    nlohmann::json card;
    card["header"]["event_type"] = "card.action.trigger";
    card["header"]["token"] = "verify-token";
    card["event"]["action"]["value"] = "确认";
    const auto card_decision =
        robot_pm::admit_feishu_event(test_config(), "ou_bot", signed_request(card.dump()));
    const auto private_decision = robot_pm::admit_feishu_event(
        test_config(), "ou_bot", signed_request(message_event("p2p", "text", false).dump()));
    nlohmann::json join;
    join["header"]["event_type"] = "im.chat.member.user.added_v1";
    join["header"]["token"] = "verify-token";
    const auto join_decision =
        robot_pm::admit_feishu_event(test_config(), "ou_bot", signed_request(join.dump()));
    nlohmann::json added;
    added["header"]["event_type"] = "im.chat.member.bot.added_v1";
    added["header"]["token"] = "verify-token";
    const auto added_decision =
        robot_pm::admit_feishu_event(test_config(), "ou_bot", signed_request(added.dump()));

    REQUIRE(card_decision.has_value());
    CHECK(card_decision->route == "card_callback");
    CHECK_FALSE(card_decision->import_file);
    REQUIRE(private_decision.has_value());
    CHECK(private_decision->route == "private");
    CHECK_FALSE(private_decision->import_file);
    REQUIRE(join_decision.has_value());
    CHECK(join_decision->onboarding);
    CHECK(join_decision->route == "member_join");
    REQUIRE(added_decision.has_value());
    CHECK(added_decision->onboarding);
    CHECK(added_decision->route == "bot_added");
}

TEST_CASE("feishu.encrypted_callback_is_decrypted_after_the_signature") {
    const std::string plain = nlohmann::json{{"type", "url_verification"},
                                             {"token", "verify-token"},
                                             {"challenge", "challenge-ok"}}
                                      .dump();
    const std::string cipher = robot_pm::encrypt_feishu_payload("encrypt-key", plain);
    const std::string body = nlohmann::json{{"encrypt", cipher}}.dump();
    robot_pm::FeishuRequest request;
    request.timestamp = "1710000000";
    request.nonce = "nonce-1";
    request.body = body;
    request.signature =
            robot_pm::feishu_event_signature(request.timestamp, request.nonce, "encrypt-key", request.body);

    const auto decision = robot_pm::admit_feishu_event(test_config(), "ou_bot", request);

    REQUIRE(decision.has_value());
    CHECK(decision->route == "url_verification");
    CHECK(decision->challenge == "challenge-ok");

    request.signature = "0000000000000000000000000000000000000000000000000000000000000000";
    const auto rejected = robot_pm::admit_feishu_event(test_config(), "ou_bot", request);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == robot_pm::ErrorCode::kForbidden);
}

TEST_CASE("feishu.aes256_matches_the_published_block") {
    const std::uint8_t key[32] = {0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae,
                                  0xf0, 0x85, 0x7d, 0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61,
                                  0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4};
    const std::uint8_t block[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                                    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    const std::uint8_t expect[16] = {0xf3, 0xee, 0xd1, 0xbd, 0xb5, 0xd2, 0xa0, 0x3c,
                                     0x06, 0x4b, 0x5a, 0x7e, 0x3d, 0xb1, 0x81, 0xf8};
    const auto cipher = robot_pm::aes256_encrypt_block(key, block);
    CHECK(std::equal(cipher.begin(), cipher.end(), expect));
}

TEST_CASE("feishu.send_uses_tenant_access_token") {
    const auto config = test_config();
    const auto token = robot_pm::feishu_tenant_token_request(config);
    const auto sent = robot_pm::feishu_send_request(
        config, "tat_example", "ou_example", {{"title", "确认"}});

    CHECK(token.method == "POST");
    CHECK(token.url == "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal");
    CHECK(token.body.at("app_id") == "cli_example");
    REQUIRE(sent.has_value());
    CHECK(sent->authorization == "Bearer tat_example");
    CHECK(sent->authorization.find("secret-value") == std::string::npos);
    CHECK(sent->body.at("msg_type") == "interactive");
    CHECK(sent->body.dump().find("secret-value") == std::string::npos);
    CHECK(sent->url == "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=open_id");
}
