#pragma once

#include "modern_osc/packet.hpp"

#include <chrono>

namespace modern_osc {

constexpr std::size_t ipv4_udp_payload_limit = 65'507;

struct Endpoint {
    std::array<std::uint8_t, 4> address;
    std::uint16_t port;
};

enum class ReceiveStatus { datagram, would_block, truncated, peer_unreachable };
enum class SendStatus { sent, would_block, peer_unreachable };
enum class WaitStatus { readable, timed_out };

struct ReceiveResult {
    ReceiveStatus status;
    ByteView bytes;
    Endpoint source;
};

class UdpSocket {
public:
    static Result<UdpSocket> bind(Endpoint local);
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    ~UdpSocket();

    // The caller serializes socket access and provides the receive storage.
    std::uint16_t local_port() const noexcept { return local_port_; }
    Result<WaitStatus> wait_readable(std::chrono::milliseconds timeout) const;
    Result<ReceiveResult> receive_into(MutableBytes output) const;
    Result<SendStatus> send_to(Endpoint remote, ByteView bytes) const;

private:
    static constexpr std::uintptr_t invalid_handle = UINTPTR_MAX;
    UdpSocket() = default;
    void close() noexcept;

    std::uintptr_t handle_ = invalid_handle;
    std::uint16_t local_port_ = 0;
};

} // namespace modern_osc
