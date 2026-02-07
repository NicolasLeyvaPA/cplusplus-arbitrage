#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace arb {

/**
 * Hyperliquid EIP-712 typed data signer.
 *
 * Signing flow:
 *   1. Encode action + nonce + vault_address via msgpack
 *   2. Hash with keccak256 → connectionId
 *   3. Build EIP-712 Agent struct: {source, connectionId}
 *   4. Compute EIP-712 hash = keccak256("\x19\x01" || domainSep || structHash)
 *   5. ECDSA sign with secp256k1 → (r, s, v)
 */
class HyperliquidSigner {
public:
    /// Construct from hex-encoded private key (64 hex chars, no 0x prefix)
    explicit HyperliquidSigner(const std::string& private_key_hex, bool testnet = false);
    ~HyperliquidSigner();

    HyperliquidSigner(const HyperliquidSigner&) = delete;
    HyperliquidSigner& operator=(const HyperliquidSigner&) = delete;

    struct Signature {
        std::array<uint8_t, 32> r;
        std::array<uint8_t, 32> s;
        uint8_t v;  // 27 or 28

        std::string to_hex() const;
        nlohmann::json to_json() const;
    };

    /**
     * Sign an action for the /exchange endpoint.
     * @param action   The action JSON (e.g., {"type": "order", ...})
     * @param nonce    Timestamp-based nonce (milliseconds)
     * @param vault_address  Optional vault address (empty for main account)
     * @return Signature (r, s, v)
     */
    Signature sign_action(const nlohmann::json& action,
                          uint64_t nonce,
                          const std::string& vault_address = "") const;

    /**
     * Sign a user-signed action (e.g., approve agent).
     * Uses the "agent" typed data with source=hyperliquid chain address.
     */
    Signature sign_user_signed_action(const nlohmann::json& action,
                                       const std::vector<nlohmann::json>& payload_types,
                                       const std::string& primary_type) const;

    /**
     * Sign an L1 action (withdraw, USD transfer, etc.)
     */
    Signature sign_l1_action(const nlohmann::json& action,
                              uint64_t nonce,
                              const std::string& vault_address = "") const;

    /// Get the Ethereum address derived from the private key (0x-prefixed)
    std::string address() const { return address_; }

    /// Check if signer is properly initialized
    bool is_valid() const { return valid_; }

private:
    /// Compute EIP-712 domain separator
    std::array<uint8_t, 32> compute_domain_separator(bool use_l1_chain = false) const;

    /// Compute EIP-712 struct hash for Agent(address source, bytes32 connectionId)
    std::array<uint8_t, 32> compute_agent_struct_hash(
        const std::string& source, const std::array<uint8_t, 32>& connection_id) const;

    /// Pack action into msgpack bytes for hashing
    std::vector<uint8_t> pack_action(const nlohmann::json& action,
                                      uint64_t nonce,
                                      const std::string& vault_address) const;

    /// Raw secp256k1 ECDSA sign a 32-byte hash
    Signature ecdsa_sign(const std::array<uint8_t, 32>& hash) const;

    /// Derive Ethereum address from private key
    std::string derive_address() const;

    std::vector<uint8_t> private_key_;
    std::string address_;
    bool testnet_;
    bool valid_{false};

    // Chain IDs
    static constexpr uint64_t MAINNET_CHAIN_ID = 1337;
    static constexpr uint64_t TESTNET_CHAIN_ID = 421614;

    uint64_t chain_id() const { return testnet_ ? TESTNET_CHAIN_ID : MAINNET_CHAIN_ID; }
};

} // namespace arb
