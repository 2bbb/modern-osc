#include "modern_osc/dispatch.hpp"
#include "check.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace modern_osc;

namespace {

ByteView view(const std::vector<std::uint8_t>& bytes) { return {bytes.data(), bytes.size()}; }

Packet wire_fixture() {
    return {Message{"/x", {
        {std::int32_t{-2}}, {Float32{0x8000'0000}}, {std::string{"a"}}, {Blob{{0x81, 0, 0xff}}},
        {std::int64_t{-2}}, {Timetag{0x0102'0304'0506'0708}}, {Float64{0x7ff8'0000'0000'0042}},
        {Symbol{"sym"}}, {Character{'A'}}, {Rgba{{1, 2, 3, 4}}}, {Midi{{5, 0x90, 60, 100}}},
        {true}, {false}, {Nil{}}, {Impulse{}},
        {Argument::Array{{std::int32_t{7}}, {Argument::Array{{false}}}}}, {Argument::Array{}}
    }}};
}

void codec() {
    const std::vector<std::uint8_t> golden{
        '/', 'x', 0, 0,
        ',', 'i', 'f', 's', 'b', 'h', 't', 'd', 'S', 'c', 'r', 'm',
        'T', 'F', 'N', 'I', '[', 'i', '[', 'F', ']', ']', '[', ']', 0, 0, 0, 0,
        0xff, 0xff, 0xff, 0xfe,
        0x80, 0, 0, 0,
        'a', 0, 0, 0,
        0, 0, 0, 3, 0x81, 0, 0xff, 0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
        1, 2, 3, 4, 5, 6, 7, 8,
        0x7f, 0xf8, 0, 0, 0, 0, 0, 0x42,
        's', 'y', 'm', 0,
        0, 0, 0, 'A',
        1, 2, 3, 4,
        5, 0x90, 60, 100,
        0, 0, 0, 7
    };

    const Packet packet = wire_fixture();
    auto bytes = encode(packet);
    CHECK(bytes && *bytes == golden);
    auto size = encoded_size(packet);
    CHECK(size && *size == golden.size());
    CHECK(Float32::from_value(-0.0f).bits == 0x8000'0000);
    CHECK(Float64::from_value(1.0).bits == 0x3ff0'0000'0000'0000);
    CHECK(Float32{0x3f80'0000}.value() == 1.0f);
    CHECK(Float64{0x3ff0'0000'0000'0000}.value() == 1.0);

    for (std::size_t offset = 1; offset <= 3; ++offset) {
        std::vector<std::uint8_t> unaligned(offset + golden.size());
        std::copy(golden.begin(), golden.end(), unaligned.data() + offset);
        auto decoded = decode({unaligned.data() + offset, golden.size()});
        CHECK(decoded);
        const auto& args = std::get<MessageView>(decoded->value).arguments;
        CHECK(std::get<std::int32_t>(args[0].value) == -2);
        CHECK(std::get<Float32>(args[1].value).bits == 0x8000'0000);
        CHECK(std::get<std::int64_t>(args[4].value) == -2);
        CHECK(std::get<Float64>(args[6].value).bits == 0x7ff8'0000'0000'0042);
        auto roundtrip = encode(to_owned(*decoded));
        CHECK(roundtrip && *roundtrip == golden);
    }

    std::vector<std::uint8_t> small(golden.size() - 1, 0x55);
    const auto insufficient = encode_into(packet, {small.data(), small.size()});
    CHECK(!insufficient && insufficient.error().code == ErrorCode::insufficient_buffer);
    CHECK(std::all_of(small.begin(), small.end(), [](auto byte) { return byte == 0x55; }));

    std::vector<std::uint8_t> padded{'/', 'x', 0, 1, ',', 0, 0, 0};
    auto bad = decode(view(padded));
    CHECK(!bad && bad.error().code == ErrorCode::invalid_padding && bad.error().offset == 3);
    const std::vector<std::uint8_t> broken_array{'/', 'x', 0, 0, ',', '[', 'T', 0};
    bad = decode(view(broken_array));
    CHECK(!bad && bad.error().code == ErrorCode::invalid_tags);
    const std::vector<std::uint8_t> negative_blob{'/', 'x', 0, 0, ',', 'b', 0, 0,
                                                 0xff, 0xff, 0xff, 0xff};
    bad = decode(view(negative_blob));
    CHECK(!bad && bad.error().code == ErrorCode::invalid_size);
    CHECK(!decode({nullptr, 4}));

    const char utf8_text[] = {char(0xc3), char(0xa9)};
    const Packet unicode{Message{"/x", {{std::string(utf8_text, sizeof utf8_text)}}}};
    CHECK(!encode(unicode));
    EncodeOptions permissive;
    permissive.allow_non_ascii_strings = true;
    auto sent = encode(unicode, permissive);
    CHECK(sent);
    auto received = decode(view(*sent));
    CHECK(received);
    auto kept = encode(to_owned(*received), permissive);
    CHECK(kept && *kept == *sent);
    DecodeOptions strict;
    strict.strict_strings = true;
    bad = decode(view(*sent), strict);
    CHECK(!bad && bad.error().code == ErrorCode::invalid_string);

    const std::vector<std::uint8_t> legacy{'/', 'x', 0, 0, 0, 0, 0, 7};
    CHECK(!decode(view(legacy)));
    DecodeOptions old;
    old.allow_missing_tags = true;
    auto preserved = decode(view(legacy), old);
    CHECK(preserved && std::get<OpaqueMessageView>(preserved->value).reason == OpaqueReason::missing_tags);
    auto empty = decode({legacy.data(), 4}, old);
    CHECK(empty && std::get<MessageView>(empty->value).arguments.empty());
}

bool matches(std::string_view pattern, std::string_view address) {
    auto compiled = AddressPattern::compile(pattern);
    CHECK(compiled);
    std::size_t steps = 10000;
    auto matched = compiled->match(address, steps);
    CHECK(matched);
    return *matched;
}

void patterns() {
    CHECK(matches("/a/{one,two}[!0-2]?", "/a/two3x"));
    CHECK(!matches("/a/{one,two}[!0-2]?", "/a/one2x"));
    CHECK(matches("/a/*", "/a/b"));
    CHECK(!matches("/a/*", "/a/b/c"));
    CHECK(matches("/a//b", "/a/b"));
    CHECK(matches("/a//b", "/a/m/n/b"));
    CHECK(!matches("/a//b", "/ax/b"));
    CHECK(matches("//b", "/b"));
    CHECK(matches("/a//", "/a/b/c"));
    CHECK(!matches("/a//", "/a"));
    CHECK(matches("//", "/a"));
    CHECK(matches("/x:[a-]", "/x:-"));
    CHECK(!is_address("/a//b"));
    CHECK(!AddressPattern::compile("/a/{x,*}"));
    CHECK(!AddressPattern::compile("/a/[z-a]"));
    CHECK(!AddressPattern::compile("/a///b"));
    CHECK(!AddressPattern::compile("/a/"));
    CHECK(!AddressPattern::compile("/a/{x,}"));

    auto compiled = AddressPattern::compile("//*a*a*a*a*b");
    CHECK(compiled);
    const std::string long_address = "/" + std::string(1000, 'a');
    std::size_t steps = 2000;
    auto limited = compiled->match(long_address, steps);
    CHECK(!limited && limited.error().code == ErrorCode::dispatch_limit);
    steps = 100000;
    auto matched = compiled->match(long_address, steps);
    CHECK(matched && !*matched);
}

void dispatcher() {
    const std::vector<std::uint8_t> known{'/', 'x', 0, 0, ',', 0, 0, 0};
    auto opaque_wire = known;
    opaque_wire[5] = 'z';
    std::vector<std::uint8_t> bundle{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    bundle.insert(bundle.end(), {0, 0, 0, 8});
    bundle.insert(bundle.end(), opaque_wire.begin(), opaque_wire.end());
    bundle.insert(bundle.end(), {0, 0, 0, 8});
    bundle.insert(bundle.end(), known.begin(), known.end());
    auto decoded = decode(view(bundle));
    CHECK(decoded);
    const auto& elements = std::get<BundleView>(decoded->value).elements;
    CHECK(elements.size() == 2 && std::holds_alternative<OpaqueMessageView>(elements[0].value));

    Dispatcher<int> receiver;
    int calls = 0;
    auto id = receiver.add("/x", [&](const MessageView& message, const int& context) {
        CHECK(context == 77 && message.address == "/x");
        ++calls;
        auto rejected = receiver.remove(42);
        CHECK(!rejected && rejected.error().code == ErrorCode::busy);
        auto nested = receiver.dispatch(message, context);
        CHECK(!nested && nested.error().code == ErrorCode::busy);
    });
    CHECK(id);
    auto report = receiver.dispatch_packet(*decoded, 77, BundlePolicy::immediate);
    CHECK(report && report->calls == 1 && report->opaque == 1 && calls == 1);
    CHECK(receiver.remove(*id).value());

    const Packet nested{Bundle{{1}, {
        Packet{Message{"/x", {{std::int32_t{1}}}}},
        Packet{Bundle{{1000}, {Packet{Message{"/x", {{std::int32_t{2}}}}}}}},
        Packet{Message{"/not-registered", {}}}
    }}};
    auto encoded = encode(nested);
    CHECK(encoded);
    auto usable = decode(view(*encoded));
    CHECK(usable);
    auto rebuilt = encode(to_owned(*usable));
    CHECK(rebuilt && *rebuilt == *encoded);
    DecodeOptions shallow;
    shallow.limits.depth = 1;
    CHECK(!decode(view(*encoded), shallow));

    Dispatcher<int> ordered;
    std::vector<int> seen;
    CHECK(ordered.add("/x", [&](const MessageView& message, const int& context) {
        CHECK(context == 17);
        seen.push_back(std::get<std::int32_t>(message.arguments.at(0).value));
    }));
    CHECK(ordered.add("/x", [&](const MessageView&, const int&) { seen.push_back(0); }));
    auto routed = ordered.dispatch_packet(*usable, 17, BundlePolicy::immediate);
    CHECK(routed && routed->calls == 4 && routed->unmatched == 1);
    CHECK((seen == std::vector<int>{1, 0, 2, 0}));

    DispatchLimits limits;
    limits.steps = 15;
    Dispatcher<> bounded(limits);
    CHECK(bounded.add("/x", [&](const MessageView&, const NoContext&) { ++calls; }));
    auto over = bounded.dispatch_packet(*usable, {}, BundlePolicy::immediate);
    CHECK(!over && over.error().code == ErrorCode::dispatch_limit && calls == 1);

    Dispatcher<> throwing;
    CHECK(throwing.add("/x", [](const MessageView&, const NoContext&) { throw std::runtime_error("callback"); }));
    CHECK(throwing.add("/x", [&](const MessageView&, const NoContext&) { ++calls; }));
    try {
        throwing.dispatch_packet(*usable, {}, BundlePolicy::immediate);
        CHECK(false);
    } catch (const std::runtime_error&) {
        CHECK(calls == 1);
    }
    CHECK(throwing.remove(1).value());
}

void mixer_scene() {
    Dispatcher<> dispatcher;
    Bundle scene{Timetag::immediate(), {}};
    int next = 100;
    for (int channel = 100; channel < 612; ++channel) {
        auto address = "/ch/" + std::to_string(channel) + "/gain";
        CHECK(dispatcher.add(address, [&, channel](const MessageView& message, const NoContext&) {
            CHECK(next == channel);
            CHECK(std::get<std::int32_t>(message.arguments.front().value) == channel);
            ++next;
        }));
        scene.elements.push_back(Packet{Message{std::move(address), {{std::int32_t{channel}}}}});
    }
    auto wire = encode(Packet{std::move(scene)});
    CHECK(wire && wire->size() == 14352);
    auto packet = decode(view(*wire));
    CHECK(packet);
    auto result = dispatcher.dispatch_packet(*packet, {}, BundlePolicy::immediate);
    CHECK(result && result->calls == 512 && result->unmatched == 0 && next == 612);
}

} // namespace

int main() {
    codec();
    patterns();
    dispatcher();
    mixer_scene();
    std::cout << "OSC wire conformance, patterns and dispatch passed\n";
}
