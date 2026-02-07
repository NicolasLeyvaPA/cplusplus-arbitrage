#include "exchange/hyperliquid_signer.hpp"
#include "utils/keccak256.hpp"
#include "utils/msgpack_lite.hpp"
#include "utils/crypto.hpp"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace arb {

namespace {

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return ss.str();
}

std::array<uint8_t, 32> hex_to_bytes32(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    std::string clean = hex;
    if (clean.substr(0, 2) == "0x") clean = clean.substr(2);
    if (clean.size() > 64) clean = clean.substr(0, 64);
    while (clean.size() < 64) clean = "0" + clean;
    for (size_t i = 0; i < 32; i++) {
        result[i] = static_cast<uint8_t>(std::stoi(clean.substr(i * 2, 2), nullptr, 16));
    }
    return result;
}

/// ABI-encode an address (left-padded to 32 bytes)
std::array<uint8_t, 32> abi_encode_address(const std::string& addr) {
    std::array<uint8_t, 32> result{};
    std::string clean = addr;
    if (clean.substr(0, 2) == "0x") clean = clean.substr(2);
    while (clean.size() < 40) clean = "0" + clean;
    for (size_t i = 0; i < 20; i++) {
        result[12 + i] = static_cast<uint8_t>(std::stoi(clean.substr(i * 2, 2), nullptr, 16));
    }
    return result;
}

/// ABI-encode a uint256 from a uint64
std::array<uint8_t, 32> abi_encode_uint256(uint64_t val) {
    std::array<uint8_t, 32> result{};
    for (int i = 7; i >= 0; i--) {
        result[31 - (7 - i)] = static_cast<uint8_t>((val >> (i * 8)) & 0xff);
    }
    return result;
}

void pack_action_value(arb::msgpack::Packer& p, const nlohmann::json& val) {
    if (val.is_null()) {
        p.pack_nil();
    } else if (val.is_boolean()) {
        p.pack_bool(val.get<bool>());
    } else if (val.is_number_integer()) {
        p.pack_int(val.get<int64_t>());
    } else if (val.is_number_unsigned()) {
        p.pack_uint(val.get<uint64_t>());
    } else if (val.is_number_float()) {
        // Hyperliquid expects floats as strings in msgpack
        std::string s = val.dump();
        p.pack_string(s);
    } else if (val.is_string()) {
        p.pack_string(val.get<std::string>());
    } else if (val.is_array()) {
        p.pack_array(val.size());
        for (const auto& elem : val) {
            pack_action_value(p, elem);
        }
    } else if (val.is_object()) {
        p.pack_map(val.size());
        for (auto it = val.begin(); it != val.end(); ++it) {
            p.pack_string(it.key());
            pack_action_value(p, it.value());
        }
    }
}

} // anonymous namespace

HyperliquidSigner::HyperliquidSigner(const std::string& private_key_hex, bool testnet)
    : testnet_(testnet)
{
    std::string clean = private_key_hex;
    if (clean.substr(0, 2) == "0x") clean = clean.substr(2);
    if (clean.size() != 64) {
        spdlog::error("Invalid private key length: {} (expected 64 hex chars)", clean.size());
        return;
    }
    private_key_ = crypto::hex_decode(clean);
    address_ = derive_address();
    valid_ = !address_.empty();
    if (valid_) {
        spdlog::info("HyperliquidSigner initialized: address={}, chain_id={}", address_, chain_id());
    }
}

HyperliquidSigner::~HyperliquidSigner() {
    // Zero out private key material
    if (!private_key_.empty()) {
        std::memset(private_key_.data(), 0, private_key_.size());
    }
}

std::string HyperliquidSigner::Signature::to_hex() const {
    std::string result;
    result.reserve(130);
    for (uint8_t b : r) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        result += buf;
    }
    for (uint8_t b : s) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        result += buf;
    }
    char vbuf[3];
    snprintf(vbuf, sizeof(vbuf), "%02x", v);
    result += vbuf;
    return result;
}

nlohmann::json HyperliquidSigner::Signature::to_json() const {
    return {
        {"r", "0x" + std::string(reinterpret_cast<const char*>(r.data()), 0).empty()
            ? bytes_to_hex(r.data(), 32) : bytes_to_hex(r.data(), 32)},
        {"s", "0x" + bytes_to_hex(s.data(), 32)},
        {"v", v}
    };
}

std::string HyperliquidSigner::derive_address() const {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!pctx) return "";

    EVP_PKEY* pkey = nullptr;

    // Build params with the private key
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) { EVP_PKEY_CTX_free(pctx); return ""; }

    BIGNUM* priv_bn = BN_bin2bn(private_key_.data(), static_cast<int>(private_key_.size()), nullptr);
    if (!priv_bn) { OSSL_PARAM_BLD_free(bld); EVP_PKEY_CTX_free(pctx); return ""; }

    // We need to compute the public key from the private key
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_secp256k1);
    EC_POINT* pub_point = EC_POINT_new(group);
    EC_POINT_mul(group, pub_point, priv_bn, nullptr, nullptr, nullptr);

    // Get uncompressed public key bytes
    size_t pub_len = EC_POINT_point2oct(group, pub_point, POINT_CONVERSION_UNCOMPRESSED,
                                         nullptr, 0, nullptr);
    std::vector<uint8_t> pub_bytes(pub_len);
    EC_POINT_point2oct(group, pub_point, POINT_CONVERSION_UNCOMPRESSED,
                       pub_bytes.data(), pub_len, nullptr);

    EC_POINT_free(pub_point);
    EC_GROUP_free(group);
    BN_free(priv_bn);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_CTX_free(pctx);

    // Hash public key (skip 0x04 prefix) with keccak256
    // Ethereum address = last 20 bytes of keccak256(public_key[1:])
    auto hash = crypto::keccak256(pub_bytes.data() + 1, pub_len - 1);
    std::string addr = "0x";
    for (size_t i = 12; i < 32; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        addr += buf;
    }
    return addr;
}

std::vector<uint8_t> HyperliquidSigner::pack_action(
    const nlohmann::json& action, uint64_t nonce, const std::string& vault_address) const
{
    msgpack::Packer packer;

    // Pack as a tuple: (action, nonce, vault_address_or_nil)
    packer.pack_array(3);
    pack_action_value(packer, action);
    packer.pack_uint(nonce);
    if (vault_address.empty()) {
        packer.pack_nil();
    } else {
        packer.pack_string(vault_address);
    }

    return packer.take();
}

std::array<uint8_t, 32> HyperliquidSigner::compute_domain_separator(bool use_l1_chain) const {
    // EIP-712 domain: name="Exchange", version="1", chainId=..., verifyingContract=0x0...0
    // domainSeparator = keccak256(
    //   keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
    //   || keccak256("Exchange")
    //   || keccak256("1")
    //   || abi.encode(chainId)
    //   || abi.encode(address(0))
    // )

    auto type_hash = crypto::keccak256(std::string(
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"));
    auto name_hash = crypto::keccak256(std::string("Exchange"));
    auto version_hash = crypto::keccak256(std::string("1"));

    uint64_t cid = use_l1_chain ? chain_id() : chain_id();
    auto chain_bytes = abi_encode_uint256(cid);
    auto contract_bytes = abi_encode_address("0x0000000000000000000000000000000000000000");

    std::vector<uint8_t> encoded;
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());
    encoded.insert(encoded.end(), name_hash.begin(), name_hash.end());
    encoded.insert(encoded.end(), version_hash.begin(), version_hash.end());
    encoded.insert(encoded.end(), chain_bytes.begin(), chain_bytes.end());
    encoded.insert(encoded.end(), contract_bytes.begin(), contract_bytes.end());

    return crypto::keccak256(encoded);
}

std::array<uint8_t, 32> HyperliquidSigner::compute_agent_struct_hash(
    const std::string& source, const std::array<uint8_t, 32>& connection_id) const
{
    // Agent struct type hash
    auto type_hash = crypto::keccak256(std::string(
        "Agent(address source,bytes32 connectionId)"));

    auto source_bytes = abi_encode_address(source);

    std::vector<uint8_t> encoded;
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());
    encoded.insert(encoded.end(), source_bytes.begin(), source_bytes.end());
    encoded.insert(encoded.end(), connection_id.begin(), connection_id.end());

    return crypto::keccak256(encoded);
}

HyperliquidSigner::Signature HyperliquidSigner::ecdsa_sign(
    const std::array<uint8_t, 32>& hash) const
{
    Signature sig{};

    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_secp256k1);
    EC_KEY* eckey = EC_KEY_new();
    EC_KEY_set_group(eckey, group);

    BIGNUM* priv_bn = BN_bin2bn(private_key_.data(), static_cast<int>(private_key_.size()), nullptr);
    EC_KEY_set_private_key(eckey, priv_bn);

    // Compute and set public key
    EC_POINT* pub_point = EC_POINT_new(group);
    EC_POINT_mul(group, pub_point, priv_bn, nullptr, nullptr, nullptr);
    EC_KEY_set_public_key(eckey, pub_point);

    ECDSA_SIG* ecdsa_sig = ECDSA_do_sign(hash.data(), 32, eckey);
    if (!ecdsa_sig) {
        EC_POINT_free(pub_point);
        BN_free(priv_bn);
        EC_KEY_free(eckey);
        EC_GROUP_free(group);
        throw std::runtime_error("ECDSA signing failed");
    }

    const BIGNUM* r_bn = nullptr;
    const BIGNUM* s_bn = nullptr;
    ECDSA_SIG_get0(ecdsa_sig, &r_bn, &s_bn);

    // Convert r, s to 32-byte arrays
    std::vector<uint8_t> r_bytes(BN_num_bytes(r_bn));
    BN_bn2bin(r_bn, r_bytes.data());
    std::vector<uint8_t> s_bytes(BN_num_bytes(s_bn));
    BN_bn2bin(s_bn, s_bytes.data());

    // Pad to 32 bytes (left-pad with zeros)
    std::memset(sig.r.data(), 0, 32);
    std::memcpy(sig.r.data() + (32 - r_bytes.size()), r_bytes.data(), r_bytes.size());
    std::memset(sig.s.data(), 0, 32);
    std::memcpy(sig.s.data() + (32 - s_bytes.size()), s_bytes.data(), s_bytes.size());

    // Enforce low-S (EIP-2): if s > N/2, replace with N-s
    BIGNUM* order = BN_new();
    EC_GROUP_get_order(group, order, nullptr);
    BIGNUM* half_order = BN_new();
    BN_rshift1(half_order, order);

    BIGNUM* s_check = BN_bin2bn(sig.s.data(), 32, nullptr);
    if (BN_cmp(s_check, half_order) > 0) {
        // s = order - s
        BIGNUM* new_s = BN_new();
        BN_sub(new_s, order, s_check);
        std::vector<uint8_t> new_s_bytes(BN_num_bytes(new_s));
        BN_bn2bin(new_s, new_s_bytes.data());
        std::memset(sig.s.data(), 0, 32);
        std::memcpy(sig.s.data() + (32 - new_s_bytes.size()), new_s_bytes.data(), new_s_bytes.size());
        BN_free(new_s);
    }

    // Determine v (recovery ID): try recid 0 and 1
    // Manual ECDSA public key recovery: Q = r^-1 * (s*R - e*G)
    sig.v = 27;
    BIGNUM* r_bn_copy = BN_bin2bn(sig.r.data(), 32, nullptr);
    BIGNUM* s_bn_copy = BN_bin2bn(sig.s.data(), 32, nullptr);
    BIGNUM* e_bn = BN_bin2bn(hash.data(), 32, nullptr);
    BN_CTX* ctx = BN_CTX_new();

    for (int recid = 0; recid <= 1; recid++) {
        // x = r + recid * order (for recid=0, x=r; recid=1, x=r+n)
        BIGNUM* x = BN_dup(r_bn_copy);
        if (recid == 1) {
            BN_add(x, x, order);
        }

        // Find point R on curve with x-coordinate = x
        EC_POINT* R = EC_POINT_new(group);
        if (!EC_POINT_set_compressed_coordinates_GFp(group, R, x, 0, ctx)) {
            EC_POINT_free(R);
            BN_free(x);
            continue;
        }

        // r_inv = r^(-1) mod n
        BIGNUM* r_inv = BN_mod_inverse(nullptr, r_bn_copy, order, ctx);
        if (!r_inv) {
            EC_POINT_free(R);
            BN_free(x);
            continue;
        }

        // recovered = r_inv * (s * R - e * G)
        EC_POINT* sR = EC_POINT_new(group);
        EC_POINT_mul(group, sR, nullptr, R, s_bn_copy, ctx);

        EC_POINT* eG = EC_POINT_new(group);
        EC_POINT_mul(group, eG, e_bn, nullptr, nullptr, ctx);

        // Negate eG to get -eG
        EC_POINT_invert(group, eG, ctx);

        // sR + (-eG)
        EC_POINT* sum = EC_POINT_new(group);
        EC_POINT_add(group, sum, sR, eG, ctx);

        // recovered = r_inv * sum
        EC_POINT* recovered = EC_POINT_new(group);
        EC_POINT_mul(group, recovered, nullptr, sum, r_inv, ctx);

        // Compare with our public key
        if (EC_POINT_cmp(group, recovered, pub_point, ctx) == 0) {
            sig.v = static_cast<uint8_t>(27 + recid);
            EC_POINT_free(recovered);
            EC_POINT_free(sum);
            EC_POINT_free(eG);
            EC_POINT_free(sR);
            BN_free(r_inv);
            EC_POINT_free(R);
            BN_free(x);
            break;
        }

        EC_POINT_free(recovered);
        EC_POINT_free(sum);
        EC_POINT_free(eG);
        EC_POINT_free(sR);
        BN_free(r_inv);
        EC_POINT_free(R);
        BN_free(x);
    }

    BN_CTX_free(ctx);
    BN_free(e_bn);
    BN_free(s_bn_copy);
    BN_free(r_bn_copy);

    BN_free(s_check);
    BN_free(half_order);
    BN_free(order);
    ECDSA_SIG_free(ecdsa_sig);
    EC_POINT_free(pub_point);
    BN_free(priv_bn);
    EC_KEY_free(eckey);
    EC_GROUP_free(group);

    return sig;
}

HyperliquidSigner::Signature HyperliquidSigner::sign_action(
    const nlohmann::json& action, uint64_t nonce, const std::string& vault_address) const
{
    if (!valid_) throw std::runtime_error("Signer not initialized");

    // Step 1: msgpack encode (action, nonce, vault_address)
    auto packed = pack_action(action, nonce, vault_address);

    // Step 2: keccak256 of packed data → connectionId
    auto connection_id = crypto::keccak256(packed);

    // Step 3: Compute phantom agent struct hash
    // Source address: "0x0000000000000000000000000000000000000000" for API
    std::string source = "0x0000000000000000000000000000000000000000";
    auto struct_hash = compute_agent_struct_hash(source, connection_id);

    // Step 4: EIP-712 hash = keccak256("\x19\x01" || domainSep || structHash)
    auto domain_sep = compute_domain_separator();
    std::vector<uint8_t> eip712_input;
    eip712_input.push_back(0x19);
    eip712_input.push_back(0x01);
    eip712_input.insert(eip712_input.end(), domain_sep.begin(), domain_sep.end());
    eip712_input.insert(eip712_input.end(), struct_hash.begin(), struct_hash.end());
    auto sign_hash = crypto::keccak256(eip712_input);

    // Step 5: ECDSA sign
    return ecdsa_sign(sign_hash);
}

HyperliquidSigner::Signature HyperliquidSigner::sign_l1_action(
    const nlohmann::json& action, uint64_t nonce, const std::string& vault_address) const
{
    if (!valid_) throw std::runtime_error("Signer not initialized");

    auto packed = pack_action(action, nonce, vault_address);
    auto connection_id = crypto::keccak256(packed);

    // L1 actions use the actual chain address as source
    std::string source = "0x0000000000000000000000000000000000000000";
    auto struct_hash = compute_agent_struct_hash(source, connection_id);

    // Use L1-specific domain separator
    auto domain_sep = compute_domain_separator(true);
    std::vector<uint8_t> eip712_input;
    eip712_input.push_back(0x19);
    eip712_input.push_back(0x01);
    eip712_input.insert(eip712_input.end(), domain_sep.begin(), domain_sep.end());
    eip712_input.insert(eip712_input.end(), struct_hash.begin(), struct_hash.end());
    auto sign_hash = crypto::keccak256(eip712_input);

    return ecdsa_sign(sign_hash);
}

HyperliquidSigner::Signature HyperliquidSigner::sign_user_signed_action(
    const nlohmann::json& action,
    const std::vector<nlohmann::json>& payload_types,
    const std::string& primary_type) const
{
    if (!valid_) throw std::runtime_error("Signer not initialized");

    // Build type string for the primary type
    std::string type_str = primary_type + "(";
    for (size_t i = 0; i < payload_types.size(); i++) {
        if (i > 0) type_str += ",";
        type_str += payload_types[i]["type"].get<std::string>() + " " +
                    payload_types[i]["name"].get<std::string>();
    }
    type_str += ")";

    auto type_hash = crypto::keccak256(type_str);

    // Encode the struct values
    std::vector<uint8_t> encoded;
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());

    for (const auto& field : payload_types) {
        std::string name = field["name"].get<std::string>();
        std::string type = field["type"].get<std::string>();

        if (type == "address") {
            auto addr = abi_encode_address(action[name].get<std::string>());
            encoded.insert(encoded.end(), addr.begin(), addr.end());
        } else if (type == "uint64" || type == "uint256") {
            auto val = abi_encode_uint256(action[name].get<uint64_t>());
            encoded.insert(encoded.end(), val.begin(), val.end());
        } else if (type == "bool") {
            auto val = abi_encode_uint256(action[name].get<bool>() ? 1 : 0);
            encoded.insert(encoded.end(), val.begin(), val.end());
        } else if (type == "bytes32") {
            auto val = hex_to_bytes32(action[name].get<std::string>());
            encoded.insert(encoded.end(), val.begin(), val.end());
        } else if (type == "string") {
            auto val = crypto::keccak256(action[name].get<std::string>());
            encoded.insert(encoded.end(), val.begin(), val.end());
        }
    }

    auto struct_hash = crypto::keccak256(encoded);
    auto domain_sep = compute_domain_separator();

    std::vector<uint8_t> eip712_input;
    eip712_input.push_back(0x19);
    eip712_input.push_back(0x01);
    eip712_input.insert(eip712_input.end(), domain_sep.begin(), domain_sep.end());
    eip712_input.insert(eip712_input.end(), struct_hash.begin(), struct_hash.end());
    auto sign_hash = crypto::keccak256(eip712_input);

    return ecdsa_sign(sign_hash);
}

} // namespace arb
