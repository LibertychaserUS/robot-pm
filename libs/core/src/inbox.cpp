#include "robot_pm/inbox.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/project.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace robot_pm {
namespace {

[[nodiscard]] std::string extension_lower(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

[[nodiscard]] bool content_has_file_key(const nlohmann::json& message) {
    if (!message.contains("content") || !message["content"].is_string()) {
        return false;
    }
    const nlohmann::json parsed =
        nlohmann::json::parse(message["content"].get_ref<const std::string&>(), nullptr, false);
    return !parsed.is_discarded() && parsed.is_object() && parsed.contains("file_key");
}

[[nodiscard]] const nlohmann::json* message_node(const nlohmann::json& event) {
    if (event.is_object() && event.contains("event") && event["event"].is_object() &&
        event["event"].contains("message") && event["event"]["message"].is_object()) {
        return &event["event"]["message"];
    }
    return nullptr;
}

[[nodiscard]] std::expected<void, Error> audit_line(const std::filesystem::path& memory_root,
                                                    std::chrono::system_clock::time_point now,
                                                    std::string_view op,
                                                    const std::filesystem::path& path,
                                                    bool ok) {
    const nlohmann::json line = {{"time", format_beijing(now)},
                                 {"op", op},
                                 {"path", path.string()},
                                 {"actor", "system"},
                                 {"ok", ok}};
    return append_jsonl(memory_root / "episodic" / "audit.jsonl", line);
}

[[nodiscard]] std::expected<void, Error> move_out_of_inbox(const std::filesystem::path& source,
                                                          const std::filesystem::path& destination_dir) {
    std::error_code error;
    std::filesystem::create_directories(destination_dir, error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "收件文件没有移出"});
    }
    std::filesystem::rename(source, destination_dir / source.filename(), error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "收件文件没有移出"});
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> write_plan(const std::filesystem::path& path,
                                                   std::string_view progress,
                                                   const std::filesystem::path& source) {
    const nlohmann::json plan = {{"step", "project"},
                                 {"progress", progress},
                                 {"background", true},
                                 {"proposer", "system"},
                                 {"source", source.string()}};
    return append_jsonl(path, plan).and_then([&]() -> std::expected<void, Error> {
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string bytes = buffer.str();
        if (bytes.empty() || bytes.back() != '\n') {
            return std::unexpected(Error{ErrorCode::kEditRejected, "计划文件没有写成"});
        }
        std::error_code error;
        const std::filesystem::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << bytes.substr(0, bytes.size() - 1);
            if (!output) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "计划文件没有写成"});
            }
        }
        std::filesystem::rename(temporary, path, error);
        if (error) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "计划文件没有写成"});
        }
        return {};
    });
}

[[nodiscard]] std::expected<nlohmann::json, Error> manifest_from_stdout(std::string_view stdout_text) {
    std::size_t begin = 0;
    while (begin < stdout_text.size() &&
           (stdout_text[begin] == ' ' || stdout_text[begin] == '\n' || stdout_text[begin] == '\r' ||
            stdout_text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = stdout_text.size();
    while (end > begin && (stdout_text[end - 1] == ' ' || stdout_text[end - 1] == '\n' ||
                           stdout_text[end - 1] == '\r' || stdout_text[end - 1] == '\t')) {
        --end;
    }
    const std::string_view trimmed = stdout_text.substr(begin, end - begin);
    if (trimmed.empty() || trimmed.front() != '{') {
        return std::unexpected(Error{ErrorCode::kEditRejected, "抽清单的输出不是清单 JSON"});
    }
    const nlohmann::json parsed = nlohmann::json::parse(std::string(trimmed), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "抽清单的输出不是清单 JSON"});
    }
    return parsed;
}

[[nodiscard]] std::expected<InboxOutcome, Error> finish_without_upload(
    const std::filesystem::path& memory_root,
    std::chrono::system_clock::time_point now,
    const std::filesystem::path& source,
    std::string_view op) {
    const std::expected<void, Error> logged = audit_line(memory_root, now, op, source, false);
    if (!logged.has_value()) {
        return std::unexpected(logged.error());
    }
    const std::expected<void, Error> moved = move_out_of_inbox(source, memory_root / "handled");
    if (!moved.has_value()) {
        return std::unexpected(moved.error());
    }
    InboxOutcome outcome;
    return outcome;
}

}  // namespace

bool is_group_file_message(const nlohmann::json& event) {
    const nlohmann::json* message = message_node(event);
    if (message == nullptr || !message->contains("chat_type") || !(*message)["chat_type"].is_string() ||
        (*message)["chat_type"].get_ref<const std::string&>() != "group") {
        return false;
    }
    if (message->contains("message_type") && (*message)["message_type"].is_string() &&
        (*message)["message_type"].get_ref<const std::string&>() == "file") {
        return true;
    }
    return content_has_file_key(*message);
}

std::expected<InboxOutcome, Error> ignore_group_file_message(const nlohmann::json& event,
                                                             const std::filesystem::path& inbox,
                                                             GroupNotice& notice) {
    static_cast<void>(inbox);
    static_cast<void>(notice);
    if (!is_group_file_message(event)) {
        return std::unexpected(Error{ErrorCode::kUnknownEvent, "不是群里的文件消息"});
    }
    InboxOutcome outcome;
    outcome.ignored = true;
    return outcome;
}

std::expected<InboxOutcome, Error> import_next_inbox_file(const std::filesystem::path& memory_root,
                                                          std::chrono::system_clock::time_point now,
                                                          TextImport* text_import,
                                                          ModelAct* extract,
                                                          BackgroundUpload& upload,
                                                          GroupNotice& notice) {
    static_cast<void>(notice);
    const std::filesystem::path inbox = memory_root / "inbox";
    std::error_code error;
    if (!std::filesystem::is_directory(inbox, error) || error) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "收件目录不存在"});
    }
    std::vector<std::filesystem::path> files;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(inbox, error); !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (!it->is_regular_file(file_error) || file_error) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.empty() || name.front() == '.') {
            continue;
        }
        files.push_back(it->path());
    }
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "收件目录读不出来"});
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        return InboxOutcome{};
    }
    const std::filesystem::path source = files.front();
    const std::string extension = extension_lower(source);
    nlohmann::json manifest;
    std::string source_text;
    bool from_json = false;
    if (extension == ".json") {
        from_json = true;
        std::ifstream input(source);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string bytes = buffer.str();
        const std::size_t first = bytes.find_first_not_of(" \r\n\t");
        if (first == std::string::npos || bytes[first] != '{') {
            return finish_without_upload(memory_root, now, source, "import");
        }
        manifest = nlohmann::json::parse(bytes, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            return finish_without_upload(memory_root, now, source, "import");
        }
    } else if (extension == ".md" || extension == ".docx" || extension == ".pdf") {
        if (text_import == nullptr) {
            return std::unexpected(Error{ErrorCode::kConfigMissing, "文字导入没有配置"});
        }
        const std::expected<nlohmann::json, Error> imported = text_import->read(source);
        if (!imported.has_value()) {
            return std::unexpected(imported.error());
        }
        if (!imported->is_object() || !imported->contains("kind") || !(*imported)["kind"].is_string()) {
            return finish_without_upload(memory_root, now, source, "import");
        }
        const std::string& kind = (*imported)["kind"].get_ref<const std::string&>();
        if (kind == "empty_text") {
            const std::filesystem::path review =
                memory_root / "semantic" / "supervision" / (source.stem().string() + ".txt");
            std::error_code directory_error;
            std::filesystem::create_directories(review.parent_path(), directory_error);
            if (directory_error) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "复查单没有写成"});
            }
            const std::filesystem::path temporary = review.string() + ".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << source.string() << '\n';
                if (!output) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "复查单没有写成"});
                }
            }
            std::filesystem::rename(temporary, review, directory_error);
            if (directory_error) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "复查单没有写成"});
            }
            return finish_without_upload(memory_root, now, source, "import");
        }
        if (kind == "unrecognized" || kind == "invalid_json") {
            return finish_without_upload(memory_root, now, source, "import");
        }
        if (kind != "text" || !imported->contains("text") || !(*imported)["text"].is_string()) {
            return finish_without_upload(memory_root, now, source, "import");
        }
        source_text = (*imported)["text"].get<std::string>();
        if (source_text.empty()) {
            return finish_without_upload(memory_root, now, source, "import");
        }
        if (extract == nullptr) {
            return std::unexpected(Error{ErrorCode::kConfigMissing, "抽清单命令没有配置"});
        }
        const std::expected<ModelResponse, Error> response =
            extract->run(ModelRequest{"", source_text});
        if (!response.has_value()) {
            return std::unexpected(response.error());
        }
        if (response->exit_code != 0) {
            return finish_without_upload(memory_root, now, source, "import");
        }
        const std::expected<nlohmann::json, Error> parsed = manifest_from_stdout(response->stdout_text);
        if (!parsed.has_value()) {
            return finish_without_upload(memory_root, now, source, "import");
        }
        manifest = *parsed;
    } else {
        return finish_without_upload(memory_root, now, source, "import");
    }

    const std::expected<std::vector<nlohmann::json>, Error> rows =
        project_manifest(manifest, from_json ? std::string_view{} : std::string_view{source_text});
    if (!rows.has_value()) {
        return finish_without_upload(memory_root, now, source, "import");
    }
    const std::filesystem::path plan_path =
        memory_root / "working" / (std::string("project-") + source.stem().string()) / "plan" / "context.json";
    const std::expected<void, Error> executing = write_plan(plan_path, "executing", source);
    if (!executing.has_value()) {
        return std::unexpected(executing.error());
    }
    const std::expected<void, Error> uploaded = upload.upload(*rows);
    if (!uploaded.has_value()) {
        const std::expected<void, Error> cancelled = write_plan(plan_path, "cancelled", source);
        if (!cancelled.has_value()) {
            return std::unexpected(cancelled.error());
        }
        const std::expected<void, Error> logged = audit_line(memory_root, now, "project", source, false);
        if (!logged.has_value()) {
            return std::unexpected(logged.error());
        }
        const std::expected<void, Error> moved = move_out_of_inbox(source, memory_root / "handled");
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        return InboxOutcome{};
    }
    const std::expected<void, Error> done = write_plan(plan_path, "done", source);
    if (!done.has_value()) {
        return std::unexpected(done.error());
    }
    const nlohmann::json event = {{"kind", "projection"}, {"path", source.string()}, {"ok", true}};
    const std::expected<void, Error> recorded = append_jsonl(memory_root / "episodic" / "events.jsonl", event);
    if (!recorded.has_value()) {
        return std::unexpected(recorded.error());
    }
    const std::expected<void, Error> logged = audit_line(memory_root, now, "project", source, true);
    if (!logged.has_value()) {
        return std::unexpected(logged.error());
    }
    const std::filesystem::path manifest_path =
        memory_root / "semantic" / "manifests" / (source.stem().string() + ".json");
    const std::expected<void, Error> stored = append_jsonl(manifest_path, manifest);
    if (!stored.has_value()) {
        return std::unexpected(stored.error());
    }
    std::ifstream manifest_input(manifest_path);
    std::ostringstream manifest_buffer;
    manifest_buffer << manifest_input.rdbuf();
    const std::string manifest_bytes = manifest_buffer.str();
    if (!manifest_bytes.empty() && manifest_bytes.back() == '\n') {
        std::error_code rename_error;
        const std::filesystem::path temporary = manifest_path.string() + ".tmp";
        std::ofstream stripped(temporary, std::ios::binary | std::ios::trunc);
        stripped << manifest_bytes.substr(0, manifest_bytes.size() - 1);
        stripped.close();
        std::filesystem::rename(temporary, manifest_path, rename_error);
    }
    const std::expected<void, Error> moved = move_out_of_inbox(source, memory_root / "handled");
    if (!moved.has_value()) {
        return std::unexpected(moved.error());
    }
    InboxOutcome outcome;
    outcome.projected = true;
    outcome.uploaded = true;
    return outcome;
}

}  // namespace robot_pm
