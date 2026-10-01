#pragma once

// 这个文件负责只追加的审计和情景记录。
// 不变量：旧行保持原样；先写临时文件再改名；失败不留下半行。
// 规格：docs/memory.md 的「日志」和「写入」。

#include "robot_pm/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace robot_pm {

// 前置条件：now 是 UTC 时间点。
// 失败：不失败。输出是北京时间 YYYY-MM-DD HH:mm。
[[nodiscard]] std::string format_beijing(std::chrono::system_clock::time_point now);

// 前置条件：line 是一条 JSON 对象。parent 目录可以还不存在。
// 失败：kEditRejected，目标文件保持调用前的字节。
[[nodiscard]] std::expected<void, Error> append_jsonl(const std::filesystem::path& destination,
                                                      const nlohmann::json& line);

// 前置条件：bytes 是要落盘的全部内容。parent 目录可以还不存在。
// 失败：kEditRejected，目标文件保持调用前的字节。
[[nodiscard]] std::expected<void, Error> replace_file(const std::filesystem::path& destination,
                                                      std::string_view bytes);

}  // namespace robot_pm
