#pragma once

// 这个文件负责数据目录的写入锁。
// 不变量：进程活着就占着 .writer.lock；第二个进程拿不到锁就退出。
// 锁文件不跟随符号链接。

#include <filesystem>

namespace robot_pm {

class WriterLock {
public:
    WriterLock() = default;
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    WriterLock(WriterLock&& other) noexcept;
    WriterLock& operator=(WriterLock&& other) noexcept;
    ~WriterLock();

    [[nodiscard]] explicit operator bool() const { return fd_ >= 0; }

    // 前置条件：目录存在，调用方可以在里面建文件。
    // 失败：锁被占、路径不对，或建不了文件。返回的对象是空的。
    [[nodiscard]] static WriterLock acquire(const std::filesystem::path& data_root);

private:
    int fd_{-1};
};

}  // namespace robot_pm
