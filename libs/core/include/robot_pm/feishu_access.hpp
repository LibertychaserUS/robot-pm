#pragma once

// 这个文件负责飞书事件验签和接入分流。
// 不变量：先验签再处理；群消息没有 @ 机器人就丢掉；飞书消息里的文件不导入。
// 规格：docs/user-manual.md 的「怎么跟它说话」。

#include "robot_pm/error.hpp"

#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace robot_pm {

struct FeishuAccessConfig {
    std::string app_id;
    std::string app_secret;
    std::string encrypt_key;
    std::string verification_token;
    std::string base_url{"https://open.feishu.cn"};
};

struct FeishuRequest {
    std::string timestamp;
    std::string nonce;
    std::string signature;
    std::string body;
};

struct AccessDecision {
    bool handled{false};
    bool dropped{false};
    bool onboarding{false};
    bool import_file{false};
    std::string route;
    std::string challenge;
};

struct TenantTokenRequest {
    std::string method;
    std::string url;
    nlohmann::json body;
};

struct SendRequest {
    std::string method;
    std::string url;
    std::string authorization;
    nlohmann::json body;
};

// 前置条件：timestamp、nonce、encrypt_key、body 按飞书文档拼接。
// 失败：不失败。结果是小写十六进制 SHA-256。
[[nodiscard]] std::string feishu_event_signature(std::string_view timestamp,
                                                 std::string_view nonce,
                                                 std::string_view encrypt_key,
                                                 std::string_view body);

// 前置条件：env 只来自进程环境，不读仓库里的 .env。空字符串算缺失。
// 失败：kConfigMissing，消息只列出变量名。
[[nodiscard]] std::expected<FeishuAccessConfig, Error> feishu_access_from_environ(
    const std::map<std::string, std::string>& env);

// 前置条件：config 含验签凭证；bot_open_id 是这个机器人的 open_id，不是密钥。
// 失败：kForbidden 验签或 token 不对；kUnknownEvent 验签通过但不是要处理的事件；kEditRejected 正文不是可处理的 JSON。
// 失败时不导入文件、不进入 onboarding。
[[nodiscard]] std::expected<AccessDecision, Error> admit_feishu_event(const FeishuAccessConfig& config,
                                                                      std::string_view bot_open_id,
                                                                      const FeishuRequest& request);

// 前置条件：config.base_url 没有末尾斜杠。
// 失败：不失败。这只是取 tenant_access_token 的请求，不访问网络。
[[nodiscard]] TenantTokenRequest feishu_tenant_token_request(const FeishuAccessConfig& config);

// 前置条件：tenant_access_token 非空。card 不含凭证。
// 失败：kConfigMissing 没有 token；kEditRejected 正文里出现了凭证。
[[nodiscard]] std::expected<SendRequest, Error> feishu_send_request(const FeishuAccessConfig& config,
                                                                   std::string_view tenant_access_token,
                                                                   std::string_view receive_id,
                                                                   const nlohmann::json& card);

}  // namespace robot_pm
