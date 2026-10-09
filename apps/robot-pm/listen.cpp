#include "listen.hpp"

#include "robot_pm/frame.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

namespace robot_pm {
namespace {

volatile std::sig_atomic_t g_process_stop = 0;

[[nodiscard]] bool read_request(int fd, std::stop_token stop, FeishuRequest& request) {
    std::string incoming;
    incoming.reserve(4096);
    bool peer_closed = false;
    while (!stop.stop_requested() && g_process_stop == 0) {
        const HttpFrame framed = read_http_frame(incoming, peer_closed);
        if (framed.kind == FrameKind::kComplete) {
            if (framed.body.size() > 1024U * 1024U) {
                return false;
            }
            request.timestamp = framed.timestamp;
            request.nonce = framed.nonce;
            request.signature = framed.signature;
            request.body = framed.body;
            return true;
        }
        if (framed.kind == FrameKind::kAbsent || peer_closed) {
            return false;
        }
        pollfd ready{};
        ready.fd = fd;
        ready.events = POLLIN;
        const int polled = ::poll(&ready, 1, 200);
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (polled == 0) {
            continue;
        }
        char buffer[1024];
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            peer_closed = true;
            continue;
        }
        incoming.append(buffer, static_cast<std::size_t>(got));
        if (incoming.size() > 2U * 1024U * 1024U) {
            return false;
        }
    }
    return false;
}

}  // namespace

void note_process_stop() noexcept { g_process_stop = 1; }

HttpEventPort::HttpEventPort(std::uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        failure_ = "端口被占";
        return;
    }
    const int enabled = 1;
    static_cast<void>(::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_fd_, 16) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        failure_ = "端口被占";
        return;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }
}

HttpEventPort::~HttpEventPort() {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

bool HttpEventPort::ok() const { return listen_fd_ >= 0; }

std::string_view HttpEventPort::failure() const { return failure_; }

std::uint16_t HttpEventPort::bound_port() const { return bound_port_; }

bool HttpEventPort::failed() const { return failed_; }

std::optional<FeishuRequest> HttpEventPort::take(std::stop_token stop) {
    while (!stop.stop_requested() && g_process_stop == 0 && listen_fd_ >= 0) {
        pollfd ready{};
        ready.fd = listen_fd_;
        ready.events = POLLIN;
        const int polled = ::poll(&ready, 1, 200);
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed_ = true;
            return std::nullopt;
        }
        if (polled == 0) {
            continue;
        }
        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed_ = true;
            return std::nullopt;
        }
        FeishuRequest request;
        if (!read_request(client_fd_, stop, request)) {
            if (client_fd_ >= 0) {
                ::close(client_fd_);
                client_fd_ = -1;
            }
            continue;
        }
        return request;
    }
    return std::nullopt;
}

void HttpEventPort::reply(std::string_view body) {
    if (client_fd_ < 0) {
        return;
    }
    const std::string payload =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t wrote = ::send(client_fd_, payload.data() + offset, payload.size() - offset, MSG_NOSIGNAL);
        if (wrote < 0) {
            break;
        }
        offset += static_cast<std::size_t>(wrote);
    }
    ::close(client_fd_);
    client_fd_ = -1;
}

}  // namespace robot_pm
