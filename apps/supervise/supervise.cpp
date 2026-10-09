#include "supervise.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <sys/syscall.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>

#include <utility>

namespace robot_pm {
namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_child = 0;

void handle_stop(int) {
    g_stop = 1;
    const sig_atomic_t child = g_child;
    if (child > 0) {
        const auto pid = static_cast<pid_t>(child);
        if (::kill(-pid, SIGTERM) != 0) {
            ::kill(pid, SIGTERM);
        }
    }
}

void block_stop_signals(sigset_t* old) {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, old);
}

[[nodiscard]] bool pause_for(std::chrono::milliseconds delay) {
    if (stop_requested()) {
        return false;
    }
    auto remaining = delay;
    while (remaining.count() > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
        const auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds);
        timespec request{};
        request.tv_sec = static_cast<std::time_t>(seconds.count());
        request.tv_nsec = static_cast<long>(nanos.count());
        timespec left{};
        if (::nanosleep(&request, &left) == 0) {
            return !stop_requested();
        }
        if (errno != EINTR) {
            return false;
        }
        if (stop_requested()) {
            return false;
        }
        remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds{left.tv_sec} + std::chrono::nanoseconds{left.tv_nsec});
    }
    return !stop_requested();
}

void note_child(int pid) {
    g_child = static_cast<sig_atomic_t>(pid);
}

void clear_child(int pid) {
    if (g_child == static_cast<sig_atomic_t>(pid)) {
        g_child = 0;
    }
}

#ifndef __NR_pidfd_open
#define ROBOT_PM_NO_PIDFD 1
#endif

[[nodiscard]] int open_pidfd(int pid) {
#ifdef ROBOT_PM_NO_PIDFD
    static_cast<void>(pid);
    return -1;
#else
    return static_cast<int>(::syscall(__NR_pidfd_open, static_cast<pid_t>(pid), 0));
#endif
}

}  // namespace

std::chrono::milliseconds restart_delay(int failures) {
    if (failures < 1) {
        failures = 1;
    }
    std::chrono::milliseconds delay = kRestartFloor;
    for (int step = 1; step < failures; ++step) {
        if (delay > kRestartCap / 2) {
            return kRestartCap;
        }
        delay *= 2;
    }
    if (delay > kRestartCap) {
        return kRestartCap;
    }
    return delay;
}

bool unexpected_exit(const ChildExit& exit) {
    if (exit.stopped || exit.clean) {
        return false;
    }
    return exit.signaled || exit.code != 0;
}

OneChild::~OneChild() {
    halt();
}

std::expected<bool, SuperviseError> OneChild::start(const std::vector<std::string>& command) {
    if (command.empty() || command.front().empty()) {
        return std::unexpected(SuperviseError{"缺少程序"});
    }
    if (alive()) {
        return false;
    }

    std::vector<std::string> owned = command;
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (std::string& word : owned) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    sigset_t previous{};
    block_stop_signals(&previous);
    const pid_t pid = ::fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        return std::unexpected(SuperviseError{"没有拉起"});
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        ::setpgid(0, 0);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    pid_ = static_cast<int>(pid);
    note_child(pid_);
    ++starts_;
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    return true;
}

bool OneChild::alive() {
    if (pid_ <= 0) {
        return false;
    }
    int status = 0;
    const pid_t got = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (got == 0 || (got < 0 && errno == EINTR)) {
        return true;
    }
    clear_child(pid_);
    pid_ = 0;
    return false;
}

int OneChild::id() const {
    return pid_;
}

int OneChild::starts() const {
    return starts_;
}

std::expected<ChildExit, SuperviseError> OneChild::wait() {
    if (pid_ <= 0) {
        return std::unexpected(SuperviseError{"没有子进程"});
    }
    int status = 0;
    for (;;) {
        // 堵在这里。不用 WNOHANG，避免空转。
        const pid_t got = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
        if (got < 0) {
            if (errno == EINTR) {
                if (stop_requested()) {
                    halt();
                    ChildExit exit;
                    exit.stopped = true;
                    return exit;
                }
                continue;
            }
            return std::unexpected(SuperviseError{"没有等到"});
        }
        break;
    }
    clear_child(pid_);
    pid_ = 0;

    ChildExit exit;
    if (stop_requested()) {
        exit.stopped = true;
    }
    if (WIFEXITED(status)) {
        exit.code = WEXITSTATUS(status);
        exit.clean = exit.code == 0;
    } else if (WIFSIGNALED(status)) {
        exit.signaled = true;
        exit.code = WTERMSIG(status);
        exit.clean = false;
    }
    return exit;
}

void OneChild::halt() {
    const int pid = pid_;
    if (pid <= 0) {
        return;
    }
    const auto child = static_cast<pid_t>(pid);
    if (::kill(-child, SIGTERM) != 0) {
        ::kill(child, SIGTERM);
    }
    const int fd = open_pidfd(pid);
    if (fd >= 0) {
        pollfd slot{};
        slot.fd = fd;
        slot.events = POLLIN;
        const int polled = ::poll(&slot, 1, 2000);
        if (polled <= 0) {
            ::kill(child, SIGKILL);
        }
        ::close(fd);
    } else {
        ::kill(child, SIGKILL);
    }
    int status = 0;
    for (;;) {
        const pid_t got = ::waitpid(child, &status, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    clear_child(pid);
    if (pid_ == pid) {
        pid_ = 0;
    }
}

std::expected<WatchResult, SuperviseError> watch(const std::vector<std::string>& command) {
    if (command.empty() || command.front().empty()) {
        return std::unexpected(SuperviseError{"缺少程序"});
    }
    OneChild child;
    WatchResult result;
    int failures = 0;
    while (!stop_requested()) {
        const std::expected<bool, SuperviseError> spawned = child.start(command);
        if (!spawned.has_value()) {
            return std::unexpected(spawned.error());
        }
        if (*spawned) {
            ++result.starts;
        }
        const auto began = std::chrono::steady_clock::now();
        const std::expected<ChildExit, SuperviseError> ended = child.wait();
        if (!ended.has_value()) {
            return std::unexpected(ended.error());
        }
        if (ended->stopped || stop_requested()) {
            if (child.alive()) {
                child.halt();
            }
            result.stopped = true;
            return result;
        }
        if (!unexpected_exit(*ended)) {
            return result;
        }
        const auto lived = std::chrono::steady_clock::now() - began;
        if (lived >= kRestartFloor) {
            failures = 0;
        }
        ++failures;
        if (!pause_for(restart_delay(failures))) {
            result.stopped = true;
            return result;
        }
    }
    if (child.alive()) {
        child.halt();
    }
    result.stopped = true;
    return result;
}

void arm_stop_signals() {
    struct sigaction action {};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

bool stop_requested() {
    return g_stop != 0;
}

void clear_stop() {
    g_stop = 0;
}

}  // namespace robot_pm
