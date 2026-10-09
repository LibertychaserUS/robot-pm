#pragma once

// 这个文件负责让进程一直处理飞书事件。
// 不变量：缺配置就退出；没有 @ 的群消息不进库；群文件不导入。
// 每张卡片只有一个决定。标题 2 到 4 个字，动作单独一行且不超过 20 个字。
// 要办事的人另起一行，写名字。名字不进标题，也不进动作那一行。没有名字就用已有的称呼，不写 open id。
// 提出人的卡片只给提出人点。群里的会只给职责对上的人点。对上的人一个都没有，就谁也不能点。

#include "robot_pm/app.hpp"
#include "robot_pm/feishu_access.hpp"

#include <map>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace robot_pm {

class EventPort {
public:
    virtual ~EventPort() = default;
    EventPort() = default;
    EventPort(const EventPort&) = delete;
    EventPort& operator=(const EventPort&) = delete;

    // 前置条件：停止被请求时必须返回，不能一直等。
    // 失败：不失败。没有下一条时返回空。accept 失败时 failed() 为真。
    [[nodiscard]] virtual std::optional<FeishuRequest> take(std::stop_token stop) = 0;

    // 前置条件：body 不含凭证。回答上一条 take。
    virtual void reply(std::string_view body) = 0;

    // 前置条件：take 已经返回空。
    // 失败：不失败。accept 或 poll 出错时为真，正常停下时为假。
    [[nodiscard]] virtual bool failed() const { return false; }
};

class ServiceObserver {
public:
    virtual ~ServiceObserver() = default;
    virtual void before_library(const nlohmann::json& event) { static_cast<void>(event); }
};

struct ServicePaths {
    std::filesystem::path data_root;
    std::filesystem::path repo_root;
};

struct ServeDeps {
    Clock& clock;
    EventPort& events;
    FeishuPort& feishu;
    Model& model;
    BitablePort& bitable;
    ProcessRunner& process;
    ServiceObserver* observer = nullptr;
};

struct CardDispatch {
    std::vector<nlohmann::json> requests;
    std::string notice;
};

// 前置条件：recorded_form 是这个群已经录入的形式。空字符串表示没有这一行。
// 实际形式在 message 的 chat_mode 里。对不上，或没有录入，requests 为空，不换另一种送法。
// 失败：不失败。notice 只有「形式没记」或「形式不符」。
[[nodiscard]] CardDispatch present_feishu_cards(const nlohmann::json& message,
                                               std::string_view recorded_form,
                                               std::string_view base_url = "https://open.feishu.cn");

// 前置条件：form 只能是已经有的形式。group_id 不含路径分隔符。
// 失败：kEditRejected，形式或标识不对，不改已有的行。
[[nodiscard]] std::expected<void, Error> enter_group_form(const std::filesystem::path& data_root,
                                                         std::string_view group_id,
                                                         std::string_view form);

// 前置条件：data_root 是这次运行的数据目录。
// 失败：不失败。没有这一行时返回空。
[[nodiscard]] std::optional<std::string> recorded_group_form(const std::filesystem::path& data_root,
                                                            std::string_view group_id);

// 前置条件：env 是进程环境。空着或只有空白算缺。
// 失败：不失败。缺了就写到 out，一行一个「中文名 变量名」，并返回 1。齐了返回 0。
[[nodiscard]] int missing_runtime_settings(const std::map<std::string, std::string>& env, std::ostream& out);

// 前置条件：deps 里的对象活得比这次调用久。
// 失败：缺配置时返回 1，并把缺的名字写到 out，一行一个。停下来返回 0。accept 失败返回 1。
[[nodiscard]] int serve(const std::map<std::string, std::string>& env,
                        const ServeDeps& deps,
                        ServicePaths paths,
                        std::stop_token stop,
                        std::ostream& out);

}  // namespace robot_pm
