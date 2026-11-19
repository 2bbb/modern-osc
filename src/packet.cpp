#include "modern_osc/packet.hpp"
#include "modern_osc/pattern.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

namespace modern_osc {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

constexpr std::size_t hard_depth_limit = 64;
constexpr std::string_view bundle_header{"#bundle\0", 8};
constexpr std::string_view known_tags{"ifsbhtdScrmTFNI[]"};

constexpr std::size_t padding(std::size_t size) noexcept { return (4 - size % 4) % 4; }

bool ascii(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 128; });
}

template<class Signed, class Unsigned>
Signed signed_value(Unsigned bits) {
    if (bits <= static_cast<Unsigned>((std::numeric_limits<Signed>::max)()))
        return static_cast<Signed>(bits);
    return static_cast<Signed>(-1 - static_cast<Signed>(static_cast<Unsigned>(~bits)));
}

struct Reader {
    ByteView bytes;
    std::size_t base;
    std::size_t pos = 0;

    std::size_t offset() const { return base + pos; }
    std::size_t remaining() const { return bytes.size - pos; }

    Result<ByteView> take(std::size_t count) {
        if (count > remaining()) return Error{ErrorCode::truncated, offset()};
        ByteView result{bytes.data + pos, count};
        pos += count;
        return result;
    }

    std::optional<Error> zero_padding(std::size_t count) {
        const auto start = offset();
        auto pad = take(count);
        if (!pad) return pad.error();
        for (std::size_t i = 0; i < pad->size; ++i)
            if (pad->data[i] != 0) return Error{ErrorCode::invalid_padding, start + i};
        return {};
    }

    template<class UInt>
    Result<UInt> number() {
        auto span = take(sizeof(UInt));
        if (!span) return span.error();
        UInt result = 0;
        for (std::size_t i = 0; i < span->size; ++i)
            result = static_cast<UInt>((result << 8) | span->data[i]);
        return result;
    }

    Result<std::string_view> string() {
        if (remaining() == 0) return Error{ErrorCode::truncated, offset()};
        auto start = bytes.data + pos;
        auto end = static_cast<const std::uint8_t*>(std::memchr(start, 0, remaining()));
        if (!end) return Error{ErrorCode::truncated, offset()};
        const auto size = static_cast<std::size_t>(end - start);
        pos += size + 1;
        if (auto err = zero_padding(padding(size + 1))) return *err;
        return std::string_view{reinterpret_cast<const char*>(start), size};
    }
};

class Decoder {
public:
    explicit Decoder(DecodeOptions options)
        : options_(options), items_left_(options.limits.items),
          depth_limit_(std::min(options.limits.depth, hard_depth_limit)) {}

    Result<PacketView> packet(ByteView bytes, std::size_t base, std::size_t depth) {
        if (bytes.size == 0 || bytes.size % 4 != 0)
            return Error{ErrorCode::invalid_size, base};
        if (depth > depth_limit_) return Error{ErrorCode::depth_limit, base};
        Reader reader{bytes, base};
        if (bytes.size >= bundle_header.size() &&
            std::memcmp(bytes.data, bundle_header.data(), bundle_header.size()) == 0) {
            reader.pos = bundle_header.size();
            auto time = reader.number<std::uint64_t>();
            if (!time) return time.error();
            BundleView result{{*time}, {}};
            while (reader.remaining() != 0) {
                const auto length_offset = reader.offset();
                if (!consume_item()) return Error{ErrorCode::item_limit, length_offset};
                auto size = reader.number<std::uint32_t>();
                if (!size) return size.error();
                if (*size == 0 || *size > 0x7fff'ffffu || *size % 4 != 0)
                    return Error{ErrorCode::invalid_size, length_offset};
                const auto child_offset = reader.offset();
                auto span = reader.take(*size);
                if (!span) return span.error();
                auto child = packet(*span, child_offset, depth + 1);
                if (!child) return child.error();
                result.elements.push_back(std::move(child).value());
            }
            return PacketView{std::move(result)};
        }

        auto address = reader.string();
        if (!address) return address.error();
        auto pattern = AddressPattern::compile(*address, {options_.limits.address_bytes, 4096});
        if (!pattern) {
            auto err = pattern.error();
            err.offset += base;
            if (err.code == ErrorCode::invalid_pattern) err.code = ErrorCode::invalid_address;
            return err;
        }
        if (reader.remaining() == 0 || reader.bytes.data[reader.pos] != ',') {
            if (!options_.allow_missing_tags) return Error{ErrorCode::invalid_tags, reader.offset()};
            if (reader.remaining() == 0) return PacketView{MessageView{*address, {}}};
            return PacketView{OpaqueMessageView{*address, bytes, OpaqueReason::missing_tags}};
        }

        auto tag_offset = reader.offset() + 1;
        auto tags = reader.string();
        if (!tags) return tags.error();
        tags->remove_prefix(1);
        bool opaque = false;
        std::size_t array_depth = 0;
        for (std::size_t i = 0; i < tags->size(); ++i) {
            const auto tag = static_cast<unsigned char>((*tags)[i]);
            if (tag <= 32 || tag >= 127 || tag == ',')
                return Error{ErrorCode::invalid_tags, tag_offset + i};
            if (tag == ']') {
                if (array_depth == 0) return Error{ErrorCode::invalid_tags, tag_offset + i};
                --array_depth;
                continue;
            }
            if (!consume_item()) return Error{ErrorCode::item_limit, tag_offset + i};
            if (tag == '[' && ++array_depth > depth_limit_)
                return Error{ErrorCode::depth_limit, tag_offset + i};
            opaque = opaque || known_tags.find(static_cast<char>(tag)) == std::string_view::npos;
        }
        if (array_depth != 0) return Error{ErrorCode::invalid_tags, tag_offset + tags->size()};
        if (opaque) return PacketView{OpaqueMessageView{*address, bytes, OpaqueReason::unknown_tag}};

        std::size_t index = 0;
        auto args = arguments(reader, *tags, index);
        if (!args) return args.error();
        if (reader.remaining() != 0) return Error{ErrorCode::trailing_bytes, reader.offset()};
        return PacketView{MessageView{*address, std::move(args).value()}};
    }

private:
    DecodeOptions options_;
    std::size_t items_left_;
    std::size_t depth_limit_;

    bool consume_item() {
        if (items_left_ == 0) return false;
        --items_left_;
        return true;
    }

    Result<std::vector<ArgumentView>> arguments(Reader& reader, std::string_view tags,
                                                 std::size_t& index) {
        std::vector<ArgumentView> result;
        while (index < tags.size()) {
            auto tag = tags[index++];
            if (tag == ']') break;
            auto argument = tag == '[' ? array(reader, tags, index) : scalar(reader, tag);
            if (!argument) return argument.error();
            result.push_back(std::move(argument).value());
        }
        return result;
    }

    Result<ArgumentView> array(Reader& reader, std::string_view tags, std::size_t& index) {
        auto result = arguments(reader, tags, index);
        if (!result) return result.error();
        return ArgumentView{std::move(result).value()};
    }

    Result<ArgumentView> scalar(Reader& reader, char tag) {
        const auto offset = reader.offset();
        switch (tag) {
        case 'T': return ArgumentView{true};
        case 'F': return ArgumentView{false};
        case 'N': return ArgumentView{Nil{}};
        case 'I': return ArgumentView{Impulse{}};
        case 's':
        case 'S': {
            auto text = reader.string();
            if (!text) return text.error();
            if (options_.strict_strings && !ascii(*text)) return Error{ErrorCode::invalid_string, offset};
            if (tag == 'S') return ArgumentView{SymbolView{*text}};
            return ArgumentView{*text};
        }
        case 'b': {
            auto size = reader.number<std::uint32_t>();
            if (!size) return size.error();
            if (*size > 0x7fff'ffffu) return Error{ErrorCode::invalid_size, offset};
            auto data = reader.take(*size);
            if (!data) return data.error();
            if (auto err = reader.zero_padding(padding(*size))) return *err;
            return ArgumentView{BlobView{*data}};
        }
        case 'h':
        case 't':
        case 'd': {
            auto bits = reader.number<std::uint64_t>();
            if (!bits) return bits.error();
            if (tag == 'h') return ArgumentView{signed_value<std::int64_t>(*bits)};
            if (tag == 't') return ArgumentView{Timetag{*bits}};
            return ArgumentView{Float64{*bits}};
        }
        case 'r':
        case 'm': {
            auto data = reader.take(4);
            if (!data) return data.error();
            const std::array<std::uint8_t, 4> bytes{
                data->data[0], data->data[1], data->data[2], data->data[3]};
            if (tag == 'r') return ArgumentView{Rgba{bytes}};
            return ArgumentView{Midi{bytes}};
        }
        default: {
            auto bits = reader.number<std::uint32_t>();
            if (!bits) return bits.error();
            if (tag == 'i') return ArgumentView{signed_value<std::int32_t>(*bits)};
            if (tag == 'f') return ArgumentView{Float32{*bits}};
            if (*bits > 127) return Error{ErrorCode::invalid_argument, offset};
            return ArgumentView{Character{static_cast<std::uint8_t>(*bits)}};
        }
        }
    }
};

std::vector<std::uint8_t> copy_bytes(ByteView view) {
    if (view.size == 0) return {};
    return {view.data, view.data + view.size};
}

Argument own_argument(const ArgumentView& argument) {
    return std::visit([](const auto& value) -> Argument {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string_view>) {
            return {std::string(value)};
        } else if constexpr (std::is_same_v<T, SymbolView>) {
            return {Symbol{std::string(value.value)}};
        } else if constexpr (std::is_same_v<T, BlobView>) {
            return {Blob{copy_bytes(value.value)}};
        } else if constexpr (std::is_same_v<T, ArgumentView::Array>) {
            Argument::Array array;
            array.reserve(value.size());
            for (const auto& child : value) array.push_back(own_argument(child));
            return {std::move(array)};
        } else {
            return {value};
        }
    }, argument.value);
}

// The same writer measures and writes. The writing pass follows a successful
// measurement of the same packet and therefore needs no allocations.
class Encoder {
public:
    Encoder(EncodeOptions options, std::uint8_t* output = nullptr)
        : options_(options), output_(output), items_left_(options.limits.items),
          depth_limit_(std::min(options.limits.depth, hard_depth_limit)) {}

    Result<std::size_t> run(const Packet& packet) {
        write_packet(packet, 0);
        if (error_) return *error_;
        return pos_;
    }

private:
    EncodeOptions options_;
    std::uint8_t* output_;
    std::size_t items_left_;
    std::size_t depth_limit_;
    std::size_t pos_ = 0;
    std::optional<Error> error_;

    void fail(ErrorCode code) {
        if (!error_) error_ = Error{code, pos_};
    }

    bool reserve(std::size_t size) {
        if (error_) return false;
        if (size > options_.limits.packet_bytes - pos_) {
            fail(ErrorCode::size_limit);
            return false;
        }
        return true;
    }

    bool item() {
        if (items_left_ == 0) {
            fail(ErrorCode::item_limit);
            return false;
        }
        --items_left_;
        return true;
    }

    void bytes(const void* source, std::size_t size) {
        if (!reserve(size)) return;
        if (output_ && size) std::memcpy(output_ + pos_, source, size);
        pos_ += size;
    }

    void zeros(std::size_t size) {
        if (!reserve(size)) return;
        if (output_ && size) std::memset(output_ + pos_, 0, size);
        pos_ += size;
    }

    static void store32(std::uint8_t* out, std::uint32_t value) {
        for (int i = 3; i >= 0; --i) {
            out[i] = static_cast<std::uint8_t>(value);
            value >>= 8;
        }
    }

    template<class UInt>
    void number(UInt value) {
        std::uint8_t data[sizeof(UInt)];
        for (std::size_t i = sizeof(UInt); i > 0; --i) {
            data[i - 1] = static_cast<std::uint8_t>(value);
            value = static_cast<UInt>(value >> 8);
        }
        bytes(data, sizeof(data));
    }

    void string(std::string_view value, bool check_ascii) {
        if (value.find('\0') != std::string_view::npos || (check_ascii && !ascii(value))) {
            fail(ErrorCode::invalid_string);
            return;
        }
        bytes(value.data(), value.size());
        zeros(1);
        zeros(padding(pos_));
    }

    void tag(char value) { bytes(&value, 1); }

    void write_tags(const std::vector<Argument>& arguments, std::size_t depth) {
        if (depth > depth_limit_) { fail(ErrorCode::depth_limit); return; }
        constexpr char tags[] = {'i','f','s','b','h','t','d','S','c','r','m','T','N','I','['};
        for (const auto& argument : arguments) {
            if (error_ || !item()) return;
            if (argument.value.valueless_by_exception()) { fail(ErrorCode::invalid_argument); return; }
            if (const auto* nested = std::get_if<Argument::Array>(&argument.value)) {
                tag('[');
                write_tags(*nested, depth + 1);
                tag(']');
            } else if (const auto* boolean = std::get_if<bool>(&argument.value)) {
                tag(*boolean ? 'T' : 'F');
            } else {
                tag(tags[argument.value.index()]);
            }
        }
    }

    void write_payload(const std::vector<Argument>& arguments) {
        for (const auto& argument : arguments) {
            if (error_) return;
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::int32_t>) {
                    number(static_cast<std::uint32_t>(value));
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    number(static_cast<std::uint64_t>(value));
                } else if constexpr (std::is_same_v<T, Float32> || std::is_same_v<T, Float64> ||
                                     std::is_same_v<T, Timetag>) {
                    number(value.bits);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    string(value, !options_.allow_non_ascii_strings);
                } else if constexpr (std::is_same_v<T, Symbol>) {
                    string(value.value, !options_.allow_non_ascii_strings);
                } else if constexpr (std::is_same_v<T, Blob>) {
                    if (value.value.size() > 0x7fff'ffffu) { fail(ErrorCode::invalid_size); return; }
                    number(static_cast<std::uint32_t>(value.value.size()));
                    bytes(value.value.data(), value.value.size());
                    zeros(padding(value.value.size()));
                } else if constexpr (std::is_same_v<T, Character>) {
                    if (value.value > 127) { fail(ErrorCode::invalid_argument); return; }
                    number(static_cast<std::uint32_t>(value.value));
                } else if constexpr (std::is_same_v<T, Rgba> || std::is_same_v<T, Midi>) {
                    bytes(value.bytes.data(), value.bytes.size());
                } else if constexpr (std::is_same_v<T, Argument::Array>) {
                    write_payload(value);
                }
            }, argument.value);
        }
    }

    void write_packet(const Packet& packet, std::size_t depth) {
        if (error_) return;
        if (depth > depth_limit_) { fail(ErrorCode::depth_limit); return; }
        if (const auto* message = std::get_if<Message>(&packet.value)) {
            if (!output_) {
                auto pattern = AddressPattern::compile(message->address,
                                                       {options_.limits.address_bytes, 4096});
                if (!pattern) {
                    error_ = pattern.error();
                    error_->offset += pos_;
                    if (error_->code == ErrorCode::invalid_pattern) error_->code = ErrorCode::invalid_address;
                    return;
                }
            }
            string(message->address, false);
            tag(',');
            write_tags(message->arguments, 0);
            zeros(1);
            zeros(padding(pos_));
            write_payload(message->arguments);
        } else if (const auto* bundle = std::get_if<Bundle>(&packet.value)) {
            bytes(bundle_header.data(), bundle_header.size());
            number(bundle->time.bits);
            for (const auto& child : bundle->elements) {
                if (error_ || !item()) return;
                const auto length_offset = pos_;
                zeros(4);
                const auto payload = pos_;
                write_packet(child, depth + 1);
                if (error_) return;
                const auto length = pos_ - payload;
                if (length > 0x7fff'ffffu) { fail(ErrorCode::invalid_size); return; }
                if (output_) store32(output_ + length_offset, static_cast<std::uint32_t>(length));
            }
        } else {
            fail(ErrorCode::opaque_message);
        }
    }
};

} // namespace

Float32 Float32::from_value(float value) noexcept {
    Float32 result;
    std::memcpy(&result.bits, &value, sizeof(value));
    return result;
}

float Float32::value() const noexcept {
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

Float64 Float64::from_value(double value) noexcept {
    Float64 result;
    std::memcpy(&result.bits, &value, sizeof(value));
    return result;
}

double Float64::value() const noexcept {
    double result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

Result<PacketView> decode(ByteView bytes, DecodeOptions options) {
    if (bytes.data == nullptr && bytes.size != 0) return Error{ErrorCode::invalid_buffer};
    if (bytes.size > options.limits.packet_bytes) return Error{ErrorCode::size_limit};
    return Decoder(options).packet(bytes, 0, 0);
}

Packet to_owned(const PacketView& packet) {
    return std::visit([](const auto& value) -> Packet {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MessageView>) {
            Message message{std::string(value.address), {}};
            message.arguments.reserve(value.arguments.size());
            for (const auto& argument : value.arguments)
                message.arguments.push_back(own_argument(argument));
            return {std::move(message)};
        } else if constexpr (std::is_same_v<T, BundleView>) {
            Bundle bundle{value.time, {}};
            bundle.elements.reserve(value.elements.size());
            for (const auto& child : value.elements) bundle.elements.push_back(to_owned(child));
            return {std::move(bundle)};
        } else {
            return {OpaqueMessage{std::string(value.address), copy_bytes(value.bytes), value.reason}};
        }
    }, packet.value);
}

Result<std::size_t> encoded_size(const Packet& packet, EncodeOptions options) {
    return Encoder(options).run(packet);
}

Result<std::size_t> encode_into(const Packet& packet, MutableBytes output, EncodeOptions options) {
    if (output.data == nullptr && output.size != 0) return Error{ErrorCode::invalid_buffer};
    auto size = encoded_size(packet, options);
    if (!size) return size.error();
    if (*size > output.size) return Error{ErrorCode::insufficient_buffer, *size};
    return Encoder(options, output.data).run(packet);
}

Result<std::vector<std::uint8_t>> encode(const Packet& packet, EncodeOptions options) {
    auto size = encoded_size(packet, options);
    if (!size) return size.error();
    std::vector<std::uint8_t> output(*size);
    auto written = Encoder(options, output.data()).run(packet);
    if (!written) return written.error();
    return output;
}

} // namespace modern_osc
