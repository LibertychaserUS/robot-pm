#pragma once

// 这个文件负责一次模型动作的输入和输出。
// 不变量：一次动作只有系统提示和用户消息，没有工具，没有下一轮。
// 规格：docs/cpp23-standard.md 的「做法」。

#include "robot_pm/error.hpp"

#include <expected>
#include <string>

namespace robot_pm {

struct ModelRequest {
    std::string system_prompt;
    std::string user_message;
};

struct ModelResponse {
    int exit_code{0};
    std::string stdout_text;
};

class ModelAct {
public:
    virtual ~ModelAct() = default;

    // 前置条件：system_prompt 是提示文件原文，user_message 只含这一步的输入。
    // 失败：kBridgeFailed，进程没有启动。退出码非零仍放在 ModelResponse 里，由门禁丢弃 stdout。
    [[nodiscard]] virtual std::expected<ModelResponse, Error> run(const ModelRequest& request) = 0;
};

}  // namespace robot_pm
