#include "utils/keccak256.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

namespace arb {
namespace crypto {

namespace {

// Keccak-f[1600] round constants
constexpr uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

// Rotation offsets
constexpr int ROTC[24] = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
};

// Pi lane indices
constexpr int PILN[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
};

inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

void keccak_f1600(uint64_t state[25]) {
    uint64_t bc[5];

    for (int round = 0; round < 24; round++) {
        // Theta
        for (int i = 0; i < 5; i++) {
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];
        }
        for (int i = 0; i < 5; i++) {
            uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) {
                state[j + i] ^= t;
            }
        }

        // Rho and Pi
        uint64_t t = state[1];
        for (int i = 0; i < 24; i++) {
            int j = PILN[i];
            uint64_t tmp = state[j];
            state[j] = rotl64(t, ROTC[i]);
            t = tmp;
        }

        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++) {
                bc[i] = state[j + i];
            }
            for (int i = 0; i < 5; i++) {
                state[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
            }
        }

        // Iota
        state[0] ^= RC[round];
    }
}

} // anonymous namespace

std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) {
    // Keccak-256: rate=136 bytes (1088 bits), capacity=64 bytes (512 bits)
    constexpr size_t RATE = 136;

    uint64_t state[25] = {};

    // Absorb
    size_t offset = 0;
    while (offset + RATE <= len) {
        for (size_t i = 0; i < RATE / 8; i++) {
            uint64_t word;
            std::memcpy(&word, data + offset + i * 8, 8);
            state[i] ^= word;
        }
        keccak_f1600(state);
        offset += RATE;
    }

    // Final block with padding
    uint8_t block[RATE] = {};
    size_t remaining = len - offset;
    std::memcpy(block, data + offset, remaining);

    // Keccak padding (NOT SHA-3): pad with 0x01 ... 0x80
    block[remaining] = 0x01;
    block[RATE - 1] |= 0x80;

    for (size_t i = 0; i < RATE / 8; i++) {
        uint64_t word;
        std::memcpy(&word, block + i * 8, 8);
        state[i] ^= word;
    }
    keccak_f1600(state);

    // Squeeze (only need 32 bytes)
    std::array<uint8_t, 32> hash;
    std::memcpy(hash.data(), state, 32);
    return hash;
}

std::array<uint8_t, 32> keccak256(const std::vector<uint8_t>& data) {
    return keccak256(data.data(), data.size());
}

std::array<uint8_t, 32> keccak256(const std::string& data) {
    return keccak256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string keccak256_hex(const uint8_t* data, size_t len) {
    auto hash = keccak256(data, len);
    std::stringstream ss;
    for (uint8_t b : hash) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

std::string keccak256_hex(const std::vector<uint8_t>& data) {
    return keccak256_hex(data.data(), data.size());
}

std::string keccak256_hex(const std::string& data) {
    return keccak256_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace crypto
} // namespace arb
