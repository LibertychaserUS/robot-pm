#pragma once

// 这个文件负责一件事的步骤和动作，只放数据。
// 不变量：一次事件只前进一步，最多带出一条动作；这里不发消息、不写表。
// 开会、改状态、进群不写在这里，它们以后只是步骤列表里的一步。

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace robot_pm {

enum class EffectKind {
    kModelCall,
    kFeishuPost,
    kBitableWrite,
    kCalendar,
};

struct Effect {
    EffectKind kind{};
    std::string step;
    nlohmann::json payload = nlohmann::json::object();
};

enum class EventKind {
    kRunStep,
    kEffectOk,
    kEffectFail,
    kCancel,
};

struct Event {
    EventKind kind{};
    std::string person_id;
    std::string workflow_id;
    std::string step;
    std::string time;
    nlohmann::json payload = nlohmann::json::object();
};

enum class Phase {
    kIdle,
    kReady,
    kAwaiting,
    kDone,
    kCancelled,
    kFailed,
};

struct StepSpec {
    std::string name;
    std::optional<EffectKind> effect;
    nlohmann::json payload = nlohmann::json::object();
};

struct WorkflowState {
    std::string id;
    std::string person_id;
    std::vector<StepSpec> steps;
    std::size_t index = 0;
    Phase phase = Phase::kIdle;
    std::string started_at;
    std::vector<std::string> finished_steps;
    std::optional<Effect> inflight;
};

struct Decision {
    WorkflowState state;
    std::vector<Effect> effects;
};

} // namespace robot_pm
