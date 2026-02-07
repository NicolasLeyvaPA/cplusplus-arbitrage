#include "exchange/hyperliquid_client.hpp"
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <stdexcept>

namespace arb {

HyperliquidHttpClient::HyperliquidHttpClient(
    const std::string& base_url,
    HyperliquidSigner& signer,
    const std::string& user_address)
    : base_url_(base_url)
    , signer_(signer)
    , user_address_(user_address)
{
    spdlog::info("HyperliquidHttpClient initialized: {}", base_url);
}

HyperliquidHttpClient::~HyperliquidHttpClient() = default;

size_t HyperliquidHttpClient::write_callback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

bool HyperliquidHttpClient::is_configured() const {
    return signer_.is_valid() && !user_address_.empty();
}

nlohmann::json HyperliquidHttpClient::perform_post(
    const std::string& endpoint, const nlohmann::json& body)
{
    std::lock_guard<std::mutex> lock(request_mutex_);
    rate_limiter_.acquire();

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string url = base_url_ + endpoint;
    std::string body_str = body.dump();
    std::string response_body;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(response_body);
    } catch (...) {
        throw std::runtime_error("Failed to parse JSON: " + response_body.substr(0, 200));
    }

    if (http_code >= 400) {
        std::string msg = response_body.substr(0, 500);
        spdlog::error("Hyperliquid API error {}: {}", http_code, msg);
        throw std::runtime_error("Hyperliquid API error " + std::to_string(http_code) + ": " + msg);
    }

    return j;
}

nlohmann::json HyperliquidHttpClient::info_request(
    const std::string& request_type, const nlohmann::json& params)
{
    nlohmann::json body = {{"type", request_type}};
    if (!params.is_null() && !params.empty()) {
        for (auto it = params.begin(); it != params.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    // Some info requests need user address
    if (request_type == "clearinghouseState" ||
        request_type == "openOrders" ||
        request_type == "userFunding" ||
        request_type == "userFills" ||
        request_type == "userRateLimit") {
        if (!body.contains("user")) {
            body["user"] = user_address_;
        }
    }

    return perform_post("/info", body);
}

nlohmann::json HyperliquidHttpClient::exchange_request(
    const nlohmann::json& action, const std::string& vault_address)
{
    uint64_t nonce = static_cast<uint64_t>(now_ms());

    auto signature = signer_.sign_action(action, nonce, vault_address);

    nlohmann::json body = {
        {"action", action},
        {"nonce", nonce},
        {"signature", {
            {"r", "0x" + std::string(64, '0')},
            {"s", "0x" + std::string(64, '0')},
            {"v", signature.v}
        }}
    };

    // Fill in r and s properly
    std::string r_hex, s_hex;
    for (uint8_t b : signature.r) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        r_hex += buf;
    }
    for (uint8_t b : signature.s) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        s_hex += buf;
    }
    body["signature"]["r"] = "0x" + r_hex;
    body["signature"]["s"] = "0x" + s_hex;

    if (!vault_address.empty()) {
        body["vaultAddress"] = vault_address;
    }

    return perform_post("/exchange", body);
}

} // namespace arb
