#include "robot_pm/audit.hpp"

#include <chrono>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace robot_pm {

std::expected<void, Error> replace_file(const std::filesystem::path& destination, std::string_view bytes) {
    std::error_code error;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "记忆目录没有写成"});
        }
    }
    const std::filesystem::path temporary = destination.string() + ".tmp";
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "记忆文件没有写成"});
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            ::close(descriptor);
            std::filesystem::remove(temporary, error);
            return std::unexpected(Error{ErrorCode::kEditRejected, "记忆文件没有写成"});
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        std::filesystem::remove(temporary, error);
        return std::unexpected(Error{ErrorCode::kEditRejected, "记忆文件没有写成"});
    }
    ::close(descriptor);
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(Error{ErrorCode::kEditRejected, "记忆文件没有写成"});
    }
    return {};
}

std::string format_beijing(std::chrono::system_clock::time_point now) {
    const std::chrono::zoned_time zoned{"Asia/Shanghai", now};
    return std::format("{:%Y-%m-%d %H:%M}", zoned);
}

std::expected<void, Error> append_jsonl(const std::filesystem::path& destination,
                                        const nlohmann::json& line) {
    if (!line.is_object()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "日志只能追加一条 JSON 对象"});
    }
    std::string existing;
    std::error_code error;
    if (std::filesystem::is_regular_file(destination, error)) {
        std::ifstream input(destination);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        existing = buffer.str();
    }
    existing.append(line.dump());
    existing.push_back('\n');
    return replace_file(destination, existing);
}

}  // namespace robot_pm
