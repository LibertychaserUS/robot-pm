#include "robot_pm/feishu_access.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace robot_pm {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Round{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, std::uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

void sha256_block(std::array<std::uint32_t, 8>& state, const std::uint8_t block[64]) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                       static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t small = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                                    (words[index - 15] >> 3U);
        const std::uint32_t large = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                                    (words[index - 2] >> 10U);
        words[index] = words[index - 16] + small + words[index - 7] + large;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + choose + kSha256Round[index] + words[index];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

[[nodiscard]] std::array<std::uint8_t, 32> sha256_bytes(std::string_view input) {
    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> padded(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    padded.push_back(0x80);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bit_length >> static_cast<unsigned>(shift)) & 0xffU));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        sha256_block(state, padded.data() + offset);
    }
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4] = static_cast<std::uint8_t>((state[index] >> 24U) & 0xffU);
        digest[index * 4 + 1] = static_cast<std::uint8_t>((state[index] >> 16U) & 0xffU);
        digest[index * 4 + 2] = static_cast<std::uint8_t>((state[index] >> 8U) & 0xffU);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(state[index] & 0xffU);
    }
    return digest;
}

[[nodiscard]] std::string hex_lower(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        text[index * 2] = kDigits[digest[index] >> 4U];
        text[index * 2 + 1] = kDigits[digest[index] & 0x0fU];
    }
    return text;
}

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
    return hex_lower(sha256_bytes(material));
}

std::expected<FeishuAccessConfig, Error> feishu_access_from_environ(
    const std::map<std::string, std::string>& env) {
    static constexpr std::array<const char*, 4> kRequired{
        "FEISHU_APP_ID", "FEISHU_APP_SECRET", "FEISHU_ENCRYPT_KEY", "FEISHU_VERIFICATION_TOKEN"};
    std::string missing;
    for (const char* name : kRequired) {
        const auto found = env.find(name);
        if (found == env.end() || found->second.empty()) {
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
    config.app_id = env.at("FEISHU_APP_ID");
    config.app_secret = env.at("FEISHU_APP_SECRET");
    config.encrypt_key = env.at("FEISHU_ENCRYPT_KEY");
    config.verification_token = env.at("FEISHU_VERIFICATION_TOKEN");
    const auto base = env.find("FEISHU_BASE_URL");
    if (base != env.end() && !base->second.empty()) {
        config.base_url = base->second;
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
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "事件不是 JSON"});
    }
    if (body.contains("encrypt") && !body.contains("header") && !body.contains("type")) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "加密事件验签通过后仍不直接处理"});
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
