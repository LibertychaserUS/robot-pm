#include "robot_pm/frame.hpp"

#include <nlohmann/json.hpp>

namespace robot_pm {
namespace {

[[nodiscard]] std::string lower_copy(std::string_view text) {
    std::string copy(text);
    for (char& character : copy) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return copy;
}

[[nodiscard]] std::string header_value(std::string_view headers, std::string_view name) {
    const std::string lowered = lower_copy(headers);
    const std::string needle = std::string(name) + ":";
    const std::size_t mark = lowered.find(needle);
    if (mark == std::string::npos) {
        return {};
    }
    std::size_t cursor = mark + needle.size();
    while (cursor < headers.size() && (headers[cursor] == ' ' || headers[cursor] == '\t')) {
        ++cursor;
    }
    const std::size_t end = headers.find("\r\n", cursor);
    if (end == std::string::npos) {
        return std::string(headers.substr(cursor));
    }
    return std::string(headers.substr(cursor, end - cursor));
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (static_cast<std::size_t>(-1) - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return true;
}

[[nodiscard]] int hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

enum class ChunkKind { kNeedMore, kDone, kBad };

struct ChunkRead {
    ChunkKind kind{ChunkKind::kNeedMore};
    std::string body;
};

[[nodiscard]] ChunkRead read_chunks(std::string_view raw, bool closed) {
    ChunkRead read;
    std::size_t cursor = 0;
    while (cursor < raw.size() || !closed) {
        const std::size_t line_end = raw.find("\r\n", cursor);
        if (line_end == std::string::npos) {
            read.kind = closed ? ChunkKind::kBad : ChunkKind::kNeedMore;
            return read;
        }
        std::size_t size = 0;
        bool any = false;
        for (std::size_t index = cursor; index < line_end; ++index) {
            if (raw[index] == ';') {
                break;
            }
            const int digit = hex_digit(raw[index]);
            if (digit < 0) {
                read.kind = ChunkKind::kBad;
                return read;
            }
            any = true;
            if (size > (static_cast<std::size_t>(-1) >> 4U)) {
                read.kind = ChunkKind::kBad;
                return read;
            }
            size = (size << 4U) + static_cast<std::size_t>(digit);
        }
        if (!any) {
            read.kind = ChunkKind::kBad;
            return read;
        }
        cursor = line_end + 2;
        if (size == 0) {
            if (raw.size() < cursor + 2) {
                read.kind = closed ? ChunkKind::kBad : ChunkKind::kNeedMore;
                return read;
            }
            if (raw.substr(cursor, 2) != "\r\n") {
                read.kind = ChunkKind::kBad;
                return read;
            }
            read.kind = ChunkKind::kDone;
            return read;
        }
        if (raw.size() < cursor + size + 2) {
            read.kind = closed ? ChunkKind::kBad : ChunkKind::kNeedMore;
            return read;
        }
        read.body.append(raw.substr(cursor, size));
        cursor += size;
        if (raw.substr(cursor, 2) != "\r\n") {
            read.kind = ChunkKind::kBad;
            return read;
        }
        cursor += 2;
    }
    read.kind = closed ? ChunkKind::kBad : ChunkKind::kNeedMore;
    return read;
}

[[nodiscard]] bool stream_end_mark(std::string_view body) {
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        std::size_t end = body.find('\n', cursor);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string_view line = body.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line == "data: [DONE]" || line == "data:[DONE]") {
            return true;
        }
        cursor = end < body.size() ? end + 1 : body.size();
    }
    return false;
}

}  // namespace

HttpFrame read_http_frame(std::string_view raw, bool closed) {
    HttpFrame frame;
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) {
        frame.kind = closed ? FrameKind::kAbsent : FrameKind::kNeedMore;
        return frame;
    }
    const std::string_view headers = raw.substr(0, split);
    const std::string_view rest = raw.substr(split + 4);
    frame.timestamp = header_value(headers, "x-lark-request-timestamp");
    frame.nonce = header_value(headers, "x-lark-request-nonce");
    frame.signature = header_value(headers, "x-lark-signature");
    const std::string encoding = lower_copy(header_value(headers, "transfer-encoding"));
    if (encoding.find("chunked") != std::string::npos) {
        const ChunkRead chunks = read_chunks(rest, closed);
        if (chunks.kind == ChunkKind::kNeedMore) {
            frame.kind = FrameKind::kNeedMore;
            return frame;
        }
        if (chunks.kind == ChunkKind::kBad) {
            frame.kind = FrameKind::kAbsent;
            return frame;
        }
        frame.kind = FrameKind::kComplete;
        frame.body = chunks.body;
        return frame;
    }
    const std::string length_text = header_value(headers, "content-length");
    if (length_text.empty()) {
        frame.kind = closed ? FrameKind::kAbsent : FrameKind::kNeedMore;
        return frame;
    }
    std::size_t length = 0;
    if (!parse_size(length_text, length)) {
        frame.kind = FrameKind::kAbsent;
        return frame;
    }
    if (rest.size() < length) {
        frame.kind = closed ? FrameKind::kAbsent : FrameKind::kNeedMore;
        return frame;
    }
    frame.kind = FrameKind::kComplete;
    frame.body = std::string(rest.substr(0, length));
    return frame;
}

std::optional<std::string> model_reply_text(std::string_view body) {
    if (body.find("data:") == std::string::npos) {
        const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
        if (parsed.is_discarded() || (!parsed.is_object() && !parsed.is_array())) {
            return std::nullopt;
        }
        return std::string(body);
    }
    if (!stream_end_mark(body)) {
        return std::nullopt;
    }
    std::string last;
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        std::size_t end = body.find('\n', cursor);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string_view line = body.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::string_view payload;
        if (line.starts_with("data: ")) {
            payload = line.substr(6);
        } else if (line.starts_with("data:")) {
            payload = line.substr(5);
        }
        if (!payload.empty() && payload != "[DONE]") {
            last = std::string(payload);
        }
        cursor = end < body.size() ? end + 1 : body.size();
    }
    if (last.empty()) {
        return std::nullopt;
    }
    return last;
}

}  // namespace robot_pm
