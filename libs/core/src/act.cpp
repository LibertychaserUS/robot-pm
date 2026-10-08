#include "robot_pm/act.hpp"

#include <utility>

namespace robot_pm {

std::expected<std::vector<nlohmann::json>, Error> apply_effects(EffectInterpreter& interpreter,
                                                                const std::vector<Effect>& effects,
                                                                std::stop_token stop) {
    std::vector<nlohmann::json> results;
    results.reserve(effects.size());
    for (const Effect& effect : effects) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "已经取消"});
        }
        std::expected<nlohmann::json, Error> result = interpreter.apply(effect, stop);
        if (!result.has_value()) {
            return std::unexpected(result.error());
        }
        results.push_back(std::move(*result));
    }
    return results;
}

} // namespace robot_pm
