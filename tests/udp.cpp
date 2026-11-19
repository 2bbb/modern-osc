#include "modern_osc/dispatch.hpp"
#include "modern_osc/udp.hpp"
#include "check.hpp"

#include <array>

using namespace modern_osc;
using namespace std::chrono_literals;

int main() {
    const Endpoint loopback{{127, 0, 0, 1}, 0};
    auto receiver = UdpSocket::bind(loopback);
    auto sender = UdpSocket::bind(loopback);
    CHECK(receiver && sender && receiver->local_port() && sender->local_port());
    auto destination = loopback;
    destination.port = receiver->local_port();

    std::array<std::uint8_t, 256> buffer{};
    CHECK(receiver->wait_readable(5ms).value() == WaitStatus::timed_out);
    CHECK(receiver->receive_into({buffer.data(), buffer.size()})->status == ReceiveStatus::would_block);

    const Packet packet{Bundle{Timetag::immediate(), {Packet{Message{"/x", {{std::int32_t{42}}}}}}}};
    auto bytes = encode(packet);
    CHECK(bytes);
    const ByteView data{bytes->data(), bytes->size()};
    CHECK(sender->send_to(destination, data).value() == SendStatus::sent);
    CHECK(receiver->wait_readable(1000ms).value() == WaitStatus::readable);

    // The first 16 bytes of this datagram look like a valid empty Bundle.
    auto truncated = receiver->receive_into({buffer.data(), 16});
    CHECK(truncated && truncated->status == ReceiveStatus::truncated && truncated->bytes.size == 0);
    CHECK(decode({buffer.data(), 16}));
    CHECK(sender->send_to(destination, data).value() == SendStatus::sent);
    CHECK(receiver->wait_readable(1000ms).value() == WaitStatus::readable);
    auto arrived = receiver->receive_into({buffer.data(), buffer.size()});
    CHECK(arrived && arrived->status == ReceiveStatus::datagram && arrived->bytes.size == data.size);
    CHECK(arrived->source.port == sender->local_port() && arrived->source.address == loopback.address);
    auto incoming = decode(arrived->bytes);
    CHECK(incoming);

    Dispatcher<Endpoint> dispatcher;
    CHECK(dispatcher.add("/x", [&](const MessageView&, const Endpoint& source) {
        CHECK(receiver->send_to(source, data).value() == SendStatus::sent);
    }));
    auto reported = dispatcher.dispatch_packet(*incoming, arrived->source, BundlePolicy::immediate);
    CHECK(reported && reported->calls == 1);
    CHECK(sender->wait_readable(1000ms).value() == WaitStatus::readable);
    auto reply = sender->receive_into({buffer.data(), buffer.size()});
    CHECK(reply && reply->status == ReceiveStatus::datagram && reply->source.port == receiver->local_port());
    CHECK(decode(reply->bytes));

#ifdef _WIN32
    auto closed = loopback;
    {
        auto temporary = UdpSocket::bind(loopback);
        CHECK(temporary);
        closed.port = temporary->local_port();
    }
    CHECK(receiver->send_to(closed, data).value() == SendStatus::sent);
    if (receiver->wait_readable(100ms).value() == WaitStatus::readable) {
        auto reset = receiver->receive_into({buffer.data(), buffer.size()});
        CHECK(reset && reset->status == ReceiveStatus::peer_unreachable);
    }
    CHECK(sender->send_to(destination, data).value() == SendStatus::sent);
    CHECK(receiver->wait_readable(1000ms).value() == WaitStatus::readable);
    auto next = receiver->receive_into({buffer.data(), buffer.size()});
    CHECK(next && next->status == ReceiveStatus::datagram);
#endif
    std::cout << "UDP loopback, truncation and sender reply passed\n";
}
