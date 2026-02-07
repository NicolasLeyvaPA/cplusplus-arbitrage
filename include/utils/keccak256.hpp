#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace arb {
namespace crypto {

/**
 * Keccak-256 hash function (Ethereum variant, NOT SHA-3).
 * Ethereum uses the original Keccak with 0x01 padding, not NIST SHA-3's 0x06.
 */
std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len);
std::array<uint8_t, 32> keccak256(const std::vector<uint8_t>& data);
std::array<uint8_t, 32> keccak256(const std::string& data);

/// Returns hex-encoded keccak256 hash
std::string keccak256_hex(const uint8_t* data, size_t len);
std::string keccak256_hex(const std::vector<uint8_t>& data);
std::string keccak256_hex(const std::string& data);

} // namespace crypto
} // namespace arb
