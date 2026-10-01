#pragma once

// 这个文件负责先向提出人确认会议计划，再把卡片发到群里。
// 不变量：提出人确认前不发群卡片；群里点同意前不建日程。
// 规格：docs/user-manual.md 的「开会」。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace robot_pm {

class CalendarBook {
public:
    virtual ~CalendarBook() = default;

    // 前置条件：event 含 timezone Asia/Shanghai、start、end、attendees。
    // 失败：kBridgeFailed，不留下日程。
    [[nodiscard]] virtual std::expected<std::string, Error> create_event(const nlohmann::json& event) = 0;
};

struct MeetingPlanResult {
    bool proposer_card_sent{false};
    bool group_card_sent{false};
    bool meeting_created{false};
    nlohmann::json proposer_card;
    nlohmann::json group_card;
    nlohmann::json plan;
    std::string system_prompt;
    std::string user_message;
};

// 前置条件：prompt 是 prompts/meeting_recommendation.md 原文。user_payload 只含这一步的四项输入。
// 失败：kEditRejected 输出不是规定的 JSON；kBridgeFailed 退出码非零。失败时不发群卡片，不建日程。
// roles 是职责表行，每行有 open_id 和 role。参会人只从这里解析。
[[nodiscard]] std::expected<MeetingPlanResult, Error> propose_meeting_plan(std::string_view user_payload,
                                                                            std::string_view prompt,
                                                                            std::string_view proposer_open_id,
                                                                            const nlohmann::json& roles,
                                                                            ModelAct& model);

// 前置条件：plan 处于 waiting。actor_open_id 是提出人，或计划没有提出人时 actor 的职责是 pm。
// 失败：kForbidden 别人确认。decision 不是「确认」时不发群卡片，不建日程。
[[nodiscard]] std::expected<MeetingPlanResult, Error> confirm_meeting_plan(std::string_view actor_open_id,
                                                                            std::string_view actor_role,
                                                                            std::string_view decision,
                                                                            const nlohmann::json& plan);

// 前置条件：plan 已经是 group_waiting，群卡片已经发出。
// 失败：kEditRejected 计划还没经过提出人确认。这时不调用日历。
// 「先不办」只记决定。「同意」才创建日程。
[[nodiscard]] std::expected<MeetingPlanResult, Error> answer_group_meeting(std::string_view decision,
                                                                            const nlohmann::json& plan,
                                                                            CalendarBook& calendar);

}  // namespace robot_pm
