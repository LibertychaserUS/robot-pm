#include "robot_pm/member_reply.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace robot_pm {
namespace {

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

}  // namespace

std::expected<MemberReplyResult, Error> run_member_reply(std::string_view user_payload,
                                                         std::string_view member_reply_prompt,
                                                         ModelAct& model) {
    MemberReplyResult result;
    result.meeting_created = false;
    result.table_written = false;
    result.system_prompt = std::string(member_reply_prompt);
    if (member_reply_prompt.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "成员对话提示原文是空的"});
    }
    const std::expected<std::string, Error> wrapped = wrap_untrusted_input(user_payload);
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
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "成员对话这一步失败，已丢弃输出"});
    }

    const std::string trimmed = trim_ascii_space(response->stdout_text);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "成员对话输出不是规定的 JSON"});
    }
    const nlohmann::json parsed = nlohmann::json::parse(trimmed, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 1 || !parsed.contains("text") ||
        !parsed["text"].is_string() || parsed["text"].get_ref<const std::string&>().empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "成员对话只能输出 text"});
    }
    result.text = parsed["text"].get<std::string>();
    return result;
}

}  // namespace robot_pm
