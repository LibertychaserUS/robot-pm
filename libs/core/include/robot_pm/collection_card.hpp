#pragma once

// 这个文件负责程序拼出的职责收集卡片。
// 不变量：卡片由程序生成；模型不写卡片 JSON。
// 规格：docs/sop.md 的「你进群之后」。

#include <nlohmann/json.hpp>

namespace robot_pm {

// 前置条件：无。
// 失败：不失败。必填项的名字固定为 open_id 和 role。
[[nodiscard]] nlohmann::json build_collection_card();

}  // namespace robot_pm
