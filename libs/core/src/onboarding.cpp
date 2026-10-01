#include "robot_pm/onboarding.hpp"

#include <string>
#include <vector>

namespace robot_pm {
namespace {

[[nodiscard]] std::string onboarding_system_text(std::string_view identity_prompt,
                                                 std::string_view onboarding_prompt) {
    std::string system;
    system.reserve(identity_prompt.size() + onboarding_prompt.size() + 1);
    system.append(identity_prompt);
    if (system.empty() || system.back() != '\n') {
        system.push_back('\n');
    }
    system.append(onboarding_prompt);
    return system;
}

[[nodiscard]] std::string trim_ascii_space(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\n' || text[begin] == '\r' ||
                                    text[begin] == '\t')) {
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

[[nodiscard]] std::expected<nlohmann::json, Error> parse_json_object(std::string_view text) {
    const std::string trimmed = trim_ascii_space(text);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "模型输出不是规定的 JSON 对象"});
    }
    nlohmann::json parsed = nlohmann::json::parse(trimmed, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "模型输出不是规定的 JSON 对象"});
    }
    return parsed;
}

[[nodiscard]] const nlohmann::json* event_type_node(const nlohmann::json& event) {
    if (event.is_object() && event.contains("header") && event["header"].is_object() &&
        event["header"].contains("event_type")) {
        return &event["header"]["event_type"];
    }
    if (event.is_object() && event.contains("event_type")) {
        return &event["event_type"];
    }
    return nullptr;
}

[[nodiscard]] bool text_asks_for_role(std::string_view text) {
    return text.find("填写职责") != std::string_view::npos ||
           text.find("收集卡片") != std::string_view::npos ||
           text.find("阐述你的职责") != std::string_view::npos ||
           text.find("阐述自己的职责") != std::string_view::npos ||
           text.find("一句话") != std::string_view::npos;
}

[[nodiscard]] bool is_bot_added(const nlohmann::json& event) {
    const nlohmann::json* type_node = event_type_node(event);
    return type_node != nullptr && type_node->is_string() &&
           type_node->get_ref<const std::string&>() == "im.chat.member.bot.added_v1";
}

[[nodiscard]] std::string strip_mentions(std::string_view text) {
    std::string stripped;
    for (std::size_t index = 0; index < text.size();) {
        if (text.compare(index, 3, "<at") == 0) {
            const std::size_t end = text.find("</at>", index);
            if (end == std::string_view::npos) {
                const std::size_t close = text.find('>', index);
                if (close == std::string_view::npos) {
                    stripped.push_back(text[index]);
                    ++index;
                    continue;
                }
                index = close + 1;
                continue;
            }
            index = end + 5;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    const std::string markers[] = {"@所有人", "@_all", "@_everyone"};
    for (const std::string& marker : markers) {
        for (std::size_t found = stripped.find(marker); found != std::string::npos;
             found = stripped.find(marker, found)) {
            stripped.erase(found, marker.size());
        }
    }
    return stripped;
}

[[nodiscard]] std::vector<std::string> joiner_open_ids(const nlohmann::json& event) {
    std::vector<std::string> ids;
    if (!event.contains("event") || !event["event"].is_object() || !event["event"].contains("users") ||
        !event["event"]["users"].is_array()) {
        return ids;
    }
    for (const nlohmann::json& user : event["event"]["users"]) {
        std::string open_id;
        if (user.contains("user_id") && user["user_id"].is_object() && user["user_id"].contains("open_id") &&
            user["user_id"]["open_id"].is_string()) {
            open_id = user["user_id"]["open_id"].get<std::string>();
        } else if (user.contains("open_id") && user["open_id"].is_string()) {
            open_id = user["open_id"].get<std::string>();
        }
        if (!open_id.empty()) {
            bool seen = false;
            for (const std::string& existing : ids) {
                if (existing == open_id) {
                    seen = true;
                }
            }
            if (!seen) {
                ids.push_back(open_id);
            }
        }
    }
    return ids;
}

[[nodiscard]] nlohmann::json role_confirm_card(std::string_view open_id, std::string_view role_text) {
    nlohmann::json confirm_text = {{"tag", "plain_text"}, {"content", "确认"}};
    nlohmann::json cancel_text = {{"tag", "plain_text"}, {"content", "取消"}};
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(nlohmann::json{{"tag", "button"},
                                      {"text", std::move(confirm_text)},
                                      {"value", {{"action", "确认"}, {"open_id", open_id}, {"role", role_text}}}});
    actions.push_back(nlohmann::json{
        {"tag", "button"}, {"text", std::move(cancel_text)}, {"value", {{"action", "取消"}}}});
    nlohmann::json card = {
        {"header", {{"title", {{"tag", "plain_text"}, {"content", "确认职责"}}}}},
        {"elements",
         nlohmann::json::array(
             {nlohmann::json{{"tag", "div"},
                             {"text", {{"tag", "plain_text"}, {"content", std::string(role_text)}}}},
              nlohmann::json{{"tag", "action"}, {"actions", std::move(actions)}}})},
    };
    return {{"msg_type", "interactive"}, {"card", card}};
}

}  // namespace

bool is_onboarding_event(const nlohmann::json& event) {
    const nlohmann::json* type_node = event_type_node(event);
    if (type_node == nullptr || !type_node->is_string()) {
        return false;
    }
    const std::string& type = type_node->get_ref<const std::string&>();
    return type == "im.chat.member.user.added_v1" || type == "im.chat.member.bot.added_v1";
}

std::expected<OnboardingResult, Error> run_onboarding(const nlohmann::json& event,
                                                      bool has_role,
                                                      std::string_view identity_prompt,
                                                      std::string_view onboarding_prompt,
                                                      ModelAct& model) {
    OnboardingResult result;
    result.meeting_created = false;
    result.table_written = false;
    result.system_prompt = onboarding_system_text(identity_prompt, onboarding_prompt);
    if (!is_onboarding_event(event)) {
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "不是成员入群或机器人入群事件"});
    }
    if (identity_prompt.empty() || onboarding_prompt.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "进群提示原文是空的"});
    }

    const std::string payload = nlohmann::json{{"event", event}, {"has_role", has_role}}.dump();
    const std::expected<std::string, Error> wrapped = wrap_untrusted_input(payload);
    if (!wrapped.has_value()) {
        return std::unexpected(wrapped.error());
    }
    result.user_message = *wrapped;

    const ModelRequest request{result.system_prompt, result.user_message};
    const std::expected<ModelResponse, Error> response = model.run(request);
    if (!response.has_value()) {
        return std::unexpected(response.error());
    }
    if (response->exit_code != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "进群这一步失败，已丢弃输出"});
    }

    const std::expected<nlohmann::json, Error> parsed = parse_json_object(response->stdout_text);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (parsed->size() != 2 || !parsed->contains("text") || !parsed->contains("need_role")) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "进群输出只能有 text 和 need_role"});
    }
    const nlohmann::json& text = parsed->at("text");
    const nlohmann::json& need_role = parsed->at("need_role");
    if (!text.is_string() || text.get_ref<const std::string&>().empty() || !need_role.is_boolean()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "进群输出的 text 或 need_role 类型不对"});
    }
    const bool asked = need_role.get<bool>();
    if (asked == has_role) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "need_role 和职责表不一致，不发收集卡片"});
    }
    const std::string greeting = strip_mentions(text.get<std::string>());
    if (greeting.find("收集卡片") != std::string::npos) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "进群不发收集表单"});
    }
    if (!asked && text_asks_for_role(greeting)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "已有职责时不再要求填写职责"});
    }

    result.need_role = asked;
    result.collection_card_sent = false;
    result.confirm_card_sent = false;
    if (!asked) {
        result.text = greeting;
        return result;
    }
    if (is_bot_added(event)) {
        result.mention_open_ids.clear();
        result.text = greeting;
        if (result.text.find("一句话") == std::string::npos) {
            result.text.append("\n请各自 @ 我，用一句话阐述你的职责。");
        }
        return result;
    }
    const std::vector<std::string> joiners = joiner_open_ids(event);
    if (joiners.size() != 1) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "一次只 @ 进群的那一个人"});
    }
    result.mention_open_ids = joiners;
    result.text = "<at user_id=\"" + joiners.front() + "\"></at> " + greeting;
    if (greeting.find("一句话") == std::string::npos) {
        result.text.append("\n请 @ 我，用一句话阐述你的职责。");
    }
    return result;
}

std::expected<RoleConfirmDraft, Error> accept_role_reply(const RoleReply& reply) {
    if (!reply.mentioned_bot || reply.open_id.empty() || reply.text.empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有 @ 机器人，不发确认卡片"});
    }
    RoleConfirmDraft draft;
    draft.confirm_card_sent = true;
    draft.collection_card_sent = false;
    draft.table_written = false;
    draft.open_id = reply.open_id;
    draft.role_text = reply.text;
    draft.confirm_card = role_confirm_card(reply.open_id, reply.text);
    return draft;
}

std::expected<void, Error> confirm_role(std::string_view owner_open_id,
                                        std::string_view actor_open_id,
                                        std::string_view decision,
                                        std::string_view role,
                                        RoleTable& table) {
    if (owner_open_id.empty() || actor_open_id != owner_open_id) {
        return std::unexpected(Error{ErrorCode::kForbidden, "只有提出人能确认自己的职责"});
    }
    if (decision != "确认") {
        return {};
    }
    if (role.empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "职责是空的，不写表"});
    }
    return table.write_role(owner_open_id, role);
}

}  // namespace robot_pm
