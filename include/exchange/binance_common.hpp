#pragma once

#include "common/types.hpp"
#include "config/config.hpp"
#include "utils/time_utils.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <mutex>

namespace arb {

/**
 * Shared HTTP client for Binance REST API.
 * Handles request signing, rate limiting, and error handling.
 */
class BinanceHttpClient {
public:
    BinanceHttpClient(const std::string& base_url,
                      const std::string& api_key,
                      const std::string& api_secret);
    ~BinanceHttpClient();

    BinanceHttpClient(const BinanceHttpClient&) = delete;
    BinanceHttpClient& operator=(const BinanceHttpClient&) = delete;

    nlohmann::json signed_get(const std::string& endpoint,
                              const std::map<std::string, std::string>& params = {});

    nlohmann::json signed_post(const std::string& endpoint,
                               const std::map<std::string, std::string>& params = {});

    nlohmann::json signed_delete(const std::string& endpoint,
                                 const std::map<std::string, std::string>& params = {});

    nlohmann::json public_get(const std::string& endpoint,
                              const std::map<std::string, std::string>& params = {});

    bool is_configured() const { return !api_key_.empty() && !api_secret_.empty(); }

private:
    std::string sign_query(const std::string& query_string);
    std::string build_query_string(const std::map<std::string, std::string>& params);
    nlohmann::json perform_request(const std::string& method,
                                   const std::string& url,
                                   bool add_auth_header);

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data);

    std::string base_url_;
    std::string api_key_;
    std::string api_secret_;
    std::mutex request_mutex_;
    time_utils::RateLimiter rate_limiter_{20, 1};
};

} // namespace arb
