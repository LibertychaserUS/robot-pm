#include "robot_pm/child_boot.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

constexpr std::string_view kUsage = "用法\n目录 这一次的数据目录\n方式 崩溃、停着、失手、做完或连崩\n";

[[nodiscard]] bool append_line(const std::filesystem::path& path, std::string_view line) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t wrote = ::write(fd, line.data() + offset, line.size() - offset);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return false;
        }
        offset += static_cast<std::size_t>(wrote);
    }
    const int synced = ::fsync(fd);
    ::close(fd);
    return synced == 0;
}

[[nodiscard]] bool crashed_once(std::string_view log) {
    return log.find("崩过") != std::string_view::npos;
}

[[nodiscard]] int send_receipt(robot_pm::ServiceHold& hold, const std::filesystem::path& path) {
    std::ofstream receipt(path, std::ios::binary | std::ios::noreplace);
    if (!receipt) {
        return 1;
    }
    const auto sent = hold.send(receipt, hold.recovered());
    if (!sent.has_value()) {
        return 1;
    }
    receipt.flush();
    return receipt ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--帮助") {
        std::cout << kUsage;
        return 0;
    }
    if (argc != 3) {
        std::cout << kUsage;
        return 2;
    }

    const std::filesystem::path root{argv[1]};
    const std::string_view mode{argv[2]};
    if (mode != "崩溃" && mode != "停着" && mode != "失手" && mode != "做完" && mode != "连崩") {
        std::cout << kUsage;
        return 2;
    }

    auto hold = robot_pm::ServiceHold::open(root);
    if (!hold.has_value()) {
        return 1;
    }

    if ((mode == "崩溃" || mode == "失手") && !crashed_once(hold->recovered())) {
        std::string noted = hold->recovered();
        noted.append("崩过\n");
        const auto stored = hold->store(root / "episodic" / "events.jsonl", noted);
        if (!stored.has_value()) {
            return 1;
        }
        if (mode == "崩溃") {
            ::kill(::getpid(), SIGKILL);
        }
        return 1;
    }

    if (mode == "连崩") {
        if (!append_line(root / "次数", "又\n")) {
            return 1;
        }
        ::kill(::getpid(), SIGKILL);
        return 1;
    }

    if (mode == "停着") {
        const std::string line = std::to_string(::getpid()) + "\n";
        const auto stored = hold->store(root / "活着", line);
        if (!stored.has_value()) {
            return 1;
        }
        for (;;) {
            ::pause();
        }
    }

    return send_receipt(*hold, root / "回执");
}
