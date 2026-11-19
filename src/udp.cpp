#include "modern_osc/udp.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace modern_osc {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket invalid_socket = INVALID_SOCKET;
int socket_error() { return WSAGetLastError(); }
bool would_block(int code) { return code == WSAEWOULDBLOCK; }
bool interrupted(int code) { return code == WSAEINTR; }
bool peer_unreachable(int code) { return code == WSAECONNRESET || code == WSAECONNREFUSED; }
#else
using Socket = int;
using AddressLength = socklen_t;
constexpr Socket invalid_socket = -1;
int socket_error() { return errno; }
bool would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK; }
bool interrupted(int code) { return code == EINTR; }
bool peer_unreachable(int code) { return code == ECONNREFUSED; }
#endif

Error system_error(int code) { return {ErrorCode::socket_error, 0, code}; }

sockaddr_in native_address(Endpoint endpoint) {
    const auto& ip = endpoint.address;
    const auto address = (std::uint32_t(ip[0]) << 24) | (std::uint32_t(ip[1]) << 16) |
                         (std::uint32_t(ip[2]) << 8) | std::uint32_t(ip[3]);
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_addr.s_addr = htonl(address);
    native.sin_port = htons(endpoint.port);
    return native;
}

Endpoint endpoint(const sockaddr_in& native) {
    const auto address = ntohl(native.sin_addr.s_addr);
    return {{{static_cast<std::uint8_t>(address >> 24), static_cast<std::uint8_t>(address >> 16),
              static_cast<std::uint8_t>(address >> 8), static_cast<std::uint8_t>(address)}},
            ntohs(native.sin_port)};
}

} // namespace

Result<UdpSocket> UdpSocket::bind(Endpoint local) {
#ifdef _WIN32
    WSADATA data{};
    if (const int status = WSAStartup(MAKEWORD(2, 2), &data)) return system_error(status);
#endif
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        const auto error = system_error(socket_error());
#ifdef _WIN32
        WSACleanup();
#endif
        return error;
    }

    UdpSocket bound;
    bound.handle_ = static_cast<std::uintptr_t>(socket);
#ifdef _WIN32
    unsigned long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return system_error(socket_error());
#else
    const auto flags = fcntl(socket, F_GETFL);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0)
        return system_error(socket_error());
    if (fcntl(socket, F_SETFD, FD_CLOEXEC) < 0) return system_error(socket_error());
#endif

    auto native = native_address(local);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) != 0)
        return system_error(socket_error());
    AddressLength size = sizeof(native);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&native), &size) != 0)
        return system_error(socket_error());
    bound.local_port_ = ntohs(native.sin_port);
    return bound;
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, invalid_handle)),
      local_port_(std::exchange(other.local_port_, 0)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle_ = std::exchange(other.handle_, invalid_handle);
    local_port_ = std::exchange(other.local_port_, 0);
    return *this;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() noexcept {
    if (handle_ == invalid_handle) return;
#ifdef _WIN32
    closesocket(static_cast<Socket>(handle_));
    WSACleanup();
#else
    ::close(static_cast<Socket>(handle_));
#endif
    handle_ = invalid_handle;
    local_port_ = 0;
}

Result<WaitStatus> UdpSocket::wait_readable(std::chrono::milliseconds timeout) const {
    if (handle_ == invalid_handle || timeout.count() < 0 ||
        timeout.count() > (std::numeric_limits<int>::max)()) return Error{ErrorCode::invalid_argument};

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = timeout;
    for (;;) {
#ifdef _WIN32
        WSAPOLLFD item{static_cast<Socket>(handle_), POLLRDNORM, 0};
        const auto status = WSAPoll(&item, 1, static_cast<int>(remaining.count()));
#else
        pollfd item{static_cast<Socket>(handle_), POLLIN, 0};
        const auto status = poll(&item, 1, static_cast<int>(remaining.count()));
#endif
        // An asynchronously reported network error also makes receive actionable.
        if (status > 0) return WaitStatus::readable;
        if (status == 0) return WaitStatus::timed_out;
        const auto code = socket_error();
        if (!interrupted(code)) return system_error(code);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return WaitStatus::timed_out;
        remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    }
}

Result<ReceiveResult> UdpSocket::receive_into(MutableBytes output) const {
    if (handle_ == invalid_handle) return Error{ErrorCode::invalid_argument};
    if (output.data == nullptr || output.size == 0) return Error{ErrorCode::invalid_buffer};
    const auto capacity = (std::min)(output.size, ipv4_udp_payload_limit);
    sockaddr_in source{};
#ifdef _WIN32
    AddressLength size = sizeof(source);
    const auto count = recvfrom(static_cast<Socket>(handle_), reinterpret_cast<char*>(output.data),
                                static_cast<int>(capacity), 0,
                                reinterpret_cast<sockaddr*>(&source), &size);
    if (count == SOCKET_ERROR && socket_error() == WSAEMSGSIZE)
        return ReceiveResult{ReceiveStatus::truncated, {}, {}};
#else
    iovec part{output.data, capacity};
    msghdr header{};
    header.msg_name = &source;
    header.msg_namelen = sizeof(source);
    header.msg_iov = &part;
    header.msg_iovlen = 1;
    const auto count = recvmsg(static_cast<Socket>(handle_), &header, 0);
    if (count >= 0 && (header.msg_flags & MSG_TRUNC) != 0)
        return ReceiveResult{ReceiveStatus::truncated, {}, {}};
#endif
    if (count < 0) {
        const auto code = socket_error();
        if (would_block(code) || interrupted(code))
            return ReceiveResult{ReceiveStatus::would_block, {}, {}};
        if (peer_unreachable(code))
            return ReceiveResult{ReceiveStatus::peer_unreachable, {}, {}};
        return system_error(code);
    }
    return ReceiveResult{ReceiveStatus::datagram, {output.data, static_cast<std::size_t>(count)},
                         endpoint(source)};
}

Result<SendStatus> UdpSocket::send_to(Endpoint remote, ByteView bytes) const {
    if (handle_ == invalid_handle) return Error{ErrorCode::invalid_argument};
    if (remote.port == 0) return Error{ErrorCode::invalid_endpoint};
    if (bytes.data == nullptr && bytes.size != 0) return Error{ErrorCode::invalid_buffer};
    if (bytes.size > ipv4_udp_payload_limit) return Error{ErrorCode::size_limit};
    const auto native = native_address(remote);
    const char empty = 0;
    const auto data = bytes.data ? reinterpret_cast<const char*>(bytes.data) : &empty;
#ifdef _WIN32
    const auto count = sendto(static_cast<Socket>(handle_), data, static_cast<int>(bytes.size), 0,
                              reinterpret_cast<const sockaddr*>(&native), sizeof(native));
#else
    const auto count = sendto(static_cast<Socket>(handle_), data, bytes.size, 0,
                              reinterpret_cast<const sockaddr*>(&native), sizeof(native));
#endif
    if (count < 0) {
        const auto code = socket_error();
        if (would_block(code) || interrupted(code)) return SendStatus::would_block;
        if (peer_unreachable(code)) return SendStatus::peer_unreachable;
        return system_error(code);
    }
    if (static_cast<std::size_t>(count) != bytes.size) return Error{ErrorCode::socket_error};
    return SendStatus::sent;
}

} // namespace modern_osc
