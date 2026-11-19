#include "modern_osc/dispatch.hpp"
#include "modern_osc/udp.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

using namespace modern_osc;
using namespace std::chrono_literals;

namespace {

bool parse(std::string_view text, unsigned& value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
    unsigned port = 0;
    unsigned limit = 1;
    if (argc > 3 || (argc > 1 && (!parse(argv[1], port) || port > 65535)) ||
        (argc > 2 && (!parse(argv[2], limit) || limit == 0))) {
        std::cerr << "usage: modern_osc_echo [port] [reply count]\n";
        return 2;
    }
    auto socket = UdpSocket::bind({{127, 0, 0, 1}, static_cast<std::uint16_t>(port)});
    if (!socket) {
        std::cerr << "bind failed: " << socket.error().native_code << '\n';
        return 1;
    }

    Dispatcher<Endpoint> dispatcher;
    unsigned replies = 0;
    bool send_failed = false;
    const auto added = dispatcher.add("/echo", [&](const MessageView& input, const Endpoint& from) {
        auto reply = to_owned(PacketView{input});
        std::get<Message>(reply.value).address = "/reply";
        EncodeOptions options;
        options.allow_non_ascii_strings = true;
        auto bytes = encode(reply, options);
        if (!bytes) {
            send_failed = true;
            return;
        }
        const auto sent = socket->send_to(from, {bytes->data(), bytes->size()});
        if (!sent || *sent != SendStatus::sent) {
            send_failed = true;
            return;
        }
        ++replies;
        std::cout << "replied " << replies << " to " << from.port << '\n';
    });
    if (!added) return 1;

    std::cout << "listening " << socket->local_port() << std::endl;
    std::vector<std::uint8_t> buffer(ipv4_udp_payload_limit);
    while (replies < limit && !send_failed) {
        const auto ready = socket->wait_readable(5s);
        if (!ready || *ready != WaitStatus::readable) {
            std::cerr << "receive wait failed or timed out\n";
            return 1;
        }
        const auto received = socket->receive_into({buffer.data(), buffer.size()});
        if (!received) {
            std::cerr << "receive failed: " << received.error().native_code << '\n';
            return 1;
        }
        if (received->status != ReceiveStatus::datagram) continue;
        const auto packet = decode(received->bytes);
        if (!packet) {
            std::cerr << "decode rejected packet at " << packet.error().offset << '\n';
            continue;
        }
        const auto report = dispatcher.dispatch_packet(*packet, received->source, BundlePolicy::immediate);
        if (!report) {
            std::cerr << "dispatch failed\n";
            return 1;
        }
    }
    return send_failed ? 1 : 0;
}
