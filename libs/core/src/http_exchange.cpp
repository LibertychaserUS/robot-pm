#include "robot_pm/http_exchange.hpp"

#include "robot_pm/frame.hpp"

#include <openssl/ssl.h>

#include <optional>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>

namespace robot_pm {
namespace {

struct UrlParts {
    bool tls{false};
    std::string host;
    std::string port;
    std::string target;
};

[[nodiscard]] std::optional<UrlParts> split_url(std::string_view url) {
    UrlParts parts;
    std::string_view rest = url;
    if (rest.starts_with("https://")) {
        parts.tls = true;
        rest.remove_prefix(8);
        parts.port = "443";
    } else if (rest.starts_with("http://")) {
        rest.remove_prefix(7);
        parts.port = "80";
    } else {
        return std::nullopt;
    }
    const std::size_t slash = rest.find('/');
    const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    parts.target = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        parts.host = std::string(authority.substr(0, colon));
        parts.port = std::string(authority.substr(colon + 1));
    } else {
        parts.host = std::string(authority);
    }
    if (parts.host.empty() || parts.port.empty()) {
        return std::nullopt;
    }
    return parts;
}

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_{-1};
};

[[nodiscard]] std::expected<Socket, Error> connect_host(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "连不上对方"});
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(result, &freeaddrinfo);
    for (addrinfo* info = result; info != nullptr; info = info->ai_next) {
        const int fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, info->ai_addr, info->ai_addrlen) == 0) {
            return Socket(fd);
        }
        ::close(fd);
    }
    return std::unexpected(Error{ErrorCode::kBridgeFailed, "连不上对方"});
}

class Tls {
public:
    Tls() = default;
    Tls(const Tls&) = delete;
    Tls& operator=(const Tls&) = delete;
    ~Tls() {
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
        }
    }

    [[nodiscard]] std::expected<void, Error> open(int fd, const std::string& host) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) {
            return std::unexpected(Error{ErrorCode::kBridgeFailed, "连不上对方"});
        }
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) {
            return std::unexpected(Error{ErrorCode::kBridgeFailed, "连不上对方"});
        }
        SSL_set_fd(ssl_, fd);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) {
            return std::unexpected(Error{ErrorCode::kBridgeFailed, "连不上对方"});
        }
        return {};
    }

    [[nodiscard]] bool write_all(std::string_view bytes) const {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const int wrote = SSL_write(ssl_, bytes.data() + offset, static_cast<int>(bytes.size() - offset));
            if (wrote <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    [[nodiscard]] int read_some(char* buffer, int size) const { return SSL_read(ssl_, buffer, size); }

private:
    SSL_CTX* ctx_{nullptr};
    SSL* ssl_{nullptr};
};

[[nodiscard]] bool plain_write(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t wrote = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(wrote);
    }
    return true;
}

}  // namespace

std::expected<HttpExchange, Error> http_exchange(std::string_view method,
                                                std::string_view url,
                                                const std::vector<std::pair<std::string, std::string>>& headers,
                                                std::string_view body) {
    const std::optional<UrlParts> parts = split_url(url);
    if (!parts) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "地址不对"});
    }
    std::expected<Socket, Error> socket = connect_host(parts->host, parts->port);
    if (!socket) {
        return std::unexpected(socket.error());
    }
    Tls tls;
    if (parts->tls) {
        const std::expected<void, Error> opened = tls.open(socket->get(), parts->host);
        if (!opened) {
            return std::unexpected(opened.error());
        }
    }
    std::string request;
    request.reserve(body.size() + 256);
    request.append(method);
    request.push_back(' ');
    request.append(parts->target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(parts->host);
    request.append("\r\nContent-Length: ");
    request.append(std::to_string(body.size()));
    request.append("\r\nConnection: close\r\n");
    for (const auto& [name, value] : headers) {
        request.append(name);
        request.append(": ");
        request.append(value);
        request.append("\r\n");
    }
    request.append("\r\n");
    request.append(body);
    const bool wrote = parts->tls ? tls.write_all(request) : plain_write(socket->get(), request);
    if (!wrote) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "请求没有送出"});
    }
    std::string incoming;
    char buffer[2048];
    while (true) {
        int got = 0;
        if (parts->tls) {
            got = tls.read_some(buffer, static_cast<int>(sizeof(buffer)));
            if (got < 0) {
                return std::unexpected(Error{ErrorCode::kBridgeFailed, "响应没有收全"});
            }
        } else {
            const ssize_t plain = ::recv(socket->get(), buffer, sizeof(buffer), 0);
            if (plain < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(Error{ErrorCode::kBridgeFailed, "响应没有收全"});
            }
            got = static_cast<int>(plain);
        }
        if (got == 0) {
            break;
        }
        incoming.append(buffer, static_cast<std::size_t>(got));
        if (incoming.size() > 8U * 1024U * 1024U) {
            return std::unexpected(Error{ErrorCode::kBridgeFailed, "响应没有收全"});
        }
    }
    const HttpFrame frame = read_http_frame(incoming, true);
    if (frame.kind != FrameKind::kComplete) {
        return std::unexpected(Error{ErrorCode::kBridgeFailed, "响应没有收全"});
    }
    HttpExchange exchange;
    exchange.body = frame.body;
    exchange.status = 200;
    if (incoming.starts_with("HTTP/")) {
        const std::size_t space = incoming.find(' ');
        if (space != std::string::npos) {
            exchange.status = 0;
            for (std::size_t index = space + 1; index < incoming.size() && incoming[index] >= '0' && incoming[index] <= '9';
                 ++index) {
                exchange.status = exchange.status * 10 + (incoming[index] - '0');
            }
        }
    }
    return exchange;
}

}  // namespace robot_pm
