#include "robot_pm/working_memory.hpp"

#include "robot_pm/audit.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace robot_pm {
namespace {

[[nodiscard]] bool safe_id(std::string_view id) {
    if (id.empty() || id == "." || id == "..") {
        return false;
    }
    for (const char character : id) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool mark = character == '.' || character == '_' || character == '-';
        if (!digit && !lower && !upper && !mark) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<std::filesystem::path, Error> interaction_directory(
    const std::filesystem::path& memory_root,
    std::string_view open_id,
    std::string_view interaction_id) {
    if (!safe_id(open_id) || !safe_id(interaction_id)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "交互标识不能越过自己的目录"});
    }
    return memory_root / "working" / std::string(open_id) / std::string(interaction_id);
}

[[nodiscard]] bool person_has_open_folder(const std::filesystem::path& person) {
    std::error_code error;
    if (!std::filesystem::is_directory(person, error) || error) {
        return false;
    }
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(person, error); !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (it->is_directory(file_error) && !file_error) {
            return true;
        }
    }
    return false;
}

void remove_person_if_empty(const std::filesystem::path& person) {
    std::error_code error;
    if (!std::filesystem::is_directory(person, error) || error) {
        return;
    }
    bool any = false;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(person, error); !error && it != end; it.increment(error)) {
        any = true;
        break;
    }
    if (!error && !any) {
        std::filesystem::remove(person, error);
    }
}

[[nodiscard]] bool is_tombstone(const nlohmann::json& line) {
    if (!line.is_object() || line.size() < 5) {
        return false;
    }
    const char* keys[] = {"interaction_id", "actor", "op", "outcome", "time"};
    for (const char* key : keys) {
        if (!line.contains(key) || !line[key].is_string() || line[key].get_ref<const std::string&>().empty()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<void, Error> delete_tombstoned_directory(const std::filesystem::path& memory_root,
                                                                    const nlohmann::json& line) {
    if (!safe_id(line["actor"].get_ref<const std::string&>()) ||
        !safe_id(line["interaction_id"].get_ref<const std::string&>())) {
        return {};
    }
    const std::filesystem::path person =
        memory_root / "working" / line["actor"].get_ref<const std::string&>();
    const std::filesystem::path directory =
        person / line["interaction_id"].get_ref<const std::string&>();
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return {};
    }
    std::filesystem::remove_all(directory, error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "交互目录没有删掉"});
    }
    remove_person_if_empty(person);
    return {};
}

[[nodiscard]] bool context_is_chat(const nlohmann::json& context) {
    return context.contains("messages") || context.contains("chat") || context.contains("history");
}

}  // namespace

std::expected<std::filesystem::path, Error> begin_interaction(const std::filesystem::path& memory_root,
                                                             std::string_view open_id,
                                                             std::string_view interaction_id,
                                                             const nlohmann::json& context) {
    const std::expected<void, Error> recovered = recover_finished_interactions(memory_root);
    if (!recovered.has_value()) {
        return std::unexpected(recovered.error());
    }
    if (!context.is_object() || context_is_chat(context)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "聊天记录不是这次交互的上下文"});
    }
    const std::expected<std::filesystem::path, Error> directory =
        interaction_directory(memory_root, open_id, interaction_id);
    if (!directory.has_value()) {
        return std::unexpected(directory.error());
    }
    if (person_has_open_folder(directory->parent_path())) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "这个人已经有一次未结束的交互"});
    }
    std::error_code error;
    std::filesystem::create_directories(*directory, error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "交互目录没有写成"});
    }
    const std::expected<void, Error> written = replace_file(*directory / "context.json", context.dump());
    if (!written.has_value()) {
        std::filesystem::remove_all(*directory, error);
        remove_person_if_empty(directory->parent_path());
        if (error) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "交互目录没有回到原状"});
        }
        return std::unexpected(written.error());
    }
    return *directory;
}

std::expected<nlohmann::json, Error> read_interaction_context(const std::filesystem::path& memory_root,
                                                             std::string_view open_id,
                                                             std::string_view interaction_id) {
    const std::expected<std::filesystem::path, Error> directory =
        interaction_directory(memory_root, open_id, interaction_id);
    if (!directory.has_value()) {
        return std::unexpected(directory.error());
    }
    const std::filesystem::path file = *directory / "context.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "这次交互没有上下文"});
    }
    std::ifstream input(file);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const nlohmann::json parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "这次交互没有上下文"});
    }
    return parsed;
}

std::expected<void, Error> end_interaction(const std::filesystem::path& memory_root,
                                          std::string_view open_id,
                                          std::string_view interaction_id,
                                          std::string_view op,
                                          std::string_view outcome,
                                          std::chrono::system_clock::time_point now) {
    if (op.empty() || outcome.empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "墓碑缺少 op 或 outcome"});
    }
    const std::expected<std::filesystem::path, Error> directory =
        interaction_directory(memory_root, open_id, interaction_id);
    if (!directory.has_value()) {
        return std::unexpected(directory.error());
    }
    std::error_code error;
    if (!std::filesystem::is_directory(*directory, error) || error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "这次交互已经结束"});
    }
    const nlohmann::json tombstone = {{"interaction_id", std::string(interaction_id)},
                                      {"actor", std::string(open_id)},
                                      {"op", std::string(op)},
                                      {"outcome", std::string(outcome)},
                                      {"time", format_beijing(now)}};
    const std::expected<void, Error> appended =
        append_jsonl(memory_root / "episodic" / "events.jsonl", tombstone);
    if (!appended.has_value()) {
        return std::unexpected(appended.error());
    }
    std::filesystem::remove_all(*directory, error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "交互目录没有删掉"});
    }
    remove_person_if_empty(directory->parent_path());
    return {};
}

std::expected<void, Error> recover_finished_interactions(const std::filesystem::path& memory_root) {
    const std::filesystem::path events = memory_root / "episodic" / "events.jsonl";
    std::error_code error;
    if (!std::filesystem::is_regular_file(events, error) || error) {
        return {};
    }
    std::ifstream input(events);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !is_tombstone(parsed)) {
            continue;
        }
        const std::expected<void, Error> deleted = delete_tombstoned_directory(memory_root, parsed);
        if (!deleted.has_value()) {
            return std::unexpected(deleted.error());
        }
    }
    return {};
}

}  // namespace robot_pm
