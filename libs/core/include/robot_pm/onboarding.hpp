#pragma once

// 这个文件负责进群打招呼。
// 不变量：进群只 @ 该成员一次并请他阐述职责；机器人入群不逐个 @；确认前不写职责、不发表单。
// 规格：prompts/onboarding.md，docs/sop/role.md，docs/feishu-access.md。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace robot_pm {

struct OnboardingResult {
    std::string text;
    std::vector<std::string> mention_open_ids;
    bool need_role{false};
    bool collection_card_sent{false};
    nlohmann::json collection_card = nullptr;
    bool confirm_card_sent{false};
    bool meeting_created{false};
    bool table_written{false};
    std::string system_prompt;
    std::string user_message;
};

struct RoleReply {
    std::string open_id;
    std::string text;
    bool mentioned_bot{false};
};

struct RoleConfirmDraft {
    bool confirm_card_sent{false};
    bool collection_card_sent{false};
    bool table_written{false};
    nlohmann::json confirm_card = nullptr;
    std::string open_id;
    std::string role_text;
};

class RoleTable {
public:
    virtual ~RoleTable() = default;

    // 前置条件：open_id 和 role 都非空。失败时不写这一行。
    [[nodiscard]] virtual std::expected<void, Error> write_role(std::string_view open_id,
                                                                std::string_view role) = 0;
};

// 前置条件：identity_prompt、onboarding_prompt 是两个文件的原文；event 是成员入群或机器人入群；has_role 来自职责表。
// 失败：kUnknownEvent 不是入群；kEditRejected JSON 不合格或 need_role 与职责表矛盾；kBridgeFailed 退出码非零。
// 失败时不发卡片、不建会、不写表。need_role 为真时也不发收集表单。
[[nodiscard]] std::expected<OnboardingResult, Error> run_onboarding(const nlohmann::json& event,
                                                                    bool has_role,
                                                                    std::string_view identity_prompt,
                                                                    std::string_view onboarding_prompt,
                                                                    ModelAct& model);

[[nodiscard]] bool is_onboarding_event(const nlohmann::json& event);

// 前置条件：reply.mentioned_bot 为真，text 是那一句职责。
// 失败：kEditRejected 没有 @ 机器人或职责是空的。不写表，不发收集表单。
[[nodiscard]] std::expected<RoleConfirmDraft, Error> accept_role_reply(const RoleReply& reply);

// 前置条件：actor_open_id 等于这句职责的提出人。decision 为「确认」或「取消」。
// 失败：kForbidden 不是提出人。取消不写表。确认时只调用一次 write_role。
[[nodiscard]] std::expected<void, Error> confirm_role(std::string_view owner_open_id,
                                                      std::string_view actor_open_id,
                                                      std::string_view decision,
                                                      std::string_view role,
                                                      RoleTable& table);

}  // namespace robot_pm
