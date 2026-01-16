#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace arb {
namespace crypto {

/**
 * HMAC-SHA256 for Polymarket L2 authentication.
 */
std::string hmac_sha256(const std::string& key, const std::string& message);

/**
 * SHA256 hash.
 */
std::string sha256(const std::string& data);

/**
 * Base64 encoding/decoding.
 */
std::string base64_encode(const std::string& data);
std::string base64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base64_decode(const std::string& encoded);

/**
 * Hex encoding.
 */
std::string hex_encode(const std::vector<uint8_t>& data);
std::string hex_encode(const std::string& data);
std::vector<uint8_t> hex_decode(const std::string& hex);

/**
 * Generate random bytes.
 */
std::vector<uint8_t> random_bytes(size_t count);

/**
 * Generate random hex string.
 */
std::string random_hex(size_t bytes);

/**
 * EIP-712 signing utilities (for L1 auth if needed).
 * Note: Full EIP-712 implementation would require Ethereum library.
 * This is a placeholder for the signing interface.
 */
struct EIP712Domain {
    std::string name;
    std::string version;
    int chain_id;
    std::string verifying_contract;
};

// Placeholder - actual implementation would use secp256k1
std::string sign_typed_data(
    const EIP712Domain& domain,
    const std::string& type_hash,
    const std::string& data_hash,
    const std::string& private_key
);

} // namespace crypto
} // namespace arb
