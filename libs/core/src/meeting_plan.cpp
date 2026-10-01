#include "robot_pm/meeting_plan.hpp"

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

[[nodiscard]] bool is_beijing_clock(std::string_view text) {
    if (text.size() != 16 || text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] nlohmann::json button(const char* label, const nlohmann::json& value) {
    nlohmann::json action = value;
    action["action"] = label;
    return nlohmann::json{{"tag", "button"},
                          {"text", {{"tag", "plain_text"}, {"content", label}}},
                          {"type", "primary"},
                          {"value", std::move(action)}};
}

[[nodiscard]] nlohmann::json interactive_card(std::string_view title, std::string_view body,
                                              const nlohmann::json& actions) {
    return nlohmann::json{
        {"msg_type", "interactive"},
        {"card",
         {{"header", {{"title", {{"tag", "plain_text"}, {"content", title}}}}},
          {"elements",
           nlohmann::json::array({{{"tag", "div"}, {"text", {{"tag", "plain_text"}, {"content", body}}}},
                                  {{"tag", "action"}, {"actions", actions}}})}}}};
}

[[nodiscard]] std::expected<nlohmann::json, Error> attendees_for(const nlohmann::json& roles,
                                                                 const nlohmann::json& attendee_roles) {
    nlohmann::json attendees = nlohmann::json::array();
    if (!roles.is_array() || !attendee_roles.is_array()) {
        return attendees;
    }
    for (const nlohmann::json& role_name : attendee_roles) {
        if (!role_name.is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "参会职责不在表里"});
        }
        for (const nlohmann::json& role : roles) {
            if (!role.is_object() || !role.contains("open_id") || !role["open_id"].is_string() ||
                !role.contains("role") || !role["role"].is_string()) {
                continue;
            }
            if (role["role"].get_ref<const std::string&>() == role_name.get_ref<const std::string&>()) {
                attendees.push_back(role["open_id"]);
            }
        }
    }
    return attendees;
}

}  // namespace

std::expected<MeetingPlanResult, Error> propose_meeting_plan(std::string_view user_payload,
                                                             std::string_view prompt,
                                                             std::string_view proposer_open_id,
                                                             const nlohmann::json& roles,
                                                             ModelAct& model) {
    MeetingPlanResult result;
    if (prompt.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "会议计划缺少提示"});
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
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "会议计划这一步失败，已丢弃输出"});
    }
    const std::string trimmed = trim_ascii_space(response->stdout_text);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "会议计划输出不是规定的 JSON"});
    }
    const nlohmann::json output = nlohmann::json::parse(trimmed, nullptr, false);
    if (output.is_discarded() || !output.is_object() || output.size() != 3 || !output.contains("meetings") ||
        !output["meetings"].is_array() || !output.contains("todos") || !output["todos"].is_array() ||
        !output.contains("ai_recommended_item_id")) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "会议计划输出不是规定的 JSON"});
    }
    nlohmann::json meetings = nlohmann::json::array();
    for (const nlohmann::json& meeting : output["meetings"]) {
        if (!meeting.is_object() || !meeting.contains("item_id") || !meeting["item_id"].is_string() ||
            !meeting.contains("title") || !meeting["title"].is_string() || !meeting.contains("start") ||
            !meeting["start"].is_string() || !is_beijing_clock(meeting["start"].get_ref<const std::string&>()) ||
            !meeting.contains("attendee_roles") || !meeting["attendee_roles"].is_array()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "会议计划输出不是规定的 JSON"});
        }
        for (auto it = meeting.begin(); it != meeting.end(); ++it) {
            if (it.key() != "item_id" && it.key() != "title" && it.key() != "agenda" && it.key() != "start" &&
                it.key() != "end" && it.key() != "attendee_roles") {
                return std::unexpected(Error{ErrorCode::kEditRejected, "会议计划输出不是规定的 JSON"});
            }
        }
        const std::expected<nlohmann::json, Error> attendees =
            attendees_for(roles, meeting["attendee_roles"]);
        if (!attendees.has_value()) {
            return std::unexpected(attendees.error());
        }
        meetings.push_back({{"item_id", meeting["item_id"]},
                            {"title", meeting["title"]},
                            {"start", meeting["start"]},
                            {"attendees", *attendees}});
    }
    result.plan = {{"progress", "waiting"},
                   {"proposer", std::string(proposer_open_id)},
                   {"meetings", meetings},
                   {"group_card_sent", false},
                   {"meeting_created", false}};
    if (meetings.empty()) {
        return result;
    }
    const std::string body = "将向群里发送会议卡片：" + meetings[0]["title"].get<std::string>() + " " +
                             meetings[0]["start"].get<std::string>() + "（北京时间）。同意之后才发到群里。";
    result.proposer_card = interactive_card(
        "确认会议计划", body, nlohmann::json::array({button("同意", {{"stage", "plan"}}), button("取消", {{"stage", "plan"}})}));
    result.proposer_card_sent = true;
    result.group_card_sent = false;
    result.meeting_created = false;
    return result;
}

std::expected<MeetingPlanResult, Error> confirm_meeting_plan(std::string_view actor_open_id,
                                                             std::string_view actor_role,
                                                             std::string_view decision,
                                                             const nlohmann::json& plan) {
    MeetingPlanResult result;
    result.plan = plan;
    if (!plan.is_object() || !plan.contains("progress") || !plan["progress"].is_string() ||
        plan["progress"].get_ref<const std::string&>() != "waiting" || !plan.contains("proposer") ||
        !plan["proposer"].is_string() || !plan.contains("meetings") || !plan["meetings"].is_array()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有等待中的会议计划"});
    }
    const std::string& proposer = plan["proposer"].get_ref<const std::string&>();
    const bool allowed = (!proposer.empty() && actor_open_id == proposer) || (proposer.empty() && actor_role == "pm");
    if (!allowed) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有提出人能确认这次会议计划"});
    }
    result.meeting_created = false;
    if (decision != "同意" || plan["meetings"].empty()) {
        result.plan["progress"] = "cancelled";
        result.group_card_sent = false;
        return result;
    }
    const nlohmann::json& meeting = plan["meetings"][0];
    const std::string body = "AI推荐会议时间为" + meeting["start"].get<std::string>() + "（北京时间）";
    result.group_card = interactive_card(
        meeting["title"].get<std::string>(), body,
        nlohmann::json::array({button("同意", {{"stage", "group"}}), button("先不办", {{"stage", "group"}})}));
    result.group_card_sent = true;
    result.plan["progress"] = "group_waiting";
    result.plan["group_card_sent"] = true;
    return result;
}

std::expected<MeetingPlanResult, Error> answer_group_meeting(std::string_view decision,
                                                             const nlohmann::json& plan,
                                                             CalendarBook& calendar) {
    MeetingPlanResult result;
    result.plan = plan;
    result.group_card_sent = true;
    if (!plan.is_object() || !plan.contains("progress") || !plan["progress"].is_string() ||
        plan["progress"].get_ref<const std::string&>() != "group_waiting" || !plan.contains("meetings") ||
        !plan["meetings"].is_array() || plan["meetings"].empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "会议卡片还没经过提出人确认"});
    }
    if (decision == "先不办") {
        result.meeting_created = false;
        result.plan["progress"] = "done";
        result.plan["decision"] = "先不办";
        return result;
    }
    if (decision != "同意") {
        return std::unexpected(Error{ErrorCode::kEditRejected, "会议卡片只有同意或先不办"});
    }
    const nlohmann::json& meeting = plan["meetings"][0];
    if (!meeting.contains("attendees") || !meeting["attendees"].is_array() || meeting["attendees"].empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有对上的参会人，不预定"});
    }
    const nlohmann::json event = {{"timezone", "Asia/Shanghai"},
                                  {"start", meeting["start"]},
                                  {"title", meeting["title"]},
                                  {"attendees", meeting["attendees"]}};
    const std::expected<std::string, Error> created = calendar.create_event(event);
    if (!created.has_value()) {
        result.meeting_created = false;
        result.plan["decision"] = "未决";
        return std::unexpected(created.error());
    }
    result.meeting_created = true;
    result.plan["progress"] = "done";
    result.plan["decision"] = "同意";
    result.plan["event_id"] = *created;
    return result;
}

}  // namespace robot_pm
