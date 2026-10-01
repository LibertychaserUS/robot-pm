#pragma once

// 这个文件负责把合法清单投影成模版行。
// 不变量：不写状态列；不向群里发计划；清单不合法时一行都不产出。
// 规格：docs/sop/project.md，docs/prd-to-bitable.md。

#include "robot_pm/error.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace robot_pm {

// 前置条件：manifest 是对象。source_text 为空表示这份清单来自已合法的 JSON 文件。
// 非空时，每个工作项的 source_quote 必须是 source_text 的连续子串。
// 失败：kEditRejected，不产出行。
[[nodiscard]] std::expected<std::vector<nlohmann::json>, Error> project_manifest(
    const nlohmann::json& manifest, std::string_view source_text);

}  // namespace robot_pm
