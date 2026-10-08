#include "robot_pm/service.hpp"

#include "robot_pm/audit.hpp"

#include <fstream>
#include <utility>

namespace robot_pm {
namespace {

using json = nlohmann::json;

template <std::size_t Size>
consteval bool pure_chinese(const char (&text)[Size]) {
    if (Size < 2 || ((Size - 1) % 3) != 0) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < Size; ++index) {
        if (static_cast<unsigned char>(text[index]) < 0x80U) {
            return false;
        }
    }
    return true;
}

template <std::size_t Size>
consteval int chinese_len(const char (&text)[Size]) {
    return text[Size - 1] == '\0' ? static_cast<int>((Size - 1) / 3) : 0;
}

constexpr const char* kStatusTitle = "改进度";
constexpr const char* kStatusBody = "点确认才写入进度";
constexpr const char* kReserveTitle = "预定会";
constexpr const char* kReserveBody = "点确认才订上这场会";
constexpr const char* kRoleTitle = "记职责";
constexpr const char* kRoleBody = "点确认才记下职责";
constexpr const char* kMeetTitle = "开会";
constexpr const char* kMeetBody = "点同意才订上这场会";
constexpr const char* kFallbackTitle = "请确认";
constexpr const char* kFallbackBody = "点确认才继续办理";

static_assert(pure_chinese("改进度") && chinese_len("改进度") >= 2 && chinese_len("改进度") <= 4);
static_assert(pure_chinese("点确认才写入进度") && chinese_len("点确认才写入进度") <= 20);
static_assert(pure_chinese("预定会") && chinese_len("预定会") >= 2 && chinese_len("预定会") <= 4);
static_assert(pure_chinese("点确认才订上这场会") && chinese_len("点确认才订上这场会") <= 20);
static_assert(pure_chinese("记职责") && chinese_len("记职责") >= 2 && chinese_len("记职责") <= 4);
static_assert(pure_chinese("点确认才记下职责") && chinese_len("点确认才记下职责") <= 20);
static_assert(pure_chinese("开会") && chinese_len("开会") >= 2 && chinese_len("开会") <= 4);
static_assert(pure_chinese("点同意才订上这场会") && chinese_len("点同意才订上这场会") <= 20);
static_assert(pure_chinese("请确认") && chinese_len("请确认") >= 2 && chinese_len("请确认") <= 4);
static_assert(pure_chinese("点确认才继续办理") && chinese_len("点确认才继续办理") <= 20);
static_assert(pure_chinese("应用编号") && chinese_len("应用编号") >= 2 && chinese_len("应用编号") <= 4);
static_assert(pure_chinese("应用密钥") && chinese_len("应用密钥") >= 2 && chinese_len("应用密钥") <= 4);
static_assert(pure_chinese("加密密钥") && chinese_len("加密密钥") >= 2 && chinese_len("加密密钥") <= 4);
static_assert(pure_chinese("校验口令") && chinese_len("校验口令") >= 2 && chinese_len("校验口令") <= 4);
static_assert(pure_chinese("形式没记") && chinese_len("形式没记") == 4);
static_assert(pure_chinese("形式不符") && chinese_len("形式不符") == 4);

struct SettingName {
    const char* key;
    const char* name;
};

constexpr SettingName kRequired[] = {
        {"FEISHU_APP_ID", "应用编号"},
        {"FEISHU_APP_SECRET", "应用密钥"},
        {"FEISHU_ENCRYPT_KEY", "加密密钥"},
        {"FEISHU_VERIFICATION_TOKEN", "校验口令"},
};

[[nodiscard]] bool blank_setting(const std::map<std::string, std::string>& env, const char* key) {
    const auto found = env.find(key);
    return found == env.end() || found->second.empty();
}

[[nodiscard]] bool open_id_text(std::string_view text) {
    return text.starts_with("ou_") || text.starts_with("oc_") || text.starts_with("on_") ||
           text.starts_with("om_");
}

[[nodiscard]] std::string usable_label(const json& value) {
    if (!value.is_string()) {
        return {};
    }
    const std::string text = value.get<std::string>();
    if (text.empty() || open_id_text(text)) {
        return {};
    }
    return text;
}

[[nodiscard]] std::string person_line(const json& message) {
    if (message.contains("actor_name")) {
        const std::string name = usable_label(message.at("actor_name"));
        if (!name.empty()) {
            return name;
        }
    }
    if (message.contains("mention")) {
        const std::string mention = usable_label(message.at("mention"));
        if (!mention.empty()) {
            return mention;
        }
    }
    if (!message.contains("mentions") || !message.at("mentions").is_array()) {
        return {};
    }
    for (const json& mention : message.at("mentions")) {
        if (mention.is_object() && mention.contains("name")) {
            const std::string name = usable_label(mention.at("name"));
            if (!name.empty()) {
                return name;
            }
        }
        if (mention.is_string()) {
            const std::string text = usable_label(mention);
            if (!text.empty()) {
                return text;
            }
        }
    }
    return {};
}

[[nodiscard]] bool button_named(const json& buttons, const char* name) {
    if (!buttons.is_array()) {
        return false;
    }
    for (const json& button : buttons) {
        if (button.is_string() && button.get_ref<const std::string&>() == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool form_card(const json& message) {
    return message.contains("card") && message.at("card").is_object() &&
           message.at("card").dump().find("\"form\"") != std::string::npos;
}

[[nodiscard]] bool decision_card(const json& message) {
    if (form_card(message)) {
        return true;
    }
    return message.contains("buttons") && message.at("buttons").is_array() && !message.at("buttons").empty();
}

[[nodiscard]] std::vector<std::string> presser_ids(const json& message) {
    std::vector<std::string> ids;
    if (!message.contains("pressers") || !message.at("pressers").is_array()) {
        return ids;
    }
    for (const json& id : message.at("pressers")) {
        if (!id.is_string()) {
            continue;
        }
        const std::string text = id.get<std::string>();
        if (!text.empty()) {
            ids.push_back(text);
        }
    }
    return ids;
}

[[nodiscard]] json button_json(const char* label, const json& hidden) {
    json value = hidden;
    value["action"] = label;
    return json{{"tag", "button"},
                {"text", {{"tag", "plain_text"}, {"content", label}}},
                {"type", "primary"},
                {"value", std::move(value)}};
}

[[nodiscard]] json card_face(const char* title, const char* body, const std::string& person, json actions) {
    json elements = json::array();
    elements.push_back(json{{"tag", "div"}, {"text", {{"tag", "plain_text"}, {"content", body}}}});
    if (!person.empty()) {
        elements.push_back(json{{"tag", "div"}, {"text", {{"tag", "plain_text"}, {"content", person}}}});
    }
    elements.push_back(json{{"tag", "action"}, {"actions", std::move(actions)}});
    return json{{"config", {{"update_multi", false}}},
                {"header", {{"title", {{"tag", "plain_text"}, {"content", title}}}}},
                {"elements", std::move(elements)}};
}

[[nodiscard]] std::string ephemeral_url(std::string_view base) {
    const std::string_view fallback = "https://open.feishu.cn";
    std::string root(base.empty() ? fallback : base);
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    return root + "/open-apis/ephemeral/v1/send";
}

// 普通群，包括消息是帖子的群，一次 POST 一个 open_id。飞书把卡片标成仅对你可见。
// 话题群会拒绝这个接口，不能改发全群都能点的卡片。
[[nodiscard]] json ephemeral_envelope(const json& card,
                                     std::string_view base_url,
                                     std::string_view chat_id,
                                     const std::string& open_id) {
    json body{{"open_id", open_id}, {"msg_type", "interactive"}, {"card", card}};
    if (!chat_id.empty()) {
        body["chat_id"] = std::string(chat_id);
    }
    return json{{"method", "POST"}, {"url", ephemeral_url(base_url)}, {"body", std::move(body)}};
}

[[nodiscard]] std::string api_root(std::string_view base) {
    const std::string_view fallback = "https://open.feishu.cn";
    std::string root(base.empty() ? fallback : base);
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    return root;
}

[[nodiscard]] json private_envelope(const json& card, std::string_view base_url, const std::string& open_id) {
    json body{{"receive_id", open_id}, {"msg_type", "interactive"}, {"content", card.dump()}};
    return json{{"method", "POST"},
                {"url", api_root(base_url) + "/open-apis/im/v1/messages?receive_id_type=open_id"},
                {"body", std::move(body)}};
}

[[nodiscard]] const json* object_at(const json& body, const char* key) {
    if (!body.contains(key) || !body.at(key).is_object()) {
        return nullptr;
    }
    return &body.at(key);
}

[[nodiscard]] std::string sender_name(const json& body) {
    const json* event = object_at(body, "event");
    if (event == nullptr || !event->contains("sender") || !event->at("sender").is_object()) {
        return {};
    }
    const json& sender = event->at("sender");
    const std::string named = usable_label(sender.contains("name") ? sender.at("name") : json());
    if (!named.empty()) {
        return named;
    }
    if (sender.contains("sender_id") && sender.at("sender_id").is_object()) {
        return usable_label(sender.at("sender_id").contains("name") ? sender.at("sender_id").at("name") : json());
    }
    return {};
}

[[nodiscard]] std::string sender_open_id(const json& body) {
    const json* event = object_at(body, "event");
    if (event == nullptr || !event->contains("sender") || !event->at("sender").is_object()) {
        return {};
    }
    const json& sender = event->at("sender");
    if (sender.contains("sender_id") && sender.at("sender_id").is_object() &&
        sender.at("sender_id").contains("open_id") && sender.at("sender_id").at("open_id").is_string()) {
        return sender.at("sender_id").at("open_id").get<std::string>();
    }
    if (sender.contains("open_id") && sender.at("open_id").is_string()) {
        return sender.at("open_id").get<std::string>();
    }
    return {};
}

[[nodiscard]] std::string message_text(const json& message) {
    if (!message.contains("content") || !message.at("content").is_string()) {
        return {};
    }
    const std::string& content = message.at("content").get_ref<const std::string&>();
    const json parsed = json::parse(content, nullptr, false);
    if (parsed.is_object() && parsed.contains("text") && parsed.at("text").is_string()) {
        return parsed.at("text").get<std::string>();
    }
    return content;
}

[[nodiscard]] std::optional<json> library_event(const FeishuRequest& request, const AccessDecision& decision) {
    const json body = json::parse(request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::nullopt;
    }
    if (decision.route == "group_mention" || decision.route == "private") {
        json event{{"kind", decision.route == "private" ? "p2p_message" : "group_message"},
                   {"mentions_bot", decision.route == "group_mention"}};
        const json* outer = object_at(body, "event");
        if (outer != nullptr && outer->contains("message") && outer->at("message").is_object()) {
            const json& message = outer->at("message");
            event["message_id"] = message.value("message_id", "");
            event["chat_id"] = message.value("chat_id", "");
            event["chat_mode"] = message.value("chat_mode", "");
            event["group_message_type"] = message.value("group_message_type", "");
            event["text"] = message_text(message);
            if (message.value("message_type", "") == "file") {
                event["attachments"] = json::array({"file"});
            }
        }
        event["open_id"] = sender_open_id(body);
        const std::string name = sender_name(body);
        if (!name.empty()) {
            event["actor_name"] = name;
        }
        return event;
    }
    if (decision.route == "card_callback") {
        json event{{"kind", "card_callback"}, {"mentions_bot", false}};
        const json* outer = object_at(body, "event");
        if (outer == nullptr) {
            return event;
        }
        if (outer->contains("operator") && outer->at("operator").is_object() &&
            outer->at("operator").contains("open_id") && outer->at("operator").at("open_id").is_string()) {
            event["open_id"] = outer->at("operator").at("open_id");
        }
        if (outer->contains("action") && outer->at("action").is_object()) {
            const json& action = outer->at("action");
            if (action.contains("value")) {
                const json& value = action.at("value");
                if (value.is_string()) {
                    event["action"] = value;
                } else if (value.is_object()) {
                    if (value.contains("action") && value.at("action").is_string()) {
                        event["action"] = value.at("action");
                    }
                    if (value.contains("interaction_id") && value.at("interaction_id").is_string()) {
                        event["interaction_id"] = value.at("interaction_id");
                    }
                }
            }
        }
        if (outer->contains("context") && outer->at("context").is_object() &&
            outer->at("context").contains("open_message_id") && outer->at("context").at("open_message_id").is_string()) {
            event["message_id"] = outer->at("context").at("open_message_id");
        }
        return event;
    }
    if (decision.route == "member_join" || decision.route == "bot_added") {
        json event{{"kind", decision.route == "member_join" ? "join" : "bot_added"}, {"has_role", false}};
        const json* outer = object_at(body, "event");
        if (outer != nullptr && outer->contains("users") && outer->at("users").is_array() &&
            !outer->at("users").empty() && outer->at("users").at(0).is_object()) {
            const json& user = outer->at("users").at(0);
            if (user.contains("user_id") && user.at("user_id").is_object() && user.at("user_id").contains("open_id") &&
                user.at("user_id").at("open_id").is_string()) {
                event["open_id"] = user.at("user_id").at("open_id");
            }
            const std::string name = usable_label(user.contains("name") ? user.at("name") : json());
            if (!name.empty()) {
                event["actor_name"] = name;
            }
        }
        return event;
    }
    return std::nullopt;
}

[[nodiscard]] std::string env_or_empty(const std::map<std::string, std::string>& env, const char* key) {
    const auto found = env.find(key);
    if (found == env.end()) {
        return {};
    }
    return found->second;
}

class ShapingFeishu final : public FeishuPort {
public:
    ShapingFeishu(FeishuPort& inner, std::string base_url, std::filesystem::path data_root)
        : inner_(inner), base_url_(std::move(base_url)), data_root_(std::move(data_root)) {}

    std::expected<json, Error> send(const json& message) override {
        std::string form;
        if (message.is_object() && message.contains("chat_id") && message.at("chat_id").is_string()) {
            if (const std::optional<std::string> recorded =
                        recorded_group_form(data_root_, message.at("chat_id").get_ref<const std::string&>())) {
                form = *recorded;
            }
        }
        const CardDispatch dispatch = present_feishu_cards(message, form, base_url_);
        if (!dispatch.notice.empty()) {
            return std::unexpected(Error{ErrorCode::kConfigMissing, dispatch.notice});
        }
        if (dispatch.requests.empty()) {
            return json{{"message_id", "om_none"}};
        }
        json last = json::object();
        for (const json& card : dispatch.requests) {
            const std::expected<json, Error> sent = inner_.send(card);
            if (!sent.has_value()) {
                return std::unexpected(sent.error());
            }
            last = *sent;
        }
        return last;
    }

    std::expected<std::string, Error> primary_calendar_id() override { return inner_.primary_calendar_id(); }

    std::expected<json, Error> create_calendar_event(const json& event) override {
        return inner_.create_calendar_event(event);
    }

    std::expected<void, Error> delete_calendar_event(std::string_view calendar_id, std::string_view event_id) override {
        return inner_.delete_calendar_event(calendar_id, event_id);
    }

private:
    FeishuPort& inner_;
    std::string base_url_;
    std::filesystem::path data_root_;
};

}  // namespace

struct DeliveryRow {
    const char* recorded;
    const char* actual;
    bool in_group;
};

// 预备录入的形式，对上群的实际形式，才有一种送达。没有第三种分支。
constexpr DeliveryRow kDelivery[] = {
        {"普通群", "group", true},
        {"话题群", "topic", false},
};

[[nodiscard]] const DeliveryRow* find_delivery(std::string_view recorded, std::string_view actual) {
    for (const DeliveryRow& row : kDelivery) {
        if (recorded == row.recorded && actual == row.actual) {
            return &row;
        }
    }
    return nullptr;
}

CardDispatch present_feishu_cards(const json& message, std::string_view recorded_form, std::string_view base_url) {
    CardDispatch dispatch;
    if (!message.is_object() || !decision_card(message)) {
        dispatch.requests.push_back(message);
        return dispatch;
    }
    if (recorded_form.empty()) {
        dispatch.notice = "形式没记";
        return dispatch;
    }
    const DeliveryRow* row = find_delivery(recorded_form, message.value("chat_mode", ""));
    if (row == nullptr) {
        dispatch.notice = "形式不符";
        return dispatch;
    }
    const std::vector<std::string> pressers = presser_ids(message);
    if (pressers.empty()) {
        return dispatch;
    }
    const bool meeting = message.contains("buttons") && button_named(message.at("buttons"), "同意") &&
                         button_named(message.at("buttons"), "先不办");
    const std::string text = message.value("text", "");
    const char* title = kFallbackTitle;
    const char* body = kFallbackBody;
    const char* first = "确认";
    const char* second = "取消";
    if (meeting) {
        title = kMeetTitle;
        body = kMeetBody;
        first = "同意";
        second = "先不办";
    } else if (form_card(message) || text.find("职责") != std::string::npos) {
        title = kRoleTitle;
        body = kRoleBody;
    } else if (text.find("预定") != std::string::npos) {
        title = kReserveTitle;
        body = kReserveBody;
    } else if (text.find("进度") != std::string::npos) {
        title = kStatusTitle;
        body = kStatusBody;
    }
    json hidden = json::object();
    if (message.contains("interaction_id") && message.at("interaction_id").is_string() &&
        !open_id_text(message.at("interaction_id").get_ref<const std::string&>())) {
        hidden["interaction_id"] = message.at("interaction_id");
    }
    const std::string person = person_line(message);
    const json face = card_face(title, body, person, json::array({button_json(first, hidden), button_json(second, hidden)}));
    const std::string chat_id = message.value("chat_id", "");
    dispatch.requests.reserve(pressers.size());
    for (const std::string& presser : pressers) {
        if (row->in_group) {
            dispatch.requests.push_back(ephemeral_envelope(face, base_url, chat_id, presser));
        } else {
            dispatch.requests.push_back(private_envelope(face, base_url, presser));
        }
    }
    return dispatch;
}

int serve(const std::map<std::string, std::string>& env,
          const ServeDeps& deps,
          ServicePaths paths,
          std::stop_token stop,
          std::ostream& out) {
    bool missing = false;
    for (const SettingName& setting : kRequired) {
        if (blank_setting(env, setting.key)) {
            out << setting.name << '\n';
            missing = true;
        }
    }
    if (missing) {
        return 1;
    }
    const std::expected<FeishuAccessConfig, Error> access = feishu_access_from_environ(env);
    if (!access.has_value()) {
        return 1;
    }

    Config config;
    config.data_root = paths.data_root.empty() ? std::filesystem::path("var/robot_pm") : paths.data_root;
    config.repo_root = paths.repo_root.empty() ? std::filesystem::current_path() : paths.repo_root;
    config.app_id = access->app_id;
    config.app_secret = access->app_secret;
    config.bot_open_id = env_or_empty(env, "FEISHU_BOT_OPEN_ID");
    config.group_id = env_or_empty(env, "FEISHU_GROUP_ID");
    config.calendar_id = env_or_empty(env, "FEISHU_CALENDAR_ID");
    config.timezone = env_or_empty(env, "ROBOT_PM_TIMEZONE");
    if (config.timezone.empty()) {
        config.timezone = "Asia/Shanghai";
    }

    ShapingFeishu shaping(deps.feishu, access->base_url, config.data_root);
    Ports ports{deps.model, shaping, deps.bitable, deps.process, deps.clock};
    App app(std::move(config), ports);
    const std::string bot_open_id = env_or_empty(env, "FEISHU_BOT_OPEN_ID");

    while (!stop.stop_requested()) {
        static_cast<void>(deps.clock.now());
        static_cast<void>(app.sweep());
        const std::optional<FeishuRequest> request = deps.events.take(stop);
        if (!request.has_value()) {
            break;
        }
        const std::expected<AccessDecision, Error> decision =
                admit_feishu_event(*access, bot_open_id, *request);
        if (!decision.has_value()) {
            deps.events.reply("{}");
            continue;
        }
        if (decision->route == "url_verification") {
            deps.events.reply(json{{"challenge", decision->challenge}}.dump());
            continue;
        }
        if (decision->dropped || !decision->handled || decision->import_file) {
            deps.events.reply("{}");
            continue;
        }
        const std::optional<json> event = library_event(*request, *decision);
        if (!event.has_value()) {
            deps.events.reply("{}");
            continue;
        }
        if (deps.observer != nullptr) {
            deps.observer->before_library(*event);
        }
        const std::expected<json, Error> handled = app.handle_event(*event);
        if (decision->route == "card_callback" && (!handled.has_value() || handled->value("result", "") == "拒")) {
            deps.events.reply("{\"toast\":{\"type\":\"error\",\"content\":\"拒\"}}");
            continue;
        }
        if (decision->route == "card_callback") {
            deps.events.reply("{\"toast\":{\"type\":\"info\",\"content\":\"过\"}}");
            continue;
        }
        deps.events.reply("{}");
    }
    return 0;
}

namespace {

[[nodiscard]] bool known_recorded_form(std::string_view form) {
    for (const DeliveryRow& row : kDelivery) {
        if (form == row.recorded) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool safe_group_id(std::string_view group_id) {
    if (group_id.empty() || group_id == "." || group_id == "..") {
        return false;
    }
    for (const char character : group_id) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool mark = character == '_' || character == '.' || character == '-';
        if (!digit && !lower && !upper && !mark) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path group_form_file(const std::filesystem::path& data_root) {
    return data_root / "group-forms.jsonl";
}

}  // namespace

std::optional<std::string> recorded_group_form(const std::filesystem::path& data_root, std::string_view group_id) {
    if (!safe_group_id(group_id)) {
        return std::nullopt;
    }
    std::ifstream input(group_form_file(data_root));
    if (!input) {
        return std::nullopt;
    }
    std::optional<std::string> found;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const json parsed = json::parse(line, nullptr, false);
        if (!parsed.is_object() || !parsed.contains("群标识") || !parsed.at("群标识").is_string() ||
            !parsed.contains("形式") || !parsed.at("形式").is_string()) {
            continue;
        }
        if (parsed.at("群标识").get_ref<const std::string&>() == group_id) {
            found = parsed.at("形式").get<std::string>();
        }
    }
    return found;
}

std::expected<void, Error> enter_group_form(const std::filesystem::path& data_root,
                                           std::string_view group_id,
                                           std::string_view form) {
    if (!safe_group_id(group_id) || !known_recorded_form(form)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "形式不符"});
    }
    std::vector<std::string> lines;
    {
        std::ifstream input(group_form_file(data_root));
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }
    const json row = json{{"群标识", std::string(group_id)}, {"形式", std::string(form)}};
    bool replaced = false;
    for (std::string& line : lines) {
        const json parsed = json::parse(line, nullptr, false);
        if (parsed.is_object() && parsed.value("群标识", "") == group_id) {
            line = row.dump();
            replaced = true;
        }
    }
    if (!replaced) {
        lines.push_back(row.dump());
    }
    std::string body;
    for (const std::string& line : lines) {
        body.append(line);
        body.push_back('\n');
    }
    return replace_file(group_form_file(data_root), body);
}

}  // namespace robot_pm
