#pragma once

// 这个文件负责把对话收成一次状态或小节点日期的确认计划。
// 不变量：同意前不写表；deadline 和 release 的日期不改；标题和前置不改。
// 规格：docs/sop.md 的「更新状态」，prompts/status_update.md。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace robot_pm {

class StatusLedger {
public:
    virtual ~StatusLedger() = default;

    // 前置条件：fields 只含 业务id，以及 状态 或 开始、结束。
    // 失败：kBridgeFailed，表保持调用前的样子。
    [[nodiscard]] virtual std::expected<void, Error> update_item(const nlohmann::json& fields) = 0;
};

struct StatusUpdateResult {
    bool confirm_card_sent{false};
    bool table_written{false};
    nlohmann::json confirm_card;
    nlohmann::json plan;
    std::string text;
    std::string system_prompt;
    std::string user_message;
};

// 前置条件：prompt 是 prompts/status_update.md 原文。rows 是工作项数组，每项有 id、owner_role、node、status、start、end。
// speaker_role 是职责表里的职责，不从模型输出里取。
// 失败：kEditRejected 输出改了标题、前置、职责，或改了非 flexible 的日期；kForbidden 职责不符；kBridgeFailed 退出码非零。
// 失败时不发确认卡片，不写表。
[[nodiscard]] std::expected<StatusUpdateResult, Error> propose_status_update(
    std::string_view user_payload,
    std::string_view prompt,
    std::string_view proposer_open_id,
    std::string_view speaker_role,
    const nlohmann::json& rows,
    ModelAct& model);

// 前置条件：plan 是 propose_status_update 留下的计划。actor_open_id 必须是提出人。
// 失败：kForbidden 别人确认；kEditRejected 计划里夹带标题或前置，或日期不属于 flexible。
// decision 不是「同意」时不写表。
[[nodiscard]] std::expected<StatusUpdateResult, Error> confirm_status_update(
    std::string_view actor_open_id,
    std::string_view decision,
    const nlohmann::json& plan,
    StatusLedger& ledger);

}  // namespace robot_pm
