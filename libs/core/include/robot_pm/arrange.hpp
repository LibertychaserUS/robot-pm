#pragma once

// 这个文件负责谁先谁后。
// 不变量：同时最多五个人；每人同时只办一件；第六个人不建文件夹。
// 同一个人排队且顺序不变。取消只停这一件，不碰别人。

#include "robot_pm/act.hpp"
#include "robot_pm/decide.hpp"
#include "robot_pm/error.hpp"
#include "robot_pm/step.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace robot_pm {

inline constexpr int kMaxInFlight = 5;

class StepClock {
  public:
    StepClock() = default;
    virtual ~StepClock() = default;
    StepClock(const StepClock&) = delete;
    StepClock& operator=(const StepClock&) = delete;
    StepClock(StepClock&&) = delete;
    StepClock& operator=(StepClock&&) = delete;

    // 前置条件：返回 UTC。安排会把它换成北京时间再交给决定。
    [[nodiscard]] virtual std::chrono::system_clock::time_point now() const = 0;
};

struct WorkflowRequest {
    std::string person_id;
    std::string workflow_id;
    std::vector<StepSpec> steps;
};

struct ArrangeView {
    int live = 0;
    std::map<std::string, int> queued;
    std::map<std::string, bool> person_live;
    std::map<std::string, WorkflowState> states;
};

struct Lane;

class Arrange {
  public:
    // 前置条件：interpreter 和 clock 活得比本对象久。root 是这次测试或运行的根目录。
    Arrange(EffectInterpreter& interpreter, StepClock& clock, std::filesystem::path root);
    ~Arrange();
    Arrange(const Arrange&) = delete;
    Arrange& operator=(const Arrange&) = delete;
    Arrange(Arrange&&) = delete;
    Arrange& operator=(Arrange&&) = delete;

    // 前置条件：人和事的标识只含字母、数字、点、下划线和短横线。步骤名非空。
    // 失败：kEditRejected，名字不对或没有步骤，不建文件夹。
    // 失败：kForbidden，已经有五个人在办，不建文件夹。
    [[nodiscard]] std::expected<void, Error> submit(WorkflowRequest request);

    // 前置条件：workflow_id 是已经提交的事。没有这件事先记下来，等它出现再停。
    // 只停这一件。同一个人排在后面的事还在。别人不受影响。
    void cancel(std::string_view workflow_id);

    [[nodiscard]] ArrangeView view() const;

    // 前置条件：pred 只读视图，不再调用本对象。预算到了还没满足就返回 false。
    [[nodiscard]] bool wait_until(const std::function<bool(const ArrangeView&)>& pred,
                                  std::chrono::milliseconds budget);

  private:
    void worker(Lane& lane);
    void run_job(Lane& lane, std::string workflow_id, std::vector<StepSpec> steps);
    void clear_current(Lane& lane);
    void reap();
    [[nodiscard]] int live_locked() const;
    [[nodiscard]] ArrangeView view_locked() const;
    void publish(const WorkflowState& state);
    [[nodiscard]] std::expected<void, Error> open_directory(std::string_view person_id,
                                                            std::string_view workflow_id);
    void close_directory(std::string_view person_id, std::string_view workflow_id);

    EffectInterpreter& interpreter_;
    StepClock& clock_;
    std::filesystem::path root_;
    struct Locked;
    std::unique_ptr<Locked> locked_;
};

} // namespace robot_pm
