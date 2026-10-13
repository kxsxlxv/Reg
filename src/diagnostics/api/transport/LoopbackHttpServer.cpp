#include "diagnostics/api/transport/LoopbackHttpServer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace reg::diagnostics::api::transport {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto kReadDeadline = 1900ms;
constexpr auto kSendDeadline = 2s;
constexpr auto kPollInterval = 100ms;
constexpr auto kSocketTimeout = 100ms;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
constexpr int kShutdownBoth = SD_BOTH;
int socketErrorCode() noexcept { return WSAGetLastError(); }
bool isRetryable(int code) noexcept {
    return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT || code == WSAEINTR;
}
void closeSocket(Socket s) noexcept { if (s != kInvalid) (void)closesocket(s); }
void shutdownSocket(Socket s) noexcept { if (s != kInvalid) (void)::shutdown(s, kShutdownBoth); }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
constexpr int kShutdownBoth = SHUT_RDWR;
int socketErrorCode() noexcept { return errno; }
bool isRetryable(int code) noexcept {
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
}
void closeSocket(Socket s) noexcept { if (s != kInvalid) (void)::close(s); }
void shutdownSocket(Socket s) noexcept { if (s != kInvalid) (void)::shutdown(s, kShutdownBoth); }
#endif

Socket fromHandle(std::uintptr_t value) noexcept {
    return value == ~std::uintptr_t{0} ? kInvalid : static_cast<Socket>(value);
}
std::uintptr_t toHandle(Socket value) noexcept {
    return value == kInvalid ? ~std::uintptr_t{0} : static_cast<std::uintptr_t>(value);
}
std::runtime_error socketFailure(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed (socket error " +
                              std::to_string(socketErrorCode()) + ")");
}

bool setNonblocking(Socket s, bool enabled) noexcept {
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}

bool setTimeouts(Socket s) noexcept {
#ifdef _WIN32
    const DWORD millis = static_cast<DWORD>(kSocketTimeout.count());
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&millis), sizeof(millis)) == 0 &&
           setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                      reinterpret_cast<const char*>(&millis), sizeof(millis)) == 0;
#else
    timeval timeout{0, static_cast<suseconds_t>(kSocketTimeout.count() * 1000)};
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

// Status strings are a fixed table; neither request input nor Handler-supplied
// strings are ever interpolated into the HTTP status line or headers.
std::string_view reason(int code) noexcept {
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return {};
    }
}

HttpResponse failure(int code) {
    // Error messages are deliberately independent of untrusted request bytes.
    return {code, "application/json; charset=utf-8", "{\"error\":\"request_rejected\"}"};
}

bool sendAll(Socket s, std::string_view bytes, Clock::time_point until) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size() && Clock::now() < until) {
        const std::size_t batch = std::min<std::size_t>(bytes.size() - offset, 64 * 1024);
#ifdef _WIN32
        const int written = ::send(s, bytes.data() + offset, static_cast<int>(batch), 0);
#else
        const ssize_t written = ::send(s, bytes.data() + offset, batch, MSG_NOSIGNAL);
#endif
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
        } else if (written == 0 || !isRetryable(socketErrorCode())) {
            return false;
        }
    }
    return offset == bytes.size();
}

void sendResponse(Socket s, HttpResponse response) noexcept {
    try {
        if (reason(response.statusCode).empty() ||
            (response.statusCode == 204 && !response.body.empty()) ||
            (response.contentType != "application/json; charset=utf-8" &&
             response.contentType != "application/json" &&
             response.contentType != "text/plain; charset=utf-8" &&
             response.contentType != "text/plain") ||
            response.body.size() > LoopbackHttpServer::kMaxResponseBodyBytes) {
            response = failure(500);
        }
        std::string header;
        header.reserve(256);
        header += "HTTP/1.1 ";
        header += std::to_string(response.statusCode);
        header += ' ';
        header += reason(response.statusCode);
        header += "\r\nContent-Type: ";
        header += response.contentType;
        header += "\r\nContent-Length: ";
        header += std::to_string(response.body.size());
        header += "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        const auto deadline = Clock::now() + kSendDeadline;
        if (sendAll(s, header, deadline)) {
            (void)sendAll(s, response.body, deadline);
        }
    } catch (...) {
        // Even allocation failures in diagnostic transport must not kill Reg.
    }
}

bool isTokenChar(unsigned char ch) noexcept {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch == 0x60 || std::string_view("!#$%&'*+-.^_|~").find(static_cast<char>(ch)) != std::string_view::npos);
}

bool equalsAsciiIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ch = static_cast<unsigned char>(a[i]);
        const auto bh = static_cast<unsigned char>(b[i]);
        if ((ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) !=
            (bh >= 'A' && bh <= 'Z' ? bh + 32 : bh)) return false;
    }
    return true;
}

bool validTarget(std::string_view target) noexcept {
    if (target.empty() || target[0] != '/' || target.find('#') != std::string_view::npos ||
        target.find('\\') != std::string_view::npos) return false;
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto ch = static_cast<unsigned char>(target[i]);
        if (ch < 0x21 || ch > 0x7e) return false;
        if (ch == '%') {
            if (i + 2 >= target.size() ||
                !std::isxdigit(static_cast<unsigned char>(target[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(target[i + 2]))) return false;
            // Encoded ASCII control characters are not allowed in a URL.
            auto hex = [](unsigned char c) -> unsigned {
                return c >= '0' && c <= '9' ? c - '0' :
                       c >= 'A' && c <= 'F' ? c - 'A' + 10U : c - 'a' + 10U;
            };
            const unsigned decoded = 16U * hex(static_cast<unsigned char>(target[i + 1])) +
                                      hex(static_cast<unsigned char>(target[i + 2]));
            if (decoded <= 0x20 || decoded == 0x7f) return false;
            i += 2;
        }
    }
    return true;
}

// Parse only the request line + headers. No request bodies, chunking or
// persistent connections are supported: reject *any* framing header, even
// Content-Length: 0, rather than risk ambiguous interpretation by peers.
int parseRequest(std::string_view header, HttpRequest& request) noexcept {
    if (header.size() < 4 || header.substr(header.size() - 4) != "\r\n\r\n") return 400;
    for (std::size_t i = 0; i < header.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(header[i]);
        if (ch == '\r' && (i + 1 >= header.size() || header[i + 1] != '\n')) return 400;
        if (ch == '\n' && (i == 0 || header[i - 1] != '\r')) return 400;
        if (ch == 0 || ch == 0x7f || ch >= 0x80) return 400;
    }
    const auto firstEnd = header.find("\r\n");
    if (firstEnd == std::string_view::npos || firstEnd == 0) return 400;
    const auto line = header.substr(0, firstEnd);
    const auto firstSpace = line.find(' ');
    const auto secondSpace = firstSpace == std::string_view::npos
                                 ? std::string_view::npos : line.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos ||
        line.find(' ', secondSpace + 1) != std::string_view::npos ||
        line.substr(secondSpace + 1) != "HTTP/1.1") return 400;
    const auto method = line.substr(0, firstSpace);
    const auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (method.empty() || !std::all_of(method.begin(), method.end(),
          [](unsigned char ch) { return isTokenChar(ch); })) return 400;
    if (target.size() > LoopbackHttpServer::kMaxTargetBytes) return 414;
    if (!validTarget(target)) return 400;

    bool hostSeen = false;
    std::size_t position = firstEnd + 2;
    while (position < header.size() - 2) {
        const auto end = header.find("\r\n", position);
        if (end == std::string_view::npos || end == position) return 400;
        const auto field = header.substr(position, end - position);
        const auto colon = field.find(':');
        if (colon == std::string_view::npos || colon == 0) return 400;
        const auto name = field.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(),
              [](unsigned char ch) { return isTokenChar(ch); })) return 400;
        const auto value = field.substr(colon + 1);
        for (unsigned char ch : value) {
            if (ch != '\t' && (ch < 0x20 || ch > 0x7e)) return 400;
        }
        if (equalsAsciiIgnoreCase(name, "host")) {
            if (hostSeen || value.find_first_not_of(" \t") == std::string_view::npos) return 400;
            hostSeen = true;
        }
        if (equalsAsciiIgnoreCase(name, "content-length") ||
            equalsAsciiIgnoreCase(name, "transfer-encoding") ||
            equalsAsciiIgnoreCase(name, "expect") ||
            equalsAsciiIgnoreCase(name, "upgrade")) return 400;
        position = end + 2;
    }
    if (!hostSeen || position != header.size() - 2) return 400;
    if (method != "GET") return 405;
    request.method = "GET";
    request.target.assign(target);
    return 200;
}

} // namespace

LoopbackHttpServer::LoopbackHttpServer(Handler handler, std::uint16_t port)
    : handler_(std::move(handler)), requestedPort_(port) {
    if (!handler_) throw std::invalid_argument("Diagnostic HTTP handler is required");
}

LoopbackHttpServer::~LoopbackHttpServer() { stop(); }

void LoopbackHttpServer::start() {
    std::lock_guard lifecycle(lifecycleMutex_);
    if (running_.load()) return;
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw socketFailure("WSAStartup");
    winsockInitialized_ = true;
#endif
    Socket listening = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listening == kInvalid) {
#ifdef _WIN32
        WSACleanup();
        winsockInitialized_ = false;
#endif
        throw socketFailure("socket");
    }
    try {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(requestedPort_);
        if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
            ::bind(listening, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw socketFailure("bind(127.0.0.1)");
        if (::listen(listening, 2) != 0) throw socketFailure("listen");
        if (!setNonblocking(listening, true)) throw socketFailure("setNonblocking(listener)");
        sockaddr_in local{};
#ifdef _WIN32
        int length = sizeof(local);
#else
        socklen_t length = sizeof(local);
#endif
        if (getsockname(listening, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
            local.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            throw socketFailure("getsockname(127.0.0.1)");
        listener_ = toHandle(listening);
        boundPort_.store(ntohs(local.sin_port));
        running_.store(true);
        for (std::size_t i = 0; i < kWorkers; ++i)
            workers_[i] = std::thread([this, i] { worker(i); });
    } catch (...) {
        running_.store(false);
        for (auto& thread : workers_) if (thread.joinable()) thread.join();
        listener_ = kInvalidSocket;
        boundPort_.store(0);
        closeSocket(listening);
#ifdef _WIN32
        WSACleanup();
        winsockInitialized_ = false;
#endif
        throw;
    }
}

void LoopbackHttpServer::stop() noexcept {
    std::lock_guard lifecycle(lifecycleMutex_);
    if (listener_ == kInvalidSocket) return;
    running_.store(false);
    {
        std::lock_guard active(activeMutex_);
        for (auto handle : activeSockets_) shutdownSocket(fromHandle(handle));
    }
    // Nonblocking listener + 100ms poll means no close/FD-reuse race with select.
    // A Handler must be finite and nonblocking; user code cannot be forcibly stopped.
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
    closeSocket(fromHandle(listener_));
    listener_ = kInvalidSocket;
    boundPort_.store(0);
#ifdef _WIN32
    if (winsockInitialized_) { WSACleanup(); winsockInitialized_ = false; }
#endif
}

bool LoopbackHttpServer::isRunning() const noexcept { return running_.load(); }
std::uint16_t LoopbackHttpServer::boundPort() const noexcept { return boundPort_.load(); }

void LoopbackHttpServer::worker(std::size_t slot) noexcept {
    try {
        const Socket listener = fromHandle(listener_);
        while (running_.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener, &readSet);
            timeval timeout{0, static_cast<long>(kPollInterval.count() * 1000)};
#ifdef _WIN32
            const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int ready = ::select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (!running_.load()) break;
            if (ready <= 0) continue;
            sockaddr_in peer{};
#ifdef _WIN32
            int peerLength = sizeof(peer);
#else
            socklen_t peerLength = sizeof(peer);
#endif
            Socket client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (client == kInvalid) continue; // another worker won the select/accept race
            if (!running_.load() || peer.sin_family != AF_INET ||
                peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                !setNonblocking(client, false) || !setTimeouts(client)) {
                closeSocket(client);
                continue;
            }
            {
                std::lock_guard active(activeMutex_);
                if (!running_.load()) { closeSocket(client); break; }
                activeSockets_[slot] = toHandle(client);
            }
            handleClient(toHandle(client));
            {
                std::lock_guard active(activeMutex_);
                closeSocket(client);
                activeSockets_[slot] = kInvalidSocket;
            }
        }
    } catch (...) {
        // Isolated worker failure must not terminate the host application.
    }
}

void LoopbackHttpServer::handleClient(std::uintptr_t handle) noexcept {
    const Socket client = fromHandle(handle);
    try {
        std::string header;
        header.reserve(1024);
        std::array<char, 1024> chunk{};
        const auto deadline = Clock::now() + kReadDeadline;
        while (running_.load() && Clock::now() < deadline) {
            // Never read more than the bounded header storage.
            if (header.size() == kMaxHeaderBytes) { sendResponse(client, failure(431)); return; }
            const auto count = std::min<std::size_t>(chunk.size(), kMaxHeaderBytes - header.size());
#ifdef _WIN32
            const int received = ::recv(client, chunk.data(), static_cast<int>(count), 0);
#else
            const ssize_t received = ::recv(client, chunk.data(), count, 0);
#endif
            if (received == 0) { sendResponse(client, failure(400)); return; }
            if (received < 0) {
                if (isRetryable(socketErrorCode())) continue;
                return; // client disconnected or stop() shutdown
            }
            header.append(chunk.data(), static_cast<std::size_t>(received));
            // Reject LF-only and mixed line endings without waiting for a
            // terminator. A final CR is allowed until the next recv chunk.
            for (std::size_t i = 0; i < header.size(); ++i) {
                if ((header[i] == '\n' && (i == 0 || header[i - 1] != '\r')) ||
                    (header[i] == '\r' && i + 1 < header.size() && header[i + 1] != '\n')) {
                    sendResponse(client, failure(400));
                    return;
                }
            }
            const auto end = header.find("\r\n\r\n");
            if (end != std::string::npos) {
                HttpRequest request;
                // A pipelined message or body delivered in the same read is
                // rejected, never forwarded to the handler.
                const int status = end + 4 == header.size()
                    ? parseRequest(header, request) : 400;
                if (status != 200) { sendResponse(client, failure(status)); return; }
                try {
                    sendResponse(client, handler_(request));
                } catch (...) {
                    sendResponse(client, failure(500));
                }
                return;
            }
        }
        if (running_.load()) sendResponse(client, failure(408));
    } catch (...) {
        if (running_.load()) sendResponse(client, failure(500));
    }
}

} // namespace reg::diagnostics::api::transport
