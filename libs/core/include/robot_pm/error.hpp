#pragma once

// 这个文件负责可恢复失败的代码和给人看的消息。
// 不变量：消息里不出现密钥；代码不另起别名。
// 规格：docs/cpp23-standard.md 的「缺陷分类」。

#include <string>
#include <string_view>

namespace robot_pm {

enum class ErrorCode {
    kConfigMissing,
    kUnknownEvent,
    kForbidden,
    kUnknownField,
    kUnsupportedType,
    kEditRejected,
    kBridgeFailed,
};

struct Error {
    ErrorCode code{};
    std::string message;
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code);

}  // namespace robot_pm
