#include "robot_pm/collection_card.hpp"

#include <utility>

namespace robot_pm {

nlohmann::json build_collection_card() {
    nlohmann::json open_id_label = {{"tag", "plain_text"}, {"content", "人员"}};
    nlohmann::json role_label = {{"tag", "plain_text"}, {"content", "职责"}};
    nlohmann::json submit_text = {{"tag", "plain_text"}, {"content", "提交"}};
    nlohmann::json elements = nlohmann::json::array();
    elements.push_back(
        nlohmann::json{{"tag", "input"}, {"name", "open_id"}, {"label", std::move(open_id_label)}});
    elements.push_back(
        nlohmann::json{{"tag", "input"}, {"name", "role"}, {"label", std::move(role_label)}});
    elements.push_back(nlohmann::json{{"tag", "button"},
                                       {"name", "submit"},
                                       {"text", std::move(submit_text)},
                                       {"form_action_type", "submit"}});
    nlohmann::json form = {{"tag", "form"}, {"name", "role_form"}, {"elements", std::move(elements)}};
    nlohmann::json card = {
        {"header",
         {{"title", {{"tag", "plain_text"}, {"content", "填写职责"}}}}},
        {"elements", nlohmann::json::array({form})},
    };
    return {{"msg_type", "interactive"}, {"card", card}};
}

}  // namespace robot_pm
