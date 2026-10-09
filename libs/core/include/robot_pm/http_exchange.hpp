#pragma once

// 这个文件负责发出一次 HTTP 请求，并只在分帧完整时返回正文。
// 不变量：连接提前断开就当这次传输没有发生。不把半截正文交给调用方。

#include "robot_pm/error.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace robot_pm {

struct HttpExchange {
    int status{0};
    std::string body;
};

// 前置条件：url 是 http 或 https。headers 不含密钥以外的换行。
// 失败：kBridgeFailed，没有拿到完整响应。
[[nodiscard]] std::expected<HttpExchange, Error> http_exchange(
        std::string_view method,
        std::string_view url,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::string_view body);

}  // namespace robot_pm
