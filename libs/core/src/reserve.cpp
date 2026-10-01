#include "robot_pm/reserve.hpp"

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

[[nodiscard]] bool digits_at(std::string_view text, std::size_t index, std::size_t count) {
    if (index + count > text.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset < count; ++offset) {
        if (text[index + offset] < '0' || text[index + offset] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_date(std::string_view text) {
    return text.size() == 10 && text[4] == '-' && text[7] == '-' && digits_at(text, 0, 4) &&
           digits_at(text, 5, 2) && digits_at(text, 8, 2);
}

[[nodiscard]] bool is_clock(std::string_view text) {
    return text.size() == 16 && text[10] == ' ' && text[13] == ':' && is_date(text.substr(0, 10)) &&
           digits_at(text, 11, 2) && digits_at(text, 14, 2);
}

[[nodiscard]] std::string clock_or_empty(std::string_view raw, std::string_view date_only_clock) {
    if (is_clock(raw)) {
        return std::string(raw);
    }
    if (is_date(raw)) {
        std::string clock(raw);
        clock.push_back(' ');
        clock.append(date_only_clock);
        return clock;
    }
    return {};
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

[[nodiscard]] nlohmann::json attendees_for(const nlohmann::json& roles, std::string_view owner_role) {
    nlohmann::json attendees = nlohmann::json::array();
    if (!roles.is_array()) {
        return attendees;
    }
    for (const nlohmann::json& role : roles) {
        if (!role.is_object() || !role.contains("open_id") || !role["open_id"].is_string() ||
            !role.contains("role") || !role["role"].is_string()) {
            continue;
        }
        if (role["role"].get_ref<const std::string&>() == owner_role) {
            attendees.push_back(role["open_id"]);
        }
    }
    return attendees;
}

[[nodiscard]] nlohmann::json confirm_card(const nlohmann::json& plan) {
    const auto button = [](const char* label, const nlohmann::json& item_id) {
        return nlohmann::json{{"tag", "button"},
                              {"text", {{"tag", "plain_text"}, {"content", label}}},
                              {"type", "primary"},
                              {"value", {{"action", label}, {"item_id", item_id}}}};
    };
    const std::string body = "将预定 " + plan["title"].get<std::string>() + " " +
                             plan["start"].get<std::string>() + " 至 " + plan["end"].get<std::string>() +
                             "（北京时间）";
    return nlohmann::json{
        {"msg_type", "interactive"},
        {"card",
         {{"header", {{"title", {{"tag", "plain_text"}, {"content", "确认预定"}}}}},
          {"elements",
           nlohmann::json::array(
               {{{"tag", "div"}, {"text", {{"tag", "plain_text"}, {"content", body}}}},
                {{"tag", "action"},
                 {"actions", nlohmann::json::array({button("同意", plan["item_id"]), button("取消", plan["item_id"])})}}})}}}};
}

[[nodiscard]] bool allowed_reserve_key(std::string_view key) {
    return key == "action" || key == "item_id" || key == "start" || key == "end" || key == "title";
}

}  // namespace

std::expected<ReserveResult, Error> propose_reserve(std::string_view user_payload,
                                                    std::string_view prompt,
                                                    std::string_view proposer_open_id,
                                                    std::string_view speaker_role,
                                                    const nlohmann::json& rows,
                                                    const nlohmann::json& roles,
                                                    ModelAct& model) {
    ReserveResult result;
    if (prompt.empty() || proposer_open_id.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "预定缺少提示或提出人"});
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
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "预定这一步失败，已丢弃输出"});
    }
    const std::string trimmed = trim_ascii_space(response->stdout_text);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "预定输出不是规定的 JSON"});
    }
    const nlohmann::json output = nlohmann::json::parse(trimmed, nullptr, false);
    if (output.is_discarded() || !output.is_object() || !output.contains("action") || !output["action"].is_string()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "预定输出不是规定的 JSON"});
    }
    const std::string& action = output["action"].get_ref<const std::string&>();
    for (auto it = output.begin(); it != output.end(); ++it) {
        const bool none_key = action == "none" && it.key() == "action";
        const bool clarify_key = action == "clarify" && (it.key() == "action" || it.key() == "text");
        const bool reserve_key = action == "reserve" && allowed_reserve_key(it.key());
        if (!none_key && !clarify_key && !reserve_key) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "预定输出不是规定的 JSON"});
        }
    }
    if (action == "none") {
        return result;
    }
    if (action == "clarify") {
        if (!output.contains("text") || !output["text"].is_string() || output["text"].get_ref<const std::string&>().empty()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "追问只能有一句话"});
        }
        result.user_message = *wrapped;
        return result;
    }
    if (action != "reserve" || !output.contains("item_id") || !output["item_id"].is_string() ||
        !output.contains("start") || !output["start"].is_string() || !output.contains("end") ||
        !output["end"].is_string() || !output.contains("title") || !output["title"].is_string() ||
        output["title"].get_ref<const std::string&>().empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "预定输出不是规定的 JSON"});
    }
    const nlohmann::json* row = find_row(rows, output["item_id"].get_ref<const std::string&>());
    if (row == nullptr || !row->contains("owner_role") || !(*row)["owner_role"].is_string()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "工作项不在表里"});
    }
    if (speaker_role.empty() ||
        (speaker_role != "pm" && (*row)["owner_role"].get_ref<const std::string&>() != speaker_role)) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有职责匹配的人或 pm 可以预定"});
    }
    const std::string start = clock_or_empty(output["start"].get_ref<const std::string&>(), "10:00");
    const std::string end = clock_or_empty(output["end"].get_ref<const std::string&>(), "11:00");
    if (start.empty() || end.empty() || end <= start) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "预定时间不是北京时间"});
    }
    const nlohmann::json attendees = attendees_for(roles, (*row)["owner_role"].get_ref<const std::string&>());
    if (attendees.empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有对上的参会人，不预定"});
    }
    result.plan = {{"progress", "waiting"},
                   {"proposer", std::string(proposer_open_id)},
                   {"item_id", (*row)["id"]},
                   {"title", output["title"]},
                   {"start", start},
                   {"end", end},
                   {"attendees", attendees},
                   {"decision", "未决"}};
    result.confirm_card = confirm_card(result.plan);
    result.confirm_card_sent = true;
    result.decision = "未决";
    result.calendar_called = false;
    result.table_written = false;
    return result;
}

std::expected<ReserveResult, Error> confirm_reserve(std::string_view actor_open_id,
                                                    std::string_view decision,
                                                    std::string_view calendar_id,
                                                    const nlohmann::json& plan,
                                                    ReserveCalendar& calendar,
                                                    DecisionTable& table) {
    ReserveResult result;
    result.plan = plan;
    result.decision = "未决";
    if (!plan.is_object() || !plan.contains("progress") || !plan["progress"].is_string() ||
        plan["progress"].get_ref<const std::string&>() != "waiting" || !plan.contains("proposer") ||
        !plan["proposer"].is_string() || !plan.contains("item_id") || !plan["item_id"].is_string() ||
        !plan.contains("start") || !plan["start"].is_string() || !plan.contains("end") || !plan["end"].is_string() ||
        !plan.contains("title") || !plan["title"].is_string() || !plan.contains("attendees") ||
        !plan["attendees"].is_array()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有等待中的预定"});
    }
    if (actor_open_id != plan["proposer"].get_ref<const std::string&>()) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有提出人能确认这次预定"});
    }
    if (decision != "同意") {
        result.plan["progress"] = "cancelled";
        return result;
    }
    if (plan["attendees"].empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有对上的参会人，不预定"});
    }
    std::string resolved_calendar(calendar_id);
    if (resolved_calendar.empty()) {
        const std::expected<std::string, Error> primary = calendar.primary_calendar();
        if (!primary.has_value()) {
            return std::unexpected(primary.error());
        }
        resolved_calendar = *primary;
    }
    result.event = {{"calendar_id", resolved_calendar},
                    {"start", {{"datetime", plan["start"]}, {"timezone", "Asia/Shanghai"}}},
                    {"end", {{"datetime", plan["end"]}, {"timezone", "Asia/Shanghai"}}},
                    {"title", plan["title"]},
                    {"attendees", plan["attendees"]}};
    const std::expected<std::string, Error> created = calendar.create_event(result.event);
    result.calendar_called = true;
    if (!created.has_value()) {
        result.decision = "未决";
        result.table_written = false;
        return std::unexpected(created.error());
    }
    const std::expected<void, Error> written = table.set_decision(plan["item_id"].get_ref<const std::string&>(), "同意");
    if (!written.has_value()) {
        const std::expected<void, Error> deleted = calendar.delete_event(resolved_calendar, *created);
        if (!deleted.has_value()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "没有回到原状"});
        }
        result.decision = "未决";
        result.table_written = false;
        return std::unexpected(written.error());
    }
    result.decision = "同意";
    result.table_written = true;
    result.plan["progress"] = "done";
    result.plan["decision"] = "同意";
    result.plan["event_id"] = *created;
    return result;
}

}  // namespace robot_pm
