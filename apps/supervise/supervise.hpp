#pragma once

// 这个文件负责在用户态看管一个子进程。不进内核。
// 不变量：同时只拉起一个；父进程堵在 wait，不空转。
// 崩溃或非零退出才再拉起：先等至少一秒，连续马上死掉就把等待加倍，一秒里不会拉起多次。
// 收到停止信号，或子进程自己正常结束，就关掉，不再拉起。
// 不占数据目录的锁，不记第二份日志，不自己发回执。锁和日志都在子进程里。

#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace robot_pm {

inline constexpr std::chrono::milliseconds kRestartFloor{1000};
inline constexpr std::chrono::milliseconds kRestartCap{60000};

struct SuperviseError {
    const char* message;
};

struct ChildExit {
    bool clean{false};
    bool signaled{false};
    bool stopped{false};
    int code{0};
};

struct WatchResult {
    int starts{0};
    bool stopped{false};
};

// 前置条件：failures 从 1 起算。第一次至少一秒，之后加倍，到顶为止。
[[nodiscard]] std::chrono::milliseconds restart_delay(int failures);

// 崩溃（被信号打死）或退出码不是 0，且不是人家要求停下。
[[nodiscard]] bool unexpected_exit(const ChildExit& exit);

class OneChild {
  public:
    OneChild() = default;
    ~OneChild();
    OneChild(const OneChild&) = delete;
    OneChild& operator=(const OneChild&) = delete;

    // 已经有一个活着的子进程时不再拉起，返回 false。
    // 失败：缺少程序，或内核没有拉起。
    [[nodiscard]] std::expected<bool, SuperviseError> start(
        const std::vector<std::string>& command);

    [[nodiscard]] bool alive();
    [[nodiscard]] int id() const;
    [[nodiscard]] int starts() const;

    // 堵在 wait，直到子进程结束或收到停止信号。
    [[nodiscard]] std::expected<ChildExit, SuperviseError> wait();

    // 让子进程停下来并等它结束。不会再拉起一个。
    void halt();

  private:
    int pid_{0};
    int starts_{0};
};

// 前置条件：command 的第一项是要拉起的程序。
// 失败：程序拉不起来。正常结束或被要求停下都算成功，不发回执。
[[nodiscard]] std::expected<WatchResult, SuperviseError> watch(
    const std::vector<std::string>& command);

void arm_stop_signals();
[[nodiscard]] bool stop_requested();
void clear_stop();

}  // namespace robot_pm
