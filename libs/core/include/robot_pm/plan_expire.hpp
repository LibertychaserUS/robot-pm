#pragma once

// 这个文件负责让群里等待过久的计划超时。
// 不变量：后台投影不因 30 分钟超时取消；executing 不取消；超时不写表、不改日历。
// 规格：docs/sop.md 的「先确认再动手」，docs/sop/project.md。

#include "robot_pm/error.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace robot_pm {

struct CancelNotice {
    std::string mention_open_id;
    std::string text;
};

// 前置条件：等待中的计划在 working/<open_id>/<interaction_id>/。now 是 UTC。
// 失败：kEditRejected，计划文件保持原样。
// 超时只产生通知意图，不在这里发送。
[[nodiscard]] std::expected<std::vector<CancelNotice>, Error> expire_waiting_plans(
    const std::filesystem::path& memory_root, std::chrono::system_clock::time_point now);

}  // namespace robot_pm
