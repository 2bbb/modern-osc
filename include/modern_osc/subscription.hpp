#pragma once

#include "modern_osc/packet.hpp"

#include <tuple>

namespace modern_osc {

namespace detail {

template<class T, bool Tagged = false>
struct Output {
    using value_type = T;
    static constexpr bool tagged = Tagged;
    T& value;
};

template<class T>
Output<T> bind_output(T& value) {
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>,
                  "A subscription needs a writable output");
    return {value};
}

template<class T>
Output<T, true> bind_output(Output<T, true> value) { return value; }

template<class T> inline constexpr bool is_vector = false;
template<class T> inline constexpr bool is_vector<std::vector<T>> = true;

template<class T> struct WireType { using type = T; };
template<> struct WireType<float> { using type = Float32; };
template<> struct WireType<double> { using type = Float64; };
template<> struct WireType<std::string> { using type = std::string_view; };
template<> struct WireType<Blob> { using type = BlobView; };
template<> struct WireType<Symbol> { using type = SymbolView; };

template<class T>
bool read_scalar(const ArgumentView& argument, T& out) {
    static_assert(std::is_constructible_v<Argument::Value, std::in_place_type_t<T>> ||
                  std::is_same_v<T, float> || std::is_same_v<T, double>,
                  "Subscription elements must be owning OSC scalar types");
    static_assert(!std::is_same_v<T, Argument::Array>, "Nested output arrays are unsupported");
    const auto* value = std::get_if<typename WireType<T>::type>(&argument.value);
    if (!value) return false;
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
        out = value->value();
    } else if constexpr (std::is_same_v<T, Symbol>) {
        out.value = value->value;
    } else if constexpr (std::is_same_v<T, Blob>) {
        const auto bytes = value->value;
        if (bytes.size && !bytes.data) return false;
        if (bytes.size) out.value.assign(bytes.data, bytes.data + bytes.size);
        else out.value.clear();
    } else {
        out = *value;
    }
    return true;
}

using Arguments = std::vector<ArgumentView>;

template<class T>
bool read_vector(const Arguments& args, std::size_t start, std::vector<T>& out) {
    out.reserve(args.size() - start);
    for (std::size_t index = start; index < args.size(); ++index) {
        T value{};
        if (!read_scalar(args[index], value)) return false;
        out.push_back(std::move(value));
    }
    return true;
}

template<bool Last, class Slot>
bool read_output(const Arguments& args, std::size_t& index, typename Slot::value_type& out) {
    if constexpr (Slot::tagged) {
        if (index == args.size()) return false;
        const auto* children = std::get_if<ArgumentView::Array>(&args[index++].value);
        return children && read_vector(*children, 0, out);
    } else if constexpr (is_vector<typename Slot::value_type>) {
        static_assert(Last, "A bare vector consumes all remaining arguments and must come last");
        const auto start = std::exchange(index, args.size());
        return read_vector(args, start, out);
    } else {
        return index < args.size() && read_scalar(args[index++], out);
    }
}

template<class... Slots, std::size_t... I>
bool update_outputs(const Arguments& args, const std::tuple<Slots...>& slots,
                    std::index_sequence<I...>) {
    std::tuple<typename Slots::value_type...> staged;
    std::size_t index = 0;
    if (!(read_output<I + 1 == sizeof...(Slots), Slots>(args, index, std::get<I>(staged)) && ...)
        || index != args.size())
        return false;
    ((std::get<I>(slots).value = std::move(std::get<I>(staged))), ...);
    return true;
}

} // namespace detail

// Consume one OSC [ ... ] argument into this vector at any argument position.
template<class T>
auto array(std::vector<T>& destination) noexcept {
    return detail::Output<std::vector<T>, true>{destination};
}

} // namespace modern_osc
