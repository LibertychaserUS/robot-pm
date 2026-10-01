#include "robot_pm/project.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace robot_pm {
namespace {

[[nodiscard]] bool is_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

[[nodiscard]] bool keys_allowed(const nlohmann::json& object, const std::set<std::string>& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_cycle(const std::map<std::string, std::vector<std::string>>& predecessors) {
    enum class Color { kWhite, kGray, kBlack };
    std::map<std::string, Color> color;
    for (const auto& entry : predecessors) {
        color[entry.first] = Color::kWhite;
    }
    const auto visit = [&](auto&& self, const std::string& id) -> bool {
        color[id] = Color::kGray;
        const auto found = predecessors.find(id);
        if (found != predecessors.end()) {
            for (const std::string& next : found->second) {
                const auto next_color = color.find(next);
                if (next_color == color.end() || next_color->second == Color::kGray) {
                    return true;
                }
                if (next_color->second == Color::kWhite && self(self, next)) {
                    return true;
                }
            }
        }
        color[id] = Color::kBlack;
        return false;
    };
    for (const auto& entry : predecessors) {
        if (color[entry.first] == Color::kWhite && visit(visit, entry.first)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::expected<nlohmann::json, Error> project_item(const nlohmann::json& item,
                                                               std::string_view parent_id,
                                                               std::string_view source_text,
                                                               std::set<std::string>& ids,
                                                               std::map<std::string, std::vector<std::string>>&
                                                                   predecessors) {
    static const std::set<std::string> kAllowed{"id",          "title",     "kind",         "status",
                                                "start",       "end",       "predecessors", "owner_role",
                                                "meet",        "source_quote"};
    if (!item.is_object() || !keys_allowed(item, kAllowed) || !item.contains("id") || !item["id"].is_string() ||
        item["id"].get_ref<const std::string&>().empty() || !item.contains("title") || !item["title"].is_string() ||
        item["title"].get_ref<const std::string&>().empty() || !item.contains("kind") || !item["kind"].is_string() ||
        !item.contains("status") || !item["status"].is_string() || !item.contains("start") ||
        !item["start"].is_string() || !item.contains("end") || !item["end"].is_string() ||
        !item.contains("predecessors") || !item["predecessors"].is_array() || !item.contains("owner_role") ||
        !item["owner_role"].is_string() || item["owner_role"].get_ref<const std::string&>().empty()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "工作项字段不在规格里"});
    }
    const std::string& id = item["id"].get_ref<const std::string&>();
    const std::string& kind = item["kind"].get_ref<const std::string&>();
    const std::string& status = item["status"].get_ref<const std::string&>();
    const std::string& start = item["start"].get_ref<const std::string&>();
    const std::string& end = item["end"].get_ref<const std::string&>();
    if ((kind != "milestone" && kind != "work") ||
        (status != "todo" && status != "doing" && status != "done" && status != "blocked") || !is_date(start) ||
        !is_date(end) || end < start) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "工作项的类型、状态或日期不在规格里"});
    }
    if (item.contains("meet")) {
        if (!item["meet"].is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "meet 不在规格里"});
        }
        const std::string& meet = item["meet"].get_ref<const std::string&>();
        if (meet != "at_start" && meet != "at_end" && meet != "when_blocked") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "meet 不在规格里"});
        }
    }
    if (!source_text.empty()) {
        if (!item.contains("source_quote") || !item["source_quote"].is_string()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "source_quote 不是原文的连续子串"});
        }
        const std::string& quote = item["source_quote"].get_ref<const std::string&>();
        if (quote.empty() || source_text.find(quote) == std::string_view::npos) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "source_quote 不是原文的连续子串"});
        }
    }
    if (!ids.insert(id).second) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "清单里的 id 重复"});
    }
    std::vector<std::string> previous;
    for (const nlohmann::json& predecessor : item["predecessors"]) {
        if (!predecessor.is_string() || predecessor.get_ref<const std::string&>().empty()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "前置不在这份文档里"});
        }
        const std::string& previous_id = predecessor.get_ref<const std::string&>();
        if (previous_id == id) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "工作项不能依赖自己"});
        }
        previous.push_back(previous_id);
    }
    predecessors[id] = std::move(previous);
    nlohmann::json row = {{"业务id", id},
                          {"标题", item["title"]},
                          {"层级", "item"},
                          {"父记录", std::string(parent_id)},
                          {"类型", kind},
                          {"开始", start},
                          {"结束", end},
                          {"职责", item["owner_role"]},
                          {"前置", item["predecessors"]}};
    return row;
}

}  // namespace

std::expected<std::vector<nlohmann::json>, Error> project_manifest(const nlohmann::json& manifest,
                                                                   std::string_view source_text) {
    static const std::set<std::string> kDocumentKeys{"id", "type", "title", "sections", "items"};
    static const std::set<std::string> kSectionKeys{"id", "title", "items"};
    if (!manifest.is_object() || manifest.size() != 2 || !manifest.contains("schema_version") ||
        !manifest.contains("documents") || !manifest["schema_version"].is_number_integer() ||
        manifest["schema_version"].get<std::int64_t>() != 1 || !manifest["documents"].is_array()) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "清单顶层只能有 schema_version 和 documents"});
    }
    std::vector<nlohmann::json> rows;
    std::set<std::string> ids;
    for (const nlohmann::json& document : manifest["documents"]) {
        if (!document.is_object() || !keys_allowed(document, kDocumentKeys) || !document.contains("id") ||
            !document["id"].is_string() || document["id"].get_ref<const std::string&>().empty() ||
            !document.contains("type") || !document["type"].is_string() || !document.contains("title") ||
            !document["title"].is_string() || document["title"].get_ref<const std::string&>().empty()) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "文档字段不在规格里"});
        }
        if (document["type"].get_ref<const std::string&>() != "prd") {
            return std::unexpected(Error{ErrorCode::kEditRejected, "文档类型目前只有 prd"});
        }
        const std::string& document_id = document["id"].get_ref<const std::string&>();
        if (!ids.insert(document_id).second) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "清单里的 id 重复"});
        }
        rows.push_back(nlohmann::json{{"业务id", document_id}, {"标题", document["title"]}, {"层级", "document"}});
        std::map<std::string, std::vector<std::string>> predecessors;
        std::set<std::string> item_ids;
        const auto consume_items = [&](const nlohmann::json& items,
                                       std::string_view parent_id) -> std::expected<void, Error> {
            if (!items.is_array()) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "工作项字段不在规格里"});
            }
            for (const nlohmann::json& item : items) {
                std::expected<nlohmann::json, Error> row =
                    project_item(item, parent_id, source_text, ids, predecessors);
                if (!row.has_value()) {
                    return std::unexpected(row.error());
                }
                item_ids.insert(row->at("业务id").get_ref<const std::string&>());
                rows.push_back(std::move(*row));
            }
            return {};
        };
        if (document.contains("sections")) {
            if (!document["sections"].is_array()) {
                return std::unexpected(Error{ErrorCode::kEditRejected, "文档字段不在规格里"});
            }
            for (const nlohmann::json& section : document["sections"]) {
                if (!section.is_object() || !keys_allowed(section, kSectionKeys) || !section.contains("id") ||
                    !section["id"].is_string() || section["id"].get_ref<const std::string&>().empty() ||
                    !section.contains("title") || !section["title"].is_string() ||
                    section["title"].get_ref<const std::string&>().empty() || !section.contains("items")) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "节的字段不在规格里"});
                }
                const std::string& section_id = section["id"].get_ref<const std::string&>();
                if (!ids.insert(section_id).second) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "清单里的 id 重复"});
                }
                rows.push_back(nlohmann::json{{"业务id", section_id},
                                              {"标题", section["title"]},
                                              {"层级", "section"},
                                              {"父记录", document_id}});
                const std::expected<void, Error> consumed = consume_items(section["items"], section_id);
                if (!consumed.has_value()) {
                    return std::unexpected(consumed.error());
                }
            }
        }
        if (document.contains("items")) {
            const std::expected<void, Error> consumed = consume_items(document["items"], document_id);
            if (!consumed.has_value()) {
                return std::unexpected(consumed.error());
            }
        }
        for (const auto& entry : predecessors) {
            for (const std::string& previous : entry.second) {
                if (!item_ids.contains(previous)) {
                    return std::unexpected(Error{ErrorCode::kEditRejected, "前置不在这份文档里"});
                }
            }
        }
        if (has_cycle(predecessors)) {
            return std::unexpected(Error{ErrorCode::kEditRejected, "前置形成了环"});
        }
    }
    return rows;
}

}  // namespace robot_pm
