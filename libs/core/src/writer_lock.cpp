#include "robot_pm/writer_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace robot_pm {

WriterLock::WriterLock(WriterLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

WriterLock& WriterLock::operator=(WriterLock&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

WriterLock::~WriterLock() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

WriterLock WriterLock::acquire(const std::filesystem::path& data_root) {
    WriterLock lock;
    std::error_code error;
    if (!std::filesystem::is_directory(data_root, error) || error) {
        return lock;
    }
    const std::filesystem::path path = data_root / ".writer.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return lock;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return lock;
    }
    if (::fchmod(fd, 0600) != 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return lock;
    }
    struct stat info {};
    if (::stat(data_root.c_str(), &info) == 0 && info.st_uid == ::geteuid()) {
        ::chmod(data_root.c_str(), 0700);
    }
    lock.fd_ = fd;
    return lock;
}

}  // namespace robot_pm
