#pragma once

#include "robot_pm/service.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace robot_pm {

// 进程要停时调用。信号处理器里只能调用这个。
void note_process_stop() noexcept;

// 飞书把事件 POST 到这个端口。测试用假的 EventPort，不走这里。
class HttpEventPort final : public EventPort {
public:
    explicit HttpEventPort(std::uint16_t port);
    ~HttpEventPort() override;
    HttpEventPort(const HttpEventPort&) = delete;
    HttpEventPort& operator=(const HttpEventPort&) = delete;

    [[nodiscard]] bool ok() const;
    [[nodiscard]] std::string_view failure() const;
    [[nodiscard]] std::uint16_t bound_port() const;

    std::optional<FeishuRequest> take(std::stop_token stop) override;
    void reply(std::string_view body) override;
    [[nodiscard]] bool failed() const override;

private:
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::uint16_t bound_port_ = 0;
    bool failed_ = false;
    std::string failure_;
};

}  // namespace robot_pm
