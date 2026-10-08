#include "robot_pm/act.hpp"
#include "robot_pm/arrange.hpp"
#include "robot_pm/decide.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::filesystem::path make_root(const char* name) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::chrono::system_clock::time_point beijing_ten() {
    return std::chrono::sys_days{std::chrono::year{2026} / std::chrono::October / 1} +
           std::chrono::hours{2};
}

int leaf_dirs(const std::filesystem::path& root) {
    int count = 0;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return 0;
    }
    for (const std::filesystem::directory_entry& person :
         std::filesystem::directory_iterator(root, error)) {
        if (error || !person.is_directory()) {
            continue;
        }
        std::error_code inner;
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(person.path(), inner)) {
            if (!inner && item.is_directory()) {
                ++count;
            }
        }
    }
    return count;
}

robot_pm::StepSpec act_step(std::string name, robot_pm::EffectKind kind, std::string mark) {
    robot_pm::StepSpec step;
    step.name = std::move(name);
    step.effect = kind;
    step.payload = {{"mark", std::move(mark)}};
    return step;
}

robot_pm::WorkflowState two_effects() {
    robot_pm::WorkflowState state;
    state.id = "w";
    state.person_id = "p";
    state.steps = {act_step("one", robot_pm::EffectKind::kModelCall, "one"),
                   act_step("two", robot_pm::EffectKind::kFeishuPost, "two")};
    return state;
}

robot_pm::Event run_current(const robot_pm::WorkflowState& state) {
    robot_pm::Event event;
    event.kind = robot_pm::EventKind::kRunStep;
    event.person_id = state.person_id;
    event.workflow_id = state.id;
    event.step = state.steps.at(state.index).name;
    event.time = "2026-10-01 10:00";
    return event;
}

class FakeClock final : public robot_pm::StepClock {
  public:
    explicit FakeClock(std::chrono::system_clock::time_point start) { set(start); }

    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::time_point{
            std::chrono::nanoseconds{nanos_.load(std::memory_order_relaxed)}};
    }

    void set(std::chrono::system_clock::time_point time) {
        nanos_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(),
            std::memory_order_relaxed);
    }

  private:
    std::atomic<std::chrono::nanoseconds::rep> nanos_{0};
};

class Script final : public robot_pm::EffectInterpreter {
  public:
    std::expected<nlohmann::json, robot_pm::Error> apply(const robot_pm::Effect& effect,
                                                         std::stop_token) override {
        calls.push_back(effect.step);
        if (effect.payload.value("fail", false)) {
            return std::unexpected(
                robot_pm::Error{robot_pm::ErrorCode::kBridgeFailed, "这一步没做成"});
        }
        return nlohmann::json::object();
    }

    std::vector<std::string> calls;
};

class Gate final : public robot_pm::EffectInterpreter {
  public:
    std::expected<nlohmann::json, robot_pm::Error> apply(const robot_pm::Effect& effect,
                                                         std::stop_token stop) override {
        const std::string mark = effect.payload.value("mark", "");
        {
            std::lock_guard lock(mu_);
            entered_.push_back(mark);
            threads_.insert(std::this_thread::get_id());
            ++inside_;
            if (inside_ > max_inside_) {
                max_inside_ = inside_;
            }
        }
        cv_.notify_all();
        {
            std::unique_lock lock(mu_);
            const std::stop_callback callback(stop, [&]() noexcept { cv_.notify_all(); });
            cv_.wait(lock, [&] { return released_ || stop.stop_requested(); });
            --inside_;
        }
        cv_.notify_all();
        if (stop.stop_requested()) {
            return std::unexpected(robot_pm::Error{robot_pm::ErrorCode::kEditRejected, "已经取消"});
        }
        std::lock_guard lock(mu_);
        applied_.push_back(mark);
        return nlohmann::json::object();
    }

    bool wait_inside(int count) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds{5}, [&] { return inside_ >= count; });
    }

    void release() {
        std::lock_guard lock(mu_);
        released_ = true;
        cv_.notify_all();
    }

    [[nodiscard]] int max_inside() const {
        std::lock_guard lock(mu_);
        return max_inside_;
    }

    [[nodiscard]] std::vector<std::string> entered() const {
        std::lock_guard lock(mu_);
        return entered_;
    }

    [[nodiscard]] std::vector<std::string> applied() const {
        std::lock_guard lock(mu_);
        return applied_;
    }

    [[nodiscard]] std::size_t thread_count() const {
        std::lock_guard lock(mu_);
        return threads_.size();
    }

  private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> entered_;
    std::vector<std::string> applied_;
    std::set<std::thread::id> threads_;
    int inside_ = 0;
    int max_inside_ = 0;
    bool released_ = false;
};

const robot_pm::WorkflowState* find_state(const robot_pm::ArrangeView& view,
                                          const std::string& id) {
    const auto found = view.states.find(id);
    if (found == view.states.end()) {
        return nullptr;
    }
    return &found->second;
}

} // namespace

TEST_CASE("decide.one_step_emits_one_effect") {
    const robot_pm::WorkflowState state = two_effects();

    const robot_pm::Decision decision = robot_pm::decide(state, run_current(state));

    CHECK(decision.effects.size() == 1);
    CHECK(decision.effects[0].kind == robot_pm::EffectKind::kModelCall);
    CHECK(decision.effects[0].step == "one");
    CHECK(decision.state.phase == robot_pm::Phase::kAwaiting);
    CHECK(decision.state.index == 0);
    CHECK(decision.state.finished_steps.empty());
    CHECK(decision.state.started_at == "2026-10-01 10:00");
}

TEST_CASE("decide.ok_does_not_start_the_next_step") {
    robot_pm::WorkflowState state = two_effects();
    state = robot_pm::decide(state, run_current(state)).state;
    robot_pm::Event ok;
    ok.kind = robot_pm::EventKind::kEffectOk;
    ok.person_id = state.person_id;
    ok.workflow_id = state.id;
    ok.step = "one";

    const robot_pm::Decision decision = robot_pm::decide(state, ok);

    CHECK(decision.effects.empty());
    CHECK(decision.state.phase == robot_pm::Phase::kReady);
    CHECK(decision.state.index == 1);
    CHECK(decision.state.finished_steps == std::vector<std::string>{"one"});
    CHECK_FALSE(decision.state.inflight.has_value());
}

TEST_CASE("decide.fail_emits_no_later_effect") {
    robot_pm::WorkflowState state = two_effects();
    state = robot_pm::decide(state, run_current(state)).state;
    robot_pm::Event fail;
    fail.kind = robot_pm::EventKind::kEffectFail;
    fail.person_id = state.person_id;
    fail.workflow_id = state.id;
    fail.step = "one";

    const robot_pm::Decision decision = robot_pm::decide(state, fail);

    CHECK(decision.effects.empty());
    CHECK(decision.state.phase == robot_pm::Phase::kFailed);
    CHECK(decision.state.finished_steps.empty());
    const robot_pm::Decision later = robot_pm::decide(decision.state, run_current(state));
    CHECK(later.effects.empty());
    CHECK(later.state.phase == robot_pm::Phase::kFailed);
}

TEST_CASE("decide.plain_step_emits_nothing") {
    robot_pm::WorkflowState state;
    state.id = "w";
    state.person_id = "p";
    robot_pm::StepSpec step;
    step.name = "note";
    state.steps = {step};

    const robot_pm::Decision decision = robot_pm::decide(state, run_current(state));

    CHECK(decision.effects.empty());
    CHECK(decision.state.phase == robot_pm::Phase::kDone);
    CHECK(decision.state.finished_steps == std::vector<std::string>{"note"});
}

TEST_CASE("decide.other_person_is_unchanged") {
    const robot_pm::WorkflowState state = two_effects();
    robot_pm::Event event = run_current(state);
    event.person_id = "other";

    const robot_pm::Decision decision = robot_pm::decide(state, event);

    CHECK(decision.effects.empty());
    CHECK(decision.state.phase == robot_pm::Phase::kIdle);
    CHECK(decision.state.index == 0);
}

TEST_CASE("act.later_effect_is_not_applied") {
    Script script;
    const std::vector<robot_pm::Effect> effects = {
        robot_pm::Effect{robot_pm::EffectKind::kModelCall, "one", {{"fail", true}}},
        robot_pm::Effect{robot_pm::EffectKind::kCalendar, "two", {{"mark", "two"}}},
    };

    const auto applied = robot_pm::apply_effects(script, effects, std::stop_token{});

    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().code == robot_pm::ErrorCode::kBridgeFailed);
    CHECK(script.calls == std::vector<std::string>{"one"});
}

TEST_CASE("arrange.two_people_overlap") {
    const auto root = make_root("robot-pm-arrange-overlap");
    Gate gate;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{gate, clock, root};

    robot_pm::WorkflowRequest first;
    first.person_id = "pa";
    first.workflow_id = "wa";
    first.steps = {act_step("ask", robot_pm::EffectKind::kModelCall, "pa")};
    robot_pm::WorkflowRequest second = first;
    second.person_id = "pb";
    second.workflow_id = "wb";
    second.steps = {act_step("ask", robot_pm::EffectKind::kBitableWrite, "pb")};
    REQUIRE(arrange.submit(first).has_value());
    REQUIRE(arrange.submit(second).has_value());
    REQUIRE(gate.wait_inside(2));

    CHECK(gate.thread_count() == 2);
    CHECK(gate.max_inside() == 2);
    CHECK(leaf_dirs(root) == 2);
    const robot_pm::ArrangeView mid = arrange.view();
    CHECK(mid.live == 2);
    gate.release();

    const bool both_done = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* left = find_state(view, "wa");
            const robot_pm::WorkflowState* right = find_state(view, "wb");
            return view.live == 0 && left != nullptr && right != nullptr &&
                   left->phase == robot_pm::Phase::kDone && right->phase == robot_pm::Phase::kDone;
        },
        std::chrono::seconds{5});
    REQUIRE(both_done);
    const std::vector<std::string> applied = gate.applied();
    CHECK(applied.size() == 2);
    CHECK((applied[0] == "pa" || applied[1] == "pa"));
    CHECK((applied[0] == "pb" || applied[1] == "pb"));
}

TEST_CASE("arrange.same_person_stays_ordered") {
    const auto root = make_root("robot-pm-arrange-order");
    Gate gate;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{gate, clock, root};

    robot_pm::WorkflowRequest first;
    first.person_id = "pa";
    first.workflow_id = "a1";
    first.steps = {act_step("ask", robot_pm::EffectKind::kModelCall, "a1")};
    robot_pm::WorkflowRequest second = first;
    second.workflow_id = "a2";
    second.steps = {act_step("send", robot_pm::EffectKind::kFeishuPost, "a2")};
    REQUIRE(arrange.submit(first).has_value());
    REQUIRE(gate.wait_inside(1));
    REQUIRE(arrange.submit(second).has_value());

    const robot_pm::ArrangeView mid = arrange.view();
    CHECK(mid.live == 1);
    const auto queued = mid.queued.find("pa");
    REQUIRE(queued != mid.queued.end());
    CHECK(queued->second == 1);
    CHECK(gate.max_inside() == 1);
    CHECK(std::filesystem::is_directory(root / "pa" / "a1"));
    CHECK_FALSE(std::filesystem::exists(root / "pa" / "a2"));
    gate.release();

    const bool ordered_done = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* early = find_state(view, "a1");
            const robot_pm::WorkflowState* late = find_state(view, "a2");
            return view.live == 0 && early != nullptr && late != nullptr &&
                   early->phase == robot_pm::Phase::kDone && late->phase == robot_pm::Phase::kDone;
        },
        std::chrono::seconds{5});
    REQUIRE(ordered_done);
    CHECK(gate.entered() == std::vector<std::string>{"a1", "a2"});
    CHECK(gate.max_inside() == 1);
}

TEST_CASE("arrange.sixth_person_is_rejected") {
    const auto root = make_root("robot-pm-arrange-sixth");
    Gate gate;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{gate, clock, root};

    for (int index = 1; index <= robot_pm::kMaxInFlight; ++index) {
        robot_pm::WorkflowRequest request;
        request.person_id = "p" + std::to_string(index);
        request.workflow_id = "w" + std::to_string(index);
        request.steps = {act_step("ask", robot_pm::EffectKind::kModelCall, request.person_id)};
        REQUIRE(arrange.submit(request).has_value());
    }
    REQUIRE(gate.wait_inside(robot_pm::kMaxInFlight));
    CHECK(leaf_dirs(root) == robot_pm::kMaxInFlight);

    robot_pm::WorkflowRequest sixth;
    sixth.person_id = "p6";
    sixth.workflow_id = "w6";
    sixth.steps = {act_step("ask", robot_pm::EffectKind::kCalendar, "p6")};
    const auto rejected = arrange.submit(sixth);

    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == robot_pm::ErrorCode::kForbidden);
    CHECK(rejected.error().message == "人满了，请稍后再试");
    CHECK(leaf_dirs(root) == robot_pm::kMaxInFlight);
    CHECK_FALSE(std::filesystem::exists(root / "p6"));
    CHECK(arrange.view().live == robot_pm::kMaxInFlight);
    gate.release();
    const bool drained =
        arrange.wait_until([](const robot_pm::ArrangeView& view) { return view.live == 0; }, std::chrono::seconds{5});
    REQUIRE(drained);
}

TEST_CASE("arrange.cancel_stops_only_that_workflow") {
    const auto root = make_root("robot-pm-arrange-cancel");
    Gate gate;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{gate, clock, root};

    robot_pm::WorkflowRequest left;
    left.person_id = "ca";
    left.workflow_id = "caw";
    left.steps = {act_step("one", robot_pm::EffectKind::kModelCall, "ca1"),
                  act_step("two", robot_pm::EffectKind::kFeishuPost, "ca2")};
    robot_pm::WorkflowRequest right;
    right.person_id = "cb";
    right.workflow_id = "cbw";
    right.steps = {act_step("one", robot_pm::EffectKind::kCalendar, "cb1")};
    REQUIRE(arrange.submit(left).has_value());
    REQUIRE(arrange.submit(right).has_value());
    REQUIRE(gate.wait_inside(2));

    arrange.cancel("caw");

    const bool only_other_left = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* cancelled = find_state(view, "caw");
            return cancelled != nullptr && cancelled->phase == robot_pm::Phase::kCancelled &&
                   !view.person_live.contains("ca") && view.person_live.contains("cb") &&
                   view.live == 1;
        },
        std::chrono::seconds{5});
    REQUIRE(only_other_left);
    CHECK(std::filesystem::is_directory(root / "cb" / "cbw"));
    CHECK_FALSE(std::filesystem::exists(root / "ca" / "caw"));
    const std::vector<std::string> entered = gate.entered();
    CHECK(entered.size() == 2);
    CHECK_FALSE(std::ranges::contains(entered, std::string{"ca2"}));
    gate.release();

    const bool other_finished = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* kept = find_state(view, "cbw");
            const robot_pm::WorkflowState* cancelled = find_state(view, "caw");
            return view.live == 0 && kept != nullptr && cancelled != nullptr &&
                   kept->phase == robot_pm::Phase::kDone &&
                   cancelled->phase == robot_pm::Phase::kCancelled;
        },
        std::chrono::seconds{5});
    REQUIRE(other_finished);
    CHECK(gate.applied() == std::vector<std::string>{"cb1"});
    CHECK_FALSE(std::ranges::contains(gate.entered(), std::string{"ca2"}));
}

TEST_CASE("arrange.failed_effect_does_not_run_the_next") {
    const auto root = make_root("robot-pm-arrange-fail");
    Script script;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{script, clock, root};
    robot_pm::WorkflowRequest request;
    request.person_id = "pf";
    request.workflow_id = "wf";
    robot_pm::StepSpec fail = act_step("one", robot_pm::EffectKind::kModelCall, "one");
    fail.payload["fail"] = true;
    request.steps = {fail, act_step("two", robot_pm::EffectKind::kBitableWrite, "two")};

    REQUIRE(arrange.submit(request).has_value());

    const bool failed = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* state = find_state(view, "wf");
            return view.live == 0 && state != nullptr && state->phase == robot_pm::Phase::kFailed;
        },
        std::chrono::seconds{5});
    REQUIRE(failed);
    CHECK(script.calls == std::vector<std::string>{"one"});
    const robot_pm::ArrangeView done = arrange.view();
    const robot_pm::WorkflowState* state = find_state(done, "wf");
    REQUIRE(state != nullptr);
    CHECK(state->finished_steps.empty());
}

TEST_CASE("arrange.stamps_beijing_time") {
    const auto root = make_root("robot-pm-arrange-time");
    Script script;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{script, clock, root};
    robot_pm::WorkflowRequest request;
    request.person_id = "pt";
    request.workflow_id = "wt";
    robot_pm::StepSpec note;
    note.name = "note";
    request.steps = {note};

    REQUIRE(arrange.submit(request).has_value());

    const bool stamped = arrange.wait_until(
        [](const robot_pm::ArrangeView& view) {
            const robot_pm::WorkflowState* state = find_state(view, "wt");
            return view.live == 0 && state != nullptr && state->phase == robot_pm::Phase::kDone;
        },
        std::chrono::seconds{5});
    REQUIRE(stamped);
    const robot_pm::ArrangeView done = arrange.view();
    const robot_pm::WorkflowState* state = find_state(done, "wt");
    REQUIRE(state != nullptr);
    CHECK(state->started_at == "2026-10-01 10:00");
    CHECK(script.calls.empty());
}

TEST_CASE("arrange.bad_name_makes_no_folder") {
    const auto root = make_root("robot-pm-arrange-name");
    Script script;
    FakeClock clock{beijing_ten()};
    robot_pm::Arrange arrange{script, clock, root};
    robot_pm::WorkflowRequest request;
    request.person_id = "../evil";
    request.workflow_id = "w";
    request.steps = {act_step("ask", robot_pm::EffectKind::kModelCall, "x")};

    const auto rejected = arrange.submit(request);

    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == robot_pm::ErrorCode::kEditRejected);
    CHECK(rejected.error().message == "名字不对");
    CHECK(leaf_dirs(root) == 0);
    CHECK(arrange.view().live == 0);
}
