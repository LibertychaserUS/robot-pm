#include "robot_pm/child_boot.hpp"

#include "robot_pm/audit.hpp"
#include "robot_pm/working_memory.hpp"

#include <utility>

namespace robot_pm {
namespace {

[[nodiscard]] Error reject(std::string message) {
    return Error{ErrorCode::kEditRejected, std::move(message)};
}

}  // namespace

std::expected<ServiceHold, Error> ServiceHold::open(const std::filesystem::path& data_root) {
    if (data_root.empty()) {
        return std::unexpected(Error{ErrorCode::kConfigMissing, "缺少目录"});
    }
    std::error_code error;
    if (!std::filesystem::is_directory(data_root, error) || error) {
        return std::unexpected(reject("目录不存在"));
    }

    std::expected<std::string, Error> log = read_recovery_log(data_root);
    if (!log.has_value()) {
        return std::unexpected(log.error());
    }
    const std::expected<void, Error> applied = apply_recovery(data_root, *log);
    if (!applied.has_value()) {
        return std::unexpected(applied.error());
    }

    ServiceHold hold;
    hold.log_ = std::move(*log);
    hold.ready_ = true;
    return hold;
}

ServiceHold::ServiceHold(ServiceHold&& other) noexcept
    : log_(std::move(other.log_)), ready_(other.ready_) {
    other.ready_ = false;
}

ServiceHold& ServiceHold::operator=(ServiceHold&& other) noexcept {
    if (this != &other) {
        log_ = std::move(other.log_);
        ready_ = other.ready_;
        other.ready_ = false;
    }
    return *this;
}

const std::string& ServiceHold::recovered() const {
    return log_;
}

std::expected<void, Error> ServiceHold::store(const std::filesystem::path& path,
                                               std::string_view bytes) {
    if (!ready_) {
        return std::unexpected(reject("还没读完"));
    }
    return replace_file(path, bytes);
}

std::expected<void, Error> ServiceHold::send(std::ostream& out, std::string_view text) const {
    if (!ready_) {
        return std::unexpected(reject("还没读完"));
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return std::unexpected(reject("回执没写成"));
    }
    return {};
}

}  // namespace robot_pm
