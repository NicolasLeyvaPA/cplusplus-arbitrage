#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace arb {
namespace msgpack {

/**
 * Minimal msgpack encoder for Hyperliquid action serialization.
 * Only supports the types needed: maps, strings, integers, booleans, nil, binary.
 */
class Packer {
public:
    // Pack nil
    void pack_nil();

    // Pack boolean
    void pack_bool(bool val);

    // Pack integers
    void pack_uint(uint64_t val);
    void pack_int(int64_t val);

    // Pack string
    void pack_string(const std::string& val);

    // Pack binary data
    void pack_bin(const uint8_t* data, size_t len);
    void pack_bin(const std::vector<uint8_t>& data);

    // Pack array header (caller must then pack N elements)
    void pack_array(size_t count);

    // Pack map header (caller must then pack N key-value pairs)
    void pack_map(size_t count);

    // Pack float64
    void pack_double(double val);

    // Get the serialized bytes
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }

    void clear() { buf_.clear(); }

private:
    void write_byte(uint8_t b);
    void write_bytes(const uint8_t* data, size_t len);
    void write_u16_be(uint16_t val);
    void write_u32_be(uint32_t val);
    void write_u64_be(uint64_t val);

    std::vector<uint8_t> buf_;
};

} // namespace msgpack
} // namespace arb
