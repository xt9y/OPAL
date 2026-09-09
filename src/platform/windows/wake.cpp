#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/wake.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace opal {
namespace {

bool winsock_ready()
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

SOCKET tcp_socket()
{
    if (!winsock_ready()) return INVALID_SOCKET;
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
}

SOCKET udp_socket()
{
    if (!winsock_ready()) return INVALID_SOCKET;
    return WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
}

void socket_deadlines(SOCKET socket, int seconds = 3)
{
    const DWORD timeout = static_cast<DWORD>(std::max(0, seconds) * 1000);
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

bool connect_deadline(SOCKET socket, const sockaddr_in& address, int timeout_ms)
{
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
    int rc = connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (rc == 0) {
        nonblocking = 0;
        (void)ioctlsocket(socket, FIONBIO, &nonblocking);
        return true;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        nonblocking = 0;
        (void)ioctlsocket(socket, FIONBIO, &nonblocking);
        return false;
    }

    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLWRNORM;
    rc = WSAPoll(&descriptor, 1, std::max(0, timeout_ms));
    int error = 0;
    int length = sizeof(error);
    const bool ok = rc > 0 && !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
                    getsockopt(socket, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
    nonblocking = 0;
    (void)ioctlsocket(socket, FIONBIO, &nonblocking);
    return ok;
}

bool send_all(SOCKET socket, const std::string& text)
{
    std::size_t offset = 0;
    while (offset < text.size()) {
        const int amount = static_cast<int>(std::min<std::size_t>(text.size() - offset, INT_MAX));
        const int written = send(socket, text.data() + offset, amount, 0);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written == SOCKET_ERROR && WSAGetLastError() == WSAEINTR) continue;
        return false;
    }
    return true;
}

void bridge_peer(SOCKET client, const std::string& secret, const std::string& mac)
{
    socket_deadlines(client);
    const auto nonce = random_hex(24);
    if (!send_all(client, nonce + "\n")) {
        closesocket(client);
        return;
    }

    char buffer[512]{};
    int received = 0;
    do {
        received = recv(client, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
    } while (received == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);

    const std::string proof = trim(std::string(buffer, received > 0 ? static_cast<std::size_t>(received) : 0));
    if (received > 0 && secure_equal(proof, hmac_sha256_hex(secret, nonce))) {
        const bool ok = send_wol(mac);
        (void)send_all(client, ok ? "OK\n" : "ERR\n");
    } else {
        (void)send_all(client, "DENY\n");
    }
    closesocket(client);
}

bool remote_wake(const std::string& address, std::uint16_t port, const std::string& secret)
{
    if (secret.empty()) return false;
    const auto colon = address.rfind(':');
    const std::string host = colon == std::string::npos ? address : address.substr(0, colon);
    if (colon != std::string::npos) {
        try {
            const int parsed = std::stoi(address.substr(colon + 1));
            if (parsed < 1 || parsed > 65535) return false;
            port = static_cast<std::uint16_t>(parsed);
        } catch (...) {
            return false;
        }
    }

    SOCKET socket = tcp_socket();
    if (socket == INVALID_SOCKET) return false;
    socket_deadlines(socket);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (InetPtonA(AF_INET, host.c_str(), &target.sin_addr) != 1 || !connect_deadline(socket, target, 3000)) {
        closesocket(socket);
        return false;
    }

    char buffer[256]{};
    int received = 0;
    do {
        received = recv(socket, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
    } while (received == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
    if (received <= 0) {
        closesocket(socket);
        return false;
    }

    const auto nonce = trim(std::string(buffer, static_cast<std::size_t>(received)));
    const auto proof = hmac_sha256_hex(secret, nonce) + "\n";
    if (!send_all(socket, proof)) {
        closesocket(socket);
        return false;
    }
    do {
        received = recv(socket, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
    } while (received == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
    closesocket(socket);
    return received > 0 && std::string(buffer, static_cast<std::size_t>(received)).rfind("OK", 0) == 0;
}

}

std::vector<std::uint8_t> wol_packet(const std::string& mac)
{
    std::array<unsigned, 6> bytes{};
    char separator = 0;
    std::istringstream input(mac);
    for (int i = 0; i < 6; ++i) {
        if (!(input >> std::hex >> bytes[static_cast<std::size_t>(i)]) || bytes[static_cast<std::size_t>(i)] > 0xff)
            return {};
        if (i < 5 && (!(input >> separator) || separator != ':')) return {};
    }
    std::vector<std::uint8_t> packet(102, 0xff);
    for (int repeat = 0; repeat < 16; ++repeat)
        for (int i = 0; i < 6; ++i)
            packet[6 + repeat * 6 + i] = static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(i)]);
    return packet;
}

bool send_wol(const std::string& mac, const std::string& broadcast, std::uint16_t port)
{
    const auto packet = wol_packet(mac);
    if (packet.empty()) return false;
    SOCKET socket = udp_socket();
    if (socket == INVALID_SOCKET) return false;
    int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
        closesocket(socket);
        return false;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (InetPtonA(AF_INET, broadcast.c_str(), &target.sin_addr) != 1) {
        closesocket(socket);
        return false;
    }
    const int written = sendto(socket, reinterpret_cast<const char*>(packet.data()),
                               static_cast<int>(packet.size()), 0,
                               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    closesocket(socket);
    return written == static_cast<int>(packet.size());
}

int run_bridge(std::uint16_t port)
{
    const auto paths = Paths::load();
    (void)ensure_layout(paths);
    Ini config;
    if (!config.load(paths.root / "bridge.ini")) {
        std::cerr << "bridge not configured; run: opal bridge setup --mac XX:XX:XX:XX:XX:XX\n";
        return 2;
    }
    const auto secret = config.get("bridge", "secret");
    const auto mac = config.get("bridge", "mac");
    if (secret.size() < 32 || wol_packet(mac).empty()) {
        std::cerr << "bridge configuration invalid\n";
        return 2;
    }

    SOCKET listener = tcp_socket();
    if (listener == INVALID_SOCKET) return 1;
    int one = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 16) == SOCKET_ERROR) {
        closesocket(listener);
        return 1;
    }

    std::cout << "OPAL wake bridge listening on " << port << '\n';
    auto worker = [&] {
        for (;;) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (WSAGetLastError() == WSAEINTR) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            bridge_peer(client, secret, mac);
        }
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back(worker);
    for (auto& worker_thread : workers) worker_thread.join();
    return 0;
}

int wake_named(const std::string& name)
{
    const auto paths = Paths::load();
    Ini hosts;
    if (!hosts.load(paths.hosts)) {
        std::cerr << "no saved hosts\n";
        return 2;
    }
    const auto mac = hosts.get(name, "mac");
    const auto bridge = hosts.get(name, "wake_bridge");
    const auto secret = hosts.get(name, "wake_secret");
    if (mac.empty()) {
        std::cerr << "host has no MAC configured\n";
        return 2;
    }
    const bool ok = bridge.empty() ? send_wol(mac) : remote_wake(bridge, 47992, secret);
    std::cout << (ok ? "wake request sent\n" : "wake failed\n");
    return ok ? 0 : 1;
}

}
