#include "live.hpp"

#include "robot_pm/frame.hpp"
#include "robot_pm/http_exchange.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace robot_pm {
namespace {

using json = nlohmann::json;

[[nodiscard]] std::string root_of(std::string base) {
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.empty()) {
        return "https://open.feishu.cn";
    }
    return base;
}

[[nodiscard]] json fields_of(const json& item) {
    json row = json::object();
    if (item.contains("fields") && item.at("fields").is_object()) {
        row = item.at("fields");
    }
    if (item.contains("record_id") && item.at("record_id").is_string()) {
        row["record_id"] = item.at("record_id");
    }
    return row;
}

}  // namespace

std::expected<std::string, Error> LiveModel::complete(std::string_view step,
                                                      std::string_view system_prompt,
                                                      std::string_view user_message) {
    if (url_.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "没有模型地址"});
    }
    const json payload{{"step", std::string(step)},
                       {"system", std::string(system_prompt)},
                       {"user", std::string(user_message)}};
    const std::string body = payload.dump();
    const std::expected<HttpExchange, Error> response = http_exchange(
            "POST", url_, {{"Content-Type", "application/json"}}, body);
    if (!response || response->status >= 400) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "模型没有答完"});
    }
    const std::optional<std::string> reply = model_reply_text(response->body);
    if (!reply) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "模型没有答完"});
    }
    return *reply;
}

std::expected<std::string, Error> LiveFeishu::token() {
    if (!token_.empty()) {
        return token_;
    }
    const json body{{"app_id", app_id_}, {"app_secret", app_secret_}};
    const std::expected<json, Error> response =
            api("POST", root_of(base_url_) + "/open-apis/auth/v3/tenant_access_token/internal", &body, false);
    if (!response || !response->contains("tenant_access_token") || !response->at("tenant_access_token").is_string()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "没有拿到访问凭证"});
    }
    token_ = response->at("tenant_access_token").get<std::string>();
    return token_;
}

std::expected<json, Error> LiveFeishu::api(std::string_view method,
                                           std::string_view url,
                                           const json* body,
                                           bool authorize) {
    std::vector<std::pair<std::string, std::string>> headers{{"Content-Type", "application/json"}};
    if (authorize) {
        const std::expected<std::string, Error> access = token();
        if (!access) {
            return std::unexpected(access.error());
        }
        headers.emplace_back("Authorization", "Bearer " + *access);
    }
    const std::string payload = body == nullptr ? std::string() : body->dump();
    const std::expected<HttpExchange, Error> response = http_exchange(method, url, headers, payload);
    if (!response) {
        return std::unexpected(response.error());
    }
    const json parsed = json::parse(response->body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "对方没有给完整回复"});
    }
    if (response->status >= 400 || (parsed.contains("code") && parsed.at("code").is_number() && parsed.at("code") != 0 &&
                                    !parsed.contains("tenant_access_token"))) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "对方没有接受这次请求"});
    }
    return parsed;
}

std::expected<json, Error> LiveFeishu::send(const json& message) {
    std::string url = root_of(base_url_) + "/open-apis/im/v1/messages?receive_id_type=open_id";
    json body = message;
    if (message.is_object() && message.contains("method") && message.contains("url") && message.contains("body")) {
        url = message.at("url").get<std::string>();
        body = message.at("body");
    }
    const std::expected<json, Error> response = api("POST", url, &body, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    std::string message_id;
    if (response->contains("data") && response->at("data").is_object()) {
        const json& data = response->at("data");
        if (data.contains("message_id") && data.at("message_id").is_string()) {
            message_id = data.at("message_id").get<std::string>();
        }
    }
    if (message_id.empty() && response->contains("message_id") && response->at("message_id").is_string()) {
        message_id = response->at("message_id").get<std::string>();
    }
    if (message_id.empty()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "对方没有给完整回复"});
    }
    return json{{"message_id", message_id}};
}

std::expected<std::string, Error> LiveFeishu::primary_calendar_id() {
    const std::expected<json, Error> response =
            api("GET", root_of(base_url_) + "/open-apis/calendar/v4/calendars", nullptr, true);
    if (!response || !response->contains("data") || !response->at("data").is_object()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "没有主日历"});
    }
    const json& data = response->at("data");
    if (!data.contains("calendar_list") || !data.at("calendar_list").is_array()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "没有主日历"});
    }
    for (const json& calendar : data.at("calendar_list")) {
        if (calendar.is_object() && calendar.value("type", "") == "primary" && calendar.contains("calendar_id") &&
            calendar.at("calendar_id").is_string()) {
            return calendar.at("calendar_id").get<std::string>();
        }
    }
    return std::unexpected(Error{ErrorCode::kBridgeFailed, "没有主日历"});
}

std::expected<json, Error> LiveFeishu::create_calendar_event(const json& event) {
    const std::expected<std::string, Error> calendar = primary_calendar_id();
    if (!calendar) {
        return std::unexpected(calendar.error());
    }
    const std::expected<json, Error> response = api(
            "POST", root_of(base_url_) + "/open-apis/calendar/v4/calendars/" + *calendar + "/events", &event, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    return *response;
}

std::expected<void, Error> LiveFeishu::delete_calendar_event(std::string_view calendar_id, std::string_view event_id) {
    const std::expected<json, Error> response = api(
            "DELETE",
            root_of(base_url_) + "/open-apis/calendar/v4/calendars/" + std::string(calendar_id) + "/events/" +
                    std::string(event_id),
            nullptr,
            true);
    if (!response) {
        return std::unexpected(response.error());
    }
    return {};
}

std::string LiveBitable::table_path(std::string_view) const {
    return root_of(feishu_.base_url()) + "/open-apis/bitable/v1/apps/" + app_token_ + "/tables/" + table_id_;
}

std::expected<json, Error> LiveBitable::list_fields(std::string_view) {
    const std::expected<json, Error> response = feishu_.api("GET", table_path({}) + "/fields", nullptr, true);
    if (!response || !response->contains("data") || !response->at("data").is_object()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "表格没有列出来"});
    }
    const json& data = response->at("data");
    if (!data.contains("items") || !data.at("items").is_array()) {
        return json::array();
    }
    return data.at("items");
}

std::expected<void, Error> LiveBitable::create_field(std::string_view, const json& field) {
    const std::expected<json, Error> response = feishu_.api("POST", table_path({}) + "/fields", &field, true);
    if (!response) {
        return std::unexpected(response.error());
    }
    return {};
}

std::expected<json, Error> LiveBitable::list_records(std::string_view) {
    const std::expected<json, Error> response = feishu_.api("GET", table_path({}) + "/records", nullptr, true);
    if (!response || !response->contains("data") || !response->at("data").is_object()) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "表格没有列出来"});
    }
    json rows = json::array();
    const json& data = response->at("data");
    if (data.contains("items") && data.at("items").is_array()) {
        for (const json& item : data.at("items")) {
            rows.push_back(fields_of(item));
        }
    }
    return rows;
}

std::expected<json, Error> LiveBitable::upsert(std::string_view, const json& incoming) {
    if (!incoming.is_array()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "行必须是数组"});
    }
    json written = json::array();
    for (const json& row : incoming) {
        json fields = row;
        fields.erase("record_id");
        const json body{{"fields", fields}};
        const std::expected<json, Error> response =
                feishu_.api("POST", table_path({}) + "/records", &body, true);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->contains("data") && response->at("data").is_object() && response->at("data").contains("record") &&
            response->at("data").at("record").contains("record_id") &&
            response->at("data").at("record").at("record_id").is_string()) {
            created_.push_back(response->at("data").at("record").at("record_id").get<std::string>());
        }
        written.push_back(row);
    }
    return written;
}

std::expected<void, Error> LiveBitable::undo(const json&) {
    for (const std::string& record_id : created_) {
        const std::expected<json, Error> response =
                feishu_.api("DELETE", table_path({}) + "/records/" + record_id, nullptr, true);
        if (!response) {
            return std::unexpected(response.error());
        }
    }
    created_.clear();
    return {};
}

std::expected<ProcessResult, Error> LiveProcess::run(const ProcessRequest& request) {
    int pipes[2];
    if (::pipe(pipes) != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "外部进程没有跑起来"});
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipes[0]);
        ::close(pipes[1]);
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "外部进程没有跑起来"});
    }
    if (child == 0) {
        ::dup2(pipes[1], STDOUT_FILENO);
        ::close(pipes[0]);
        ::close(pipes[1]);
        if (request.args.empty()) {
            ::execl("/bin/sh", "sh", "-c", "printf '%s' '{}'", nullptr);
            _exit(127);
        }
        std::vector<char*> argv;
        argv.reserve(request.args.size() + 1);
        for (const std::string& arg : request.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(pipes[1]);
    std::string output;
    char buffer[1024];
    while (true) {
        const ssize_t got = ::read(pipes[0], buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(pipes[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    ProcessResult result;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    result.stdout_text = std::move(output);
    return result;
}

}  // namespace robot_pm
