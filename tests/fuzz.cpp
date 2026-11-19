#include "modern_osc/pattern.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>

namespace {
using namespace modern_osc;
using Buffer = std::vector<std::uint8_t>;

void exercise_packet(ByteView input) {
    const auto decoded = decode(input);
    if (!decoded) return;
    const auto encoded = encode(to_owned(*decoded), {{}, true});
    if (!encoded && encoded.error().code == ErrorCode::opaque_message) return;
    if (!encoded || encoded->size() != input.size ||
        !std::equal(encoded->begin(), encoded->end(), input.data)) std::abort();
}

void exercise_pattern(ByteView input) {
    if (input.size == 0) return;
    const auto pattern = AddressPattern::compile(
        {reinterpret_cast<const char*>(input.data), std::min<std::size_t>(input.size, 1024)});
    if (pattern) {
        std::size_t steps = 5000;
        pattern->match("/a/level/mixer/cue/123", steps);
    }
}

Buffer bytes(std::string_view value) { return {value.begin(), value.end()}; }

} // namespace

int main() {
    const Packet fixture{Bundle{{1}, {Packet{Message{
        "/a/{left,right}//level",
        {{Argument::Array{{Float32{0x8000'0000}}, {false}, {Blob{{0, 1, 255}}}}},
         {Float64{0x7ff8'0000'0000'0042}}}
    }}}}};
    auto wire = encode(fixture);
    if (!wire) return 1;
    Packet deep{Message{"/deep", {}}};
    for (unsigned i = 0; i < 32; ++i) deep = Packet{Bundle{{1}, {std::move(deep)}}};
    auto depth = encode(deep);
    if (!depth) return 1;

    const std::vector<Buffer> seeds{
        *wire, *depth, {'/', 'x', 0, 0, ',', 'b', 0, 0, 0xff, 0xff, 0xff, 0xff},
        bytes("/a//{left,right}/[!1-5]?"), bytes("//*a*a*a*a*b"), {}
    };
    for (const auto& seed : seeds) {
        exercise_packet({seed.data(), seed.size()});
        exercise_pattern({seed.data(), seed.size()});
    }

    std::mt19937 random(0x4f534331);
    constexpr unsigned trials = 30000;
    for (unsigned iteration = 0; iteration < trials; ++iteration) {
        auto input = seeds[random() % seeds.size()];
        const auto edits = 1 + random() % 8;
        for (unsigned edit = 0; edit < edits; ++edit) {
            const std::size_t pos = random() % (input.size() + 1);
            const auto byte = static_cast<std::uint8_t>(random());
            if (pos == input.size()) input.push_back(byte);
            else if (random() % 4 == 0) input.erase(input.begin() + static_cast<Buffer::difference_type>(pos));
            else input[pos] = byte;
        }
        const std::size_t offset = iteration % 4;
        Buffer shifted(offset, 0xff);
        shifted.insert(shifted.end(), input.begin(), input.end());
        const ByteView sample{shifted.empty() ? nullptr : shifted.data() + offset, input.size()};
        exercise_packet(sample);
        exercise_pattern(sample);
    }
    std::cout << trials << " seeded mutations passed with ASan/UBSan\n";
}
