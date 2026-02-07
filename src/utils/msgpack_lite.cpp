#include "utils/msgpack_lite.hpp"
#include <cstring>

namespace arb {
namespace msgpack {

void Packer::write_byte(uint8_t b) {
    buf_.push_back(b);
}

void Packer::write_bytes(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

void Packer::write_u16_be(uint16_t val) {
    write_byte(static_cast<uint8_t>(val >> 8));
    write_byte(static_cast<uint8_t>(val));
}

void Packer::write_u32_be(uint32_t val) {
    write_byte(static_cast<uint8_t>(val >> 24));
    write_byte(static_cast<uint8_t>(val >> 16));
    write_byte(static_cast<uint8_t>(val >> 8));
    write_byte(static_cast<uint8_t>(val));
}

void Packer::write_u64_be(uint64_t val) {
    write_byte(static_cast<uint8_t>(val >> 56));
    write_byte(static_cast<uint8_t>(val >> 48));
    write_byte(static_cast<uint8_t>(val >> 40));
    write_byte(static_cast<uint8_t>(val >> 32));
    write_byte(static_cast<uint8_t>(val >> 24));
    write_byte(static_cast<uint8_t>(val >> 16));
    write_byte(static_cast<uint8_t>(val >> 8));
    write_byte(static_cast<uint8_t>(val));
}

void Packer::pack_nil() {
    write_byte(0xc0);
}

void Packer::pack_bool(bool val) {
    write_byte(val ? 0xc3 : 0xc2);
}

void Packer::pack_uint(uint64_t val) {
    if (val <= 0x7f) {
        // positive fixint
        write_byte(static_cast<uint8_t>(val));
    } else if (val <= 0xff) {
        write_byte(0xcc);
        write_byte(static_cast<uint8_t>(val));
    } else if (val <= 0xffff) {
        write_byte(0xcd);
        write_u16_be(static_cast<uint16_t>(val));
    } else if (val <= 0xffffffff) {
        write_byte(0xce);
        write_u32_be(static_cast<uint32_t>(val));
    } else {
        write_byte(0xcf);
        write_u64_be(val);
    }
}

void Packer::pack_int(int64_t val) {
    if (val >= 0) {
        pack_uint(static_cast<uint64_t>(val));
    } else if (val >= -32) {
        // negative fixint
        write_byte(static_cast<uint8_t>(val));
    } else if (val >= -128) {
        write_byte(0xd0);
        write_byte(static_cast<uint8_t>(val));
    } else if (val >= -32768) {
        write_byte(0xd1);
        write_u16_be(static_cast<uint16_t>(val));
    } else if (val >= -2147483648LL) {
        write_byte(0xd2);
        write_u32_be(static_cast<uint32_t>(val));
    } else {
        write_byte(0xd3);
        write_u64_be(static_cast<uint64_t>(val));
    }
}

void Packer::pack_string(const std::string& val) {
    size_t len = val.size();
    if (len <= 31) {
        // fixstr
        write_byte(static_cast<uint8_t>(0xa0 | len));
    } else if (len <= 0xff) {
        write_byte(0xd9);
        write_byte(static_cast<uint8_t>(len));
    } else if (len <= 0xffff) {
        write_byte(0xda);
        write_u16_be(static_cast<uint16_t>(len));
    } else {
        write_byte(0xdb);
        write_u32_be(static_cast<uint32_t>(len));
    }
    write_bytes(reinterpret_cast<const uint8_t*>(val.data()), len);
}

void Packer::pack_bin(const uint8_t* data, size_t len) {
    if (len <= 0xff) {
        write_byte(0xc4);
        write_byte(static_cast<uint8_t>(len));
    } else if (len <= 0xffff) {
        write_byte(0xc5);
        write_u16_be(static_cast<uint16_t>(len));
    } else {
        write_byte(0xc6);
        write_u32_be(static_cast<uint32_t>(len));
    }
    write_bytes(data, len);
}

void Packer::pack_bin(const std::vector<uint8_t>& data) {
    pack_bin(data.data(), data.size());
}

void Packer::pack_array(size_t count) {
    if (count <= 15) {
        // fixarray
        write_byte(static_cast<uint8_t>(0x90 | count));
    } else if (count <= 0xffff) {
        write_byte(0xdc);
        write_u16_be(static_cast<uint16_t>(count));
    } else {
        write_byte(0xdd);
        write_u32_be(static_cast<uint32_t>(count));
    }
}

void Packer::pack_map(size_t count) {
    if (count <= 15) {
        // fixmap
        write_byte(static_cast<uint8_t>(0x80 | count));
    } else if (count <= 0xffff) {
        write_byte(0xde);
        write_u16_be(static_cast<uint16_t>(count));
    } else {
        write_byte(0xdf);
        write_u32_be(static_cast<uint32_t>(count));
    }
}

void Packer::pack_double(double val) {
    write_byte(0xcb);
    uint64_t bits;
    std::memcpy(&bits, &val, 8);
    write_u64_be(bits);
}

} // namespace msgpack
} // namespace arb
