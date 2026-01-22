#include "utils/crypto.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace arb {
namespace crypto {

std::string hmac_sha256(const std::string& key, const std::string& message) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         key.data(), key.size(),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         hash, &hash_len);

    return base64_encode(std::vector<uint8_t>(hash, hash + hash_len));
}

std::string sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

std::string base64_encode(const std::string& data) {
    return base64_encode(std::vector<uint8_t>(data.begin(), data.end()));
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    int val = 0;
    int bits = -6;

    for (uint8_t c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            result.push_back(chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }

    if (bits > -6) {
        result.push_back(chars[((val << 8) >> (bits + 8)) & 0x3F]);
    }

    while (result.size() % 4) {
        result.push_back('=');
    }

    return result;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
    static const int lookup[] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> result;
    int val = 0;
    int bits = -8;

    for (char c : encoded) {
        if (c == '=') break;
        if (c < 0 || lookup[static_cast<int>(c)] == -1) continue;

        val = (val << 6) + lookup[static_cast<int>(c)];
        bits += 6;

        if (bits >= 0) {
            result.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }

    return result;
}

std::string hex_encode(const std::vector<uint8_t>& data) {
    std::stringstream ss;
    for (uint8_t b : data) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

std::string hex_encode(const std::string& data) {
    return hex_encode(std::vector<uint8_t>(data.begin(), data.end()));
}

std::vector<uint8_t> hex_decode(const std::string& hex) {
    std::vector<uint8_t> result;

    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
        result.push_back(byte);
    }

    return result;
}

std::vector<uint8_t> random_bytes(size_t count) {
    std::vector<uint8_t> bytes(count);
    if (RAND_bytes(bytes.data(), count) != 1) {
        throw std::runtime_error("Failed to generate random bytes");
    }
    return bytes;
}

std::string random_hex(size_t bytes) {
    return hex_encode(random_bytes(bytes));
}

std::string sign_typed_data(
    const EIP712Domain& domain,
    const std::string& type_hash,
    const std::string& data_hash,
    const std::string& private_key)
{
    // Note: Full EIP-712 implementation would require secp256k1 library
    // This is a placeholder that indicates the signature would be generated here
    // For a complete implementation, use a library like secp256k1 or ethers

    (void)domain;
    (void)type_hash;
    (void)data_hash;
    (void)private_key;

    throw std::runtime_error("EIP-712 signing requires secp256k1 library - use L2 auth instead");
}

} // namespace crypto
} // namespace arb
