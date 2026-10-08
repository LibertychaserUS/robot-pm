#pragma once

// 这个文件负责按顺序执行动作。
// 不变量：前面一条没做成，后面的不做。决定函数不调用这里。
// 问模型、发飞书、写表格、订日历都是动作数据，本文件只按列表往下做。

#include "robot_pm/error.hpp"
#include "robot_pm/step.hpp"

#include <expected>
#include <stop_token>
#include <vector>

namespace robot_pm {

class EffectInterpreter {
  public:
    EffectInterpreter() = default;
    virtual ~EffectInterpreter() = default;
    EffectInterpreter(const EffectInterpreter&) = delete;
    EffectInterpreter& operator=(const EffectInterpreter&) = delete;
    EffectInterpreter(EffectInterpreter&&) = delete;
    EffectInterpreter& operator=(EffectInterpreter&&) = delete;

    // 前置条件：stop 被请求时必须返回，不能一直等。
    // 失败：沿用动作自己的错误。返回失败时，这一条不算做成。
    [[nodiscard]] virtual std::expected<nlohmann::json, Error> apply(const Effect& effect,
                                                                     std::stop_token stop) = 0;
};

// 前置条件：effects 按要做的顺序排好。stop 被请求时停止，不再做后面的。
// 失败：第一条失败的错误。失败点之后的动作不会调用 apply。
[[nodiscard]] std::expected<std::vector<nlohmann::json>, Error>
apply_effects(EffectInterpreter& interpreter, const std::vector<Effect>& effects,
              std::stop_token stop);

} // namespace robot_pm
