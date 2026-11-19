#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace modern_osc {

struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct MutableBytes {
    std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

enum class ErrorCode {
    invalid_buffer, invalid_size, truncated, invalid_padding, invalid_address,
    invalid_pattern, invalid_string, invalid_tags, invalid_argument,
    trailing_bytes, size_limit, depth_limit, item_limit, insufficient_buffer,
    opaque_message, dispatch_limit, busy, invalid_endpoint, socket_error
};

struct Error {
    ErrorCode code;
    std::size_t offset = 0;
    int native_code = 0;
};

template<class T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(error) {}

    // Tests whether a result exists. For Result<bool>, inspect value() for the predicate.
    explicit operator bool() const noexcept { return std::holds_alternative<T>(storage_); }
    T& value() & { return std::get<T>(storage_); }
    const T& value() const & { return std::get<T>(storage_); }
    T&& value() && { return std::get<T>(std::move(storage_)); }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    Error error() const { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

struct Float32 {
    std::uint32_t bits = 0;
    static Float32 from_value(float value) noexcept;
    float value() const noexcept;
};

struct Float64 {
    std::uint64_t bits = 0;
    static Float64 from_value(double value) noexcept;
    double value() const noexcept;
};

struct Timetag {
    std::uint64_t bits = 1;
    static constexpr Timetag immediate() noexcept { return {1}; }
    constexpr bool is_immediate() const noexcept { return bits == 1; }
};

struct Character { std::uint8_t value; };
struct Rgba { std::array<std::uint8_t, 4> bytes; };
struct Midi { std::array<std::uint8_t, 4> bytes; };
struct Nil {};
struct Impulse {};

template<bool View>
using Text = std::conditional_t<View, std::string_view, std::string>;

template<bool View>
using Bytes = std::conditional_t<View, ByteView, std::vector<std::uint8_t>>;

template<bool View> struct BasicSymbol { Text<View> value; };
template<bool View> struct BasicBlob { Bytes<View> value; };

template<bool View>
struct BasicArgument {
    using Array = std::vector<BasicArgument>;
    using Value = std::variant<std::int32_t, Float32, Text<View>, BasicBlob<View>,
                               std::int64_t, Timetag, Float64, BasicSymbol<View>,
                               Character, Rgba, Midi, bool, Nil, Impulse, Array>;
    Value value;
};

template<bool View>
struct BasicMessage {
    Text<View> address;
    std::vector<BasicArgument<View>> arguments;
};

enum class OpaqueReason { unknown_tag, missing_tags };

template<bool View>
struct BasicOpaqueMessage {
    Text<View> address;
    Bytes<View> bytes;
    OpaqueReason reason;
};

template<bool View> struct BasicPacket;

template<bool View>
struct BasicBundle {
    Timetag time;
    std::vector<BasicPacket<View>> elements;
};

template<bool View>
struct BasicPacket {
    std::variant<BasicMessage<View>, BasicBundle<View>, BasicOpaqueMessage<View>> value;
};

using Symbol = BasicSymbol<false>;
using SymbolView = BasicSymbol<true>;
using Blob = BasicBlob<false>;
using BlobView = BasicBlob<true>;
using Argument = BasicArgument<false>;
using ArgumentView = BasicArgument<true>;
using Message = BasicMessage<false>;
using MessageView = BasicMessage<true>;
using OpaqueMessage = BasicOpaqueMessage<false>;
using OpaqueMessageView = BasicOpaqueMessage<true>;
using Bundle = BasicBundle<false>;
using BundleView = BasicBundle<true>;
using Packet = BasicPacket<false>;
using PacketView = BasicPacket<true>;

struct Limits {
    std::size_t packet_bytes = 1024 * 1024;
    std::size_t items = 16384;
    std::size_t depth = 32;
    std::size_t address_bytes = 1024;
};

struct DecodeOptions {
    Limits limits;
    bool strict_strings = false;
    bool allow_missing_tags = false;
};

struct EncodeOptions {
    Limits limits;
    bool allow_non_ascii_strings = false;
};

// A decoded view borrows the input bytes. Keep them unchanged for its lifetime.
Result<PacketView> decode(ByteView bytes, DecodeOptions options = {});
Packet to_owned(const PacketView& packet);

// A failure leaves the output buffer unchanged. Forward opaque input via its raw bytes.
Result<std::size_t> encoded_size(const Packet& packet, EncodeOptions options = {});
Result<std::size_t> encode_into(const Packet& packet, MutableBytes output,
                                EncodeOptions options = {});
Result<std::vector<std::uint8_t>> encode(const Packet& packet, EncodeOptions options = {});

} // namespace modern_osc
