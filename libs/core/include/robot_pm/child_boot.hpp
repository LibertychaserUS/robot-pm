#pragma once

// 这个文件负责子进程开工。
// 不变量：先把日志读完，然后才可以写、存、发回执。
// 不占数据目录的锁，不另写一份日志。父进程不调用这里，也不发回执。

#include "robot_pm/error.hpp"

#include <expected>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

namespace robot_pm {

class ServiceHold {
  public:
    // 前置条件：data_root 是已经准备好的数据目录。
    // 失败：kConfigMissing 或 kEditRejected。失败时不发回执。
    [[nodiscard]] static std::expected<ServiceHold, Error> open(
        const std::filesystem::path& data_root);

    ServiceHold(const ServiceHold&) = delete;
    ServiceHold& operator=(const ServiceHold&) = delete;
    ServiceHold(ServiceHold&& other) noexcept;
    ServiceHold& operator=(ServiceHold&& other) noexcept;

    // 前置条件：open 已经成功。返回读完的日志原文。
    [[nodiscard]] const std::string& recovered() const;

    // 前置条件：日志已经读完。
    // 失败：kEditRejected，还没读完，或没写成。
    [[nodiscard]] std::expected<void, Error> store(const std::filesystem::path& path,
                                                   std::string_view bytes);

    // 前置条件：日志已经读完。text 是这次要给人的回执。
    // 失败：kEditRejected，还没读完，或没发出去。
    [[nodiscard]] std::expected<void, Error> send(std::ostream& out, std::string_view text) const;

  private:
    ServiceHold() = default;

    std::string log_;
    bool ready_{false};
};

}  // namespace robot_pm
