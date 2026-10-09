#pragma once

// 这个文件负责 HTTP 传输什么时候算完整。
// 不变量：正文完整只看 Content-Length 或分块结束，不看 JSON 是不是能解析。
// 连接提前断开就是没有这次传输。流式模型回复还要等到它自己的结束标记。

#include <optional>
#include <string>
#include <string_view>

namespace robot_pm {

enum class FrameKind { kNeedMore, kComplete, kAbsent };

struct HttpFrame {
    FrameKind kind{FrameKind::kNeedMore};
    std::string body;
    std::string timestamp;
    std::string nonce;
    std::string signature;
};

// 前置条件：raw 是到目前为止收到的字节。closed 表示对端已经关掉连接。
// 失败：不失败。缺长度或缺最后一块，并且连接已断，就是 kAbsent。
[[nodiscard]] HttpFrame read_http_frame(std::string_view raw, bool closed);

// 前置条件：body 是已经按 HTTP 分帧收全的模型响应正文。
// 失败：不失败。事件流没有 data: [DONE] 时返回空，调用方不得解析，也不得发送或落盘。
[[nodiscard]] std::optional<std::string> model_reply_text(std::string_view body);

}  // namespace robot_pm
