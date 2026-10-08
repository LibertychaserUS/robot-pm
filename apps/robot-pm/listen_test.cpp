#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "listen.hpp"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

namespace {

[[nodiscard]] int connect_to(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

TEST_CASE("listen reads one feishu request and answers it") {
    robot_pm::HttpEventPort port(0);
    REQUIRE(port.ok());
    std::stop_source source;
    std::optional<robot_pm::FeishuRequest> got;
    std::thread worker([&] { got = port.take(source.get_token()); });

    const std::string payload = "{\"type\":\"url_verification\"}";
    const std::string request = "POST / HTTP/1.1\r\nHost: localhost\r\nX-Lark-Request-Timestamp: 1710000000\r\n"
                                "X-Lark-Request-Nonce: nonce-1\r\nX-Lark-Signature: abc\r\nContent-Length: " +
                                std::to_string(payload.size()) + "\r\n\r\n" + payload;
    const int client = connect_to(port.bound_port());
    REQUIRE(client >= 0);
    REQUIRE(::send(client, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
    worker.join();
    REQUIRE(got.has_value());
    CHECK(got->timestamp == "1710000000");
    CHECK(got->nonce == "nonce-1");
    CHECK(got->signature == "abc");
    CHECK(got->body == payload);

    const std::string answer = "{\"challenge\":\"c1\"}";
    port.reply(answer);
    std::string received;
    pollfd ready{};
    ready.fd = client;
    ready.events = POLLIN;
    REQUIRE(::poll(&ready, 1, 2000) > 0);
    char buffer[512];
    const ssize_t got_bytes = ::recv(client, buffer, sizeof(buffer), 0);
    REQUIRE(got_bytes > 0);
    received.assign(buffer, static_cast<std::size_t>(got_bytes));
    CHECK(received.find(answer) != std::string::npos);
    CHECK(received.find("secret") == std::string::npos);
    ::close(client);
}

TEST_CASE("listen returns when stopped") {
    robot_pm::HttpEventPort port(0);
    REQUIRE(port.ok());
    std::stop_source source;
    std::optional<robot_pm::FeishuRequest> got{robot_pm::FeishuRequest{}};
    std::thread worker([&] { got = port.take(source.get_token()); });
    source.request_stop();
    worker.join();
    CHECK_FALSE(got.has_value());
}
