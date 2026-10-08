#pragma once

// 这个文件负责纯决定：现状加一件小事，得到新现状和最多一条动作。
// 不变量：不读时钟、不碰文件、不发请求。终态再收到事件也不改。
// 新需求加事件或动作，不在这里写具体业务。

#include "robot_pm/step.hpp"

namespace robot_pm {

// 前置条件：事件的人和事要和现状一致，否则原样返回。
// 失败：不失败。对不上、步骤名不对、或已经结束，都保持原样且不带动作。
[[nodiscard]] Decision decide(const WorkflowState& state, const Event& event);

} // namespace robot_pm
