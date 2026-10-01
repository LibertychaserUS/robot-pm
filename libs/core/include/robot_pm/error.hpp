#pragma once

// 这个文件负责可恢复失败的代码和给人看的消息。
// 不变量：消息里不出现密钥；代码不另起别名。
// 错误码供调用方区分缺配置、拒绝写入和外部调用失败。

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
