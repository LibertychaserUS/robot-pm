#pragma once

// 这个文件负责回答群成员的一句话。
// 不变量：系统提示只是 member_reply.md 原文；用户消息包在 untrusted_input 里；不建会，不写表。
// 规格：prompts/member_reply.md，docs/user-manual.md 的「能问什么」。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace robot_pm {

struct MemberReplyResult {
    std::string text;
    bool meeting_created{false};
    bool table_written{false};
    std::string system_prompt;
    std::string user_message;
};

// 前置条件：member_reply_prompt 是 prompts/member_reply.md 的原文；user_payload 只含成员的话、职责、进度和未决卡片。
// 失败：kEditRejected 输出不是 {"text":"..."}，或输入试图改写提示边界；kBridgeFailed 退出码非零。失败时不建会、不写表。
[[nodiscard]] std::expected<MemberReplyResult, Error> run_member_reply(std::string_view user_payload,
                                                                        std::string_view member_reply_prompt,
                                                                        ModelAct& model);

}  // namespace robot_pm
