#include "robot_pm/arrange.hpp"

#include "robot_pm/audit.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>

namespace robot_pm {
namespace {

[[nodiscard]] bool safe_id(std::string_view id) {
    if (id.empty() || id == "." || id == "..") {
        return false;
    }
    for (const char character : id) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool mark = character == '.' || character == '_' || character == '-';
        if (!digit && !lower && !upper && !mark) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Event make_event(EventKind kind, const WorkflowState& state, std::string step) {
    Event event;
    event.kind = kind;
    event.person_id = state.person_id;
    event.workflow_id = state.id;
    event.step = std::move(step);
    return event;
}

} // namespace

struct WorkflowJob {
    std::string workflow_id;
    std::vector<StepSpec> steps;
};

struct Lane {
    std::string person_id;
    std::deque<WorkflowJob> queue;
    std::thread thread;
    bool finished = false;
    std::string current_id;
    std::optional<std::stop_source> stop;
};

struct Arrange::Locked {
    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, std::unique_ptr<Lane>> lanes;
    std::map<std::string, WorkflowState> states;
    std::set<std::string> cancelled;
    bool stopping = false;
};

Arrange::Arrange(EffectInterpreter& interpreter, StepClock& clock, std::filesystem::path root)
    : interpreter_(interpreter), clock_(clock), root_(std::move(root)),
      locked_(std::make_unique<Locked>()) {}

Arrange::~Arrange() {
    {
        std::lock_guard lock(locked_->mu);
        locked_->stopping = true;
        for (auto& [person, lane] : locked_->lanes) {
            static_cast<void>(person);
            if (lane->stop.has_value()) {
                lane->stop->request_stop();
            }
        }
        locked_->cv.notify_all();
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(locked_->mu);
        for (auto& [person, lane] : locked_->lanes) {
            static_cast<void>(person);
            if (lane->thread.joinable()) {
                threads.push_back(std::move(lane->thread));
            }
        }
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

std::expected<void, Error> Arrange::submit(WorkflowRequest request) {
    if (!safe_id(request.person_id) || !safe_id(request.workflow_id)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "名字不对"});
    }
    if (request.steps.empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "没有步骤"});
    }
    for (const StepSpec& step : request.steps) {
        if (step.name.empty()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "没有步骤"});
        }
    }

    for (;;) {
        reap();
        std::lock_guard lock(locked_->mu);
        if (locked_->stopping) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "已经取消"});
        }
        const auto existing = locked_->lanes.find(request.person_id);
        if (existing != locked_->lanes.end() && !existing->second->finished) {
            existing->second->queue.push_back(WorkflowJob{request.workflow_id, request.steps});
            locked_->cv.notify_all();
            return {};
        }
        if (existing != locked_->lanes.end()) {
            continue;
        }
        if (live_locked() >= kMaxInFlight) {
            return std::unexpected(Error{ErrorCode::kForbidden, "人满了，请稍后再试"});
        }

        auto lane = std::make_unique<Lane>();
        lane->person_id = request.person_id;
        lane->queue.push_back(WorkflowJob{request.workflow_id, request.steps});
        Lane* const raw = lane.get();
        locked_->lanes.emplace(request.person_id, std::move(lane));
        raw->thread = std::thread([this, raw] { worker(*raw); });
        locked_->cv.notify_all();
        return {};
    }
}

void Arrange::cancel(std::string_view workflow_id) {
    const std::string id{workflow_id};
    std::lock_guard lock(locked_->mu);
    locked_->cancelled.insert(id);
    for (auto& [person, lane] : locked_->lanes) {
        std::deque<WorkflowJob> kept;
        for (WorkflowJob& job : lane->queue) {
            if (job.workflow_id == id) {
                WorkflowState state;
                state.id = id;
                state.person_id = person;
                state.phase = Phase::kCancelled;
                locked_->states[id] = std::move(state);
                continue;
            }
            kept.push_back(std::move(job));
        }
        lane->queue = std::move(kept);
        if (lane->current_id == id && lane->stop.has_value()) {
            lane->stop->request_stop();
        }
    }
    locked_->cv.notify_all();
}

ArrangeView Arrange::view() const {
    std::lock_guard lock(locked_->mu);
    return view_locked();
}

bool Arrange::wait_until(const std::function<bool(const ArrangeView&)>& pred,
                         std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        reap();
        std::unique_lock lock(locked_->mu);
        if (pred(view_locked())) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        locked_->cv.wait_until(lock, deadline);
    }
}

void Arrange::worker(Lane& lane) {
    for (;;) {
        WorkflowJob job;
        {
            std::lock_guard lock(locked_->mu);
            if (locked_->stopping || lane.queue.empty()) {
                lane.finished = true;
                lane.current_id.clear();
                lane.stop.reset();
                locked_->cv.notify_all();
                return;
            }
            job = std::move(lane.queue.front());
            lane.queue.pop_front();
        }
        run_job(lane, std::move(job.workflow_id), std::move(job.steps));
    }
}

void Arrange::run_job(Lane& lane, std::string workflow_id, std::vector<StepSpec> steps) {
    WorkflowState state;
    state.id = std::move(workflow_id);
    state.person_id = lane.person_id;
    state.steps = std::move(steps);

    std::stop_token token;
    {
        std::lock_guard lock(locked_->mu);
        lane.stop.emplace();
        lane.current_id = state.id;
        token = lane.stop->get_token();
        if (locked_->stopping || locked_->cancelled.contains(state.id)) {
            lane.stop->request_stop();
        }
    }
    if (token.stop_requested()) {
        state = decide(state, make_event(EventKind::kCancel, state, {})).state;
        publish(state);
        clear_current(lane);
        return;
    }

    const std::expected<void, Error> opened = open_directory(state.person_id, state.id);
    if (!opened.has_value()) {
        state.phase = Phase::kFailed;
        publish(state);
        clear_current(lane);
        return;
    }

    while (state.phase == Phase::kIdle || state.phase == Phase::kReady) {
        if (token.stop_requested()) {
            state = decide(state, make_event(EventKind::kCancel, state, {})).state;
            break;
        }
        if (state.index >= state.steps.size()) {
            state.phase = Phase::kDone;
            break;
        }
        Event run = make_event(EventKind::kRunStep, state, state.steps[state.index].name);
        run.time = format_beijing(clock_.now());
        const Phase phase_before = state.phase;
        const std::size_t index_before = state.index;
        Decision decision = decide(state, run);
        state = std::move(decision.state);
        publish(state);
        if (decision.effects.empty()) {
            if (state.index == index_before && state.phase == phase_before) {
                state.phase = Phase::kFailed;
                break;
            }
            continue;
        }
        const std::expected<std::vector<nlohmann::json>, Error> applied =
            apply_effects(interpreter_, decision.effects, token);
        if (!applied.has_value()) {
            if (token.stop_requested()) {
                state = decide(state,
                               make_event(EventKind::kCancel, state, decision.effects.front().step))
                            .state;
            } else {
                state = decide(state, make_event(EventKind::kEffectFail, state,
                                                 decision.effects.front().step))
                            .state;
            }
            break;
        }
        for (const Effect& effect : decision.effects) {
            state = decide(state, make_event(EventKind::kEffectOk, state, effect.step)).state;
        }
    }

    publish(state);
    close_directory(state.person_id, state.id);
    clear_current(lane);
}

void Arrange::clear_current(Lane& lane) {
    std::lock_guard lock(locked_->mu);
    lane.current_id.clear();
    lane.stop.reset();
}

void Arrange::reap() {
    std::vector<std::string> people;
    {
        std::lock_guard lock(locked_->mu);
        for (const auto& [person, lane] : locked_->lanes) {
            if (lane->finished && lane->thread.joinable()) {
                people.push_back(person);
            }
        }
    }
    for (const std::string& person : people) {
        std::thread thread;
        {
            std::lock_guard lock(locked_->mu);
            const auto it = locked_->lanes.find(person);
            if (it == locked_->lanes.end() || !it->second->finished ||
                !it->second->thread.joinable()) {
                continue;
            }
            thread = std::move(it->second->thread);
        }
        if (thread.joinable()) {
            thread.join();
        }
        std::lock_guard lock(locked_->mu);
        const auto it = locked_->lanes.find(person);
        if (it != locked_->lanes.end() && it->second->finished && !it->second->thread.joinable()) {
            locked_->lanes.erase(it);
        }
    }
}

int Arrange::live_locked() const {
    int live = 0;
    for (const auto& [person, lane] : locked_->lanes) {
        static_cast<void>(person);
        if (!lane->finished) {
            ++live;
        }
    }
    return live;
}

ArrangeView Arrange::view_locked() const {
    ArrangeView view;
    for (const auto& [person, lane] : locked_->lanes) {
        if (lane->finished) {
            continue;
        }
        ++view.live;
        view.person_live[person] = true;
        view.queued[person] = static_cast<int>(lane->queue.size());
    }
    view.states = locked_->states;
    return view;
}

void Arrange::publish(const WorkflowState& state) {
    std::lock_guard lock(locked_->mu);
    locked_->states[state.id] = state;
    locked_->cv.notify_all();
}

std::expected<void, Error> Arrange::open_directory(std::string_view person_id,
                                                   std::string_view workflow_id) {
    if (!safe_id(person_id) || !safe_id(workflow_id)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "名字不对"});
    }
    const std::filesystem::path person_path = root_ / std::string(person_id);
    const std::filesystem::path workflow_path = person_path / std::string(workflow_id);
    std::error_code error;
    std::filesystem::create_directories(person_path, error);
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "文件夹没建成"});
    }
    error.clear();
    if (!std::filesystem::create_directory(workflow_path, error) || error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "文件夹没建成"});
    }
    return {};
}

void Arrange::close_directory(std::string_view person_id, std::string_view workflow_id) {
    // 办完就删文件夹。结束记录以后再补，补的时候要先写再删。
    std::error_code error;
    const std::filesystem::path person_path = root_ / std::string(person_id);
    std::filesystem::remove_all(person_path / std::string(workflow_id), error);
    error.clear();
    std::filesystem::remove(person_path, error);
}

} // namespace robot_pm
