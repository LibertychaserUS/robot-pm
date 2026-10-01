#pragma once

// 这个文件负责确认后预定一场会，时间用北京时间。
// 不变量：同意前不调用日历；日历 id 为空时改用主日历；表没写上就删掉已创建的日程。
// 规格：prompts/meeting_reserve.md，docs/cpp23-standard.md 的日历段落。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace robot_pm {

class ReserveCalendar {
public:
    virtual ~ReserveCalendar() = default;

    // 前置条件：没有配置 ROBOT_PM_CALENDAR_ID。
    // 失败：kBridgeFailed，不创建日程。
    [[nodiscard]] virtual std::expected<std::string, Error> primary_calendar() = 0;

    // 前置条件：event 的开始和结束都带 timezone Asia/Shanghai，并带有 calendar_id 和 attendees。
    // 失败：kBridgeFailed，决定保持未决。
    [[nodiscard]] virtual std::expected<std::string, Error> create_event(const nlohmann::json& event) = 0;

    // 前置条件：create_event 已经返回了 event_id，随后写表失败。
    // 失败：kEditRejected，错误里写明没有回到原状。
    [[nodiscard]] virtual std::expected<void, Error> delete_event(std::string_view calendar_id,
                                                                   std::string_view event_id) = 0;
};

class DecisionTable {
public:
    virtual ~DecisionTable() = default;

    // 前置条件：decision 是「同意」。创建日程已经成功。
    // 失败：kBridgeFailed，调用方必须删除刚创建的日程。
    [[nodiscard]] virtual std::expected<void, Error> set_decision(std::string_view item_id,
                                                                  std::string_view decision) = 0;
};

struct ReserveResult {
    bool confirm_card_sent{false};
    bool calendar_called{false};
    bool table_written{false};
    std::string decision{"未决"};
    nlohmann::json confirm_card;
    nlohmann::json event;
    nlohmann::json plan;
    std::string system_prompt;
    std::string user_message;
};

// 前置条件：prompt 是 prompts/meeting_reserve.md 原文。rows 含 id、owner_role。roles 含 open_id、role。
// 失败：kEditRejected 输出不是一场预定；kForbidden 职责不符；kBridgeFailed 退出码非零。
// 失败时不调用日历，不写决定。
[[nodiscard]] std::expected<ReserveResult, Error> propose_reserve(std::string_view user_payload,
                                                                   std::string_view prompt,
                                                                   std::string_view proposer_open_id,
                                                                   std::string_view speaker_role,
                                                                   const nlohmann::json& rows,
                                                                   const nlohmann::json& roles,
                                                                   ModelAct& model);

// 前置条件：plan 处于 waiting。calendar_id 为空表示没有 ROBOT_PM_CALENDAR_ID。
// 失败：kForbidden 别人确认；kBridgeFailed 主日历或创建失败。创建失败时决定保持未决。
// 写表失败时删除日程。decision 不是「同意」时不调用日历。
[[nodiscard]] std::expected<ReserveResult, Error> confirm_reserve(std::string_view actor_open_id,
                                                                   std::string_view decision,
                                                                   std::string_view calendar_id,
                                                                   const nlohmann::json& plan,
                                                                   ReserveCalendar& calendar,
                                                                   DecisionTable& table);

}  // namespace robot_pm
