#include <modern_osc/dispatch.hpp>
#ifdef MODERN_OSC_TEST_UDP
#include <modern_osc/udp.hpp>
#endif

int main() {
    using namespace modern_osc;
    const Packet input{Message{"/gain", {{Float32::from_value(0.5f)}}}};
    auto datagram = encode(input);
    if (!datagram) return 1;
    const auto packet = decode({datagram->data(), datagram->size()});
    if (!packet) return 1;

    // Demonstrate the callback boundary without linking the native UDP adapter.
    struct Peer { int id; };
    Dispatcher<Peer> dispatcher;
    bool handled = false;
    if (!dispatcher.add("/gain", [&](const MessageView& message, const Peer& peer) {
            handled = peer.id == 3 &&
                      std::get<Float32>(message.arguments.front().value).value() == 0.5f;
        })) return 1;
    auto dispatched = dispatcher.dispatch_packet(*packet, {3}, BundlePolicy::immediate);
    if (!dispatched || !handled) return 1;
#ifdef MODERN_OSC_TEST_UDP
    auto socket = UdpSocket::bind({{127, 0, 0, 1}, 0});
    if (!socket || socket->local_port() == 0) return 1;
#endif
}
