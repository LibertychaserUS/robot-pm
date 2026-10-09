#pragma once

// 这个文件负责一条日志算一次提交。
// 不变量：同步完成之前都不算数；撕掉的尾巴不是记录；坏的校验和不是记录。
// 重启时先把尾巴丢掉，再读剩下的完整行。

#include "robot_pm/error.hpp"

#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace robot_pm {

// 前置条件：body 是一个 JSON 对象。
// 失败：不失败。结果是一行，末尾有换行。
[[nodiscard]] std::string seal_commit(const nlohmann::json& body);

// 前置条件：line 不含换行。
// 失败：不失败。校验和不对或不是对象时返回空。
[[nodiscard]] std::optional<nlohmann::json> open_commit(std::string_view line);

// 前置条件：bytes 是文件里的全部字节，可以在一行中间断开。
// 失败：不失败。只返回连续的完整提交，坏行之后的都不要。
[[nodiscard]] std::vector<nlohmann::json> read_durable_bytes(std::string_view bytes);

// 前置条件：path 可以不存在。
// 失败：kEditRejected，原文件还在。把撕掉的尾巴从磁盘上拿掉。
[[nodiscard]] std::expected<void, Error> recover_durable_file(const std::filesystem::path& path);

}  // namespace robot_pm
