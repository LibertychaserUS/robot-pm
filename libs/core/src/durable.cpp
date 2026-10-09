#include "robot_pm/durable.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/crypto.hpp"

#include <fstream>
#include <sstream>

namespace robot_pm {
namespace {

[[nodiscard]] std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string join_commits(const std::vector<nlohmann::json>& records) {
    std::string text;
    for (const nlohmann::json& body : records) {
        text += seal_commit(body);
    }
    return text;
}

}  // namespace

std::string seal_commit(const nlohmann::json& body) {
    const std::string dumped = body.dump();
    const nlohmann::json line = {{"body", body}, {"sum", sha256_hex(dumped)}};
    return line.dump() + "\n";
}

std::optional<nlohmann::json> open_commit(std::string_view line) {
    if (line.empty()) {
        return std::nullopt;
    }
    const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("body") || !parsed.contains("sum") ||
        !parsed.at("sum").is_string() || !parsed.at("body").is_object()) {
        return std::nullopt;
    }
    if (sha256_hex(parsed.at("body").dump()) != parsed.at("sum").get_ref<const std::string&>()) {
        return std::nullopt;
    }
    return parsed.at("body");
}

std::vector<nlohmann::json> read_durable_bytes(std::string_view bytes) {
    std::vector<nlohmann::json> records;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const std::size_t end = bytes.find('\n', cursor);
        if (end == std::string::npos) {
            break;
        }
        std::string_view line = bytes.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            const std::optional<nlohmann::json> opened = open_commit(line);
            if (!opened) {
                break;
            }
            records.push_back(*opened);
        }
        cursor = end + 1;
    }
    return records;
}

std::expected<void, Error> recover_durable_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return {};
    }
    const std::string bytes = file_bytes(path);
    const std::string kept = join_commits(read_durable_bytes(bytes));
    if (kept == bytes) {
        return {};
    }
    return replace_file(path, kept);
}

}  // namespace robot_pm
