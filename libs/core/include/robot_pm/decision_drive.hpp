#pragma once

// 这个文件负责一次决定从已提交状态走到下一状态。
// 不变量：切断之后只可能是原来的状态，或新的完整状态。不能是混在一起的半份。
// 外面对表格、日历和卡片只发生 0 次或 1 次。人看到的回复要么是上一份完整的，要么是新的完整的。

#include <filesystem>
#include <optional>
#include <set>
#include <string>

namespace robot_pm {

enum class DecisionCut {
    kWebhookBeforeSignature = 0,
    kFramingNotFinished,
    kWebhookAfterSignatureBeforeBody,
    kWebhookMiddleOfBody,
    kFramingDoneBadSignature,
    kSignatureMatches,
    kModelRequestFraming,
    kModelResponseFraming,
    kModelStreamBeforeEnd,
    kLocalWrite,
    kDiskFull,
    kWriteFailed,
    kLocalStoreSync,
    kRead,
    kReadStopped,
    kFeedbackBeforeFirstByte,
    kFeedbackMiddle,
    kAfterSendBeforeLocalCommit,
    kAfterLocalCommitBeforeAck,
};

inline constexpr DecisionCut kDecisionCuts[] = {
        DecisionCut::kWebhookBeforeSignature,
        DecisionCut::kFramingNotFinished,
        DecisionCut::kWebhookAfterSignatureBeforeBody,
        DecisionCut::kWebhookMiddleOfBody,
        DecisionCut::kFramingDoneBadSignature,
        DecisionCut::kSignatureMatches,
        DecisionCut::kModelRequestFraming,
        DecisionCut::kModelResponseFraming,
        DecisionCut::kModelStreamBeforeEnd,
        DecisionCut::kLocalWrite,
        DecisionCut::kDiskFull,
        DecisionCut::kWriteFailed,
        DecisionCut::kLocalStoreSync,
        DecisionCut::kRead,
        DecisionCut::kReadStopped,
        DecisionCut::kFeedbackBeforeFirstByte,
        DecisionCut::kFeedbackMiddle,
        DecisionCut::kAfterSendBeforeLocalCommit,
        DecisionCut::kAfterLocalCommitBeforeAck,
};

inline constexpr const char* kFeedbackOld = "上一张";
inline constexpr const char* kFeedbackNew = "新的回复";

struct DecisionPerson {
    std::string feedback{kFeedbackOld};
    int table{0};
    int calendar{0};
    int card{0};
    std::set<std::string> seen{"d0"};
};

struct DecisionView {
    bool present{false};
    int generation{0};
    std::string feedback;
    std::string status;
    std::string decision;
    std::string table;
    std::string calendar;
    std::string card;
};

struct DriveResult {
    bool read_returned{false};
    bool violated{false};
    DecisionView read_view;
};

[[nodiscard]] const char* decision_cut_name(DecisionCut cut);

// 前置条件：目录可写。写成上一份已提交的决定。
void plant_state_n(const std::filesystem::path& dir);

// 前置条件：目录里是 plant_state_n 留下的文件，或已经有下一份提交。
// cut 有值时，在那一步停下并丢掉没同步的内存。空表示把这次决定走完。
// 同一次决定已经提交过时，不再写、不再对外做第二次。
[[nodiscard]] DriveResult run_decision(const std::filesystem::path& dir, std::optional<DecisionCut> cut);

// 前置条件：进程已经停。只从磁盘上的完整行恢复，然后最多对外做一次。
[[nodiscard]] DecisionView reopen_decision(const std::filesystem::path& dir, DecisionPerson& person);

}  // namespace robot_pm
