#include "robot_pm/decide.hpp"

#include <utility>

namespace robot_pm {
namespace {

[[nodiscard]] bool same_workflow(const WorkflowState& state, const Event& event) {
    return event.person_id == state.person_id && event.workflow_id == state.id;
}

[[nodiscard]] bool terminal(Phase phase) {
    return phase == Phase::kDone || phase == Phase::kCancelled || phase == Phase::kFailed;
}

[[nodiscard]] Decision unchanged(WorkflowState state) {
    Decision decision;
    decision.state = std::move(state);
    return decision;
}

[[nodiscard]] Decision run_step(WorkflowState state, const Event& event) {
    Decision decision;
    if (state.phase != Phase::kIdle && state.phase != Phase::kReady) {
        decision.state = std::move(state);
        return decision;
    }
    if (state.started_at.empty()) {
        state.started_at = event.time;
    }
    if (state.index >= state.steps.size()) {
        state.phase = Phase::kDone;
        decision.state = std::move(state);
        return decision;
    }
    const StepSpec& step = state.steps[state.index];
    if (event.step != step.name) {
        decision.state = std::move(state);
        return decision;
    }
    if (!step.effect.has_value()) {
        state.finished_steps.push_back(step.name);
        ++state.index;
        state.phase = state.index >= state.steps.size() ? Phase::kDone : Phase::kReady;
        decision.state = std::move(state);
        return decision;
    }

    Effect effect;
    effect.kind = *step.effect;
    effect.step = step.name;
    effect.payload = step.payload;
    state.inflight = effect;
    state.phase = Phase::kAwaiting;
    decision.effects.push_back(std::move(effect));
    decision.state = std::move(state);
    return decision;
}

[[nodiscard]] Decision effect_ok(WorkflowState state, const Event& event) {
    Decision decision;
    if (state.phase != Phase::kAwaiting || !state.inflight.has_value() ||
        event.step != state.inflight->step) {
        decision.state = std::move(state);
        return decision;
    }
    state.finished_steps.push_back(state.inflight->step);
    state.inflight.reset();
    ++state.index;
    state.phase = state.index >= state.steps.size() ? Phase::kDone : Phase::kReady;
    decision.state = std::move(state);
    return decision;
}

[[nodiscard]] Decision effect_fail(WorkflowState state, const Event& event) {
    Decision decision;
    if (state.phase != Phase::kAwaiting || !state.inflight.has_value() ||
        event.step != state.inflight->step) {
        decision.state = std::move(state);
        return decision;
    }
    state.inflight.reset();
    state.phase = Phase::kFailed;
    decision.state = std::move(state);
    return decision;
}

[[nodiscard]] Decision cancel(WorkflowState state) {
    Decision decision;
    state.inflight.reset();
    state.phase = Phase::kCancelled;
    decision.state = std::move(state);
    return decision;
}

} // namespace

Decision decide(const WorkflowState& state, const Event& event) {
    if (!same_workflow(state, event) || terminal(state.phase)) {
        return unchanged(state);
    }
    switch (event.kind) {
    case EventKind::kRunStep:
        return run_step(state, event);
    case EventKind::kEffectOk:
        return effect_ok(state, event);
    case EventKind::kEffectFail:
        return effect_fail(state, event);
    case EventKind::kCancel:
        return cancel(state);
    }
    return unchanged(state);
}

} // namespace robot_pm
