#include "robot_pm/feishu_access.hpp"

#include "robot_pm/crypto.hpp"

#include <array>
#include <map>
#include <vector>

namespace robot_pm {
namespace {

[[nodiscard]] bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        diff = static_cast<unsigned char>(diff | static_cast<unsigned char>(left[index] ^ right[index]));
    }
    return diff == 0;
}

[[nodiscard]] std::string lower_copy(std::string_view text) {
    std::string copy(text);
    for (char& character : copy) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return copy;
}

[[nodiscard]] const nlohmann::json* event_message(const nlohmann::json& body) {
    if (!body.contains("event") || !body["event"].is_object()) {
        return nullptr;
    }
    const nlohmann::json& event = body["event"];
    if (event.contains("message") && event["message"].is_object()) {
        return &event["message"];
    }
    return nullptr;
}

[[nodiscard]] bool mentions_bot(const nlohmann::json& message, std::string_view bot_open_id) {
    if (bot_open_id.empty() || !message.contains("mentions") || !message["mentions"].is_array()) {
        return false;
    }
    for (const nlohmann::json& mention : message["mentions"]) {
        if (!mention.is_object() || !mention.contains("id") || !mention["id"].is_object()) {
            continue;
        }
        const nlohmann::json& id = mention["id"];
        if (id.contains("open_id") && id["open_id"].is_string() &&
            id["open_id"].get_ref<const std::string&>() == bot_open_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string event_token(const nlohmann::json& body) {
    if (body.contains("header") && body["header"].is_object() && body["header"].contains("token") &&
        body["header"]["token"].is_string()) {
        return body["header"]["token"].get<std::string>();
    }
    if (body.contains("token") && body["token"].is_string()) {
        return body["token"].get<std::string>();
    }
    return {};
}

[[nodiscard]] std::string event_type_of(const nlohmann::json& body) {
    if (body.contains("type") && body["type"].is_string()) {
        return body["type"].get<std::string>();
    }
    if (body.contains("header") && body["header"].is_object() && body["header"].contains("event_type") &&
        body["header"]["event_type"].is_string()) {
        return body["header"]["event_type"].get<std::string>();
    }
    return {};
}

[[nodiscard]] bool body_contains_secret(const nlohmann::json& body, const FeishuAccessConfig& config) {
    const std::string dumped = body.dump();
    return (!config.app_secret.empty() && dumped.find(config.app_secret) != std::string::npos) ||
           (!config.encrypt_key.empty() && dumped.find(config.encrypt_key) != std::string::npos) ||
           (!config.verification_token.empty() && dumped.find(config.verification_token) != std::string::npos);
}

}  // namespace

std::string feishu_event_signature(std::string_view timestamp,
                                  std::string_view nonce,
                                  std::string_view encrypt_key,
                                  std::string_view body) {
    std::string material;
    material.reserve(timestamp.size() + nonce.size() + encrypt_key.size() + body.size());
    material.append(timestamp);
    material.append(nonce);
    material.append(encrypt_key);
    material.append(body);
    return sha256_hex(material);
}

[[nodiscard]] std::string trim_setting(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                           text[end - 1] == '\r')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::expected<FeishuAccessConfig, Error> feishu_access_from_environ(
    const std::map<std::string, std::string>& env) {
    static constexpr std::array<const char*, 4> kRequired{
        "FEISHU_APP_ID", "FEISHU_APP_SECRET", "FEISHU_ENCRYPT_KEY", "FEISHU_VERIFICATION_TOKEN"};
    std::string missing;
    for (const char* name : kRequired) {
        const auto found = env.find(name);
        if (found == env.end() || trim_setting(found->second).empty()) {
            if (!missing.empty()) {
                missing.append(", ");
            }
            missing.append(name);
        }
    }
    if (!missing.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "缺少环境变量：" + missing});
    }
    FeishuAccessConfig config;
    config.app_id = trim_setting(env.at("FEISHU_APP_ID"));
    config.app_secret = trim_setting(env.at("FEISHU_APP_SECRET"));
    config.encrypt_key = trim_setting(env.at("FEISHU_ENCRYPT_KEY"));
    config.verification_token = trim_setting(env.at("FEISHU_VERIFICATION_TOKEN"));
    const auto base = env.find("FEISHU_BASE_URL");
    if (base != env.end() && !trim_setting(base->second).empty()) {
        config.base_url = trim_setting(base->second);
        while (!config.base_url.empty() && config.base_url.back() == '/') {
            config.base_url.pop_back();
        }
    }
    return config;
}

std::expected<AccessDecision, Error> admit_feishu_event(const FeishuAccessConfig& config,
                                                       std::string_view bot_open_id,
                                                       const FeishuRequest& request) {
    if (request.timestamp.empty() || request.nonce.empty() || request.signature.empty()) {
        return std::unexpected(Error{ErrorCode::kForbidden, "事件验签失败"});
    }
    const std::string expected = feishu_event_signature(
        request.timestamp, request.nonce, config.encrypt_key, request.body);
    if (!same_text(lower_copy(request.signature), expected)) {
        return std::unexpected(Error{ErrorCode::kForbidden, "事件验签失败"});
    }
    nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "事件不是 JSON"});
    }
    if (body.contains("encrypt") && body.at("encrypt").is_string() && !body.contains("header") &&
        !body.contains("type")) {
        const std::expected<std::string, Error> plain =
                decrypt_feishu_payload(config.encrypt_key, body.at("encrypt").get_ref<const std::string&>());
        if (!plain) {
            return std::unexpected(plain.error());
        }
        body = nlohmann::json::parse(*plain, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "事件不是 JSON"});
        }
    }
    if (!same_text(event_token(body), config.verification_token)) {
        return std::unexpected(Error{ErrorCode::kForbidden, "事件验签失败"});
    }

    AccessDecision decision;
    const std::string type = event_type_of(body);
    if (type == "url_verification") {
        if (!body.contains("challenge") || !body["challenge"].is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "地址校验缺少 challenge"});
        }
        decision.route = "url_verification";
        decision.challenge = body["challenge"].get<std::string>();
        return decision;
    }
    if (type == "card.action.trigger") {
        decision.handled = true;
        decision.route = "card_callback";
        return decision;
    }
    if (type == "im.chat.member.user.added_v1") {
        decision.handled = true;
        decision.onboarding = true;
        decision.route = "member_join";
        return decision;
    }
    if (type == "im.chat.member.bot.added_v1") {
        decision.handled = true;
        decision.onboarding = true;
        decision.route = "bot_added";
        return decision;
    }
    if (type != "im.message.receive_v1") {
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "不处理这个飞书事件"});
    }
    const nlohmann::json* message = event_message(body);
    if (message == nullptr || !message->contains("chat_type") || !(*message)["chat_type"].is_string()) {
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "不处理这个飞书事件"});
    }
    const std::string& chat_type = (*message)["chat_type"].get_ref<const std::string&>();
    if (chat_type == "p2p") {
        decision.handled = true;
        decision.route = "private";
        return decision;
    }
    if (chat_type == "group") {
        if (!mentions_bot(*message, bot_open_id)) {
            decision.dropped = true;
            decision.route = "dropped";
            return decision;
        }
        decision.handled = true;
        decision.route = "group_mention";
        return decision;
    }
    return std::unexpected(Error{ErrorCode::kUnknownEvent, "不处理这个飞书事件"});
}

TenantTokenRequest feishu_tenant_token_request(const FeishuAccessConfig& config) {
    TenantTokenRequest request;
    request.method = "POST";
    request.url = config.base_url + "/open-apis/auth/v3/tenant_access_token/internal";
    request.body = {{"app_id", config.app_id}, {"app_secret", config.app_secret}};
    return request;
}

std::expected<SendRequest, Error> feishu_send_request(const FeishuAccessConfig& config,
                                                     std::string_view tenant_access_token,
                                                     std::string_view receive_id,
                                                     const nlohmann::json& card) {
    if (tenant_access_token.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "缺少 tenant_access_token"});
    }
    SendRequest request;
    request.method = "POST";
    request.url = config.base_url + "/open-apis/im/v1/messages?receive_id_type=open_id";
    request.authorization = std::string("Bearer ") + std::string(tenant_access_token);
    request.body = {{"receive_id", std::string(receive_id)},
                    {"msg_type", "interactive"},
                    {"content", card.dump()}};
    if (body_contains_secret(request.body, config) ||
        request.authorization.find(config.app_secret) != std::string::npos) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "发送内容里不能带凭证"});
    }
    return request;
}

}  // namespace robot_pm
