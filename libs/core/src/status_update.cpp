#include "robot_pm/status_update.hpp"

#include <string>

namespace robot_pm {
namespace {

[[nodiscard]] std::string trim_ascii_space(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\n' || text[begin] == '\r' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\r' ||
                           text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::expected<std::string, Error> wrap_untrusted_input(std::string_view payload) {
    if (payload.find("<untrusted_input>") != std::string_view::npos ||
        payload.find("</untrusted_input>") != std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "输入不能改写提示边界"});
    }
    std::string wrapped;
    wrapped.reserve(payload.size() + 40);
    wrapped.append("<untrusted_input>\n");
    wrapped.append(payload);
    wrapped.append("\n</untrusted_input>");
    return wrapped;
}

[[nodiscard]] bool is_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

[[nodiscard]] bool allowed_status(std::string_view status) {
    return status == "todo" || status == "doing" || status == "done" || status == "blocked";
}

[[nodiscard]] const nlohmann::json* find_row(const nlohmann::json& rows, std::string_view item_id) {
    if (!rows.is_array()) {
        return nullptr;
    }
    for (const nlohmann::json& row : rows) {
        if (row.is_object() && row.contains("id") && row["id"].is_string() &&
            row["id"].get_ref<const std::string&>() == item_id) {
            return &row;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string shown_item(const nlohmann::json& row) {
    if (row.contains("title") && row["title"].is_string()) {
        const std::string& title = row["title"].get_ref<const std::string&>();
        if (!title.empty()) {
            return title;
        }
    }
    return "这项";
}

[[nodiscard]] std::string shown_status(std::string_view status) {
    if (status == "todo") {
        return "未开始";
    }
    if (status == "doing") {
        return "进行中";
    }
    if (status == "done") {
        return "完成";
    }
    if (status == "blocked") {
        return "卡住";
    }
    return "新进度";
}

[[nodiscard]] bool forbidden_key(std::string_view key) {
    return key == "title" || key == "predecessors" || key == "owner_role" || key == "node" ||
           key == "标题" || key == "前置" || key == "职责";
}

[[nodiscard]] nlohmann::json confirm_card(std::string_view summary, const nlohmann::json& plan) {
    const auto button = [&](const char* label) {
        return nlohmann::json{{"tag", "button"},
                              {"text", {{"tag", "plain_text"}, {"content", label}}},
                              {"type", "primary"},
                              {"value", {{"action", label}, {"item_id", plan.at("item_id")}}}};
    };
    return nlohmann::json{
        {"msg_type", "interactive"},
        {"card",
         {{"header", {{"title", {{"tag", "plain_text"}, {"content", "改进度"}}}}},
          {"elements",
           nlohmann::json::array(
               {{{"tag", "div"}, {"text", {{"tag", "plain_text"}, {"content", summary}}}},
                {{"tag", "action"}, {"actions", nlohmann::json::array({button("同意"), button("取消")})}}})}}}};
}

[[nodiscard]] std::expected<nlohmann::json, Error> fields_from_output(const nlohmann::json& output,
                                                                      const nlohmann::json& row) {
    const bool has_status = output.contains("status");
    const bool has_start = output.contains("start");
    const bool has_end = output.contains("end");
    if (has_status && (has_start || has_end)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "一次只改状态或只改日期"});
    }
    if (!has_status && !has_start && !has_end) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有可写入的状态或日期"});
    }
    nlohmann::json fields = {{"业务id", row["id"]}};
    if (has_status) {
        if (!output["status"].is_string() || !allowed_status(output["status"].get_ref<const std::string&>())) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "状态不在规格里"});
        }
        fields["状态"] = output["status"];
    }
    if (has_start || has_end) {
        const std::string node = row.contains("node") && row["node"].is_string()
                                     ? row["node"].get<std::string>()
                                     : std::string{};
        if (node == "deadline" || node == "release") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "大节点和发布节点的日期不能改"});
        }
        if (node != "flexible") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "只有小节点的日期可以改"});
        }
        if (!row.contains("start") || !row["start"].is_string() || !row.contains("end") || !row["end"].is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "日期不在规格里"});
        }
        if ((has_start && !output["start"].is_string()) || (has_end && !output["end"].is_string())) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "日期不在规格里"});
        }
        const std::string start =
            has_start ? output["start"].get<std::string>() : row["start"].get<std::string>();
        const std::string end = has_end ? output["end"].get<std::string>() : row["end"].get<std::string>();
        if (!is_date(start) || !is_date(end) || end < start) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "日期不在规格里"});
        }
        if (has_start) {
            fields["开始"] = start;
        }
        if (has_end) {
            fields["结束"] = end;
        }
    }
    return fields;
}

}  // namespace

std::expected<StatusUpdateResult, Error> propose_status_update(std::string_view user_payload,
                                                               std::string_view prompt,
                                                               std::string_view proposer_open_id,
                                                               std::string_view speaker_role,
                                                               const nlohmann::json& rows,
                                                               ModelAct& model) {
    StatusUpdateResult result;
    if (prompt.empty() || proposer_open_id.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "状态修改缺少提示或提出人"});
    }
    result.system_prompt = std::string(prompt);
    const std::expected<std::string, Error> wrapped = wrap_untrusted_input(user_payload);
    if (!wrapped.has_value()) {
        return std::unexpected(wrapped.error());
    }
    result.user_message = *wrapped;
    const std::expected<ModelResponse, Error> response =
        model.run(ModelRequest{result.system_prompt, result.user_message});
    if (!response.has_value()) {
        return std::unexpected(response.error());
    }
    if (response->exit_code != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "状态修改这一步失败，已丢弃输出"});
    }
    const std::string trimmed = trim_ascii_space(response->stdout_text);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "状态修改输出不是规定的 JSON"});
    }
    const nlohmann::json output = nlohmann::json::parse(trimmed, nullptr, false);
    if (output.is_discarded() || !output.is_object() || !output.contains("action") || !output["action"].is_string()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "状态修改输出不是规定的 JSON"});
    }
    const std::string& action = output["action"].get_ref<const std::string&>();
    for (auto it = output.begin(); it != output.end(); ++it) {
        const bool update_key = action == "update" &&
                                (it.key() == "action" || it.key() == "item_id" || it.key() == "status" ||
                                 it.key() == "start" || it.key() == "end");
        const bool clarify_key = action == "clarify" && (it.key() == "action" || it.key() == "text");
        const bool none_key = action == "none" && it.key() == "action";
        if (update_key || clarify_key || none_key) {
            continue;
        }
        if (forbidden_key(it.key())) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "标题和前置不能在对话里改"});
        }
        return std::unexpected(Error{ErrorCode::kEditRejected, "状态修改输出不是规定的 JSON"});
    }
    if (action == "none") {
        if (output.size() != 1) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "无关修改不能带其它字段"});
        }
        return result;
    }
    if (action == "clarify") {
        if (output.size() != 2 || !output.contains("text") || !output["text"].is_string() ||
            output["text"].get_ref<const std::string&>().empty()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "追问只能有一句话"});
        }
        result.text = output["text"].get<std::string>();
        return result;
    }
    if (action != "update") {
        return std::unexpected(Error{ErrorCode::kEditRejected, "状态修改输出不是规定的 JSON"});
    }
    if (!output.contains("item_id") || !output["item_id"].is_string()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "工作项不在表里"});
    }
    const nlohmann::json* row = find_row(rows, output["item_id"].get_ref<const std::string&>());
    if (row == nullptr) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "工作项不在表里"});
    }
    if (speaker_role.empty() ||
        (speaker_role != "pm" &&
         (!row->contains("owner_role") || !(*row)["owner_role"].is_string() ||
          (*row)["owner_role"].get_ref<const std::string&>() != speaker_role))) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有职责匹配的人或 pm 可以改"});
    }
    const std::expected<nlohmann::json, Error> fields = fields_from_output(output, *row);
    if (!fields.has_value()) {
        return std::unexpected(fields.error());
    }
    const std::string node =
        row->contains("node") && (*row)["node"].is_string() ? (*row)["node"].get<std::string>() : std::string{};
    result.plan = {{"progress", "waiting"},
                   {"proposer", std::string(proposer_open_id)},
                   {"item_id", (*row)["id"]},
                   {"node", node},
                   {"fields", *fields}};
    std::string summary = shown_item(*row);
    if (fields->contains("状态") && (*fields)["状态"].is_string()) {
        summary += "改为" + shown_status((*fields)["状态"].get_ref<const std::string&>());
    }
    const bool has_start = fields->contains("开始") && (*fields)["开始"].is_string();
    const bool has_end = fields->contains("结束") && (*fields)["结束"].is_string();
    if (has_start) {
        summary += "开始改为" + (*fields)["开始"].get<std::string>();
    }
    if (has_end) {
        if (has_start) {
            summary += "，";
        }
        summary += "结束改为" + (*fields)["结束"].get<std::string>();
    }
    result.confirm_card = confirm_card(summary, result.plan);
    result.confirm_card_sent = true;
    result.table_written = false;
    return result;
}

std::expected<StatusUpdateResult, Error> confirm_status_update(std::string_view actor_open_id,
                                                               std::string_view decision,
                                                               const nlohmann::json& plan,
                                                               StatusLedger& ledger) {
    StatusUpdateResult result;
    result.plan = plan;
    if (!plan.is_object() || !plan.contains("proposer") || !plan["proposer"].is_string() ||
        !plan.contains("fields") || !plan["fields"].is_object() || !plan.contains("node") ||
        !plan["node"].is_string() || !plan.contains("progress") || !plan["progress"].is_string() ||
        plan["progress"].get_ref<const std::string&>() != "waiting") {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有等待中的修改计划"});
    }
    if (actor_open_id != plan["proposer"].get_ref<const std::string&>()) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有提出人能确认这次修改"});
    }
    if (decision != "同意") {
        result.plan["progress"] = "cancelled";
        return result;
    }
    const nlohmann::json& fields = plan["fields"];
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const std::string& key = it.key();
        if (key != "业务id" && key != "状态" && key != "开始" && key != "结束") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "标题和前置不能在对话里改"});
        }
    }
    const bool date_change = fields.contains("开始") || fields.contains("结束");
    if (fields.contains("状态") && date_change) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "一次只改状态或只改日期"});
    }
    if (date_change && plan["node"].get_ref<const std::string&>() != "flexible") {
        return std::unexpected(Error{ErrorCode::kEditRejected, "大节点和发布节点的日期不能改"});
    }
    if (date_change) {
        const auto illegal_date = [&](const char* key) {
            return fields.contains(key) &&
                   (!fields[key].is_string() || !is_date(fields[key].get_ref<const std::string&>()));
        };
        if (illegal_date("开始") || illegal_date("结束")) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "日期不在规格里"});
        }
        if (fields.contains("开始") && fields.contains("结束") &&
            fields["结束"].get_ref<const std::string&>() < fields["开始"].get_ref<const std::string&>()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "日期不在规格里"});
        }
    }
    if (fields.contains("状态") &&
        (!fields["状态"].is_string() || !allowed_status(fields["状态"].get_ref<const std::string&>()))) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "状态不在规格里"});
    }
    const std::expected<void, Error> written = ledger.update_item(fields);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    result.table_written = true;
    result.plan["progress"] = "done";
    return result;
}

}  // namespace robot_pm
