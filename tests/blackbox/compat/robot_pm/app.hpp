#pragma once

// Build adapter for black-box cases. libs/core on main does not declare robot_pm::App.
// The assertions stay in the case files. Those cases are skipped until this symbol exists.

#include "robot_pm/error.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace robot_pm {

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;
};

class Model {
public:
    virtual ~Model() = default;
    virtual std::expected<std::string, Error> complete(std::string_view step,
                                                       std::string_view system_prompt,
                                                       std::string_view user_message) = 0;
};

class FeishuPort {
public:
    virtual ~FeishuPort() = default;
    virtual std::expected<nlohmann::json, Error> send(const nlohmann::json& message) = 0;
    virtual std::expected<std::string, Error> primary_calendar_id() = 0;
    virtual std::expected<nlohmann::json, Error> create_calendar_event(
            const nlohmann::json& event) = 0;
    virtual std::expected<void, Error> delete_calendar_event(std::string_view calendar_id,
                                                             std::string_view event_id) = 0;
};

class BitablePort {
public:
    virtual ~BitablePort() = default;
    virtual std::expected<nlohmann::json, Error> list_fields(std::string_view table) = 0;
    virtual std::expected<void, Error> create_field(std::string_view table,
                                                    const nlohmann::json& field) = 0;
    virtual std::expected<nlohmann::json, Error> list_records(std::string_view table) = 0;
    virtual std::expected<nlohmann::json, Error> upsert(std::string_view table,
                                                        const nlohmann::json& incoming) = 0;
    virtual std::expected<void, Error> undo(const nlohmann::json& compensation) = 0;
};

struct ProcessRequest {
    std::vector<std::string> args;
    std::string stdin_text;
};

struct ProcessResult {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    virtual std::expected<ProcessResult, Error> run(const ProcessRequest& request) = 0;
};

struct Config {
    std::filesystem::path data_root;
    std::filesystem::path repo_root;
    std::string delivery;
    std::string timezone;
    std::string calendar_id;
    std::string group_id;
    std::string bot_open_id;
    std::string app_id;
    std::string app_secret;
    std::string bitable_app_token;
    std::vector<std::string> chase_allowlist;
    std::vector<std::string> meet_allowlist;
    std::string template_name;
    bool fail_semantic_replace = false;
    bool stop_after_durable_audit = false;
};

struct Ports {
    Model& model;
    FeishuPort& feishu;
    BitablePort& bitable;
    ProcessRunner& process;
    Clock& clock;
};

class App {
public:
    App(Config config, Ports ports) : config_(std::move(config)), ports_(ports) {}
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    std::expected<nlohmann::json, Error> handle_event(const nlohmann::json&) { return missing(); }
    std::expected<nlohmann::json, Error> import_inbox() { return missing(); }
    std::expected<nlohmann::json, Error> project_manifest(const nlohmann::json&) {
        return missing();
    }
    std::expected<nlohmann::json, Error> project_manifest_text(std::string_view) {
        return missing();
    }
    std::expected<nlohmann::json, Error> compare_edits(const nlohmann::json&, const nlohmann::json&) {
        return missing();
    }
    std::expected<nlohmann::json, Error> write_edits(const nlohmann::json&) { return missing(); }
    std::expected<nlohmann::json, Error> watch(const nlohmann::json&,
                                               const nlohmann::json&,
                                               std::string_view) {
        return missing();
    }
    std::expected<nlohmann::json, Error> audit_slices(const nlohmann::json&) { return missing(); }
    std::expected<nlohmann::json, Error> enqueue(const nlohmann::json&) { return missing(); }
    std::vector<std::string> command_ids() const { return {}; }
    std::expected<nlohmann::json, Error> sweep() { return missing(); }
    std::expected<nlohmann::json, Error> preview_prompt(std::string_view) { return missing(); }

private:
    std::expected<nlohmann::json, Error> missing() const {
        static_cast<void>(config_.data_root);
        static_cast<void>(&ports_.model);
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "missing symbol: robot_pm::App"});
    }

    Config config_;
    Ports ports_;
};

}  // namespace robot_pm
