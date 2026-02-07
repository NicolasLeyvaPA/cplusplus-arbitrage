#pragma once

#include "exchange/hyperliquid_signer.hpp"
#include "utils/time_utils.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <mutex>

namespace arb {

/**
 * HTTP client for Hyperliquid's REST API.
 * All reads: POST to /info with JSON body.
 * All writes: POST to /exchange with signed payload.
 */
class HyperliquidHttpClient {
public:
    HyperliquidHttpClient(const std::string& base_url,
                          HyperliquidSigner& signer,
                          const std::string& user_address);
    ~HyperliquidHttpClient();

    HyperliquidHttpClient(const HyperliquidHttpClient&) = delete;
    HyperliquidHttpClient& operator=(const HyperliquidHttpClient&) = delete;

    /**
     * Read operations: POST to /info
     * @param request_type  The "type" field (e.g., "allMids", "l2Book", etc.)
     * @param params        Additional parameters merged into the request body
     */
    nlohmann::json info_request(const std::string& request_type,
                                 const nlohmann::json& params = {});

    /**
     * Write operations: POST to /exchange with signature
     * @param action  The action payload (e.g., {"type": "order", ...})
     * @param vault_address  Optional vault address
     */
    nlohmann::json exchange_request(const nlohmann::json& action,
                                     const std::string& vault_address = "");

    bool is_configured() const;

private:
    nlohmann::json perform_post(const std::string& endpoint,
                                 const nlohmann::json& body);

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data);

    std::string base_url_;
    HyperliquidSigner& signer_;
    std::string user_address_;
    std::mutex request_mutex_;
    time_utils::RateLimiter rate_limiter_{20, 1};  // 20 req/s (conservative)
};

} // namespace arb
