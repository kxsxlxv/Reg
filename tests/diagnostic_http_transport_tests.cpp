#include "diagnostics/api/transport/LoopbackHttpServer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using reg::diagnostics::api::HttpRequest;
using reg::diagnostics::api::HttpResponse;
using reg::diagnostics::api::transport::LoopbackHttpServer;
using namespace std::chrono_literals;

namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
void closeSocket(Socket s) { if (s != invalidSocket) (void)closesocket(s); }
#else
using Socket = int;
constexpr Socket invalidSocket = -1;
void closeSocket(Socket s) { if (s != invalidSocket) (void)::close(s); }
#endif

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TcpClient {
public:
    TcpClient(std::uint16_t port, const char* address = "127.0.0.1") {
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(fd_ != invalidSocket, "TCP client socket");
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        require(inet_pton(AF_INET, address, &dest.sin_addr) == 1, "test address");
#ifdef _WIN32
        const DWORD millis = 4000;
        (void)setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&millis), sizeof(millis));
#else
        timeval timeout{4, 0};
        (void)setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0;
    }
    ~TcpClient() { closeSocket(fd_); }
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    bool connected() const { return connected_; }
    void write(std::string_view bytes) {
        require(connected_, "send requires connected client");
        while (!bytes.empty()) {
#ifdef _WIN32
            const int sent = ::send(fd_, bytes.data(), static_cast<int>(bytes.size()), 0);
#else
            const ssize_t sent = ::send(fd_, bytes.data(), bytes.size(), 0);
#endif
            require(sent > 0, "test client send");
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
    }
    std::string readToClose() {
        std::string result;
        char buf[8192];
        for (;;) {
#ifdef _WIN32
            const int n = ::recv(fd_, buf, sizeof(buf), 0);
#else
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            result.append(buf, static_cast<std::size_t>(n));
        }
        return result;
    }
    void disconnect() { closeSocket(fd_); fd_ = invalidSocket; connected_ = false; }
private:
    Socket fd_{invalidSocket};
    bool connected_{false};
};

std::string tcpExchange(std::uint16_t port, std::string_view request, bool split = false) {
    TcpClient client(port);
    require(client.connected(), "connect to loopback server");
    if (split) {
        auto half = request.size() / 2;
        client.write(request.substr(0, half));
        std::this_thread::sleep_for(20ms);
        client.write(request.substr(half));
    } else {
        client.write(request);
    }
    return client.readToClose();
}

std::string request(std::string_view target = "/healthz") {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
}

std::string hostRequest(std::string_view host) {
    return "GET /v1/snapshot HTTP/1.1\r\nHost: " + std::string(host) + "\r\n\r\n";
}

void expectStatus(std::uint16_t port, std::string_view raw, int status) {
    const std::string response = tcpExchange(port, raw);
    if (response.find("HTTP/1.1 " + std::to_string(status) + " ") != 0) {
        std::cerr << "expected=" << status << " got=" << response.substr(0, 60)
                  << " request=" << std::string(raw).substr(0, 90) << "\n";
        throw std::runtime_error("expected status");
    }
    require(response.find("\r\nConnection: close\r\n") != std::string::npos,
            "connection must close");
}

void verifyLengths(std::string_view result, std::string_view expectedBody) {
    const auto sep = result.find("\r\n\r\n");
    require(sep != std::string_view::npos, "response header terminator");
    require(result.substr(sep + 4) == expectedBody, "response body must match");
    require(result.find("Content-Length: " + std::to_string(expectedBody.size()) + "\r\n") !=
                std::string_view::npos, "response length exact");
}

void testServer() {
    std::atomic<unsigned> handled{0};
    std::atomic<unsigned> inHandler{0};
    std::atomic<unsigned> maxInHandler{0};
    LoopbackHttpServer server([&](const HttpRequest& req) -> HttpResponse {
        ++handled;
        if (req.target == "/throw") throw std::runtime_error("confidential-token");
        if (req.target == "/huge") return {200, "application/json", std::string(1024 * 1024 + 1, 'a')};
        if (req.target == "/exact-limit") return {200, "application/json", std::string(1024 * 1024, 'a')};
        if (req.target == "/injected") return {200, "text/plain\r\nSet-Cookie: stolen=1", "foo"};
        if (req.target == "/slow") {
            const unsigned now = inHandler.fetch_add(1) + 1;
            unsigned prior = maxInHandler.load();
            while (prior < now && !maxInHandler.compare_exchange_weak(prior, now)) {}
            std::this_thread::sleep_for(50ms);
            inHandler.fetch_sub(1);
        }
        return {200, "application/json", "{\"ok\":true}"};
    }, 0);
    require(!server.isRunning() && server.boundPort() == 0, "disabled before start");
    server.start();
    server.start();
    const auto port = server.boundPort();
    require(port != 0 && server.isRunning(), "port 0 must allocate nonzero port");

    const auto good = tcpExchange(port, request());
    require(good.starts_with("HTTP/1.1 200 OK\r\n"), "healthy status");
    verifyLengths(good, "{\"ok\":true}");
    require(good.find("X-Content-Type-Options: nosniff") != std::string::npos,
            "response must disable sniffing");
    verifyLengths(tcpExchange(port, request(), true), "{\"ok\":true}");

    // A browser-origin DNS rebinding attempt reaches 127.0.0.1 with an
    // attacker-controlled Host. Never forward it to the diagnostic Handler.
    verifyLengths(tcpExchange(port, hostRequest("127.0.0.1:" + std::to_string(port))),
                  "{\"ok\":true}");
    verifyLengths(tcpExchange(port, hostRequest("LOCALHOST")),
                  "{\"ok\":true}");
    verifyLengths(tcpExchange(port, hostRequest("localhost:" + std::to_string(port))),
                  "{\"ok\":true}");
    const auto handledBeforeInvalidHosts = handled.load();
    expectStatus(port, hostRequest("attacker.example"), 400);
    expectStatus(port, hostRequest("example.org:8765"), 400);
    expectStatus(port, hostRequest("127.0.0.1@attacker.example"), 400);
    expectStatus(port, hostRequest("127.0.0.1:0"), 400);
    expectStatus(port, hostRequest("127.0.0.1:65536"), 400);
    expectStatus(port, hostRequest("127.0.0.1:xyz"), 400);
    expectStatus(port, hostRequest("127.0.0.1:"), 400);
    expectStatus(port, hostRequest("127.0.0.1:00080"), 400);
    expectStatus(port, hostRequest("[::1]"), 400);
    expectStatus(port, hostRequest("localhost."), 400);
    expectStatus(port, hostRequest("127.0.0.1,attacker.example"), 400);
    expectStatus(port, hostRequest("127.0.0.1:" +
                 std::to_string(port == 65535 ? 65534 : port + 1)), 400);
    expectStatus(port, "GET /v1/snapshot HTTP/1.1\r\nHost: localhost\r\nHost: 127.0.0.1\r\n\r\n", 400);
    require(handled.load() == handledBeforeInvalidHosts,
            "invalid Host requests must not be forwarded to Handler");
    require(good.find("Access-Control-Allow-Origin") == std::string::npos,
            "diagnostic responses must not enable cross-origin reads");

    expectStatus(port, "POST /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 405);
    expectStatus(port, "DELETE /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 405);
    expectStatus(port, "GET /healthz HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", 400);
    expectStatus(port, "GET  /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 400);
    expectStatus(port, "GET http://evil.example/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 400);
    expectStatus(port, "GET /a%0d%0a HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\nHost: 127.0.0.1\n\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n bad-fold\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: x\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n", 400);
    expectStatus(port, "GET /healthz HTTP/1.1\r\nHost : 127.0.0.1\r\n\r\n", 400);
    expectStatus(port, request(std::string(2050, 'x').insert(0,"/")), 414);
    expectStatus(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Large: " + std::string(8192, 'A') + "\r\n\r\n", 431);
    expectStatus(port, request("/throw"), 500);
    const auto thrown = tcpExchange(port, request("/throw"));
    require(thrown.find("confidential-token") == std::string::npos, "handler error leaked secret");
    expectStatus(port, request("/huge"), 500);
    const auto maximumBody = tcpExchange(port, request("/exact-limit"));
    require(maximumBody.find("Content-Length: 1048576\r\n") != std::string::npos,
            "exact 1 MiB permitted and length correct");
    require(maximumBody.size() >= 1048576, "maximum body transmitted");
    expectStatus(port, request("/injected"), 500);

    // 127.0.0.2 is also within IPv4 loopback, but the server must never bind there.
    TcpClient otherAddress(port, "127.0.0.2");
    require(!otherAddress.connected(), "socket must bind only 127.0.0.1, not INADDR_ANY");

    // Attempt to re-bind the same port: failure must not terminate existing listener.
    LoopbackHttpServer conflict([](const HttpRequest&) { return HttpResponse{}; }, port);
    bool rejected = false;
    try { conflict.start(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "occupied port must fail");
    require(tcpExchange(port, request()).starts_with("HTTP/1.1 200"), "existing listener intact");

    std::vector<std::thread> threads;
    std::atomic<unsigned> successful{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            if (tcpExchange(port, request("/slow")).starts_with("HTTP/1.1 200")) ++successful;
        });
    }
    for (auto& thread : threads) thread.join();
    require(successful == 8, "bounded workers serve concurrent requests");
    require(maxInHandler.load() <= LoopbackHttpServer::kWorkers,
            "never invoke more than two Handlers concurrently");

    // A client that disappears while the request is incomplete must not crash Reg.
    {
        TcpClient disconnected(port);
        require(disconnected.connected(), "disconnect client connected");
        disconnected.write("GET /healthz HTTP/1.1\r\nHost:");
        disconnected.disconnect();
    }
    // Stop must actively interrupt blocked recv(), not wait for 2 s timeout.
    {
        TcpClient incomplete(port);
        require(incomplete.connected(), "incomplete client connected");
        incomplete.write("GET /healthz HTTP/1.1\r\nHost:");
        std::this_thread::sleep_for(50ms);
        const auto started = std::chrono::steady_clock::now();
        server.stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < 1200ms, "stop under blocked client must be bounded");
    }
    server.stop();
    require(!server.isRunning() && server.boundPort() == 0, "stopped status");
    server.start();
    require(server.isRunning() && server.boundPort() != 0, "restart works");
    server.stop();
    require(handled.load() >= 10, "handler was invoked on real sockets");
}

void testDestructorShutdown() {
    auto handler = [](const HttpRequest&) { return HttpResponse{}; };
    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<TcpClient> incomplete;
    {
        LoopbackHttpServer server(handler, 0);
        server.start();
        incomplete = std::make_unique<TcpClient>(server.boundPort());
        require(incomplete->connected(), "destructor test connected");
        incomplete->write("GET /healthz HTTP/1.1\r\nHost:");
        std::this_thread::sleep_for(30ms);
        // Destructor must interrupt the socket while the client stays open.
    }
    incomplete.reset();
    require(std::chrono::steady_clock::now() - started < 800ms,
            "destructor shutdown must not wait for read timeout");
}

void testDeadline() {
    LoopbackHttpServer server([](const HttpRequest&) { return HttpResponse{}; }, 0);
    server.start();
    TcpClient incomplete(server.boundPort());
    incomplete.write("GET /healthz HTTP/1.1\r\nHost:");
    const auto started = std::chrono::steady_clock::now();
    const auto response = incomplete.readToClose();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(response.starts_with("HTTP/1.1 408"), "slowloris request timeout");
    require(elapsed >= 1500ms && elapsed < 3100ms, "overall read deadline <=2 seconds approximately");
    server.stop();
}
} // namespace

int main() {
    try {
        testServer();
        testDeadline();
        testDestructorShutdown();
        std::cout << "diagnostic_http_transport_tests: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "diagnostic_http_transport_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
