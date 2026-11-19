#include "modern_osc/pattern.hpp"

#include <algorithm>

namespace modern_osc {
namespace {

bool name_character(unsigned char ch) noexcept {
    return ch > 32 && ch < 127 &&
           std::string_view("#*,/?[]{}").find(static_cast<char>(ch)) == std::string_view::npos;
}

bool take_step(std::size_t& remaining) noexcept {
    if (remaining == 0) return false;
    --remaining;
    return true;
}

} // namespace

bool is_address(std::string_view address) noexcept {
    if (address.size() < 2 || address.front() != '/') return false;
    bool has_character = false;
    for (auto ch : address.substr(1)) {
        if (ch == '/') {
            if (!has_character) return false;
            has_character = false;
        } else {
            if (!name_character(static_cast<unsigned char>(ch))) return false;
            has_character = true;
        }
    }
    return has_character;
}

Result<AddressPattern> AddressPattern::compile(std::string_view pattern, PatternLimits limits) {
    if (pattern.size() > limits.bytes) return Error{ErrorCode::size_limit, limits.bytes};
    if (pattern.empty() || pattern.front() != '/') return Error{ErrorCode::invalid_pattern};

    AddressPattern result;
    std::size_t pos = 0;
    auto states_left = limits.states;
    while (pos < pattern.size()) {
        if (pos + 1 < pattern.size() && pattern[pos + 1] == '/') {
            if (!take_step(states_left)) return Error{ErrorCode::size_limit, pos};
            result.segments_.push_back({true, {}});
            pos += 2;
            if (pos == pattern.size()) break;
            if (pattern[pos] == '/') return Error{ErrorCode::invalid_pattern, pos};
        } else {
            ++pos;
        }

        auto end = pattern.find('/', pos);
        if (end == std::string_view::npos) end = pattern.size();
        auto compiled = compile_segment(pattern.substr(pos, end - pos), pos, states_left);
        if (!compiled) return compiled.error();
        result.segments_.push_back(std::move(compiled).value());
        pos = end;
    }
    return result;
}

Result<AddressPattern::Segment> AddressPattern::compile_segment(std::string_view text,
                                                               std::size_t offset,
                                                               std::size_t& states_left) {
    if (text.empty()) return Error{ErrorCode::invalid_pattern, offset};
    Segment segment;
    auto add_state = [&]() {
        if (!take_step(states_left)) return false;
        segment.edges.emplace_back();
        return true;
    };
    if (!add_state()) return Error{ErrorCode::size_limit, offset};

    std::size_t pos = 0;
    auto character = [](unsigned char ch) {
        std::bitset<128> set;
        set.set(ch);
        return set;
    };
    const std::bitset<128> any = ~std::bitset<128>{};

    while (pos < text.size()) {
        const auto begin = pos;
        const auto source = segment.edges.size() - 1;
        const auto ch = static_cast<unsigned char>(text[pos++]);
        if (ch == '{') {
            std::vector<std::size_t> ends;
            while (true) {
                auto last = source;
                const auto alternative_start = pos;
                while (pos < text.size() && text[pos] != ',' && text[pos] != '}') {
                    auto literal = static_cast<unsigned char>(text[pos]);
                    if (!name_character(literal)) return Error{ErrorCode::invalid_pattern, offset + pos};
                    if (!add_state()) return Error{ErrorCode::size_limit, offset + pos};
                    auto next = segment.edges.size() - 1;
                    segment.edges[last].push_back({next, Transition::characters, character(literal)});
                    last = next;
                    ++pos;
                }
                if (pos == text.size() || pos == alternative_start)
                    return Error{ErrorCode::invalid_pattern, offset + pos};
                ends.push_back(last);
                if (text[pos++] == '}') break;
            }
            if (!add_state()) return Error{ErrorCode::size_limit, offset + begin};
            for (auto last : ends)
                segment.edges[last].push_back({segment.edges.size() - 1, Transition::epsilon, {}});
            continue;
        }

        if (!add_state()) return Error{ErrorCode::size_limit, offset + begin};
        const auto next = segment.edges.size() - 1;
        if (ch == '*') {
            segment.edges[source].push_back({source, Transition::characters, any});
            segment.edges[source].push_back({next, Transition::epsilon, {}});
            continue;
        }

        std::bitset<128> selected;
        if (ch == '?') {
            selected = any;
        } else if (ch == '[') {
            const bool negate = pos < text.size() && text[pos] == '!';
            if (negate) ++pos;
            const auto start = pos;
            while (pos < text.size() && text[pos] != ']') {
                const auto first = static_cast<unsigned char>(text[pos++]);
                if (first <= 32 || first >= 127 || first == '[')
                    return Error{ErrorCode::invalid_pattern, offset + pos - 1};
                if (pos + 1 < text.size() && text[pos] == '-' && text[pos + 1] != ']') {
                    const auto last = static_cast<unsigned char>(text[pos + 1]);
                    if (last < first || last >= 127 || last == '[')
                        return Error{ErrorCode::invalid_pattern, offset + pos + 1};
                    for (auto member = first; member <= last; ++member) selected.set(member);
                    pos += 2;
                } else {
                    selected.set(first);
                }
            }
            if (pos == start || pos == text.size())
                return Error{ErrorCode::invalid_pattern, offset + pos};
            ++pos;
            if (negate) selected.flip();
        } else if (name_character(ch)) {
            selected = character(ch);
        } else {
            return Error{ErrorCode::invalid_pattern, offset + begin};
        }
        segment.edges[source].push_back({next, Transition::characters, selected});
    }
    return segment;
}

Result<bool> AddressPattern::match_segment(const Segment& segment, std::string_view text,
                                           std::size_t& steps_left) {
    std::vector<bool> current(segment.edges.size());
    std::vector<bool> next(segment.edges.size());
    current[0] = true;
    // Epsilon edges always go to a higher state. One forward sweep closes the set.
    for (std::size_t pos = 0;; ++pos) {
        const bool end = pos == text.size();
        for (std::size_t state = 0; state < segment.edges.size(); ++state) {
            if (!take_step(steps_left)) return Error{ErrorCode::dispatch_limit};
            if (!current[state]) continue;
            for (const auto& edge : segment.edges[state]) {
                if (!take_step(steps_left)) return Error{ErrorCode::dispatch_limit};
                if (edge.kind == Transition::epsilon) {
                    current[edge.destination] = true;
                } else if (!end && edge.characters[static_cast<unsigned char>(text[pos])]) {
                    next[edge.destination] = true;
                }
            }
        }
        if (end) return static_cast<bool>(current.back());
        if (std::none_of(next.begin(), next.end(), [](bool active) { return active; })) return false;
        current.swap(next);
        std::fill(next.begin(), next.end(), false);
    }
}

Result<bool> AddressPattern::match(std::string_view address, std::size_t& steps_left) const {
    if (address.size() > steps_left) { steps_left = 0; return Error{ErrorCode::dispatch_limit}; }
    steps_left -= address.size();
    if (!is_address(address)) return Error{ErrorCode::invalid_address};

    std::vector<std::string_view> parts;
    std::size_t pos = 1;
    while (pos < address.size()) {
        auto end = address.find('/', pos);
        if (end == std::string_view::npos) end = address.size();
        parts.push_back(address.substr(pos, end - pos));
        pos = end + 1;
    }

    std::vector<bool> current(parts.size() + 1);
    std::vector<bool> next(parts.size() + 1);
    current[0] = true;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        if (segment.recursive) {
            const bool trailing = i + 1 == segments_.size();
            bool can_consume = false;
            for (std::size_t used = 0; used <= parts.size(); ++used) {
                if (!take_step(steps_left)) return Error{ErrorCode::dispatch_limit};
                next[used] = can_consume || (!trailing && current[used]);
                can_consume = can_consume || current[used];
            }
        } else {
            for (std::size_t used = 0; used < parts.size(); ++used) {
                if (!take_step(steps_left)) return Error{ErrorCode::dispatch_limit};
                if (!current[used]) continue;
                const auto matched = match_segment(segment, parts[used], steps_left);
                if (!matched) return matched.error();
                next[used + 1] = *matched;
            }
        }
        current.swap(next);
        std::fill(next.begin(), next.end(), false);
    }
    return static_cast<bool>(current.back());
}

} // namespace modern_osc
