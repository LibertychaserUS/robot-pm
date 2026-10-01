#pragma once

// 这个文件负责从云上收件目录导入一份文件，并在后台投影。
// 不变量：一次只处理 inbox 里的一个文件；群文件不导入；不向群里发导入进度。
// 规格：docs/user-manual.md 的「文档」。

#include "robot_pm/error.hpp"
#include "robot_pm/model_act.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace robot_pm {

struct InboxOutcome {
    bool ignored{false};
    bool projected{false};
    bool uploaded{false};
    bool group_notified{false};
};

class TextImport {
public:
    virtual ~TextImport() = default;

    // 前置条件：path 是 md、docx 或 pdf。
    // 失败：kBridgeFailed，进程没有启动。
    [[nodiscard]] virtual std::expected<nlohmann::json, Error> read(
        const std::filesystem::path& path) = 0;
};

class BackgroundUpload {
public:
    virtual ~BackgroundUpload() = default;

    // 前置条件：rows 已通过投影。失败时不得留下部分行。
    // 失败：kBridgeFailed 或 kEditRejected，表保持调用前的样子。
    [[nodiscard]] virtual std::expected<void, Error> upload(
        const std::vector<nlohmann::json>& rows) = 0;
};

class GroupNotice {
public:
    virtual ~GroupNotice() = default;

    virtual void post(std::string_view text) = 0;
};

// 前置条件：event 是飞书报文。
// 失败：不失败。群里的文件消息返回 true，私聊和普通文本返回 false。
[[nodiscard]] bool is_group_file_message(const nlohmann::json& event);

// 前置条件：inbox 是收件目录，可以不存在。
// 失败：kUnknownEvent，这不是群文件。群文件不读 inbox、不写审计、不发通知。
[[nodiscard]] std::expected<InboxOutcome, Error> ignore_group_file_message(
    const nlohmann::json& event, const std::filesystem::path& inbox, GroupNotice& notice);

// 前置条件：memory_root/inbox 存在。text_import 和 extract 只在非 JSON 文件时使用。
// 失败：kConfigMissing 没有收件目录；kEditRejected 审计没有写成。
// 不调用 notice。失败不上传。
[[nodiscard]] std::expected<InboxOutcome, Error> import_next_inbox_file(
    const std::filesystem::path& memory_root,
    std::chrono::system_clock::time_point now,
    TextImport* text_import,
    ModelAct* extract,
    BackgroundUpload& upload,
    GroupNotice& notice);

}  // namespace robot_pm
