#include "diagnostics/api/runtime/DiagnosticRuntime.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {
using reg::diagnostics::api::runtime::DiagnosticRuntime;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string httpGet(std::uint16_t port, std::string_view path) {
#ifdef _WIN32
    using NativeSocket = SOCKET;
    const auto invalid = INVALID_SOCKET;
#else
    using NativeSocket = int;
    const auto invalid = -1;
#endif
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket != invalid, "socket creation");
    const auto close = [socket] {
#ifdef _WIN32
        (void)closesocket(socket);
#else
        (void)::close(socket);
#endif
    };
#ifdef _WIN32
    DWORD timeoutMs = 1500;
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout{1, 500000};
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(port);
    (void)inet_pton(AF_INET, "127.0.0.1", &remote.sin_addr);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0) {
        close();
        throw std::runtime_error("loopback connect");
    }
    const std::string request = "GET " + std::string(path) +
        " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
#ifdef _WIN32
    const int sent = ::send(socket, request.data(), static_cast<int>(request.size()), 0);
#else
    const auto sent = ::send(socket, request.data(), request.size(), 0);
#endif
    if (sent != static_cast<decltype(sent)>(request.size())) {
        close();
        throw std::runtime_error("loopback send");
    }
    std::string response;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
        const int count = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const auto count = ::recv(socket, buffer, sizeof(buffer), 0);
#endif
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
        require(response.size() < 1024U * 1024U, "unexpected oversized response");
    }
    close();
    return response;
}
} // namespace

int main() {
    try {
        reg::telemetry::TelemetryModel model{5min, 250ms, 16};
        DiagnosticRuntime service(model, 0, 100ms);
        require(!service.running() && service.boundPort() == 0, "default opt-in service disabled");
        require(service.respond({"GET", "/healthz"}).statusCode == 200, "healthz is listener-only");
        require(service.respond({"GET", "/v1/snapshot"}).statusCode == 503,
                "no timed telemetry measurement must return 503");

        const auto t0 = std::chrono::steady_clock::now() - 300ms;
        reg::telemetry::Counters counters{};
        counters.rtspConnected = true;
        counters.decoderSessions = 1;
        counters.videoSignalPresent = true;
        counters.decodedFrames = 42;
        counters.rawPresentedFrames = 41;
        counters.metadataPackets = 3;
        counters.exactSync.sessionGeneration = 7;
        counters.exactSync.matchedPairs = 1;
        model.updateCounters(counters, t0);

        service.sampleOnce(std::chrono::steady_clock::now());
        const auto direct = service.respond({"GET", "/v1/snapshot?sections=rtsp,video,cv,exact_sync"});
        require(direct.statusCode == 200, "sampled Reg snapshot");
        require(direct.body.find("\"data_origin\":\"live\"") != std::string::npos,
                "no test data masked as live");
        require(direct.body.find("\"counter\":\"42\"") != std::string::npos,
                "real decoder count must be returned");
        require(direct.body.find("\"reset_id\":null") == std::string::npos,
                "cross-agent reset identity must survive JSON");
        require(direct.body.find("decoder.decoded_frames_total") != std::string::npos,
                "real telemetry adapter missing");

        service.start();
        require(service.running() && service.boundPort() != 0, "explicit HTTP start");
        const auto health = httpGet(service.boundPort(), "/healthz");
        require(health.starts_with("HTTP/1.1 200 OK\r\n"), "HTTP loopback health");
        require(health.find("\"scope\":\"listener_only\"") != std::string::npos,
                "HTTP health cannot imply camera health");
        const auto snapshot = httpGet(service.boundPort(), "/v1/snapshot?sections=video,cv");
        require(snapshot.starts_with("HTTP/1.1 200 OK\r\n"), "HTTP live snapshot");
        require(snapshot.find("decoder.decoded_frames_total") != std::string::npos &&
                snapshot.find("\"counter\":\"42\"") != std::string::npos,
                "end-to-end counter over real TCP");
        require(snapshot.find("\"data_origin\":\"live\"") != std::string::npos,
                "actual HTTP snapshot provenance");
        const auto rejected = httpGet(service.boundPort(), "/v1/snapshot?sections=video,invalid");
        require(rejected.starts_with("HTTP/1.1 400 Bad Request\r\n"), "HTTP codec filter");

        service.stop();
        service.stop();
        require(!service.running() && service.boundPort() == 0, "idempotent runtime shutdown");
        std::cout << "diagnostic_runtime_v1_tests: PASS\n";
    } catch (const std::exception& e) {
        std::cerr << "diagnostic_runtime_v1_tests: FAIL: " << e.what() << '\n';
        return 1;
    }
}
