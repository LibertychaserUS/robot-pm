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

void remove_person_if_empty(const std::filesystem::path& person) {
    std::error_code error;
    if (!std::filesystem::is_directory(person, error) || error) {
        return;
    }
    if (std::filesystem::directory_iterator(person, error) == std::filesystem::directory_iterator() && !error) {
        std::filesystem::remove(person, error);
    }
}

}  // namespace

std::expected<std::vector<CancelNotice>, Error> expire_waiting_plans(
    const std::filesystem::path& memory_root, std::chrono::system_clock::time_point now) {
    std::vector<CancelNotice> notices;
    const std::filesystem::path working = memory_root / "working";
    std::error_code error;
    if (!std::filesystem::is_directory(working, error) || error) {
        return notices;
    }
    struct WaitingPlan {
        std::filesystem::path directory;
        std::string open_id;
        std::string interaction_id;
        nlohmann::json plan;
    };
    std::vector<WaitingPlan> waiting;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator person(working, error); !error && person != end;
         person.increment(error)) {
        std::error_code person_error;
        if (!person->is_directory(person_error) || person_error) {
            continue;
        }
        if (person->path().filename() == "plans") {
            continue;
        }
        for (std::filesystem::directory_iterator interaction(person->path(), person_error);
             !person_error && interaction != end; interaction.increment(person_error)) {
            std::error_code directory_error;
            if (!interaction->is_directory(directory_error) || directory_error) {
                continue;
            }
            std::ifstream input(interaction->path() / "context.json");
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
            waiting.push_back(WaitingPlan{interaction->path(), person->path().filename().string(),
                                          interaction->path().filename().string(), plan});
        }
    }
    if (error) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "计划目录读不出来"});
    }
    for (const WaitingPlan& waiting_plan : waiting) {
        std::string mention = waiting_plan.open_id.empty() ? "system" : waiting_plan.open_id;
        if (waiting_plan.plan.contains("mention_open_id") && waiting_plan.plan["mention_open_id"].is_string() &&
            !waiting_plan.plan["mention_open_id"].get_ref<const std::string&>().empty()) {
            mention = waiting_plan.plan["mention_open_id"].get<std::string>();
        }
        const nlohmann::json tombstone = {{"interaction_id", waiting_plan.interaction_id},
                                          {"actor", waiting_plan.open_id},
                                          {"op", "cancel"},
                                          {"outcome", "cancelled"},
                                          {"time", format_beijing(now)}};
        const std::expected<void, Error> recorded =
            append_jsonl(memory_root / "episodic" / "events.jsonl", tombstone);
        if (!recorded.has_value()) {
            return std::unexpected(recorded.error());
        }
        nlohmann::json audit = {{"time", format_beijing(now)},
                                {"op", "cancel"},
                                {"path", waiting_plan.directory.string()},
                                {"actor", mention},
                                {"ok", true}};
        const std::expected<void, Error> logged =
            append_jsonl(memory_root / "episodic" / "audit.jsonl", audit);
        if (!logged.has_value()) {
            return std::unexpected(logged.error());
        }
        std::filesystem::remove_all(waiting_plan.directory, error);
        if (error) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "计划没有回到原状"});
        }
        remove_person_if_empty(waiting_plan.directory.parent_path());
        notices.push_back(CancelNotice{mention, "这次已取消"});
    }
    return notices;
}

}  // namespace robot_pm
