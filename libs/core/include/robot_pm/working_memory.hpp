#pragma once

// 这个文件负责一次交互的临时目录。
// 不变量：一个人同时只有一个打开的目录；结束先写墓碑再删目录；不读别人的文件。
// 规格：docs/memory.md 的工作记忆条目。

#include "robot_pm/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>

namespace robot_pm {

// 前置条件：open_id 和 interaction_id 不含路径分隔符。context 是这一次的上下文，不是聊天记录。
// 失败：kEditRejected，这个人已经有打开的目录，或标识会越过自己的目录。失败时不新建目录。
[[nodiscard]] std::expected<std::filesystem::path, Error> begin_interaction(
    const std::filesystem::path& memory_root,
    std::string_view open_id,
    std::string_view interaction_id,
    const nlohmann::json& context);

// 前置条件：目录是 begin_interaction 建出来的。
// 失败：kEditRejected，这次交互没有上下文，或标识不属于这个人。
[[nodiscard]] std::expected<nlohmann::json, Error> read_interaction_context(
    const std::filesystem::path& memory_root,
    std::string_view open_id,
    std::string_view interaction_id);

// 前置条件：目录还在。op 和 outcome 非空。now 是 UTC。
// 失败：kEditRejected。墓碑没写成时目录还在。墓碑写成后目录删不掉，重启时再删。
[[nodiscard]] std::expected<void, Error> end_interaction(const std::filesystem::path& memory_root,
                                                        std::string_view open_id,
                                                        std::string_view interaction_id,
                                                        std::string_view op,
                                                        std::string_view outcome,
                                                        std::chrono::system_clock::time_point now);

// 前置条件：episodic/events.jsonl 里已有墓碑。重启后目录如果还在，就删掉。
// 失败：kEditRejected，删不掉。不改已有的墓碑行。
[[nodiscard]] std::expected<void, Error> recover_finished_interactions(
    const std::filesystem::path& memory_root);

}  // namespace robot_pm
