#pragma once

#include "robot_pm/app.hpp"

#include <string>

namespace robot_pm {

class LiveModel final : public Model {
public:
    explicit LiveModel(std::string url) : url_(std::move(url)) {}

    std::expected<std::string, Error> complete(std::string_view step,
                                               std::string_view system_prompt,
                                               std::string_view user_message) override;

private:
    std::string url_;
};

class LiveFeishu final : public FeishuPort {
public:
    LiveFeishu(std::string app_id, std::string app_secret, std::string base_url)
        : app_id_(std::move(app_id)), app_secret_(std::move(app_secret)), base_url_(std::move(base_url)) {}

    std::expected<nlohmann::json, Error> send(const nlohmann::json& message) override;
    std::expected<std::string, Error> primary_calendar_id() override;
    std::expected<nlohmann::json, Error> create_calendar_event(const nlohmann::json& event) override;
    std::expected<void, Error> delete_calendar_event(std::string_view calendar_id, std::string_view event_id) override;

    [[nodiscard]] const std::string& base_url() const { return base_url_; }
    [[nodiscard]] std::expected<nlohmann::json, Error> api(std::string_view method,
                                                          std::string_view url,
                                                          const nlohmann::json* body,
                                                          bool authorize);

private:
    [[nodiscard]] std::expected<std::string, Error> token();

    std::string app_id_;
    std::string app_secret_;
    std::string base_url_;
    std::string token_;
};

class LiveBitable final : public BitablePort {
public:
    LiveBitable(LiveFeishu& feishu, std::string app_token, std::string table_id)
        : feishu_(feishu), app_token_(std::move(app_token)), table_id_(std::move(table_id)) {}

    std::expected<nlohmann::json, Error> list_fields(std::string_view table) override;
    std::expected<void, Error> create_field(std::string_view table, const nlohmann::json& field) override;
    std::expected<nlohmann::json, Error> list_records(std::string_view table) override;
    std::expected<nlohmann::json, Error> upsert(std::string_view table, const nlohmann::json& incoming) override;
    std::expected<void, Error> undo(const nlohmann::json& compensation) override;

private:
    [[nodiscard]] std::string table_path(std::string_view table) const;

    LiveFeishu& feishu_;
    std::string app_token_;
    std::string table_id_;
    std::vector<std::string> created_;
};

class LiveProcess final : public ProcessRunner {
public:
    std::expected<ProcessResult, Error> run(const ProcessRequest& request) override;
};

}  // namespace robot_pm
