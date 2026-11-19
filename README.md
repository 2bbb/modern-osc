# modern-osc

[日本語](README_ja.md)

A C++17 OSC library with a standalone packet codec, sender-side address pattern dispatch, and an optional IPv4 UDP adapter.

## OSC support

- Encodes and decodes Message and Bundle packets, including nested Bundles and every argument tag listed in OSC 1.0.
- Matches OSC 1.0 address patterns and the multi-level `//` from the OSC 1.1 paper. The receiving dispatcher matches against registered concrete addresses.
- Preserves Bundle timetags and provides explicit immediate delivery in wire order. Applications can also inspect Bundle elements and arrange their own timing.
- Reads `s` and `S` arguments as received bytes by default. Encoding defaults to ASCII. Strict receive and compatibility send are separate options.

Protocol references. [OSC 1.0](https://opensoundcontrol.stanford.edu/spec-1_0.html) · [OSC 1.1 paper](https://opensoundcontrol.stanford.edu/files/2009-NIME-OSC-1.1.pdf)

## Build and use

Requirements are a C++17 compiler and CMake 3.16 or newer. The core uses the standard library. The optional adapter uses the operating system's sockets, including Winsock on Windows.

```sh
cmake -S . -B build -DMODERN_OSC_BUILD_TESTS=ON -DMODERN_OSC_BUILD_EXAMPLES=ON
cmake --build build --config Release
(cd build && ctest -C Release --output-on-failure)
cmake --install build --config Release --prefix "$PWD/install"
```

Use CMake's `add_subdirectory` to add a checkout, or set `CMAKE_PREFIX_PATH` to the installed prefix.

```cmake
find_package(modern_osc 0.1 CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_app PRIVATE modern_osc::core)
```

For the native IPv4 unicast adapter, request the `udp` component and link `modern_osc::udp`. A source consumer can set `MODERN_OSC_BUILD_UDP=OFF` before `add_subdirectory` or `FetchContent_MakeAvailable` when it already owns the transport.

## Send and receive

```cpp
#include <modern_osc/dispatch.hpp>

using namespace modern_osc;

auto make_gain_packet() {
    Packet packet{Message{"/deck/1/gain", {{Float32::from_value(0.5f)}}}};
    return encode(packet);
}

template<class Peer>
auto on_datagram(Dispatcher<Peer>& dispatcher, ByteView bytes, const Peer& sender)
    -> Result<DispatchReport>
{
    auto packet = decode(bytes);
    if (!packet) return packet.error();
    return dispatcher.dispatch_packet(*packet, sender, BundlePolicy::immediate);
}
```

Pass the output of `encode` or `encode_into` to your transport's send function. The receive callback supplies one complete UDP datagram and the transport's peer type. Register handlers with `dispatcher.add("/deck/1/gain", handler)`. A handler takes a `const MessageView&` and a `const Peer&`. [Native UDP echo example](examples/echo.cpp)

Views borrow the receive buffer. Keep it unchanged through synchronous dispatch, and copy values you need later. Keep registration and dispatch serialized per Dispatcher. A dispatch budget failure calls zero handlers. If a handler throws, the exception propagates and the remaining handlers are skipped.

The default dispatch budget is one million evaluation steps per packet. Concrete addresses use an indexed lookup. Patterns spend budget on the registered addresses and matching states they visit. Tune `DispatchLimits` for workloads that fan out widely.

For APIs that do not signal UDP truncation, use a receive path sized for the whole datagram. For ordinary IPv4 UDP that means at least 65,507 payload bytes. [UDP usage guidelines](https://www.rfc-editor.org/rfc/rfc8085.html)

## Development

`MODERN_OSC_BUILD_FUZZER=ON` builds a reproducible seed-mutation executable using Clang with AddressSanitizer and UndefinedBehaviorSanitizer. The [design plan](PLAN.md) lists support, trade-offs, library comparisons, and the verification scope.

## License

This project is licensed under the [MIT License](LICENSE).
