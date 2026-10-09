#include "robot_pm/child_boot.hpp"
#include "robot_pm/working_memory.hpp"
#include "supervise.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::filesystem::path make_root(const char* name) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int line_count(std::string_view text) {
    int count = 0;
    for (const char character : text) {
        if (character == '\n') {
            ++count;
        }
    }
    return count;
}

int chinese_count(std::string_view text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = 0;
        std::size_t length = 1;
        if ((lead & 0x80U) == 0) {
            code = lead;
        } else if ((lead & 0xE0U) == 0xC0U && index + 1 < text.size()) {
            length = 2;
            code = lead & 0x1FU;
            code = (code << 6U) | (static_cast<unsigned char>(text[index + 1]) & 0x3FU);
        } else if ((lead & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            length = 3;
            code = lead & 0x0FU;
            code = (code << 12U) | ((static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 6U);
            code |= static_cast<unsigned char>(text[index + 2]) & 0x3FU;
        } else {
            length = 1;
        }
        if (code >= 0x4E00U && code <= 0x9FFFU) {
            ++count;
        }
        index += length;
    }
    return count;
}

bool wait_until(const std::filesystem::path& path, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && !error) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

int children_of(int parent) {
    int count = 0;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator("/proc", error)) {
        if (error) {
            break;
        }
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            continue;
        }
        std::ifstream status(entry.path() / "status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("PPid:", 0) != 0) {
                continue;
            }
            if (std::atoi(line.c_str() + 5) == parent) {
                ++count;
            }
            break;
        }
    }
    return count;
}

bool directory_empty(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::directory_iterator end;
    const std::filesystem::directory_iterator it(path, error);
    return !error && it == end;
}

std::string read_fd(int fd) {
    std::string data;
    char buffer[256];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        data.append(buffer, static_cast<std::size_t>(got));
    }
    return data;
}

struct Captured {
    int code{-1};
    bool exited{false};
    bool signaled{false};
    bool timed_out{false};
    std::string out;
    std::string err;
};

class ParentRun {
  public:
    ParentRun(std::vector<std::string> args, std::filesystem::path cwd)
        : args_(std::move(args)), cwd_(std::move(cwd)) {
        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};
        if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
            return;
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
            return;
        }
        if (pid == 0) {
            ::setpgid(0, 0);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
            if (::chdir(cwd_.c_str()) != 0) {
                ::_exit(126);
            }
            std::vector<char*> argv;
            argv.reserve(args_.size() + 1);
            for (std::string& word : args_) {
                argv.push_back(word.data());
            }
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        ::setpgid(pid, pid);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        pid_ = static_cast<int>(pid);
        out_ = out_pipe[0];
        err_ = err_pipe[0];
    }

    ParentRun(const ParentRun&) = delete;
    ParentRun& operator=(const ParentRun&) = delete;

    ~ParentRun() {
        if (pid_ > 0) {
            ::kill(-pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        if (out_ >= 0) {
            ::close(out_);
        }
        if (err_ >= 0) {
            ::close(err_);
        }
    }

    [[nodiscard]] int pid() const { return pid_; }
    [[nodiscard]] bool running() const { return pid_ > 0; }

    Captured finish(std::chrono::milliseconds budget) {
        Captured result;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        int status = 0;
        while (pid_ > 0 && std::chrono::steady_clock::now() < deadline) {
            const pid_t got = ::waitpid(pid_, &status, WNOHANG);
            if (got == pid_) {
                result.exited = WIFEXITED(status) != 0;
                result.signaled = WIFSIGNALED(status) != 0;
                if (result.exited) {
                    result.code = WEXITSTATUS(status);
                } else if (result.signaled) {
                    result.code = WTERMSIG(status);
                }
                result.out = read_fd(out_);
                result.err = read_fd(err_);
                pid_ = -1;
                return result;
            }
            std::this_thread::sleep_for(20ms);
        }
        result.timed_out = true;
        if (pid_ > 0) {
            ::kill(-pid_, SIGKILL);
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        if (out_ >= 0) {
            result.out = read_fd(out_);
        }
        if (err_ >= 0) {
            result.err = read_fd(err_);
        }
        return result;
    }

  private:
    std::vector<std::string> args_;
    std::filesystem::path cwd_;
    int pid_{-1};
    int out_{-1};
    int err_{-1};
};

void check_usage(std::string_view text) {
    std::istringstream lines{std::string{text}};
    std::string line;
    REQUIRE(std::getline(lines, line));
    CHECK(chinese_count(line) >= 2);
    CHECK(chinese_count(line) <= 4);
    int details = 0;
    while (std::getline(lines, line)) {
        const auto space = line.find(' ');
        REQUIRE(space != std::string::npos);
        CHECK(chinese_count(line.substr(0, space)) >= 2);
        CHECK(chinese_count(line.substr(0, space)) <= 4);
        CHECK(chinese_count(line.substr(space + 1)) <= 20);
        ++details;
    }
    CHECK(details >= 1);
}

std::chrono::system_clock::time_point beijing_ten() {
    return std::chrono::sys_days{std::chrono::year{2026} / std::chrono::October / 1} +
           std::chrono::hours{2};
}

}  // namespace

TEST_CASE("an immediate death is not restarted many times in one second") {
    using robot_pm::restart_delay;
    CHECK(restart_delay(1) >= 1s);
    CHECK(restart_delay(2) > restart_delay(1));
    CHECK(restart_delay(3) > restart_delay(2));
    CHECK(restart_delay(2) == 2s);
    CHECK(restart_delay(7) == robot_pm::kRestartCap);

    std::chrono::milliseconds window{0};
    int fits = 0;
    for (int failures = 1; window + restart_delay(failures) <= 1s; ++failures) {
        window += restart_delay(failures);
        ++fits;
    }
    CHECK(fits <= 1);

    robot_pm::ChildExit crashed;
    crashed.signaled = true;
    crashed.code = SIGKILL;
    robot_pm::ChildExit failed;
    failed.code = 1;
    robot_pm::ChildExit clean;
    clean.clean = true;
    robot_pm::ChildExit stopped;
    stopped.stopped = true;
    stopped.signaled = true;
    CHECK(robot_pm::unexpected_exit(crashed));
    CHECK(robot_pm::unexpected_exit(failed));
    CHECK_FALSE(robot_pm::unexpected_exit(clean));
    CHECK_FALSE(robot_pm::unexpected_exit(stopped));
}

TEST_CASE("names and descriptions stay short") {
    ParentRun help({ROBOT_PM_SUPERVISE, "--帮助"}, make_root("robot-pm-supervise-help"));
    REQUIRE(help.running());
    const Captured shown = help.finish(2s);
    CHECK(shown.exited);
    CHECK(shown.code == 0);
    CHECK(shown.err.empty());
    check_usage(shown.out);

    ParentRun bad({ROBOT_PM_SUPERVISE}, make_root("robot-pm-supervise-bad"));
    REQUIRE(bad.running());
    const Captured rejected = bad.finish(2s);
    CHECK(rejected.exited);
    CHECK(rejected.code == 2);
    check_usage(rejected.out);

    ParentRun child_help({ROBOT_PM_STAND_IN, "--帮助"}, make_root("robot-pm-stand-help"));
    REQUIRE(child_help.running());
    const Captured child = child_help.finish(2s);
    CHECK(child.exited);
    CHECK(child.code == 0);
    check_usage(child.out);
}

TEST_CASE("the child finishes its recovery read before feedback") {
    const auto root = make_root("robot-pm-child-recover");
    REQUIRE(robot_pm::begin_interaction(root, "ou_a", "om_1", {{"utterance", "确认"}}).has_value());
    REQUIRE(robot_pm::end_interaction(root, "ou_a", "om_1", "cancel", "cancelled", beijing_ten())
                .has_value());
    const std::string log_before = read_text(root / "episodic" / "events.jsonl");
    std::filesystem::create_directories(root / "working" / "ou_a" / "om_1");
    {
        std::ofstream leftover(root / "working" / "ou_a" / "om_1" / "context.json");
        leftover << "{\"utterance\":\"残留\"}";
    }

    auto hold = robot_pm::ServiceHold::open(root);
    REQUIRE(hold.has_value());
    CHECK_FALSE(std::filesystem::exists(root / "working" / "ou_a" / "om_1"));
    CHECK_FALSE(std::filesystem::exists(root / "回执"));
    CHECK(read_text(root / "episodic" / "events.jsonl") == log_before);
    CHECK(hold->recovered() == log_before);
    CHECK(hold->recovered().find("om_1") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root / ".writer.lock"));

    std::ofstream receipt(root / "回执", std::ios::binary | std::ios::noreplace);
    REQUIRE(receipt);
    REQUIRE(hold->send(receipt, hold->recovered()).has_value());
    receipt.flush();
    CHECK(read_text(root / "回执") == log_before);
    CHECK_FALSE(std::filesystem::exists(root / ".writer.lock"));
}

TEST_CASE("a second start while the first child is alive does not create a second child") {
    robot_pm::clear_stop();
    const auto root = make_root("robot-pm-one-child");
    std::filesystem::create_directories(root);
    const int before = children_of(::getpid());
    robot_pm::OneChild child;
    const auto first = child.start({ROBOT_PM_STAND_IN, root.string(), "停着"});
    REQUIRE(first.has_value());
    CHECK(*first);
    REQUIRE(wait_until(root / "活着", 2s));
    const int live = child.id();
    CHECK(live > 0);
    CHECK(children_of(::getpid()) == before + 1);
    CHECK_FALSE(std::filesystem::exists(root / ".writer.lock"));
    CHECK(read_text(root / "活着") == std::to_string(live) + "\n");
    CHECK(live != ::getpid());

    const auto second = child.start({ROBOT_PM_STAND_IN, root.string(), "停着"});
    REQUIRE(second.has_value());
    CHECK_FALSE(*second);
    CHECK(child.id() == live);
    CHECK(child.starts() == 1);
    CHECK(children_of(::getpid()) == before + 1);
    CHECK(line_count(read_text(root / "活着")) == 1);

    child.halt();
    CHECK_FALSE(child.alive());
    CHECK(child.starts() == 1);
    CHECK(line_count(read_text(root / "活着")) == 1);
    CHECK(children_of(::getpid()) == before);
}

TEST_CASE("a crash is restarted once") {
    const auto data = make_root("robot-pm-crash-data");
    const auto cwd = make_root("robot-pm-crash-cwd");
    std::filesystem::create_directories(data / "episodic");
    {
        std::ofstream log(data / "episodic" / "events.jsonl");
        log << "旧记录\n";
    }
    const auto began = std::chrono::steady_clock::now();
    ParentRun parent({ROBOT_PM_SUPERVISE, "--程序", ROBOT_PM_STAND_IN, data.string(), "崩溃"}, cwd);
    REQUIRE(parent.running());
    const Captured done = parent.finish(4s);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    CHECK_FALSE(done.timed_out);
    CHECK(done.exited);
    CHECK(done.code == 0);
    CHECK(done.out.empty());
    CHECK(done.err.empty());
    CHECK(elapsed >= 1s);
    CHECK(elapsed < 2800ms);
    const std::string log = read_text(data / "episodic" / "events.jsonl");
    CHECK(log == "旧记录\n崩过\n");
    CHECK(read_text(data / "回执") == log);
    CHECK(directory_empty(cwd));
    CHECK_FALSE(std::filesystem::exists(data / ".writer.lock"));
}

TEST_CASE("a non-zero exit is restarted once") {
    const auto data = make_root("robot-pm-fail-data");
    const auto cwd = make_root("robot-pm-fail-cwd");
    std::filesystem::create_directories(data / "episodic");
    {
        std::ofstream log(data / "episodic" / "events.jsonl");
        log << "旧记录\n";
    }
    const auto began = std::chrono::steady_clock::now();
    ParentRun parent({ROBOT_PM_SUPERVISE, "--程序", ROBOT_PM_STAND_IN, data.string(), "失手"}, cwd);
    REQUIRE(parent.running());
    const Captured done = parent.finish(4s);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    CHECK_FALSE(done.timed_out);
    CHECK(done.exited);
    CHECK(done.code == 0);
    CHECK(done.out.empty());
    CHECK(done.err.empty());
    CHECK(elapsed >= 1s);
    CHECK(elapsed < 2800ms);
    CHECK(read_text(data / "回执") == "旧记录\n崩过\n");
    CHECK(directory_empty(cwd));
}

TEST_CASE("a clean exit is not restarted") {
    const auto data = make_root("robot-pm-clean-data");
    const auto cwd = make_root("robot-pm-clean-cwd");
    const auto began = std::chrono::steady_clock::now();
    ParentRun parent({ROBOT_PM_SUPERVISE, "--程序", ROBOT_PM_STAND_IN, data.string(), "做完"}, cwd);
    REQUIRE(parent.running());
    const Captured done = parent.finish(2s);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    CHECK_FALSE(done.timed_out);
    CHECK(done.exited);
    CHECK(done.code == 0);
    CHECK(done.out.empty());
    CHECK(done.err.empty());
    CHECK(elapsed < 800ms);
    CHECK(std::filesystem::is_regular_file(data / "回执"));
    CHECK(directory_empty(cwd));
}

TEST_CASE("a requested stop is not restarted") {
    const auto data = make_root("robot-pm-stop-data");
    const auto cwd = make_root("robot-pm-stop-cwd");
    ParentRun parent({ROBOT_PM_SUPERVISE, "--程序", ROBOT_PM_STAND_IN, data.string(), "停着"}, cwd);
    REQUIRE(parent.running());
    REQUIRE(wait_until(data / "活着", 2s));
    const std::string alive = read_text(data / "活着");
    CHECK(line_count(alive) == 1);
    CHECK_FALSE(std::filesystem::exists(data / ".writer.lock"));
    CHECK(alive != std::to_string(parent.pid()) + "\n");

    const auto marked = std::chrono::steady_clock::now();
    REQUIRE(::kill(parent.pid(), SIGTERM) == 0);
    const Captured done = parent.finish(2s);
    const auto elapsed = std::chrono::steady_clock::now() - marked;
    CHECK_FALSE(done.timed_out);
    CHECK(done.exited);
    CHECK(done.code == 0);
    CHECK(done.out.empty());
    CHECK(done.err.empty());
    CHECK(elapsed < 800ms);
    CHECK(read_text(data / "活着") == alive);
    CHECK_FALSE(std::filesystem::exists(data / "回执"));
    CHECK(directory_empty(cwd));

    const int child = std::atoi(alive.c_str());
    CHECK(::kill(child, 0) != 0);
    CHECK_FALSE(std::filesystem::exists(data / ".writer.lock"));
}

TEST_CASE("a child that keeps dying is not restarted many times in one second") {
    const auto data = make_root("robot-pm-loop-data");
    const auto cwd = make_root("robot-pm-loop-cwd");
    ParentRun parent({ROBOT_PM_SUPERVISE, "--程序", ROBOT_PM_STAND_IN, data.string(), "连崩"}, cwd);
    REQUIRE(parent.running());
    REQUIRE(wait_until(data / "次数", 2s));
    std::this_thread::sleep_for(1200ms);
    const int during = line_count(read_text(data / "次数"));
    CHECK(during >= 1);
    CHECK(during <= 2);
    REQUIRE(::kill(parent.pid(), SIGTERM) == 0);
    const Captured done = parent.finish(2s);
    CHECK(done.exited);
    CHECK(done.code == 0);
    CHECK(done.out.empty());
    CHECK(done.err.empty());
    CHECK(line_count(read_text(data / "次数")) <= 2);
    CHECK(directory_empty(cwd));
    CHECK_FALSE(std::filesystem::exists(data / "回执"));
}
