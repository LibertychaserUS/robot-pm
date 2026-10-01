#include "robot_pm/plan_expire.hpp"

#include "robot_pm/audit.hpp"

#include <fstream>
#include <sstream>

namespace robot_pm {
namespace {

constexpr std::chrono::minutes kWaitingLimit{30};

[[nodiscard]] bool exempt_from_group_timeout(const nlohmann::json& plan) {
    const bool background = plan.contains("background") && plan["background"].is_boolean() &&
                            plan["background"].get<bool>();
    const bool projection = plan.contains("step") && plan["step"].is_string() &&
                            plan["step"].get_ref<const std::string&>() == "project";
    return background || projection;
}

}  // namespace

std::expected<std::vector<CancelNotice>, Error> expire_waiting_plans(
    const std::filesystem::path& memory_root, std::chrono::system_clock::time_point now) {
    std::vector<CancelNotice> notices;
    const std::filesystem::path plans = memory_root / "working" / "plans";
    std::error_code error;
    if (!std::filesystem::is_directory(plans, error)) {
        return notices;
    }
    std::vector<std::filesystem::path> paths;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator it(plans, error); !error && it != end; it.increment(error)) {
        std::error_code file_error;
        if (it->is_regular_file(file_error) && !file_error) {
            paths.push_back(it->path());
        }
    }
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "计划目录读不出来"});
    }
    for (const std::filesystem::path& path : paths) {
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const nlohmann::json plan = nlohmann::json::parse(buffer.str(), nullptr, false);
        if (plan.is_discarded() || !plan.is_object() || exempt_from_group_timeout(plan)) {
            continue;
        }
        if (!plan.contains("progress") || !plan["progress"].is_string() ||
            plan["progress"].get_ref<const std::string&>() != "waiting" ||
            !plan.contains("waiting_since_epoch") || !plan["waiting_since_epoch"].is_number_integer()) {
            continue;
        }
        const auto since = std::chrono::system_clock::time_point{
            std::chrono::seconds{plan["waiting_since_epoch"].get<std::int64_t>()}};
        if (now - since < kWaitingLimit) {
            continue;
        }
        std::string mention = "system";
        if (plan.contains("mention_open_id") && plan["mention_open_id"].is_string() &&
            !plan["mention_open_id"].get_ref<const std::string&>().empty()) {
            mention = plan["mention_open_id"].get<std::string>();
        }
        nlohmann::json event = {{"kind", "cancelled"},
                                {"plan_id", path.stem().string()},
                                {"progress", "cancelled"}};
        const std::expected<void, Error> recorded =
            append_jsonl(memory_root / "episodic" / "events.jsonl", event);
        if (!recorded.has_value()) {
            return std::unexpected(recorded.error());
        }
        nlohmann::json audit = {{"time", format_beijing(now)},
                                {"op", "cancel"},
                                {"path", path.string()},
                                {"actor", mention},
                                {"ok", true}};
        const std::expected<void, Error> logged =
            append_jsonl(memory_root / "episodic" / "audit.jsonl", audit);
        if (!logged.has_value()) {
            return std::unexpected(logged.error());
        }
        std::filesystem::remove(path, error);
        if (error) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "计划没有回到原状"});
        }
        notices.push_back(CancelNotice{mention, "这次已取消"});
    }
    return notices;
}

}  // namespace robot_pm
