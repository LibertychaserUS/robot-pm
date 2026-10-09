#include "supervise.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kUsage = "用法\n程序 要看管的那个程序\n";

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--帮助") {
        std::cout << kUsage;
        return 0;
    }
    if (argc < 3 || std::string_view{argv[1]} != "--程序") {
        std::cout << kUsage;
        return 2;
    }

    std::vector<std::string> command;
    command.reserve(static_cast<std::size_t>(argc - 2));
    for (int index = 2; index < argc; ++index) {
        command.emplace_back(argv[index]);
    }

    robot_pm::arm_stop_signals();
    const auto watched = robot_pm::watch(command);
    if (!watched.has_value()) {
        return 1;
    }
    return 0;
}
