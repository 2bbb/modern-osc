#include "check.hpp"
#include <modern_osc/dispatch.hpp>

#include <stdexcept>

using namespace modern_osc;

namespace {

DispatchReport deliver(Dispatcher<int>& dispatcher, const Packet& message) {
    auto bytes = encode(message);
    CHECK(bytes);
    auto packet = decode({bytes->data(), bytes->size()});
    CHECK(packet);
    auto report = dispatcher.dispatch_packet(*packet, 17, BundlePolicy::immediate);
    CHECK(report);
    return *report;
}

void shapes() {
    Dispatcher<int> d;
    const Argument f{Float32::from_value(0.5f)};
    const Argument id{std::int32_t{9}};
    const Argument empty{Argument::Array{}};
    const Argument pair{Argument::Array{f, f}};
    std::int32_t number = 1;
    std::vector<float> xs{2}, ys{3};
    float gain = 5;
    CHECK(d.subscribe("/flat", number, xs));
    CHECK(d.subscribe("/curve", array(xs), array(ys)));
    CHECK(d.subscribe("/voice", number, array(xs), gain));
    CHECK(deliver(d, Packet{Message{"/flat", {id, f, f}}}).calls == 1);
    CHECK(number == 9 && xs == std::vector<float>({0.5f, 0.5f}));
    CHECK(deliver(d, Packet{Message{"/flat", {id, pair}}}).mismatched == 1);
    CHECK(xs.size() == 2);
    CHECK(deliver(d, Packet{Message{"/flat", {id}}}).calls == 1 && xs.empty());
    CHECK(deliver(d, Packet{Message{"/curve", {empty, pair}}}).calls == 1);
    CHECK(xs.empty() && ys.size() == 2);
    CHECK(deliver(d, Packet{Message{"/curve", {pair}}}).mismatched == 1);
    CHECK(xs.empty() && ys.size() == 2);
    CHECK(deliver(d, Packet{Message{"/curve", {pair, Argument{Argument::Array{pair}}}}}).mismatched == 1);
    CHECK(xs.empty());
    CHECK(deliver(d, Packet{Message{"/voice", {id, pair, f}}}).calls == 1);
    CHECK(gain == 0.5f && xs.size() == 2);
    number = 4;
    CHECK(deliver(d, Packet{Message{"/voice", {id, empty, f, id}}}).mismatched == 1);
    CHECK(number == 4 && xs.size() == 2);
}

void values() {
    Dispatcher<int> d;
    std::string name;
    Symbol symbol;
    Blob data;
    std::vector<bool> flags;
    CHECK(d.subscribe("/values", name, symbol, data, flags));
    CHECK(deliver(d, Packet{Message{"/values", {{std::string("key")}, {Symbol{"abc"}}, {Blob{{1, 2}}}, {true}, {false}}}}).calls == 1);
    CHECK(name == "key" && symbol.value == "abc" && data.value == std::vector<std::uint8_t>({1, 2}));
    CHECK(flags == std::vector<bool>({true, false}));
    CHECK(deliver(d, Packet{Message{"/values", {{Symbol{"key"}}, {Symbol{"next"}}, {Blob{}}, {false}}}}).mismatched == 1);
    CHECK(symbol.value == "abc" && flags.size() == 2);

    std::int64_t count{};
    double duration{};
    Float32 raw_f{};
    Float64 raw_d{};
    Timetag stamp{};
    Character ch{};
    Rgba color{};
    Midi midi{};
    Nil nil;
    CHECK(d.subscribe_notify("/types", [](const int&) {}, count, duration, raw_f, raw_d,
                             stamp, ch, color, midi, nil));
    CHECK(deliver(d, Packet{Message{"/types", {{std::int64_t{-4}}, {Float64::from_value(1.25)},
        {Float32{0x80000000}}, {Float64{0x7ff8000000001234}}, {Timetag{42}},
        {Character{65}}, {Rgba{{1, 2, 3, 4}}}, {Midi{{0, 144, 60, 100}}}, {Nil{}}}}}).calls == 1);
    CHECK(count == -4 && duration == 1.25 && raw_f.bits == 0x80000000 &&
          raw_d.bits == 0x7ff8000000001234 && stamp.bits == 42 &&
          ch.value == 65 && color.bytes[3] == 4 && midi.bytes[2] == 60);
}

void notifications() {
    Dispatcher<int> d;
    std::int32_t value = 0;
    std::vector<int> sequence;
    auto id = d.subscribe_notify("/n", [&](const int& context) {
        CHECK(context == 17);
        sequence.push_back(value);
    }, value);
    CHECK(id && d.add("/n", [&](const MessageView&, const int&) { sequence.push_back(-1); }));
    CHECK(d.subscribe("/also", value));
    CHECK(d.subscribe_notify("/trigger", [&](const int&) { sequence.push_back(0); }));
    Impulse impulse;
    CHECK(d.subscribe_notify("/bang", [&](const int&) { sequence.push_back(3); }, impulse));
    auto n = Packet{Message{"/n", {{std::int32_t{1}}}}};
    auto report = deliver(d, Packet{Bundle{{1}, {n, n, Packet{Message{"/trigger", {}}},
        Packet{Message{"/trigger", {{false}}}}, Packet{Message{"/bang", {{Impulse{}}}}},
        Packet{Message{"/n", {{false}}}}, Packet{Message{"/unknown", {}}},
        Packet{Message{"/also", {{std::int32_t{2}}}}}}}});
    CHECK(sequence == std::vector<int>({1, -1, 1, -1, 0, 3, -1}));
    CHECK(value == 2 && report.calls == 8 && report.unmatched == 1 && report.mismatched == 2);
    CHECK(d.remove(*id).value());
    CHECK(deliver(d, n).calls == 1 && value == 2);
    CHECK(d.subscribe_notify("/throw", [](const int&) { throw std::runtime_error("notify"); }, value));
    CHECK(d.add("/throw", [](const MessageView&, const int&) { CHECK(false); }));
    try {
        deliver(d, Packet{Message{"/throw", {{std::int32_t{11}}}}});
        CHECK(false);
    } catch (const std::runtime_error&) {
        CHECK(value == 11);
    }
}
} // namespace

int main() {
    shapes();
    values();
    notifications();
}
