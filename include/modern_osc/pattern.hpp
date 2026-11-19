#pragma once

#include "modern_osc/packet.hpp"

#include <bitset>

namespace modern_osc {

bool is_address(std::string_view address) noexcept;

struct PatternLimits {
    std::size_t bytes = 1024;
    std::size_t states = 4096;
};

class AddressPattern {
public:
    static Result<AddressPattern> compile(std::string_view pattern, PatternLimits limits = {});

    // Each automaton transition and each pair of segment positions consumes budget.
    Result<bool> match(std::string_view address, std::size_t& steps_left) const;

private:
    enum class Transition { epsilon, characters };
    struct Edge {
        std::size_t destination;
        Transition kind;
        std::bitset<128> characters;
    };
    struct Segment {
        bool recursive = false;
        std::vector<std::vector<Edge>> edges;
    };

    std::vector<Segment> segments_;

    static Result<Segment> compile_segment(std::string_view text, std::size_t offset,
                                          std::size_t& states_left);
    static Result<bool> match_segment(const Segment& segment, std::string_view text,
                                      std::size_t& steps_left);
};

} // namespace modern_osc
